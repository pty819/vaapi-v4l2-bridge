# Linux 多媒体地图

Linux 上「播一段视频」会穿过好几层，名字容易混。先把层分清，再谈 VA 和 V4L2 各自站哪。

## 五层

```{mermaid}
flowchart TB
  subgraph L1 [应用]
    A[播放器 / 浏览器]
  end
  subgraph L2 [解码 API 用户态]
    VA[VA-API libva]
    VD[VDPAU]
    V4U[V4L2 用户态直接 ioctl]
  end
  subgraph L3 [驱动胶水]
    THIS[vaapi-v4l2-bridge]
    MESA[Mesa VA 后端 i965/radeonsi]
    MPP[Rockchip MPP 厂商]
  end
  subgraph L4 [内核子系统]
    V4K[V4L2]
    MC[Media controller]
    DRM[DRM]
  end
  subgraph L5 [硬件]
    VPU[VPU]
    GPU[GPU]
    DIS[HDMI / 面板]
  end
  A --> VA & VD & V4U
  VA --> THIS & MESA
  THIS --> V4K & MC
  MESA --> DRM
  V4U --> V4K
  MPP --> V4K
  V4K --> VPU
  DRM --> GPU --> DIS
```

- **VA-API**：Intel 发起、现在跨厂商的 *用户态* API。头文件 `<va/va.h>`。不认识 `/dev/video*`。
- **V4L2**：内核 *设备* API。头文件 `<linux/videodev2.h>`。摄像头、编解码器、scaler 都用它。
- **Media controller**：V4L2 旁边的图 API（`/dev/media*`），描述 SoC 内部 pipeline；无状态解码的 **request fd** 从这里分配。
- **DRM/KMS**：显示。GPU 提交、连接器、framebuffer。dma-buf 是它和 V4L2 交换内存的通行证。
- **GBM**：DRM 上分配 buffer 的用户态库。Chrome 要的「能 import 的 NV12」就是一座 GBM bo。

这座桥站在 **L3**：对上假装是 Mesa 那种 `*_drv_video.so`，对下当一个守规矩的 V4L2 Request 客户端。

## 无状态 vs 有状态（两种 VPU 用法）

解码器硬件有两种合同：

| | 有状态（stateful） | 无状态（stateless） |
|---|---|---|
| 谁记参考帧 | 硬件 / 内核 | **用户态**（DPB 表、timestamp） |
| 每帧交什么 | 一串 Annex-B 字节，设备自己找起始码 | 已经解析好的控制块 + 这一帧的 slice/tile |
| 典型设备 | 旧 SoC 编解码器、JPEG、RGA | 主线 rkvdec、hantro G2 |
| V4L2 形态 | 普通 M2M：OUTPUT 进码流，CAPTURE 出图 | OUTPUT + CAPTURE + **request** 绑控制 |

本仓库两种都用：H.264/HEVC/AV1/VP8/MPEG-2 是无状态 Request；JPEG 编码和 RGA VPP 是有状态 M2M。`vaEndPicture` 按 codec 分开，不能写进同一个提交函数。

## 一次播放在板上的物理设备（Orange Pi 5 / RK3588 主线）

这些号会随探测变化，以 fourcc 为准，数字只是这份 STATE 里常见的：

| 节点 | 角色 | 本仓库谁打开 |
|---|---|---|
| `/dev/video1` | rkvdec，H.264 / HEVC slice | h264.c / hevc.c |
| `/dev/video4` + `/dev/media3` | hantro AV1 | av1.c |
| `/dev/video2` | hantro VP8 / MPEG-2 | vp8.c / mpeg2.c |
| `/dev/video3` | VEPU JPEG 编码 | jpeg.c |
| `/dev/video0` | RGA scaler | vpp.c |
| `/dev/dri/renderD128` | panthor GPU（GBM、libva DRM 显示） | gbm.c、vaInitialize |

**探测**在 `v4l2stateless_probe.c`：扫 `/dev/video0..63` 的 OUTPUT fourcc，而不是写死 video 号。这是学 V4L2 的第一课——节点号不稳定，fourcc 才稳定。

## 内存不能想当然

RK3588 上 VPU 的 CAPTURE 往往在 **CMA**。把这块 buffer `EXPBUF` 给 GPU import，这颗芯片会 IOMMU 卡死。所以桥的合同是：

- VPU 解完 → 用户态 **memcpy 一份** 到 GBM（给 Chrome）或 memfd（给 ffmpeg）
- 永远不把 VPU 的 dma-buf 交给 EGL

学 dma-buf 零拷贝时，记住：**零拷贝是优化，不是义务**；有的 SoC 上拷一次才是正确。

## 接下来读哪

1. {doc}`vaapi` — 应用侧合同（config/context/surface）
2. {doc}`v4l2` — 设备侧合同（queue/format/buffer）
3. {doc}`request` — 无状态解码为什么必须 media request
4. {doc}`memory` — mmap / dmabuf / GBM 怎么叠
5. {doc}`bridge-as-textbook` — 对照本仓库函数读一遍
