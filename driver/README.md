# driver/ — vgvdo (virtio-gpu video bridge, L0)

KMDF (WDF) kernel driver, "pipe only, no protocol" (design doc §2).
Binds the headless virtio-gpu PCI instance (second device, XML in
`xml/virtio-gpu-dual.xml`) and exposes `\\.\VGLVIDEO0`.

## Status

M1 target. Scaffolding directory until the wire gate (G1: authoritative
`virgl_video_hw.h`) clears, then implementation starts here.

## Implementation plan

- Reuse virtio-win `VirtIOLib` (virtio PCI transport + virtqueue) as
  NetKVM/ivshmem do; target ~2-3k lines total.
- Six IOCTOs only - the full contract lives in
  `../vgl/include/vgl/vgl_transport.h` (single source of truth shared
  with libvgl; copy into driver tree at M1 kick-off).
- Control queue initialized by the driver (depth 64), virtqueue submit
  (descriptors chain) + reap + MSI-X wait event.
- `IOCTL_VGL_PIN` locks user memory via MDL and returns guest PFNs for
  ATTACH_BACKING; `IOCTL_VGL_UNPIN` releases (design doc §2.3).
- Test-signed for self-hosted use (bcdedit /set testsigning on).
  WHQL is out of scope this phase (design doc §11).

## Out of scope

- Any video semantics (L1 owns those; keep this driver dumb).
- Supporting the display-bound virtio-gpu instance (viogpudo owns it).
