# GBM-Backed Display Surfaces for Chrome Hardware Video Decode

**Date:** 2026-09-02
**Status:** Approved direction (user), platform-verified, ready for implementation
**Repo:** `~/vaapi-v4l2-bridge` on 192.168.1.21 (Orange Pi 5, RK3588, Armbian 26.04, kernel 7.1.8-edge)

## Problem

Chrome's `VaapiVideoDecoder` (GL backend, default) decodes on the VPU but the
video renders **black**: after decode it calls `vaExportSurfaceHandle` and
per-frame `eglCreateImage(EGL_LINUX_DMA_BUF_EXT)` on the returned fd. Our
decode surfaces live in **memfd** — a plain anonymous fd that is not a
dma-buf — so the EGL import fails every frame and no pixel is ever read back
(commit 7907909 made the export fail cleanly, and hardware decode is disabled
in `scripts/google-chrome-vaapi` until this lands).

The obvious fix — exporting the real VPU capture buffers via `VIDIOC_EXPBUF` —
is **banned**: the chip's CMA/IOMMU path hangs when the GPU still holds a VPU
buffer (proven earlier; do not revisit).

## Verified Platform Facts (tests/gbm_probe.c, run on the NAS 2026-09-02)

All findings bit-exact-verified through CPU write → dmabuf fd → Mesa EGL
import → GPU texture sample → FBO readback:

1. `/dev/dri/renderD128` (panthor): `gbm_create_device` works. The GPU
   sandbox is already disabled in the Chrome wrapper, so the driver can open
   it from inside Chrome's GPU process.
2. **Multiplanar YUV bos are refused** by panthor: `gbm_bo_create` for
   NV12 / P010 / Y210 fails EINVAL under every flag combination. A "NV12
   gbm_bo" does not exist on this render node.
3. Single-plane linear bos **work**: R8, R16, GR88, GR1616 allocate with
   `GBM_BO_USE_LINEAR`, modifier reports `DRM_FORMAT_MOD_LINEAR` (0), stride
   is tight (or padded at 4K widths), CPU `gbm_bo_map` round-trip is exact.
4. **Per-plane EGL import**: R8 is bit-exact; **GR88 is broken in Mesa
   26.0.8/panfrost** — the import succeeds but the texture samples (0,0).
   Consequence: never hand clients a GR88 single-plane image on this box.
5. **Single-object NV12 import is bit-exact at every tested size**
   (1280×720, 1920×1080, 3840×2160, 1279×719): one linear R8 bo of
   `stride × (h + ceil(h/2))` bytes, imported as one NV12 EGLImage with
   plane0 at offset 0 and plane1 (interleaved UV) at offset `stride × h`.
6. Chromium's `VaapiWrapper::ExportVASurfaceAsNativePixmapDmaBuf`
   **rejects descriptors with `num_objects != 1`** ("We only support one bo
   containing all the planes", TODO crbug.com/974438). Two separate bos
   would be discarded by Chrome.

## Design

Allocate **one driver-owned linear gbm bo per surface** (panthor shmem —
ordinary system memory, no CMA, no VPU involvement, so the EXPBUF ban is
untouched). After each VPU decode, copy the decoded frame into the bo.
`vaExportSurfaceHandle` then returns a real dma-buf descriptor in the exact
single-object shape Chrome requires.

### Data flow (per frame)

```
V4L2 capture buffer (CMA, VPU-owned — never exported)
   │ memcpy (existing pull_capture path, unchanged)
   ▼
surface memfd (unchanged: serves vaGetImage / vaDeriveImage / tests)
   │ NEW: second copy, row-wise if strides differ
   ▼
surface gbm bo (linear, R8 geometry, Y@0 + UV@stride*h)
   │ vaExportSurfaceHandle → fd + offsets/pitches
   ▼
Chrome eglCreateImage NV12 → VaapiVideoDecoder zero-copy GL → visible picture
```

### Components

**New `src/v4l2stateless_gbm.c`** (+ declarations in `v4l2stateless.h`):

- `v4l2sl_gbm_device()` — lazily opens `/dev/dri/renderD128`
  (`V4L2SL_RENDER_NODE` env override) and creates the `gbm_device`. Cached
  process-wide; a failed init is remembered so failures degrade to today's
  behavior (export → `UNIMPLEMENTED`) without retry storms.
- `v4l2sl_gbm_surface_ensure(surf)` — allocates the bo on first export:
  `GBM_FORMAT_R8`, width = `surf->width`, height = `h + ceil(h/2)` rows,
  `GBM_BO_USE_LINEAR`. Only for capture format NV12 (first release); other
  formats keep the clean `UNIMPLEMENTED`. On success stores bo + bo stride
  in the surface and immediately uploads the current memfd contents.
- `v4l2sl_gbm_surface_upload(surf, src, src_stride, rows_y)` — maps the bo
  WRITE, copies Y plane then UV plane (single memcpy per plane when strides
  match, row-wise otherwise), unmaps.
- `v4l2sl_gbm_surface_destroy(surf)` — frees the bo.
- `v4l2sl_surface_fill_prime_gbm(surf, flags, descriptor)` — fills
  `VADRMPRIMESurfaceDescriptor`: `num_objects = 1`, fd from
  `gbm_bo_get_fd_for_plane(bo, 0)` (fresh fd per export, caller-owned),
  `drm_format_modifier = gbm_bo_get_modifier(bo)` (LINEAR),
  `fourcc = VA_FOURCC_NV12`. With `VA_EXPORT_SURFACE_COMPOSED_LAYERS`:
  one `DRM_FORMAT_NV12` layer, 2 planes, offsets `[0, bo_stride × h]`.
  With `VA_EXPORT_SURFACE_SEPARATE_LAYERS`: two one-plane layers (`R8` at
  offset 0, `GR88` at offset `bo_stride × h`) — spec-conformant, same single
  object.

**Surface struct** (`v4l2stateless.h`): add `struct gbm_bo *gbm_bo;
uint32_t gbm_stride;` (opaque pointer; `struct gbm_bo` forward-declared to
keep non-GBM builds clean — meson grows a `gbm` dependency, mandatory since
libgbm-dev 26.0.8 is present).

**`v4l2sl_surface_pull_capture`** (`v4l2stateless_device.c`): after the
existing memfd copy, if `surf->gbm_bo` exists, also upload into the bo.

**`v4l2sl_export_surface_handle`** (`v4l2stateless.c`): replace the
unconditional `UNIMPLEMENTED` for decode surfaces with: NV12 →
`v4l2sl_gbm_surface_ensure` + `fill_prime_gbm`; other formats → keep
`UNIMPLEMENTED`. All under `g_v4l2sl_lock` (unchanged threading: decode and
export are already serialized).

**Surface teardown**: `v4l2sl_destroy_surfaces` calls
`v4l2sl_gbm_surface_destroy`.

**`scripts/google-chrome-vaapi`**: re-enable hardware decode —
`--disable-features=Vulkan` only; replace the "disabled until GBM lands"
comment block with the outcome matrix update (GL backend now expected to
show a picture).

### Correctness notes

- **First-export ordering**: Chrome exports after `vaSyncSurface`, and
  `pull_capture` has already filled the memfd inside the decode that
  `vaSyncSurface` waited on; `ensure` uploads current memfd so the first
  frame is present. Later frames are kept in sync by `pull_capture`.
- **Sizing/staleness**: bo is allocated from `surf->width/height` which are
  immutable after creation; capture geometry only affects the upload stride
  handling. `vaSyncSurface` ordering guarantees the CPU copy lands before
  the GPU reads (same-process; shmem is coherent — probe-verified).
- **Failure handling**: any gbm step failing logs one line and falls back to
  `UNIMPLEMENTED` — identical to today, never crashes the decode path.
- **Formats out of scope (documented, not implemented)**: 10-bit (NV15) would
  need an R16-bo P010 layout; Mesa GR88 is broken so Y210 single-plane is
  not reachable. 8-bit 4:2:0 covers H.264/HEVC/VP9/AV1-main (all bilibili /
  general web video). 10-bit falls back to software as today.

### Tests

1. **`tests/gbm_probe.c`** (already written, to be cleaned and committed):
   platform gate — runs the full bo→fd→EGL→sample chain; wired into
   `tests/run_full_matrix.sh` as a new `gbm-probe` block (PASS count 25→26).
2. **`tests/va_export_client.c`** (new): decode one NV12 IDR through the real
   VA-API path, `vaExportSurfaceHandle(DRM_PRIME_2, COMPOSED_LAYERS)`,
   assert `num_objects == 1`, import via EGL NV12, render, read back and
   compare luma against the software-decoded reference (payload-hash style,
   same rigor as the existing 422 client). Wired into the matrix.
3. **Full matrix regression** — existing 25 entries must stay PASS (GBM is
   purely additive; ffmpeg/GST tests never export).
4. **Live Chrome verification** (established toolchain): main profile via
   bind-mounted `gc-dbg` + port 9222, `mi8.js` reads
   `chrome://media-internals` → expect `VaapiVideoDecoder`; `canvas2.js`
   brightness → expect non-black (~60-75 avg); GPU process holds rkvdec
   `/dev/video*` fd (state_main.sh); wrapper no longer disables
   `AcceleratedVideoDecoder`.

## Success Criteria

- Matrix: PASS=27 (25 existing + gbm-probe + va-export), FAIL=0.
- Chrome live on live.bilibili.com H.264: `VaapiVideoDecoder` **and** a
  visible (non-black) picture **and** rkvdec held by the GPU process.
- No regression in the software paths (ffmpeg/GST clients unchanged).
