#!/bin/bash
# Live Chrome smoke for the bridge driver: hardware decoder + visible picture.
# Prerequisites: chrome already running via the google-chrome-vaapi wrapper
# with --remote-debugging-port=9222. A tab is opened by this script:
#   - preferred: a live.bilibili.com tab (pass its URL as $1 if you want
#     a specific room; default opens the last-used room URL)
#   - fallback when no live room is playing: a local 1080p H.264 clip
#     served over http (file:// is NOT usable - opaque origin taints the
#     canvas and getImageData throws SecurityError).
# Exit 0 = all checks pass. Run on the NAS host.
#
# Checks:
#   1. media-internals reports VaapiVideoDecoder (not FFmpegVideoDecoder)
#   2. chrome://gpu shows no eglCreateImage problems
#   3. canvas brightness of the playing video: max of 3 samples >= 50/255
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export PATH="$HOME/.nvm/versions/node/v24.16.0/bin:$PATH"
CLIPS=$ROOT/verify/clips
GATE_CLIP=$CLIPS/h264_gate.mp4
SRV_PORT=8931
SRV_URL="http://127.0.0.1:$SRV_PORT/h264_gate.mp4"
fail=0

# --- pick target: live room argument, else local clip -------------------
TARGET="${1:-}"
if [ -z "$TARGET" ]; then
  TARGET="$SRV_URL"
  if [ ! -f "$GATE_CLIP" ] && [ -f "$CLIPS/h264_high_b.mp4" ]; then
    for i in $(seq 15); do echo "file '$CLIPS/h264_high_b.mp4'"; done \
      > /tmp/concat-gate.txt
    ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 \
      -i /tmp/concat-gate.txt -c copy "$GATE_CLIP"
  fi
  if [ ! -f "$GATE_CLIP" ]; then
    echo "SMOKE_FAIL no gate clip and no live URL given"; exit 1
  fi
  if ! curl -sf -m 2 -o /dev/null "http://127.0.0.1:$SRV_PORT/"; then
    (cd "$CLIPS" && setsid nohup python3 -m http.server $SRV_PORT \
       </dev/null >/tmp/smoke-httpsrv.log 2>&1 &)
    sleep 1
  fi
fi

# --- open the tab (chrome forwards to the running instance) -------------
export WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000
google-chrome-stable --user-data-dir="$HOME/.config/gc-dbg" \
  --remote-debugging-port=9222 --autoplay-policy=no-user-gesture-required \
  "$TARGET" >/dev/null 2>&1 &
sleep 10

FILTER="8931"; case "$TARGET" in *bilibili*) FILTER="live.bilibili";; esac

out=$(node "$ROOT/scripts/chrome_mi.js") || fail=1
echo "$out"
echo "$out" | grep -q "VaapiVideoDecoder" || { echo "SMOKE_FAIL decoder"; fail=1; }
echo "$out" | grep -iE "eglCreateImage.*(error|fail)" && { echo "SMOKE_FAIL egl"; fail=1; }

br=$(node "$ROOT/scripts/chrome_canvas.js" "$FILTER") || fail=1
echo "$br"
avg=$(echo "$br" | grep -oE '"avg":[0-9]+' | grep -oE '[0-9]+' | sort -n | tail -1)
[ -n "$avg" ] && [ "$avg" -ge 50 ] || { echo "SMOKE_FAIL canvas max-avg=${avg:-none}"; fail=1; }

[ "$fail" -eq 0 ] && echo "CHROME_SMOKE_PASS" || echo "CHROME_SMOKE_FAIL"
exit $fail
