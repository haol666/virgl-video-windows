/*
 * vgl_wire.h - AUTHORITATIVE wire contract for the virtio-gpu video protocol.
 *
 * Gate G1 CLOSED (2026-10-12). This is a faithful transcription of:
 *   - struct definitions: virgl_video_hw.h (host handover copy, libvirglrenderer
 *     v12.5-era) - byte-identical to Mesa's copy
 *     (src/virtio/virtio-gpu/virgl_video_hw.h, openKylin fork, upstream branch;
 *     verified by fc /L diff: whitespace only).
 *   - command ids & payload layouts: src/virtio/virtio-gpu/virgl_protocol.h
 *     (same Mesa source; payload #defines verbatim).
 *   - pipe enums: src/gallium/include/pipe/p_video_enums.h (profile numbering
 *     == guest Mesa 26.2.4 == host r9 guest_to_host_profile table - three-way
 *     cross-validated), p_video_state.h (picture type / rc method, explicit
 *     values in source).
 *
 * Original code: Copyright (C) 2022 Kylin Software Co., Ltd. (MIT), via the
 * virgl-video patches merged into Mesa upstream. Reference copies of every
 * source file live in reference/ of this repository.
 *
 * Wire value rules (handover doc section 3.3, F17 lesson):
 *   - profile: see enum vgl_wire_profile below (7/8/9/11 H264, 15/16 HEVC).
 *     NEVER re-derive from any local gallium headers.
 *   - entrypoint: BITSTREAM=1, ENCODE=4 (both sides agree; PROCESSING=5
 *     exists on guest only).
 *   - level: 0 is legal input, host derives from frame size (F9/r4).
 *   - H264 dimensions are macroblock-aligned BY THE GUEST before
 *     create_codec (Mesa aligns to 16) - mirror that behaviour.
 */
#ifndef VGL_WIRE_H
#define VGL_WIRE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Command ids (enum virgl_context_cmd, transcribed subset; the full   */
/* enum is ordered - 0 NOP .. 52 LINK_SHADER, then the video block)    */
/* ------------------------------------------------------------------ */

enum {
    VIRGL_CCMD_NOP = 0,
    /* ... 1..52: GL-only commands, see reference/mesa-ref-virgl_protocol.h */
    VIRGL_CCMD_TRANSFER3D = 43,          /* cross-checked vs virglrenderer */
    VIRGL_CCMD_SET_TWEAKS = 46,
    VIRGL_CCMD_CREATE_VIDEO_CODEC       = 53,
    VIRGL_CCMD_DESTROY_VIDEO_CODEC      = 54,
    VIRGL_CCMD_CREATE_VIDEO_BUFFER      = 55,
    VIRGL_CCMD_DESTROY_VIDEO_BUFFER     = 56,
    VIRGL_CCMD_BEGIN_FRAME              = 57,
    VIRGL_CCMD_DECODE_MACROBLOCK        = 58,
    VIRGL_CCMD_DECODE_BITSTREAM         = 59,
    VIRGL_CCMD_ENCODE_BITSTREAM         = 60,
    VIRGL_CCMD_END_FRAME                = 61,
};

/* 8-bit cmd | 8-bit obj | 16-bit length (payload dwords, header excluded) */
#define VIRGL_CMD0(cmd, obj, len) ((cmd) | ((obj) << 8) | ((len) << 16))

/* Payload dword indices (verbatim from virgl_protocol.h lines 727-780) */

/* VIRGL_CCMD_CREATE_VIDEO_CODEC - dw0 = VIRGL_CMD0 header */
#define VIRGL_CREATE_VIDEO_CODEC_HANDLE     1  /* guest-assigned object handle */
#define VIRGL_CREATE_VIDEO_CODEC_PROFILE    2
#define VIRGL_CREATE_VIDEO_CODEC_ENTRYPOINT 3
#define VIRGL_CREATE_VIDEO_CODEC_CHROMA_FMT 4
#define VIRGL_CREATE_VIDEO_CODEC_LEVEL      5
#define VIRGL_CREATE_VIDEO_CODEC_WIDTH      6
#define VIRGL_CREATE_VIDEO_CODEC_HEIGHT     7
#define VIRGL_CREATE_VIDEO_CODEC_MAX_REF    8  /* ONLY when host_feature_check_version >= 14 */

/* VIRGL_CCMD_DESTROY_VIDEO_CODEC */
#define VIRGL_DESTROY_VIDEO_CODEC_HANDLE    1

/* VIRGL_CCMD_CREATE_VIDEO_BUFFER */
#define VIRGL_CREATE_VIDEO_BUFFER_HANDLE    1
#define VIRGL_CREATE_VIDEO_BUFFER_FORMAT    2  /* enum virgl_formats numeric id */
#define VIRGL_CREATE_VIDEO_BUFFER_WIDTH     3
#define VIRGL_CREATE_VIDEO_BUFFER_HEIGHT    4
#define VIRGL_CREATE_VIDEO_BUFFER_RES_BASE  5  /* followed by num_planes resource handles */

/* VIRGL_CCMD_DESTROY_VIDEO_BUFFER */
#define VIRGL_DESTROY_VIDEO_BUFFER_HANDLE   1

/* VIRGL_CCMD_BEGIN_FRAME */
#define VIRGL_BEGIN_FRAME_CDC_HANDLE        1  /* codec handle */
#define VIRGL_BEGIN_FRAME_TGT_HANDLE        2  /* video buffer handle */

/* VIRGL_CCMD_ENCODE_BITSTREAM */
#define VIRGL_ENCODE_BS_CDC_HANDLE          1  /* codec handle */
#define VIRGL_ENCODE_BS_SRC_HANDLE          2  /* source video buffer handle */
#define VIRGL_ENCODE_BS_DEST_HANDLE         3  /* coded buffer RESOURCE handle */
#define VIRGL_ENCODE_BS_DESC_HANDLE         4  /* desc buffer RESOURCE handle  */
#define VIRGL_ENCODE_BS_FEED_HANDLE         5  /* feedback buffer RESOURCE handle */

/* VIRGL_CCMD_END_FRAME */
#define VIRGL_END_FRAME_CDC_HANDLE          1
#define VIRGL_END_FRAME_TGT_HANDLE          2

/* ------------------------------------------------------------------ */
/* pipe enums as they appear ON THE WIRE                               */
/* ------------------------------------------------------------------ */

/* enum pipe_video_profile - guest Mesa 26.2.4 / openKylin numbering
 * (F17 final: MPEG4_SIMPLE/ADVANCED_SIMPLE deleted, + MPEG1, + HEVC
 * 10-bit/444/422 variants). Matches host guest_to_host_profile() 1:1. */
enum vgl_wire_profile {
    PIPE_VIDEO_PROFILE_UNKNOWN = 0,
    PIPE_VIDEO_PROFILE_MPEG1,
    PIPE_VIDEO_PROFILE_MPEG2_SIMPLE,
    PIPE_VIDEO_PROFILE_MPEG2_MAIN,
    PIPE_VIDEO_PROFILE_VC1_SIMPLE,
    PIPE_VIDEO_PROFILE_VC1_MAIN,
    PIPE_VIDEO_PROFILE_VC1_ADVANCED,
    PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE,             /* 7 */
    PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE, /* 8 */
    PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN,                 /* 9  */
    PIPE_VIDEO_PROFILE_MPEG4_AVC_EXTENDED,             /* 10 */
    PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH,                 /* 11 - RustDesk/ffmpeg default */
    PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH10,
    PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH422,
    PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH444,
    PIPE_VIDEO_PROFILE_HEVC_MAIN,                      /* 15 - r9 verified working */
    PIPE_VIDEO_PROFILE_HEVC_MAIN_10,                   /* 16 - r9 verified working */
    PIPE_VIDEO_PROFILE_HEVC_MAIN_STILL,
    PIPE_VIDEO_PROFILE_HEVC_MAIN_12,
    PIPE_VIDEO_PROFILE_HEVC_MAIN10_444,
    PIPE_VIDEO_PROFILE_HEVC_MAIN_422,
    PIPE_VIDEO_PROFILE_HEVC_MAIN10_422,
    PIPE_VIDEO_PROFILE_HEVC_MAIN_444,
    PIPE_VIDEO_PROFILE_JPEG_BASELINE,                  /* 23 */
    PIPE_VIDEO_PROFILE_VP9_PROFILE0,                   /* 24 */
    PIPE_VIDEO_PROFILE_VP9_PROFILE2,
    PIPE_VIDEO_PROFILE_AV1_MAIN,                       /* 26 */
    PIPE_VIDEO_PROFILE_AV1_PROFILE2,
};

/* enum pipe_video_entrypoint - identical on guest 26.2.4 and host tree. */
enum vgl_wire_entrypoint {
    PIPE_VIDEO_ENTRYPOINT_UNKNOWN   = 0,
    PIPE_VIDEO_ENTRYPOINT_BITSTREAM = 1,   /* decode */
    PIPE_VIDEO_ENTRYPOINT_IDCT      = 2,
    PIPE_VIDEO_ENTRYPOINT_MC        = 3,
    PIPE_VIDEO_ENTRYPOINT_ENCODE    = 4,   /* encode (VAEncSlice) */
    PIPE_VIDEO_ENTRYPOINT_PROCESSING = 5,  /* guest only */
};

/* enum pipe_video_chroma_format (gallium-stable numbering). */
enum vgl_wire_chroma_format {
    PIPE_VIDEO_CHROMA_FORMAT_NONE = 0,
    PIPE_VIDEO_CHROMA_FORMAT_400,
    PIPE_VIDEO_CHROMA_FORMAT_420,   /* 2 - use for NV12 input */
    PIPE_VIDEO_CHROMA_FORMAT_422,
    PIPE_VIDEO_CHROMA_FORMAT_444,
};

/* enum pipe_h2645_enc_picture_type (explicit in p_video_state.h). */
enum vgl_wire_enc_picture_type {
    PIPE_H2645_ENC_PICTURE_TYPE_P    = 0x00,
    PIPE_H2645_ENC_PICTURE_TYPE_B    = 0x01,
    PIPE_H2645_ENC_PICTURE_TYPE_I    = 0x02,
    PIPE_H2645_ENC_PICTURE_TYPE_IDR  = 0x03,
    PIPE_H2645_ENC_PICTURE_TYPE_SKIP = 0x04,
};

/* enum pipe_h2645_enc_rate_control_method (explicit in p_video_state.h). */
enum vgl_wire_enc_rc_method {
    PIPE_H2645_ENC_RATE_CONTROL_METHOD_DISABLE          = 0x00,
    PIPE_H2645_ENC_RATE_CONTROL_METHOD_CONSTANT_SKIP    = 0x01,
    PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE_SKIP    = 0x02,
    PIPE_H2645_ENC_RATE_CONTROL_METHOD_CONSTANT         = 0x03,  /* CBR */
    PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE         = 0x04,  /* VBR */
    PIPE_H2645_ENC_RATE_CONTROL_METHOD_QUALITY_VARIABLE = 0x05,
};

/* enum virgl_formats id of PIPE_FORMAT_NV12 (virgl_hw.h:222). */
#define VGL_WIRE_FORMAT_NV12  166

/* ------------------------------------------------------------------ */
/* Wire structs - verbatim from virgl_video_hw.h (enc subset).         */
/* Natural alignment, NO packing pragma - the authoritative header     */
/* relies on plain member ordering (all structs are 4-byte multiples). */
/* ------------------------------------------------------------------ */

struct virgl_base_picture_desc {
    uint16_t profile;       /* enum vgl_wire_profile */
    uint8_t entry_point;    /* enum vgl_wire_entrypoint */
    uint8_t protected_playback;
    uint8_t decrypt_key[256];
    uint32_t key_size;
};                                          /* 264 bytes */

struct virgl_enc_quality_modes {
    uint32_t level;
    uint32_t preset_mode;
    uint32_t pre_encode_mode;
    uint32_t vbaq_mode;
};                                          /* 16 bytes */

/* H.264 encode ------------------------------------------------------------ */

struct virgl_h264_enc_seq_param {
    uint32_t enc_constraint_set_flags;
    uint32_t enc_frame_cropping_flag;
    uint32_t enc_frame_crop_left_offset;
    uint32_t enc_frame_crop_right_offset;
    uint32_t enc_frame_crop_top_offset;
    uint32_t enc_frame_crop_bottom_offset;
    uint32_t pic_order_cnt_type;
    uint32_t num_temporal_layers;
    uint32_t vui_parameters_present_flag;
    struct {
        uint32_t aspect_ratio_info_present_flag: 1;
        uint32_t timing_info_present_flag: 1;
        uint32_t reserved: 30;
    } vui_flags;
    uint32_t aspect_ratio_idc;
    uint32_t sar_width;
    uint32_t sar_height;
    uint32_t num_units_in_tick;
    uint32_t time_scale;
};                                          /* 60 bytes */

struct virgl_h264_enc_rate_control {
    uint32_t target_bitrate;
    uint32_t peak_bitrate;
    uint32_t frame_rate_num;
    uint32_t frame_rate_den;
    uint32_t vbv_buffer_size;
    uint32_t vbv_buf_lv;
    uint32_t target_bits_picture;
    uint32_t peak_bits_picture_integer;
    uint32_t peak_bits_picture_fraction;
    uint32_t fill_dataEnable;
    uint32_t skip_frame_enable;
    uint32_t enforce_hrd;
    uint32_t max_au_size;
    uint32_t max_qp;
    uint32_t min_qp;

    uint8_t rate_ctrl_method; /* enum vgl_wire_enc_rc_method */
    uint8_t reserved[3];
};                                          /* 64 bytes - array[4] on the desc */

struct virgl_h264_enc_motion_estimation {
    uint32_t motion_est_quarter_pixel;
    uint32_t enc_disable_sub_mode;
    uint32_t lsmvert;
    uint32_t enc_en_ime_overw_dis_subm;
    uint32_t enc_ime_overw_dis_subm_no;
    uint32_t enc_ime2_search_range_x;
    uint32_t enc_ime2_search_range_y;
};                                          /* 28 bytes */

struct virgl_h264_enc_pic_control {
    uint32_t enc_cabac_enable;
    uint32_t enc_cabac_init_idc;
};                                          /* 8 bytes */

struct virgl_h264_slice_descriptor {
    uint32_t macroblock_address;
    uint32_t num_macroblocks;

    uint8_t slice_type; /* enum pipe_h264_slice_type */
    uint8_t reserved[3];
};                                          /* 12 bytes */

struct virgl_h264_enc_picture_desc {
    struct virgl_base_picture_desc base;    /* +0   */

    struct virgl_h264_enc_seq_param seq;             /* +264 */
    struct virgl_h264_enc_rate_control rate_ctrl[4]; /* +324 */
    struct virgl_h264_enc_motion_estimation motion_est; /* +580 */
    struct virgl_h264_enc_pic_control pic_ctrl;      /* +608 */

    uint32_t intra_idr_period;              /* +616 */

    uint32_t quant_i_frames;                /* +620 */
    uint32_t quant_p_frames;
    uint32_t quant_b_frames;

    uint32_t frame_num;                     /* +632 */
    uint32_t frame_num_cnt;
    uint32_t p_remain;
    uint32_t i_remain;
    uint32_t idr_pic_id;
    uint32_t gop_cnt;
    uint32_t pic_order_cnt;
    uint32_t num_ref_idx_l0_active_minus1;
    uint32_t num_ref_idx_l1_active_minus1;
    uint32_t ref_idx_l0_list[32];           /* +668 */
    uint8_t l0_is_long_term[32];
    uint32_t ref_idx_l1_list[32];
    uint8_t l1_is_long_term[32];
    uint32_t gop_size;                      /* +988 */
    struct virgl_enc_quality_modes quality_modes;

    uint32_t num_slice_descriptors;         /* +1008 */
    struct virgl_h264_slice_descriptor slices_descriptors[128]; /* +1012 */

    uint8_t picture_type;                   /* +2548: enum vgl_wire_enc_picture_type */
    uint8_t not_referenced;
    uint8_t is_ltr;
    uint8_t enable_vui;

    uint32_t ltr_index;                     /* +2552 */
};                                          /* 2556 bytes total */

/* H.265 encode (M5; host closed-loop since r9/r10/v12.5) ----------------- */

struct virgl_h265_enc_seq_param {
    uint8_t general_profile_idc;
    uint8_t general_level_idc;
    uint8_t general_tier_flag;
    uint8_t strong_intra_smoothing_enabled_flag;

    uint32_t intra_period;
    uint32_t ip_period;

    uint16_t pic_width_in_luma_samples;
    uint16_t pic_height_in_luma_samples;

    uint32_t chroma_format_idc;
    uint32_t bit_depth_luma_minus8;
    uint32_t bit_depth_chroma_minus8;

    uint8_t amp_enabled_flag;
    uint8_t sample_adaptive_offset_enabled_flag;
    uint8_t pcm_enabled_flag;
    uint8_t sps_temporal_mvp_enabled_flag;

    uint8_t log2_min_luma_coding_block_size_minus3;
    uint8_t log2_diff_max_min_luma_coding_block_size;
    uint8_t log2_min_transform_block_size_minus2;
    uint8_t log2_diff_max_min_transform_block_size;

    uint16_t conf_win_left_offset;
    uint16_t conf_win_right_offset;
    uint16_t conf_win_top_offset;
    uint16_t conf_win_bottom_offset;

    uint32_t vui_parameters_present_flag;
    struct {
        uint32_t aspect_ratio_info_present_flag: 1;
        uint32_t timing_info_present_flag: 1;
        uint32_t reserved: 30;
    } vui_flags;
    uint32_t aspect_ratio_idc;
    uint32_t sar_width;
    uint32_t sar_height;
    uint32_t num_units_in_tick;
    uint32_t time_scale;

    uint8_t max_transform_hierarchy_depth_inter;
    uint8_t max_transform_hierarchy_depth_intra;
    uint8_t conformance_window_flag;
    uint8_t reserved;
};                                          /* 76 bytes */

struct virgl_h265_enc_pic_param {
    uint8_t log2_parallel_merge_level_minus2;
    uint8_t nal_unit_type;
    uint8_t constrained_intra_pred_flag;
    uint8_t pps_loop_filter_across_slices_enabled_flag;

    uint8_t transform_skip_enabled_flag;
    uint8_t reserved[3];
};                                          /* 8 bytes */

struct virgl_h265_enc_slice_param {
    uint8_t max_num_merge_cand;
    int8_t slice_cb_qp_offset;
    int8_t slice_cr_qp_offset;
    int8_t slice_beta_offset_div2;

    uint32_t slice_deblocking_filter_disabled_flag;

    int8_t slice_tc_offset_div2;
    uint8_t cabac_init_flag;
    uint8_t slice_loop_filter_across_slices_enabled_flag;
    uint8_t reserved;
};                                          /* 12 bytes */

struct virgl_h265_enc_rate_control {
    uint32_t target_bitrate;
    uint32_t peak_bitrate;
    uint32_t frame_rate_num;
    uint32_t frame_rate_den;
    uint32_t quant_i_frames;
    uint32_t quant_p_frames;
    uint32_t quant_b_frames;
    uint32_t vbv_buffer_size;
    uint32_t vbv_buf_lv;
    uint32_t target_bits_picture;
    uint32_t peak_bits_picture_integer;
    uint32_t peak_bits_picture_fraction;
    uint32_t fill_dataEnable;
    uint32_t skip_frame_enable;
    uint32_t enforce_hrd;
    uint32_t max_au_size;
    uint32_t max_qp;
    uint32_t min_qp;

    uint8_t rate_ctrl_method;
    uint8_t reserved[3];
};                                          /* 76 bytes: 18 x u32 + method + reserved */

struct virgl_h265_slice_descriptor {
    uint32_t slice_segment_address;
    uint32_t num_ctu_in_slice;

    uint8_t slice_type; /* enum pipe_h265_slice_type */
    uint8_t reserved[3];
};                                          /* 12 bytes */

struct virgl_h265_enc_picture_desc {
    struct virgl_base_picture_desc base;            /* +0 */
    struct virgl_h265_enc_seq_param seq;            /* +264 */
    struct virgl_h265_enc_pic_param pic;            /* +340 */
    struct virgl_h265_enc_slice_param slice;        /* +348 */
    struct virgl_h265_enc_rate_control rc;          /* +360 */

    uint32_t decoded_curr_pic;                      /* +436 */
    uint32_t reference_frames[16];
    uint32_t frame_num;
    uint32_t pic_order_cnt;
    uint32_t pic_order_cnt_type;
    uint32_t num_ref_idx_l0_active_minus1;
    uint32_t num_ref_idx_l1_active_minus1;
    uint32_t ref_idx_l0_list[15];
    uint32_t ref_idx_l1_list[15];
    uint32_t num_slice_descriptors;                 /* +644 */
    struct virgl_h265_slice_descriptor slices_descriptors[128];
    struct virgl_enc_quality_modes quality_modes;

    uint8_t picture_type;                           /* enum vgl_wire_enc_picture_type */
    uint8_t not_referenced;
    uint8_t reserved[2];
};                                          /* 2204 bytes total */

/* Feedback ---------------------------------------------------------------- */

enum virgl_video_encode_stat {
    VIRGL_VIDEO_ENCODE_STAT_NOT_STARTED = 0,
    VIRGL_VIDEO_ENCODE_STAT_IN_PROGRESS,
    VIRGL_VIDEO_ENCODE_STAT_SUCCESS,
    VIRGL_VIDEO_ENCODE_STAT_FAILURE,
};

struct virgl_video_encode_feedback {
    uint8_t stat;           /* enum virgl_video_encode_stat */
    uint8_t reserved[3];

    uint32_t coded_size;    /* size of encoded data in bytes */
};                                          /* 8 bytes */

/* ------------------------------------------------------------------ */
/* Caps (GET_CAPS v2 tail; see reference/mesa-ref-virgl_hw.h for the    */
/* full struct virgl_caps_v2 - transcribe at M1 once the bridge can    */
/* answer GET_CAPS).                                                    */
/* ------------------------------------------------------------------ */

struct virgl_video_caps {
    uint32_t profile:8;           /* raw guest-numeric profile value */
    uint32_t entrypoint:8;
    uint32_t max_level:8;
    uint32_t stacked_frames:8;

    uint32_t max_width:16;
    uint32_t max_height:16;

    uint32_t prefered_format:16;
    uint32_t max_macroblocks:16;

    uint32_t npot_texture:1;
    uint32_t supports_progressive:1;
    uint32_t supports_interlaced:1;
    uint32_t prefers_interlaced:1;
    uint32_t max_temporal_layers:8;
    uint32_t reserved:20;
};

/* caps.v2.host_feature_check_version >= 14: CREATE_VIDEO_CODEC carries the
 * max_ref dword (index 8). Mirrors virgl_encode.c:1780. */
#define VGL_WIRE_HOST_FEAT_MAX_REF 14

/* Desc buffer size: Mesa allocates sizeof(union virgl_picture_desc) per
 * codec; the host reads only the codec-specific struct, so sizing by the
 * largest desc we actually send is sufficient. */
#define VGL_WIRE_DESC_BUF_SIZE \
    ((sizeof(struct virgl_h264_enc_picture_desc) > \
      sizeof(struct virgl_h265_enc_picture_desc)) ? \
      sizeof(struct virgl_h264_enc_picture_desc) : \
      sizeof(struct virgl_h265_enc_picture_desc))

/* ------------------------------------------------------------------ */
/* Validation                                                           */
/* ------------------------------------------------------------------ */

/* Compile-time pins: any mismatch with the authoritative layout fails the
 * build loudly instead of silently misaligned IO (F17 lesson). */
typedef char vgl_wire_size_check_base[
    (sizeof(struct virgl_base_picture_desc) == 264) ? 1 : -1];
typedef char vgl_wire_size_check_h264_rc[
    (sizeof(struct virgl_h264_enc_rate_control) == 64) ? 1 : -1];
typedef char vgl_wire_size_check_h264_enc[
    (sizeof(struct virgl_h264_enc_picture_desc) == 2556) ? 1 : -1];
typedef char vgl_wire_size_check_h265_enc[
    (sizeof(struct virgl_h265_enc_picture_desc) == 2204) ? 1 : -1];
typedef char vgl_wire_size_check_feedback[
    (sizeof(struct virgl_video_encode_feedback) == 8) ? 1 : -1];
typedef char vgl_wire_size_check_caps[
    (sizeof(struct virgl_video_caps) == 16) ? 1 : -1];

/* Runtime gate (unchanged API): verifies the pins plus field offsets. */
int  vgl_wire_check(void);

/* Helpers used by tests and vgl_session.c. */
size_t   vgl_wire_enc_desc_size(void);
uint16_t vgl_wire_profile_from_h264(int high);   /* 1->11, 0->9 */

#ifdef __cplusplus
}
#endif

#endif /* VGL_WIRE_H */
