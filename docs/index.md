# vaapi-v4l2-bridge

libva DRM 后端：把 VA-API 译成 RK3588 **主线** 上的 V4L2 Request API（无状态解码）以及两路有状态 M2M（RGA VPP、VEPU JPEG 编码）。产物是 `v4l2stateless_drv_video.so`。

Chrome 出画默认 `VIDIOC_EXPBUF` VPU capture（无 capture→GBM memcpy）。
`V4L2SL_EXPBUF_EXPORT=0` 回退 GBM 拷贝。见 [EXPBUF 零拷贝](pipeline/expbuf.md)。

```{toctree}
:maxdepth: 2
:caption: 从这座桥学习 VA-API 与 V4L2

learn/index
learn/map
learn/vaapi
learn/v4l2
learn/request
learn/memory
learn/bridge-as-textbook
```

```{toctree}
:maxdepth: 2
:caption: 总览

overview
objects
pipeline/decode
pipeline/pixels
pipeline/expbuf
pipeline/clients
```

```{toctree}
:maxdepth: 2
:caption: 源码模块

modules/index
modules/front
modules/device
modules/probe
modules/format
modules/gbm
modules/h264
modules/hevc
modules/av1
modules/vp8
modules/mpeg2
modules/jpeg
modules/vpp
```

```{toctree}
:maxdepth: 2
:caption: 手册与仓库文档

handbook/invariants
handbook/tests
handbook/desktop
```

仓库根目录还有一份英文模块图 [docs/ARCHITECTURE.md](https://github.com/pty819/vaapi-v4l2-bridge/blob/master/docs/ARCHITECTURE.md)（与本站同源，站点按主题拆页并写得更细）。

能力表（谁能播、矩阵绿不绿）仍以仓库根目录 [README.md](https://github.com/pty819/vaapi-v4l2-bridge/blob/master/README.md) 和 [STATE.md](https://github.com/pty819/vaapi-v4l2-bridge/blob/master/STATE.md) 为准。本站点解释 **模块怎么拼、一帧怎么走、像素住哪**。若目的是弄懂 Linux 上 VA-API 与 V4L2 两套栈，从 [学习路径](learn/index.md) 开始。
