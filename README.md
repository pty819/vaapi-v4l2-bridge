# vaapi-v4l2-bridge

Last verified **2026-08-29** on Orange Pi 5 (Armbian 26.8.3, kernel 7.1.8-edge-rockchip64). Ops notes: [HANDOFF.md](HANDOFF.md). Snapshot: [STATE.md](STATE.md).

libva backend that translates VA-API decode to the Linux V4L2 Request API (stateless).
Target: Rockchip RK3588 / Orange Pi 5 on **mainline** Armbian (no vendor BSP, no MPP).

Applications that only speak VA-API (`ffmpeg -hwaccel vaapi`, VLC, Firefox, official Linux Chrome) can then use the VPU. Desktop wiring (Chrome wrapper, Firefox `user.js`, VLC `avcodec-hw`) is in [APPS.md](APPS.md).

Codecs:

| Codec | Device (this RK3588) | Status |
|---|---|---|
| H.264 Constrained Baseline / Main / High | rkvdec `/dev/video1` | bit-exact vs ffmpeg software (`hwdownload` framemd5) |
| H.264 High10 | same, capture NV15 | advertised; capture renegotiates to NV15 (`vaExportSurfaceHandle` PRIME) |
| H.264 High422 | same, capture NV16 | advertised (ffmpeg's vaapi hwaccel often still picks software for 4:2:2) |
| HEVC 8-bit Main (incl. WPP) | same | bit-exact |
| HEVC Main10 | same, capture NV15 → P010 | bit-exact vs ffmpeg software (`hwdownload,format=p010le`) |
| AV1 Profile0 8-bit | hantro `/dev/video4` + matching media node | bit-exact vs ffmpeg software (libaom, libaom realtime, SVT-AV1 RA) |
| VP8 | hantro `/dev/video2` | bit-exact vs ffmpeg software |
| MPEG-2 Simple / Main | same `/dev/video2` | bit-exact vs GStreamer `v4l2slmpeg2dec` (hantro IDCT ≠ ffmpeg SW) |
| JPEG Baseline encode | VEPU121 `/dev/video3` | `mjpeg_vaapi` (stateful M2M) |
| VPP (scale / CSC / rotate / flip) | RGA `/dev/video0` | `scale_vaapi` (`VAProfileNone` + `VAEntrypointVideoProc`) |

Decoder nodes are selected by OUTPUT fourcc, not by a hardcoded `/dev/videoN`.

## Build / install

On the RK3588 host:

```bash
meson setup builddir
ninja -C builddir
sudo cp -f builddir/v4l2stateless_drv_video.so \
  /usr/lib/aarch64-linux-gnu/dri/
export LIBVA_DRIVER_NAME=v4l2stateless
vainfo --display drm --device /dev/dri/renderD128
```

## Use with ffmpeg

Success must be judged with `hwdownload`. A plain `-f framemd5` can silently fall back to software.

```bash
export LIBVA_DRIVER_NAME=v4l2stateless
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device /dev/dri/renderD128 -i FILE \
  -vf "hwdownload,format=nv12" -pix_fmt yuv420p -f framemd5 -
```

Mid-stream resolution / bit-depth / chroma changes renegotiate capture (`STREAMOFF` / `S_FMT` / `REQBUFS`). Zero-copy: `vaExportSurfaceHandle` DRM PRIME 2.

Full host matrix (needs `/dev/video*` and writes clips under `verify/`, gitignored):

```bash
bash tests/run_full_matrix.sh
```

Last recorded host run: **PASS=22 FAIL=0**. The script covers H.264 (CB/Main/High/B/all-P/slices/4K/QCIF), HEVC (Main/WPP/4K/Main10 p010le), AV1 (libaom 8+49, SVT 32, 4K, default 16), VP8 (480+720), MPEG-2 vs GST (IP/B/1080), JPEG `mjpeg_vaapi`, RGA `scale_vaapi`, plus unit probe/fill and `vainfo`.


## Desktop apps (Chrome / Firefox / VLC)

All of them need `LIBVA_DRIVER_NAME=v4l2stateless` in the **graphical** environment
(`~/.config/environment.d/90-libva.conf`), not only in an interactive shell.

| App | Extra |
|---|---|
| Official Chrome arm64 | Wrapper [`scripts/google-chrome-vaapi`](scripts/google-chrome-vaapi): render-node override, GPU-sandbox off, pure Wayland + ANGLE/GLES, Vulkan off (details in [APPS.md](APPS.md)); hw decode + zero-copy picture verified 2026-09-03. Distro Chromium arm64 does **not** use this `.so`. |
| Firefox | [`scripts/firefox-vaapi-user.js`](scripts/firefox-vaapi-user.js) into the active profile. Use Mozilla's `.deb` repo, not Ubuntu's snap stub. |
| VLC | `avcodec-hw=vaapi` in `vlcrc`. |

Full steps, checks, and caveats: **[APPS.md](APPS.md)**.

## License

LGPL-2.1-or-later (same as libva).
