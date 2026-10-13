/*
 * vgl - virtio-gpu video (virgl channel) codec client library for Windows guests.
 *
 * Layer 1 of the Windows guest H.264 passthrough stack (see
 * docs/WINDOWS_IMPL_DESIGN.md).  Pure C, no OS dependencies beyond Win32
 * device IO in vgl_transport_win.c so that it can be statically linked into
 * FFmpeg (h264_vgl encoder) and reused by any other consumer.
 *
 * Contract references are to WINDOWS_GUEST_DESIGN.md (handover document,
 * host side frozen at v12.3h-r8 / libvirglrenderer 2f8c71ce).
 */
#ifndef VGL_H
#define VGL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

#define VGL_OK                 0
#define VGL_E_NO_DEVICE      (-1)   /* bridge driver (\\.\VGLVIDEO0) absent */
#define VGL_E_DRIVER         (-2)   /* IOCTL failed / device removed */
#define VGL_E_WIRE           (-3)   /* vgl_wire_check() failed (see G1 gate) */
#define VGL_E_PROTO          (-4)   /* command sequence / fence timeout */
#define VGL_E_PARAM          (-5)   /* invalid encoder params */
#define VGL_E_NOMEM          (-6)
#define VGL_E_HOST           (-7)   /* host side rejected / faulted session */

/* ------------------------------------------------------------------ */
/* Wire-level enums (hard values - vgl_wire.h enum vgl_wire_profile,     */
/* guest Mesa 26.2.4 numbering; three-way cross-validated at G1)         */
/* ------------------------------------------------------------------ */

enum {
    VGL_PROFILE_H264_BASELINE = 7,
    VGL_PROFILE_H264_CONSTRAINED_BASELINE = 8,
    VGL_PROFILE_H264_MAIN    = 9,
    VGL_PROFILE_H264_HIGH    = 11,   /* hwcodec/ffmpeg default request */
    /* HEVC: host r9/r10/v12.5 closed-loop verified - M5 scope */
    VGL_PROFILE_HEVC_MAIN_8  = 15,
    VGL_PROFILE_HEVC_MAIN_10 = 16,
};

enum {
    VGL_ENTRYPOINT_BITSTREAM = 1,   /* decode, out of scope this phase */
    VGL_ENTRYPOINT_ENCODE    = 4,
};

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct vgl_caps {
    uint32_t profiles;            /* bitmask: see VGL_CAP_PROFILE_* */
    uint32_t max_width;
    uint32_t max_height;
    uint32_t frame_rates;         /* reported ceiling after probe     */
} vgl_caps_t;

#define VGL_CAP_PROFILE_H264_BASELINE (1u << 8)
#define VGL_CAP_PROFILE_H264_MAIN    (1u << 9)
#define VGL_CAP_PROFILE_H264_HIGH    (1u << 11)

typedef enum {
    VGL_RC_CBR = 0,
    VGL_RC_VBR,
    VGL_RC_QP,                    /* constant QP */
} vgl_rc_mode_t;

typedef struct vgl_enc_params {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t gop;
    uint32_t bitrate_kbps;
    uint32_t qp;                  /* used when rc == VGL_RC_QP */
    uint16_t profile;             /* VGL_PROFILE_* */
    vgl_rc_mode_t rc;
    int zerolatency;              /* bf=0, slice-level output (default on) */
} vgl_enc_params_t;

/* Input frame: NV12 system memory (LINEAR - satisfies host §3.4-4) */
typedef struct vgl_frame {
    const uint8_t *y_plane;
    const uint8_t *uv_plane;
    uint32_t y_stride;
    uint32_t uv_stride;
    uint32_t width;               /* active area, may be < stride */
    uint32_t height;
    int64_t pts_us;
} vgl_frame_t;

/* Output packet: Annex-B bitstream exactly as synthesized by the host
 * (in-band SPS/PPS/AUD + slice data, handover §3.4-2/3).  No extradata. */
typedef struct vgl_packet {
    uint8_t *data;
    uint32_t len;
    int64_t pts_us;
    int keyframe;                 /* 1 = IDR */
    int end_of_stream;
} vgl_packet_t;

typedef struct vgl_encoder vgl_encoder_t;

/* ------------------------------------------------------------------ */
/* Process-level API                                                   */
/* ------------------------------------------------------------------ */

/* Open the bridge device (lazy singleton).  Idempotent. */
int  vgl_open(void);
void vgl_close(void);

/* Capability probe: performs one create_codec/destroy round trip at a
 * minimal (e.g. 64x64) resolution.  Used by enumeration paths; must be
 * cheap and leave no residual session on the host.
 * Fills caps on success.  Returns VGL_OK or negative error code. */
int  vgl_probe(vgl_caps_t *caps);

/* ------------------------------------------------------------------ */
/* Session API (single-threaded per encoder instance)                  */
/* ------------------------------------------------------------------ */

vgl_encoder_t *vgl_encoder_open(const vgl_enc_params_t *p);
void vgl_encoder_close(vgl_encoder_t *e);

/* Submit one frame; on VGL_OK, *out holds one encoded packet (owned by
 * the session until the next call).  may_block up to fence timeout. */
int  vgl_encode_frame(vgl_encoder_t *e, const vgl_frame_t *f, vgl_packet_t *out);

/* Flush pending (no-op in zerolatency mode), then end of stream marker. */
int  vgl_encoder_flush(vgl_encoder_t *e, vgl_packet_t *out);

/* Runtime reconfig, mirrors hwcodec EncodeContext::set_bitrate. */
int  vgl_encoder_set_bitrate(vgl_encoder_t *e, uint32_t kbps);

/* Force an IDR on the next frame (RustDesk request_key_frame path). */
int  vgl_encoder_force_idr(vgl_encoder_t *e);

/* Runtime diagnostics. */
const char *vgl_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif /* VGL_H */
