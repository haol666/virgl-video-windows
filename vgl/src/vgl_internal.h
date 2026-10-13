/*
 * vgl_internal.h - internal types shared by libvgl translation units.
 */
#ifndef VGL_INTERNAL_H
#define VGL_INTERNAL_H

#include "vgl/vgl.h"
#include "vgl/vgl_wire.h"

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* transport prototypes (vgl_transport_win.c)                          */
/* ------------------------------------------------------------------ */

int  vgl_tport_ready(void);
const struct vgl_tport_info *vgl_tport_info(void);
int  vgl_tport_open(void);
void vgl_tport_close(void);

int  vgl_tport_submit(const void *cmd, uint32_t len, uint32_t *token);
int  vgl_tport_reap(void *out, uint32_t cap, uint32_t *out_len);
int  vgl_tport_wait(uint32_t timeout_ms, uint32_t *signaled);
int  vgl_tport_pin(const void *va, uint32_t len, uint64_t *handle,
                   uint64_t *pfns, uint32_t max_pfns, uint32_t *pfn_count,
                   uint32_t *first_off);
int  vgl_tport_unpin(uint64_t handle);

#define VGL_MAX_PFNS 4096      /* 1080p NV12 ~ 3MB = ~768 PFNs, ample */

/* ------------------------------------------------------------------ */
/* resource pool                                                       */
/* ------------------------------------------------------------------ */

typedef struct vgl_res_slot {
    uint8_t *buffer;           /* pinned NV12 frame storage        */
    uint32_t size;
    uint64_t pin_handle;
    uint64_t pfns[VGL_MAX_PFNS];
    uint32_t pfn_count;
    uint32_t res_id;           /* virtio-gpu resource id, M1       */
    int      ready;
} vgl_res_slot_t;

typedef struct vgl_resource_pool {
    uint32_t width, height;
    uint32_t y_stride, uv_stride;
    uint32_t y_size, uv_size, frame_size;
    uint32_t depth, next;
    int      bound;
    vgl_res_slot_t slots[4];
} vgl_resource_pool_t;

vgl_resource_pool_t *vgl_pool_create(uint32_t width, uint32_t height);
int   vgl_pool_bind(vgl_resource_pool_t *p);
void  vgl_pool_unbind(vgl_resource_pool_t *p);
void  vgl_pool_destroy(vgl_resource_pool_t *p);
int   vgl_pool_fill(vgl_resource_pool_t *p, const vgl_frame_t *f);
size_t vgl_pool_frame_size(uint32_t width, uint32_t height);

/* ------------------------------------------------------------------ */
/* session                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    VGL_SESS_OPENED = 0,
    VGL_SESS_CTX_CREATED,
    VGL_SESS_CODEC_CREATED,
    VGL_SESS_RESOURCES_READY,
    VGL_SESS_ACTIVE,
    VGL_SESS_FAILED,
} vgl_sess_state_t;

struct vgl_encoder {
    vgl_enc_params_t params;
    vgl_sess_state_t state;

    uint32_t ctx_id;             /* virgl context, 0 until created */
    uint32_t codec_id;           /* host session id from create_codec */
    uint32_t next_res_id;
    uint32_t next_buf_id;
    uint32_t cur_slot;

    vgl_resource_pool_t *pool;

    /* feedback + in-band bitstream staging */
    uint8_t *out_buf;
    uint32_t out_cap;
    vgl_packet_t last_packet;

    uint32_t fence_timeout_ms;
    uint32_t flags;              /* pending_force_idr etc. */
};

/* Command emission abstraction - the ONLY place that changes when the
 * authoritative header decides encoding (a) SUBMIT_3D embedded vs
 * (b) dedicated control-queue commands (design doc §3.4). */
int vgl_cmd_emit_create_codec(struct vgl_encoder *e);
int vgl_cmd_emit_destroy_codec(struct vgl_encoder *e);
int vgl_cmd_emit_frame_seq(struct vgl_encoder *e, int slot, int force_idr);
int vgl_cmd_collect_feedback(struct vgl_encoder *e, vgl_packet_t *out);

#endif /* VGL_INTERNAL_H */
