/*
 * vgl_cmd.c - wire command emission layer.
 *
 * G1 CLOSED: the encoding is the Mesa (alpha) route - plain VIRGL_CCMD_*
 * dwords inside the virgl 3D command stream, per reference/mesa-ref-
 * virgl_encode.c. Exact dword layouts are in vgl_wire.h (VIRGL_CMD0 +
 * payload index defines).
 *
 * M1 stub behaviour: every emitter returns VGL_E_NO_DEVICE unless the
 * bridge transport is open, and VGL_E_PROTO once it is (command
 * construction not yet wired to real resources).  This keeps
 * vgl_encoder_open() honest - callers (hwcodec enumeration) fall back to
 * software encoding cleanly.
 *
 * M1 session construction (mirror of Mesa virgl_video.c):
 *   per codec: desc_buffers[2] + feed_buffers[2] (STAGING pipe buffers,
 *              sizes VGL_WIRE_DESC_BUF_SIZE / 8), guest-assigned handles;
 *   CREATE_VIDEO_CODEC  (53) handle/profile/entrypoint/chroma/level/w/h
 *                        [+ max_ref if caps.v2.host_feature_check_version>=14];
 *   per surface: CREATE_VIDEO_BUFFER (55) format=VGL_WIRE_FORMAT_NV12,
 *                        w/h + 2 per-plane RESOURCE handles (R8, GR88);
 *   per frame: BEGIN_FRAME(57,cdc,tgt) -> write desc buf, clear feed buf
 *              -> ENCODE_BITSTREAM(60,cdc,src,coded,desc,feed)
 *              -> END_FRAME(61,cdc,tgt) -> flush + fence
 *              -> wait, read feed buf {stat(SUCCESS=2), coded_size},
 *                 copy coded_size bytes from the coded buffer resource.
 */
#include "vgl_internal.h"

#include <string.h>

static int emit_ready(const struct vgl_encoder *e)
{
    if (!e)
        return VGL_E_PARAM;
    if (!vgl_tport_ready())
        return VGL_E_NO_DEVICE;
    return VGL_OK;
}

int vgl_cmd_emit_create_codec(struct vgl_encoder *e)
{
    int rc = emit_ready(e);
    if (rc != VGL_OK)
        return rc;

    /* TODO(M1, gate G1): build vgl_wire_h264_enc_picture_desc_t
     *   base.profile     = vgl_wire_profile_from_h264(is_high)
     *   base.entry_point = VGL_WIRE_ENTRYPOINT_ENCODE (=4)
     *   chroma_format    = 2 (host auto-falls back to 420)
     *   level_idc        = 0 (host derives, F9)
     * and submit the create_codec command. */
    return VGL_E_PROTO;
}

int vgl_cmd_emit_destroy_codec(struct vgl_encoder *e)
{
    if (!e)
        return VGL_E_PARAM;
    if (!vgl_tport_ready())
        return VGL_E_NO_DEVICE;

    /* TODO(M1): destroy_codec + resource cleanup. */
    return VGL_E_PROTO;
}

int vgl_cmd_emit_frame_seq(struct vgl_encoder *e, int slot, int force_idr)
{
    int rc = emit_ready(e);
    if (rc != VGL_OK)
        return rc;
    if (slot < 0)
        return VGL_E_PARAM;

    /* TODO(M1): begin_frame -> create_buffer -> encode_bitstream ->
     * end_frame with the pool slot's backing resource id, forcing an
     * IDR when requested (pic_ctrl field of the enc picture desc). */
    (void)force_idr;
    return VGL_E_PROTO;
}

int vgl_cmd_collect_feedback(struct vgl_encoder *e, vgl_packet_t *out)
{
    int rc = emit_ready(e);
    if (rc != VGL_OK)
        return rc;

    /* TODO(M1): WAIT(timeout=fence_timeout_ms) -> REAP ->
     * vgl_wire_video_encode_feedback_t {status, coded_size}; copy the
     * in-band Annex-B payload (host synthesized SPS/PPS/AUD + slice)
     * into e->out_buf, fill *out, set keyframe=1 on IDR frames. */
    memset(out, 0, sizeof(*out));
    return VGL_E_PROTO;
}
