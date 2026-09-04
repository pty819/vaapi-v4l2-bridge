# EXPBUF retry after PSU swap (2026-09-04)

Isolated worktree: `.worktrees/expbuf-retry` on `experiment/expbuf-retry`.
**Does not change the shipping driver.** Shipping path still copies
CAPTURE → GBM and never `VIDIOC_EXPBUF`s VPU buffers.

## Why retry

The original ban was “EXPBUF + GPU import hangs the RK3588 CMA/IOMMU”.
That hang coincided with an undersized PSU (later replaced). This run
re-tests the *import* path only, with `timeout -k 2 15..25` around every
stage so a hang is distinguishable from a clean fail.

## Ladder (`tests/expbuf_ladder.c`)

Always: CAPTURE `S_FMT` + `REQBUFS(MMAP)` + `VIDIOC_EXPBUF`.

| Stage | Extra | rkvdec `/dev/video1` | hantro AV1 `/dev/video4` |
|---|---|---|---|
| 1 | close fd | PASS | (not required) |
| 2 | CPU `mmap` 64 B | PASS | |
| 3 | `DRM_IOCTL_PRIME_FD_TO_HANDLE` + `GEM_CLOSE` | PASS | |
| 4 | `gbm_bo_import` | PASS (NV12 EINVAL, **R8 OK**) | |
| 5 | `gbm_bo_map` 16×16 | PASS | |
| 6 | `eglCreateImageKHR` NV12, **one fd two planes** | **PASS** (`egl=0x3000`) | **PASS** (64×64 default fmt) |

Also PASS: **1280×720** rkvdec, `sizeimage=1843200`, with **CAPTURE STREAMON**
then EXPBUF then full ladder through EGL.

Host stayed up (`uptime` 4h+ after the sequence). Empty buffers (xor=0)
— this is *import liveness*, not pixel correctness.

## What this does **not** prove

- A **decoded** frame’s capture buffer (DPB still referenced) imported
  while the VPU is running.
- Chrome `VaapiVideoDecoder` using that fd as a `NativePixmap` (ANGLE
  Wayland, not this GBM EGL display).
- Bit-exact sampling vs `vaGetImage` (the shipping `va_export_client`
  tests the **GBM copy** path).
- Multi-frame / 4K / 10-bit NV15.

`gbm_bo_import` of fourcc **NV12** is still `EINVAL` on panthor; only the
R8-tall-buffer lie imports, same as the shipping display path. EGL NV12
single-fd image **did** create (`0x3000`).

## Verdict

On this board **after the PSU swap**, EXPBUF of an idle (and STREAMON)
rkvdec/hantro capture buffer + panthor PRIME + GBM R8 import + EGL NV12
image **did not reboot the SoC** in this ladder.

That weakens “any EXPBUF is an instant chip bug”. It does **not** yet
justify flipping the driver to export VPU buffers: DPB lifetime, Chrome
ANGLE, and bit-exact sampling are untested. Next experiment, if wanted:
decode one H.264 IDR, EXPBUF *that* index, EGL sample vs GetImage, still
under `timeout`, still not in the shipping `.so`.
