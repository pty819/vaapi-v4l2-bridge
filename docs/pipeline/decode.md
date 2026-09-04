# 一帧怎么走完（无状态）

`vaEndPicture` 持 `g_v4l2sl_lock` 直到 `decode_submit` 返回。解码是
**同步**的：`vaSyncSurface` 不再等 VPU。

```{mermaid}
sequenceDiagram
  participant App
  participant Front as v4l2stateless.c
  participant Tx as *_translate
  participant Dev as device.c
  participant VPU as /dev/videoN
  App->>Front: vaBeginPicture(surface)
  Front->>Front: current_surface, timestamp
  App->>Front: vaRenderPicture(params)
  Front->>Front: pending_buffers[]
  App->>Front: vaEndPicture
  Front->>Tx: h264/hevc/av1/vp8/mpeg2_translate
  Tx->>Tx: collect + fill v4l2_ctrl_*
  Tx->>Dev: ensure_capture + STREAMON
  Tx->>Dev: memcpy OUTPUT + S_EXT_CTRLS
  Tx->>Dev: decode_submit
  Dev->>VPU: QBUF OUT+CAP, QUEUE, poll, DQBUF
  Dev-->>Tx: done_cap index
  Tx->>Dev: pull_capture snapshot
  Tx-->>Front: VA_STATUS_SUCCESS
  App->>Front: vaSyncSurface (Ready)
  App->>Front: Export 或 GetImage
```

## BeginPicture

- 找不到 surface → `INVALID_SURFACE`
- 该 surface 仍挂着 `buf_index`：
  - 非 AV1 模型：push 回 capture 池，然后 `buf_index = -1`
  - AV1 `model_active`：只清 `buf_index`，**不** push（内核 DPB 还指着它）
- memfd 保留（DRM-PRIME 客户端要稳定 fd）
- 没有 `request_fd` 就 `MEDIA_IOC_REQUEST_ALLOC` 一次，之后每帧 REINIT

## RenderPicture

只收集。MPEG-2 可能上百个 slice buffer，所以数组是 256 不是 32。

## EndPicture 分派

```
JPEG_ENC  → v4l2sl_jpeg_encode     (M2M)
VPP       → v4l2sl_vpp_run         (M2M)
else      → v4l2sl_*_translate     (Request API)
fd < 0    → stub：标 Ready 返回成功（无设备时的测试路径）
```

## translate 内部（各 codec 同构）

1. `v4l2sl_collect_decode_buffers`
2. 填 sequence / SPS（GLOBAL，memcmp 缓存）
3. 填 frame / PPS / tile / scaling（request-scoped）
4. `v4l2sl_ensure_capture(w, h, fourcc)` — 变分辨率或 NV12→NV15 会
   STREAMOFF + S_FMT + REQBUFS，并 `v4l2sl_av1_dpb_model_reset`
5. 尚未 STREAMON 则两边 STREAMON
6. pop OUTPUT 槽，memcpy 载荷
7. `v4l2sl_decode_submit`
8. `v4l2sl_surface_pull_capture`
9. AV1：`av1_release_unrefd`（仅 `model_active`）

AV1 的 grain+frame+tile-group 是 **一次** `S_EXT_CTRLS`，且发生在
pop OUTPUT **之前**：控制失败时池子还是干净的。

## decode_submit

```
n_free_cap == 0          → 打 "no free capture buffer"，还 OUTPUT，返回 -1
QBUF OUTPUT 失败         → 两边还池
QBUF CAPTURE 失败        → 还 capture；poll OUTPUT 把码流槽捞回来
QUEUE 失败               → decode_reset（不要只 push，内核还占着 buffer）
poll 超时 3s             → decode_reset
DQBUF CAPTURE FLAG_ERROR → push capture，返回 -2（skipped，对 Chrome 仍 SUCCESS）
DQBUF OUTPUT             → 还 OUTPUT 池
REINIT request           → EINVAL 才 close，BeginPicture 会再 alloc
```

`-2` 的语义：corrupt 帧。translator 把 surface 标 `VASurfaceSkipped` 然后
**成功返回**。Chrome 缓存失败的 entrypoint；一次 `vaEndPicture` 失败，
整个会话不再试 AV1。

## 时间戳与 DPB

`surface->timestamp` 是 V4L2 OUTPUT 时间戳，内核用它把 CAPTURE 和
参考帧对上。H.264/HEVC DPB 表填的是这些 timestamp，不是 VA surface id。
AV1 `reference_frame_ts[8]` 同样。
