# driver/ — viogpudo (merged vgvdo, L0)

KMDOD display-only driver (WDF) that also hosts the VGL video bridge channel.
The vgvdo standalone driver has been merged into viogpudo: a custom WDFQUEUE
(`m_VglQueue`) is attached to the display WDFDEVICE, exposing the six IOCTls
from `vgl/include/vgl/vgl_transport.h` (shared with libvgl).

## Architecture

- viogpudo is a KMDOD driver: `DxgkInitializeDisplayOnlyDriver` creates the
  WDFDEVICE, and the DXGK framework manages display lifecycle.
- The VGL channel reuses the **same** WDFDEVICE: `WdfIoQueueCreate` adds a
  sequential dispatch queue with `EvtIoDeviceControl = VglIoDeviceControl`.
- `VglIoDeviceControl` dispatches `IOCTL_VGL_GET_INFO` (fills transport info
  from VirtIODevice/PCI resources) and returns `STATUS_NOT_IMPLEMENTED` for
  SUBMIT/REAP/WAIT/PIN/UNPIN (M1b wires VirtIOLib + virtqueue).
- The `VIOGPU_VGL_QUEUE_CONTEXT` (pDod) carries the VioGpuDod pointer so the
  IOCTL handler can access PCI resources (MSI-X status) and future VirtIODevice
  state.

## Status

M1a skeleton: bridge device present, datapath (VirtIOLib + virtqueue) lands in M1b.
