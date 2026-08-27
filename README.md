# vaapi-v4l2-bridge

libva backend that translates VA-API decode to the Linux V4L2 Request API (stateless).  
Target: Rockchip RK3588 / Orange Pi 5 on **mainline** Armbian (no vendor BSP, no MPP).

Applications that only speak VA-API (`ffmpeg -hwaccel vaapi`, mpv, Firefox) can then use:

| Codec | Device | Status |
|---|---|---|
| H.264 Main / High | rkvdec `/dev/video1` | bit-exact vs ffmpeg software (`hwdownload` framemd5) |
| HEVC 8-bit Main | same | bit-exact, including WPP |
| AV1 Profile0 8-bit | hantro `/dev/video4` + matching media node | bit-exact on the 1080p test clip (I + P, 49 frames) |

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
  -vaapi_device /dev/dri/renderD128 -i FILE.mp4 \
  -vf "hwdownload,format=nv12" ...
```

H.264 Constrained Baseline is not advertised (Main/High only). HEVC Main10 is advertised but not verified. VP8/MPEG2 are stubs.

Regression script (needs the test clips and `/dev/video*`): `tests/vaapi_hwdownload.sh`.

## License

LGPL-2.1-or-later (same as libva).
