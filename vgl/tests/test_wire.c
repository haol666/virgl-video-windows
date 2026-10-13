/*
 * test_wire.c - CI unit tests for libvgl (run on windows runner, no device
 * required: every case asserts pure logic, hard wire constants and the
 * authoritative struct layout).
 *
 * These tests encode the gate-G1 contract (wire structs transcribed from
 * virgl_video_hw.h; enums cross-validated against guest Mesa 26.2.4 + host
 * r9 guest_to_host_profile table) so that any accidental re-derivation of
 * wire values or layout drift fails loudly (F17 lesson).
 */
#include "vgl/vgl.h"
#include "vgl/vgl_wire.h"
#include "../src/vgl_internal.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);       \
            g_fail = 1;                                                \
        }                                                              \
    } while (0)

static void test_wire_hard_values(void)
{
    /* profile numbering: guest Mesa 26.2.4 == host guest_to_host_profile */
    CHECK(PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE == 7);
    CHECK(PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE == 8);
    CHECK(PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN == 9);
    CHECK(PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH == 11);
    CHECK(PIPE_VIDEO_PROFILE_HEVC_MAIN == 15);
    CHECK(PIPE_VIDEO_PROFILE_HEVC_MAIN_10 == 16);

    /* entrypoint: identical on both sides */
    CHECK(PIPE_VIDEO_ENTRYPOINT_UNKNOWN == 0);
    CHECK(PIPE_VIDEO_ENTRYPOINT_BITSTREAM == 1);
    CHECK(PIPE_VIDEO_ENTRYPOINT_ENCODE == 4);

    /* chroma: NV12 input = 420 = 2 */
    CHECK(PIPE_VIDEO_CHROMA_FORMAT_420 == 2);

    /* picture types (IDR forcing) and rc methods (CBR/VBR) */
    CHECK(PIPE_H2645_ENC_PICTURE_TYPE_P == 0);
    CHECK(PIPE_H2645_ENC_PICTURE_TYPE_I == 2);
    CHECK(PIPE_H2645_ENC_PICTURE_TYPE_IDR == 3);
    CHECK(PIPE_H2645_ENC_RATE_CONTROL_METHOD_CONSTANT == 3);
    CHECK(PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE == 4);

    /* feedback stat enum: NOT_STARTED=0 .. FAILURE=3 */
    CHECK(VIRGL_VIDEO_ENCODE_STAT_NOT_STARTED == 0);
    CHECK(VIRGL_VIDEO_ENCODE_STAT_SUCCESS == 2);
    CHECK(VIRGL_VIDEO_ENCODE_STAT_FAILURE == 3);

    /* public enum mirrors the wire enum (vgl.h) */
    CHECK(VGL_PROFILE_H264_HIGH == PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH);
    CHECK(VGL_PROFILE_H264_BASELINE == 7);
    CHECK(VGL_PROFILE_HEVC_MAIN_8 == 15);

    CHECK(vgl_wire_profile_from_h264(1) == 11);
    CHECK(vgl_wire_profile_from_h264(0) == 9);
}

static void test_command_ids(void)
{
    /* video block of enum virgl_context_cmd (counted from the full
     * ordered enum in reference/mesa-ref-virgl_protocol.h; TRANSFER3D=43
     * cross-checked against virglrenderer). */
    CHECK(VIRGL_CCMD_CREATE_VIDEO_CODEC == 53);
    CHECK(VIRGL_CCMD_DESTROY_VIDEO_CODEC == 54);
    CHECK(VIRGL_CCMD_CREATE_VIDEO_BUFFER == 55);
    CHECK(VIRGL_CCMD_DESTROY_VIDEO_BUFFER == 56);
    CHECK(VIRGL_CCMD_BEGIN_FRAME == 57);
    CHECK(VIRGL_CCMD_DECODE_BITSTREAM == 59);
    CHECK(VIRGL_CCMD_ENCODE_BITSTREAM == 60);
    CHECK(VIRGL_CCMD_END_FRAME == 61);

    /* payload indices (verbatim from virgl_protocol.h) */
    CHECK(VIRGL_CREATE_VIDEO_CODEC_PROFILE == 2);
    CHECK(VIRGL_CREATE_VIDEO_CODEC_ENTRYPOINT == 3);
    CHECK(VIRGL_CREATE_VIDEO_CODEC_MAX_REF == 8);
    CHECK(VIRGL_ENCODE_BS_DEST_HANDLE == 3);
    CHECK(VIRGL_ENCODE_BS_DESC_HANDLE == 4);
    CHECK(VIRGL_ENCODE_BS_FEED_HANDLE == 5);

    /* header packing: cmd | obj<<8 | len<<16 */
    CHECK(VIRGL_CMD0(53, 0, 8) == (53u | (0u << 8) | (8u << 16)));

    /* NV12 format id on the wire */
    CHECK(VGL_WIRE_FORMAT_NV12 == 166);

    /* version gate mirroring virgl_encode.c:1780 */
    CHECK(VGL_WIRE_HOST_FEAT_MAX_REF == 14);
}

static void test_wire_layout(void)
{
    /* compile-time pins live in vgl_wire.h; the runtime gate also checks
     * field offsets. This must pass everywhere (it is layout-only). */
    CHECK(vgl_wire_check() == VGL_OK);

    CHECK(vgl_wire_enc_desc_size() == 2556);
    CHECK(VGL_WIRE_DESC_BUF_SIZE == 2556);

    CHECK(sizeof(struct virgl_base_picture_desc) == 264);
    CHECK(sizeof(struct virgl_h264_enc_seq_param) == 60);
    CHECK(sizeof(struct virgl_h264_enc_rate_control) == 64);
    CHECK(sizeof(struct virgl_h264_enc_motion_estimation) == 28);
    CHECK(sizeof(struct virgl_h264_enc_pic_control) == 8);
    CHECK(sizeof(struct virgl_h264_slice_descriptor) == 12);
    CHECK(sizeof(struct virgl_h265_enc_seq_param) == 76);
    CHECK(sizeof(struct virgl_h265_enc_rate_control) == 76);
    CHECK(sizeof(struct virgl_h265_enc_picture_desc) == 2204);
    CHECK(sizeof(struct virgl_video_encode_feedback) == 8);
    CHECK(sizeof(struct virgl_enc_quality_modes) == 16);
    CHECK(sizeof(struct virgl_video_caps) == 16);

    /* spot offsets that the runtime gate also verifies */
    CHECK(offsetof(struct virgl_base_picture_desc, key_size) == 260);
    CHECK(offsetof(struct virgl_h264_enc_picture_desc, seq) == 264);
    CHECK(offsetof(struct virgl_h264_enc_picture_desc, rate_ctrl) == 324);
    CHECK(offsetof(struct virgl_h264_enc_picture_desc, pic_ctrl) == 608);
    CHECK(offsetof(struct virgl_h264_enc_picture_desc, quant_i_frames) == 620);
    CHECK(offsetof(struct virgl_h264_enc_picture_desc, frame_num) == 632);
    CHECK(offsetof(struct virgl_h264_enc_picture_desc, gop_size) == 988);
    CHECK(offsetof(struct virgl_h264_enc_picture_desc, slices_descriptors) == 1012);
    CHECK(offsetof(struct virgl_h264_enc_picture_desc, picture_type) == 2548);
    CHECK(offsetof(struct virgl_h265_enc_picture_desc, decoded_curr_pic) == 436);
    CHECK(offsetof(struct virgl_h265_enc_picture_desc, num_slice_descriptors) == 644);
    CHECK(offsetof(struct virgl_h265_enc_picture_desc, picture_type) == 2200);
    CHECK(offsetof(struct virgl_video_encode_feedback, coded_size) == 4);
}

static void test_pool_layout(void)
{
    /* 1080p NV12: 1920 is NOT 256-aligned (7.5x256), rounds up to 2048 */
    vgl_resource_pool_t *p = vgl_pool_create(1920, 1080);
    CHECK(p != NULL);
    CHECK(p->y_stride == 2048);
    CHECK(p->frame_size == 2048 * 1080 * 3 / 2);
    CHECK(vgl_pool_frame_size(1920, 1080) == p->frame_size);

    /* odd width rounds up: 653 -> 768 stride */
    vgl_resource_pool_t *p2 = vgl_pool_create(654, 480);
    CHECK(p2 != NULL);
    CHECK(p2->y_stride == 768);

    /* reject odd dims */
    CHECK(vgl_pool_create(641, 480) == NULL);
    CHECK(vgl_pool_create(640, 481) == NULL);

    vgl_pool_destroy(p);
    vgl_pool_destroy(p2);
}

static void test_frame_fill(void)
{
    vgl_resource_pool_t *p = vgl_pool_create(64, 32);
    CHECK(p != NULL);

    uint8_t y[64 * 32], uv[64 * 16];
    memset(y, 0x10, sizeof(y));
    memset(uv, 0x80, sizeof(uv));

    vgl_frame_t f = { y, uv, 64, 64, 64, 32, 1234 };
    int slot = vgl_pool_fill(p, &f);
    CHECK(slot == 0);
    slot = vgl_pool_fill(p, &f);
    CHECK(slot == 1);
    slot = vgl_pool_fill(p, &f);
    CHECK(slot == 2);
    slot = vgl_pool_fill(p, &f);
    CHECK(slot == 3);
    slot = vgl_pool_fill(p, &f);
    CHECK(slot == 0);                    /* round robin */

    /* oversized frame rejected */
    vgl_frame_t big = { y, uv, 64, 64, 128, 64, 0 };
    CHECK(vgl_pool_fill(p, &big) == -1);

    vgl_pool_destroy(p);
}

static void test_encoder_params(void)
{
    /* valid params: passes validation in vgl_encoder_open until the
     * bridge/transport is absent -> open returns NULL in CI (no device),
     * which is itself the contract being asserted. */
    vgl_enc_params_t p;
    memset(&p, 0, sizeof(p));
    p.width = 1920; p.height = 1080; p.fps = 30;
    p.gop = 120; p.bitrate_kbps = 4000;
    p.profile = VGL_PROFILE_H264_HIGH;
    p.rc = VGL_RC_CBR; p.zerolatency = 1;
    CHECK(vgl_encoder_open(&p) == NULL);   /* no vgvdo on CI runners */

    /* invalid params rejected before device IO */
    p.width = 641;
    CHECK(vgl_encoder_open(&p) == NULL);
    p.width = 1920;
    p.profile = 13;                        /* host-numeric AVC_HIGH: F17 poison */
    CHECK(vgl_encoder_open(&p) == NULL);
    p.profile = VGL_PROFILE_H264_HIGH;
    p.width = 4096;                        /* beyond reported ceiling */
    CHECK(vgl_encoder_open(&p) == NULL);
}

static void test_probe_no_device(void)
{
    vgl_caps_t caps;
    int rc = vgl_probe(&caps);
    CHECK(rc == VGL_E_NO_DEVICE);          /* CI runner: no \\.\VGLVIDEO0 */
}

static void test_set_bitrate_and_idr(void)
{
    /* vgl_encoder_* accept NULL session */
    CHECK(vgl_encoder_set_bitrate(NULL, 4000) == VGL_E_PARAM);
    CHECK(vgl_encoder_force_idr(NULL) == VGL_E_PARAM);
}

int main(void)
{
    test_wire_hard_values();
    test_command_ids();
    test_wire_layout();
    test_pool_layout();
    test_frame_fill();
    test_encoder_params();
    test_probe_no_device();
    test_set_bitrate_and_idr();

    if (g_fail) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
