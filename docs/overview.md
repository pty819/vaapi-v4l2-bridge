# 这是什么、不是什么

## 产物

`v4l2stateless_drv_video.so`，装在 libva 的 `driverdir`（本机
`/usr/lib/aarch64-linux-gnu/dri/`）。`LIBVA_DRIVER_NAME=v4l2stateless`
时 libva 用 `dlopen` 加载，入口 `__vaDriverInit_1_20`。

meson：全部 `src/*.c` 打进静态库 `libv4l2sl_core`，再用 `link_whole`
链进 `.so`。普通 `link_with` 会把没被引用的 codec 翻译单元丢掉——
所以必须 `link_whole`。测试 exe 链同一份 static lib，保证字段填充和
`.so` 同源。

## 它翻译什么

```{mermaid}
flowchart LR
  subgraph clients [客户端]
    FF[ffmpeg -hwaccel vaapi]
    CH[Chrome VaapiVideoDecoder]
    FX[Firefox ffmpeg VA]
    VLC[VLC avcodec-hw=vaapi]
  end
  subgraph this [本仓库]
    SO[v4l2stateless_drv_video.so]
  end
  subgraph kernel [内核已有节点]
    RK[rkvdec video1 H264/HEVC]
    HA[hantro video4 AV1]
    H2[hantro video2 VP8/MPEG2]
    JP[VEPU video3 JPEG enc]
    RG[RGA video0 VPP]
  end
  clients --> SO
  SO -->|Request API 无状态| RK
  SO -->|Request API 无状态| HA
  SO -->|Request API 无状态| H2
  SO -->|stateful M2M| JP
  SO -->|stateful M2M| RG
```

客户端只说 VA-API。内核只懂 V4L2。中间这层把：

- `VADecPictureParameterBuffer*` → `v4l2_ctrl_*`
- slice / tile payload → OUTPUT 平面
- 解码完成的 CAPTURE → surface 快照（GBM 或 memfd）
- `vaExportSurfaceHandle` → 单 object NV12 dma-buf（**不是** VPU EXPBUF）

## 明确不是

| 不是 | 为什么 |
|---|---|
| 内核驱动 | VPU 节点已经在；本仓库是用户态 glue |
| Chromium 原生 V4L2 解码 | 发行版 Chromium 从不 `dlopen` 这个 `.so`。官方 Chrome `.deb` 才走 VA-API |
| Rockchip MPP / 厂商 BSP userspace | 目标就是主线 + 标准 VA |
| HDR 合成器 | 解 10-bit ≠ HDMI 打 HDR InfoFrame。mainline dw-hdmi-qp 目前是能出画 |
| VP9 硬解 | 本机 hantro 无 VP9 fourcc |

## 谁在何时加载

```{mermaid}
sequenceDiagram
  participant App
  participant libva
  participant SO as drv_video.so
  participant Probe as probe.c
  App->>libva: vaInitialize(DRM, renderD128)
  libva->>SO: dlopen + __vaDriverInit_1_20
  SO->>Probe: v4l2sl_scan_all_cached()
  Probe-->>SO: dev_h264 / hevc / av1 / ...
  SO-->>libva: 填好的 VADriverVTable
  App->>libva: vaCreateConfig(profile, VLD)
  libva->>SO: v4l2sl_create_config
```

探测结果缓存在 `$XDG_RUNTIME_DIR`，按 `boot_id` 分文件。Chrome /
vainfo / Firefox 启动都走这条，避免每次 `vaInitialize` 把 `/dev/video0..63`
扫一遍。`V4L2SL_PROBE_NOCACHE=1` 强制重扫。

## 两种设备模型（组合关系的第一条裂缝）

无状态解码（H.264/HEVC/AV1/VP8/MPEG-2）走 **V4L2 Request API**：

- 一个 `request_fd`（`MEDIA_IOC_REQUEST_ALLOC`，每帧 `REINIT`）
- OUTPUT `QBUF` **带** request
- CAPTURE `QBUF` **不带** request（规范禁止，否则 `EPERM`）
- 一帧一个 request，同步 poll ≤ 3s

JPEG / VPP 走 **有状态 M2M**：

- 没有 request fd
- `S_FMT` / `REQBUFS` / mmap 缓存在 `v4l2sl_m2m_state`
- 稳态只有 STREAMON + QBUF + DQBUF + STREAMOFF

`vaEndPicture` 按 `context->codec` 把这两类完全分开。不要把
`v4l2sl_decode_submit` 用在 JPEG 上，也不要把 M2M STREAMON 套到 rkvdec 上。
