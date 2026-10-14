/*
 * vglctl.cpp - VGL video bridge WDM control device (M1a-v2).
 *
 * W8 ROOT CAUSE (do not reintroduce KMDF here!):
 *   A KMDOD initialises through DxgkInitializeDisplayOnlyDriver, which never
 *   binds the KMDF WdfFunctions_01015 table into this image.  Evidence:
 *   in-box KMDODs (qxldod.sys, BasicDisplay.sys) import no WDFLDR.SYS, and
 *   real KMDF drivers (monitor.sys, UsbHub3.sys) always do; wdfldr.lib is
 *   only an import library for WDFLDR.SYS.  Calling any Wdf* API from this
 *   driver jumps through a zeroed function table -> BSOD at device start
 *   (observed: boot failure after driver swap, 2026-10-14).
 *
 * M1a-v2 implementation (plain WDM control device object):
 *   - VglAttachToDriverObject (DriverEntry, after DxgkInitialize*):
 *     saves dxgkrnl's original IRP_MJ_CREATE/CLOSE/DEVICE_CONTROL entries
 *     and installs our dispatchers.  IRPs for any other device object are
 *     forwarded to the saved originals unchanged.
 *   - VglControlDeviceCreate (StartDevice, after HWInit): creates
 *     \Device\VGLVIDEO0 + \\DosDevices\VGLVIDEO0.  Failure is non-fatal:
 *     display keeps working, only the VGL bridge stays absent.
 *   - VglControlDeviceDestroy (StopDevice): deletes both.
 *   - GET_INFO fills vgl_tport_info_t from cached globals only (no shared
 *     pointer deref at dispatch time in M1a).  SUBMIT/REAP/WAIT/PIN/UNPIN
 *     return STATUS_NOT_IMPLEMENTED until M1b (VirtIOLib wiring).
 */

#include "driver.h"
#include "viogpudo.h"

#if !DBG
#include "vglctl.tmh"
#endif

namespace
{

PDRIVER_OBJECT g_VglDriverObject = NULL;
PDEVICE_OBJECT g_VglCDO = NULL;
volatile LONG g_VglMsixEnabled = 0;

DRIVER_DISPATCH *g_VglOrigCreate = NULL;
DRIVER_DISPATCH *g_VglOrigClose = NULL;
DRIVER_DISPATCH *g_VglOrigDeviceControl = NULL;

const WCHAR g_VglNtDeviceName[] = L"\\Device\\VGLVIDEO0";
const WCHAR g_VglDosDeviceName[] = L"\\DosDevices\\VGLVIDEO0";

NTSTATUS
VglDispatchCreate(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAGED_CODE();

    if (DeviceObject != g_VglCDO)
    {
        return g_VglOrigCreate(DeviceObject, Irp);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
VglDispatchClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAGED_CODE();

    if (DeviceObject != g_VglCDO)
    {
        return g_VglOrigClose(DeviceObject, Irp);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
VglDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

    if (DeviceObject != g_VglCDO)
    {
        return g_VglOrigDeviceControl(DeviceObject, Irp);
    }

    PIO_STACK_LOCATION ioStack = IoGetCurrentIrpStackLocation(Irp);
    ULONG ioControlCode = ioStack->Parameters.DeviceIoControl.IoControlCode;
    ULONG outBufferLength = ioStack->Parameters.DeviceIoControl.OutputBufferLength;

    Irp->IoStatus.Information = 0;

    switch (ioControlCode)
    {
    case IOCTL_VGL_GET_INFO:
    {
        if (outBufferLength < sizeof(vgl_tport_info_t))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        // METHOD_BUFFERED: SystemBuffer is already mapped/dual-purpose.
        vgl_tport_info_t *pInfo = (vgl_tport_info_t *)Irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(pInfo, sizeof(*pInfo));
        pInfo->magic = 'V' | ('G' << 8) | ('V' << 16) | ('1' << 24);
        pInfo->virtio_version = 1;
        pInfo->control_queue_depth = 64;
        pInfo->notify_cap = 0;
        pInfo->msix_enabled = (ULONG)g_VglMsixEnabled;
        Irp->IoStatus.Information = sizeof(*pInfo);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_VGL_SUBMIT:
    case IOCTL_VGL_REAP:
    case IOCTL_VGL_WAIT:
    case IOCTL_VGL_PIN:
    case IOCTL_VGL_UNPIN:
        // M1b (VirtIOLib + virtqueue) fills these in.
        status = STATUS_NOT_IMPLEMENTED;
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

} // anonymous namespace

VOID
VglAttachToDriverObject(_In_ PDRIVER_OBJECT pDriverObject)
{
    PAGED_CODE();

    g_VglDriverObject = pDriverObject;

    g_VglOrigCreate = pDriverObject->MajorFunction[IRP_MJ_CREATE];
    g_VglOrigClose = pDriverObject->MajorFunction[IRP_MJ_CLOSE];
    g_VglOrigDeviceControl = pDriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];

    pDriverObject->MajorFunction[IRP_MJ_CREATE] = VglDispatchCreate;
    pDriverObject->MajorFunction[IRP_MJ_CLOSE] = VglDispatchClose;
    pDriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = VglDispatchDeviceControl;

    DbgPrint(TRACE_LEVEL_INFORMATION, ("VGL dispatch hooks installed on driver object %p\n", pDriverObject));
}

NTSTATUS
VglControlDeviceCreate(_In_ BOOLEAN MsixEnabled)
{
    PAGED_CODE();

    // Stop/start cycles after the first one: refresh the cached
    // capabilities and keep the existing control device.
    if (g_VglCDO != NULL)
    {
        InterlockedExchange(&g_VglMsixEnabled, MsixEnabled ? 1 : 0);
        return STATUS_SUCCESS;
    }

    if (g_VglDriverObject == NULL)
    {
        return STATUS_UNSUCCESSFUL;
    }

    UNICODE_STRING ntName;
    RtlInitUnicodeString(&ntName, g_VglNtDeviceName);

    PDEVICE_OBJECT pDeviceObject = NULL;
    NTSTATUS status = IoCreateDevice(g_VglDriverObject,
                                     0, /* no device extension in M1a */
                                     &ntName,
                                     FILE_DEVICE_UNKNOWN,
                                     FILE_DEVICE_SECURE_OPEN,
                                     FALSE, /* not exclusive */
                                     &pDeviceObject);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("VGL IoCreateDevice(%ws) failed: 0x%X\n", g_VglNtDeviceName, status));
        return status;
    }

    pDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    UNICODE_STRING dosName;
    RtlInitUnicodeString(&dosName, g_VglDosDeviceName);
    status = IoCreateSymbolicLink(&dosName, &ntName);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("VGL IoCreateSymbolicLink(%ws) failed: 0x%X\n", g_VglDosDeviceName, status));
        IoDeleteDevice(pDeviceObject);
        return status;
    }

    InterlockedExchange(&g_VglMsixEnabled, MsixEnabled ? 1 : 0);
    g_VglCDO = pDeviceObject;

    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("VGL control device %ws + %ws created (msix=%u)\n", g_VglNtDeviceName, g_VglDosDeviceName, (ULONG)MsixEnabled));
    return STATUS_SUCCESS;
}

VOID
VglControlDeviceDestroy(VOID)
{
    PAGED_CODE();

    if (g_VglCDO == NULL)
    {
        return;
    }

    UNICODE_STRING dosName;
    RtlInitUnicodeString(&dosName, g_VglDosDeviceName);
    IoDeleteSymbolicLink(&dosName);

    IoDeleteDevice(g_VglCDO);
    g_VglCDO = NULL;

    DbgPrint(TRACE_LEVEL_INFORMATION, ("VGL control device destroyed\n"));
}
