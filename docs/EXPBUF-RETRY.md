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


## Decoded-frame export (same day, later)

Worktree driver gated by `V4L2SL_EXPBUF_EXPORT=1` (default off). Loaded via
`LIBVA_DRIVERS_PATH` — **system dri .so unchanged**.

`va_export_client` on `h264_idr_nv12.h264` (8 all-IDR 1280x720 frames):

- Without the env: `EXPORT_EXACT 0..7` + `LAZY_EXACT 0..7` (GBM copy, as shipping).
- With the env: log `EXPBUF export ok idx=23..16 fd=… stride=1280 alh=720` then
  the same `EXPORT_EXACT` / `LAZY_EXACT` 0..7. EGL sampling of the **VPU**
  dma-buf matched `vaGetImage` byte-for-byte. Host stayed up.

`gbm_bo_import` of NV12 fourcc is still EINVAL; the client samples via
EGL R8-at-offset (same as the existing export client), not GBM NV12 import.

Still untested: Chrome ANGLE/Wayland, HEVC/AV1 export, 4K, buffers that
remain in the kernel DPB while a later frame decodes.


## Phase 1 — skip capture→GBM memcpy (2026-09-04)

`pull_capture` returns after setting `has_pic`/`buf_index` when
`V4L2SL_EXPBUF_EXPORT=1` (no `gbm_surface_upload`, no memfd memcpy).
GetImage/Derive read `capture_buf_ptr[buf_index]`.

`va_export_client` h264_idr_nv12 8 frames:

- env unset: EXPORT/LAZY_EXACT 0-7 (GBM copy)
- env set: 8× `EXPBUF export ok`, 0× falling back, 0× gbm upload,
  EXPORT/LAZY_EXACT 0-7. Host alive.


## Phase 2 — DPB-live hold (2026-09-04)

`tests/va_expbuf_hold.c`: decode frame 0 onto surface 0, EXPBUF + keep
dma-buf fd and an EGLImage, then decode 7 later H.264 pictures onto
surfaces 1/2/3 cycling (never re-BeginPicture on surface 0). Re-import
the held fd after those pictures.

`V4L2SL_EXPBUF_EXPORT=1` `h264_baseline.mp4` annex-B (`/tmp/base.h264`):
`HOLD_EXACT`, 7 later P-frames, host alive (uptime ~5:07).


## Phase 3 — HEVC/AV1 GetImage under EXPBUF-no-memcpy (2026-09-04)

ffmpeg hwdownload vs software framemd5, `V4L2SL_EXPBUF_EXPORT=1`,
worktree `.so` via `LIBVA_DRIVERS_PATH`, 0 gbm upload, 0 fallback:

- `h264_baseline.mp4` 16 frames MATCH
- `hevc_main.mp4` 16 frames MATCH
- `av1_aom.mp4` 8 frames MATCH

ffmpeg (and Chrome) decode onto surfaces that are **not** listed as
context `render_targets`. GetImage therefore cannot look up the owning
context that way. `pull_capture` now stores `surf->cap_view` (the capture
mmap) and GetImage/Derive/Export read that pointer. Host alive (~5:16).


## Phase 4 — Chrome zero-copy (2026-09-04)

Worktree `.so` via `LIBVA_DRIVERS_PATH` + `--gpu-launcher`. Chrome GPU
process **drops** `V4L2SL_*` env; the experiment switch that Chrome can
see is `$XDG_RUNTIME_DIR/v4l2sl-expbuf` (or `V4L2SL_EXPBUF_EXPORT=1` for
ffmpeg / lab clients).

Chrome exports the VA surface pool **before** the first decode
(`buf_index=-1`). EXPBUF mode now claims a capture slot at first export
and `decode_submit` re-queues that same index. `begin_picture` does not
recycle the slot while EXPBUF is on.

Local `http://127.0.0.1:8931/h264_gate.mp4`:
- GPU maps worktree `.so` (not dri)
- 22× `EXPBUF export ok`, 0 fallback, 0 gbm upload
- `kVideoDecoderName=VaapiVideoDecoder`, chrome://gpu clean
- canvas avg=123 / max=255, 1920×1080
- host alive

Bilibili live `https://live.bilibili.com/22957791` ~75s:
- 23× `EXPBUF export ok`, 0 fallback
- VaapiVideoDecoder throughout
- canvas avg 63–140, 1920×1080, currentTime 20→78, paused=false
- host alive (~5:45). dri `.so` still 2026-09-04 17:13 (GBM shipping).

**Not shipped.** Do not `sudo cp` dri or merge master until asked.


## Phase 5 — shipping default (2026-09-04)

Chrome gate passed (local 1080p + bilibili live). EXPBUF is now the
**default** path: `v4l2sl_expbuf_export_wanted()` returns 1 unless
`V4L2SL_EXPBUF_EXPORT=0`. Per-frame `/tmp/v4l2sl-expbuf.log` is gone;
claim/export traces only with `V4L2SL_DEBUG=1`.

System dri `.so` is the EXPBUF build after this merge. GBM copy remains
as fallback when EXPBUF ioctl fails or the env opts out.
