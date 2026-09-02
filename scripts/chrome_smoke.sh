#!/bin/bash
# Live Chrome smoke for the bridge driver: hardware decoder + visible picture.
# Prerequisites: chrome already running via the google-chrome-vaapi wrapper
# with --remote-debugging-port=9222 and a live.bilibili.com tab playing.
# Exit 0 = all checks pass. Run on the NAS host.
#
# Checks:
#   1. media-internals reports VaapiVideoDecoder (not FFmpegVideoDecoder)
#   2. chrome://gpu shows no eglCreateImage problems (absence-of-evidence
#      check: the black-frame era lit this page up, clean is the baseline)
#   3. canvas brightness of the playing video: max of 3 samples >= 50/255
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export PATH="$HOME/.nvm/versions/node/v24.16.0/bin:$PATH"
fail=0

out=$(node "$ROOT/scripts/chrome_mi.js") || fail=1
echo "$out"
echo "$out" | grep -q "VaapiVideoDecoder" || { echo "SMOKE_FAIL decoder"; fail=1; }
echo "$out" | grep -iE "eglCreateImage.*(error|fail)" && { echo "SMOKE_FAIL egl"; fail=1; }

br=$(node "$ROOT/scripts/chrome_canvas.js") || fail=1
echo "$br"
avg=$(echo "$br" | grep -oE '"avg":[0-9]+' | grep -oE '[0-9]+' | sort -n | tail -1)
[ -n "$avg" ] && [ "$avg" -ge 50 ] || { echo "SMOKE_FAIL canvas max-avg=${avg:-none}"; fail=1; }

[ "$fail" -eq 0 ] && echo "CHROME_SMOKE_PASS" || echo "CHROME_SMOKE_FAIL"
exit $fail
