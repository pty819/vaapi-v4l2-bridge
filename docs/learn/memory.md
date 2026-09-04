# 内存：mmap、dma-buf、GBM、CMA

多媒体栈一半的坑在「这块内存谁能碰」。

## 四种常见 backing

| 种类 | 是什么 | 本仓库 |
|---|---|---|
| **mmap 的 V4L2 buffer** | `QUERYBUF` + `mmap`，CPU 和设备都能碰（经内核） | OUTPUT 码流、CAPTURE 解码结果 |
| **dma-buf** | 跨设备的 fd 通行证（VPU↔GPU↔显示） | **默认** `VIDIOC_EXPBUF` VPU capture 给 Chrome；GBM bo 是回退 |
| **GBM bo** | DRM 分配、通常 GPU 可 import | `V4L2SL_EXPBUF_EXPORT=0` 时的 R8 假 NV12 |
| **memfd** | 匿名文件，纯 CPU | ffmpeg GetImage 的回退；DeriveImage 的 map |

另外还有 **CMA**（Contiguous Memory Allocator）：很多 VPU 要求物理连续。CAPTURE 的 mmap 背后经常是 CMA。CMA 池小（4K 解码 `cma=256M` 不够会静默回软解——别的项目的坑）。hantro **AV1** capture 不吃 CMA，所以 AV1 池可以到 40。

## 默认 EXPBUF（换电源后）

`VIDIOC_EXPBUF` 把 V4L2 buffer 变成 dma-buf fd。理想路径：VPU 写 → GPU 直接读，零拷贝。这就是现在 Chrome 出画的热路径。

2026-09-02 曾经禁止：当时 panthor import VPU CMA fd 会把整机挂死。
那次与 **欠功率电源** 同期。换电源后梯子、解码、Chrome、B 站、矩阵
全部活着，量产改回默认 EXPBUF。细节见 {doc}`/pipeline/expbuf`。

```
VPU CAPTURE (CMA, mmap)
    │  默认 VIDIOC_EXPBUF ──► Chrome EGL（单 object NV12）
    │  GetImage 读 cap_view mmap（ffmpeg，无 GBM 拷）
    └  V4L2SL_EXPBUF_EXPORT=0 ──► memcpy 进 GBM / memfd
```

学零拷贝时仍要把 SoC 的 IOMMU 拓扑算进去：这条路在这颗板、这个电源
上验证过，不是「所有 RK3588 永远安全」的数学证明。挂了先查电源，
再用 `=0` 回退。

## 单 object NV12

Chrome `vaapi_wrapper`：`num_objects == 1`。真 NV12 常常是两个 plane、两个 fd。

EXPBUF 路径：一个 capture fd，描述符声称 NV12，UV 在 `stride * aligned_h`。
GBM 回退路径：一块 `GBM_FORMAT_R8`，高度 `h + ceil(h/2)`，同样一个 fd。

panthor 不肯给真的多平面 YUV bo；`gbm_bo_import` 真 NV12 fourcc 仍
`EINVAL`。这是 **显示栈** 和 **视频栈** 接缝上的产品约束。

## 所有权状态机（学完能看懂 copy-out）

无状态解码里，一块 CAPTURE 同时可能是：

1. 空闲（在 `free_cap_bufs`）
2. 正在被这一帧写入（QBUF 之后、DQBUF 之前）
3. 仍被硬件当作 **参考帧**（DPB）
4. 已被用户态 snapshot / Export，但 3 还成立

ffmpeg 的 VA 客户端 surface 少，3 和 4 的寿命差不多，旧逻辑「surface 换目标就把 index 还池」够用。

Chrome WebCodecs 给每个 queued `VideoFrame` 一张 VA surface → 4 的寿命变成「播放器队列深度」，3 仍只有 ~8 个参考槽。AV1 copy-out：DQBUF 后像素已经在用户态（GBM/memfd 或 cap_view），**3 一结束（refresh 槽被覆写）就把 index 还池**，不再等 surface 销毁。

EXPBUF + Chrome 还多一档：Export 发生在解码前，surface 必须先 **claim**
一个 capture 槽，解码复用同一 index，`begin_picture` 不得还池。

时间戳把 3 和 VA surface 对上；`refresh_frame_flags`（解析真值）告诉你何时 3 结束。

## 自己练

```bash
# 看 Chrome 到底 import 了什么
# tests/ioctl_interpose.c LD_PRELOAD 进 Chrome
# PRIME_FD_TO_HANDLE target=/dmabuf:  → EXPBUF（或 GBM 回退）
# target=/memfd:v4l2sl-surf           → 退化 memfd，零拷贝没走成
```

`/proc/$pid/maps` 里出现 `v4l2stateless_drv_video.so` 才是这座桥；发行版 Chromium 不会有。
