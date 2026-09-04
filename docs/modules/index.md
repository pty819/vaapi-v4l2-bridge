# 源码模块索引

每个 `.c` 一份职责。新增 codec 或像素格式时，先选定这里的一行和
{doc}`/pipeline/pixels` 里的一种 backing，再写翻译器——不要把码流解析
堆进 `v4l2stateless.c`。

| 文件 | 行数（约） | 角色 |
|---|---|---|
| [`v4l2stateless.c`](front.md) | 2200 | VA 前端、vtable、对象生命周期、EndPicture 分派、EXPBUF Export |
| [`v4l2stateless.h`](front.md) | 520 | 全部公开结构与原型（含 `cap_view`、claim/expbuf） |
| [`v4l2stateless_device.c`](device.md) | 1300 | Request API 引擎、池、pull_capture、claim、reset |
| [`v4l2stateless_probe.c`](probe.md) | 510 | `/dev/video*` 按 fourcc 探测 + boot 缓存 |
| [`v4l2stateless_format.c`](format.md) | 360 | fourcc / stride / NV15↔P010 等 |
| [`v4l2stateless_gbm.c`](gbm.md) | 230 | EXPBUF 失败 / `=0` 时的 R8 bo、PRIME 描述符 |
| [`v4l2stateless_h264.c`](h264.md) | 530 | H.264 含 High10/422 |
| [`v4l2stateless_hevc.c`](hevc.md) | 410 | HEVC Main/Main10、WPP |
| [`v4l2stateless_av1.c`](av1.md) | 1320 | AV1：OBU、refresh、DPB 模型、合并控制 |
| [`v4l2stateless_vp8.c`](vp8.md) | 260 | VP8 header 长度回补 |
| [`v4l2stateless_mpeg2.c`](mpeg2.md) | 270 | MPEG-2 多 slice |
| [`v4l2stateless_jpeg.c`](jpeg.md) | 400 | VEPU 编码，有状态 M2M |
| [`v4l2stateless_vpp.c`](vpp.md) | 430 | RGA scale/CSC/rotate；EXPBUF src 读 `cap_view` |

```{mermaid}
flowchart TB
  subgraph front [VA 前端]
    C[v4l2stateless.c]
  end
  subgraph tx [码流翻译]
    H264
    HEVC
    AV1
    VP8
    MPEG2
  end
  subgraph m2m [有状态]
    JPEG
    VPP
  end
  C --> H264 & HEVC & AV1 & VP8 & MPEG2
  C --> JPEG & VPP
  H264 & HEVC & AV1 & VP8 & MPEG2 --> DEV[device.c]
  DEV -->|opt-out / fallback| GBM[gbm.c]
  DEV --> FMT[format.c]
  C --> PROBE[probe.c]
  JPEG --> DEV
  VPP --> DEV
```
