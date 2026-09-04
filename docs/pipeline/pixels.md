# 像素住哪

默认路径：`VIDIOC_EXPBUF` 把 VPU CAPTURE 变成单 object NV12 dma-buf，
Chrome EGL 直接 import。不再每帧 `memcpy` 进 GBM bo。
`V4L2SL_EXPBUF_EXPORT=0` 或 EXPBUF ioctl 失败才走 GBM 拷贝回退。

实验记录：[EXPBUF-RETRY.md](https://github.com/pty819/vaapi-v4l2-bridge/blob/master/docs/EXPBUF-RETRY.md)
（换电源后重测；旧「CMA/IOMMU 一 EXPBUF 就挂」与欠功率电源同期）。

```{mermaid}
flowchart TB
  VPU[VPU CAPTURE CMA]
  VPU -->|默认 VIDIOC_EXPBUF| EGL[Chrome EGL num_objects=1]
  VPU -->|GetImage 读 cap_view mmap| GI[ffmpeg hwdownload]
  VPU -->|opt-out =0 或 ioctl 失败| BO[GBM R8 bo 假 NV12]
  BO --> EGL
  CPU[cpu_ptr] -->|PutImage / VPP dst| GI
  CPU --> JPEG[JPEG 编码源]
```

## last_writer

`enum v4l2sl_last_writer`：`NONE / CPU / MEMFD / BO`。
`v4l2sl_memfd_stale(s)` ≡ `last_writer == BO`。

| 谁写 | backing | 典型生产者 | 典型消费者 |
|---|---|---|---|
| MEMFD | capture mmap（`cap_view`）或 grow-only memfd | EXPBUF 的 `pull_capture`；无 bo 时的快照 | GetImage / Derive / Chrome Export |
| BO | GBM | `V4L2SL_EXPBUF_EXPORT=0` 的 `pull_capture` 上传；`gbm_surface_sync` | 回退路径的 Chrome Export |
| CPU | `cpu_ptr` 懒 malloc | `vaPutImage`、VPP 输出 | JPEG、VPP 输入、GetImage（优先于空 memfd） |

GetImage 顺序（2026-09-04 修过空 memfd 谎）：

1. EXPBUF 模式且 `surf->cap_view` → 读 capture mmap
2. `last_writer == CPU && cpu_ptr` → 读 VPP / PutImage 结果
3. 否则 `ensure_memfd` / memfd

## has_pic vs buf_index

- `has_pic`：已经有一帧像素（capture mmap、bo 或 cpu_ptr）
- `buf_index`：这个 surface **现在**是否还占着一个内核 CAPTURE 槽
- `cap_view`：该槽的 mmap 指针（ffmpeg 动态池不在 `render_targets` 里，
  GetImage 不能靠 context 反查）

AV1 copy-out 之后：`buf_index = -1`，`has_pic` 仍为 1。GetImage /
Derive 必须看 `has_pic`，不能看 `buf_index >= 0`。

Chrome 在第一帧解码**之前**就 Export 整个 surface 池（`buf_index=-1`）。
EXPBUF 模式在首次 Export 时 `claim` 一个 capture 槽，`decode_submit`
复用同一 index；`begin_picture` 在 EXPBUF 开启时不回收该槽。

## GBM 约束（`v4l2stateless_gbm.c`，回退路径）

panthor 不给多平面 YUV bo。回退做法：

- `gbm_bo_create(w, h+ceil(h/2), GBM_FORMAT_R8, LINEAR)`
- 描述符撒谎成 NV12：Y 在 0，UV 在 `stride*h`，**一个** dma-buf
- 不要把 GR88 当主 image（Mesa 导入是黑的）
- **只服务 NV12**。`cap_fourcc != NV12` 或 `format != VA_FOURCC_NV12`
  → `gbm_surface_ensure` 失败，Export 走 unimplemented，客户端软解

10-bit（NV15/P010）走 CPU GetImage，不走这条。Chrome 网页 10-bit
同样不走 VA 零拷贝。

## memfd

- 创建时 `ftruncate` 一块，只增不减
- mapping 持久；`DeriveImage` 用 borrow 计数
- 增长时旧 mapping 进 `memfd_retired`，等最后一个 derived image 销毁再 munmap
- `ensure_memfd`：bo 是权威时，按 **可见行** 从 bo 拷回（不读 padding）
- EXPBUF 热路径 GetImage **不读** 这块（读 `cap_view`）

## GetImage 转换（`v4l2stateless_format.c`）

| capture fourcc | GetImage 目标 |
|---|---|
| NV12 | NV12 行拷 |
| NV15 | P010（5 字节 → 两个 10-bit，低 6 位置 0） |
| NV16 | YUY2 |
| NV20 | YUY2 或 Y210 |

ffmpeg `hwdownload,format=p010le` 走 NV15→P010。这就是 High10/Main10
命令行能 bit-exact、Chrome 不能上屏的原因：Chrome 从不 GetImage。
