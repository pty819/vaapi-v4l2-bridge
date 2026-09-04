# 从这座桥学习 VA-API 与 V4L2

本仓库是一个 **VA-API → V4L2** 的用户态翻译器。读完它等于同时摸到两套 Linux 多媒体栈的接缝：上面是 Intel 风格的 VA-API（ffmpeg / Chrome / Firefox 说话的方式），下面是内核 Video4Linux2 和 media controller（板子上 VPU 真正听话的方式）。

建议按下面顺序读。每一页都把 **行业概念** 和 **本仓库对应代码** 钉在一起，避免只背 ioctl 名字、或只看这座桥却不知道它在模仿谁。

```{toctree}
:maxdepth: 2
:caption: 学习路径

map
vaapi
v4l2
request
memory
bridge-as-textbook
```

## 读之前要有的图

```{mermaid}
flowchart TB
  subgraph user [用户态]
    APP[ffmpeg / Chrome / VLC]
    LIBVA[libva]
    DRV[本仓库 drv_video.so]
  end
  subgraph kernel [内核]
    V4L[V4L2 videodev]
    MC[media controller]
    DRM[DRM/KMS + panthor]
    VPU[rkvdec / hantro / RGA / VEPU]
  end
  APP --> LIBVA --> DRV
  DRV -->|Request API 或 M2M| V4L
  DRV -->|request fd| MC
  DRV -->|EXPBUF dma-buf 显示| DRM
  V4L --> VPU
  MC --> VPU
```

三句话版本：

1. **VA-API** 是给「解码器用户」的稳定 C API：config / context / surface / buffer，一次 picture 三拍（Begin / Render / End）。
2. **V4L2** 是给「视频设备」的 ioctl API：QUEUE 一块内存进去，DEQUEUE 一块出来；无状态解码还要 media request 把「这一帧的控制块」和「这一帧的码流」绑死。
3. **这座桥** 把 1 的一次 `vaEndPicture` 变成 2 的一次 `QUEUE` + `poll` + `DQBUF`。Chrome 用 `VIDIOC_EXPBUF` 拿走 VPU dma-buf；ffmpeg 用 GetImage 读 capture mmap。

## 和本站其它栏目的关系

| 栏目 | 用途 |
|---|---|
| 本学习路径 | 两套栈的概念 + 和代码的对照 |
| [总览 / 流水线](../overview.md) | 这座桥自己怎么跑 |
| [EXPBUF 零拷贝](../pipeline/expbuf.md) | 显示热路径（默认，无 capture→GBM memcpy） |
| [源码模块](../modules/index.md) | 每个 `.c` 文件干什么 |
| 仓库 README / STATE | 能力表、验证记录（会过时的数字以那里为准） |
