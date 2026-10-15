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
 *   - GET_INFO serves cached globals; SUBMIT/REAP/WAIT implemented (M1b-p0);
 *     PIN/UNPIN remain STATUS_NOT_IMPLEMENTED (M1b-p1).
 *
 * IRQL notes: no PAGED_CODE() in the dispatchers; everything runs from
 * nonpaged init/static data.  METHOD_BUFFERED needs no DO_BUFFERED_IO flag
 * (set automatically for non-Direct stack locations by the I/O manager).
 */

#include "driver.h"
#include "viogpudo.h"

#include <ntstatus.h>

#if !DBG
#include "vglctl.tmh"
#endif

// IoCreateDriver: exported by ntoskrnl.lib since forever (verified present
// in this WDK's ntoskrnl.lib, plain C symbol), but the 10.0.28000 headers
// dropped the declaration.  Declare it locally as extern "C" (in a C++
// TU without it the name gets mangled -> LNK2019) - public ABI since XP.
extern "C" NTKERNELAPI
NTSTATUS
IoCreateDriver(
    _In_opt_ PUNICODE_STRING DriverName,
    _In_ PDRIVER_INITIALIZE InitializationFunction
    );

namespace
{

PDRIVER_OBJECT g_VglDriverObject = NULL;
volatile LONG g_VglMsixEnabled = 0;

// M1b-p0: cached CtrlQueue pointer injected by viogpudo after HWInit.
// NULL until VglControlDeviceCreate receives &m_pHWDevice->m_CtrlQueue.
CtrlQueue *g_pVglCtrlQueue = NULL;

// M1b-p0: token table for SUBMIT→REAP round-trip.  Polling mode (no MSI-X),
// so a single outstanding entry suffices for the smoke test.
KSPIN_LOCK g_VglTokenLock;
volatile LONG g_VglNextToken = 1;
ULONG g_VglOutstandingToken = 0;
PGPU_VBUFFER g_pVglOutstandingVbuf = NULL;   // vbuf ptr for the outstanding command

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
    {
        // Input buffer: [command_data (len bytes)][vgl_tport_submit_t]
        // vgl_tport_submit_t is at the TAIL; command_data precedes it.
        // p0.2 fix: pSubmit must be computed from inLen (tail), not the
        // buffer start - the start holds the command's first dword
        // (ctrl_hdr.type), so the old placement misread len and read the
        // command copy source out of bounds (pSubmit - cmdLen < SystemBuffer).
        ULONG inLen = ioStack->Parameters.DeviceIoControl.InputBufferLength;
        vgl_tport_submit_t *pSubmit = (vgl_tport_submit_t *)
            ((PUCHAR)Irp->AssociatedIrp.SystemBuffer + inLen - sizeof(vgl_tport_submit_t));

        if (!g_pVglCtrlQueue) {
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }
        if (inLen < sizeof(vgl_tport_submit_t)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        ULONG cmdLen = pSubmit->len;
        if (cmdLen == 0 || cmdLen > MAX_INLINE_CMD_SIZE) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (inLen < sizeof(vgl_tport_submit_t) + cmdLen) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        // Allocate response buffer (up to 512 bytes, well beyond any ctrl resp).
        SIZE_T respSz = 512;
        PVOID respBuf = ExAllocatePoolUninitialized(NonPagedPoolNx, respSz, 'gLVG');
        if (!respBuf) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        // AllocCmdResp: gets a vbuffer from the pool with cmd area + resp area.
        // auto_release=TRUE (set by GetBuf), so DPC will call ReleaseBuffer.
        // REAP must NOT call ReleaseBuffer — would double-free.
        PGPU_VBUFFER vbuf;
        PVOID cmdBuf = g_pVglCtrlQueue->AllocCmdResp(&vbuf, (int)cmdLen, respBuf, (int)respSz);
        if (!cmdBuf || !vbuf) {
            ExFreePool(respBuf);
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        // Copy user command data into the vbuffer's command area.
        // cmdBuf points to vbuf->buf; source is right before the header.
        RtlCopyMemory(cmdBuf, (PUCHAR)pSubmit - cmdLen, cmdLen);

        // Enqueue to virtqueue (acquires m_SpinLock internally, kicks).
        g_pVglCtrlQueue->QueueBuffer(vbuf);

        // Assign token for REAP to pick up.
        ULONG token = InterlockedIncrement(&g_VglNextToken);
        pSubmit->token = token;
        pSubmit->flags = 0;
        pSubmit->reserved = 0;

        // Store outstanding vbuf for REAP polling.
        KIRQL OldIrql;
        KeAcquireSpinLock(&g_VglTokenLock, &OldIrql);
        g_pVglOutstandingVbuf = vbuf;
        g_VglOutstandingToken = token;
        KeReleaseSpinLock(&g_VglTokenLock, OldIrql);

        // p0.2 fix #2: METHOD_BUFFERED copies back the FIRST Information
        // bytes of SystemBuffer, but pSubmit (holding the token) sits at the
        // buffer TAIL.  Copy the filled header to the front so userspace
        // (libvgl reads out[0..16]) actually gets the token - previously the
        // command header was copied back and the token never reached user mode.
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, pSubmit, sizeof(*pSubmit));
        Irp->IoStatus.Information = sizeof(vgl_tport_submit_t);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_VGL_REAP:
    {
        vgl_tport_reap_t *pReap = (vgl_tport_reap_t *)Irp->AssociatedIrp.SystemBuffer;
        ULONG inLen = ioStack->Parameters.DeviceIoControl.InputBufferLength;

        if (!g_pVglCtrlQueue) {
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }
        if (inLen < sizeof(vgl_tport_reap_t)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        ULONG token = pReap->token;
        ULONG cap = pReap->out_len;

        // W11 0xCE FIX: no kernel-side polling loop.  The old 1000x1ms loop
        // kept this IRP pending for up to 1s; killing the caller process tied
        // IRP_MJ_CLOSE to the unfinished IOCTL, and any driver image unload
        // in that window bugchecked 0xCE (observed 2026-10-15 18:1x).
        // REAP now checks the outstanding slot once and returns immediately;
        // user mode retries at its own pace.
        PGPU_VBUFFER vbuf = NULL;
        {
            KIRQL OldIrql;
            KeAcquireSpinLock(&g_VglTokenLock, &OldIrql);
            if (g_pVglOutstandingVbuf && g_VglOutstandingToken == token) {
                vbuf = g_pVglOutstandingVbuf;
                g_pVglOutstandingVbuf = NULL;
                g_VglOutstandingToken = 0;
            }
            KeReleaseSpinLock(&g_VglTokenLock, OldIrql);
        }

        if (vbuf) {
            ULONG copyLen = (ULONG)min((UINT)vbuf->resp_size, cap);
            PUCHAR outData = (PUCHAR)pReap + sizeof(vgl_tport_reap_t);

            // DPC processes command then calls ReleaseBuffer (auto_release=TRUE).
            // For dynamic responses (resp_size > MAX_INLINE_RESP_SIZE=24),
            // ReleaseBuffer sets resp_buf=NULL. We must read before that.
            BOOLEAN valid = FALSE;
            if (copyLen >= 4) {
                volatile ULONG type = *(volatile ULONG*)(vbuf->resp_buf);
                valid = (type != 0);
            }

            if (valid) {
                // Response is ready. Copy immediately.
                RtlCopyMemory(outData, vbuf->resp_buf, copyLen);
                pReap->has_data = 1;
                pReap->status = 0;
                pReap->out_len = copyLen;
                Irp->IoStatus.Information = sizeof(vgl_tport_reap_t) + copyLen;
            } else {
                // DPC hasn't processed yet, or already released
                // (resp_buf=NULL for dynamic responses).
                pReap->has_data = 0;
                pReap->status = 0;
                Irp->IoStatus.Information = sizeof(vgl_tport_reap_t);
            }
            // NOTE: Do NOT call ReleaseBuffer — DPC does that (auto_release=TRUE).
            // Double-free would occur if both DPC and REAP call ReleaseBuffer.
            status = STATUS_SUCCESS;
        } else {
            pReap->has_data = 0;
            pReap->status = 0;
            Irp->IoStatus.Information = sizeof(vgl_tport_reap_t);
            status = STATUS_SUCCESS;  // No data yet, not an error.
        }
        break;
    }

    case IOCTL_VGL_WAIT:
    {
        vgl_tport_wait_t *pWait = (vgl_tport_wait_t *)Irp->AssociatedIrp.SystemBuffer;
        ULONG inLen = ioStack->Parameters.DeviceIoControl.InputBufferLength;

        if (inLen < sizeof(vgl_tport_wait_t)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        // W11 0xCE FIX: clamp kernel sleep to 50ms.  Sleeping the full
        // caller-supplied timeout had the same pending-IRP window that
        // bugchecked 0xCE via the old REAP loop.
        ULONG timeoutMs = pWait->timeout_ms;
        if (timeoutMs > 50) {
            timeoutMs = 50;
        }
        LARGE_INTEGER interval;
        interval.QuadPart = -(LONGLONG)timeoutMs * 10000LL;  // ms -> 100ns units

        KeDelayExecutionThread(KernelMode, FALSE, &interval);

        // Polling mode: IRQ not used (notify_cap=0).  signaled=0.
        pWait->signaled = 0;

        Irp->IoStatus.Information = sizeof(vgl_tport_wait_t);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_VGL_PIN:
    case IOCTL_VGL_UNPIN:
        // M1b-p1 fills these (MmProbeAndLockPages + PFN table).
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

    KeInitializeSpinLock(&g_VglTokenLock);

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

// M1b-p0: receives theCtrlQueue* after HWInit.  Stores it for SUBMIT/REAP/WAIT.
NTSTATUS
VglControlDeviceCreate(_In_ BOOLEAN MsixEnabled, _In_opt_ CtrlQueue *pCtrlQueue)
{
    VglUpdateCaps(MsixEnabled);
    if (pCtrlQueue) {
        InterlockedExchangePointer((volatile PVOID *)&g_pVglCtrlQueue, pCtrlQueue);
    }
    return (g_VglDriverObject != NULL) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

// W11 0xCE FIX: called from VioGpuDod::StopDevice / ~VioGpuDod.  NULLs the
// cached CtrlQueue so SUBMIT/REAP return STATUS_INVALID_DEVICE_REQUEST
// instead of touching the hardware objects that ~VioGpuDod deletes right
// after this call (use-after-free guard).  The control device object itself
// stays alive; with REAP returning immediately no IRP stays pending, so the
// driver image unload path is clean.
VOID
VglControlDeviceDestroy(VOID)
{
    InterlockedExchangePointer((volatile PVOID *)&g_pVglCtrlQueue, NULL);
    DbgPrint(TRACE_LEVEL_INFORMATION, ("VGL control channel stopped (queue detached)\n"));
}
