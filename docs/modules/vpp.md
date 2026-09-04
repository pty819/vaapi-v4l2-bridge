# v4l2stateless_vpp.c — RGA

`VAProfileNone` + `VAEntrypointVideoProc`。ffmpeg `scale_vaapi`。
设备 `/dev/video0`。

支持：scale、CSC、rotate、flip。filter caps 走
`v4l2sl_vpp_query_*`。pipeline 与 JPEG 同为 `v4l2sl_m2m_state`。

输入像素来自源 surface 的 cpu/memfd（VPP 不读 GBM bo 当源，除非先
`ensure_memfd`）。输出写 dst surface 的 cpu_ptr，再按需 sync 到 bo。
