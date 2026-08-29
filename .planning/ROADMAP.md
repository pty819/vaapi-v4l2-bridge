# ROADMAP — vaapi-v4l2-bridge

Original six phases are **done** on RK3588 mainline (2026-06 → 2026-08). This file is the historical plan plus what actually shipped.

## Milestone 1: Working VA-API → V4L2-stateless bridge — DONE

| Phase | Goal | Outcome |
|-------|------|---------|
| 1 Skeleton | vainfo lists profiles | `v4l2stateless_drv_video.so`, probe by fourcc |
| 2 H.264 | Request-API decode | bit-exact vs ffmpeg SW; CB/Main/High; High10 advertised |
| 3 HEVC | VDPU381 RPS | Main + WPP + 4K; Main10 vs p010le |
| 4 AV1 | hantro AV1 | bit-exact 8-bit; infer `refresh_frame_flags` |
| 5 Apps | real players | Firefox ffmpeg VA-API, VLC `avcodec-hw=vaapi`, official Chrome wrapper ([APPS.md](../APPS.md)) |
| 6 Packaging | meson + docs | GitHub `pty819/vaapi-v4l2-bridge`, README / HANDOFF / APPS |

Extra vs the June plan: VP8, MPEG-2, JPEG encode, RGA VPP, DRM PRIME 2 export, capture renegotiate, Chrome `FillProfileInfo_Locked` attribs.

Verification on host: `bash tests/run_full_matrix.sh` (last record PASS=22 FAIL=0).

## Later (not scheduled)

- VP9 if a mainline fourcc appears
- Stateless encode beyond JPEG
- Multi-stream / browser resolution-change matrix
- Upstream to a libva-drivers tree

*Last updated: 2026-08-29*
