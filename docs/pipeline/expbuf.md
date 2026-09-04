# EXPBUF 零拷贝（默认路径）

Chrome 出画不再把整帧 capture **memcpy** 进 GBM bo。
`vaExportSurfaceHandle` 对 VPU CAPTURE 做 `VIDIOC_EXPBUF`，交出
单 object NV12 dma-buf（Y@0，UV@stride×aligned_h，`num_objects==1`）。

关闭：`V4L2SL_EXPBUF_EXPORT=0`（恢复 GBM 拷贝）。
Chrome GPU 进程会丢掉未知环境变量；实验期用过
`$XDG_RUNTIME_DIR/v4l2sl-expbuf` 文件旗标。量产默认开，不必再设。

实验室梯子与分阶段记录：[EXPBUF-RETRY.md](https://github.com/pty819/vaapi-v4l2-bridge/blob/master/docs/EXPBUF-RETRY.md)。

## 为什么以前禁止

2026-09-02 的结论是「EXPBUF + panthor import 会把 RK3588 CMA/IOMMU 挂死」，
所以量产走 memfd / GBM 拷贝。那次挂机与 **欠功率电源** 同期。
换电源后空闲 ladder、解码后 EGL 采样、DPB-live hold、Chrome
VaapiVideoDecoder、B 站直播、全量矩阵全部活着。

旧禁令不再是不变量。审计文档 `refactor-audit-2026-09-03.md` 里
「永不 EXPBUF」是当时的快照，以本页和 {doc}`/handbook/invariants` 为准。

## 一帧怎么走

```{mermaid}
sequenceDiagram
  participant Chrome
  participant SO as drv_video.so
  participant VPU as rkvdec / hantro
  Chrome->>SO: vaCreateSurfaces + vaExportSurfaceHandle
  Note over SO: buf_index 仍是 -1<br/>claim 一个 capture 槽并 EXPBUF
  Chrome->>SO: vaBeginPicture / Render / EndPicture
  SO->>VPU: decode_submit 复用已 claim 的 index
  VPU-->>SO: DQBUF，pull_capture 只记账 cap_view<br/>不 memcpy 进 GBM
  Chrome->>SO: 之后的 Export 再 EXPBUF 同一 index
```

要点：

- Chrome **先 Export 再解码**。没有 claim-at-export 就会对空槽 EXPBUF。
- `begin_picture` 在 EXPBUF 开启时 **不** 把旧 capture index 还池，
  否则 Chrome 还握着 fd、下一帧却拿到另一块 CMA。
- `pull_capture` 置 `has_pic` / `buf_index` / `cap_view`，跳过
  `gbm_surface_upload`。
- GetImage（ffmpeg）读 `surf->cap_view`，因为 ffmpeg 动态 surface
  不在 context `render_targets` 里。

## 还在拷的部分

拿掉的是 **显示热路径** 那次 capture→GBM 整帧拷。仍然存在：

| 拷贝 | 谁 | 为什么还在 |
|---|---|---|
| 压缩码流 → OUTPUT mmap | 每帧 decode | V4L2 合同：OUTPUT 是驱动提供的槽 |
| vaGetImage / Derive | ffmpeg hwdownload | VA-API CPU 读回合同 |
| VPP dst → cpu_ptr | `scale_vaapi` | RGA 输出给 CPU 读 |
| GBM 回退 | `=0` 或 EXPBUF 失败 | 兼容旧路径 |

## 验证（系统 dri，2026-09-04）

HEAD `cbcead7`。系统
`/usr/lib/aarch64-linux-gnu/dri/v4l2stateless_drv_video.so`
md5 `2f378b1a1d3391f5fe231e021f77be1e`。

- `va_export_client` 8× `EXPBUF export ok`，EXPORT/LAZY_EXACT
- `va_expbuf_hold` HOLD_EXACT（持 fd 再解 7 张后续图）
- ffmpeg H.264 / HEVC / AV1 hwdownload MATCH 软件
- Chrome 本地 1080p：22× EXPBUF，0 GBM upload，VaapiVideoDecoder，canvas avg=123
- B 站直播 ~60s：23× EXPBUF，canvas avg 58–90
- `tests/run_full_matrix.sh` **PASS=32 FAIL=0**，VPP 8 个不同 scale hash
  （修过 GetImage 读空 create-time memfd 的谎）
- `V4L2SL_EXPBUF_EXPORT=0` 仍能回到 GBM 拷贝

回退文件：`v4l2stateless_drv_video.so.gbm-20260904-1713`。
