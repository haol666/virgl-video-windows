/*
 * vgvdo.c - VGL video bridge driver (KMDF, layer L0) - M1a skeleton.
 *
 * Scope (design doc 2.1): "pipe only, no protocol".  The whole virtio-gpu
 * control-queue plumbing (VirtIOLib PCI transport + virtqueue, kick/notify,
 * MSI-X, MDL pin) lands in M1b.  M1a delivers the contract surface:
 *
 *   - named device \\Device\\VGLVIDEO0 + symlink \\DosDevices\\VGLVIDEO0
 *     + device interface, so libvgl's CreateFile("\\\\.\\.VGLVIDEO0") path
 *     and the install surface (INF/devcon) can be validated on a real VM.
 *   - EvtIoDeviceControl dispatching the six IOCTls from the shared
 *     contract header vgl/include/vgl/vgl_transport.h.
 *       GET_INFO  -> returns vgl_tport_info_t (skeleton: depth 0 = bridge
 *                    queue not wired yet; L1 treats it as transport-absent)
 *       SUBMIT/REAP/WAIT/PIN/UNPIN -> STATUS_NOT_IMPLEMENTED until M1b
 *   - nothing else: no resource tracking, no command semantics (L1 owns
 *     those), no viogpudo interference (binds only the second, non-display
 *     virtio-gpu instance per the dual-device XML).
 */
#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>          /* make DEFINE_GUID emit an instantiated GUID */

#include "vgl/vgl_transport.h"   /* shared L0/L1 contract (six IOCTls) */

#define VGV_TAG 'ogvV'

/* Device interface GUID exposed alongside the classic symlink.
 * {1C6A4B27-2C05-4F7E-998D-2FA56B4D1077} */
DEFINE_GUID(GUID_DEVINTERFACE_VGLVIDEO,
    0x1c6a4b27, 0x2c05, 0x4f7e, 0x99, 0x8d,
    0x2f, 0xa5, 0x6b, 0x4d, 0x10, 0x77);

#define VGV_INFO_MAGIC  ('V' | ('G' << 8) | ('V' << 16) | ('1' << 24))

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;

#if DBG
#define VGV_TRACE(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, \
               "vgvdo: " fmt "\n", __VA_ARGS__)
#else
#define VGV_TRACE(fmt, ...) ((void)0)
#endif

static NTSTATUS FillInfo(_Out_ vgl_tport_info_t *info)
{
    RtlZeroMemory(info, sizeof(*info));
    info->magic = VGV_INFO_MAGIC;
    /* M1a skeleton: no virtqueue yet.  control_queue_depth == 0 is the
     * documented "bridge present but datapath absent" state; M1b starts
     * reporting the real 64-deep control queue. */
    info->virtio_version    = 0;
    info->control_queue_depth = 0;
    info->notify_cap        = 0;
    info->msix_enabled      = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject,
                     _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);

    status = WdfDriverCreate(DriverObject, RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vgvdo: DriverEntry WdfDriverCreate fail 0x%08X\n", status);
        return status;
    }

    VGV_TRACE("DriverEntry ok (M1a skeleton, datapath lands in M1b)");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS EvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    DECLARE_CONST_UNICODE_STRING(ntName, L"\\Device\\VGLVIDEO0");
    DECLARE_CONST_UNICODE_STRING(dosName, L"\\DosDevices\\VGLVIDEO0");
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, TRUE);

    /* Named device + DOS symlink so user mode can open \\\\.\\VGLVIDEO0
     * (VGL_TRANSPORT_DEVICE in the shared header). */
    status = WdfDeviceInitAssignName(DeviceInit, &ntName);
    if (!NT_SUCCESS(status)) {
        VGV_TRACE("AssignName fail 0x%08X", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        VGV_TRACE("WdfDeviceCreate fail 0x%08X", status);
        return status;
    }

    status = WdfDeviceCreateSymbolicLink(device, &dosName);
    if (!NT_SUCCESS(status)) {
        VGV_TRACE("SymbolicLink fail 0x%08X", status);
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device,
                                            &GUID_DEVINTERFACE_VGLVIDEO, NULL);
    if (!NT_SUCCESS(status)) {
        VGV_TRACE("DeviceInterface fail 0x%08X", status);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                           WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        VGV_TRACE("QueueCreate fail 0x%08X", status);
        return status;
    }

    VGV_TRACE("device ready: \\DosDevices\\VGLVIDEO0 (queue depth 0, M1a)");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID EvtIoDeviceControl(_In_ WDFQUEUE Queue,
                        _In_ WDFREQUEST Request,
                        _In_ size_t OutputBufferLength,
                        _In_ size_t InputBufferLength,
                        _In_ ULONG IoControlCode)
{
    NTSTATUS status;
    void *buffer;
    size_t length;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {

    case IOCTL_VGL_GET_INFO: {
        if (OutputBufferLength < sizeof(vgl_tport_info_t)) {
            WdfRequestCompleteWithInformation(Request, STATUS_BUFFER_TOO_SMALL,
                                              sizeof(vgl_tport_info_t));
            return;
        }
        status = WdfRequestRetrieveOutputBuffer(Request,
                                                sizeof(vgl_tport_info_t),
                                                &buffer, &length);
        if (!NT_SUCCESS(status)) {
            break;
        }
        status = FillInfo((vgl_tport_info_t *)buffer);
        if (NT_SUCCESS(status)) {
            WdfRequestCompleteWithInformation(Request, status,
                                              sizeof(vgl_tport_info_t));
            return;
        }
        break;
    }

    case IOCTL_VGL_SUBMIT:
    case IOCTL_VGL_REAP:
    case IOCTL_VGL_WAIT:
    case IOCTL_VGL_PIN:
    case IOCTL_VGL_UNPIN:
        /* M1b (VirtIOLib + virtqueue) fills these in. */
        WdfRequestCompleteWithInformation(Request, STATUS_NOT_IMPLEMENTED, 0);
        return;

    default:
        WdfRequestCompleteWithInformation(Request,
                                          STATUS_INVALID_DEVICE_REQUEST, 0);
        return;
    }

    WdfRequestCompleteWithInformation(Request, status, 0);
}
