#!/bin/bash
# Full VA-API → V4L2-stateless matrix. Must run on the RK3588 host.
# Success = hwdownload framemd5 vs software, except MPEG-2 which is
# compared to GStreamer v4l2slmpeg2dec (same kernel IDCT; ffmpeg SW differs).
set -u
FF=${FF:-/usr/bin/ffmpeg}
export LIBVA_DRIVER_NAME=v4l2stateless
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CLIP=${CLIPDIR:-$ROOT/verify/clips}
OUT=${OUTDIR:-$ROOT/verify/matrix}
mkdir -p "$CLIP" "$OUT"
fail=0
pass=0

forbidden() {
  if grep -E "hardware accelerator failed|Failed to query surface attributes|hwaccel initialisation returned error" "$1" >/dev/null; then
    echo "FORBIDDEN $1"
    grep -E "hardware accelerator failed|Failed to query surface attributes|hwaccel initialisation" "$1" | head -5
    return 1
  fi
  return 0
}

used_hw() {
  # Must have opened a v4l2 node, not silently software-decoded.
  if ! grep -q "v4l2stateless: .* config uses /dev/video" "$1"; then
    echo "NO_HW $1 (driver never opened a decoder)"
    return 1
  fi
  return 0
}

enc() {
  local dest="$1"; shift
  if [ -f "$dest" ]; then
    echo "have $dest"
    return 0
  fi
  echo "ENC $dest"
  $FF -hide_banner -y "$@" "$dest" >"${dest}.enc.log" 2>&1
  echo "enc exit=$? $dest"
}

pair_sw() {
  local src="$1" n="$2" tag="$3"
  echo "== SW $tag $src n=$n =="
  $FF -hide_banner -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device /dev/dri/renderD128 -i "$src" \
    -vf "hwdownload,format=nv12" -pix_fmt yuv420p -frames:v "$n" \
    -f framemd5 -y "$OUT/${tag}-hw.md5" >"$OUT/${tag}-hw.stderr" 2>&1
  local he=$?
  $FF -hide_banner -i "$src" -pix_fmt yuv420p -frames:v "$n" \
    -f framemd5 -y "$OUT/${tag}-sw.md5" >"$OUT/${tag}-sw.stderr" 2>&1
  if [ "$he" -ne 0 ]; then
    echo "HW_EXIT $he $tag"; fail=$((fail+1)); tail -15 "$OUT/${tag}-hw.stderr"; return
  fi
  forbidden "$OUT/${tag}-hw.stderr" || { fail=$((fail+1)); return; }
  used_hw "$OUT/${tag}-hw.stderr" || { fail=$((fail+1)); return; }
  if ! diff -q "$OUT/${tag}-hw.md5" "$OUT/${tag}-sw.md5" >/dev/null; then
    echo "MD5_DIFF $tag"
    diff -u "$OUT/${tag}-sw.md5" "$OUT/${tag}-hw.md5" | head -16
    fail=$((fail+1))
  else
    echo "MD5_MATCH $tag"
    pass=$((pass+1))
  fi
}

pair_gst_mpeg2() {
  local src="$1" n="$2" tag="$3"
  echo "== GST $tag $src n=$n =="
  $FF -hide_banner -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device /dev/dri/renderD128 -i "$src" \
    -vf "hwdownload,format=nv12" -pix_fmt yuv420p -frames:v "$n" \
    -f framemd5 -y "$OUT/${tag}-hw.md5" >"$OUT/${tag}-hw.stderr" 2>&1
  local he=$?
  if [ "$he" -ne 0 ]; then
    echo "HW_EXIT $he $tag"; fail=$((fail+1)); tail -15 "$OUT/${tag}-hw.stderr"; return
  fi
  forbidden "$OUT/${tag}-hw.stderr" || { fail=$((fail+1)); return; }
  used_hw "$OUT/${tag}-hw.stderr" || { fail=$((fail+1)); return; }
  local yuv="$OUT/${tag}-gst.yuv"
  gst-launch-1.0 -q filesrc location="$src" ! parsebin ! v4l2slmpeg2dec \
    ! videoconvert ! video/x-raw,format=I420 ! filesink location="$yuv" \
    >"$OUT/${tag}-gst.stderr" 2>&1 || { echo "GST_FAIL $tag"; fail=$((fail+1)); return; }
  # Probe size from our hw md5 header
  local dim
  dim=$(awk '/^#dimensions 0:/{print $3; exit}' "$OUT/${tag}-hw.md5")
  $FF -hide_banner -s "$dim" -pix_fmt yuv420p -i "$yuv" -frames:v "$n" \
    -f framemd5 -y "$OUT/${tag}-gst.md5" >"$OUT/${tag}-gstmd5.stderr" 2>&1
  # Compare payload hashes only — GStreamer I420 dumps SAR 0/1.
  if python3 - "$OUT/${tag}-hw.md5" "$OUT/${tag}-gst.md5" << 'PY'
import sys
def hashes(p):
    h=[]
    for line in open(p):
        parts=line.strip().split(",")
        if len(parts)>=6 and parts[0].strip().startswith("0"):
            h.append(parts[-1].strip())
    return h
a, b = hashes(sys.argv[1]), hashes(sys.argv[2])
sys.exit(0 if a and a == b else 1)
PY
  then
    echo "GST_MATCH $tag"
    pass=$((pass+1))
  else
    echo "GST_DIFF $tag"
    diff -u "$OUT/${tag}-gst.md5" "$OUT/${tag}-hw.md5" | head -16
    fail=$((fail+1))
  fi
}

echo "===== unit ====="
"$ROOT/builddir/test_video_probe" | tee "$OUT/unit-probe.log"
u1=${PIPESTATUS[0]}
"$ROOT/builddir/test_vp8_mpeg2_fill" | tee "$OUT/unit-fill.log"
u2=${PIPESTATUS[0]}
"$ROOT/builddir/test_video_probe" --live | tee "$OUT/live-probe.log"
u3=${PIPESTATUS[0]}
"$ROOT/builddir/test_export_recapture" | tee "$OUT/unit-export.log"
u4=${PIPESTATUS[0]}
"$ROOT/builddir/test_format" | tee "$OUT/unit-format.log"
u5=${PIPESTATUS[0]}
if [ "$u1" -ne 0 ] || [ "$u2" -ne 0 ] || [ "$u3" -ne 0 ] || [ "$u4" -ne 0 ] || [ "$u5" -ne 0 ]; then
  echo "UNIT_FAIL u1=$u1 u2=$u2 u3=$u3 u4=$u4 u5=$u5"
  fail=$((fail+1))
else
  echo "UNIT_PASS"
  pass=$((pass+1))
fi

echo "===== gbm-probe: platform gate for GBM display surfaces ====="
cc -O2 -Wall -o "$OUT/gbm_probe" tests/gbm_probe.c \
  -lgbm -lEGL -lGLESv2 || { echo CC_FAIL gbm_probe; fail=$((fail+1)); }
if [ -x "$OUT/gbm_probe" ]; then
  if "$OUT/gbm_probe" 1280 720 >"$OUT/gbm_probe.log" 2>&1 &&
     "$OUT/gbm_probe" 1920 1080 >>"$OUT/gbm_probe.log" 2>&1; then
    echo "PASS gbm-probe"; pass=$((pass+1))
  else
    echo "FAIL gbm-probe"; fail=$((fail+1))
    tail -3 "$OUT/gbm_probe.log"
  fi
fi

echo "===== vainfo ====="
vainfo --display drm --device /dev/dri/renderD128 >"$OUT/vainfo.log" 2>&1 || true
if ! grep -q "v4l2stateless/vaapi-v4l2-bridge" "$OUT/vainfo.log"; then
  echo "vainfo did not load driver"; fail=$((fail+1))
else
  # AV1 is intentionally not advertised over VA-API (short sessions
  # reopening the VPU981 node can hang the SoC); desktop AV1 goes
  # Chromium/GStreamer native V4L2. Do not expect it here.
  for p in VAProfileH264ConstrainedBaseline VAProfileH264Main VAProfileH264High \
           VAProfileH264High10 VAProfileH264High422 \
           VAProfileHEVCMain VAProfileHEVCMain10 \
           VAProfileVP8Version0_3 VAProfileMPEG2Simple VAProfileMPEG2Main \
           VAProfileJPEGBaseline VAProfileNone; do
    if grep -q "$p" "$OUT/vainfo.log"; then
      echo "VAINFO_OK $p"
    else
      echo "VAINFO_MISS $p"; fail=$((fail+1))
    fi
  done
  pass=$((pass+1))
fi

echo "===== generate clips ====="
# H.264
enc "$CLIP/h264_high_b.mp4" -f lavfi -i testsrc=size=1920x1080:rate=30:duration=4 \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v high -g 30 -bf 2
enc "$CLIP/h264_allp.mp4" -f lavfi -i testsrc=size=1920x1080:rate=30:duration=4 \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v high -g 30 -bf 0
enc "$CLIP/h264_slices.mp4" -f lavfi -i testsrc=size=1920x1080:rate=30:duration=4 \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v high -g 30 -bf 0 -slices 4
enc "$CLIP/h264_baseline.mp4" -f lavfi -i testsrc=size=1280x720:rate=30:duration=2 \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v baseline -g 30 -bf 0
enc "$CLIP/h264_main.mp4" -f lavfi -i testsrc=size=1280x720:rate=30:duration=2 \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v main -g 30 -bf 2
enc "$CLIP/h264_4k.mp4" -f lavfi -i testsrc=size=3840x2160:rate=30:duration=1 \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v high -g 30 -bf 0 -frames:v 8
	enc "$CLIP/h264_qcif.mp4" -f lavfi -i testsrc=size=320x240:rate=25:duration=2 \
	  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v high -g 25 -bf 0
	enc "$CLIP/h264_10.mp4" -f lavfi -i testsrc=size=1280x720:rate=30:duration=1 \
	  -pix_fmt yuv420p10le -c:v libx264 -preset ultrafast -profile:v high10 -frames:v 8
	enc "$CLIP/h264_422_idr10.h264" -f lavfi -i testsrc=size=1280x720:rate=30:duration=1 \
	  -pix_fmt yuv422p10le -c:v libx264 -preset ultrafast -g 1 -keyint_min 1 -sc_threshold 0 -frames:v 8 -f h264
	enc "$CLIP/h264_422_idr8.h264" -f lavfi -i testsrc=size=1280x720:rate=30:duration=1 \
	  -pix_fmt yuv422p -c:v libx264 -preset ultrafast -g 1 -keyint_min 1 -sc_threshold 0 -frames:v 8 -f h264
	enc "$CLIP/h264_idr_nv12.h264" -f lavfi -i testsrc=size=1280x720:rate=30:duration=1 \
	  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -g 1 -keyint_min 1 -sc_threshold 0 -frames:v 8 -f h264

# HEVC
enc "$CLIP/hevc_main.mp4" -f lavfi -i testsrc=size=1920x1080:rate=30:duration=4 \
  -pix_fmt yuv420p -c:v libx265 -preset ultrafast -x265-params "log-level=error" -g 30
enc "$CLIP/hevc_wpp.mp4" -f lavfi -i testsrc=size=1920x1080:rate=30:duration=4 \
  -pix_fmt yuv420p -c:v libx265 -preset ultrafast -x265-params "log-level=error:wpp=1" -g 30
enc "$CLIP/hevc_4k.mp4" -f lavfi -i testsrc=size=3840x2160:rate=30:duration=1 \
  -pix_fmt yuv420p -c:v libx265 -preset ultrafast -x265-params "log-level=error" -g 30 -frames:v 8
enc "$CLIP/hevc10.mp4" -f lavfi -i testsrc=size=1280x720:rate=30:duration=1 \
  -pix_fmt yuv420p10le -c:v libx265 -preset ultrafast -x265-params "log-level=error" -frames:v 8

# AV1 clips unused while AV1 is un-advertised (see decode matrix below).
# enc "$CLIP/av1_aom.mp4" -f lavfi -i testsrc=size=1280x720:rate=30:duration=2 \
#   -pix_fmt yuv420p -c:v libaom-av1 -cpu-used 8 -crf 32 -b:v 0 -g 30 -usage realtime
# enc "$CLIP/av1_svt.mp4" -f lavfi -i testsrc=size=1280x720:rate=30:duration=2 \
#   -pix_fmt yuv420p -c:v libsvtav1 -preset 10 -g 32 -b:v 1M
# enc "$CLIP/av1_4k.mp4" -f lavfi -i testsrc=size=3840x2160:rate=30:duration=1 \
#   -pix_fmt yuv420p -c:v libaom-av1 -cpu-used 8 -crf 36 -b:v 0 -g 8 -usage realtime -frames:v 8
# enc "$CLIP/av1_default.mp4" -f lavfi -i testsrc=size=640x360:rate=30:duration=1 \
#   -pix_fmt yuv420p -c:v libaom-av1 -cpu-used 8 -crf 36 -b:v 0 -g 30

# VP8
enc "$CLIP/vp8_480.webm" -f lavfi -i testsrc=size=640x480:rate=25:duration=2 \
  -pix_fmt yuv420p -c:v libvpx -b:v 500k -g 12 -auto-alt-ref 0
enc "$CLIP/vp8_720.webm" -f lavfi -i testsrc=size=1280x720:rate=30:duration=2 \
  -pix_fmt yuv420p -c:v libvpx -b:v 1M -g 15 -auto-alt-ref 1

# MPEG-2
enc "$CLIP/mpeg2_ip.mpg" -f lavfi -i testsrc=size=720x480:rate=25:duration=2 \
  -pix_fmt yuv420p -c:v mpeg2video -q:v 5 -g 12 -bf 0
enc "$CLIP/mpeg2_b.mpg" -f lavfi -i testsrc=size=720x480:rate=25:duration=2 \
  -pix_fmt yuv420p -c:v mpeg2video -q:v 5 -g 12 -bf 2
enc "$CLIP/mpeg2_1080.mpg" -f lavfi -i testsrc=size=1920x1080:rate=25:duration=2 \
  -pix_fmt yuv420p -c:v mpeg2video -q:v 8 -g 12 -bf 2

echo "===== decode matrix ====="
pair_sw "$CLIP/h264_high_b.mp4" 120 h264-high-b
pair_sw "$CLIP/h264_allp.mp4" 120 h264-allp
pair_sw "$CLIP/h264_slices.mp4" 120 h264-slices
pair_sw "$CLIP/h264_baseline.mp4" 50 h264-baseline
pair_sw "$CLIP/h264_main.mp4" 50 h264-main
pair_sw "$CLIP/h264_4k.mp4" 8 h264-4k
	pair_sw "$CLIP/h264_qcif.mp4" 40 h264-qcif

	echo "== SW h26410 $CLIP/h264_10.mp4 n=8 =="
	$FF -hide_banner -hwaccel vaapi -hwaccel_output_format vaapi \
	  -vaapi_device /dev/dri/renderD128 -i "$CLIP/h264_10.mp4" \
	  -vf "hwdownload,format=p010le" -pix_fmt p010le -frames:v 8 \
	  -f framemd5 -y "$OUT/h26410-hw.md5" >"$OUT/h26410-hw.stderr" 2>&1
	h10=$?
	$FF -hide_banner -i "$CLIP/h264_10.mp4" -pix_fmt p010le -frames:v 8 \
	  -f framemd5 -y "$OUT/h26410-sw.md5" >/dev/null 2>&1
	if [ "$h10" -ne 0 ]; then echo "HW_EXIT $h10 h26410"; fail=$((fail+1))
	elif ! forbidden "$OUT/h26410-hw.stderr"; then fail=$((fail+1))
	elif ! used_hw "$OUT/h26410-hw.stderr"; then fail=$((fail+1))
	elif ! grep -q "fourcc=NV15" "$OUT/h26410-hw.stderr"; then
	  echo "NO_NV15 h26410"; fail=$((fail+1))
	elif diff -q "$OUT/h26410-hw.md5" "$OUT/h26410-sw.md5" >/dev/null; then
	  echo "MD5_MATCH h26410"; pass=$((pass+1))
	else echo "MD5_DIFF h26410"; fail=$((fail+1)); fi

	# H.264 High 4:2:2 — stock ffmpeg cannot reach the bridge for 4:2:2
	# (h264 get_pixel_format only offers VAAPI for 4:2:0, and the vaapi
	# profile map has no H264_HIGH_422 entry), so drive the VA-API path
	# with tests/va_h264422_client.c and the kernel path with GST.
	cc -O2 -Wall -o "$OUT/va422" tests/va_h264422_client.c \
	  $(pkg-config --cflags --libs libva libva-drm) || { echo CC_FAIL h264422; fail=$((fail+1)); }
	for depth in 10:Y210:y210le:NV20 8:YUY2:yuyv422:NV16; do
	  d="${depth%%:*}"; rest="${depth#*:}"
	  img="${rest%%:*}"; rest="${rest#*:}"
	  pfmt="${rest%%:*}"; cap="${rest##*:}"
	  clip="$CLIP/h264_422_idr${d}.h264"
	  LIBVA_DRIVER_NAME=v4l2stateless "$OUT/va422" /dev/dri/renderD128 "$clip" "$d" \
	    >"$OUT/h264422${d}-va.raw" 2>"$OUT/h264422${d}-va.stderr"
	  v4=$?
	  if [ "$v4" -ne 0 ]; then echo "VA422_EXIT $v4 h264422-$d"; fail=$((fail+1)); continue; fi
	  grep -q "fourcc=$cap" "$OUT/h264422${d}-va.stderr" \
	    || { echo "NO_$cap h264422-$d"; fail=$((fail+1)); continue; }
	  $FF -hide_banner -loglevel error -f rawvideo -framerate 30 -s 1280x720 \
	    -pix_fmt "$pfmt" -i "$OUT/h264422${d}-va.raw" -frames:v 8 \
	    -f framemd5 -y "$OUT/h264422${d}-va.md5"
	  $FF -hide_banner -loglevel error -i "$clip" -pix_fmt "$pfmt" -frames:v 8 \
	    -f framemd5 -y "$OUT/h264422${d}-sw.md5"
	  if diff <(grep -v '^#' "$OUT/h264422${d}-va.md5") \
	           <(grep -v '^#' "$OUT/h264422${d}-sw.md5") >/dev/null; then
	    echo "MD5_MATCH h264422-$d-va"; pass=$((pass+1))
	  else echo "MD5_DIFF h264422-$d-va"; fail=$((fail+1)); fi
	done
	# --- va-export: exported dma-buf content == vaGetImage content ----------
meson compile -C "$ROOT/builddir" va_export_client >/dev/null 2>&1 || true
if [ -x "$ROOT/builddir/va_export_client" ]; then
  LIBVA_DRIVER_NAME=v4l2stateless "$ROOT/builddir/va_export_client" \
    /dev/dri/renderD128 "$CLIP/h264_idr_nv12.h264" \
    >"$OUT/va_export.log" 2>&1
  ve=$?
  if [ "$ve" -eq 0 ] && grep -q "EXPORT_EXACT 7" "$OUT/va_export.log" \
     && grep -q "LAZY_EXACT 7" "$OUT/va_export.log"; then
    echo "PASS va-export"; pass=$((pass+1))
  else
    echo "FAIL va-export (exit $ve)"; fail=$((fail+1))
    tail -3 "$OUT/va_export.log"
  fi
else
  echo "SKIP va-export (not built)"; fail=$((fail+1))
fi

# kernel-level: GST v4l2sl vs GST software (8-bit direct, 10-bit via
	# tests/nv20_unpack.py because videoconvert rounds 10-bit unpacking)
	gst-launch-1.0 -q filesrc location="$CLIP/h264_422_idr8.h264" ! parsebin ! v4l2slh264dec \
	  ! videoconvert ! video/x-raw,format=Y42B ! filesink location="$OUT/h264422-8gst.raw" 2>/dev/null
	gst-launch-1.0 -q filesrc location="$CLIP/h264_422_idr8.h264" ! parsebin ! avdec_h264 \
	  ! videoconvert ! video/x-raw,format=Y42B ! filesink location="$OUT/h264422-8sw.raw" 2>/dev/null
	$FF -hide_banner -loglevel error -f rawvideo -s 1280x720 -pix_fmt yuv422p -i "$OUT/h264422-8gst.raw" \
	  -frames:v 8 -f framemd5 -y "$OUT/h264422-8gst.md5"
	$FF -hide_banner -loglevel error -f rawvideo -s 1280x720 -pix_fmt yuv422p -i "$OUT/h264422-8sw.raw" \
	  -frames:v 8 -f framemd5 -y "$OUT/h264422-8sw.md5"
	if diff <(grep -v '^#' "$OUT/h264422-8gst.md5") \
	         <(grep -v '^#' "$OUT/h264422-8sw.md5") >/dev/null; then
	  echo "GST_MATCH h264422-8"; pass=$((pass+1))
	else echo "GST_DIFF h264422-8"; fail=$((fail+1)); fi
	gst-launch-1.0 -q filesrc location="$CLIP/h264_422_idr10.h264" ! parsebin ! v4l2slh264dec \
	  ! video/x-raw,format=NV16_10LE40 ! filesink location="$OUT/h264422-10gst.raw" 2>/dev/null
	gst-launch-1.0 -q filesrc location="$CLIP/h264_422_idr10.h264" ! parsebin ! avdec_h264 \
	  ! video/x-raw,format=I422_10LE ! filesink location="$OUT/h264422-10sw.raw" 2>/dev/null
	python3 tests/nv20_unpack.py "$OUT/h264422-10gst.raw" "$OUT/h264422-10up.raw" 8 1280 720 2764800
	$FF -hide_banner -loglevel error -f rawvideo -s 1280x720 -pix_fmt yuv422p10le -i "$OUT/h264422-10up.raw" \
	  -frames:v 8 -f framemd5 -y "$OUT/h264422-10gst.md5"
	$FF -hide_banner -loglevel error -f rawvideo -s 1280x720 -pix_fmt yuv422p10le -i "$OUT/h264422-10sw.raw" \
	  -frames:v 8 -f framemd5 -y "$OUT/h264422-10sw.md5"
	if diff <(grep -v '^#' "$OUT/h264422-10gst.md5") \
	         <(grep -v '^#' "$OUT/h264422-10sw.md5") >/dev/null; then
	  echo "GST_MATCH h264422-10"; pass=$((pass+1))
	else echo "GST_DIFF h264422-10"; fail=$((fail+1)); fi

pair_sw "$CLIP/hevc_main.mp4" 120 hevc-main
pair_sw "$CLIP/hevc_wpp.mp4" 120 hevc-wpp
pair_sw "$CLIP/hevc_4k.mp4" 8 hevc-4k

echo "== SW hevc10 $CLIP/hevc10.mp4 n=8 =="
$FF -hide_banner -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device /dev/dri/renderD128 -i "$CLIP/hevc10.mp4" \
  -vf "hwdownload,format=p010le" -pix_fmt p010le -frames:v 8 \
  -f framemd5 -y "$OUT/hevc10-hw.md5" >"$OUT/hevc10-hw.stderr" 2>&1
he=$?
$FF -hide_banner -i "$CLIP/hevc10.mp4" -pix_fmt p010le -frames:v 8 \
  -f framemd5 -y "$OUT/hevc10-sw.md5" >/dev/null 2>&1
if [ "$he" -ne 0 ]; then echo "HW_EXIT $he hevc10"; fail=$((fail+1))
elif ! forbidden "$OUT/hevc10-hw.stderr"; then fail=$((fail+1))
elif ! used_hw "$OUT/hevc10-hw.stderr"; then fail=$((fail+1))
elif diff -q "$OUT/hevc10-hw.md5" "$OUT/hevc10-sw.md5" >/dev/null; then
  echo "MD5_MATCH hevc10"; pass=$((pass+1))
else echo "MD5_DIFF hevc10"; fail=$((fail+1)); fi

# AV1 VA-API decode pairs disabled with the AV1 un-advertising: ffmpeg's
# vaapi hwaccel can no longer pick VAProfileAV1Profile0, so these would
# only measure the software fallback. Re-enable when AV1 returns.
# pair_sw "$CLIP/av1_aom.mp4" 8 av1-aom-8
# pair_sw "$CLIP/av1_aom.mp4" 49 av1-aom-49
# pair_sw "$CLIP/av1_svt.mp4" 32 av1-svt-32
# pair_sw "$CLIP/av1_4k.mp4" 8 av1-4k
# pair_sw "$CLIP/av1_default.mp4" 16 av1-default-16

pair_sw "$CLIP/vp8_480.webm" 40 vp8-480
pair_sw "$CLIP/vp8_720.webm" 50 vp8-720

pair_gst_mpeg2 "$CLIP/mpeg2_ip.mpg" 40 mpeg2-ip
pair_gst_mpeg2 "$CLIP/mpeg2_b.mpg" 40 mpeg2-b
pair_gst_mpeg2 "$CLIP/mpeg2_1080.mpg" 40 mpeg2-1080

echo "== JPEG mjpeg_vaapi =="
# V4L2SL_DEBUG: JPEG/VPP success prints are debug-gated; the gate below
# greps them as proof the bridge was used.
V4L2SL_DEBUG=1 $FF -hide_banner -vaapi_device /dev/dri/renderD128 -f lavfi \
  -i testsrc=size=320x240:rate=5:duration=1 -vf "format=nv12,hwupload" \
  -c:v mjpeg_vaapi -y "$OUT/va.jpg" >"$OUT/jpeg.stderr" 2>&1
if grep -q "v4l2stateless: JPEG encoded" "$OUT/jpeg.stderr" && \
   ffprobe -v error -select_streams v:0 -show_entries stream=width,height,codec_name \
     -of csv=p=0 "$OUT/va.jpg" | grep -q mjpeg; then
  echo "JPEG_OK"; pass=$((pass+1))
else
  echo "JPEG_FAIL"; tail -15 "$OUT/jpeg.stderr"; fail=$((fail+1))
fi

echo "== VPP scale_vaapi =="
V4L2SL_DEBUG=1 $FF -hide_banner -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device /dev/dri/renderD128 -i "$CLIP/h264_qcif.mp4" \
  -vf "scale_vaapi=w=160:h=120,hwdownload,format=nv12" -frames:v 8 \
  -f framemd5 -y "$OUT/vpp.md5" >"$OUT/vpp.stderr" 2>&1
if grep -q "v4l2stateless: VPP " "$OUT/vpp.stderr" && \
   grep -q "^0," "$OUT/vpp.md5"; then
  echo "VPP_OK"; pass=$((pass+1))
else
  echo "VPP_FAIL"; tail -15 "$OUT/vpp.stderr"; fail=$((fail+1))
fi

echo "PASS=$pass FAIL=$fail" | tee "$OUT/summary.txt"
if [ "$fail" -ne 0 ]; then
  echo "MATRIX_FAILED"
  exit 1
fi
echo "MATRIX_ALL_PASS"
exit 0
