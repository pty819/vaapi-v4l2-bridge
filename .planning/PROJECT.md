# VA-API to V4L2 Stateless Bridge Driver

## What This Is

A libva backend driver (`v4l2stateless_drv_video.so`) that bridges VA-API to the Linux V4L2 Request API, enabling applications that only support VA-API (Firefox, VLC, mpv) to use V4L2 stateless hardware decoders on ARM SoCs.

**Primary target:** Rockchip RK3588S (Orange Pi 5) with Mali G610 GPU.

## Why

- Firefox, VLC, mpv only support VA-API for hardware video decoding on Linux
- RK3588S has excellent V4L2 stateless hardware decoders (rkvdec, hantro) but no VA-API driver
- No modern VA-API → V4L2 stateless bridge exists (bootlin/libva-v4l2-request is 7+ years old, Allwinner-only)
- This creates a gap: V4L2 stateless works perfectly (GStreamer, Clapper) but mainstream apps can't use it

## Architecture

```
Application (Firefox/VLC/mpv)
    ↓ VA-API (libva)
v4l2stateless_drv_video.so (our driver)
    ↓ V4L2 Request API (ioctl)
    ↓ MEDIA_IOC_REQUEST_ALLOC
    ↓ VIDIOC_S_EXT_CTRLS (stateless controls)
Kernel: rkvdec (H.264/HEVC) / hantro (AV1)
    ↓ DMA-BUF output
Application (render)
```

## Target Hardware

| Device | Driver | Codecs | Kernel |
|--------|--------|--------|--------|
| /dev/video1 | rkvdec (VDPU381) | H.264, HEVC | 7.0+ |
| /dev/video2 | hantro G1 | VP8, MPEG-2 | 7.0+ |
| /dev/video4 | hantro AV1 | AV1 | 7.1+ |

## Key Constraints

- **Kernel:** Linux 7.1+ mainline (V4L2 stateless API)
- **libva:** 2.20.0 (installed at /usr/lib/aarch64-linux-gnu/)
- **Language:** C (matching libva driver conventions)
- **Build:** Meson + Ninja
- **Reference:** bootlin/libva-v4l2-request (outdated but structurally useful)
- **V4L2 headers:** /usr/include/linux/v4l2-controls.h, videodev2.h, media.h
- **VA-API headers:** /usr/include/va/va_backend.h, va_dec_*.h

## Requirements

### Validated

(None yet — ship to validate)

### Active

- [ ] R1: libva driver skeleton that loads and reports H.264/HEVC/AV1 profiles
- [ ] R2: H.264 decode pipeline (VA-API params → V4L2 stateless controls → decode → DMA-BUF export)
- [ ] R3: HEVC decode pipeline (including RPS controls for VDPU381)
- [ ] R4: AV1 decode pipeline
- [ ] R5: Integration test with Firefox, VLC, mpv (vainfo → actual video playback)
- [ ] R6: Meson build system with proper install targets

### Out of Scope

- VP9 — rkvdec on RK3588S doesn't expose VP9 format
- Encoding — only decode for now
- ChromeOS-specific paths — targeting standard Linux desktop
- Multi-instance concurrent decode — single context first

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Pure C (no C++) | libva drivers are C, bootlin reference is C | — Pending |
| Meson build | Matches GStreamer/libva conventions | — Pending |
| DMA-BUF for output | Zero-copy frame passing to application | — Pending |
| Per-codec source files | va_v4l2_h264.c, va_v4l2_hevc.c, va_v4l2_av1.c | — Pending |
| Use bootlin as skeleton | Driver init/teardown structure is reusable | — Pending |

## Evolution

This document evolves at phase transitions and milestone boundaries.

---
*Last updated: 2026-06-19 after initialization*
