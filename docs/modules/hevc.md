# v4l2stateless_hevc.c

与 H.264 同构：SPS/PPS/DecodeParams。WPP 多 slice。

## 几何时序

HEVC 常在第一帧 SPS 才知道真实高度（1080 → 1088 对齐）。
`streamed` 标志让 STREAMON 发生在 `ensure_capture` 之后。
Main10：先 NV12 建队，SPS 后再 NV15。

## Chrome

8-bit Main 1080p 网页硬解已验证。Main10 在 **Chrome demux** 被
`DEMUXER_ERROR_NO_SUPPORTED_STREAMS` 丢掉，到不了本文件。ffmpeg 矩阵
Main10 是绿的。
