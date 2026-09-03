# STATE.md — Project State

Last verified: **2026-09-03** (matrix PASS=27 FAIL=0 incl. High10 + High422 + GBM display surfaces). Host: Orange Pi 5 NAS `192.168.1.21`, Armbian 26.8.3 resolute, kernel **7.1.8-edge-rockchip64**.

Living ops notes: [HANDOFF.md](HANDOFF.md). Desktop apps: [APPS.md](APPS.md). Codec table: [README.md](README.md).

Git: `https://github.com/pty819/vaapi-v4l2-bridge.git` (`master`).

## What actually works

C libva backend `v4l2stateless_drv_video.so` translates VA-API to mainline V4L2-stateless (plus stateful JPEG encode and RGA VPP). Nodes are chosen by OUTPUT fourcc, not a hardcoded `/dev/videoN`.

Installed: `/usr/lib/aarch64-linux-gnu/dri/v4l2stateless_drv_video.so`. Graphical sessions export `LIBVA_DRIVER_NAME=v4l2stateless` via `~/.config/environment.d/90-libva.conf` and `~/.profile`.

Host matrix `tests/run_full_matrix.sh` last recorded **PASS=32 FAIL=0** (h26410 + h264422 + gbm-probe + va-export + 5 AV1 legs). Success is `hwdownload` framemd5 vs software (MPEG-2 vs GStreamer `v4l2slmpeg2dec`), plus a log line `v4l2stateless: .* config uses /dev/video`. Silent ffmpeg software fallback is not success.

| Path | Device | Status |
|---|---|---|
| H.264 CB / Main / High | rkvdec `/dev/video1` | bit-exact vs ffmpeg SW (B / all-P / slices / 4K / QCIF) |
| H.264 High10 | same, capture NV15 | bit-exact vs ffmpeg SW — strip ffmpeg QpBdOffsetY from pic_init_qp/qs (VA carries depth offset, V4L2 wants raw) |
| H.264 High422 | same, capture NV16/NV20 | bit-exact 8+10-bit: VA path via tests/va_h264422_client.c (stock ffmpeg never offers VAAPI for 4:2:2 — decoder pix_fmt list + profile map), kernel path via GST vs GST |
| HEVC Main 8-bit (WPP, 4K) | rkvdec `/dev/video1` | bit-exact vs ffmpeg SW |
| HEVC Main10 | same, NV15 → P010 | bit-exact vs ffmpeg SW (`hwdownload,format=p010le`) |
| AV1 Profile0 8-bit | hantro `/dev/video4` + sysfs media node | bit-exact vs ffmpeg SW (libaom, libaom realtime, SVT-AV1 RA, 4K); **re-advertised 2026-09-03** (old hang = undersized PSU) and Chrome-verified (local file + YouTube) |
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

- **Lazy memfd (2026-09-03)**: bo-backed surfaces skip the per-frame
  CMA!92memfd copy (the bo is the snapshot); vaGetImage / vaDeriveImage /
  VPP source refill the memfd from the bo on demand
  (`v4l2sl_surface_ensure_memfd`). Chrome's per-frame copy count: 2 !92 1
  (the EXPBUF-ban floor). Guarded by the client's LAZY_EXACT second pass.

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

## 2026-09-03 refactor branch (audit-driven P0-P4)
Branch `refactor/audit-2026-09` (docs/refactor-audit-2026-09-03.md + spec):
P0 safety net + dead-code sweep (-~270 src lines) done; P2 memory-safety
done (create_surfaces fail path, terminate teardown 11MB->0 leak, VPP clamp,
grow_memfd grow-only, lock coverage, context device_path, capture-slot
recycle, EINTR poll, skipped-frame DQBUF, 256 slice arrays, image stride,
putImage fourcc, HEVC IQ, MPEG-2 chroma fallback, AV1 flags; C12 not
actionable - libva header lacks update_grain). Gates: matrix 27/27,
ASan va_leak_client 0 leaks, chrome smoke via scripts/chrome_smoke.sh
(live room OR local 8931 clip fallback). P3/P4 pending.

P3 partial (clusters 1-6 done: last_writer enum, memfd_fd rename, shared
xioctl, surface_by_id everywhere, context_for_surface, 4096 attr unify;
7-11 deferred). P4 items 1-5 done with measured -5..6 ioctls/frame
(docs/perf-baseline-2026-09.md); items 6-8 deferred. Merged to master
2026-09-03; follow-ups live in the audit + P3/P4 plan files.

## 2026-09-03 late — deferred tail closed (branch refactor/tail-2026-09-03)

All eight deferred items landed, gates green after each (matrix 27/27,
chrome smoke, units):

- P4-6 persistent per-surface memfd mapping (mremap grow; derived images
  borrow it, a grow while borrowed parks a retired mapping instead of
  moving pages under live clients). Readback clients drop 2 mmap/munmap
  per frame.
- P4-7 persistent VPP/JPEG M2M queues — steady state is STREAMON + QBUF +
  DQBUF + STREAMOFF only (~18 -> ~7 ioctls, mmaps -> 0 per frame); any
  failure or setup-key change tears down and renegotiates. VEPU quirk
  kept: the OUTPUT plane count must come from the QUERYBUF writeback
  (the queue reports 3 planes for NV12M-out/JPEG-cap, not G_FMT's 2).
  Also fixed the uninitialized oreq/creq REQBUFS(0) early-exit path and
  gated the per-frame VPP/JPEG success prints behind V4L2SL_DEBUG (the
  matrix JPEG/VPP legs export it).
- P4-8 64-byte aligned default image stride.
- P3-7 one Annex-B concat helper (h264 prefix 3, hevc/mpeg2 4).
- P3-8 v4l2sl_collect_decode_buffers shared by all five codec translates.
- P3-9 v4l2sl_fill_prime_layers shared by the memfd and GBM export paths.
- P3-10 AV1 refresh heuristics in a ctx->av1 sub-struct; the av1
  recapture test now builds on recap_env (the two-surface test keeps its
  own env by design).
- P3-11 probe scanners share the scan_nodes() walk (unified accept
  criteria nout>0 || ncap>0); V4L2SL_REQAPI_UNKNOWN stays — the picker
  tests use it as the dont-care marker.

Close-out strace: decode path still ~7 ioctl/frame (2 S_EXT_CTRLS,
2 QBUF, 2 DQBUF, QUEUE + REINIT; 0 QUERYBUF / REQUEST_ALLOC) —
docs/perf-baseline-2026-09.md.

C12 re-check: update_grain is absent from VAFilmGrainStructAV1 in BOTH
the installed libva-dev 2.23.0 (latest apt candidate) and upstream libva
master — still not actionable, now double-verified.

Browser 10-bit (investigation, no config change): 8-bit HEVC 1080p over
http DECODES THROUGH THE BRIDGE in Chrome (bridge-side session verified:
media for /dev/video1, 24 capture buffers at 1920x1088) — a new
supported fact beyond H.264. Main10 is rejected by Chrome's demuxer
(DEMUXER_ERROR_NO_SUPPORTED_STREAMS) before any decoder runs; no
user-facing flag exists and Chrome has no HEVC software fallback by
design, so on this SDR panel there is nothing to enable. Matrix-level
Main10/High10 correctness stays ffmpeg-verified. YouTube untouched.

Ops note: the smoke debug instance now uses ~/.config/gc-smoke with
--no-first-run --no-default-browser-check (the user session runs gc-dbg
WITHOUT --remote-debugging-port, so a gc-dbg smoke launch would forward
into the portless main session and ECONNREFUSE on 9222).
## 2026-09-03/04 night — AV1 re-advertised + Chrome AV1 end-to-end (feat/av1-readvertise)

Re-enabled `VAProfileAV1Profile0` (profile table, rt_formats, codec_map; dropped the
`dev_av1` suppression). The old "reopening the node hangs the SoC" theory is retired:
the box had an undersized PSU back then; the replacement supply went through 15x vainfo,
5 back-to-back decodes and two full matrices with zero runtime dmesg errors. Matrix went
PASS=27 -> 32 (av1-aom-8, av1-aom-49, av1-svt-32, av1-4k, av1-default-16 all bit-exact).

Two Chrome-only incompatibilities surfaced (ffmpeg path never hit them) and were fixed:

1. **OUTPUT payload framing**: this frame-based UAPI wants RAW tile data. ffmpeg's AV1
   hwaccel submits exactly that; Chrome submits the whole OBU span (sequence + frame
   OBUs) with each tile's `slice_data_offset/size` pointing into that span. The driver
   now extracts each tile's payload via the slice params, concatenates, and rebases
   `V4L2_CID_STATELESS_AV1_TILE_GROUP_ENTRY` offsets onto the concatenation.
   (`n_tiles == 0` falls back to submitting the whole buffer.)
2. **Uniform tile sizes**: with `uniform_tile_spacing_flag` set, `width/height_in_sbs_minus_1`
   are *derived* quantities. VA-API clients don't fill them there (Chrome leaves zeros,
   ffmpeg happens to compute them); the old code unconditionally copied the VA array,
   contradicting the freshly derived `mi_col/row_starts` grid — the kernel then rejected
   every frame (DQBUF ERROR / green-then-white picture). Now they're derived from the
   grid we build ourselves, both in uniform and non-uniform cases.

Verification evidence: local `av1_aom.mp4` in Chrome — hardware decode (GPU process holds
video4/media3), zero DQBUF errors, canvas shows a correct picture; YouTube — AV1 480p/720p
segments delivered during the player's ABR adaptation decoded clean (4500+ pictures across
the session, zero decode errors; the only 2 `vaEndPicture` failures were transient
"no free capture buffer" during format swaps, unrelated to AV1). Note YouTube's codec
ladder is server-side: default sessions get VP9; with VP9 hidden via a page-level
`MediaSource.isTypeSupported` shim it picks avc1, and AV1 shows up during adaptation.
No h264ify-style extension exists in any profile on this box.
