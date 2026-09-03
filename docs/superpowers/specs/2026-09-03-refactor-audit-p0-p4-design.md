# Refactor Design: audit-driven P0-P4 (quality + speed)

- Date: 2026-09-03
- Status: approved direction (scope P0-P4, branch `refactor/audit-2026-09`)
- Reference: `docs/refactor-audit-2026-09-03.md` (full findings inventory with file:line)
- Out of scope: Phase 5 file split of v4l2stateless.c (explicitly deferred)

## Goal

Clear all three P0 defects, fix 14 P1 correctness issues, delete ~900-1000
lines of duplication/dead code (~12-14% of src/), and cut the Chrome decode
hot path from ~13 syscalls/frame to ~5-6. Behavior for existing clients must
not change except where a finding documents a bug.

## Non-negotiable constraints (platform truths)

These are intentional designs purchased with hardware debugging. No phase
may alter them:

1. Export shape = ONE linear GBM_FORMAT_R8 bo (w × h*3/2 rows), Y@0,
   UV@stride*h — panthor refuses multiplanar YUV bos.
2. `num_objects == 1` in VADRMPRIMESurfaceDescriptor (Chromium requirement).
3. VPU capture buffers are NEVER exported (EXPBUF chip bug: GPU holding a
   VPU CMA buffer hangs CMA/IOMMU). One CPU copy per frame is the floor.
4. Lazy memfd: bo is the per-frame snapshot; memfd refilled only on CPU
   readback demand.
5. Never return failure for shapes Chrome actually uses (session-level
   failure caching).

## Verification protocol (gate for every phase)

1. `ninja -C builddir` clean build on NAS.
2. `tests/run_full_matrix.sh` → PASS=27 FAIL=0, including
   EXPORT_EXACT 0-7 and LAZY_EXACT 0-7.
3. Chrome live smoke on the NAS session: media-internals reports
   VaapiVideoDecoder, canvas brightness non-black, zero eglCreateImage
   errors.
4. Phase 4 additionally: strace per-frame syscall count vs the recorded
   pre-refactor baseline, and GPU-process CPU% sample.

A phase that fails any gate is fixed or reverted before the next phase.

## Phase 0 — safety net

- Fix `tests/smoke.sh` design drift: drop the `VAProfileAV1Profile0`
  expectation and the av1-default smoke decode (AV1 is intentionally
  un-advertised; run_full_matrix.sh documents this).
- meson: build a static convenience library from the shared driver sources;
  link the .so and all test executables against it (kills the four drifting
  hand-maintained source lists and the stale-binary trap).
- Delete `tests/vaapi_hwdownload.sh` (superseded early-dev script with
  hardcoded /tmp paths, third copy of forbidden()/pair()).
- Add an ASan build target for the unit tests (test_export_recapture,
  test_vp8_mpeg2_fill, test_format) — expected to confirm P0-1/P0-2.

## Phase 1 — dead code deletion (~550 lines, zero behavior change)

From audit §4 "死代码清单": `if (0)` h264 remnant + h264_fill_slice_params,
AV1 unreachable block + av1_svt_layer_slot + av1_svt_pending_arf,
v4l2sl_export_dmabuf, probe dead scan APIs (~50 lines), format.c dead
helpers, jpeg.c (void)surface_by_id, device.c stale comment block + define
guard, header dead fields (media_fd, lock, output_buf_length), dead
`close(media_fd)` in terminate, the three stub files (buffer/config/context
.c deleted from build + tree), stale comments (h264.c:419, vpp.c:82-87
redundant branch).

## Phase 2 — memory safety & correctness P0/P1

Each item lands as its own commit with a unit test where the mock-ioctl
framework allows:

- P0-1 create_surfaces fail path: cleanup under lock, `created` counter
  (never scan caller's uninitialized array), bounded free-id push.
- P0-2 terminate: destroy remaining surfaces (same routine as
  vaDestroySurfaces), free orphan_buffers (munmap mmapped entries), free
  configs.
- P0-3 VPP: clamp dw/dh to dst->width/height right after reading
  output_region.
- C1 destroy_surfaces returns capture slot via context lookup +
  cap_pool_push; invalidate current_surface if it is being destroyed.
- C2 wrap QueryConfigAttributes/QuerySurfaceAttributes in g_v4l2sl_lock.
- C3 vaCreateContext returns INVALID_CONFIG before touching device_path;
  copy device_path string instead of aliasing config's.
- C4 grow_memfd never shrinks (store current size in surface).
- C5 HEVC: collect VAIQMatrixBufferType, translate into
  v4l2_ctrl_hevc_scaling_matrix; flat-16 only when absent.
- C6 MPEG-2: chroma matrices fall back to luma matrices per H.262
  (update test_vp8_mpeg2_fill expectation).
- C7 vaGetImage uses the image buffer's recorded stride, not blit width.
- C8 vaPutImage validates fourcc==NV12 and uses image pitch.
- C9 DQBUF V4L2_BUF_FLAG_ERROR propagates an error (surface marked
  skipped, no garbage frame).
- C10 poll() wrapped in EINTR retry (device.c, vpp.c).
- C11 slice arrays sized to pending_buffers capacity + loud error on
  truncation (h264, hevc).
- C12 AV1 film grain UPDATE_GRAIN flag mapped.
- C13 AV1 fabricated sequence flags removed (map only what VA reports).
- C14 format.c converters: shared heap scratch, no silent width clamp.

## Phase 3 — deduplication (~300-400 lines net)

One commit per cluster: shared xioctl in header; all surface lookups via
v4l2sl_surface_by_id (6 sites); context_for_surface hoisted above first
use; single PRIME descriptor filler; single Annex-B scan/concat helper
(parameterized prefix length); shared codec buffer-collection loop; one
attribute table (fix the 8192 vs 4096 contradiction — pick 4096, the real
rkvdec limit for these codecs per STATE.md); probe scanners merged into
one core scan; gbm_src/memfd_stale → single `enum last_writer` with
derived staleness (fixes the gbm.c sync marker drift as a byproduct);
`dma_buf_fd` → `memfd_fd` rename; AV1 heuristics grouped in a sub-struct;
tests: migrate the two old test_export_recapture cases onto recap_env.

## Phase 4 — performance (one measurable commit at a time)

Order by benefit/risk; each item records before/after syscall count:

1. Persistent request fd + MEDIA_REQUEST_IOC_REINIT (replaces per-frame
   alloc+close).
2. SPS/sequence global controls cached in ctx, resubmitted only on
   memcmp change (h264/hevc/av1/mpeg2).
3. Request-scoped controls batched into one VIDIOC_S_EXT_CTRLS.
4. Output-plane lengths cached at REQBUFS time (drops per-QBUF QUERYBUF).
5. Persistent mappings: one mmap per memfd/bo reused across frames
   (pull_capture, ensure_memfd, derive_image/get_image).
6. cpu_ptr allocated lazily on first CPU writer (putImage/VPP/upload).
7. VPP + JPEG keep persistent queue state; per-call work reduced to
   QBUF/streamon/poll/DQBUF (STREAMOFF retained — RGA fd wedge is real).
8. v4l2sl_default_image_stride gains 64-byte alignment; pitches[] report
   it honestly (clients honor pitches).

Item rollback rule: any live-Chrome regression reverts that item alone.

## Branch & merge policy

Branch `refactor/audit-2026-09` off master@a2a41a9 on the NAS repo. One
commit per logical unit, message prefix `refactor(pN):`. Master merge
happens once, after Phase 4 passes all gates; push branch after each
phase. The deployed driver .so on the NAS may track the branch tip for
verification; final master merge redeploys and re-verifies.

## Expected outcome

- P0 count 3 → 0; P1 correctness 14 → 0; P1 perf items 5 → 0.
- ~900-1000 net lines deleted from src/.
- Chrome decode path ~13 → ~5-6 syscalls/frame; ffmpeg VPP/JPEG paths
  ~70% fewer ioctls.
- Matrix and live Chrome behavior unchanged (except documented bugfixes).
