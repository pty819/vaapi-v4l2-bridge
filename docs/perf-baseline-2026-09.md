# Perf baseline — pre-refactor hot-path syscalls (2026-09-03)

Branch `refactor/audit-2026-09`, Phase 0 gate. This is the number Phase 4
items are measured against. Same procedure must be reused for the Phase 4
gate (same stream, same flags).

## Conditions

- Chrome via `google-chrome-stable` wrapper (pinned 4-7, vaapi), CDP 9222,
  `--autoplay-policy=no-user-gesture-required`, live.bilibili.com/22957791,
  H.264 1280x720 ~30 fps hardware decode (VaapiVideoDecoder, zero egl
  errors, canvas non-black — `scripts/chrome_smoke.sh` PASS).
- Driver: branch tip (a2a41a9-equivalent src; Phase 0 changed no driver code).
- Measurement: `sudo timeout 32 strace -c -f -p <gpu-process>` with
  `getVideoPlaybackQuality().totalVideoFrames` sampled before/after.

## Numbers (32.55 s window, 968 frames decoded, 29.8 fps, 0 new drops)

| syscall | calls | per frame |
|---|---|---|
| ioctl | 18,070 | **18.7** |
| close | 2,838 | 2.93 |
| munmap | 1,966 | 2.03 |
| mmap | 1,893 | 1.96 |
| ppoll | 1,892 | 1.96 |
| sendto | 2,845 | 2.94 (IPC, not ours) |
| futex/epoll | (thread sync + event loop, not per-frame) | |

Caveat: this counts the WHOLE GPU process, so Mesa's own GL/rendering
ioctls are included. The driver-attributable share per the audit's static
inventory is ~13 (request alloc+close, per-frame QUERYBUF, global SPS +
4 request controls, QBUF×2, REQ queue, DQBUF, gbm map/unmap ioctls);
Phase 4 aims to remove 7-9 of those per frame.

## Phase 4 acceptance

Re-run this exact procedure; expect ioctl/frame to drop by ≥5 with zero
matrix regressions and unchanged Chrome smoke. mmap/munmap per frame should
approach ~0.3 (persistent mappings).

## After (P4 items 1-5, 2026-09-03, same 1080p path via local clip)

Per-ioctl-cmd counts (22.4s window, 672 frames, REQ_QUEUE=656 cross-check):

| ioctl | before (derived) | after | per frame after |
|---|---|---|---|
| S_EXT_CTRLS | 5/frame | 1312 | **2.0** (batch + scaling) |
| QUERYBUF | 1/frame | 0 | **0** (memoized) |
| MEDIA_REQUEST_ALLOC + close | 1+1/frame | 0 + REINIT 656 | **1** (reinit recycle) |
| MEDIA_REQUEST_IOC_QUEUE | 1/frame | 656 | 1 (irreducible) |

Decode-path driver ioctls: ~12/frame -> ~7/frame (−5..−6). Matrix 27/27
and chrome smoke green after each item.

Deferred (documented follow-ups): P4 items 6-8 (persistent memfd mapping,
VPP/JPEG persistent queues, stride alignment) and P3 clusters 7-11.
