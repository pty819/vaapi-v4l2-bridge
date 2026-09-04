# AV1 hot-path cleanup — verification coverage FIRST, then improvements

Date: 2026-09-04 (revised same day per user direction) · Status: scope
approved · Baseline commit: `2e59466`

**Ordering rule (user decision, binding):** every Phase A item below must
be verified — passing, or root-caused as a hardware limitation and
documented — **before any Phase B (improvement) code is touched**. Driver
bugs surfaced by Phase A are fixed inside Phase A.

## Phase A — close the unverified gap

The capability inventory lists four items as "unverified, not claimed".
Phase A verifies each. Prediction on record: item A1 is the most likely
to expose a real driver bug (analysis below).

### A1. super-res streams

AV1 codes frames downscaled (denominator 9–16) and upscales at output.
**Hypothesized bug**: `av1_fill_frame_params` fills the kernel's
`frame_width_minus_1` and `upscaled_width` with the SAME value (VA's
frame width). They are only equal when denominator == 8; a super-res
stream feeds the kernel the display width as the coded width → wrong
tile/SB grid → per-frame rejects or corruption. VA exposes only
display width + denominator; coded width must be derived:
`coded_w = (disp_w * 8 + denom / 2) / denom`, and the tile-grid
`mi_cols` must be computed from `coded_w`.

Verify: `aomenc --superres-mode=1 --superres-denominator=12` clip →
hw-vs-sw framemd5. Outcomes:
- bit-exact → PASS;
- failure → apply the coded-width fix (plan Phase A Task 1) and retest;
- kernel still rejects correct params → hardware limitation, record in
  README "will not decode" list, Phase A still closes green.

### A2. lossless / intrabc streams

Flags (`ALLOW_INTRABC`, lossless via q=0) are forwarded correctly from
VA; risk is purely untested hardware/kernel behavior. Verify: one
`-lossless 1` clip (ffmpeg libaom) and one intrabc-oriented clip
(aomenc `--lossless=1 --enable-intrabc=1`, testsrc2 content), both
hw-vs-sw bit-exact.

### A3. concurrent AV1 decoders

Each decoder context opens `/dev/video4` with independent queues (40
capture buffers each). Kernel M2M concurrency on the single hantro AV1
node is untested. Verify: two parallel ffmpeg VA-API AV1 decodes, both
bit-exact; then browser: two tabs each playing an AV1 video, both
decoding on hardware (≥2 live AV1 contexts in driver stderr, full frame
rate, zero VA errors).

### A4. browser AV1 mid-stream resolution change

The capture-renegotiate machinery (STREAMOFF/S_FMT/REQBUFS + DPB-model
reset) has ffmpeg-path matrix coverage only. Browser behavior (whether
Chrome renegotiates or recreates the decoder; whether playback stays on
AV1 after the switch) is unobserved. Verify: YouTube av01-forcing shim
with a delayed quality switch tiny→hd1080, plus bilibili quality-menu
switch as secondary; pass = decode stays hardware-AV1 across the switch
(currentTime advances, video4 held, zero VA errors, canvas sane).

## Phase B — hot-path cleanup (unchanged from prior approval)

B1. One `S_EXT_CTRLS` per frame (grain+frame+tile-group batched,
9 → 7 ioctls). Controls move ahead of the output-buffer pop; the three
error-path shapes collapse to one clean return.
B2. Loud warning when tile params exceed the 32-entry table
(warn-and-continue — a failed entrypoint is cached by Chrome for the
session).
B3. Fallback OBU-parse loop skips the already-tried `span` pointer.
B4. `V4L2SL_DEBUG` read once at init into `v4l2sl_debug`; all gates test
the flag.

### Phase B decisions (user-confirmed, carried over)

- **No fallback** if the kernel rejects combined control submission —
  the matrix fails loudly; the change is revisited, not patched around.
- Phase B verification: matrix double-run + full-clip SVT. No browser
  round (userspace-only sequencing of identical kernel objects).

## Non-goals

- No change to decode semantics: refresh parsing, slot model, pools,
  trust gate are frozen.
- Phase A does not chase VP9, 10-bit, or film grain (already documented
  limitations with known upstream causes).

## Acceptance

- Phase A gate: A1–A4 each closed (bit-exact / fixed-then-bit-exact /
  documented limitation). Only then Phase B starts.
- Phase B gate: matrix ×2 `MATRIX_ALL_PASS`, av1_svt full-clip 60/60
  bit-exact, strace shows ~1/3 the `VIDIOC_S_EXT_CTRLS` count.
- Everything on the NAS repo, one commit per task, pushed at the end.
