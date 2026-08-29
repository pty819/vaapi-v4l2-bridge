# VA-API to V4L2 Stateless Bridge Driver

## What this is

A libva backend (`v4l2stateless_drv_video.so`) that maps VA-API to the Linux V4L2 Request API (stateless decode) plus stateful JPEG encode and RGA VPP. Apps that only speak VA-API (ffmpeg, VLC, Firefox, official Linux Chrome) can use mainline rkvdec/hantro on RK3588.

**Primary target:** Rockchip RK3588 (Orange Pi 5), Mali-G610 / panthor, **mainline** kernel (no vendor BSP, no MPP).

## Why

- Firefox / VLC / official Chrome / ffmpeg-vaapi do not talk V4L2-stateless themselves
- RK3588 has working mainline V4L2-stateless nodes; there is no upstream VA-API driver for them
- bootlin/libva-v4l2-request is 2019, Allwinner-only
- Distro Chromium arm64 often compiles `use_v4l2_codec` and **bypasses** this `.so`; official Chrome compiles `use_vaapi` and uses it

## Architecture

```
Application (ffmpeg / Firefox / VLC / official Chrome)
    ↓ VA-API (libva)
v4l2stateless_drv_video.so
    ↓ V4L2 Request API (decode) or stateful M2M (JPEG / RGA)
Kernel: rkvdec (H.264/HEVC) / hantro (AV1, VP8, MPEG-2, JPEG) / RGA (VPP)
```

## Devices on this RK3588 (probed by fourcc, numbers can move)

| Node | Driver | Role |
|------|--------|------|
| `/dev/video1` | rkvdec | H.264, HEVC |
| `/dev/video2` | hantro G1 | VP8, MPEG-2 |
| `/dev/video3` | hantro VEPU121 | JPEG encode |
| `/dev/video4` | hantro AV1 | AV1 (+ matching `/dev/media*`) |
| `/dev/video0` | RGA | VPP |
| `/dev/dri/renderD128` | panthor | libva DRM display |

## Constraints

- Kernel 7.1+ mainline V4L2-stateless
- C, Meson + Ninja, LGPL-2.1-or-later (same as libva)
- Success = `hwdownload` framemd5, never silent software fallback

## Requirements

### Done

- R1: libva driver loads; vainfo reports the profiles we advertise
- R2: H.264 decode pipeline
- R3: HEVC decode pipeline (VDPU381)
- R4: AV1 decode pipeline (refresh flags inferred)
- R5: Firefox / VLC / official Chrome can load the `.so` (see APPS.md)
- R6: Meson install of `v4l2stateless_drv_video.so`

### Out of scope

- VP9 (this board's mainline hantro has no VP9 fourcc)
- Vendor MPP / BSP kernels
- ChromeOS-only builds
- Guaranteed multi-instance 4K HDR in browsers

## Evolution

Updated 2026-08-29 to match shipped code. Detail lives in [HANDOFF.md](../HANDOFF.md) and [README.md](../README.md).
