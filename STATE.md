# STATE.md — Project State

Last verified: **2026-09-03** (matrix PASS=27 FAIL=0 incl. High10 + High422 + GBM display surfaces). Host: Orange Pi 5 NAS `192.168.1.21`, Armbian 26.8.3 resolute, kernel **7.1.8-edge-rockchip64**.

Living ops notes: [HANDOFF.md](HANDOFF.md). Desktop apps: [APPS.md](APPS.md). Codec table: [README.md](README.md).

Git: `https://github.com/pty819/vaapi-v4l2-bridge.git` (`master`).

## What actually works

C libva backend `v4l2stateless_drv_video.so` translates VA-API to mainline V4L2-stateless (plus stateful JPEG encode and RGA VPP). Nodes are chosen by OUTPUT fourcc, not a hardcoded `/dev/videoN`.

Installed: `/usr/lib/aarch64-linux-gnu/dri/v4l2stateless_drv_video.so`. Graphical sessions export `LIBVA_DRIVER_NAME=v4l2stateless` via `~/.config/environment.d/90-libva.conf` and `~/.profile`.

Host matrix `tests/run_full_matrix.sh` last recorded **PASS=27 FAIL=0** (h26410 + h264422 + gbm-probe + va-export entries). Success is `hwdownload` framemd5 vs software (MPEG-2 vs GStreamer `v4l2slmpeg2dec`), plus a log line `v4l2stateless: .* config uses /dev/video`. Silent ffmpeg software fallback is not success.

| Path | Device | Status |
|---|---|---|
| H.264 CB / Main / High | rkvdec `/dev/video1` | bit-exact vs ffmpeg SW (B / all-P / slices / 4K / QCIF) |
| H.264 High10 | same, capture NV15 | bit-exact vs ffmpeg SW — strip ffmpeg QpBdOffsetY from pic_init_qp/qs (VA carries depth offset, V4L2 wants raw) |
| H.264 High422 | same, capture NV16/NV20 | bit-exact 8+10-bit: VA path via tests/va_h264422_client.c (stock ffmpeg never offers VAAPI for 4:2:2 — decoder pix_fmt list + profile map), kernel path via GST vs GST |
| HEVC Main 8-bit (WPP, 4K) | rkvdec `/dev/video1` | bit-exact vs ffmpeg SW |
| HEVC Main10 | same, NV15 → P010 | bit-exact vs ffmpeg SW (`hwdownload,format=p010le`) |
| AV1 Profile0 8-bit | hantro `/dev/video4` + sysfs media node | bit-exact vs ffmpeg SW (libaom, libaom realtime, SVT-AV1 RA, 4K) |
| VP8 | hantro `/dev/video2` | bit-exact vs ffmpeg SW |
| MPEG-2 Simple / Main | same `/dev/video2` | bit-exact vs GST `v4l2slmpeg2dec` (hantro IDCT ≠ ffmpeg SW) |
| JPEG Baseline encode | VEPU121 `/dev/video3` | `mjpeg_vaapi` (stateful M2M) |
| VPP | RGA `/dev/video0` | `scale_vaapi` |

Chrome `FillProfileInfo_Locked` attrib query is implemented (`vaQueryConfigAttributes` treats `*num_attribs` as output-only; `max_attributes=32`). Official Linux arm64 Chrome still needs the wrapper in `scripts/google-chrome-vaapi`. Firefox uses FFmpeg VA-API + `scripts/firefox-vaapi-user.js`. VLC uses `avcodec-hw=vaapi`.

## Still not done / do not claim

- **VP9** — no VP9 OUTPUT fourcc on this mainline hantro node
- **Forced browser HW on VP9 / 10-bit HDR** — has hung the VPU; keep `media.hardware-video-decoding.force-enabled=false`
- **Browser mid-stream resolution changes** — capture renegotiate exists in the driver; Chrome/Firefox path is not matrix-tested
- **Vendor BSP / MPP** — out of scope (mainline only)

## 2026-09-03 GBM display surfaces — Chrome hardware decode WITH picture (f53f3f1..452de04)

Chrome `VaapiVideoDecoder` (GL backend, zero-copy) now hardware-decodes AND
shows the picture on live.bilibili.com 1080p (verified: `VaapiVideoDecoder`
in media-internals, canvas drawImage avg ~74/255 non-black, zero
eglCreateImage errors, GPU process holds rkvdec).

How it works (`src/v4l2stateless_gbm.c`):

- Every exportable surface gets a driver-owned **linear GBM bo** on
  `/dev/dri/renderD128` (panthor shmem — ordinary system memory; the VPU/CMA
  buffers are still never exported, the EXPBUF chip-bug ban is untouched).
- `vaExportSurfaceHandle` returns the classic **single-object NV12 dmabuf**
  descriptor (Y@0, UV@stride*h) that Chromium requires
  (`num_objects != 1` is rejected upstream, TODO crbug.com/974438).
- **Chrome exports VPP OUTPUT surfaces, not decode surfaces** (VaapiVideoDecoder
  blits decode->output via VAProc, then exports). Surfaces carry a
  last-writer marker (`gbm_src`); pull_capture / VPP / vaPutImage all keep
  the bo in sync from cpu_ptr (image layout) or memfd (capture layout).
- Platform constraints baked into the design: panthor refuses multiplanar
  YUV gbm bos (R8 geometry + byte offsets instead); Mesa 26.0.8 per-plane
  GR88 import samples black (never emit GR88-only images); render-node
  override via `V4L2SL_RENDER_NODE`.
- 7907909's clean-fail gate was dead code (`cpu_ptr` is calloc'd for every
  surface at creation, so `!cpu_ptr` never held) — fixed by the new gate
  (`buf_index >= 0` || VLD context || `format == NV12`); caught by
  `tests/va_export_client.c` (exports each frame and verifies the dma-buf
  read back through real EGL == vaGetImage bytes, Y and UV).
- Debug tools that isolated the Chrome-side failure:
  `tests/wl_import_probe.c` (import matrix on Wayland/GBM/surfaceless
  displays), `tests/ioctl_interpose.c` (LD_PRELOAD PRIME-fd tracer).
- Chrome wrapper re-enabled: `--disable-features=Vulkan` only
  (AcceleratedVideoDecoder back on).

Out of scope (falls back to software, unchanged): 10-bit (NV15) would need
an R16-bo P010 layout; Y210 unreachable (GR88 broken).

## 2026-09-02 stability fixes (commit 27e8b7a)

Static review + kernel-UAPI verification round (dev-stateless-decoder spec,
rkvdec driver source, libva threading contract). Host matrix re-run after
the fixes: **PASS=20 FAIL=0, zero resets/timeouts/degradations in the log**
(all hw framemd5 still bit-exact vs software; MPEG-2 vs GStreamer). Matrix
now expects no AV1 while it stays un-advertised. Changes:

- Decode submit unified in `v4l2sl_decode_submit`: timeout / DQBUF /
  request-queue failure now STREAMOFFs both queues and rebuilds the free
  pools (`v4l2sl_decode_reset`). A wedged job is never left in the kernel.
- Full driver lock: every stateful vtable entry takes `g_v4l2sl_lock`
  (libva's threading model makes backend thread safety mandatory).
- Surface table: IDs recycled via a free stack, all lookups bounds-checked
  (fixes OOB past `surfaces[4096]` in long-lived processes).
- Free-pool pushes bounded + de-duplicated (fixes potential OOB write
  past `free_cap_bufs[24]` on error paths).
- Probe results cached per boot (`$XDG_RUNTIME_DIR/v4l2stateless-probe.cache`,
  keyed by boot_id; `V4L2SL_PROBE_NOCACHE=1` bypasses): vaInitialize no
  longer opens all 64 video nodes / fires REQUEST_ALLOC in every process.
- Renegotiate rebuilds the OUTPUT queue too (S_FMT is EBUSY while buffers
  are allocated); capture REQBUFS degrades 24→8→4 under CMA pressure;
  `vaCreateContext` fails cleanly instead of leaving a zombie fd.
- HEVC PPS uniform-spacing flag no longer wiped by the flags reset.
- VPP/JPEG error paths always STREAMOFF + REQBUFS(0).

## Layout

| Path | Role |
|---|---|
| NAS git | `/home/liyifan/vaapi-v4l2-bridge/` — **only source of truth** |
| Mac work copy | deleted 2026-09-02 (stale dma-heap experiment; that approach has a chip bug) |
| Install | `/usr/lib/aarch64-linux-gnu/dri/v4l2stateless_drv_video.so` |
