# VA-API：给播放器的解码合同

VA-API（Video Acceleration API）是 **用户态** C API。库是 **libva**；真正干活的是 `*_drv_video.so`（本仓库就是其中一个）。应用从不 `#include <linux/videodev2.h>`。

官方概念文档在 Intel libva。下面用这座桥的对象把合同讲清楚。

## 角色

| VA 对象 | 像什么 | 本仓库 |
|---|---|---|
| **Display** | 和 GPU/DRM 的连接 | `vaGetDisplayDRM(/dev/dri/renderD128)` → `vaInitialize` → `__vaDriverInit_1_20` |
| **Config** | 「我要用 H.264 High 解码」这张能力卡 | `v4l2sl_config`：profile + entrypoint + 选中的 `/dev/video*` |
| **Context** | 一次解码会话（分辨率、DPB、设备 fd） | `v4l2sl_context` |
| **Surface** | 一张图（参考帧或输出） | `v4l2sl_surface` |
| **Buffer** | 参数或码流内存 | `v4l2sl_buffer`：PictureParameter、SliceParameter、SliceData、Image |

Entrypoint 常见：

- `VAEntrypointVLD`：解码（Variable Length Decode）
- `VAEntrypointVideoProc`：后处理（本仓库 VPP）
- `VAEntrypointEncPicture`：编码（本仓库 JPEG）

Profile 是「哪种语法」：`VAProfileH264High`、`VAProfileHEVCMain10`、`VAProfileAV1Profile0`。桥在 `v4l2sl_init` 里广告自己支持哪些；Chrome 的 `FillProfileInfo_Locked` 会查 RT format 和 attrib 个数，少填会被当成残缺驱动。

## 一次 picture 的三拍

这是 VA 最重要的时序，所有硬解播放器都照这个跳：

```{mermaid}
sequenceDiagram
  participant App
  participant VA as libva + 本驱动
  App->>VA: vaBeginPicture(context, surface)
  Note over VA: 目标 surface = 这一帧要写的槽
  App->>VA: vaCreateBuffer(... PictureParameter)
  App->>VA: vaCreateBuffer(... SliceParameter / SliceData)
  App->>VA: vaRenderPicture(buffers)
  Note over VA: 只收集，不解
  App->>VA: vaEndPicture(context)
  Note over VA: 同步解码发生在这里
  App->>VA: vaSyncSurface(surface)
  Note over VA: 本驱动里已是 no-op 等待
  App->>VA: vaGetImage 或 vaExportSurfaceHandle
```

对照代码：`v4l2sl_begin_picture` / `v4l2sl_render_picture` / `v4l2sl_end_picture`（`src/v4l2stateless.c`）。

**同步 vs 异步：** 规范允许 EndPicture 只是「提交」，真正完成在 SyncSurface。这座桥选择 **EndPicture 内同步跑完 VPU**（一次 request、poll ≤ 3s），所以 SyncSurface 只改 `status = Ready`。学别的后端（iHD、radeonsi）时会看到真正的异步队列，不要以为全世界都同步。

## 参数 buffer 长什么样

解码不是「丢一串 Annex-B 进去」。无状态 VA 要求应用（ffmpeg 的 `h264_vaapi` 等）**先解析头部**，再按结构体交：

- `VAPictureParameterBufferH264`：SPS/PPS 里这一帧需要的字段、参考 surface id 列表
- `VASliceParameterBufferH264`：每个 slice 的 offset/size、NAL 类型
- `VASliceDataBufferType`：NAL 字节

AV1 同样：`VADecPictureParameterBufferAV1` + 每 tile 一条 slice param + 一块 data。

**VA 不保证有的字段** 就是桥要自己猜或解析的地方。AV1 的 `refresh_frame_flags` 是典型：结构体没有，Chrome 把整段 OBU 放在 slice data 里还能解，ffmpeg 只交裸 tile 就丢信息。这是 API 边界，不是 VPU 边界。

## 两种把像素拿走的方式

学 VA 必分清，否则会觉得「ffmpeg 能 10-bit、Chrome 不能」是驱动没写完：

| API | 语义 | 谁用 | 本仓库 |
|---|---|---|---|
| `vaGetImage` / `vaDeriveImage` | CPU 可读的字节（可转换 fourcc） | ffmpeg `hwdownload`、Firefox 部分路径 | memfd + `format.c`（NV15→P010 等） |
| `vaExportSurfaceHandle` | dma-buf 描述符给 GPU import | Chrome 零拷贝 GL | 默认 VPU EXPBUF，**仅 NV12**，`num_objects==1`；GBM 是 `=0` 回退 |

Chrome 几乎不 GetImage。导出失败 → 整段会话改走软解（它会缓存第一次 VA 失败）。

## 错误缓存（Chrome 特有，但反映 VA 的「entrypoint 失败」模型）

`vaEndPicture` 返回失败，Chrome 认为这个 **entrypoint 在本进程不可用**。所以：

- 内核 `BUF_FLAG_ERROR`（码流坏帧）桥会标 `VASurfaceSkipped` 却 **返回 SUCCESS**
- 真的设备错误才失败
- 换了 `.so` 必须重启 Chrome

学 VA 时把「失败」分成 **这一帧坏了** 和 **这个解码器挂了** 两档。桥显式做了这个区分。

## 自己练

```bash
export LIBVA_DRIVER_NAME=v4l2stateless
vainfo --display drm --device /dev/dri/renderD128
```

看广告出来的 Profile。再读 `v4l2sl_query_config_profiles` 对照。ffmpeg 加 `-v verbose` 能看到它创建了哪些 VA buffer 类型。
