> **2026-08-27 09:30：** 当前可接续状态见同目录 [HANDOFF.md](HANDOFF.md)。下文 AV1「整段不对」已过时——同日 09:26 驱动下 `/tmp/av1.mp4` **第 0 帧已与软解 bit-exact**，P 帧仍错。

# STATE.md — Project State

Last verified: 2026-08-27 on Orange Pi 5, Armbian 26.8.3 edge 7.1.8.

## What actually works

H.264 and HEVC hardware decode through this VA-API → V4L2-stateless bridge is bit-exact against ffmpeg software decode on the streams below. The driver is installed as `/usr/lib/aarch64-linux-gnu/dri/v4l2stateless_drv_video.so`. `~/.profile` exports `LIBVA_DRIVER_NAME=v4l2stateless`. Firefox profile `vaapi.default-release` already has `media.ffmpeg.vaapi.enabled` and `gfx.webrender.all`.

### H.264 (rkvdec `/dev/video1`, frame-based UAPI)

| Stream | Result |
|---|---|
| 1080p30 High + B-frames (`hs_test.mp4`) | 120/120 framemd5 match |
| 1080p30 High, no B-frames (`allp.mp4`) | 120/120 |
| 1080p30 4-slice (`ms.mp4`) | 120/120 |

### HEVC (same rkvdec, frame-based UAPI)

| Stream | Result |
|---|---|
| 1080p30 Main (`hevc.mp4`) | 120/120 |
| 1080p30 WPP (`hevcwpp.mp4`) | 120/120 |

Decisive test (do not use a plain `-f framemd5` without `hwdownload` — ffmpeg silently falls back to software):

```bash
LIBVA_DRIVER_NAME=v4l2stateless /usr/bin/ffmpeg \
  -hwaccel vaapi -hwaccel_output_format vaapi -vaapi_device /dev/dri/renderD128 \
  -i FILE.mp4 -vf "hwdownload,format=nv12" -pix_fmt yuv420p -f framemd5 -y hard.md5
```

Compare against a software reference from the same binary. Match on every frame hash, plus no `Failed to query surface attributes` / `hardware accelerator failed` lines.

### AV1 (hantro `/dev/video4` + `/dev/media3`)

The pipeline now completes 120 frames without ioctl errors (media device is resolved from sysfs, sequence is a global control, STREAMON is deferred until after sequence, tile-group entries are submitted). **Output is not bit-exact.** GStreamer's own `v4l2slav1dec` on the same kernel also diverges from software from frame 49 onward on the test clip, so this is not a VA-API-only gap. Do not claim AV1 hardware decode as done.

## Runtime pieces that had to exist (and were missing in the June skeleton)

- `VIDIOC_STREAMON` on both queues (HEVC/AV1: after the global SPS/sequence control)
- Capture QBUF is **bare** (no `V4L2_BUF_FLAG_REQUEST_FD`)
- Output/capture buffer pools, not hardcoded index 0
- Synchronous decode (submit request, poll, DQBUF) so VA-API `vaSyncSurface` has a frame
- `vaQuerySurfaceAttributes` / `vaCreateSurfaces2` / `vaCreateImage` / `vaGetImage`
- Thread-safe buffer IDs (`++id` under a lock; pre-increment only)
- H.264: `DPB_ENTRY_FLAG_VALID`, `FLAG_PFRAME`/`FLAG_BFRAME`, PPS `num_ref_idx`, `dec_ref_pic_marking_bit_size`, DPB sorted by `frame_num`, timestamp ns↔timeval round-trip, Annex B prefix, multi-slice concat
- HEVC: global SPS, PCM fields left zero when disabled, `chroma_format_idc`, uniform-spacing PPS flag, flat scaling matrix, DPB classified into StCurrBefore/After, capture geometry stored on the surface (1088 vs 1080)
- Media node is looked up per video device (`/dev/video4` → `/dev/media3`)

## Known remaining issues

1. **AV1 not bit-exact** — pipeline runs; picture is wrong. Kernel/GStreamer reference is also imperfect. Needs a dedicated pass: film-grain, `order_hints`, `refresh_frame_flags` (VA does not expose the bitmask), tile-group OBU wrapping vs ffmpeg's raw tiles.
2. **Resolution changes mid-stream** are not renegotiated.
3. **10-bit HEVC Main10** is advertised; not verified (test streams were 8-bit).
4. **Firefox** is configured for VA-API; user still needs to confirm `about:support` Hardware decoding after a login session that sources `~/.profile`.

## Files (local Mac copies in `~/v4l2bridge-dev/`, NAS `~/vaapi-v4l2-bridge/src/`)

| File | Role |
|---|---|
| `v4l2stateless.c` | VTable, surfaces, images, context setup, buffer IDs |
| `v4l2stateless_device.c` | open / STREAMON / QBUF / request / media lookup |
| `v4l2stateless_h264.c` | H.264 translate + sync submit |
| `v4l2stateless_hevc.c` | HEVC translate + sync submit |
| `v4l2stateless_av1.c` | AV1 translate + sync submit (not bit-exact) |
| `v4l2stateless.h` | structs, pools, helpers |
