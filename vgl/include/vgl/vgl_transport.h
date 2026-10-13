/*
 * vgl_transport.h - kernel bridge (vgvdo) user-mode transport contract.
 *
 * Layer 0/L1 boundary.  The bridge driver (driver/, KMDF) exposes exactly
 * the six IOCTOs below - "pipe only, no protocol" (design doc §2).
 * This header is shared verbatim between the driver and libvgl.
 */
#ifndef VGL_TRANSPORT_H
#define VGL_TRANSPORT_H

#ifdef _NTDDK_
/* Kernel build (vgvdo): wdm.h (included by the driver before this header)
 * provides UINT8/16/32/64.  MSVC 14.5x <stdint.h> drags in vcruntime.h
 * which is not kernel-clean (C4083/C4005 on the VS2026 toolchain), so the
 * fixed-width types are defined locally instead. */
typedef UINT8  uint8_t;
typedef UINT16 uint16_t;
typedef UINT32 uint32_t;
typedef UINT64 uint64_t;
#else
#include <stdint.h>
#include <stddef.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Device interface (symlink) created by vgvdo. */
#define VGL_TRANSPORT_DEVICE  L"\\\\.\\VGLVIDEO0"

/* CTL codes: FILE_DEVICE_UNKNOWN, METHOD_BUFFERED except PIN which needs
 * direct access to user pages via MDL internally (buffered in/out for
 * the PFN table itself). */
#define VGL_IOCTL_BASE  0x8000

#define IOCTL_VGL_GET_INFO  CTL_CODE(FILE_DEVICE_UNKNOWN, VGL_IOCTL_BASE + 0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VGL_SUBMIT    CTL_CODE(FILE_DEVICE_UNKNOWN, VGL_IOCTL_BASE + 1, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VGL_REAP      CTL_CODE(FILE_DEVICE_UNKNOWN, VGL_IOCTL_BASE + 2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VGL_WAIT     CTL_CODE(FILE_DEVICE_UNKNOWN, VGL_IOCTL_BASE + 3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VGL_PIN      CTL_CODE(FILE_DEVICE_UNKNOWN, VGL_IOCTL_BASE + 4, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VGL_UNPIN    CTL_CODE(FILE_DEVICE_UNKNOWN, VGL_IOCTL_BASE + 5, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ------------------------------------------------------------------ */

typedef struct vgl_tport_info {
    uint32_t magic;                 /* 'V','G','V','1' */
    uint32_t virtio_version;
    uint32_t control_queue_depth;   /* driver initializes 64 */
    uint32_t notify_cap;            /* 1 = separate notify BAR region */
    uint32_t msix_enabled;
    uint32_t reserved[3];
} vgl_tport_info_t;

typedef struct vgl_tport_submit {
    uint32_t len;                   /* command buffer bytes (in) */
    uint32_t flags;                 /* reserved */
    uint32_t token;                 /* out: completion token for REAP */
    uint32_t reserved;
} vgl_tport_submit_t;

typedef struct vgl_tport_reap {
    uint32_t token;                 /* in: any token, returns one response */
    uint32_t out_len;               /* in: capacity, out: actual bytes */
    uint32_t has_data;              /* out: 0 = queue empty (retry later) */
    uint32_t status;                /* out: 0 ok */
} vgl_tport_reap_t;

typedef struct vgl_tport_wait {
    uint32_t timeout_ms;            /* in */
    uint32_t signaled;              /* out: 1 = IRQ event fired, 0 = timeout */
} vgl_tport_wait_t;

typedef struct vgl_tport_pin {
    uint64_t user_va;               /* in: base of buffer to pin (page aligned) */
    uint32_t length;                /* in: bytes */
    uint32_t max_pfns;              /* in: capacity of pfn array */
    uint64_t handle;                /* out: pin handle for UNPIN */
    uint32_t pfn_count;             /* out */
    uint32_t first_pfn_offset;      /* out: offset of user_va in first page */
    uint64_t pfns[1];               /* out, variable length: guest PFNs */
} vgl_tport_pin_t;

typedef struct vgl_tport_unpin {
    uint64_t handle;                /* in */
} vgl_tport_unpin_t;

#ifdef __cplusplus
}
#endif

#endif /* VGL_TRANSPORT_H */
