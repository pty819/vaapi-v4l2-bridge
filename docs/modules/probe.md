# v4l2stateless_probe.c — 按 fourcc 找节点

`/dev/videoN` 重启会换号。匹配规则是 OUTPUT_MPLANE fourcc，不是路径。

## 编码 fourcc

`v4l2sl_codec_coded_fourcc`：

| codec | fourcc |
|---|---|
| H.264 | `H264_SLICE`（有的内核是 `S264`） |
| HEVC | `HEVC_SLICE` |
| AV1 | `AV1_FRAME` |
| VP8 | `VP8_FRAME` / `VP8F` |
| MPEG-2 | `MPEG2_SLICE` / `MG2S` |

`v4l2sl_pick_device_for_codec`：第一个 OUTPUT 列表含该 fourcc、且
`request_api != NO` 的节点。

## request API 过滤

有的 `/dev/video*` 广告了 AV1 fourcc 但 sibling media 不能
`MEDIA_IOC_REQUEST_ALLOC`（stub）。必须跳过，否则 create_context 会打开
一个永远 QUEUE 失败的 fd。单元测试覆盖「stub 在前、真节点在后」。

## 缓存

`v4l2sl_scan_all_cached`：

- 路径 `$XDG_RUNTIME_DIR/v4l2stateless-probe.cache`
- 文件头写 `boot_id`，对不上就当 miss
- 一次扫描填齐 h264/hevc/av1/vp8/mpeg2/jpeg/vpp

JPEG / VPP 不是 request API，探测逻辑在 `scan_all_uncached` 里按
OUTPUT/CAPTURE 能力另认（VEPU mjpeg、RGA nv12 等）。
