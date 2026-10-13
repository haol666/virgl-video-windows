# ffmpeg-h264-vgl

FFmpeg encoder source (`vglenc.c`) providing `h264_vgl`: H.264 encoding
delegated to the KVM host hardware (Unraid / AMD VCN) through the
virtio-gpu video protocol, consumed by hwcodec/RustDesk unchanged.

## Integration (M2)

1. Clone `rustdesk-org/externals` (the FFmpeg build project used by
   hwcodec) into your fork and pin the same FFmpeg version branch hwcodec
   ships (ABI must match the bundled `avcodec-*.dll` names).
2. Copy `vglenc.c` into `ffmpeg/libavcodec/`.
3. Register the encoder:
   - `libavcodec/Makefile`: `OBJS-$(CONFIG_H264_VGL_ENCODER) += vglenc.o`
   - `libavcodec/allcodecs.c`: `extern FFCodec ff_h264_vgl_encoder;` and
     `REGISTER_ENCODER(H264_VGL, h264_vgl);`
   - `configure`: add `h264_vgl_encoder` to the encoder list.
4. Add libvgl to the build: `pkg-config --static --cflags --libs vgl`
   (libvgl.a + headers from this repo, same commit).
5. Produce the `avcodec/avutil/...` DLLs via the externals build script
   and drop them into the RustDesk installation directory (overwrite the
   stock ones with the matching ABI version).

## Verification

```
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 \
       -c:v h264_vgl -b:v 4M -y out.mp4
ffprobe -v error -show_streams out.mp4
```

- M2 acceptance: host QEMU log shows `DBG B1 SPS` + `seg` lines for the
  session, ffprobe parses the stream (profile_idc=100, in-band SPS/PPS).
- The encoder advertises NO extradata; bitstream is Annex-B with
  host-synthesized parameter sets (handover §3.4-2), same shape the
  Linux VAAPI path already delivers.

## Notes

- Input is NV12 system memory only - no D3D11/DXVA device is created
  (hwcodec only enters its hwdevice branch for names containing "vaapi").
- `zerolatency` defaults to on (bframes=0, slice output) for remote
  control use.
