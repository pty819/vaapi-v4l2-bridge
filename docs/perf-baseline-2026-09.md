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
