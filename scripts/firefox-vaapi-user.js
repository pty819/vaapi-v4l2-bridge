// VA-API via v4l2stateless (H.264 / HEVC / AV1 8-bit).
// Do not force hardware decode: VP9 and 10-bit HDR are unsupported
// on this board and can hang the VPU.
user_pref("media.ffmpeg.vaapi.enabled", true);
user_pref("media.hardware-video-decoding.force-enabled", false);
user_pref("media.ffvpx.enabled", true);
user_pref("media.rdd-vpx.enabled", true);
user_pref("gfx.webrender.all", true);
user_pref("widget.dmabuf.force-enabled", true);
