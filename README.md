# vaapi-v4l2-bridge

libva backend that translates VA-API decode to the Linux V4L2 Request API (stateless).
Target: Rockchip RK3588 / Orange Pi 5 on **mainline** Armbian (no vendor BSP, no MPP).

Applications that only speak VA-API (`ffmpeg -hwaccel vaapi`, mpv, Firefox) can then use:

| Codec | Device (this RK3588) | Status |
|---|---|---|
| H.264 Constrained Baseline / Main / High | rkvdec `/dev/video1` | bit-exact vs ffmpeg software (`hwdownload` framemd5) |
| HEVC 8-bit Main (incl. WPP) | same | bit-exact |
| AV1 Profile0 8-bit | hantro `/dev/video4` + matching media node | bit-exact vs ffmpeg software (libaom, libaom realtime, SVT-AV1 RA) |
| VP8 | hantro `/dev/video2` | bit-exact vs ffmpeg software |
| MPEG-2 Simple / Main | same `/dev/video2` | bit-exact vs GStreamer `v4l2slmpeg2dec` (hantro IDCT ≠ ffmpeg SW) |

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

HEVC Main10 is advertised but not verified. Mid-stream resolution changes are not renegotiated.

Full host matrix (needs `/dev/video*` and writes clips under `verify/`, gitignored):

```bash
bash tests/run_full_matrix.sh
```

On Orange Pi 5 (kernel 7.1.8-edge-rockchip64) this matrix is **22/22**: H.264 (CB/Main/High/B/all-P/slices/4K/QCIF), HEVC (Main/WPP/4K), AV1 (libaom 8+49, SVT 32, 4K, default 16), VP8 (480+720), MPEG-2 vs GST (IP/B/1080), plus unit probe/fill and `vainfo`.

## License

LGPL-2.1-or-later (same as libva).
