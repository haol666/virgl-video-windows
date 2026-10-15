/*
 * probe_win.c - L1 transport probe: libvgl -> \\.\VGLVIDEO0 -> M1b data plane.
 *
 * Validates the exact code path FFmpeg's h264_vgl encoder will use
 * (vgl_tport_submit/vgl_tport_reap), using the always-safe control-queue
 * GET_DISPLAY_INFO command as the round-trip vehicle.
 *
 * Exits 0 on full pass:
 *   [1] tport open + GET_INFO (magic/virtio/qdepth/msix)
 *   [2] submit GET_DISPLAY_INFO (24B ctrl hdr) -> token
 *   [3] reap with poll -> response type 0x1101 OK_DISPLAY_INFO
 *
 * Run on the test VM with the M1b-p0.3+ driver loaded.
 */

#include "vgl/vgl.h"
#include "../src/vgl_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

/* Minimal virtio-gpu control-queue header (guest view, 24 bytes). */
typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} gpu_ctrl_hdr_t;

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100u
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101u

#define VGL_MAGIC 0x31564756u /* 'V','G','V','1' */

int main(void)
{
    printf("=== vgl L1 transport probe ===\n");

    /* ---- [1] transport open + GET_INFO ---- */
    if (vgl_tport_open() != 0) {
        printf("[FAIL] vgl_tport_open - driver absent? (\\\\.\\VGLVIDEO0)\n");
        return 1;
    }
    const vgl_tport_info_t *info = vgl_tport_info();
    if (!info) {
        printf("[FAIL] vgl_tport_info NULL after open\n");
        return 1;
    }
    printf("[1] open OK: magic=0x%08x virtio=%u qdepth=%u notify=%u msix=%u\n",
           info->magic, info->virtio_version, info->control_queue_depth,
           info->notify_cap, info->msix_enabled);
    if (info->magic != VGL_MAGIC) {
        printf("[FAIL] bad magic (expect 0x%08x)\n", VGL_MAGIC);
        return 1;
    }

    /* ---- [2] submit GET_DISPLAY_INFO ---- */
    gpu_ctrl_hdr_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    uint32_t token = 0;
    int rc = vgl_tport_submit(&cmd, sizeof(cmd), &token);
    if (rc != VGL_OK) {
        printf("[FAIL] submit rc=%d (%s)\n", rc, vgl_strerror(rc));
        return 1;
    }
    printf("[2] submit OK token=%u len=%u\n", token, (unsigned)sizeof(cmd));

    /* ---- [3] reap with poll (up to 3s, DPC completion flag model) ---- */
    uint8_t out[408]; /* hdr(24) + 16 * r_t_rect(4) = 88 used, keep full size */
    uint32_t out_len = 0;
    int tries = 0;
    for (tries = 0; tries < 300; tries++) {
        rc = vgl_tport_reap(out, sizeof(out), &out_len);
        if (rc == VGL_E_PROTO) { /* empty - not completed yet */
            Sleep(10);
            continue;
        }
        break;
    }
    if (rc != VGL_OK) {
        printf("[FAIL] reap rc=%d (%s) after %d tries\n", rc, vgl_strerror(rc), tries);
        return 1;
    }
    if (out_len < 4) {
        printf("[FAIL] reap out_len=%u < 4\n", out_len);
        return 1;
    }
    uint32_t rtype = 0;
    memcpy(&rtype, out, 4); /* ctrl hdr type of the response */
    printf("[3] reap OK len=%u type=0x%08x (expect 0x%08x)\n",
           out_len, rtype, VIRTIO_GPU_RESP_OK_DISPLAY_INFO);
    if (rtype != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        printf("[FAIL] response type mismatch (host error response?)\n");
        return 1;
    }
    if (out_len >= 28) {
        /* first r_t_rect: x,y (int16) + width,height (uint16) */
        int16_t x, y;
        uint16_t w, h;
        memcpy(&x, out + 24, 2);
        memcpy(&y, out + 26, 2);
        memcpy(&w, out + 28, 2);
        memcpy(&h, out + 30, 2);
        printf("    scanout0: %dx%d at (%d,%d)\n", w, h, (int)x, (int)y);
    }

    vgl_tport_close();
    printf("=== L1 probe PASS ===\n");
    return 0;
}
