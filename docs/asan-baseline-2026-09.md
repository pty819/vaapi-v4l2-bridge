# ASan baseline — pre-refactor leak evidence (2026-09-03)

Branch `refactor/audit-2026-09`, tree at Phase 0. All leaks reported here
are EXPECTED at this point; Phase 2 (audit P0-2 / C-items) erases the
driver-side ones. This file is the "before" picture the Phase 2 gate
compares against.

## How to reproduce

```bash
cd ~/vaapi-v4l2-bridge
rm -rf builddir-asan && CC=gcc meson setup builddir-asan -Db_sanitize=address -Db_lundef=false
ninja -C builddir-asan
export ASAN_OPTIONS=detect_leaks=1

# unit tests (white-box, mock ioctl)
./builddir-asan/test_export_recapture
# full VA client that destroys everything itself
LIBVA_DRIVER_NAME=v4l2stateless ./builddir-asan/va_export_client \
  /dev/dri/renderD128 verify/clips/h264_idr_nv12.h264
# P0-2 reproducer: create surfaces + terminate WITHOUT destroy
gcc -fsanitize=address -O1 -o /tmp/va_leak_client tests/va_leak_client.c \
  $(pkg-config --cflags --libs libva libva-drm)
LIBVA_DRIVERS_PATH=$PWD/builddir-asan LIBVA_DRIVER_NAME=v4l2stateless \
  /tmp/va_leak_client /dev/dri/renderD128
```

(Note: `CC=gcc` — the default `ccache cc` wrapper makes meson's sanitizer
support check fail.)

## Results

| run | verdict | detail |
|---|---|---|
| test_export_recapture | 327,680 B / 5 allocs | 5 × 65,536 B: the mock-ioctl anon capture buffers (`calloc` in `v4l2sl_mmap_one_capture`) — the white-box tests never unmap them; test-harness + teardown-path gap (Phase 2 P0-2 covers the driver side) |
| va_export_client | 137,385 B / 133 allocs | dominated by the CLIENT's own bitstream buffer (111,533 B at va_export_client.c:300) and ~130 small Mesa/EGL allocations from `egl_setup` — client-side, not driver |
| **va_leak_client (P0-2 reproducer)** | **11,060,008 B / 17 allocs** | one 8-surface 720p session, `vaTerminate` without destroy: 8 × 1,382,400 B eager `cpu_ptr` callocs (also audit P1#10: decode-only surfaces never use them) + surface structs + config — `v4l2sl_terminate` frees contexts only |

## Interpretation

- P0-2 confirmed with numbers: a session-cycling client (browser tab churn,
  ffmpeg runs) loses ~11 MB per 8-surface 720p cycle, ~44 MB at 4K, until
  fd/memory exhaustion.
- The 11 MB is 99% the eager `cpu_ptr` — so Phase 4 item "lazy cpu_ptr" and
  Phase 2 item "P0-2 terminate" together turn this reproducer into an
  ASan-clean run. Target for the Phase 2 gate: `va_leak_client` reports
  0 leaks (aside from the two known client-side spots, which stay).
