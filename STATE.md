# STATE.md — Project State

## Current Phase: Phase 1-4 Complete → Ready for Phase 5 (Testing)

## Completed

### Phase 1: Skeleton + Init ✅
- VTable with 17+ non-null function pointers
- vainfo shows 5 profiles: H264Main, H264High, HEVCMain, HEVCMain10, AV1Profile0
- All VLD entrypoints

### Phase 2: H.264 Decode ✅
- VAPictureParameterBufferH264 → SPS + PPS + decode_params
- VAIQMatrixBufferH264 → scaling_matrix
- VASliceParameterBufferH264 → slice_params
- DPB reference frame timestamp lookup
- Full request submission: SPS → PPS → decode_params → scaling_matrix → slice_params → queue → submit

### Phase 3: HEVC Decode ✅
- VAPictureParameterBufferHEVC → SPS + PPS + decode_params
- RPS (Reference Picture Set) for VDPU381 via decode_params
- Full request submission

### Phase 4: AV1 Decode ✅
- VADecPictureParameterBufferAV1 → sequence + frame params
- Full request submission

### V4L2 Request API Integration ✅
- MEDIA_IOC_REQUEST_ALLOC for per-frame requests
- VIDIOC_S_EXT_CTRLS with V4L2_CTRL_WHICH_REQUEST_VAL for control binding
- MEDIA_REQUEST_IOC_QUEUE for request submission
- Pre-mapped output buffers for bitstream input
- Capture buffer queue with DMA-BUF export
- sync_surface: poll + dequeue + export DMA-BUF
- derive_image: returns DMA-BUF fd for NV12 decoded frames

## File Summary (2608 lines)
| File | Lines | Description |
|------|-------|-------------|
| v4l2stateless.c | 1061 | Main driver: VTable, context/surface/buffer management, decode pipeline |
| v4l2stateless_device.c | 382 | V4L2 device management, request API, buffer queue/dequeue |
| v4l2stateless_h264.c | 500 | H.264 parameter translation + request submission |
| v4l2stateless_hevc.c | 279 | HEVC parameter translation + request submission |
| v4l2stateless_av1.c | 201 | AV1 parameter translation + request submission |
| v4l2stateless.h | 158 | Data structures and function declarations |

## Pending
- Phase 5: Integration testing (need display + video playback)
- Phase 6: Build packaging, README

## Known Issues
1. DPB timestamp lookup uses NULL driver_data (multi-reference decode needs fix)
2. Output buffer round-robin uses fixed index 0 (should cycle)
3. Capture buffer round-robin uses fixed index 0 (should cycle)
4. HEVC slice params not set (only SPS/PPS/decode_params)
5. AV1 tile_group params not set (only sequence/frame)
6. No error recovery on failed request submission
7. sync_surface has no timeout — will block indefinitely on failed decode

## Test Commands
```bash
# Verify driver loads
LIBVA_DRIVER_NAME=v4l2stateless vainfo

# Test H.264 decode (needs video file)
LIBVA_DRIVER_NAME=v4l2stateless ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -i test.mp4 -f null -

# Test with Firefox
LIBVA_DRIVER_NAME=v4l2stateless firefox
```

## Git History
```
0c85027 feat: complete V4L2 request API integration — all codecs submit decode requests
c6e78a2 docs: update STATE.md — Phase 1-4 complete
843f83a feat: Phase 3+4 — HEVC + AV1 parameter translation
b8dc7bc feat: Phase 2 — H.264 parameter translation + V4L2 device management
e23d277 feat: Phase 1 complete — driver skeleton loads, vainfo shows H264/HEVC/AV1 profiles
3718057 docs: initialize project
```
