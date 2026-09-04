# 内存：mmap、dma-buf、GBM、CMA

多媒体栈一半的坑在「这块内存谁能碰」。

## 四种常见 backing

| 种类 | 是什么 | 本仓库 |
|---|---|---|
| **mmap 的 V4L2 buffer** | `QUERYBUF` + `mmap`，CPU 和设备都能碰（经内核） | OUTPUT 码流、CAPTURE 解码结果 |
| **dma-buf** | 跨设备的 fd 通行证（VPU↔GPU↔显示） | **不** export VPU capture；只 export GBM bo |
| **GBM bo** | DRM 分配、通常 GPU 可 import | Chrome 零拷贝，R8 假 NV12 |
| **memfd** | 匿名文件，纯 CPU | ffmpeg GetImage；DeriveImage 的 map |

另外还有 **CMA**（Contiguous Memory Allocator）：很多 VPU 要求物理连续。CAPTURE 的 mmap 背后经常是 CMA。CMA 池小（4K 解码 `cma=256M` 不够会静默回软解——别的项目的坑）。hantro **AV1** capture 不吃 CMA，所以 AV1 池可以到 40。

## 为什么这里禁止 EXPBUF

`VIDIOC_EXPBUF` 把 V4L2 buffer 变成 dma-buf fd。理想路径：VPU 写 → GPU 直接读，零拷贝。

RK3588 主线：GPU（panthor）import VPU CMA fd 会 **IOMMU/CMA 卡死**。所以不变量第 1 条：永不 EXPBUF capture。改为：

```
VPU CAPTURE (mmap)
    memcpy 一行行
GBM bo 或 memfd
    Chrome import GBM / ffmpeg 读 memfd
```

这是「正确的慢」。学零拷贝时把 SoC 的 IOMMU 拓扑算进去，不要从 PC 的 i915 经验直接抄。

## 单 object NV12 谎言

Chrome `vaapi_wrapper`：`num_objects == 1`。真 NV12 常常是两个 plane、两个 fd。桥的做法：

- 一块 `GBM_FORMAT_R8`，高度 `h + ceil(h/2)`
- 描述符声称 NV12，UV 在 `stride * h`
- panthor 不肯给真的多平面 YUV bo；Mesa 单独 GR88 import 是黑的

这是 **显示栈**（DRM/GBM/EGL）和 **视频栈**（VA/V4L2）接缝上的产品约束，不是 H.264 语法的一部分。

## 所有权状态机（学完能看懂 copy-out）

无状态解码里，一块 CAPTURE 同时可能是：

1. 空闲（在 `free_cap_bufs`）
2. 正在被这一帧写入（QBUF 之后、DQBUF 之前）
3. 仍被硬件当作 **参考帧**（DPB）
4. 已被用户态 snapshot，但 3 还成立

ffmpeg 的 VA 客户端 surface 少，3 和 4 的寿命差不多，旧逻辑「surface 换目标就把 index 还池」够用。

Chrome WebCodecs 给每个 queued `VideoFrame` 一张 VA surface → 4 的寿命变成「播放器队列深度」，3 仍只有 ~8 个参考槽。copy-out：DQBUF 后立刻 memcpy，**3 一结束（refresh 槽被覆写）就把 index 还池**，不再等 surface 销毁。

时间戳把 3 和 VA surface 对上；`refresh_frame_flags`（解析真值）告诉你何时 3 结束。

## 自己练

```bash
# 看 Chrome 到底 import 了什么
# tests/ioctl_interpose.c LD_PRELOAD 进 Chrome
# PRIME_FD_TO_HANDLE target=/dmabuf:  → GBM 路径
# target=/memfd:v4l2sl-surf           → 退化 memfd，零拷贝没走成
```

`/proc/$pid/maps` 里出现 `v4l2stateless_drv_video.so` 才是这座桥；发行版 Chromium 不会有。
