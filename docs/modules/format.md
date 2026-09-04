# v4l2stateless_format.c — 像素格式

纯函数，无 fd。CreateSurfaces、GetImage、ensure_capture 都问它。

## fourcc 映射

- `v4l2sl_capture_fourcc_from_rt(rt_format)`：VA RT → NV12/NV15/NV16/NV20
- `v4l2sl_capture_fourcc_from_sps(bit_depth_minus8, chroma_idc)`：SPS
- `v4l2sl_va_fourcc_for_capture`：NV15→P010，NV16/NV20→YUY2，否则 NV12
- `v4l2sl_capture_plane_size`：含对齐高度；NV15/NV20 按 10-bit packed

## 转换

NV15 是 10-bit packed：4 个 10-bit 样本占 5 字节。`unpack_le40_to_p010`
扩成 P010（16-bit 小端，有效位在高 10）。这是 ffmpeg High10/Main10
`hwdownload,format=p010le` 的唯一路径。

NV20 类似，给 4:2:2 10-bit。Y210 路径存在但 Mesa GR88 导入黑，浏览器用不上。

## Annex-B

`annexb_missing_prefix`：H.264/HEVC 有的客户端交无起始码的 NAL。
翻译器按需补 `00 00 00 01`。
