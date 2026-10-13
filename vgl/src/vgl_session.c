/*
 * vgl_session.c - encoder session state machine and wire command glue.
 *
 * Frame loop (authoritative, from Mesa virgl_video.c @ G1):
 *   create_codec -> create_video_buffer(NV12, 2 plane resources)
 *    -> [begin_frame -> write desc buffer -> clear feed buffer
 *         -> encode_bitstream(src, coded, desc, feed) -> end_frame
 *         -> flush/fence -> read feed buffer: {stat, coded_size} ] -> destroy
 *
 * All commands are VIRGL_CCMD_* dwords on the virgl 3D command stream
 * (alpha route confirmed - no separate control-queue commands). The
 * command emission itself is isolated in vgl_cmd_emit_* (vgl_cmd.c).
 */
#include "vgl_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* wire validation (gate G1 - CLOSED 2026-10-12)                       */
/* ------------------------------------------------------------------ */

int vgl_wire_check(void)
{
    /* Compile-time size pins live in vgl_wire.h (typedef negative-array
     * trick). Here: field-offset spot checks against the authoritative
     * layouts of virgl_h264/h265_enc_picture_desc (transcribed verbatim
     * from reference/virgl_video_hw.h; h265 added after the W4b
     * rate_control 72->76 miscalc was caught by the CI size pin). */
    if (offsetof(struct virgl_h264_enc_picture_desc, rate_ctrl) != 324)
        return VGL_E_WIRE;
    if (offsetof(struct virgl_h264_enc_picture_desc, quant_i_frames) != 620)
        return VGL_E_WIRE;
    if (offsetof(struct virgl_h264_enc_picture_desc, frame_num) != 632)
        return VGL_E_WIRE;
    if (offsetof(struct virgl_h264_enc_picture_desc, num_slice_descriptors) != 1008)
        return VGL_E_WIRE;
    if (offsetof(struct virgl_h264_enc_picture_desc, picture_type) != 2548)
        return VGL_E_WIRE;
    if (offsetof(struct virgl_base_picture_desc, key_size) != 260)
        return VGL_E_WIRE;
    if (offsetof(struct virgl_h265_enc_rate_control, max_qp) != 64)
        return VGL_E_WIRE;
    if (offsetof(struct virgl_h265_enc_picture_desc, decoded_curr_pic) != 436)
        return VGL_E_WIRE;
    if (offsetof(struct virgl_h265_enc_picture_desc, num_slice_descriptors) != 644)
        return VGL_E_WIRE;
    if (offsetof(struct virgl_h265_enc_picture_desc, picture_type) != 2200)
        return VGL_E_WIRE;
    if (vgl_wire_profile_from_h264(1) != PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH)
        return VGL_E_WIRE;
    if (vgl_wire_profile_from_h264(0) != PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN)
        return VGL_E_WIRE;
    return VGL_OK;
}

size_t vgl_wire_enc_desc_size(void)
{
    return sizeof(struct virgl_h264_enc_picture_desc);
}

uint16_t vgl_wire_profile_from_h264(int high)
{
    /* guest-numeric hard values (F17); never re-derive. */
    return (uint16_t)(high ? PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH
                           : PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN);
}

/* ------------------------------------------------------------------ */
/* process level                                                       */
/* ------------------------------------------------------------------ */

int vgl_open(void)
{
    return vgl_tport_open();
}

void vgl_close(void)
{
    vgl_tport_close();
}

static int g_probed_ok = -1;    /* -1 unknown, 0 no, 1 yes */

int vgl_probe(vgl_caps_t *caps)
{
    if (g_probed_ok == 1) {
        if (caps)
            memset(caps, 0, sizeof(*caps)), caps->profiles =
                (VGL_CAP_PROFILE_H264_BASELINE |
                 VGL_CAP_PROFILE_H264_MAIN |
                 VGL_CAP_PROFILE_H264_HIGH),
            caps->max_width = 1920, caps->max_height = 1080;
        return VGL_OK;
    }
    if (g_probed_ok == 0)
        return VGL_E_NO_DEVICE;

    int rc = vgl_open();
    if (rc != VGL_OK) {
        g_probed_ok = 0;
        return rc;
    }

    /* M1: full create_codec/destroy round trip at 64x64.  Until the
     * command encoding is finalized against the authoritative header
     * (gate G1), a successful transport open is NOT sufficient to claim
     * codec capability - so report absent here, forcing callers to the
     * software fallback path.  This keeps the M0 CI build honest. */
    g_probed_ok = 0;
    vgl_close();
    return VGL_E_NO_DEVICE;
}

/* ------------------------------------------------------------------ */
/* session                                                             */
/* ------------------------------------------------------------------ */

static int params_valid(const vgl_enc_params_t *p)
{
    if (!p || p->width == 0 || p->height == 0 || (p->width | p->height) & 1)
        return 0;
    if (p->width > 1920 || p->height > 1080)      /* handover §4.3-4 */
        return 0;
    if (p->profile != VGL_PROFILE_H264_BASELINE &&
        p->profile != VGL_PROFILE_H264_CONSTRAINED_BASELINE &&
        p->profile != VGL_PROFILE_H264_MAIN &&
        p->profile != VGL_PROFILE_H264_HIGH)
        return 0;
    return 1;
}

vgl_encoder_t *vgl_encoder_open(const vgl_enc_params_t *p)
{
    if (!params_valid(p))
        return NULL;
    if (vgl_open() != VGL_OK)
        return NULL;
    if (vgl_wire_check() != VGL_OK)
        return NULL;

    vgl_encoder_t *e = (vgl_encoder_t *)calloc(1, sizeof(*e));
    if (!e)
        return NULL;

    e->params = *p;
    e->state = VGL_SESS_OPENED;
    e->next_res_id = 1;
    e->next_buf_id = 1;
    e->fence_timeout_ms = 200;   /* conservative; tune at M1 */

    e->pool = vgl_pool_create(p->width, p->height);
    if (!e->pool) {
        free(e);
        return NULL;
    }

    e->out_cap = (uint32_t)vgl_pool_frame_size(p->width, p->height) / 4 + 4096;
    e->out_buf = (uint8_t *)malloc(e->out_cap);
    if (!e->out_buf) {
        vgl_pool_destroy(e->pool);
        free(e);
        return NULL;
    }

    if (vgl_pool_bind(e->pool) != VGL_OK)
        goto fail;

    /* M1: emit ctx_create / create_codec here (vgl_cmd_emit_*).
     * The emit layer returns VGL_E_NO_DEVICE until encoding is
     * finalized (gate G1), which keeps open() honest in CI builds. */
    if (vgl_cmd_emit_create_codec(e) != VGL_OK)
        goto fail;
    e->state = VGL_SESS_CODEC_CREATED;

    if (vgl_pool_bind(e->pool) == VGL_OK)
        e->state = VGL_SESS_RESOURCES_READY;
    /* Non-bound pool still allows software-side frame staging; encode
     * attempts will surface VGL_E_PROTO in CI. */

    e->state = VGL_SESS_ACTIVE;
    return e;

fail:
    vgl_pool_destroy(e->pool);
    free(e->out_buf);
    free(e);
    return NULL;
}

void vgl_encoder_close(vgl_encoder_t *e)
{
    if (!e)
        return;
    if (e->state >= VGL_SESS_CODEC_CREATED)
        vgl_cmd_emit_destroy_codec(e);    /* best effort */
    vgl_pool_destroy(e->pool);
    free(e->out_buf);
    free(e);
}

int vgl_encode_frame(vgl_encoder_t *e, const vgl_frame_t *f, vgl_packet_t *out)
{
    if (!e || !f || !out)
        return VGL_E_PARAM;
    if (e->state != VGL_SESS_ACTIVE)
        return VGL_E_PROTO;

    int slot = vgl_pool_fill(e->pool, f);
    if (slot < 0)
        return VGL_E_PARAM;
    e->cur_slot = (uint32_t)slot;

    int force_idr = (e->flags & 1) ? 1 : 0;
    e->flags &= ~1u;

    if (vgl_cmd_emit_frame_seq(e, slot, force_idr) != VGL_OK)
        return VGL_E_PROTO;
    if (vgl_cmd_collect_feedback(e, out) != VGL_OK)
        return VGL_E_PROTO;
    return VGL_OK;
}

int vgl_encoder_flush(vgl_encoder_t *e, vgl_packet_t *out)
{
    if (!e || !out)
        return VGL_E_PARAM;
    memset(out, 0, sizeof(*out));
    out->end_of_stream = 1;
    return VGL_OK;               /* zerolatency: no reordering drain */
}

int vgl_encoder_set_bitrate(vgl_encoder_t *e, uint32_t kbps)
{
    if (!e)
        return VGL_E_PARAM;
    e->params.bitrate_kbps = kbps;
    /* Applied in the next frame's picture desc (host reads per frame). */
    return VGL_OK;
}

int vgl_encoder_force_idr(vgl_encoder_t *e)
{
    if (!e)
        return VGL_E_PARAM;
    e->flags |= 1u;
    return VGL_OK;
}

const char *vgl_strerror(int code)
{
    switch (code) {
    case VGL_OK:           return "ok";
    case VGL_E_NO_DEVICE:  return "no vgvdo bridge device";
    case VGL_E_DRIVER:     return "bridge driver error";
    case VGL_E_WIRE:       return "wire contract mismatch (gate G1)";
    case VGL_E_PROTO:      return "protocol/fence error";
    case VGL_E_PARAM:      return "invalid parameter";
    case VGL_E_NOMEM:      return "out of memory";
    case VGL_E_HOST:       return "host rejected";
    default:               return "unknown";
    }
}
