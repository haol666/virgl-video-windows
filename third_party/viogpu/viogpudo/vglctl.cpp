/*
 * vglctl.cpp - VGL video bridge WDM control device (M1a-v3).
 *
 * W8 ROOT CAUSE (KMDOD vs KMDF): a KMDOD initialises through
 * DxgkInitializeDisplayOnlyDriver, which never binds the KMDF
 * WdfFunctions_01015 table; any Wdf* call jumps through a zeroed
 * function table -> BSOD at device start (observed 2026-10-14).
 *
 * W9 ROOT CAUSE (dispatch-table hook deadlock): M1a-v2 hooked
 * IRP_MJ_CREATE/CLOSE/DEVICE_CONTROL on the *dxgkrnl-owned* DRIVER_OBJECT
 * and forwarded non-CDO IRPs to the saved originals.  dxgkrnl manages this
 * dispatch table at runtime and drives its own IRP traffic through it on
 * the display path; the extra hop with PAGED_CODE() entries deadlocked the
 * machine hard (BugcheckCode=0 hang, no dump; two occurrences 2026-10-15
 * 00:17 and 00:30, each within seconds of the first CreateFile on
 * \\.\VGLVIDEO0).
 *
 * M1a-v3 (isolated DRIVER_OBJECT):
 *   DriverEntry (after DxgkInitializeDisplayOnlyDriver succeeds) calls
 *   VglInit(), which does IoCreateDriver(NULL, VglDriverEntry).  The
 *   kernel builds a *separate* DRIVER_OBJECT owning only our control
 *   device.  dxgkrnl's dispatch table is never touched: no hooks, no
 *   forwarding, no race.  Display path is unaffected even if every VGL
 *   call deadlocks (worst case the caller hangs, machine survives).
 *
 *   - VglDriverEntry (INIT): fills the fresh object's MajorFunction table,
 *     creates \Device\VGLVIDEO0 + \DosDevices\VGLVIDEO0 right away.
 *   - VglUpdateCaps / VglControlDeviceCreate(msix): StartDevice refreshes
 *     cached caps; the device exists for the whole driver lifetime, which
 *     also removes every create/destroy race across PnP restarts.
 *   - GET_INFO serves cached globals; SUBMIT/REAP/WAIT/PIN/UNPIN return
 *     STATUS_NOT_IMPLEMENTED until M1b (VirtIOLib wiring).
 *
 * IRQL notes: no PAGED_CODE() in the dispatchers; everything runs from
 * nonpaged init/static data.  METHOD_BUFFERED needs no DO_BUFFERED_IO flag
 * (set automatically for non-Direct stack locations by the I/O manager).
 */

#include "driver.h"
#include "viogpudo.h"

#if !DBG
#include "vglctl.tmh"
#endif

// IoCreateDriver: exported by ntoskrnl.lib since forever (verified present
// in this WDK's ntoskrnl.lib), but the 10.0.28000 headers dropped the
// declaration.  Declare it locally (public ABI, stable since XP).
NTKERNELAPI
NTSTATUS
IoCreateDriver(
    _In_opt_ PUNICODE_STRING DriverName,
    _In_ PDRIVER_INITIALIZE InitializationFunction
    );

namespace
{

PDRIVER_OBJECT g_VglDriverObject = NULL;
volatile LONG g_VglMsixEnabled = 0;

const WCHAR g_VglNtDeviceName[] = L"\\Device\\VGLVIDEO0";
const WCHAR g_VglDosDeviceName[] = L"\\DosDevices\\VGLVIDEO0";

NTSTATUS
VglDispatchCreate(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
VglDispatchClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
VglDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

    UNREFERENCED_PARAMETER(DeviceObject);

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
        vgl_tport_info_t *pInfo = (vgl_tport_info_t *)Irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(pInfo, sizeof(*pInfo));
        pInfo->magic = 'V' | ('G' << 8) | ('V' << 16) | ('1' << 24);
        pInfo->virtio_version = 1;
        pInfo->control_queue_depth = 64;
        pInfo->notify_cap = 0;
        pInfo->msix_enabled = (ULONG)InterlockedCompareExchange(&g_VglMsixEnabled, 0, 0);
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

NTSTATUS
VglDispatchUnsupported(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS
VglCleanup(_In_ PDRIVER_OBJECT DriverObject)
{
    // DriverUnload equivalent for the IoCreateDriver object (never unloaded
    // in practice; the display driver owns the image's lifetime).
    UNICODE_STRING dosName;
    RtlInitUnicodeString(&dosName, g_VglDosDeviceName);
    IoDeleteSymbolicLink(&dosName);

    PDEVICE_OBJECT dev = DriverObject->DeviceObject;
    while (dev != NULL)
    {
        PDEVICE_OBJECT next = dev->NextDevice;
        IoDeleteDevice(dev);
        dev = next;
    }
    return STATUS_SUCCESS;
}

} // anonymous namespace

// Entry point for the isolated VGL driver object (INIT section).
#pragma code_seg(push)
#pragma code_seg("INIT")
NTSTATUS
VglDriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DbgPrint(TRACE_LEVEL_INFORMATION, ("VGL isolated driver object %p\n", DriverObject));

    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
    {
        DriverObject->MajorFunction[i] = VglDispatchUnsupported;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE] = VglDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = VglDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = VglDispatchDeviceControl;
    DriverObject->DriverUnload = NULL; /* image lifetime = display driver */

    UNICODE_STRING ntName;
    RtlInitUnicodeString(&ntName, g_VglNtDeviceName);

    PDEVICE_OBJECT deviceObject = NULL;
    NTSTATUS status = IoCreateDevice(DriverObject,
                                     0, /* no extension: data lives in globals */
                                     &ntName,
                                     FILE_DEVICE_UNKNOWN,
                                     FILE_DEVICE_SECURE_OPEN,
                                     FALSE, /* not exclusive */
                                     &deviceObject);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("VGL IoCreateDevice failed: 0x%X\n", status));
        return status;
    }

    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    UNICODE_STRING dosName;
    RtlInitUnicodeString(&dosName, g_VglDosDeviceName);
    status = IoCreateSymbolicLink(&dosName, &ntName);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("VGL IoCreateSymbolicLink failed: 0x%X\n", status));
        IoDeleteDevice(deviceObject);
        return status;
    }

    g_VglDriverObject = DriverObject;
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("VGL control device ready: %ws + %ws\n", g_VglNtDeviceName, g_VglDosDeviceName));
    return STATUS_SUCCESS;
}
#pragma code_seg(pop)

// Called from the display driver's DriverEntry after DxgkInitialize* succeeds.
NTSTATUS
VglInit(VOID)
{
    if (g_VglDriverObject != NULL)
    {
        return STATUS_SUCCESS; /* already up (defensive) */
    }
    NTSTATUS status = IoCreateDriver(NULL, VglDriverEntry);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("VGL IoCreateDriver failed: 0x%X (display continues)\n", status));
    }
    return status;
}

// Refresh cached capabilities from StartDevice (hardware-known values only).
VOID
VglUpdateCaps(_In_ BOOLEAN MsixEnabled)
{
    InterlockedExchange(&g_VglMsixEnabled, MsixEnabled ? 1 : 0);
}

// Compatibility shim: previous M1a-v2 API name kept so viogpudo.cpp diff
// stays minimal.  The device is created once at VglInit; this only updates
// caps.
NTSTATUS
VglControlDeviceCreate(_In_ BOOLEAN MsixEnabled)
{
    VglUpdateCaps(MsixEnabled);
    return (g_VglDriverObject != NULL) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

// Compatibility shim: no teardown on display Stop (device is static for the
// image lifetime).  Registered as a no-op to keep VglCleanup symmetric.
VOID
VglControlDeviceDestroy(VOID)
{
    /* nothing - see VglCleanup */
}
