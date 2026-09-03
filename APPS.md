# Desktop apps: Chrome, Firefox, VLC

How to make GUI players talk to this libva backend (`v4l2stateless_drv_video.so`)
instead of a real i915/AMD VA driver or a vendor MPP stack.

This is the RK3588 / Orange Pi 5 setup used on the development NAS. Codec
support and the `hwdownload` success rule are in [README.md](README.md).

Last verified **2026-09-03** on the development NAS (Chrome live hw decode + visible picture on bilibili 1080p): `LIBVA_DRIVER_NAME=v4l2stateless` is in `~/.config/environment.d/90-libva.conf`; Chrome menu uses `/usr/local/bin/google-chrome-stable` (a copy of the wrapper); Firefox is Mozilla `.deb` 154.0.1 with `user.js` in both profiles; VLC `avcodec-hw=vaapi`.


## One environment variable for everyone

Every VA-API client must load **this** driver, not `panthor_drv_video.so`:

```bash
export LIBVA_DRIVER_NAME=v4l2stateless
```

Persist it for graphical sessions (systemd user environment, not only `~/.bashrc`):

```bash
# ~/.config/environment.d/90-libva.conf
LIBVA_DRIVER_NAME=v4l2stateless
```

and/or `~/.profile`. Then:

```text
app  →  libva  →  v4l2stateless_drv_video.so  →  /dev/video* (rkvdec / hantro)
```

Install the `.so` first (see README). Check:

```bash
LIBVA_DRIVER_NAME=v4l2stateless vainfo --display drm --device /dev/dri/renderD128
```

You should see `v4l2stateless` and H.264 / HEVC / AV1 VLD. If `vainfo` loads
`panthor`, the env var is not reaching that process.

**Do not** treat a silent software fallback as success (ffmpeg does that).
VP9 is not implemented here; forcing it can wedge the VPU.

| App | Speaks | Path on this board |
|---|---|---|
| ffmpeg / VLC | VA-API | libva → this `.so` |
| Firefox (Linux) | FFmpeg VA-API hwaccel | same |
| Official Linux **Chrome** arm64 | VA-API (`VaapiWrapper`) | same, **plus** extra flags (below) |
| Debian/XtraDeb **Chromium** arm64 | V4L2-stateless compiled in | **bypasses** this `.so`, ioctls `/dev/video*` directly |

Official Chrome and distro Chromium are different binaries. You cannot flip
Chrome onto Chromium's V4L2 path with a flag.

---

## Chrome (official `.deb`, VA-API)

Google's Linux arm64 Chrome is built with `use_vaapi=true`. Three extra
problems on RK3588:

1. DRM probe **skips non-PCI** devices. panthor is a platform device, so Chrome
   reports `failed to find a suitable render node` unless you pass
   `--render-node-override=/dev/dri/renderD128`.
2. The GPU process must `open(/dev/videoN)`. The sandbox blocks that, so
   `--disable-gpu-sandbox` is required for decode (not for browsing).
3. The GL stack. Only **pure Wayland + ANGLE/GLES with Vulkan disabled**
   initializes a GL context on panthor. X11, `--ozone-platform-hint=auto`,
   or leaving Vulkan on all fail with
   `ANGLE Display::initialize error 12289: Could not create a backing OpenGL
   context` — pages never paint (YouTube stuck on "Loading…", local video
   shows a black frame). The wrapper therefore forces
   `--ozone-platform=wayland --use-gl=angle --use-angle=gles
   --disable-features=Vulkan`. Verified 2026-09-02: zero ANGLE errors and
   1080p H.264 decoding through the bridge (see Check below).

The driver side of Chrome's `FillProfileInfo_Locked` (RTFormat / `num_attribs`)
is already handled in `src/v4l2stateless.c`.

### Wrapper

[`scripts/google-chrome-vaapi`](scripts/google-chrome-vaapi) prepends the flags
and sets `LIBVA_DRIVER_NAME`. Install:

```bash
sudo install -m 0755 scripts/google-chrome-vaapi /usr/local/bin/google-chrome-stable
```

Single-file install (no symlinks). The former `google-chrome-vaapi` +
`google-chrome` symlink pair was removed 2026-09-03: a stale copy had been
shadowing `google-chrome` with the ZeroCopyGL-disabling flag, which silently
drops Chrome to FFmpeg software. Re-run the install line after editing the
wrapper.

`/usr/local/bin` is ahead of `/usr/bin` on PATH, so a terminal
`google-chrome-stable` also hits the wrapper. Menu launchers often hardcode
`/usr/bin/google-chrome-stable`; override that with a user desktop file
(`~/.local/share/applications/google-chrome.desktop`) whose `Exec=` is:

```text
Exec=/usr/local/bin/google-chrome-stable %U
```

Copy the same `Exec=` for the New Window / Incognito actions. Then:

```bash
update-desktop-database ~/.local/share/applications
xdg-settings set default-web-browser google-chrome.desktop
```

An apt upgrade of `google-chrome-stable` overwrites
`/usr/share/applications/google-chrome.desktop` and `/usr/bin/google-chrome-stable`.
It does **not** touch `/usr/local/bin/google-chrome-stable` or the user `.desktop`.

Do **not** start `/usr/bin/google-chrome-stable` or `/opt/google/chrome/google-chrome`
by full path if you want hardware decode.

### Check

Open `chrome://gpu` → **Video Acceleration Information**. H.264 / HEVC
should list Hardware (AV1 is not advertised by the bridge). Start with 1080p;
4K HDR / VP9 will not use this bridge.

Hardware decode is a resolution-dependent choice: a 320x240 QCIF clip decodes
with `FFmpegVideoDecoder` (software) even when everything works. Use a 1080p
clip and confirm in `chrome://media-internals`:

```text
kVideoDecoderName          VaapiVideoDecoder
kIsPlatformVideoDecode     true
```

The driver side prints `H.264 config uses /dev/videoN` per session. Per-buffer
`capture mmap idx=` lines are gated behind `V4L2SL_DEBUG=1`. Verified
2026-09-03 live on bilibili 1080p: `VaapiVideoDecoder` + non-black canvas
frames + GPU process holds the rkvdec node + zero `eglCreateImage` errors.

### Zero-copy display path (GBM surfaces, since 2026-09-03)

Chrome's zero-copy GL path (default `AcceleratedVideoDecodeLinuxZeroCopyGL`)
works end to end. The driver exports every Chrome-visible surface as a
driver-owned **linear GBM bo** on `/dev/dri/renderD128` — a real
single-object NV12 dma-buf (Y@0, UV@stride*h; Chromium rejects
`num_objects != 1`) — while the VPU/CMA buffers are still never exported
(the EXPBUF chip-bug ban is untouched). Verified live on bilibili 1080p:
`VaapiVideoDecoder`, zero `eglCreateImage` errors, non-black frames,
~44 `MEDIA_REQUEST_IOC_QUEUE`/s on rkvdec.

Do **not** disable `AcceleratedVideoDecodeLinuxZeroCopyGL`: that pushes
VaapiVideoDecoder into the ImageProcessor output path, which does not exist
on this platform; Chrome then silently drops to FFmpeg software for the
whole session (it caches the first VA failure and never retries Vaapi
in-session).

Operating notes:

- **Restart Chrome after updating the driver `.so`.** Same failure cache as
  above: a session that saw a broken driver stays on software until
  relaunch, even after you fix and reinstall the driver.
- **10-bit / 4:2:2 content decodes in software.** The GBM export is
  NV12-only for now (10-bit would need an R16-bo P010 layout). 8-bit 4:2:0
  H.264 / HEVC / VP8 goes through the VPU.
- GBM allocation follows `V4L2SL_RENDER_NODE` (default
  `/dev/dri/renderD128`). A failed GBM init degrades export to
  `VA_STATUS_ERROR_UNIMPLEMENTED` (clean software fallback), never breaks
  the decode path.
- Black-video triage: `LD_PRELOAD` `tests/ioctl_interpose.so` into Chrome
  and read stderr — `PRIME_FD_TO_HANDLE target=/dmabuf:` is the good path;
  `target=/memfd:v4l2sl-surf` means the memfd export lie came back.
- **Chrome is pinned to the big cores (4-7)** by the wrapper
  (`taskset -c 4-7`): this kernel has no EAS, so the plain scheduler was
  spreading Chrome's frame-deadline threads (GPU process, video renderer)
  evenly onto the A55s, costing frame pacing. Override with
  `CHROME_PIN_CPUS=0-7` (or empty) to disable; launching under your own
  `taskset` is respected. kwin/plasma are deliberately left unpinned —
  steady state parks them on the now-idle little cores, and compositing
  bursts can still borrow big cores.

---

## Firefox (deb from packages.mozilla.org, not the Ubuntu snap stub)

Linux Firefox with `media.ffmpeg.vaapi.enabled` uses **system FFmpeg's VA-API
hwaccel**, the same stack this driver was written against. It does **not** run
Chrome's `FillProfileInfo_Locked` checks.

Ubuntu's `firefox` package is often `1:1snap1` (a snap wrapper). Prefer Mozilla's
apt repo (`https://packages.mozilla.org/apt`) and pin it above Ubuntu:

```text
Package: *
Pin: origin packages.mozilla.org
Pin-Priority: 1000

Package: firefox*
Pin: release o=Ubuntu
Pin-Priority: -1
```

Copy [`scripts/firefox-vaapi-user.js`](scripts/firefox-vaapi-user.js) to the
**active** profile's `user.js` (see `about:profiles`):

```bash
cp scripts/firefox-vaapi-user.js ~/.mozilla/firefox/<profile>/user.js
```

`media.hardware-video-decoding.force-enabled` stays **false**. YouTube 4K is
often VP9; forcing hardware there has hung this VPU. H.264 / HEVC / AV1 8-bit
will still pick VA-API when the env var is set.

Graphical Firefox must see `LIBVA_DRIVER_NAME` from `environment.d` / PAM,
not only from a bashrc that login never sources.

### Check

`about:support` → **Codec Support** / hardware decoding. Process logs should
show `v4l2stateless: opened /dev/video` and FFmpeg `VAAPI_VLD`, not only
software.

---

## VLC

VLC's avcodec module can use VA-API when FFmpeg is built with it (Debian/Ubuntu
`vlc` + distro ffmpeg).

1. Environment, same as above: `LIBVA_DRIVER_NAME=v4l2stateless`.
2. Tools → Preferences → Input / Codecs → Hardware-accelerated decoding →
   **VA-API video decoder**, or in `~/.config/vlc/vlcrc`:

   ```text
   avcodec-hw=vaapi
   ```

3. Optional menu override so the GUI always exports the driver name:

   ```text
   Exec=env LIBVA_DRIVER_NAME=v4l2stateless /usr/bin/vlc --started-from-file %U
   ```

   in `~/.local/share/applications/vlc.desktop`.

Codec → **Codec information** while playing: you want a VA-API / hardware
decoder, not `avcodec` software. Same VP9 / 10-bit HDR caveat as Firefox.

---

## ffmpeg (ground truth)

```bash
export LIBVA_DRIVER_NAME=v4l2stateless
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device /dev/dri/renderD128 -i FILE \
  -vf "hwdownload,format=nv12" -pix_fmt yuv420p -frames:v 8 -f framemd5 -
```

Compare to a software decode of the same file. The matrix script is
`tests/run_full_matrix.sh`.

---

## What this board will not hardware-decode

- **VP9** — no VP9 OUTPUT fourcc on the mainline hantro node here
- **AV1 / HEVC / H.264 10-bit HDR playback in browsers** — do not force it
- Mid-stream resolution changes in Chrome/Firefox — poorly tested
- Chromium-with-V4L2: it never opens this `.so`; that is expected
