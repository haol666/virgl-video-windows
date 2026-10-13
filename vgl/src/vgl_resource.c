/*
 * vgl_resource.c - NV12 frame resource pool for libvgl.
 *
 * Design doc §3.3: pool of pinned linear NV12 buffers shared with the
 * host through virtio-gpu ATTACH_BACKING.  System memory is LINEAR by
 * construction, which satisfies the host's LINEAR surface constraint
 * (handover F1 lesson) with zero copies on the host side.
 */
#include "vgl_internal.h"

#include <stdlib.h>
#include <string.h>

#define VGL_POOL_DEPTH 4            /* design doc §3.3 */
#define VGL_STRIDE_ALIGN 256

vgl_resource_pool_t *vgl_pool_create(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || (width | height) & 1)
        return NULL;

    vgl_resource_pool_t *p = (vgl_resource_pool_t *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;

    p->width = width;
    p->height = height;
    p->y_stride = (width + VGL_STRIDE_ALIGN - 1) & ~(VGL_STRIDE_ALIGN - 1);
    p->uv_stride = p->y_stride;
    p->y_size = p->y_stride * height;
    p->uv_size = p->uv_stride * (height / 2);
    p->frame_size = p->y_size + p->uv_size;
    p->depth = VGL_POOL_DEPTH;
    p->next = 0;

    /* Buffers are plain system memory - allocate up front so frame staging
     * (vgl_pool_fill) works without the bridge too (CI, bare metal).
     * Only PIN/backing registration needs the device. */
    for (uint32_t i = 0; i < p->depth; i++) {
        p->slots[i].buffer = malloc(p->frame_size);
        if (!p->slots[i].buffer) {
            vgl_pool_destroy(p);
            return NULL;
        }
        p->slots[i].size = p->frame_size;
    }
    return p;
}

/* M1: for each slot perform
 *   RESOURCE_CREATE(NV12, width, height)
 *   IOCTL_VGL_PIN(slot->buffer, frame_size) -> pfn table
 *   ATTACH_BACKING(resource_id, pfn table, frame_size)
 * Until the bridge driver lands (M1), binding is deferred: vgl_pool_bind()
 * returns VGL_E_NO_DEVICE, callers fall back cleanly. */
int vgl_pool_bind(vgl_resource_pool_t *p)
{
    if (!p)
        return VGL_E_PARAM;
    if (p->bound)
        return VGL_OK;
    if (!vgl_tport_ready())
        return VGL_E_NO_DEVICE;

    for (uint32_t i = 0; i < p->depth; i++) {
        vgl_res_slot_t *s = &p->slots[i];

        uint64_t handle = 0;
        uint32_t count = 0, off = 0;
        int rc = vgl_tport_pin(s->buffer, p->frame_size, &handle,
                               s->pfns, VGL_MAX_PFNS, &count, &off);
        if (rc != VGL_OK)
            goto fail;
        s->pin_handle = handle;
        s->pfn_count = count;
        s->res_id = 0;           /* assigned by vgl_session emit (M1) */

        s->ready = 1;
    }
    p->bound = 1;
    return VGL_OK;

fail:
    vgl_pool_unbind(p);
    return VGL_E_DRIVER;
}

void vgl_pool_unbind(vgl_resource_pool_t *p)
{
    if (!p)
        return;
    for (uint32_t i = 0; i < p->depth; i++) {
        vgl_res_slot_t *s = &p->slots[i];
        if (s->pin_handle) {
            vgl_tport_unpin(s->pin_handle);
            s->pin_handle = 0;
        }
        s->ready = 0;
    }
    p->bound = 0;
}

void vgl_pool_destroy(vgl_resource_pool_t *p)
{
    if (!p)
        return;
    vgl_pool_unbind(p);
    for (uint32_t i = 0; i < p->depth; i++) {
        free(p->slots[i].buffer);
        p->slots[i].buffer = NULL;
    }
    free(p);
}

/* Copy an NV12 frame into the next pool slot (round robin).
 * Returns the slot index used, or -1 on invalid frame. */
int vgl_pool_fill(vgl_resource_pool_t *p, const vgl_frame_t *f)
{
    if (!p || !f || !f->y_plane || !f->uv_plane)
        return -1;
    if (f->width > p->width || f->height > p->height)
        return -1;

    int slot = (int)p->next;
    p->next = (p->next + 1) % p->depth;

    uint8_t *dst_y = (uint8_t *)p->slots[slot].buffer;
    uint8_t *dst_uv = dst_y + p->y_size;

    for (uint32_t r = 0; r < f->height; r++)
        memcpy(dst_y + (size_t)r * p->y_stride,
               f->y_plane + (size_t)r * f->y_stride, f->width);
    for (uint32_t r = 0; r < f->height / 2; r++)
        memcpy(dst_uv + (size_t)r * p->uv_stride,
               f->uv_plane + (size_t)r * f->uv_stride, f->width);

    return slot;
}

size_t vgl_pool_frame_size(uint32_t width, uint32_t height)
{
    uint32_t ys = (width + VGL_STRIDE_ALIGN - 1) & ~(VGL_STRIDE_ALIGN - 1);
    return (size_t)ys * (height + height / 2);
}
