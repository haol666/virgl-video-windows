# reference/ — authoritative protocol sources (G1 evidence, 2026-10-12)

Offline copies of the guest-side (Mesa) implementation of the virtio-gpu
video protocol. These are the blueprint the Windows L0/L1 layers are ported
from, byte-for-byte where it matters.

**Source**: gitee.com/openkylin/mesa, branch `upstream`
(the openKylin fork that carries the Kylin virtio-gpu video patches; the
same code line was merged into Mesa upstream — the CachyOS guest runs it as
the standard 26.2.4 package, per host WORKLOG F19/§13/§17).

| File | Upstream path | Why we keep it |
|---|---|---|
| `virgl_video.c` | src/gallium/drivers/virgl/ | guest session lifecycle: codec/buffer creation, per-frame desc/feed ping-pong, flush/fence, feedback read |
| `virgl_video.h` | src/gallium/drivers/virgl/ | codec/buffer object structs |
| `virgl_encode.c` | src/gallium/drivers/virgl/ | command emission — the exact dword layouts (see 1776-1865) |
| `virgl_encode.h` | src/gallium/drivers/virgl/ | encoder API declarations |
| `virgl_protocol.h` | src/virtio/virtio-gpu/ | VIRGL_CCMD_* ids + payload index defines |
| `virgl_video_hw.h` | src/virtio/virtio-gpu/ | wire structs — **identical to the host handover copy** (fc diff: whitespace only) |
| `virgl_hw.h` | src/virtio/virtio-gpu/ | enum virgl_formats (NV12=166), caps v2 incl. struct virgl_video_caps |
| `p_video_enums.h` | src/gallium/include/pipe/ | pipe_video_profile numbering (== guest 26.2.4 == host r9 table) |
| `p_video_state.h` | src/gallium/include/pipe/ | picture_type / rate_control_method enums (explicit values) |
| `p_defines.h` | src/gallium/include/pipe/ | context (chroma format enum lives elsewhere; values documented in vgl_wire.h) |

Host-side counterpart (FROZEN, separate repo): virgl-video-project
`virglrenderer/src/virgl_video.c` + `virgl_video_hw.h`, production baseline
v12.5 (16f8e79 / loaded 5ee5a73a).

## Cross-validation done at G1 (three independent sources)

1. host `virgl_video.c` `guest_to_host_profile()` / `guest_profile_value()`
2. host WORKLOG §17 (Mesa 26.2.4 numbering dump)
3. this `p_video_enums.h` (openKylin fork) — all agree:

H264: BASELINE=7, CONSTRAINTED=8, MAIN=9, EXTENDED=10, HIGH=11;
HEVC: MAIN=15, MAIN_10=16; entrypoint ENCODE=4; chroma 420=2.

## License

Mesa 3-D graphics library, MIT license (copyright headers preserved in each
file). Retrieved 2026-10-12 via gitee raw URLs (freedesktop.org blocked by
Anubis from this network; raw gitee is reachable).
