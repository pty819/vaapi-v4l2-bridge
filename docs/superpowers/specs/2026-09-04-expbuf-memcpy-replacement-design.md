# EXPBUF as memcpy replacement — design

Date: 2026-09-04  
Status: approved success bar (Chrome zero-copy required)  
Branch / worktree: `experiment/expbuf-retry` at
`~/vaapi-v4l2-bridge/.worktrees/expbuf-retry`  
Shipping `.so` stays GBM-copy until this spec’s Chrome gate passes.

## Claim we are testing

Historical: `VIDIOC_EXPBUF` of a VPU capture buffer + GPU import hangs
RK3588 (CMA/IOMMU), so every decoded frame is `memcpy`’d into a driver
GBM bo. That hang coincided with an undersized PSU, later replaced.

**Hypothesis:** on the current PSU, EXPBUF of a *decoded* capture buffer
can be imported by Chrome’s ANGLE/Wayland zero-copy path, bit-exact, without
rebooting. If true, the per-frame capture→GBM copy can be skipped.

**Success (user):** Chrome `VaapiVideoDecoder` actually imports the VPU
dma-buf (not the GBM copy, not a memfd), plays a real stream (local file
then bilibili), picture correct, host stays up. Laboratory EGL bit-exact
is a **prerequisite**, not the proof.

## What is already done (Phase 0 — do not redo)

Documented in `docs/EXPBUF-RETRY.md` on this branch:

- Idle EXPBUF ladder stages 1–6 on rkvdec 1280×720 (incl. STREAMON) and
  hantro AV1: EXPBUF, CPU mmap, `PRIME_FD_TO_HANDLE`, `gbm_bo_import`
  (NV12 fourcc EINVAL; **R8 tall** OK), `gbm_bo_map`, `eglCreateImageKHR`
  NV12 single-fd → PASS, host alive.
- `V4L2SL_EXPBUF_EXPORT=1` + `LIBVA_DRIVERS_PATH` (system dri **untouched**):
  `va_export_client` 8× 720p H.264 IDR: `EXPBUF export ok` then
  `EXPORT_EXACT` + `LAZY_EXACT` 0–7 vs GetImage. Host alive.

Caveat of Phase 0: `pull_capture` still memcpy’d into memfd/GBM *and*
Export EXPBUF’d. That proves import liveness + pixels, **not** “memcpy
removed”.

## Replacement architecture

When `v4l2sl_expbuf_export_wanted()` is true:

1. `pull_capture` **must not** `gbm_surface_upload` (that *is* the memcpy
   we want to drop). Set `has_pic`, `buf_index`, stride/aligned_h/fourcc.
   Optional: skip memfd snapshot too; GetImage maps the capture mmap
   (or EXPBUF fd) for ffmpeg-only tests.
2. `vaExportSurfaceHandle` EXPBUFs `surf->buf_index`, fills
   `VADRMPRIMESurfaceDescriptor` with **one object**, NV12, UV at
   `pitch * aligned_h` (same lie Chrome already requires). Dup/cache
   `surf->expbuf_fd`; close on surface destroy / buf_index change.
3. Capture-buffer **lifetime** stays tied to kernel DPB **and** the VA
   surface Chrome holds. Do not `cap_pool_push` an index whose fd is
   still exported and mapped by the GPU, and do not push while any AV1
   slot still references it. HEVC/H.264 keep today’s “push on
   BeginPicture re-target” only if Chrome has already released the
   previous surface (VA contract). If a hang appears here, that is a
   **real** remaining bug, not “PSU”.
4. Default (`env` unset) **during the experiment**: unchanged GBM copy.
   Never `sudo cp` over `/usr/lib/.../dri/` until Phase 4 (Chrome) passes.
   **Once Phase 4 passes, the approved product outcome is to make EXPBUF
   the shipping default** (drop capture→GBM memcpy) so the dri `.so`
   Chrome actually loads uses the cheaper path. GBM copy remains a
   fallback if EXPBUF fails at runtime.

`gbm_bo_import(NV12)` staying EINVAL is OK: Chrome uses EGL dma-buf
images (`num_objects==1`), which already succeeded in Phase 0.

## Phases (stop on hang / mismatch)

| Phase | Proof | Abort if |
|---|---|---|
| 0 | Idle + decoded H.264 export+EGL vs GetImage (done) | — |
| 1 | Skip GBM memcpy in EXPBUF mode; `va_export_client` still exact; log no `gbm upload` | hang, mismatch, silent GBM fallback |
| 2 | P-frames: export surface N while decoding N+1 (DPB still live) | hang, wrong pixels on N |
| 3 | HEVC Main + AV1 720p/1080p, same client | hang / mismatch |
| 4 | Chrome: `LIBVA_DRIVERS_PATH` + env, `ioctl_interpose` shows PRIME of **expbuf** fd; local 720p then bilibili; SIGTERM only | hang, black frames, interpose shows GBM bo fd |
| 5 | Ship: EXPBUF default in the dri `.so` (memcpy gone); GBM fallback on EXPBUF failure | — |

Every GPU-touching command: `timeout -k 5 <budget>`. Chrome: SIGTERM,
never `-9`. Worktree only.

## Chrome evidence (Phase 4)

Must have **all**:

- Driver stderr: `EXPBUF export ok` (not `falling back`)
- `tests/ioctl_interpose.so`: `PRIME_FD_TO_HANDLE` target is the V4L2
  expbuf (not `memfd:v4l2sl-surf`, not a panthor GBM handle created
  before EXPBUF)
- `chrome://media-internals`: `VaapiVideoDecoder`
- Canvas / playback: non-black, time advancing
- Host: no reboot

## Non-goals

- 10-bit / P010 GBM
- Flipping the dri `.so` / master default **before** Phase 4 Chrome evidence
  (after Phase 4, flipping default **is** the intended product change)
- Rewriting HEVC/AV1 DPB models except as needed to not recycle an
  exported index
