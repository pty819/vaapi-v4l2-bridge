#!/bin/bash
# Host smoke: meson units + vainfo + live picker + 2-frame HW paths.
# Not the full framemd5 matrix and not a browser test.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export LIBVA_DRIVER_NAME=v4l2stateless
FF=${FF:-/usr/bin/ffmpeg}
CLIP=${CLIPDIR:-$ROOT/verify/clips}
OUT=${OUTDIR:-$ROOT/verify/smoke}
mkdir -p "$OUT"
fail=0
pass=0

echo "===== meson units ====="
if [ -x "$ROOT/builddir/test_video_probe" ]; then
  if meson test -C "$ROOT/builddir" --print-errorlogs; then
    echo "MESON_OK"; pass=$((pass+1))
  else
    echo "MESON_FAIL"; fail=$((fail+1))
  fi
else
  echo "MESON_FAIL no builddir"; fail=$((fail+1))
fi

echo "===== live probe ====="
if "$ROOT/builddir/test_video_probe" --live | tee "$OUT/live-probe.log"; then
  echo "LIVE_OK"; pass=$((pass+1))
else
  echo "LIVE_FAIL"; fail=$((fail+1))
fi

echo "===== vainfo ====="
vainfo --display drm --device /dev/dri/renderD128 >"$OUT/vainfo.log" 2>&1 || true
if grep -q "v4l2stateless/vaapi-v4l2-bridge" "$OUT/vainfo.log"; then
  echo "VAINFO_DRIVER_OK"; pass=$((pass+1))
  for p in VAProfileH264High VAProfileHEVCMain VAProfileAV1Profile0 \
           VAProfileVP8Version0_3 VAProfileMPEG2Main VAProfileJPEGBaseline; do
    if grep -q "$p" "$OUT/vainfo.log"; then
      echo "VAINFO_OK $p"; pass=$((pass+1))
    else
      echo "VAINFO_MISS $p"; fail=$((fail+1))
    fi
  done
else
  echo "VAINFO_FAIL"; cat "$OUT/vainfo.log"; fail=$((fail+1))
fi

used_hw() {
  grep -q "v4l2stateless: .* config uses /dev/video" "$1"
}

forbidden() {
  grep -E "hardware accelerator failed|Failed to query surface attributes|hwaccel initialisation returned error" "$1"
}

smoke_dec() {
  local src="$1" tag="$2" n="${3:-2}"
  if [ ! -f "$src" ]; then
    echo "SKIP $tag (no $src)"; return
  fi
  echo "== DEC $tag $src n=$n =="
  $FF -hide_banner -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device /dev/dri/renderD128 -i "$src" \
    -vf "hwdownload,format=nv12" -frames:v "$n" \
    -f null - >"$OUT/${tag}.stderr" 2>&1
  local he=$?
  if [ "$he" -ne 0 ]; then
    echo "HW_EXIT $he $tag"; tail -20 "$OUT/${tag}.stderr"; fail=$((fail+1)); return
  fi
  if forbidden "$OUT/${tag}.stderr" >/dev/null; then
    echo "FORBIDDEN $tag"; fail=$((fail+1)); return
  fi
  if ! used_hw "$OUT/${tag}.stderr"; then
    echo "NO_HW $tag"; fail=$((fail+1)); return
  fi
  echo "DEC_OK $tag"; pass=$((pass+1))
}

echo "===== 2-frame HW decode ====="
smoke_dec "$CLIP/h264_qcif.mp4" h264-qcif 2
smoke_dec "$CLIP/hevc_main.mp4" hevc-main 2
smoke_dec "$CLIP/av1_default.mp4" av1-default 2
smoke_dec "$CLIP/vp8_480.webm" vp8-480 2
smoke_dec "$CLIP/mpeg2_ip.mpg" mpeg2-ip 2

echo "== JPEG mjpeg_vaapi =="
$FF -hide_banner -vaapi_device /dev/dri/renderD128 -f lavfi \
  -i testsrc=size=320x240:rate=5:duration=1 -vf "format=nv12,hwupload" \
  -frames:v 1 -c:v mjpeg_vaapi -y "$OUT/va.jpg" >"$OUT/jpeg.stderr" 2>&1
if grep -q "v4l2stateless: JPEG encoded" "$OUT/jpeg.stderr"; then
  echo "JPEG_OK"; pass=$((pass+1))
else
  echo "JPEG_FAIL"; tail -15 "$OUT/jpeg.stderr"; fail=$((fail+1))
fi

if [ -f "$CLIP/h264_qcif.mp4" ]; then
  echo "== VPP scale_vaapi =="
  $FF -hide_banner -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device /dev/dri/renderD128 -i "$CLIP/h264_qcif.mp4" \
    -vf "scale_vaapi=w=160:h=120,hwdownload,format=nv12" -frames:v 2 \
    -f null - >"$OUT/vpp.stderr" 2>&1
  if grep -q "v4l2stateless: VPP " "$OUT/vpp.stderr"; then
    echo "VPP_OK"; pass=$((pass+1))
  else
    echo "VPP_FAIL"; tail -15 "$OUT/vpp.stderr"; fail=$((fail+1))
  fi
fi

echo "PASS=$pass FAIL=$fail"
if [ "$fail" -ne 0 ]; then
  echo "SMOKE_FAILED"
  exit 1
fi
echo "SMOKE_ALL_PASS"
exit 0
