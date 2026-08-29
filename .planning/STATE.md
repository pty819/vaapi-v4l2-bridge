# STATE — vaapi-v4l2-bridge

## Current phase
- **Phase:** maintenance (codec bridge + desktop VA-API clients)
- **Status:** decode/VPP/JPEG encode on RK3588 mainline work; docs in README / HANDOFF / APPS

## Completed
- Phase 1: driver skeleton — `vainfo` lists H.264/HEVC/AV1/VP8/MPEG-2/JPEG/VPP
- Phase 2: H.264 (incl. Constrained Baseline, High10 advertised)
- Phase 3: HEVC Main + Main10
- Phase 4: AV1 inter + refresh_frame_flags inference (libaom / SVT / RTC)
- Phase 5: host matrix `tests/run_full_matrix.sh`; Firefox / VLC / official Chrome wiring ([APPS.md](../APPS.md))
- Phase 6: Meson + README + GitHub `pty819/vaapi-v4l2-bridge`

Also shipped after the original roadmap: VP8, MPEG-2, JPEG encode, RGA VPP, DRM PRIME export, capture renegotiate, Chrome `FillProfileInfo_Locked` attribs.

## Blockers
(None for the supported 8-bit decode set.)

## Context
- Orange Pi 5, Armbian 26.8.3, kernel 7.1.8-edge-rockchip64, libva 1.23 / driver init 1.20
- `LIBVA_DRIVER_NAME=v4l2stateless`
- Last host matrix record: PASS=22 FAIL=0
- Last updated: 2026-08-29
