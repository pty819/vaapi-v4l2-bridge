# Architecture — vaapi-v4l2-bridge

This document is the **module map** for this repository. Capability tables
(what codecs pass, what Chrome can play) live in [README.md](../README.md)
and [STATE.md](../STATE.md). Desktop wiring lives in [APPS.md](../APPS.md).
Ops / landmines live in [HANDOFF.md](../HANDOFF.md).

Read this when you need to know **which `.c` file owns which job**, how a
VA-API call turns into a V4L2 request, and why Chrome and ffmpeg do not
share a pixel path.

Last aligned with the tree at `2d24a13` (2026-09-04). File names below are
the source of truth — if a sentence and the file disagree, the file wins.

---

## 1. What this binary is

A **libva DRM backend**: `v4l2stateless_drv_video.so`, installed next to
Mesa’s other `*_drv_video.so` files. libva loads it when
`LIBVA_DRIVER_NAME=v4l2stateless`.

It translates Intel’s **VA-API** (what ffmpeg, Chrome’s
`VaapiVideoDecoder`, Firefox, VLC speak) into Linux **V4L2 Request API**
stateless decode (and two **stateful M2M** devices: RGA VPP and VEPU
JPEG encode) on **mainline** RK3588 — no Rockchip MPP, no vendor BSP
userspace.

It is **not**:

- Chromium’s native V4L2 decoder (distro Chromium never `dlopen`s this
  `.so`; GPU maps tell the two paths apart).
- A kernel driver. The VPU nodes (`/dev/video1` rkvdec, `/dev/video4`
  hantro AV1, …) already exist. This is userspace glue.
- An HDR compositor. Decode 10-bit ≠ HDMI HDR; see STATE.md.

meson builds one static library `libv4l2sl_core` from every `src/*.c`,
then `link_whole`s it into the `.so`. Tests link the same static lib so
codec translators cannot be dropped as “unused”. Default **buildtype is
`release` (`-O3 -DNDEBUG`)** — see README Build / install. A leftover
`builddir` created before that pin may still be meson `debug` (`-O0 -g`);
reconfigure it or you will keep installing an unoptimized `.so`.

---

## 2. How a client finds it

```
ffmpeg / Chrome / Firefox
        │  vaInitialize (DRM, /dev/dri/renderD128)
        ▼
libva  ──dlopen──►  v4l2stateless_drv_video.so
                    __vaDriverInit_1_20
                    fills VADriverVTable
        │
        ▼
vaCreateConfig / vaCreateContext / vaBeginPicture
vaRenderPicture / vaEndPicture / vaSyncSurface
vaExportSurfaceHandle  or  vaGetImage / vaDeriveImage
```

`v4l2sl_init` in `src/v4l2stateless.c` is the only entry. It:

1. Allocates `struct v4l2sl_driver_data`.
2. Calls `v4l2sl_scan_all_cached()` (`src/v4l2stateless_probe.c`) so
   each codec is bound to a `/dev/videoN` by **OUTPUT fourcc**, not by a
   hardcoded number.
3. Installs the vtable (`vaEndPicture`, `vaExportSurfaceHandle`, …).
4. Reads `V4L2SL_DEBUG` **once** into `v4l2sl_debug`.

Every stateful vtable function takes `g_v4l2sl_lock`. `vaEndPicture`
holds it across the whole synchronous decode (up to the 3 s poll
timeout). The driver is one-request-in-flight per context.

---

## 3. Object model

Four object kinds, all owned by `v4l2sl_driver_data`:

| Object | Created by | Lives in | Job |
|---|---|---|---|
| **config** | `vaCreateConfig` | linked list | Profile + entrypoint + codec enum + which `/dev/video*` |
| **surface** | `vaCreateSurfaces` | `surfaces[id]` table (4096, IDs recycled) | One picture slot: geometry, pixel backing, optional capture-buffer index |
| **context** | `vaCreateContext` | linked list | One decode/VPP/JPEG session: device fds, request fd, OUTPUT/CAPTURE pools, codec private state |
| **buffer** | `vaCreateBuffer` | context list or `orphan_buffers` | VA parameter / slice / image memory the client filled |

IDs come from the client. Every `surfaces[id]` access goes through
`v4l2sl_surface_by_id()` (bounds-checked). Pool pushes
(`v4l2sl_cap_pool_push` / `v4l2sl_out_pool_push`) refuse duplicates and
refuse writes past the array — a leaked index is cheaper than memory
corruption.

A **context** is one codec. H.264 and HEVC may share `/dev/video1`
(rkvdec) as **separate opens**; AV1 is `/dev/video4`. JPEG and VPP are
different nodes and a different queue model (section 6).

---

## 4. Source map

### 4.1 VA-API front (`src/v4l2stateless.c`)

Owns: init/terminate, vtable, configs, surfaces, contexts, VA buffers,
`vaBeginPicture` / `vaRenderPicture` / `vaEndPicture` dispatch,
`vaSyncSurface`, `vaGetImage` / `vaPutImage` / `vaDeriveImage`,
`vaExportSurfaceHandle`, format/profile advertisement.

Does **not** talk V4L2 except by calling helpers. Codec-specific
bit-packing is never in this file.

`vaEndPicture` branches:

- JPEG → `v4l2sl_jpeg_encode`
- VPP → `v4l2sl_vpp_run`
- else → `v4l2sl_{h264,hevc,av1,vp8,mpeg2}_translate`

### 4.2 V4L2 request engine (`src/v4l2stateless_device.c`)

Owns the **stateless decode** ioctl sequence used by every video codec:

open video + sibling media node, `MEDIA_IOC_REQUEST_ALLOC`, OUTPUT /
CAPTURE `REQBUFS` + mmap, `S_EXT_CTRLS` (request-scoped and global),
`QBUF` OUTPUT (with request), `QBUF` CAPTURE (bare — the spec forbids
CAPTURE in a request), `MEDIA_REQUEST_IOC_QUEUE`, poll, `DQBUF`,
`MEDIA_REQUEST_IOC_REINIT`.

Hot-path helper `v4l2sl_decode_submit(ctx, out_idx, bytesused, timestamp)`
is what every `*_translate` calls after it has packed controls and copied
the bitstream into an OUTPUT slot.

Also owns:

- `v4l2sl_ensure_capture` — mid-stream renegotiate (`STREAMOFF` / `S_FMT`
  / `REQBUFS`) when resolution, bit-depth or chroma changes. Resets the
  AV1 DPB model because STREAMOFF drops kernel references.
- `v4l2sl_surface_pull_capture` — mmap the finished CAPTURE buffer and
  **snapshot** it (GBM bo if the surface is a display surface, else memfd).
- Capture / output pool accounting.

**Default:** `VIDIOC_EXPBUF` of the VPU capture buffer (Chrome zero-copy).
The pre-PSU hang that banned this is documented in
[EXPBUF-RETRY.md](EXPBUF-RETRY.md); after the PSU swap the path is the
shipping default. `V4L2SL_EXPBUF_EXPORT=0` restores the GBM copy.

### 4.3 Device discovery (`src/v4l2stateless_probe.c` + `.h`)

Scans `/dev/video0..63`, matches **OUTPUT_MPLANE fourcc**
(`H264_SLICE`, `HEVC_SLICE`, `AV1_FRAME`, `VP8_FRAME`, `MPEG2_SLICE`,
JPEG, RGA). Skips nodes whose sibling media device cannot
`MEDIA_IOC_REQUEST_ALLOC` (AV1 stub nodes on this SoC).

Result is cached under `$XDG_RUNTIME_DIR` keyed by `boot_id` so Chrome /
vainfo / Firefox do not reopen every VPU node on every `vaInitialize`.
`V4L2SL_PROBE_NOCACHE=1` forces a rescan.

### 4.4 Pixel format helpers (`src/v4l2stateless_format.c`)

NV12 copies, NV15→P010, NV16→YUY2, NV20→YUY2/Y210, stride/sizeimage
math, VA fourcc ↔ V4L2 fourcc. Used by `vaGetImage` / `vaDeriveImage`,
not by the zero-copy GL path.

### 4.5 Display copy (`src/v4l2stateless_gbm.c`)

Chrome’s zero-copy path. Each exported surface gets a **linear GBM bo**
on `/dev/dri/renderD128` (panthor shmem — ordinary system memory, not
CMA).

Constraints this file exists to satisfy:

- panthor refuses multiplanar YUV bos → bo is `GBM_FORMAT_R8` with
  `h + ceil(h/2)` rows; NV12 planes are **byte offsets** inside it.
- Chromium rejects `num_objects != 1` → one dma-buf, two planes in the
  descriptor (`Y@0`, `UV@stride*h`).
- Mesa GR88-only import samples black → never hand that out as the
  primary shape.

**NV12 only.** Anything else (`NV15` / P010 / 4:2:2) returns failure and
the client falls back; there is no R16/P010 bo path.

`pull_capture` uploads VPU pixels into this bo. `vaExportSurfaceHandle`
returns the bo’s fd via `v4l2sl_surface_fill_prime_gbm`.

### 4.6 Codec translators (one file per bitstream)

Same job, different UAPI structs: read VA picture/slice buffers, fill
`v4l2_ctrl_*`, copy payload into OUTPUT, call `v4l2sl_decode_submit`,
`pull_capture`.

| File | VA profile(s) | V4L2 coded fourcc | Kernel node (this board) | Notes |
|---|---|---|---|---|
| `v4l2stateless_h264.c` | Constrained Baseline / Main / High / High10 / High422 | `H264_SLICE` | rkvdec `/dev/video1` | Strips ffmpeg’s QpBdOffsetY from `pic_init_qp` (VA carries depth offset, V4L2 wants the raw bitstream value). High10 capture NV15. |
| `v4l2stateless_hevc.c` | Main / Main10 | `HEVC_SLICE` | same rkvdec | WPP. Capture may start NV12 then renegotiate NV15 after SPS. STREAMON is deferred until geometry is known. |
| `v4l2stateless_av1.c` | Profile0 8-bit | `AV1_FRAME` | hantro `/dev/video4` + `/dev/media3` | See section 8. Sequence control is **global** (request-scoped succeeds the ioctl but leaves the device unconfigured). Tile grids > 32 are refused. |
| `v4l2stateless_vp8.c` | VP8 0–3 | `VP8_FRAME` | hantro `/dev/video2` | ffmpeg’s VA payload has the uncompressed header stripped; hantro `cfg_parts()` still skips it — driver adds the header length back into `first_part_size`. |
| `v4l2stateless_mpeg2.c` | Simple / Main | `MPEG2_SLICE` | same `/dev/video2` | One slice buffer per MB row (hence `pending_buffers[256]`). Bit-exact vs GStreamer, not vs ffmpeg SW (hantro IDCT). |

### 4.7 Stateful M2M (`src/v4l2stateless_jpeg.c`, `src/v4l2stateless_vpp.c`)

**Not** the request API. These devices (VEPU JPEG `/dev/video3`, RGA
`/dev/video0`) are classic OUTPUT+CAPTURE M2M: `S_FMT` / `S_CTRL` /
`REQBUFS` / mmap once, then per-frame `QBUF`+`DQBUF`.

`struct v4l2sl_m2m_state` on the context remembers the setup key. Unchanged
setup → skip renegotiate (steady state ~7 ioctls, 0 mmap). JPEG is encode
only (`VAEntrypointEncPicture`). VPP is `VAProfileNone` +
`VAEntrypointVideoProc` (`scale_vaapi`).

---

## 5. One decoded frame (stateless path)

Happy path after `vaCreateConfig` + `vaCreateContext` + `vaCreateSurfaces`:

```
vaBeginPicture(surface)
    context->current_surface = that surface
    if the surface still held a capture index:
        H.264/HEVC/VP8/MPEG2: push index back to the free pool
        AV1 + model_active: do NOT push — DPB model owns release
    timestamp = frame_count++ * 1000   (µs, matches vb2)

vaRenderPicture(param buffers…)
    stash pointers in context->pending_buffers[]

vaEndPicture
    *_translate:
        collect pic / slice / data roles
        fill sequence (global, skipped if payload unchanged)
        fill frame + codec extras (request-scoped S_EXT_CTRLS)
        ensure_capture(width, height, fourcc)
        STREAMON if needed
        pop OUTPUT slot, memcpy bitstream (AV1: raw tiles only)
        v4l2sl_decode_submit
            pop CAPTURE slot
            QBUF OUTPUT (request) + QBUF CAPTURE (bare)
            QUEUE request, poll ≤ 3s, DQBUF both
            REINIT request (not close+realloc)
        pull_capture → snapshot into GBM bo or memfd
        AV1: maybe av1_release_unrefd (slot overwrite)

vaSyncSurface
    no-op wait: decode already finished inside EndPicture
    surface->status = Ready

then the client either:
    vaExportSurfaceHandle  → GBM dma-buf  (Chrome GL)
    vaDeriveImage/GetImage → memfd map    (ffmpeg hwdownload)
```

Failure policy that is easy to get wrong:

- Kernel `V4L2_BUF_FLAG_ERROR` → translator returns success with
  `VASurfaceSkipped`. A failed `vaEndPicture` is **cached by Chrome for
  the whole session** (no AV1 retry until relaunch).
- Decode timeout / QUEUE failure → `v4l2sl_decode_reset` (STREAMOFF both
  queues, drop DPB model). Do not leak the OUTPUT index back onto the
  pool — the kernel still owns it until reset.

---

## 6. Two device classes

```
                    vaEndPicture
                         │
          ┌──────────────┼──────────────┐
          ▼              ▼              ▼
     JPEG encode     VPP (RGA)     video codecs
     jpeg.c          vpp.c         h264/hevc/av1/vp8/mpeg2
          │              │              │
     stateful M2M   stateful M2M   stateless Request API
     VEPU video3    RGA video0     device.c
                                   rkvdec / hantro
```

Mixing the two models in one function is a bug. Request API: one
request fd, CAPTURE not in the request, REINIT every picture. M2M: no
request fd, STREAMON stays up, setup cached in `v4l2sl_m2m_state`.

---

## 7. Where pixels live (`last_writer`)

A surface can hold up to three backings. `enum v4l2sl_last_writer`
says which one has the freshest bytes:

| Writer | Backing | Who produces it | Who consumes it |
|---|---|---|---|
| **BO** | GBM bo (R8, NV12 layout) | `pull_capture` upload, or `gbm_surface_sync` | Chrome `vaExportSurfaceHandle` |
| **MEMFD** | grow-only memfd | `pull_capture` when there is no bo; or `ensure_memfd` refilling from the bo | `vaGetImage` / `vaDeriveImage` / VPP source |
| **CPU** | `cpu_ptr` malloc | `vaPutImage`, VPP dest | JPEG encode source, VPP |

`has_pic` means “a decoded snapshot exists”. After AV1 copy-out the
capture **index** is released (`buf_index = -1`) but `has_pic` stays
set, so GetImage still works. Do not gate “decoded-ness” on
`buf_index >= 0`.

Lazy memfd: if the bo is the snapshot, the memfd is **stale**
(`last_writer == BO`). The first CPU reader calls
`v4l2sl_surface_ensure_memfd` and copies bo → memfd. Chrome never does
that (it never GetImages).

```
VPU CAPTURE (CMA)
        │  EXPBUF (default) ──Export──► GPU (EGL image)
        │
        └─ GetImage/Derive reads capture mmap (no GBM copy)
           V4L2SL_EXPBUF_EXPORT=0 restores GBM memcpy snapshot
```

---

## 8. AV1 extras (why `v4l2stateless_av1.c` is large)

Three problems VA-API does not solve for AV1, all in this file:

**Bitstream shape.** Kernel OUTPUT wants **raw tile bytes**. ffmpeg
submits that. Chrome submits a whole OBU span (sequence + frame) with
per-tile offsets into the span. The translator extracts tiles and
rebases `TILE_GROUP_ENTRY` offsets. `uniform_tile_spacing`:
`width/height_in_sbs_minus_1` are **derived** from the MI grid — Chrome
leaves them zero.

**`refresh_frame_flags`.** VA does not expose it. Two-level policy:

1. Walk frame OBUs in the submitted span (dav1d field order, four
   candidate layouts for optional screen-content bits). Adopt the parsed
   mask only if `frame_type` / `show_frame` / `order_hint` /
   `primary_ref_frame` match the VA picture parameters. Success **arms**
   `ctx->av1.model_active`.
2. Else heuristic (libaom RTC / SVT / libaom), used by ffmpeg raw-tile
   submissions. **Not** trusted to recycle capture buffers.

**Copy-out / DPB model** (`av1_release_unrefd`). After `pull_capture`
the capture buffer’s only remaining job is “kernel DPB reference”.
Slots are tracked with the **same** refresh mask submitted to the
kernel; overwrite → `cap_pool_push`. WebCodecs queues one surface per
buffered frame — without this, even a 40-slot pool dies in ~5 s.

Trust gate: the model is the **only** releaser while `model_active`.
`vaBeginPicture` re-target and surface destroy must not also push.
ffmpeg heuristic streams keep the legacy “buffer stuck to the surface”
release.

**Hard refuse:** `tile_cols * tile_rows > 32` or more than 32 VA slice
params → `VA_STATUS_ERROR_INVALID_PARAMETER` **before any ioctl**.
Truncating to 32 while still submitting a 40-tile grid hung the hantro
node (SoC reset). Movie streams do not hit this. Do not VA-decode
`verify/clips/av1_40tiles.mp4`.

Sequence controls are **global** (`v4l2sl_set_global_controls`) and
memcmp-cached in `ctx->g_ctrl_payload`. Request-scoped sequence ioctls
look successful and then OUTPUT `QBUF` returns `EINVAL`.

Film grain + frame + tile-group go out in **one** request
`S_EXT_CTRLS` (controls built before the OUTPUT slot is popped).

---

## 9. How clients take pixels

```
                    surface with has_pic
                           │
          ┌────────────────┼────────────────┐
          ▼                ▼                ▼
 vaExportSurfaceHandle  vaDeriveImage   vaGetImage
   GBM PRIME (NV12)     map memfd        convert + copy
   Chrome zero-copy     Firefox          ffmpeg hwdownload
   num_objects == 1     native layout    NV15→P010 etc.
```

Chrome: wrapper `scripts/google-chrome-vaapi` forces Wayland + ANGLE
GLES, Vulkan **off**, `--render-node-override=/dev/dri/renderD128`,
GPU sandbox off (needs `/dev/videoN`), big-core pin. Distro Chromium
uses V4L2 directly and will not load this `.so`.

ffmpeg: `LIBVA_DRIVER_NAME=v4l2stateless` and **always** judge with
`hwdownload`. A plain framemd5 can silently be software. High10/Main10
work here because they never need GBM.

Firefox: system ffmpeg VA-API (`user.js` in `scripts/`). Same GetImage
path. Do not set `media.hardware-video-decoding.force-enabled` (VP9 /
10-bit HDR has hung this VPU).

---

## 10. Tests and scripts

| Path | Role |
|---|---|
| `tests/run_full_matrix.sh` | Host gate. Loads the **system** `.so`. PASS=32 is the green state. AV1 SVT case only decodes 32 frames — full-clip hw-vs-sw is the stronger check. |
| `tests/test_video_probe.c` | Fourcc → node picking, stub AV1 nodes, swapped `/dev/videoN`. Links probe.c only. |
| `tests/test_vp8_mpeg2_fill.c` | Header packing vs golden fields (no device). |
| `tests/test_format.c` | NV15/NV20 converters. |
| `tests/test_export_recapture.c` | Export + recapture without leaking pool indexes. |
| `tests/gbm_probe.c` | Platform gate for the R8/NV12 GBM lie. |
| `tests/va_h264422_client.c` | Stock ffmpeg never offers VAAPI for 4:2:2; this client does. |
| `tests/ioctl_interpose.c` | `LD_PRELOAD` to see whether Chrome imported a GBM dmabuf or a memfd. |
| `scripts/google-chrome-vaapi` | Installed as `/usr/local/bin/google-chrome-stable`. |

Install loop that the matrix actually tests (release / `-O3` `builddir`):

```
ninja -C builddir
sudo cp -f builddir/v4l2stateless_drv_video.so \
  /usr/lib/aarch64-linux-gnu/dri/
bash tests/run_full_matrix.sh
```

Editing `src/` without that `cp` tests yesterday’s `.so`. Compiling
`-O0` then copying that artifact is how the dri copy stayed debug
until 2026-09-04.

---

## 11. Lock, time, and session stickiness

- One process-global mutex. Do not add a second lock ordering.
- Decode is synchronous; `vaSyncSurface` does not wait on the VPU.
- Chrome caches the first VA failure for the **session**. After a bad
  `.so` or a failed AV1 entrypoint, relaunch Chrome. Never `pkill -9`
  (profile corruption). SIGTERM, wait ~5 s.
- `gc-dbg` is a **bind mount** of `~/.config/google-chrome`. Check
  `mount | grep gc-dbg` before claiming the profile is gone.

---

## 12. Composition diagram

```
                    ┌─────────────────────────────────────────┐
                    │           v4l2stateless.c               │
                    │  vtable · configs · surfaces · lock     │
                    │  Begin/Render/EndPicture · GetImage     │
                    │  ExportSurfaceHandle                    │
                    └───────────┬───────────────┬─────────────┘
                                │               │
              codec translate   │               │  JPEG / VPP
        ┌───────────┬───────────┼───────┐       │
        ▼           ▼           ▼       ▼       ▼
     h264.c      hevc.c      av1.c   vp8.c   jpeg.c
     mpeg2.c                         mpeg2   vpp.c
        │           │           │       │       │
        └───────────┴─────┬─────┴───────┘       │
                          ▼                     ▼
                 v4l2stateless_device.c    M2M STREAMON
                 request QUEUE / poll      S_FMT cached
                          │
                          ▼
                 pull_capture snapshot
                    ┌─────┴──────┐
                    ▼            ▼
              gbm.c (NV12)   memfd (any)
              Chrome EGL     ffmpeg GetImage
                    │
                    ▼
              format.c converters (P010 / YUY2) only on CPU path

     probe.c ──scan /dev/video*──► driver_data.dev_* paths
     format.c also: stride / fourcc tables used at CreateSurfaces
```

---

## 13. Invariants (break these and you get a hang or a silent SW fallback)

1. EXPBUF of VPU capture is the shipping default; `V4L2SL_EXPBUF_EXPORT=0` restores the GBM copy.
2. Never submit AV1 sequence controls as request-scoped.
3. Never truncate an AV1 tile grid and still tell the kernel the original
   `tile_cols`/`tile_rows`.
4. Never `pkill -9` Chrome on this host.
5. Never judge ffmpeg success without `hwdownload`.
6. Chrome: keep `AcceleratedVideoDecodeLinuxZeroCopyGL` **on**. Disabling
   it pushes VaapiVideoDecoder into a nonexistent ImageProcessor path and
   the session stays on FFmpeg software.
7. AV1 DPB-model release and legacy pool-push must not both run for the
   same buffer.
8. `surface_id` is assigned at create (calloc zero used to make every
   `buf_owner[]` look like surface 0).

When adding a codec or a pixel format, pick a row in section 4 and a
backing in section 7 first — then write the translator. Do not grow
`v4l2stateless.c` with bitstream parsing.
