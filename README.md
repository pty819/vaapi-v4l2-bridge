# vaapi-v4l2-bridge

Last verified **2026-09-04** on Orange Pi 5 (Armbian 26.8.3 resolute, kernel 7.1.8-edge-rockchip64). Ops notes: [HANDOFF.md](HANDOFF.md). Snapshot: [STATE.md](STATE.md). Module map: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Generated Sphinx site (GitHub Pages, rebuilt on push): <https://pty819.github.io/vaapi-v4l2-bridge/> — sources under `docs/`, workflow `.github/workflows/docs.yml`.

libva backend that translates VA-API decode to the Linux V4L2 Request API (stateless).
Target: Rockchip RK3588 / Orange Pi 5 on **mainline** Armbian (no vendor BSP, no MPP).

Applications that only speak VA-API (`ffmpeg -hwaccel vaapi`, VLC, Firefox, official Linux Chrome) can then use the VPU. Desktop wiring (Chrome wrapper, Firefox `user.js`, VLC `avcodec-hw`) is in [APPS.md](APPS.md). Since 2026-09-03 official Chrome hardware-decodes **and shows the picture** — zero-copy GL path over GBM-backed export surfaces.

Codecs:

| Codec | Device (this RK3588) | Status |
|---|---|---|
| H.264 Constrained Baseline / Main / High | rkvdec `/dev/video1` | bit-exact vs ffmpeg software (`hwdownload` framemd5) |
| H.264 High10 | same, capture NV15 | bit-exact vs ffmpeg software (`hwdownload,format=p010le`) — driver strips the QpBdOffsetY depth offset ffmpeg bakes into `pic_init_qp` (VA carries it, V4L2 wants the raw bitstream value) |
| H.264 High422 8/10-bit | same, capture NV16/NV20 | bit-exact; VA path exercised by `tests/va_h264422_client.c` (stock ffmpeg never offers VAAPI for 4:2:2 — decoder pix_fmt list + profile map), kernel path GStreamer `v4l2slh264dec` vs `avdec_h264` |
| HEVC 8-bit Main (incl. WPP) | same | bit-exact |
| HEVC Main10 | same, capture NV15 → P010 | bit-exact vs ffmpeg software (`hwdownload,format=p010le`) |
| AV1 Profile0 8-bit | hantro `/dev/video4` + matching media node | bit-exact vs ffmpeg SW (libaom, libaom realtime, SVT-AV1 RA, 4K, default). **Will not decode** AV1 super-res (`superres_denom > 8`) — kernel/hantro limitation (driver derives coded width correctly; pictures still mismatch). libaom `-lossless 1` is 60/60 bit-exact. aomenc `--lossless=1 --enable-intrabc=1` 2-pass clip is **not** bit-exact on the ffmpeg-VA path (30/60 after shown frame 29; kernel accepts; the clip never sets `allow_intrabc`). Re-advertised 2026-09-03: the old "node reopen hangs the SoC" was an undersized PSU, not a driver bug. Since 2026-09-04 `refresh_frame_flags` is **parsed as truth from Chrome's submitted OBU span** (order-hint heuristics remain the raw-tile/ffmpeg fallback), and a **kernel-DPB-model copy-out release** bounds live capture buffers to ~the DPB instead of the player's decode-ahead queue — bilibili (WebCodecs, 5-min soak + seek storm) and YouTube (MSE-forced av01, 1080p60) both stay on AV1 with zero pool errors. Tile grids above 32 are refused (truncating hung the SoC). See `src/v4l2stateless_av1.c` |
| VP8 | hantro `/dev/video2` | bit-exact vs ffmpeg software |
| MPEG-2 Simple / Main | same `/dev/video2` | bit-exact vs GStreamer `v4l2slmpeg2dec` (hantro IDCT ≠ ffmpeg SW) |
| JPEG Baseline encode | VEPU121 `/dev/video3` | `mjpeg_vaapi` (stateful M2M) |
| VPP (scale / CSC / rotate / flip) | RGA `/dev/video0` | `scale_vaapi` (`VAProfileNone` + `VAEntrypointVideoProc`) |

Decoder nodes are selected by OUTPUT fourcc, not by a hardcoded `/dev/videoN`.

## Build / install

`meson.build` pins **`buildtype=release`** (`optimization=3`, `b_ndebug=if-release`).
A fresh `meson setup` compiles **`-O3 -DNDEBUG`** — not meson's default
`debug` (`-O0 -g`). Chrome and the matrix load the copy under
`/usr/lib/.../dri/`, so that install must come from this release build.

On the RK3588 host:

```bash
meson setup builddir          # release / -O3; do not pass -Dbuildtype=debug
ninja -C builddir
sudo cp -f builddir/v4l2stateless_drv_video.so \
  /usr/lib/aarch64-linux-gnu/dri/
export LIBVA_DRIVER_NAME=v4l2stateless
vainfo --display drm --device /dev/dri/renderD128
```

An **already-configured** `builddir` keeps whatever `buildtype` it was
set up with. If `meson configure builddir` shows `debug` / `optimization=0`,
reconfigure before ninja:

```bash
meson configure builddir -Dbuildtype=release -Doptimization=3 -Db_ndebug=if-release
ninja -C builddir
sudo cp -f builddir/v4l2stateless_drv_video.so \
  /usr/lib/aarch64-linux-gnu/dri/
```

Confirm the compile line has `-O3` (and typically `-DNDEBUG`, no `-g`):
`grep v4l2stateless.c builddir/compile_commands.json`.

## Use with ffmpeg

Success must be judged with `hwdownload`. A plain `-f framemd5` can silently fall back to software.

```bash
export LIBVA_DRIVER_NAME=v4l2stateless
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device /dev/dri/renderD128 -i FILE \
  -vf "hwdownload,format=nv12" -pix_fmt yuv420p -f framemd5 -
```

### Known client limitation: bilibili-downloaded AV1 via ffmpeg-based players

Decoding a **bilibili-downloaded AV1 file** with an ffmpeg-VA-API client
(`ffmpeg -hwaccel vaapi`, VLC, mpv — anything routed through libavcodec's VA
hwaccel) yields a corrupt picture on streams from bilibili's AV1 encoder.
The root cause sits outside this driver: ffmpeg submits only raw tile
payloads to VA-API and VA-API does not expose `refresh_frame_flags`, so no
driver can recover the reference-slot truth this encoder's policy needs.

The trigger is narrow — everything around it works:

- bilibili **web** playback in Chrome: WebCodecs submits the full OBU span
  and the driver parses the truth (soak-verified, 2026-09-04)
- playing the same downloaded file **in Chrome**: own decoder, same span
  submission — unaffected
- ffmpeg decoding **any other** AV1 encode (libaom, SVT-AV1, 4K): bit-exact
- ffmpeg **software** decode of the same file: correct (just no VPU)

Workarounds: play such files in Chrome, decode them in software, or use the
kernel-direct v4l2-request ffmpeg fork whose userspace parses OBU headers
itself.

### Known hardware/kernel limitation: AV1 super-res will not decode

AV1 **super-res** (`superres_scale_denominator > 8`, aomenc `--superres-mode=1`)
is not bit-exact on this RK3588 hantro node (`/dev/video4`, kernel 7.1.8-edge).
The driver now derives coded width from display width and the denominator
(`frame_width` vs `upscaled_width`); capture stays the DISPLAY size
(1280x720, not the ~853 coded width). After that, KEY frame 0 is bit-exact
and DQBUF errors disappear, but frames 1–59 still mismatch software. Phase A
closes this as a kernel/hardware limit, not a remaining userspace fill bug.
Non-superres libaom / SVT-AV1 streams are unaffected (SVT 60/60 still exact).

### Known limitation: aomenc 2-pass lossless+intrabc clip (ffmpeg VA)

ffmpeg libaom **lossless** (`-lossless 1`, `testsrc` 1280x720 60 frames) is bit-exact
vs software on this hantro node. The Phase A IntraBC clip — aomenc
`--lossless=1 --enable-intrabc=1` on `testsrc2`, 60 frames — is **not**:
hw rc=0, 0 DQBUF errors, 30/60 framemd5 mismatch from shown frame 29.
aomenc `--webm` on Ubuntu `aom-tools` 3.13.1-2/arm64 aborts with a buffer
overflow; the clip was encoded `--ivf` then remuxed. `--enable-intrabc`
was accepted, but the bitstream never sets `allow_intrabc` (IntraBC is
KEY/INTRA_ONLY-only; this is a 2-pass lag-35 inter pyramid). Driver already
forwards VA screen-content / integer-MV / IntraBC flags. A 10-frame all-intra
IntraBC probe was 10/10 bit-exact. This is the ffmpeg raw-tile refresh
heuristic on a high-lag GOP, not a missing userspace flag. Phase A closes
the item as documented; no C change.

Mid-stream resolution / bit-depth / chroma changes renegotiate capture (`STREAMOFF` / `S_FMT` / `REQBUFS`). Export: `vaExportSurfaceHandle` returns a single-object NV12 dmabuf from `VIDIOC_EXPBUF` of the VPU capture buffer (Chrome zero-copy, no capture→GBM memcpy). Opt out with `V4L2SL_EXPBUF_EXPORT=0` to restore the GBM copy. See [docs/EXPBUF-RETRY.md](docs/EXPBUF-RETRY.md).

Full host matrix (needs `/dev/video*` and writes clips under `verify/`, gitignored):

```bash
bash tests/run_full_matrix.sh
```

Last recorded host run: **PASS=32 FAIL=0** (2026-09-04, EXPBUF default on system dri; VPP GetImage fix, 8 unique scale hashes). Note the matrix decodes only the first 32 frames of the SVT clip — full-clip hw-vs-sw compare is the stronger AV1 check (`60/60` bit-exact). The script covers H.264 (CB/Main/High/B/all-P/slices/4K/QCIF/High10 p010le/High422 8+10-bit), HEVC (Main/WPP/4K/Main10 p010le), **AV1 (aom-8, aom-49, svt-32, 4K, default)**, VP8 (480+720), MPEG-2 vs GST (IP/B/1080), JPEG `mjpeg_vaapi`, RGA `scale_vaapi`, unit probe/fill, the `gbm-probe` and `va-export` clients, and `vainfo`.

## Desktop apps (Chrome / Firefox / VLC)

All of them need `LIBVA_DRIVER_NAME=v4l2stateless` in the **graphical** environment
(`~/.config/environment.d/90-libva.conf`), not only in an interactive shell.

| App | Extra |
|---|---|
| Official Chrome arm64 | Wrapper [`scripts/google-chrome-vaapi`](scripts/google-chrome-vaapi): render-node override, GPU-sandbox off, pure Wayland + ANGLE/GLES, Vulkan off, big-core pinning (`taskset 4-7` by default, `CHROME_PIN_CPUS` overrides); ZeroCopyGL must stay **enabled** — disabling it pushes VaapiVideoDecoder into the nonexistent ImageProcessor path and Chrome silently drops to FFmpeg software (details in [APPS.md](APPS.md)); hw decode + zero-copy picture verified 2026-09-03. Distro Chromium arm64 does **not** use this `.so`. |
| Firefox | [`scripts/firefox-vaapi-user.js`](scripts/firefox-vaapi-user.js) into the active profile. Use Mozilla's `.deb` repo, not Ubuntu's snap stub. |
| VLC | `avcodec-hw=vaapi` in `vlcrc`. |

Full steps, checks, and caveats: **[APPS.md](APPS.md)**.

## Status highlights (2026-09-04)

- **AV1 copy-out shipped (`8b5ea0c`)**: WebCodecs/MSE players queue one surface
  per buffered frame, which decoupled surface lifetime from capture-buffer usage
  and drained even the 40-slot pool in ~5s (silent HEVC fallback). Every decoded
  frame is already snapshotted at `pull_capture` (GBM bo for display surfaces,
  memfd otherwise), so the context now tracks slot→buffer with the same
  `refresh_frame_flags` submitted to the kernel and returns a buffer the moment
  its slot is overwritten. The slot model is armed **only by the OBU-parse
  truth** — heuristic-flag streams keep the legacy release, because recycling
  buffers per wrong flags exposes the kernel's equally-wrong slot table as
  corruption (proven on SVT during bring-up). Fixed en route: `surface_id` was
  never assigned (buf_owner map silently tracked surface 0), and the SVT
  heuristic stored the **absolute** order hint as the GOP length (first GOP
  masked it; later GOPs corrupted 20/60 frames — invisible to the 32-frame
  matrix window).
- **bilibili WebCodecs AV1 (`9a6a4ce`/`e136cd1`)**: BILIAV1's slot policy
  defeats every order-hint heuristic, so the driver walks all frame OBUs in the
  submitted span (4 candidate header layouts for the optional
  screen-content/integer-MV bits) and only adopts the parsed
  `refresh_frame_flags` when frame_type/show_frame/order_hint/primary_ref_frame
  all verify against the VA picture parameters. AV1 pool is 40 slots (hantro AV1
  capture does not eat CMA).

## Status highlights (2026-09-03)

- **AV1 re-advertised (branch `feat/av1-readvertise`)**: the profile is back in `vaQueryConfigProfiles`; the historical hang was PSU undersizing (since replaced), not kernel instability — 15x vainfo, 5 back-to-back decodes and two full matrices ran with zero runtime dmesg errors. Two Chrome-only incompatibilities fixed along the way: (1) the kernel wants **raw tile data** in the OUTPUT buffer while Chrome submits the whole OBU span with per-tile offsets — the driver now extracts tile payloads and rebases the offsets; (2) with `uniform_tile_spacing`, `width/height_in_sbs_minus_1` are **derived** from the mi-col/row grid (VA clients leave them zero there; the old unconditional copy contradicted the grid and the kernel rejected every frame). Chrome verified on a local AV1 file (zero DQBUF errors, correct picture) and on YouTube (AV1 segments during ABR adaptation decoded clean; 4500+ pictures, zero decode errors).
- **Stability round `27e8b7a` (2026-09-02)**: decode timeouts now `STREAMOFF` + rebuild both queues (a wedged job is never left in the kernel), every stateful vtable entry takes the driver lock, surface IDs recycled with bounds checks, probe results cached per boot (`v4l2stateless-probe.cache`, `V4L2SL_PROBE_NOCACHE=1` bypasses), capture `REQBUFS` degrades 24→8→4 under CMA pressure.
- **GBM display surfaces `f53f3f1..452de04`**: Chrome `VaapiVideoDecoder` hardware-decodes with a visible picture (bilibili 1080p live; canvas non-black, zero `eglCreateImage` errors, GPU process holds rkvdec). Platform constraints and the VPP-output-surface export model are documented in [STATE.md](STATE.md).
- **Audit-driven refactor merged `c359f91`** (full audit: [docs/refactor-audit-2026-09-03.md](docs/refactor-audit-2026-09-03.md)): P0–P2 done (terminate leak 11MB/session → 0, `create_surfaces` failure path, VPP region clamp), P3 clusters 1-6 and P4 items 1-5 landed — decode-path driver ioctls measured ~12 → ~7 per frame ([docs/perf-baseline-2026-09.md](docs/perf-baseline-2026-09.md)), lazy `cpu_ptr` saves 60–240MB per pool. The deferred tail (P3 clusters 7-11 + P4 items 6-8: persistent per-surface memfd mappings, persistent VPP/JPEG M2M queues, 64-byte stride alignment, shared Annex-B / buffer-collection / PRIME-layer / probe-walk helpers) landed the same night — decode path holds at ~7 ioctls/frame; browser-verified 8-bit HEVC hw decode came out of the 10-bit investigation. C12 (AV1 `update_grain`) stays blocked: the field is absent from libva 2.23.0 **and** upstream master.

## License

LGPL-2.1-or-later (same as libva).
