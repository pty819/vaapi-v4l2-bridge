# 像素住哪

VPU 的 CAPTURE buffer **永远不能** `EXPBUF` 给 GPU（RK3588 CMA/IOMMU 会死机）。
每帧必须 snapshot 到驱动自己的 backing。

```{mermaid}
flowchart TB
  VPU[VPU CAPTURE CMA 禁止 EXPBUF]
  VPU -->|pull_capture memcpy 一次| DEC{surface 有 gbm_bo?}
  DEC -->|是 显示面| BO[GBM R8 bo 假 NV12]
  DEC -->|否| MF[memfd 快照]
  BO -->|vaExportSurfaceHandle| EGL[Chrome EGL num_objects=1]
  BO -->|ensure_memfd 按需| MF2[memfd 从 bo 回填]
  MF --> GI[vaGetImage / Derive]
  MF2 --> GI
  CPU[cpu_ptr] -->|PutImage / VPP dst| BO
  CPU --> JPEG[JPEG 编码源]
```

## last_writer

`enum v4l2sl_last_writer`：`NONE / CPU / MEMFD / BO`。
`v4l2sl_memfd_stale(s)` ≡ `last_writer == BO`。

| 谁写 | backing | 典型生产者 | 典型消费者 |
|---|---|---|---|
| BO | GBM | `pull_capture` 上传；`gbm_surface_sync` | Chrome Export |
| MEMFD | grow-only memfd | 无 bo 时的 `pull_capture`；或 `ensure_memfd` | GetImage / Derive / VPP 源 |
| CPU | `cpu_ptr` 懒 malloc | `vaPutImage`、VPP 输出 | JPEG、VPP 输入 |

## has_pic vs buf_index

- `has_pic`：已经有一帧快照（bo 或 memfd）
- `buf_index`：这个 surface **现在**是否还占着一个内核 CAPTURE 槽

AV1 copy-out 之后：`buf_index = -1`，`has_pic` 仍为 1。GetImage /
Derive 必须看 `has_pic`，不能看 `buf_index >= 0`（曾经因此在 nonref
帧上对 ffmpeg 报 `INVALID_SURFACE`）。

## GBM 约束（`v4l2stateless_gbm.c`）

panthor 不给多平面 YUV bo。做法：

- `gbm_bo_create(w, h+ceil(h/2), GBM_FORMAT_R8, LINEAR)`
- 描述符撒谎成 NV12：Y 在 0，UV 在 `stride*h`，**一个** dma-buf
- 不要把 GR88 当主 image（Mesa 导入是黑的）
- **只服务 NV12**。`cap_fourcc != NV12` 或 `format != VA_FOURCC_NV12`
  → `gbm_surface_ensure` 失败，Export 走 unimplemented，客户端软解

10-bit（NV15/P010）走 CPU GetImage，不走这条。

## memfd

- 创建时 `ftruncate` 一块，只增不减
- mapping 持久；`DeriveImage` 用 borrow 计数
- 增长时旧 mapping 进 `memfd_retired`，等最后一个 derived image 销毁再 munmap
- `ensure_memfd`：bo 是权威时，按 **可见行** 从 bo 拷回（不读 padding）

## GetImage 转换（`v4l2stateless_format.c`）

| capture fourcc | GetImage 目标 |
|---|---|
| NV12 | NV12 行拷 |
| NV15 | P010（5 字节 → 两个 10-bit，低 6 位置 0） |
| NV16 | YUY2 |
| NV20 | YUY2 或 Y210 |

ffmpeg `hwdownload,format=p010le` 走 NV15→P010。这就是 High10/Main10
命令行能 bit-exact、Chrome 不能上屏的原因：Chrome 从不 GetImage。
