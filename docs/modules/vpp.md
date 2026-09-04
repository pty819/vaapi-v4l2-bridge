# v4l2stateless_vpp.c — RGA

`VAProfileNone` + `VAEntrypointVideoProc`。ffmpeg `scale_vaapi`。
设备 `/dev/video0`。

支持：scale、CSC、rotate、flip。filter caps 走
`v4l2sl_vpp_query_*`。pipeline 与 JPEG 同为 `v4l2sl_m2m_state`。

输入像素来自源 surface 的 `cap_view`（EXPBUF 模式）、cpu_ptr 或
memfd。VPP 不把 GBM bo 当源，除非先 `ensure_memfd`。
输出写 dst surface 的 `cpu_ptr`，置 `has_pic` 和
`last_writer = CPU`，再按需 sync 到 bo。

GetImage 必须优先读这份 `cpu_ptr`：create-time memfd 在 VPP 之后仍是
空的（2026-09-04 修过；矩阵曾经 8 个 scale 哈希全相同）。
