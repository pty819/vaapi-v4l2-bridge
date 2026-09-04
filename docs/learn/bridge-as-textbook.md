# 把这座桥当教材：对照阅读顺序

前面五页是栈。下面用 **函数** 把它们串成作业。建议开着 `src/` 读。

## 作业 1 — 一次 ffmpeg 硬解（VA + 有状态感，其实走无状态）

命令（仓库 README）：

```bash
export LIBVA_DRIVER_NAME=v4l2stateless
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device /dev/dri/renderD128 -i FILE \
  -vf "hwdownload,format=nv12" -f framemd5 -
```

对照：

1. `vaInitialize` → `v4l2sl_init` → `v4l2sl_scan_all_cached`（{doc}`map` 的探测）
2. `vaCreateConfig(H264/HEVC/AV1)` → `cached_device()` 选出 `/dev/video*`
3. `vaCreateContext` → `open` video+media，OUTPUT REQBUFS
4. 每帧 Begin/Render/End → {doc}`vaapi` 三拍 + {doc}`request` QUEUE
5. `hwdownload` → `vaGetImage` → {doc}`memory` 的 memfd 路径

把 `V4L2SL_DEBUG=1` 打开，日志里的 `opened /dev/video`、`renegotiate capture`、`AV1 frame` 就是这些步骤。

## 作业 2 — Chrome 零拷贝（同一套 VA，另一套像素出口）

同一座桥，`vaEndPicture` 之后走 `vaExportSurfaceHandle` 而不是 GetImage。读：

- `v4l2sl_export_surface_handle`
- `v4l2sl_surface_fill_prime_gbm`
- `v4l2sl_gbm_surface_upload`（在 `pull_capture` 里）

作业：用 `ioctl_interpose` 确认 fd 是 dmabuf 不是 memfd。再对比 {doc}`memory` 为什么不能 EXPBUF VPU。

## 作业 3 — 无状态控制块

打开 `v4l2stateless_h264.c` 的 `h264_fill_sps` 和内核 UAPI
`struct v4l2_ctrl_h264_sps`。每一个 VA 字段如何搬进 V4L2，就是
「用户态解析器 + 无状态硬件」的合同。

AV1 更极端：`av1_fill_frame_params` + `av1_parse_hdr_refresh`。VA 缺
`refresh_frame_flags` 时，桥要么从 OBU 解，要么启发式。这是 API 设计
缺口，不是你没读懂 ioctl。

## 作业 4 — Request 生命周期

单步 `v4l2sl_decode_submit`（`device.c`）。用纸写下：

- 哪个 fd 是 video，哪个是 request
- CAPTURE QBUF 有没有 request_fd（应该没有）
- FLAG_ERROR 时返回值如何变成 VA SUCCESS + Skipped

对照内核文档 *stateless decoder*（`Documentation/userspace-api/media/v4l/dev-stateless-decoder.rst`）。

## 作业 5 — 所有权 / copy-out

读 `av1_release_unrefd` 和 `vaBeginPicture` 里 `av1_model` 分支。画一张
「CAPTURE index 的状态机」。然后解释：为什么 HEVC 24 槽够用、AV1
WebCodecs 不够，却仍然不能把 HEVC 也改成 copy-out（证据：同负载 HEVC
零错误，不必付 memcpy）。

## 作业 6 — 有状态对照

读 `v4l2sl_jpeg_encode` 或 `v4l2sl_vpp_run`。数稳态 ioctl：没有
request、没有 S_EXT_CTRLS 每帧填 SPS。这就是同一颗 SoC 上第二套 V4L2
用法。

## 推荐的内核 / libva 原文（读桥读到卡住再翻）

- libva：`va.h` 里 `vaBeginPicture` / `vaRenderPicture` / `vaEndPicture` 注释
- 内核：`dev-stateless-decoder.rst`、`dev-decoder.rst`（有状态）
- UAPI：`include/uapi/linux/v4l2-controls.h` 搜 `STATELESS_AV1`、`STATELESS_H264`
- media：`media-request-api.rst`

本仓库不是这两套栈的规范实现，是 **RK3588 主线上能跑 Chrome 的那条接缝**。规范在上游文档；接缝上的谎言（R8 假 NV12、禁止 EXPBUF、AV1 sequence 必须全局）只在这座桥和 {doc}`../handbook/invariants` 里写全。
