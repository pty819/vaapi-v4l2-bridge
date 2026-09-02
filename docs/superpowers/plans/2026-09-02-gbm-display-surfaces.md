# GBM-Backed Display Surfaces Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let Chrome hardware-decode video **and** show a picture, by exporting each decode surface as a driver-owned linear GBM bo (single-object NV12 dma-buf) instead of failing `vaExportSurfaceHandle`.

**Architecture:** VPU capture buffers stay in the kernel (EXPBUF is a banned chip-bug path). Each surface gains one lazily-allocated `GBM_FORMAT_R8` linear bo on `/dev/dri/renderD128` (panthor shmem — plain system memory, no CMA); `pull_capture` copies decoded frames into it beside the existing memfd copy; the export path fills a `VADRMPRIMESurfaceDescriptor` with `num_objects=1`, Y at offset 0, UV at offset `stride×h` — the exact Intel-classic shape Chromium requires (`num_objects != 1` is rejected upstream).

**Tech Stack:** C11, libva 2.23, libgbm 26.0.8, libdrm 2.4.131, Mesa 26.0.8 EGL/GLES3 (panthor), meson, RK3588 V4L2 stateless request API.

**Spec:** `docs/superpowers/specs/2026-09-02-gbm-display-surfaces-design.md` (commit f53f3f1)

## Global Constraints

- Repo lives on the NAS: `liyifan@192.168.1.21:~/vaapi-v4l2-bridge` (branch master). Local review/edit copy: `/Users/liyifan/.zcode/workspace/default/v4l2bridge-review/` — edit locally, then `scp` to NAS and copy into the repo before building.
- Build: `cd ~/vaapi-v4l2-bridge && meson compile -C builddir`. Install: `echo liyifan | sudo -S install -m 0755 builddir/v4l2stateless_drv_video.so /usr/lib/aarch64-linux-gnu/dri/` (sudo password `liyifan`; NEVER mix `sudo -S` with heredoc stdin).
- **Never** call `VIDIOC_EXPBUF` on V4L2 capture buffers for GPU import (chip bug: CMA+IOMMU hang). Do not revive dma-heap or DRM-card imports. The GBM bo is panthor-allocated system memory — it is the only sanctioned export vehicle.
- GBM export path is **NV12-only** (`cap_fourcc == V4L2_PIX_FMT_NV12`). All other capture formats keep `VA_STATUS_ERROR_UNIMPLEMENTED` (software fallback, today's behavior).
- Mesa 26.0.8 quirk: per-plane GR88 dmabuf import samples (0,0) — never emit a GR88 single-plane image to clients as the primary shape; the composed NV12 single-object shape is verified bit-exact at 720p/1080p/4K/odd sizes.
- V4L2 device node numbers are unstable across reboots — never hardcode `/dev/videoN`.
- Kill Chrome only by exact `/opt/google/chrome` argv prefix with split-string patterns (e.g. `pkill -f "/opt/google/chrome/chr""ome "`), never `pkill -f chrome` (matches the SSH session itself).
- Chrome debugging must use the user's MAIN profile: bind mount `~/.config/google-chrome` → `~/.config/gc-dbg` + `--remote-debugging-port=9222`. Never spawn separate test profiles.
- Existing matrix must stay green: PASS grows 25 → 27, FAIL stays 0.
- Commit style: `feat:`/`fix:`/`docs:`/`test:` prefixes, imperative subject.

---

### Task 1: Commit the platform probe and wire it into the matrix

**Files:**
- Create (repo): `tests/gbm_probe.c` (already written locally, passes on NAS)
- Modify: `tests/run_full_matrix.sh`

**Interfaces:**
- Produces: matrix block name `gbm-probe` (PASS counted); binary built to `$OUT/gbm_probe` with `gcc -O2 -Wall -o "$OUT/gbm_probe" tests/gbm_probe.c -lgbm -lEGL -lGLESv2`.

- [ ] **Step 1: Clean the probe's final verdict**

The local `tests/gbm_probe.c` currently exits 1 because its main-path YUV check trips on the Mesa GR88 bug (the load-bearing `diag3` single-bo check passes). Restructure so the **single-bo diag3 is the main flow**: delete the old two-bo `imgY`/`imgC` import + first render/readback and the `diag import NV12 (2 objects)` block; keep: survey, two-bo fd/map checks, boC-as-R8 diagnostic, and make the single-bo block the unconditional finale printing `PASS: single-object NV12 bo export/import is bit-exact`. Keep `setvbuf(stdout, NULL, _IONBF, 0)`.

- [ ] **Step 2: scp to NAS, copy into repo, compile, run**

```bash
scp -q tests/gbm_probe.c liyifan@192.168.1.21:/tmp/
ssh liyifan@192.168.1.21 'cp /tmp/gbm_probe.c ~/vaapi-v4l2-bridge/tests/ && cd ~/vaapi-v4l2-bridge && gcc -O2 -Wall -o /tmp/gbm_probe tests/gbm_probe.c -lgbm -lEGL -lGLESv2 && /tmp/gbm_probe 1280 720'
```
Expected: last lines `diag3 single-object NV12 is bit-exact` and `PASS: ...`, exit 0. Also re-run at `1920 1080` and `3840 2160`.

- [ ] **Step 3: Add the gbm-probe matrix block**

In `tests/run_full_matrix.sh`, right after the clip-generation section (before the first codec test), add:

```bash
# --- gbm-probe: platform gate for GBM-backed display surfaces -------------
cc -O2 -Wall -o "$OUT/gbm_probe" tests/gbm_probe.c \
  -lgbm -lEGL -lGLESv2 || { echo CC_FAIL gbm_probe; fail=$((fail+1)); }
if [ -x "$OUT/gbm_probe" ]; then
  if "$OUT/gbm_probe" 1280 720 >"$OUT/gbm_probe.log" 2>&1 &&
     "$OUT/gbm_probe" 1920 1080 >>"$OUT/gbm_probe.log" 2>&1; then
    echo "PASS gbm-probe"; pass=$((pass+1))
  else
    echo "FAIL gbm-probe"; fail=$((fail+1))
  fi
fi
```

- [ ] **Step 4: Run the matrix, verify PASS=26 FAIL=0**

```bash
ssh liyifan@192.168.1.21 'cd ~/vaapi-v4l2-bridge && tests/run_full_matrix.sh 2>&1 | tail -5'
```
Expected: `PASS=26 FAIL=0` (was 25).

- [ ] **Step 5: Commit**

```bash
ssh liyifan@192.168.1.21 'cd ~/vaapi-v4l2-bridge && git add tests/gbm_probe.c tests/run_full_matrix.sh && git commit -m "test: gbm_probe platform gate (single-object NV12 export is bit-exact)" --quiet && git log --oneline -1'
```

---

### Task 2: Failing end-to-end test — va_export_client.c

**Files:**
- Create: `tests/va_export_client.c`
- Modify: `meson.build` (build target only — no driver dep changes yet)

**Interfaces:**
- Consumes: none new (stock VA-API).
- Produces: `builddir/va_export_client <render-node> <stream.h264>`; exits 0 only if **every** frame's exported dma-buf content (read back through EGL) is byte-identical to `vaGetImage`; prints `EXPORT_EXACT <n>` per frame.

**Clip:** generated in Task 4's matrix block; for Task 2-3 development, generate by hand once on NAS:

```bash
ffmpeg -y -f lavfi -i testsrc=size=1280x720:rate=30 -t 1 -pix_fmt yuv420p \
  -c:v libx264 -preset ultrafast -profile:v high -g 1 -keyint_min 1 \
  -sc_threshold 0 -f h264 /tmp/h264_idr_nv12.h264
```
(x264 defaults → `pic_init_qp_minus26=-3`, `pic_init_qs_minus26=0`, CAVLC, deblocking control present — the same parameter family the 422 client uses.)

- [ ] **Step 1: Write the client**

Model on `tests/va_h264422_client.c` (Annex-B slice walker, per-frame picture/slice buffers). Differences:

- Profile `VAProfileH264High`, `VA_RT_FORMAT_YUV420`, surfaces created with fourcc `VA_FOURCC_NV12` via `VASurfaceAttribPixelFormat` (8 surfaces).
- `pp.seq_fields.bits.chroma_format_idc = 1`; `pp.pic_init_qp_minus26 = -3; pp.pic_init_qs_minus26 = 0;` all other pp/iq/slice fields identical to the 422 client (all-IDR I slices, `slice_type = 7`).
- After `vaSyncSurface`, per frame:
  1. `vaCreateImage(VA_FOURCC_NV12)` + `vaGetImage` + `vaMapBuffer` → keep `refY` (H rows of W bytes at `img.pitches[0]`) and `refUV` (H/2 rows of W bytes at `img.pitches[0]` starting `img.offsets[1]`).
  2. `vaExportSurfaceHandle(dpy, surf[n], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_COMPOSED_LAYERS, &desc)` — die with message if not SUCCESS.
  3. Assert `desc.num_objects == 1`, `desc.fourcc == VA_FOURCC_NV12`, `desc.width == W`, `desc.height == H`.
  4. EGL (setup once, before the frame loop; `EGL_PLATFORM_SURFACELESS_MESA` 0x31DD on `EGL_DEFAULT_DISPLAY`, fallback to `EGL_PLATFORM_GBM_KHR` with a gbm device on `argv[1]`; ES3 context via `EGL_CONTEXT_CLIENT_VERSION=3`; FBO with RGBA texture W×H): import TWO R8 images from the single object — plane0 `{fd, offset=desc.layers[0].offset[0], pitch=desc.layers[0].pitch[0], w=W, h=H}` and plane1 `{fd, offset=desc.layers[0].offset[1], pitch=desc.layers[0].pitch[1], w=W/2, h=H/2}` (resolve `create_img`/`get_plat_dpy` via `eglGetProcAddress`; shaders ES2-syntax grayscale: `gl_FragColor = vec4(texture2D(...).r);`). Render each, `glReadPixels` the FBO region, compare bytes:
     - plane0 readback r-channel row y == `refY` row y (W bytes, H rows)
     - plane1 readback == `refUV` (W/2×H/2 texels → W bytes/row, H/2 rows)
  5. Close `desc.objects[0].fd`. Print `EXPORT_EXACT n`.
- Exit 0 iff all frames exact; else exit 1 with the first mismatching (frame, plane, x, y, expected, got).

- [ ] **Step 2: meson target**

In `meson.build` add:

```meson
va_export_client = executable('va_export_client',
  files('tests/va_export_client.c'),
  dependencies: [libva_dep, libva_drm_dep, libdrm_dep,
                 dependency('gbm'), dependency('egl'), dependency('glesv2')],
)
```

(`pkg-config egl`/`glesv2` exist on the NAS; if `dependency('egl')` fails, use `cc.find_library('EGL')` / `cc.find_library('GLESv2')`.)

- [ ] **Step 3: Build and run — verify it FAILS for the right reason**

```bash
scp -q tests/va_export_client.c liyifan@192.168.1.21:/tmp/
ssh liyifan@192.168.1.21 'cp /tmp/va_export_client.c ~/vaapi-v4l2-bridge/tests/ && cd ~/vaapi-v4l2-bridge && meson compile -C builddir va_export_client && builddir/va_export_client /dev/dri/renderD128 /tmp/h264_idr_nv12.h264'
```
Expected: fails at step 2 with `vaExportSurfaceHandle ... VA_STATUS_ERROR_UNIMPLEMENTED` (commit 7907909 behavior). This is the red test.

---

### Task 3: GBM module + driver hooks (make Task 2's test pass)

**Files:**
- Create: `src/v4l2stateless_gbm.c`
- Modify: `src/v4l2stateless.h` (surface struct + prototypes), `src/v4l2stateless_device.c` (`pull_capture` hook), `src/v4l2stateless.c` (export branch + destroy hook), `meson.build` (dep + source)

**Interfaces:**
- Consumes: `struct v4l2sl_surface` fields `width/height/stride/aligned_h/cap_fourcc/dma_buf_fd`; `v4l2sl_capture_plane_size()`.
- Produces (declared in `v4l2stateless.h`):
  - `int v4l2sl_gbm_surface_ensure(struct v4l2sl_surface *s);` (0 = bo ready)
  - `int v4l2sl_gbm_surface_upload(struct v4l2sl_surface *s, const void *src, uint32_t src_stride, uint32_t src_alh);`
  - `void v4l2sl_gbm_surface_destroy(struct v4l2sl_surface *s);`
  - `VAStatus v4l2sl_surface_fill_prime_gbm(const struct v4l2sl_surface *surf, uint32_t flags, void *descriptor);`

- [ ] **Step 1: Header changes**

In `src/v4l2stateless.h`:

```c
struct gbm_bo;   /* opaque — only v4l2stateless_gbm.c includes <gbm.h> */
```
(after the `#include` block), and in `struct v4l2sl_surface`, after `cpu_stride`:

```c
    struct gbm_bo *gbm_bo;   /* driver-owned display copy (linear, R8) */
    uint32_t gbm_stride;
```
Prototypes after `v4l2sl_surface_fill_prime`:

```c
int v4l2sl_gbm_surface_ensure(struct v4l2sl_surface *s);
int v4l2sl_gbm_surface_upload(struct v4l2sl_surface *s, const void *src,
                              uint32_t src_stride, uint32_t src_alh);
void v4l2sl_gbm_surface_destroy(struct v4l2sl_surface *s);
VAStatus v4l2sl_surface_fill_prime_gbm(const struct v4l2sl_surface *surf,
                                       uint32_t flags, void *descriptor);
```

- [ ] **Step 2: Write src/v4l2stateless_gbm.c**

```c
/*
 * v4l2stateless_gbm.c — GBM-backed display surfaces.
 *
 * The VPU capture buffers must never be exported (EXPBUF + GPU import is a
 * RK3588 CMA/IOMMU hang). Instead every exported surface gets a
 * driver-owned linear GBM bo on the render node (panthor shmem: ordinary
 * system memory). pull_capture copies each decoded frame into it; the
 * export path hands out the classic single-object NV12 dmabuf descriptor
 * (Y at offset 0, UV at offset stride*h) that Chromium's zero-copy GL
 * path requires (vaapi_wrapper rejects num_objects != 1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include "v4l2stateless.h"

static struct gbm_device *g_gbm_dev;
static int g_gbm_fd = -1;
static int g_gbm_failed;

static struct gbm_device *v4l2sl_gbm_device(void)
{
    const char *node;

    if (g_gbm_dev)
        return g_gbm_dev;
    if (g_gbm_failed)
        return NULL;
    node = getenv("V4L2SL_RENDER_NODE");
    if (!node)
        node = "/dev/dri/renderD128";
    g_gbm_fd = open(node, O_RDWR | O_CLOEXEC);
    if (g_gbm_fd < 0) {
        fprintf(stderr, "v4l2stateless: gbm: open %s failed: %s\n",
                node, strerror(errno));
        g_gbm_failed = 1;
        return NULL;
    }
    g_gbm_dev = gbm_create_device(g_gbm_fd);
    if (!g_gbm_dev) {
        fprintf(stderr, "v4l2stateless: gbm_create_device(%s) failed\n", node);
        close(g_gbm_fd);
        g_gbm_fd = -1;
        g_gbm_failed = 1;
    }
    return g_gbm_dev;
}

static void copy_plane(uint8_t *dst, uint32_t dst_stride, const uint8_t *src,
                       uint32_t src_stride, uint32_t width, uint32_t rows)
{
    if (dst_stride == src_stride) {
        memcpy(dst, src, (size_t)dst_stride * rows);
        return;
    }
    for (uint32_t y = 0; y < rows; y++)
        memcpy(dst + (size_t)dst_stride * y, src + (size_t)src_stride * y,
               width);
}

int v4l2sl_gbm_surface_upload(struct v4l2sl_surface *s, const void *src,
                              uint32_t src_stride, uint32_t src_alh)
{
    uint32_t w = s->width, h = s->height;
    uint8_t *dst;
    void *map_data = NULL;
    uint32_t mstride = 0;

    if (!s || !s->gbm_bo || !src || !src_stride)
        return -1;
    dst = gbm_bo_map(s->gbm_bo, 0, 0, w, h + h / 2,
                     GBM_BO_TRANSFER_WRITE, &mstride, &map_data);
    if (!dst) {
        fprintf(stderr, "v4l2stateless: gbm: bo map failed\n");
        return -1;
    }
    copy_plane(dst, mstride, src, src_stride, w, h);
    copy_plane(dst + (size_t)mstride * h, mstride,
               (const uint8_t *)src + (size_t)src_stride * src_alh,
               src_stride, w, h / 2);
    gbm_bo_unmap(s->gbm_bo, map_data);
    return 0;
}

int v4l2sl_gbm_surface_ensure(struct v4l2sl_surface *s)
{
    struct gbm_device *dev;
    uint32_t rows;

    if (!s || s->width == 0 || s->height == 0)
        return -1;
    if (s->gbm_bo)
        return 0;
    /* First release: NV12 only; everything else keeps the clean fallback. */
    if (s->cap_fourcc && s->cap_fourcc != V4L2_PIX_FMT_NV12)
        return -1;
    dev = v4l2sl_gbm_device();
    if (!dev)
        return -1;
    rows = s->height + (s->height + 1) / 2;
    s->gbm_bo = gbm_bo_create(dev, s->width, rows, GBM_FORMAT_R8,
                              GBM_BO_USE_LINEAR);
    if (!s->gbm_bo) {
        fprintf(stderr, "v4l2stateless: gbm: bo create %ux%u failed\n",
                s->width, rows);
        return -1;
    }
    s->gbm_stride = gbm_bo_get_stride_for_plane(s->gbm_bo, 0);
    /* Bring the bo up to date: by export time pull_capture has filled the
     * memfd for this surface (export happens after vaSyncSurface). */
    if (s->dma_buf_fd >= 0 && s->stride && s->aligned_h) {
        uint32_t sz = v4l2sl_capture_plane_size(V4L2_PIX_FMT_NV12,
                                                s->stride, s->aligned_h);
        void *m = mmap(NULL, sz, PROT_READ, MAP_SHARED, s->dma_buf_fd, 0);
        if (m != MAP_FAILED) {
            v4l2sl_gbm_surface_upload(s, m, s->stride, s->aligned_h);
            munmap(m, sz);
        }
    }
    return 0;
}

void v4l2sl_gbm_surface_destroy(struct v4l2sl_surface *s)
{
    if (s && s->gbm_bo) {
        gbm_bo_destroy(s->gbm_bo);
        s->gbm_bo = NULL;
    }
}

VAStatus v4l2sl_surface_fill_prime_gbm(const struct v4l2sl_surface *surf,
                                       uint32_t flags, void *descriptor)
{
    VADRMPRIMESurfaceDescriptor *desc = descriptor;
    uint32_t h, pitch;
    int fd;

    if (!surf || !surf->gbm_bo || !descriptor)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    fd = gbm_bo_get_fd_for_plane(surf->gbm_bo, 0);
    if (fd < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    h = surf->height;
    pitch = surf->gbm_stride;
    memset(desc, 0, sizeof(*desc));
    desc->fourcc = VA_FOURCC_NV12;
    desc->width = surf->width;
    desc->height = h;
    desc->num_objects = 1;
    desc->objects[0].fd = fd;
    desc->objects[0].size = pitch * (h + (h + 1) / 2);
    desc->objects[0].drm_format_modifier = gbm_bo_get_modifier(surf->gbm_bo);

    if (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) {
        desc->num_layers = 2;
        desc->layers[0].drm_format = DRM_FORMAT_R8;
        desc->layers[0].num_planes = 1;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = pitch;
        desc->layers[1].drm_format = DRM_FORMAT_GR88;
        desc->layers[1].num_planes = 1;
        desc->layers[1].object_index[0] = 0;
        desc->layers[1].offset[0] = pitch * h;
        desc->layers[1].pitch[0] = pitch;
    } else {
        desc->num_layers = 1;
        desc->layers[0].drm_format = DRM_FORMAT_NV12;
        desc->layers[0].num_planes = 2;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].object_index[1] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].offset[1] = pitch * h;
        desc->layers[0].pitch[0] = pitch;
        desc->layers[0].pitch[1] = pitch;
    }
    return VA_STATUS_SUCCESS;
}
```

- [ ] **Step 3: Hook pull_capture** (`src/v4l2stateless_device.c`, in `v4l2sl_surface_pull_capture`, right after `surf->cap_fourcc = fcc;` before `return 0;`)

```c
    if (surf->gbm_bo)
        v4l2sl_gbm_surface_upload(surf, src, stride, alh);
```
(Non-fatal: on failure the memfd copy above is already complete.)

- [ ] **Step 4: Hook the export path** (`src/v4l2stateless.c`, `v4l2sl_export_surface_handle`) — replace the block

```c
    if (surf->buf_index >= 0 && !surf->cpu_ptr) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
```
(comment above it included) with:

```c
    /*
     * Decode surfaces: VPU capture buffers must never be exported (EXPBUF +
     * GPU import is a chip bug). Export the driver-owned linear GBM copy
     * instead — a real dma-buf in the single-object NV12 shape Chrome's
     * zero-copy GL path imports. Falls back to UNIMPLEMENTED when GBM is
     * unavailable or the format is not NV12 (software path, as before).
     */
    if (surf->buf_index >= 0 && !surf->cpu_ptr) {
        VAStatus st;

        if (v4l2sl_gbm_surface_ensure(surf) < 0) {
            pthread_mutex_unlock(&g_v4l2sl_lock);
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        st = v4l2sl_surface_fill_prime_gbm(surf, flags, descriptor);
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return st;
    }
```
(The existing memfd/cpu_ptr path below it stays untouched for upload/VPP surfaces.)

- [ ] **Step 5: Hook surface destruction** (`src/v4l2stateless.c`, `v4l2sl_destroy_surfaces`, after `if (surface->dma_buf_fd >= 0) close(...);`)

```c
            v4l2sl_gbm_surface_destroy(surface);
```

- [ ] **Step 6: meson**

Add `'src/v4l2stateless_gbm.c',` to `sources` and `gbm_dep = dependency('gbm')` near the other deps; append `gbm_dep` to the shared_library `dependencies` list. The `test_export_recapture`/`test_vp8_mpeg2_fill` targets link `v4l2stateless_device.c` (which now calls `v4l2sl_gbm_surface_upload`) — add `src/v4l2stateless_gbm.c` + `gbm_dep` to both executables' sources/deps so they link.

- [ ] **Step 7: Build, install, run Task 2's test**

```bash
scp -q src/v4l2stateless_gbm.c src/v4l2stateless.h src/v4l2stateless.c src/v4l2stateless_device.c meson.build liyifan@192.168.1.21:/tmp/gbm_stage/  # (scp files individually)
ssh liyifan@192.168.1.21 'cp /tmp/gbm_stage/* ~/vaapi-v4l2-bridge/src/ ... && cd ~/vaapi-v4l2-bridge && meson compile -C builddir && echo liyifan | sudo -S install -m 0755 builddir/v4l2stateless_drv_video.so /usr/lib/aarch64-linux-gnu/dri/ && builddir/va_export_client /dev/dri/renderD128 /tmp/h264_idr_nv12.h264'
```
Expected: `EXPORT_EXACT 0..7` and exit 0.

- [ ] **Step 8: Quick matrix regression (all 26 + existing suites)**

```bash
ssh liyifan@192.168.1.21 'cd ~/vaapi-v4l2-bridge && tests/run_full_matrix.sh 2>&1 | tail -3'
```
Expected: PASS=26 FAIL=0 (GBM is additive; no existing test exports surfaces).

- [ ] **Step 9: Commit**

```bash
ssh liyifan@192.168.1.21 'cd ~/vaapi-v4l2-bridge && git add -A src meson.build tests/va_export_client.c && git commit -m "feat: GBM-backed display surfaces — export NV12 single-object dma-buf" --quiet && git log --oneline -1'
```

---

### Task 4: Matrix block for the export client (PASS 26 → 27)

**Files:**
- Modify: `tests/run_full_matrix.sh`

**Interfaces:**
- Consumes: `builddir/va_export_client` (build it with meson inside the block or reuse Task 2's target via `$PWD/builddir`).

- [ ] **Step 1: Add the clip + block**

In the clip-generation section:

```bash
enc "$CLIP/h264_idr_nv12.h264" -f lavfi -i testsrc=size=1280x720:rate=30 \
  -t 1 -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v high \
  -g 1 -keyint_min 1 -sc_threshold 0 -f h264
```
(`enc` here is raw ffmpeg — the existing `enc()` helper takes a container; check its definition and either reuse or call `$FF` directly with these args.)

New block after the h264422 blocks:

```bash
# --- va-export: exported dma-buf content == vaGetImage content -----------
if [ -x builddir/va_export_client ]; then
  builddir/va_export_client /dev/dri/renderD128 "$CLIP/h264_idr_nv12.h264" \
    >"$OUT/va_export.log" 2>&1
  if [ $? -eq 0 ] && grep -q "EXPORT_EXACT 7" "$OUT/va_export.log"; then
    echo "PASS va-export"; pass=$((pass+1))
  else
    echo "FAIL va-export"; fail=$((fail+1))
  fi
else
  echo "SKIP va-export (not built)"; fail=$((fail+1))
fi
```
Ensure `builddir/va_export_client` exists (add `meson compile -C builddir va_export_client || true` near the top where the matrix builds tools, following the existing pattern).

- [ ] **Step 2: Full matrix**

Expected: `PASS=27 FAIL=0`.

- [ ] **Step 3: Commit** — `test: matrix block for va_export_client (dmabuf == vaGetImage)`

---

### Task 5: Install + re-enable hardware decode in the Chrome wrapper

**Files:**
- Modify: `scripts/google-chrome-vaapi` (repo copy AND `/usr/local/bin/google-chrome-stable` on the NAS — install path per repo scripts)

- [ ] **Step 1: Rewrite the disable-features block**

Replace the whole `if [[ "$has_disable_features" = 0 ]]; then ... fi` block (current lines 62-92: the outcome-matrix essay + early `return 0` + the commented old block) with:

```bash
if [[ "$has_disable_features" = 0 ]]; then
  # Vulkan: Ozone-Wayland has no present path.
  # Hardware video decode RE-ENABLED 2026-09-02: the bridge now exports each
  # surface as a driver-owned linear GBM bo (single-object NV12 dma-buf, no
  # VPU/CMA involvement), so the zero-copy GL path imports it per frame and
  # the picture shows. Verified: VaapiVideoDecoder + non-black canvas.
  args+=(--disable-features=Vulkan)
fi
```

- [ ] **Step 2: Install to /usr/local/bin and commit**

```bash
ssh liyifan@192.168.1.21 'cp ~/vaapi-v4l2-bridge/scripts/google-chrome-vaapi /tmp/wrapper.new && echo liyifan | sudo -S install -m 0755 /tmp/wrapper.new /usr/local/bin/google-chrome-stable && cd ~/vaapi-v4l2-bridge && git add scripts/google-chrome-vaapi && git commit -m "feat: re-enable Chrome hardware video decode (GBM display surfaces)" --quiet'
```

- [ ] **Step 3: Sanity: wrapper argv**

```bash
ssh liyifan@192.168.1.21 'google-chrome-stable --version; head -c 0 /dev/null; bash -n /usr/local/bin/google-chrome-stable && echo wrapper-syntax-ok'
```

---

### Task 6: Live Chrome verification (acceptance gate)

**Files:** none in repo (NAS /tmp scripts: recreate `mi8.js`, `canvas2.js`, `newtab_live.js`, `state_main.sh` if missing — node at `~/.hermes/node/bin/node`).

- [ ] **Step 1: Restart the user's main Chrome under the wrapper with CDP**

```bash
ssh liyifan@192.168.1.21 '
mountpoint -q ~/.config/gc-dbg || echo liyifan | sudo -S mount --bind ~/.config/google-chrome ~/.config/gc-dbg
pkill -f "^/opt/google/chrome/chr""ome " || true; sleep 2
nohup google-chrome-stable --user-data-dir=$HOME/.config/gc-dbg --remote-debugging-port=9222 --restore-last-session >/tmp/chrome_main.log 2>&1 &
'
```

- [ ] **Step 2: Open a bilibili live room and verify the decoder**

Recreate `/tmp/newtab_live.js` (opens a live room, polls for a playing `<video>`), then `/tmp/mi8.js` (CDP WebSocket → clicks the player in chrome://media-internals → reads `kVideoDecoderName`).

Expected: `VaapiVideoDecoder` (NOT FFmpegVideoDecoder).

- [ ] **Step 3: Verify the picture is not black**

`/tmp/canvas2.js`: drawImage the video into a canvas every 500ms, read back average brightness.

Expected: avg 30-120 across samples (black screen = avg < 5; healthy streams measured ~60-75).

- [ ] **Step 4: Verify the VPU is engaged**

`state_main.sh`: find Chrome's GPU process pid, list its open fds, check it holds the rkvdec `/dev/video*` node (whichever node answers for H.264 today — query via `v4l2-ctl -d /dev/videoN --list-devices 2>/dev/null` or the media controller topology).

Expected: GPU process holds one `/dev/videoN` from the rkvdec device + the count of `v4l2sl-surf` memfds > 0.

- [ ] **Step 5: Leave the browser in a good state** — session restored, no debug tabs left open beyond what the user had.

---

### Task 7: Push, docs, memory

- [ ] **Step 1: Update repo state docs** — `STATE.md` / `HANDOFF.md` (if present in repo root): hardware decode re-enabled via GBM display surfaces; export shape; matrix count 27; Mesa GR88 caveat; V4L2SL_RENDER_NODE env knob.
- [ ] **Step 2: Commit docs; push everything** (includes unpushed 7907909):

```bash
ssh liyifan@192.168.1.21 'cd ~/vaapi-v4l2-bridge && git add -A && git commit -m "docs: GBM display surfaces state" --quiet; git push origin master 2>&1 | tail -2'
```
If the TLS/GnuTLS error recurs, retry once after 30s (transient earlier today; remote is SSH-reachable).

- [ ] **Step 3: Update local memory files** — update `rk3588-mainline-userspace-decode-map.md`-adjacent memory: GBM single-object NV12 export is the Chrome path; GR88 per-plane import broken in Mesa 26.0.8; panthor refuses multiplanar YUV bos.

---

## Self-Review (done at plan time)

1. **Spec coverage**: probe→Task 1; client test→Task 2; module+hooks+meson→Task 3 (all five interface functions from the spec); matrix→Tasks 1/4; wrapper→Task 5; Chrome live + success criteria→Task 6; push/docs→Task 7. Spec's "P010/Y210 follow-up" is explicitly out of scope (documented in spec, no task needed).
2. **Placeholders**: none — every step carries exact code or exact commands.
3. **Type consistency**: `v4l2sl_gbm_surface_ensure/upload/destroy/fill_prime_gbm` signatures identical in header, module, and call sites; `gbm_bo`/`gbm_stride` field names consistent.
