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
| AV1 Profile0 8-bit | hantro `/dev/video4` + sysfs media node | bit-exact vs ffmpeg SW (libaom, libaom realtime, SVT-AV1 RA, 4K); **re-advertised 2026-09-03** (old hang = undersized PSU) and Chrome-verified (local file + YouTube + bilibili WebCodecs, 9a6a4ce) |
| VP8 | hantro `/dev/video2` | bit-exact vs ffmpeg SW |
| MPEG-2 Simple / Main | same `/dev/video2` | bit-exact vs GST `v4l2slmpeg2dec` (hantro IDCT ≠ ffmpeg SW) |
| JPEG Baseline encode | VEPU121 `/dev/video3` | `mjpeg_vaapi` (stateful M2M) |
| VPP | RGA `/dev/video0` | `scale_vaapi` |

Chrome `FillProfileInfo_Locked` attrib query is implemented (`vaQueryConfigAttributes` treats `*num_attribs` as output-only; `max_attributes=32`). Official Linux arm64 Chrome still needs the wrapper in `scripts/google-chrome-vaapi`. Firefox uses FFmpeg VA-API + `scripts/firefox-vaapi-user.js`. VLC uses `avcodec-hw=vaapi`.

## Still not done / do not claim

- **VP9** — no VP9 OUTPUT fourcc on this mainline hantro node
- **Forced browser HW on VP9 / 10-bit HDR** — has hung the VPU; keep `media.hardware-video-decoding.force-enabled=false`
- **Browser mid-stream resolution changes** — capture renegotiate exists in the driver; Chrome/Firefox path is not matrix-tested
- **AV1 super-res (`superres_denom > 8`)** — will not decode bit-exact on this hantro/kernel. Driver now derives coded width (`frame_width`) from display width + denominator; kernel still mismatches pictures after KEY. See verification coverage 2026-09-04.
- **AV1 aomenc 2-pass lossless+intrabc clip (ffmpeg VA)** — not bit-exact. Kernel accepts the fill (0 DQBUF errors); 30/60 framemd5 mismatch from shown frame 29. The clip never sets `allow_intrabc` (IntraBC is KEY/INTRA_ONLY-only; `--enable-intrabc` was accepted). Same class as other ffmpeg raw-tile refresh-heuristic edges. A 10-frame all-intra IntraBC probe was 10/10. See verification coverage 2026-09-04.
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

## 2026-09-04 — bilibili (WebCodecs) AV1: refresh truth parsing + 40-slot pool (9a6a4ce)

bilibili's "BILIAV1" encoder exposed two gaps the YouTube/local-file verification
never hit:

1. **refresh_frame_flags inference broke**: BILIAV1 assigns DPB slots in an order
   no order-hint heuristic reproduces (hidden-showable ARF chain, prf 7->4->0,
   sequential-then-reused slots). Every INTER frame decoded wrong (reference
   ghosting; only keyframes matched sw). VA-API cannot carry the field, but
   Chrome's slice buffer contains the whole OBU run — the frame headers are
   right there. `av1_parse_hdr_refresh()` walks every frame OBU in every
   submitted slice buffer, parses the uncompressed header under 4 candidate
   layouts (libva hides the screen-content / integer-MV sequence modes), and
   accepts only parses whose frame_type / show_frame / order_hint /
   primary_ref_frame all match VA's picture params. ffmpeg's raw-tile buffers
   fail the walk and keep the heuristics (matrix paths unchanged). 1285/1289
   frames parsed true in-session, refresh sequence bit-identical to an
   independent dav1d-order bitstream dump. Bitstream-parsing traps that cost
   hours: order_hint_bits = f(3)+1; BOTH frame-dimension length prefixes are
   read before the dimensions; a leading show_existing_frame bit; KEY && !show
   DOES read f(8).
2. **WebCodecs pool depth**: bwp allocates one surface per queued VideoFrame
   (vaCreateSurfaces n=1, repeatedly) and decodes ahead of display; the shared
   24-slot capture pool drained in ~5s (no-free-capture -> vaEndPicture error
   -> player falls back to HEVC). AV1 capture buffers are NOT CMA-backed on
   this hantro, so AV1 requests 40 slots (`V4L2SL_NUM_CAPTURE_BUFS_AV1`); the
   per-context arrays now use `V4L2SL_MAX_CAPTURE_BUFS`. Remaining limitation:
   an unbounded prebuffer burst before playback starts can still drain 40 and
   fall back to HEVC gracefully (no corruption, no crash) — pool growth is
   capped by design there.

Verification: refresh truth + 1285-frame 0-error session (dropped 0), matrix
PASS=32 before and after. Known-good ffmpeg heuristic paths unchanged.

## 2026-09-04 — soak findings: bilibili WebCodecs decode-ahead vs zero-copy (open)

A 15+ min logged-in soak answered "is AV1 normal?" precisely:

- **Decode correctness: fixed and stable.** refresh truth parsing holds across
  sessions; when the AV1 pipeline runs, playback is clean (0 dropped in every
  measured window, correct picture).
- **Steady state on bilibili: falls back to HEVC within seconds of startup.**
  bwp's WebCodecs pipeline decode-ahead has no client-side queue bound; with
  our zero-copy GBM export every queued VideoFrame IS a capture buffer, and
  the player holds more frames than any pool before it starts displaying
  (buffered-start threshold). Experiments (all reverted):
  1. bounded blocking wait in vaEndPicture (2s/12s/60s, global lock released,
     dedicated pool mutex+cond): **60s wait recycled ZERO surfaces** — playback
     had not started, so nothing displays, so nothing recycles. Circular.
  2. pool 120: the kernel REQBUFS caps this hantro capture queue at **64**;
     still below the start threshold. Blocking also degrades UX from "fast
     clean fallback" to "endless loading spinner" — worse than the error path.
- **Why x86 works**: Chrome's WebCodecs on typical Intel/AMD setups uses the
  COPY path (decoder output copied into client frames, VA surfaces recycle
  immediately). We deliberately run zero-copy (GBM single-object NV12 export)
  for compositor efficiency — that choice is what couples surface lifetime to
  player queue depth.
- **Architecture direction (next session)**: eager copy-out for WebCodecs-style
  clients — at vaSyncSurface, snapshot decode->memfd (machinery exists), then
  release the capture buffer as soon as our own DPB model (refresh_frame_flags
  is now parsed from the bitstream, so the slot map is known exactly) says its
  slot was overwritten. Export then copies memfd->GBM bo (CPU) instead of
  RGA-blitting from the V4L2 buffer. Cost: ~100MB/s memcpy at 720p30; gain:
  live capture buffers bounded by kernel DPB (~10) instead of player queue.
- Current shipped state: correct decode + graceful HEVC fallback when the
  prebuffer burst exceeds the 40-slot pool. Matrix PASS=32.

## 2026-09-04 (later) — YouTube confirms: the pool death is client-agnostic

Forcing YouTube onto av01 (hide avc1/vp9/vp8 at the MediaSource.isTypeSupported
level ONLY — hiding canPlayType too trips YouTube's "browser cannot play" gate)
reproduces the identical startup death on the MSE/<video> path: av01 selected,
3x no-free-capture, playback never starts (t=0 across 90s). Both with emulated
60 Mbps and on the real ~3 Mbps path — the startup burst is bounded in FRAMES
(a low-bitrate av01.0.01M rendition dies exactly like 1080p), so network speed
does not save it. YouTube, unlike bilibili, did not fall back to another codec
within the observation window — a stuck spinner instead.

Practical exposure for daily use: YouTube serves VP9 by default (software
decode, unaffected); AV1 death only bites when their server-side rollout hands
this client av01. bilibili falls back to HEVC cleanly. The copy-out
architecture noted above is the fix for both.

(Also confirmed this morning: the overnight "Chrome graphical startup
deadlock" was the router's fake-ip DNS blackholing Google endpoints — with the
proxy path recovered, plain Chrome starts in 1s with no flags.)

## 2026-09-04 (final) — AV1 copy-out shipped: DPB-model buffer release (8b5ea0c)

The architecture direction above is implemented. pull_capture already
snapshotted every decoded frame (GBM bo for display surfaces, memfd
otherwise), so the capture buffer only remained alive as a kernel DPB
reference. The context now tracks slot->buffer with the SAME
refresh_frame_flags submitted to the kernel and hands a buffer back the
moment its slot is overwritten (refresh==0 frames return immediately).
Live capture buffers are bounded by the kernel DPB (~10), not the player
queue depth.

**The trust gate is the load-bearing wall**: the slot model is armed only
by the OBU-parse truth (Chrome-style submissions). Heuristic streams
(ffmpeg raw tiles) keep the legacy surface-attached release. Reason: the
model recycles buffers according to the flags it knows — if those are the
wrong heuristic flags, the kernel slot table (fed by the same flags) is
wrong too, and fast recycling turns latent wrong-slot reads into visible
corruption. Exactly this showed up during bring-up: av1-aom-49 MD5 diff
(double release authority — fixed by making the model the only releaser
for armed contexts) and av1-svt crashing nonref reads (derive/get_image
gated on buf_index >= 0 — fixed by an explicit has_pic flag).

Two latent bugs fixed on the way:
- surface_id was NEVER assigned (calloc zero) — the buf_owner map tracked
  surface 0 for every buffer, silently disabling owner lookups.
- SVT heuristic stored the ABSOLUTE order hint as `gop`; only the first
  GOP (key_oh=0, first ARF oh=16) coincidentally matched. Later GOPs
  (48+) broke every layer computation: 20/60 frames of av1_svt corrupt.
  The matrix only decodes the first 32 frames, so this was invisible to
  it — full-clip sw-vs-hw compare is now the extra check (60/60 exact).
  Known remaining edge: ffmpeg + BILIAV1 streams stay corrupt (heuristic
  unfixable by design, Chrome path uses the parser); matrix unaffected.

Verification: matrix 2x MATRIX_ALL_PASS (deterministic); bilibili
logged-in soak (62-min AV1 video): 5 min + 4-seek storm at 720p30, AV1
on video4 throughout, 2 dropped frames total, zero pool errors; YouTube
av01 via MSE shim: 1080p60 clean, codecs locked av01.0.09M.08.

## 2026-09-04 — AV1 verification coverage

### Super-res

- **Verdict:** A1_STILL_FAIL (hardware/kernel limitation). Phase A item closed.
- **Clip:** `verify/clips/av1_superres.webm` (gitignored) — aomenc `--superres-mode=1 --superres-denominator=12`, 1280x720, 60 frames / 2s. aomenc `--webm` aborted (buffer overflow); encoded `--ivf` then remuxed to WebM.
- **Baseline (before coded-width fix):** A1_FAIL. hw rc=0, 60/60 frames produced, 59/60 framemd5 mismatch (only frame 0 matched). `grep -cE "failed to set AV1|not mapped|no completed"` = 0. 122 `DQBUF ... ERROR` lines (capture type=9 idx=22, output type=10 idx=3). Capture stayed DISPLAY size: `renegotiate capture 1280x720  -> 1280x720 NV12 streamed=0` (not coded ~853x720).
- **Fix applied:** `av1_fill_frame_params` derives `disp_w`/`disp_h`/`sr_denom`/`coded_w` once after memset; tile-grid `mi_cols` uses coded width when `sr_denom > 8`; `upscaled_width`/`render_*` keep display, `frame_width_minus_1` is coded. `v4l2sl_ensure_capture` kept DISPLAY 1280x720.
- **After fix:** A1_STILL_FAIL. hw2 rc=0, DQBUF errors gone (0), still 59/60 framemd5 mismatch (frame 0 bit-exact; frames 1–59 unique hashes but wrong). Capture still `1280x720 -> 1280x720 NV12`. Params look correct; pictures still mismatch = hantro/kernel will not decode super-res.
- **Regression:** `av1_svt.mp4` full-clip hw-vs-sw 60/60 bit-exact (`A1_REGRESSION_CLEAN`). Coded-width derivation kept (kernel now receives distinct `frame_width` vs `upscaled_width` when denom > 8; does not regress non-superres SVT).

### Lossless

- **Verdict:** A2_lossless_PASS. Phase A item closed.
- **Clip:** `verify/clips/av1_lossless.webm` (gitignored) — ffmpeg libaom `-lossless 1 -cpu-used 8 -threads 1`, lavfi `testsrc` 1280x720@30 2s, 27549 bytes.
- **Compare:** hw rc=0, 60/60 framemd5 bit-exact vs software (`hwdownload,format=nv12` → yuv420p). `failed to set AV1|not mapped|no completed` = 0. `DQBUF ... ERROR` = 0. Capture `1280x720 -> 1280x720 NV12` on `/dev/video4`. Unique per-frame hashes (not a stuck buffer).

### IntraBC

- **Verdict:** A2_intrabc_FAIL (wrong-frames, not kernel-reject). Phase A item closed as a documented limitation; no driver patch (flags already forwarded; fix is not small/obvious).
- **Clip:** `verify/clips/av1_intrabc.webm` (gitignored) — aomenc `--lossless=1 --enable-intrabc=1 --cpu-used=8` on lavfi `testsrc2` 1280x720@30 `--limit=60`. `--enable-intrabc` was accepted (default 1). aomenc `--webm` aborted (`*** buffer overflow detected ***`, rc=134); encoded `--ivf` then `ffmpeg -c copy` remux to WebM (3195370 bytes).
- **Compare:** hw rc=0, 60/60 frames produced, 30/60 framemd5 match (shown 0–28 and 30), 30/60 mismatch (shown 29, 31–59). HW hashes all unique (not stuck). `failed to set AV1|not mapped|no completed` = 0. `DQBUF ... ERROR` = 0. Capture `1280x720 -> 1280x720 NV12` on `/dev/video4`.
- **Classification:** wrong-frames. Kernel accepted sequence/frame/tile controls. Bitstream has `seq_force_screen_content_tools=SELECT` but every frame `allow_screen_content_tools=0` / `allow_intrabc=0` — IntraBC is intra-only, so this 2-pass inter pyramid never used it. Shown 30 matching after 29 failing is `show_existing_frame` of a still-good slot. Failure starts at the first new decode after the lag-35 pyramid's later ARFs — ffmpeg-VA raw-tile `refresh_frame_flags` heuristic, not a missing IntraBC flag. Driver already maps VA `allow_intrabc` / `allow_screen_content_tools` / `force_integer_mv` onto `V4L2_AV1_FRAME_FLAG_*`.
- **Diagnostic (not the brief clip):** 10-frame 1-pass all-intra `--lossless=1 --enable-intrabc=1 --lag-in-frames=0 --kf-max-dist=1` at 640x360 actually set `allow_intrabc` on KEY 0–1; hw-vs-sw 10/10 bit-exact. IntraBC tools themselves are not the gap.

### Concurrent (local)

- **Verdict:** A3_LOCAL_PASS. Phase A item closed. Two concurrent ffmpeg VA-API AV1 decodes (libaom + SVT-AV1, both 1280x720 60 frames) are bit-exact vs sequential software framemd5.
- **Clips:** `verify/clips/av1_aom.mp4` and `verify/clips/av1_svt.mp4` (existing; 1280x720@30, 2s / 60 frames).
- **Method:** two `ffmpeg -hwaccel vaapi` processes started in background (`&` + `wait`) with `LIBVA_DRIVER_NAME=v4l2stateless`. Brief `$DEC` put `-vf` before `-i` (ffmpeg: option cannot be applied to input url, rc=234, no md5 files). `-vf hwdownload,format=nv12` moved after `-i` so it is an output option; two parallel hw processes kept. Sequential SW references, then `cmp` both pairs.
- **Overlap:** both ffmpeg PIDs alive together (384232 aom + 384231 svt); wall overlap 0.577s of ~0.58s decode; both opened `/dev/video4` (`rockchip,rk3588-av1-vpu-dec`) / `/dev/media3`.
- **Compare:** hw rc=0 both. `cmp /tmp/a_c1.md5 /tmp/a_c1sw.md5` and `cmp /tmp/a_c2.md5 /tmp/a_c2sw.md5` match. 60/60 unique hashes each clip (aom first `747488486943da9f651ad0f088203493`, svt first `057c6a06b81d8507d436ffd9e766ac87`). Capture `1280x720 -> 1280x720 NV12` 40 capture / 4 output buffers both jobs.
- **grep -hcE "no free capture|failed":** 0 and 0. Tighter: `v4l2stateless: no free capture` = 0/0, `failed to set AV1` = 0/0, `DQBUF ... ERROR` = 0/0. Loose `failed` = 0/0 (no ffmpeg "failed to" noise).
- **dmesg:** clean. `sudo dmesg | tail` unchanged vs pre-test (last lines still rockchip-rga 160x120 from earlier VPP; no new hantro/AV1/video4 errors). Usual `derive_image: surface 1 has no decoded frame` on both stderr (same as other passing AV1 runs).

### Concurrent (browser)

- **Verdict:** A4_BROWSER_PASS. Phase A item closed. Two distinct Chrome tabs played concurrent AV1 (bilibili `av01` 720p, codecid 13 / `100024.m4s`) on `/dev/video4` (`rockchip,rk3588-av1-vpu-dec`).
- **URLs:** `https://www.bilibili.com/video/BV1Vst861EwN` (tab0, 1281x720) and `https://www.bilibili.com/video/BV1G7tG6tEwL` (tab1, 1280x720). Two page targets via CDP `Target.createTarget` (brief `pages[1]||pages[0]` would have attached both URLs to one tab when `/json` had a single page).
- **Method:** wrapper `google-chrome-stable` `--user-data-dir=$HOME/.config/gc-dbg --remote-debugging-port=9222`. Official soak also passed `--disable-background-media-suspend --disable-renderer-backgrounding --disable-backgrounding-occluded-windows` so background tabs keep decoding (without these, Chrome paused the occluded tab at `t` frozen). Driver: `/tmp/twotabs.js`. SIGTERM between AV1 sessions; never `-9`. `gc-dbg/Default` left intact.
- **Probes (4 x 15s):** both `t` advanced 20 → 35 → 50 → 65, `paused:false`, `dropped` flat (tab0=2, tab1=3, not climbing). Both codecs locked `av01` (`41469086614-1-100024.m4s` / `41488810129-1-100024.m4s`). Distinct targets `1E20111C506EA46AB4FFC4E7D451CF79` and `BF94F3075F0FA0E4D249BEAC1ECB7914`.
- **video4:** Chrome GPU process held two `/dev/video4` + two `/dev/media3` FDs from the second context open through all four probes (`opened /dev/video4` x2, renegotiate `1280x720 -> 1280x720 NV12` x2).
- **grep:** `"AV1 config uses"` = 4 (≥2; 2 vainfo-style at Chrome start + 1 per tab). `no-free-capture|pull capture failed|VA_STATUS` = 0.
- **Shutdown:** `pkill -TERM -f user-data-dir=$HOME/.config/gc-dbg`; sleep 6; Chrome gone, 9222 closed.

### Browser resolution switch

- **Verdict:** A4/A5_RESSWITCH_PASS. Phase A item closed. YouTube av01 mid-stream quality switch 256x144 → 1920x1080 on `/dev/video4` (`rockchip,rk3588-av1-vpu-dec`). Codecs stayed `av01…` (tiny `av01.0.00M.08`, hd1080 `av01.0.09M.08`). Bilibili quality-menu secondary path not needed (YouTube ladder had both av01 tiny and av01 hd1080).
- **URL:** `https://www.youtube.com/watch?v=aqz-KE-bpKQ` (Big Buck Bunny). MSE shim hid `avc1`/`vp9`/`vp8` at `MediaSource.isTypeSupported` only. Driver: `/tmp/yt_resswitch.js`.
- **Method:** wrapper `google-chrome-stable` `--user-data-dir=$HOME/.config/gc-dbg --remote-debugging-port=9222 --remote-allow-origins=* --disable-background-media-suspend --disable-renderer-backgrounding --disable-backgrounding-occluded-windows`. Fresh instance (session stickiness). Log `/tmp/chrome_a4.log`. SIGTERM only; `gc-dbg/Default` left intact.
- **Probes (8 x 15s, switch at k=2):** PROBE0–1 `t` 24.8 → 39.8, `256x144`, `av01.0.00M.08`, `dropped=0`. PROBE2 (switch instant) `t=0 w=0` while YouTube rebuilt the MSE source. PROBE3–7 `t` 68.2 → 83.2 → 98.2 → 113.2 → 128.2, `1920x1080`, `av01.0.09M.08`, `dropped=0`. `paused:false` every probe. `t` advancing after the switch; dropped flat.
- **video4:** Chrome GPU pid **408519** held `/dev/video4` from first context through all probes (`fuser` = that GPU process; `lsof` one character-device FD). `opened /dev/video4` x3 (initial 1080, tiny, hd1080).
- **renegotiate (expected, informational):** `1920x1080 -> 1920x1080 NV12` then `256x144 -> 256x144 NV12` then `1920x1080 -> 1920x1080 NV12` (3 lines). `"AV1 config uses"` = 5.
- **errors:** `no-free-capture|no free capture|pull capture failed|VA_STATUS` = 0.
- **Shutdown:** `pkill -TERM -f user-data-dir=$HOME/.config/gc-dbg`; sleep 6; Chrome gone, 9222 closed, video4 free.

## 2026-09-04 — AV1 verification coverage closed (super-res / lossless+intrabc / concurrency / browser res-switch)

Phase A gate. Do not start Phase B until this heading exists. All four Phase A items recorded; browser res-switch is the last.

| Item | Verdict | Notes |
|---|---|---|
| A1 super-res | A1_STILL_FAIL (kernel/hantro limitation) | Coded-width fill landed (`f962bd8`); DQBUF errors gone; pictures still wrong when denom>8. SVT regression clean. |
| A2 lossless | A2_lossless_PASS | 60/60 framemd5 vs software. |
| A2 IntraBC | A2_intrabc_FAIL (wrong-frames, not kernel-reject) | Brief 2-pass clip never set `allow_intrabc`; flags already forwarded. All-intra diagnostic 10/10. |
| A3 concurrent local | A3_LOCAL_PASS | Two ffmpeg VA-API AV1 jobs bit-exact vs sequential SW. |
| A4 concurrent browser | A4_BROWSER_PASS | Two Chrome tabs, both `av01` 720p, video4 held two FDs, errors 0. |
| A4/A5 browser res-switch | A4/A5_RESSWITCH_PASS | YouTube av01 256x144 → 1920x1080; codecs stayed av01; t advancing; dropped 0; video4 held; error count 0; renegotiate expected. |

Phase A **closed**. Phase B (hot-path cleanup) may start after this commit.


## 2026-09-04 — Phase B hot-path (gated on Phase A)

- Tile grids / VA slice-param counts **> 32 are refused** before any ioctl
  (`4b9a54a`). Silently truncating to 32 while still submitting the original
  `tile_cols*tile_rows` hung the hantro AV1 node (SoC hard reset). Movie
  streams never hit this; do not VA-decode `verify/clips/av1_40tiles.mp4`.
- Fallback OBU-parse loop skips the span already tried (`a070541`).
- Grain + frame + tile-group batched into **one** request `S_EXT_CTRLS`
  (`1ec74f8`). strace on `av1_aom.mp4`: `VIDIOC_S_EXT_CTRLS` 186 → 66
  (exactly 1/3). Matrix 2× `MATRIX_ALL_PASS`; av1_svt full-clip bit-exact.
