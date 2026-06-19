# ROADMAP — vaapi-v4l2-bridge

## Milestone 1: Working VA-API → V4L2 Stateless Bridge

### Phase 1: Driver Skeleton + Init ✦
**Goal:** libva driver loads, vainfo reports supported profiles

**Deliverables:**
- `src/v4l2stateless.c` — driver entry point (`VA_DRIVER_INIT_FUNC`)
- `src/v4l2stateless_context.c` — context/surface management
- `src/v4l2stateless_device.c` — V4L2 device discovery + open
- `meson.build` — build system
- Driver installs to `/usr/lib/aarch64-linux-gnu/dri/v4l2stateless_drv_video.so`
- `vainfo` shows H.264, HEVC, AV1 profiles and entrypoints

**Verification:** `vainfo --display drm` lists profiles without errors

---

### Phase 2: H.264 Decode Pipeline ✦
**Goal:** Decode H.264 video via V4L2 stateless request API

**Deliverables:**
- `src/v4l2stateless_h264.c` — VA-API H.264 buffer → V4L2 H.264 stateless controls
- Parameter translation: VAPictureParameterBufferH264 → `V4L2_CID_STATELESS_H264_SPS/PPS/DECODE_PARAMS/SCALING_MATRIX/SLICE_PARAMS`
- `V4L2_CID_STATELESS_H264_DECODE_MODE=Frame-Based`
- DMA-BUF output via `VIDIOC_EXPBUF`
- `src/v4l2stateless_buffer.c` — V4L2 buffer queue management (request API)

**Verification:** `ffmpeg -hwaccel drm -i test_h264.mp4 -f null -` or sample decode program

---

### Phase 3: HEVC Decode Pipeline ✦
**Goal:** Decode HEVC video, including VDPU381-specific RPS controls

**Deliverables:**
- `src/v4l2stateless_hevc.c` — VA-API HEVC buffer → V4L2 HEVC stateless controls
- Parameter translation: VAPictureParameterBufferHEVC → `V4L2_CID_STATELESS_HEVC_SPS/PPS/SCALING_MATRIX/DECODE_PARAMS/SLICE_PARAMS`
- RK3588-specific: `V4L2_CID_STATELESS_HEVC_DECODE_PARAMS` with explicit RPS (long-term + short-term ref pic sets)
- HEVC entrypoint `VAEntrypointVLD` with `VAProfileHEVCMain`

**Verification:** Decode HEVC 4K test file via VA-API path

---

### Phase 4: AV1 Decode Pipeline ✦
**Goal:** Decode AV1 video via hantro AV1 decoder

**Deliverables:**
- `src/v4l2stateless_av1.c` — VA-API AV1 buffer → V4L2 AV1 stateless controls
- Parameter translation: VAPictureParameterBufferAV1 → `V4L2_CID_STATELESS_AV1_SEQUENCE/FRAME/TILE_GROUP_ENTRY/FILM_GRAIN`
- AV1 film grain synthesis support

**Verification:** Decode AV1 test file via VA-API path

---

### Phase 5: Integration Testing ✦
**Goal:** Real applications use our driver for hardware decode

**Deliverables:**
- Test script: `tests/test_integration.sh`
- Verify: `vainfo` (all profiles), `mpv --hwdec=vaapi`, Firefox `about:config` media.hardware-video-decoding
- Handle edge cases: resolution changes, seek, flush
- `vaSyncSurface` correctness (proper frame timing)

**Verification:** Play H.264/HEVC/AV1 video in Firefox with `MOZ_LOG=PlatformDecoderModule:5` showing VA-API decode

---

### Phase 6: Build + Packaging ✦
**Goal:** Clean build system, installable package, documentation

**Deliverables:**
- `meson.build` with proper options and install targets
- `README.md` with build instructions, usage, supported codecs
- `LICENSE` (LGPL-2.1, matching libva)
- Optional: `.deb` packaging
- Optional: udev rules for `/dev/video-dec*` symlinks

**Verification:** `meson setup builddir && ninja -C builddir && ninja -C builddir install`

---

## Future Milestones

- VP9 support (if rkvdec firmware update enables it)
- Encoding (H.264/HEVC V4L2 stateless encoder → VA-API)
- Multi-stream concurrent decode
- Upstream submission to libva-drivers or standalone project
