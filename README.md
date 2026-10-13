# virgl-video-windows

Windows guest H.264 hardware encoding passthrough for KVM (Unraid host,
AMD VCN) via the **virtio-gpu video protocol / virgl channel**.  Target
consumer: **RustDesk 被控端** — gets host-side hardware encoding with
**zero source changes** (Cargo patch only), same trick that made the
Linux guest work: install the capability at the ecosystem's standard
position (FFmpeg).

```
RustDesk (unchanged)
  └─ hwcodec fork (vgl-support, 11-line diff: add "h264_vgl" candidate)
       └─ FFmpeg h264_vgl (this repo: ffmpeg-h264-vgl/vglenc.c)
            └─ libvgl (this repo: vgl/) - session/fence/wire (PROVISIONAL)
                 └─ vgvdo bridge driver (this repo: driver/, M1) - pipe only
                      └─ virtio-gpu (2nd, headless instance) → QEMU
                           └─ virglrenderer patch (FROZEN v12.3h-r8) → VCN
```

## Documents

| Doc | Content |
|---|---|
| [`docs/WINDOWS_IMPL_DESIGN.md`](docs/WINDOWS_IMPL_DESIGN.md) | Implementation design (the one to read first) |
| [`docs/WORKLOG.md`](docs/WORKLOG.md) | Work log (W-series, mirrors host F-series discipline) |
| `../WINDOWS_GUEST_DESIGN.md` (parent project) | Host-side handover contract — host is FROZEN |

## Layout

| Dir | What |
|---|---|
| `vgl/` | libvgl: wire structs, resource pool, session, Win32 transport (CI-compilable today) |
| `ffmpeg-h264-vgl/` | FFmpeg encoder source + integration recipe (M2) |
| `hwcodec-patch/` | The 11-line hwcodec diff (fork branch `vgl-support`) |
| `driver/` | vgvdo KMDF bridge (M1) |
| `xml/` | Dual virtio-gpu libvirt fragment |
| `.github/workflows/` | CI: build-vgl (now), build-hwcodec (now), build-ffmpeg (M2) |

## Status

- [x] Design frozen for review, wire hard-values pinned by unit tests
- [x] libvgl skeleton compiles + tests green in CI (no device needed)
- [ ] **G1 gate**: authoritative `virgl_video_hw.h` + `virgl_video.c`
  (from virgl-video-project) - M1 hard prerequisite
- [ ] M1: vgvdo driver + first `create_codec` in host QEMU log
- [ ] M2: full FFmpeg build, ffprobe acceptance
- [ ] M3: RustDesk negotiates h264_vgl (HWRAM stays)
- [ ] M4: end-to-end remote control acceptance

## Local build

```
cmake -S vgl -B build -A x64
cmake --build build --config Release
ctest --timeout 60 -C Release --output-on-failure
```

## Related

- Fork: `haol666/hwcodec` branch `vgl-support`
- Fork: `haol666/externals` (FFmpeg build recipes, M2)
- Upstream contract: Unraid virgl-video-project (tag v12.3h-r8, frozen)

Host-side issues: file against virgl-video-project with QEMU log
attached. Never run vainfo/ffmpeg directly on the host (handover §2 red
line) - diagnostics go through the QEMU log window.
