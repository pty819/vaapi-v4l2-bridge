# EXPBUF memcpy-replacement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove on the PSU-replaced RK3588 that `VIDIOC_EXPBUF` of a decoded VPU capture buffer can replace the capture→GBM `memcpy` on Chrome’s zero-copy path (ANGLE/Wayland), without rebooting.

**Architecture:** Keep the experiment on `experiment/expbuf-retry` in `.worktrees/expbuf-retry`. Gate with `V4L2SL_EXPBUF_EXPORT=1`. Load the worktree `.so` via `LIBVA_DRIVERS_PATH` — never overwrite `/usr/lib/aarch64-linux-gnu/dri/v4l2stateless_drv_video.so` until Phase 4 passes and the human asks to ship. Phase 0 is already on this branch (`47d6378`); start at Phase 1 (actually skip the GBM copy).

**Tech Stack:** C libva driver, V4L2 EXPBUF, GBM/EGL probes, `va_export_client`, Chrome wrapper + `ioctl_interpose.so`.

## Global Constraints

- Host: `liyifan@192.168.1.21`, worktree `/home/liyifan/vaapi-v4l2-bridge/.worktrees/expbuf-retry`, branch `experiment/expbuf-retry`.
- Sudo: `echo liyifan | sudo -S` — **do not** `cp` the experiment `.so` to dri.
- Load experiment: `export LIBVA_DRIVER_NAME=v4l2stateless LIBVA_DRIVERS_PATH=<worktree>/builddir`.
- Every EXPBUF/EGL/Chrome command: `timeout -k 5 <seconds>`.
- Chrome: wrapper `google-chrome-stable`, `--user-data-dir=$HOME/.config/gc-dbg`, SIGTERM only, never `pkill -9`. Restart instance after VA failure (session cache).
- C patches: write a script file and `scp`; never ssh-heredoc C string literals (`\n` splits them).
- `meson.build` is release `-O3`; worktree `builddir` already exists.
- Success is Chrome importing the VPU fd, not merely laboratory EGL.
- If the host reboots, stop, append the last stage to `docs/EXPBUF-RETRY.md`, do not continue.

---

### Task 1: Skip GBM memcpy when EXPBUF export is on

**Files:**
- Modify: `src/v4l2stateless_device.c` (`v4l2sl_surface_pull_capture`)
- Modify: `src/v4l2stateless.c` (export path already EXPBUFs when env set — keep it)
- Modify: `docs/EXPBUF-RETRY.md` (Phase 1 notes)

**Interfaces:**
- Consumes: `int v4l2sl_expbuf_export_wanted(void);` `int v4l2sl_capture_expbuf(struct v4l2sl_context *ctx, int buf_index);` (already in this branch)
- Produces: `pull_capture` in EXPBUF mode sets `has_pic`, `buf_index`, geometry, `last_writer` without `gbm_surface_upload` / full-frame memfd memcpy.

- [ ] **Step 1: Confirm Phase 0 still builds**

```bash
cd ~/vaapi-v4l2-bridge/.worktrees/expbuf-retry
ninja -C builddir
test -x builddir/va_export_client
test -x builddir/v4l2stateless_drv_video.so
```

Expected: ninja success.

- [ ] **Step 2: Change `v4l2sl_surface_pull_capture`**

In `src/v4l2stateless_device.c`, after mmap success and `src = capture_buf_ptr[buf_index]`, **before** the `if (surf->gbm_bo)` upload:

```c
    if (v4l2sl_expbuf_export_wanted()) {
        /* No capture→GBM/memfd memcpy. Chrome will EXPBUF this index.
         * GetImage maps the capture mmap (below) only if someone calls it. */
        surf->has_pic = 1;
        surf->buf_index = buf_index;
        surf->stride = stride;
        surf->aligned_h = alh;
        surf->cap_fourcc = fcc;
        surf->last_writer = V4L2SL_WRITER_MEMFD; /* GetImage uses capture mmap */
        /* Point memfd at a *view* of capture for GetImage: map is already src.
         * Do not memcpy. vaGetImage/derive must read capture_buf_ptr[buf_index]
         * while buf_index >= 0. */
        return 0;
    }
```

If `vaGetImage` currently requires `memfd_fd >= 0` and a mapping, teach GetImage/Derive (same file / `v4l2stateless.c`) : when `expbuf_export_wanted() && surf->buf_index >= 0`, source pixels from `ctx->capture_buf_ptr[surf->buf_index]` (need context lookup — `context_for_surface` already exists in `v4l2stateless.c`). Keep that change minimal: in `v4l2sl_get_image` / `v4l2sl_derive_image`, if env and `buf_index >= 0`, skip `ensure_memfd` and use the capture mmap as `src`.

Do **not** call `gbm_surface_ensure` in the EXPBUF export branch (already skipped if EXPBUF succeeds).

- [ ] **Step 3: Build and run `va_export_client` twice**

```bash
cd ~/vaapi-v4l2-bridge/.worktrees/expbuf-retry
ninja -C builddir
export LIBVA_DRIVER_NAME=v4l2stateless
export LIBVA_DRIVERS_PATH=$PWD/builddir
CLIP=~/vaapi-v4l2-bridge/verify/clips/h264_idr_nv12.h264
# memcpy path still exact
unset V4L2SL_EXPBUF_EXPORT
timeout -k 5 60 builddir/va_export_client /dev/dri/renderD128 $CLIP | tail -5
# EXPBUF path: must log EXPBUF export ok, NOT "gbm upload", NOT "falling back"
export V4L2SL_EXPBUF_EXPORT=1
timeout -k 5 60 builddir/va_export_client /dev/dri/renderD128 $CLIP 2>/tmp/p1.err | tail -20
grep "EXPBUF export ok" /tmp/p1.err | wc -l    # expect 8
grep -c "falling back" /tmp/p1.err             # expect 0
grep -c "gbm upload" /tmp/p1.err               # expect 0
```

Expected: both runs `EXPORT_EXACT 0..7` (and lazy pass if still present). Host up.

- [ ] **Step 4: Commit**

```bash
git add src/v4l2stateless_device.c src/v4l2stateless.c docs/EXPBUF-RETRY.md
git commit -m "experiment: EXPBUF mode skips capture→GBM memcpy; GetImage reads capture mmap"
```

---

### Task 2: DPB-live — export frame N while decoding N+1

**Files:**
- Test: existing `va_export_client` is all-IDR (no DPB). Add `tests/va_expbuf_pframes.c` **or** run ffmpeg+a tiny C sampler. Prefer extending a small client in the worktree.
- Clip: `verify/clips/h264_high_b.mp4` or `h264_baseline.mp4` (has P/B).

**Interfaces:**
- Consumes: Task 1 EXPBUF export without GBM copy.
- Produces: evidence that a capture index still in DPB can stay mapped by EGL while the next picture uses a *different* index.

- [ ] **Step 1: Write `tests/va_expbuf_hold.c`**

Behaviour: decode frame 0 into surface 0, `vaExportSurfaceHandle` (keep fd open, `eglCreateImage` + texture), decode frames 1..N into surfaces 1..N (or reuse other surfaces, **never** re-BeginPicture on surface 0 until the end), then `glReadPixels` Y of surface 0’s image and compare to a GetImage taken **immediately after frame 0** (saved buffer). If DPB recycle stomps the buffer, pixels change → FAIL. If hang → timeout.

Keep it short: 2 surfaces if needed — actually need surface 0 held and others for later frames. Use 4 surfaces, decode 8 frames onto 1,2,3 cycling, never overwrite 0.

Skeleton (full file in worktree; link like `va_export_client` in `meson.build`):

Reuse EGL helpers from `va_export_client.c` by copy-paste (do not refactor shipping tests). `meson.build` add:

```
va_expbuf_hold = executable('va_expbuf_hold',
  files('tests/va_expbuf_hold.c'),
  dependencies: [libva_dep, libva_drm_dep, libdrm_dep,
                 dependency('gbm'), dependency('egl'), dependency('glesv2')],
)
```

Clip: elementary H.264 with P frames. If only mp4: `ffmpeg -i verify/clips/h264_baseline.mp4 -c:v copy -bsf:v h264_mp4toannexb /tmp/base.h264` then parse NALs like `va_export_client`.

- [ ] **Step 2: Run under timeout**

```bash
export LIBVA_DRIVER_NAME=v4l2stateless LIBVA_DRIVERS_PATH=$PWD/builddir V4L2SL_EXPBUF_EXPORT=1
timeout -k 5 90 builddir/va_expbuf_hold /dev/dri/renderD128 /tmp/base.h264
```

Expected: print `HOLD_EXACT` and exit 0. Hang = stop the plan.

- [ ] **Step 3: Commit**

```bash
git add tests/va_expbuf_hold.c meson.build docs/EXPBUF-RETRY.md
git commit -m "experiment: hold EXPBUF image across later H.264 pictures (DPB live)"
```

---

### Task 3: HEVC + AV1 same export client path

**Files:**
- Reuse `va_export_client` for H.264 only; for HEVC/AV1 use ffmpeg GetImage (memcpy skip must not break hwdownload) **plus** a one-frame export sampler, or run `va_export_client` only if HEVC IDR elementary exists.

- [ ] **Step 1: ffmpeg GetImage still works in EXPBUF mode (no GBM copy)**

```bash
export LIBVA_DRIVER_NAME=v4l2stateless LIBVA_DRIVERS_PATH=$PWD/builddir V4L2SL_EXPBUF_EXPORT=1
timeout -k 5 60 ffmpeg -hide_banner -loglevel error -hwaccel vaapi \
  -hwaccel_output_format vaapi -vaapi_device /dev/dri/renderD128 \
  -i ~/vaapi-v4l2-bridge/verify/clips/hevc_main.mp4 \
  -vf hwdownload,format=nv12 -frames:v 16 -f framemd5 -y /tmp/e_hevc.md5
timeout -k 5 60 ffmpeg ... -i verify/clips/av1_aom.mp4 ... /tmp/e_av1.md5
# SW refs without hwaccel
cmp against software framemd5 for those 16 frames
```

Expected: bit-exact or known AV1 heuristic limits (use `av1_aom.mp4` 8-frame clip that already matches). Host up.

- [ ] **Step 2: If `hevc_main.mp4` can be decoded via a 1-file VA client, export one frame** — otherwise log ffmpeg-only for HEVC/AV1 and proceed; Chrome Phase 4 will hit those codecs for real.

- [ ] **Step 3: Commit notes + any client**

```bash
git commit -m "experiment: HEVC/AV1 GetImage under EXPBUF-no-memcpy; host alive"
```

---

### Task 4: Chrome zero-copy (the proof)

**Files:**
- `tests/ioctl_interpose.c` (existing)
- `/tmp/expbuf_chrome.sh` on NAS (not necessarily committed)
- Chrome: `google-chrome-stable --user-data-dir=$HOME/.config/gc-dbg --remote-debugging-port=9222`

- [ ] **Step 1: Build interpose if needed**

```bash
cd ~/vaapi-v4l2-bridge/.worktrees/expbuf-retry
# ioctl_interpose.c may already be in tests/; compile:
cc -shared -fPIC -o /tmp/ioctl_interpose.so tests/ioctl_interpose.c -ldl
```

- [ ] **Step 2: Launch Chrome with experiment .so, SIGTERM-safe**

```bash
export WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000
export LIBVA_DRIVER_NAME=v4l2stateless
export LIBVA_DRIVERS_PATH=$HOME/vaapi-v4l2-bridge/.worktrees/expbuf-retry/builddir
export V4L2SL_EXPBUF_EXPORT=1
# kill existing gc-dbg chrome first
pkill -TERM -f "user-data-dir=$HOME/.config/gc-dbg"; sleep 6
timeout -k 10 180 env LD_PRELOAD=/tmp/ioctl_interpose.so \
  google-chrome-stable --user-data-dir=$HOME/.config/gc-dbg \
  --remote-debugging-port=9222 --remote-allow-origins=* \
  --no-first-run --no-default-browser-check \
  "file:///home/liyifan/vaapi-v4l2-bridge/verify/clips/h264_baseline.mp4" \
  > /tmp/chrome_expbuf.log 2>&1
```

If `timeout` is wrong for a GUI app (it will kill Chrome at 180s — OK for the test). Alternatively run Chrome in background, CDP play 20s, then SIGTERM.

- [ ] **Step 3: Evidence**

Must collect:

```bash
grep "EXPBUF export ok" /tmp/chrome_expbuf.log | wc -l     # >0
grep "falling back" /tmp/chrome_expbuf.log | wc -l         # 0
grep PRIME /tmp/chrome_expbuf.log | head
# interpose: not memfd:v4l2sl-surf
```

CDP: `kVideoDecoderName` contains `VaapiVideoDecoder`; video `currentTime` advances; optional canvas probe non-black.

- [ ] **Step 4: bilibili AV1 or H.264 (logged-in gc-dbg)**

Navigate one known-good URL from prior soaks (e.g. HEVC/H.264 1080p first, then an AV1 BV if session allows). Same env. 60s playback. Same greps.

- [ ] **Step 5: SIGTERM Chrome, confirm stopped, dri .so mtime unchanged**

```bash
pkill -TERM -f "user-data-dir=$HOME/.config/gc-dbg"; sleep 6
pgrep -c google-chrome || echo STOPPED
stat /usr/lib/aarch64-linux-gnu/dri/v4l2stateless_drv_video.so  # still 17:13 O3 GBM
```

- [ ] **Step 6: Append Chrome verdict to `docs/EXPBUF-RETRY.md`, commit, push branch**

If Chrome **fails** (black / fallback / hang): document and **stop**. Do not flip default.

If Chrome **passes**: commit `experiment: Chrome VaapiVideoDecoder EXPBUF zero-copy (no capture memcpy)` and wait for human before touching dri or master.

---

### Task 5: Docs only (no default flip)

**Files:** `docs/EXPBUF-RETRY.md`, optionally Sphinx `docs/handbook/invariants.md` on this branch only: “EXPBUF still experimental; shipping forbids it until Chrome gate.”

- [ ] **Step 1: Write the Phase 0–4 table of PASS/FAIL**
- [ ] **Step 2: Commit and push `experiment/expbuf-retry`**
- [ ] **Step 3: Do not merge to master, do not `sudo cp` dri, unless the human says so**

---

## Abort protocol

On hang/reboot: last printed `expbuf:` / `EXPBUF export` line is the failing stage. After the machine is back, append it to `docs/EXPBUF-RETRY.md` and halt. Do not “try Chrome anyway”.
