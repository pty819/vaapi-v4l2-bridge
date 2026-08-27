#!/bin/bash
# Drive the installed v4l2stateless libva driver with apt ffmpeg.
# Must run on the RK3588 host with /dev/video1, /dev/video4, /dev/dri/renderD128.
set -euo pipefail

FF=${FF:-/usr/bin/ffmpeg}
export LIBVA_DRIVER_NAME=v4l2stateless
OUT=${OUT:-/tmp/vaapi-hwdownload-$$}
mkdir -p "$OUT"
fail=0

forbidden() {
  if grep -E "hardware accelerator failed|Failed to query surface attributes" "$1"; then
    echo "FORBIDDEN stderr in $1"
    return 1
  fi
  return 0
}

pair() {
  local src="$1" n="$2" tag="$3"
  echo "== $tag $src frames=$n =="
  $FF -hide_banner -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device /dev/dri/renderD128 -i "$src" \
    -vf "hwdownload,format=nv12" -pix_fmt yuv420p -frames:v "$n" \
    -f framemd5 -y "$OUT/${tag}-hw.md5" >"$OUT/${tag}-hw.stderr" 2>&1
  $FF -hide_banner -i "$src" -pix_fmt yuv420p -frames:v "$n" \
    -f framemd5 -y "$OUT/${tag}-sw.md5" >"$OUT/${tag}-sw.stderr" 2>&1
  forbidden "$OUT/${tag}-hw.stderr" || { fail=1; return; }
  if ! diff -q "$OUT/${tag}-sw.md5" "$OUT/${tag}-hw.md5" >/dev/null; then
    echo "MD5_DIFF $tag"
    diff -u "$OUT/${tag}-sw.md5" "$OUT/${tag}-hw.md5" | head -20
    fail=1
  else
    echo "MD5_MATCH $tag"
  fi
}

echo "== vainfo =="
vainfo --display drm --device /dev/dri/renderD128 >"$OUT/vainfo.log" 2>&1 || true
if ! grep -q "v4l2stateless/vaapi-v4l2-bridge" "$OUT/vainfo.log"; then
  echo "vainfo did not load v4l2stateless"
  cat "$OUT/vainfo.log"
  exit 1
fi
grep -E "VAProfileH264|VAProfileHEVC|VAProfileAV1|Driver version" "$OUT/vainfo.log"

pair /tmp/av1.mp4 8 av1-8
pair /tmp/av1.mp4 49 av1-49
pair /tmp/hs_test.mp4 120 h264-hs
pair /tmp/allp.mp4 120 h264-allp
pair /tmp/ms.mp4 120 h264-ms
pair /tmp/hevc.mp4 120 hevc-main
pair /tmp/hevcwpp.mp4 120 hevc-wpp

if [ -f /tmp/4k_h264.mp4 ]; then
  pair /tmp/4k_h264.mp4 8 4k-h264
fi

if [ "$fail" -ne 0 ]; then
  echo "FAILED"
  exit 1
fi
echo "ALL_MATCH"
exit 0
