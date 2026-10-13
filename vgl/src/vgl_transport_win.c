/*
 * vgl_transport_win.c - Win32 client for the vgvdo kernel bridge.
 *
 * Plain CreateFile/DeviceIoControl; no WDK dependency so this compiles in
 * CI (windows runner) without any driver kit installed.
 */
#include "vgl/vgl.h"
#include "vgl/vgl_transport.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

typedef struct vgl_transport {
    HANDLE dev;
    vgl_tport_info_t info;
} vgl_transport_t;

static vgl_transport_t g_tport;

int vgl_tport_open(void)
{
    if (g_tport.dev != INVALID_HANDLE_VALUE)
        return VGL_OK;

    HANDLE h = CreateFileW(VGL_TRANSPORT_DEVICE,
                           GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return VGL_E_NO_DEVICE;

    g_tport.dev = h;
    memset(&g_tport.info, 0, sizeof(g_tport.info));

    DWORD br = 0;
    if (!DeviceIoControl(h, IOCTL_VGL_GET_INFO, NULL, 0,
                         &g_tport.info, sizeof(g_tport.info), &br, NULL) ||
        br < sizeof(g_tport.info)) {
        CloseHandle(h);
        g_tport.dev = INVALID_HANDLE_VALUE;
        return VGL_E_DRIVER;
    }
    return VGL_OK;
}

void vgl_tport_close(void)
{
    if (g_tport.dev != INVALID_HANDLE_VALUE) {
        CloseHandle(g_tport.dev);
        g_tport.dev = INVALID_HANDLE_VALUE;
    }
}

int vgl_tport_ready(void)
{
    return g_tport.dev != INVALID_HANDLE_VALUE;
}

const vgl_tport_info_t *vgl_tport_info(void)
{
    return g_tport.dev != INVALID_HANDLE_VALUE ? &g_tport.info : NULL;
}

/* Submit a fully serialized command buffer; bridge kicks the notify. */
int vgl_tport_submit(const void *cmd, uint32_t len, uint32_t *token)
{
    if (g_tport.dev == INVALID_HANDLE_VALUE)
        return VGL_E_NO_DEVICE;

    uint8_t *buf = (uint8_t *)malloc(len + sizeof(vgl_tport_submit_t));
    if (!buf)
        return VGL_E_NOMEM;

    memcpy(buf, cmd, len);
    vgl_tport_submit_t s;
    memset(&s, 0, sizeof(s));
    s.len = len;
    memcpy(buf + len, &s, sizeof(s));

    DWORD br = 0;
    vgl_tport_submit_t out;
    int rc = VGL_OK;
    if (DeviceIoControl(g_tport.dev, IOCTL_VGL_SUBMIT,
                         buf, len + sizeof(vgl_tport_submit_t),
                         &out, sizeof(out), &br, NULL) && br == sizeof(out)) {
        *token = out.token;
    } else {
        rc = VGL_E_DRIVER;
    }
    free(buf);
    return rc;
}

/* Non-blocking response pickup; *has_data==0 on empty queue. */
int vgl_tport_reap(void *out, uint32_t cap, uint32_t *out_len)
{
    if (g_tport.dev == INVALID_HANDLE_VALUE)
        return VGL_E_NO_DEVICE;

    vgl_tport_reap_t r;
    memset(&r, 0, sizeof(r));
    r.out_len = cap;

    DWORD br = 0;
    uint8_t *buf = (uint8_t *)malloc(sizeof(vgl_tport_reap_t) + cap);
    if (!buf)
        return VGL_E_NOMEM;
    memcpy(buf, &r, sizeof(r));

    int rc = VGL_OK;
    if (DeviceIoControl(g_tport.dev, IOCTL_VGL_REAP,
                        buf, sizeof(vgl_tport_reap_t),
                        buf, sizeof(vgl_tport_reap_t) + cap, &br, NULL)) {
        memcpy(&r, buf, sizeof(r));
        if (r.has_data) {
            memcpy(out, buf + sizeof(vgl_tport_reap_t), r.out_len);
            *out_len = r.out_len;
        } else {
            rc = VGL_E_PROTO;    /* empty - poll again */
        }
    } else {
        rc = VGL_E_DRIVER;
    }
    free(buf);
    return rc;
}

int vgl_tport_wait(uint32_t timeout_ms, uint32_t *signaled)
{
    if (g_tport.dev == INVALID_HANDLE_VALUE)
        return VGL_E_NO_DEVICE;

    vgl_tport_wait_t w;
    w.timeout_ms = timeout_ms;
    w.signaled = 0;

    DWORD br = 0;
    if (!DeviceIoControl(g_tport.dev, IOCTL_VGL_WAIT,
                         &w, sizeof(w), &w, sizeof(w), &br, NULL))
        return VGL_E_DRIVER;
    *signaled = w.signaled;
    return VGL_OK;
}

int vgl_tport_pin(const void *va, uint32_t len, uint64_t *handle,
                  uint64_t *pfns, uint32_t max_pfns, uint32_t *pfn_count,
                  uint32_t *first_off)
{
    if (g_tport.dev == INVALID_HANDLE_VALUE)
        return VGL_E_NO_DEVICE;

    size_t in = sizeof(vgl_tport_pin_t) + ((size_t)max_pfns - 1) * sizeof(uint64_t);
    vgl_tport_pin_t *p = (vgl_tport_pin_t *)malloc(in);
    if (!p)
        return VGL_E_NOMEM;
    memset(p, 0, in);
    p->user_va = (uint64_t)(uintptr_t)va;
    p->length = len;
    p->max_pfns = max_pfns;

    DWORD br = 0;
    int rc = VGL_OK;
    if (DeviceIoControl(g_tport.dev, IOCTL_VGL_PIN,
                        p, (DWORD)in, p, (DWORD)in, &br, NULL)) {
        *handle = p->handle;
        *pfn_count = p->pfn_count;
        *first_off = p->first_pfn_offset;
        memcpy(pfns, p->pfns, (size_t)p->pfn_count * sizeof(uint64_t));
    } else {
        rc = VGL_E_DRIVER;
    }
    free(p);
    return rc;
}

int vgl_tport_unpin(uint64_t handle)
{
    if (g_tport.dev == INVALID_HANDLE_VALUE)
        return VGL_E_NO_DEVICE;
    vgl_tport_unpin_t u;
    u.handle = handle;
    DWORD br = 0;
    if (!DeviceIoControl(g_tport.dev, IOCTL_VGL_UNPIN,
                        &u, sizeof(u), NULL, 0, &br, NULL))
        return VGL_E_DRIVER;
    return VGL_OK;
}
