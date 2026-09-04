# 对象模型

全部挂在 `struct v4l2sl_driver_data`（`src/v4l2stateless.h`）上。ID 来自
客户端，查表必须走 bounds-checked helper。

```{mermaid}
classDiagram
  class v4l2sl_driver_data {
    configs
    surfaces[4096]
    contexts
    orphan_buffers
    dev_h264..dev_vpp
  }
  class v4l2sl_config {
    profile
    entrypoint
    codec
    device_path
  }
  class v4l2sl_context {
    codec
    v4l2_fd media_fd request_fd
    pending_buffers[256]
    OUTPUT pool 4
    CAPTURE pool 24 or 40
    av1 DPB model
    vpp_q jpeg_q
  }
  class v4l2sl_surface {
    has_pic
    buf_index
    cap_view
    memfd / gbm_bo / cpu_ptr
    last_writer
  }
  class v4l2sl_buffer {
    type size data
    mmapped borrow_surf
  }
  v4l2sl_driver_data "1" --> "*" v4l2sl_config
  v4l2sl_driver_data "1" --> "*" v4l2sl_context
  v4l2sl_driver_data "1" --> "*" v4l2sl_surface
  v4l2sl_config "1" --> "*" v4l2sl_context : config_id
  v4l2sl_context "1" --> "*" v4l2sl_surface : render_targets
  v4l2sl_context "1" --> "*" v4l2sl_buffer : pending / attached
```

## config

`vaCreateConfig(profile, entrypoint)` → `v4l2sl_create_config`：

1. `codec_for_profile()` 把 VA profile 映到 `enum v4l2sl_codec`
2. `cached_device()` 取出探测阶段记下的 `/dev/videoN`
3. JPEG 只接受 `VAEntrypointEncPicture`，VPP 只接受
   `VAEntrypointVideoProc`，其余解码是 `VAEntrypointVLD`

config **不打开**设备。打开发生在 `vaCreateContext`。

## surface

`vaCreateSurfaces`：

- ID：空闲栈弹出，否则 `++next_surface_id`，永不越过 `V4L2SL_MAX_SURFACES`
- **必须**写 `surface->surface_id = id`（calloc 零曾让 AV1
  `buf_owner[]` 全指向 surface 0）
- `buf_index = -1`，`memfd_fd = -1`，`cpu_ptr` 懒分配，`cap_view = NULL`
- 立刻 `v4l2sl_surface_alloc_export_fd` 做 memfd，让 DRM-PRIME 客户端在
  第一帧之前就有稳定 fd（GBM 回退 / Derive 用；EXPBUF 热路径另 claim capture）

销毁：从所有 context 的 `render_targets[]` 摘掉；非 AV1 模型把仍持有的
capture index 还池；AV1 `model_active` 只清 `buf_owner`，不双释放。

## context

一次解码（或 VPP、JPEG）会话。

| 字段 | 意义 |
|---|---|
| `v4l2_fd` / `media_fd` / `request_fd` | 无状态路径三件套。JPEG/VPP 不分配 request |
| `pending_buffers[256]` | 当前 picture 的 VA buffer。MPEG-2 一行一个 slice，32 不够 |
| `free_out_bufs[4]` | OUTPUT（码流）槽 |
| `free_cap_bufs[40]` | CAPTURE 槽。H.264/HEVC 申请 24，AV1 申请 40（hantro AV1 不吃 CMA） |
| `g_ctrl_payload[1088]` | 上一次 GLOBAL sequence 控制的影子，memcmp 跳过重复 ioctl |
| `av1.*` | 仅 AV1：风格启发式 + DPB 模型 |
| `vpp_q` / `jpeg_q` | 有状态 M2M 的 setup 缓存 |

`vaBeginPicture` 把 `current_surface` 指到目标 surface，打时间戳
`frame_count++ * 1000`（微秒，对齐 vb2 从 timeval 存的单位）。
EXPBUF 开启时 **不** 把旧 capture index 还池——Chrome 还握着该槽的 fd。

## buffer

VA 参数/slice/image。`vaRenderPicture` 只是把指针塞进
`pending_buffers[]`。真正拆角色是 `v4l2sl_collect_decode_buffers`：

- PictureParameter → `cb.pic`
- SliceParameter → `cb.slice_params[]`（最多收 32 条，但
  `n_slice_params_seen` 会继续加，供 AV1 超限拒绝）
- SliceData → `cb.slice_datas[]`（最多 256）+ `cb.largest`
- IQ / Probability 各记一份

## 池子纪律

`v4l2sl_cap_pool_push` / `v4l2sl_out_pool_push`：

- index 越界 → 丢弃
- 已经在 free 列表里 → 丢弃（防双推）
- 列表已满 → 丢弃（漏掉一个 index 优于写爆）

AV1 在 `model_active` 时，**只有** `av1_release_unrefd` 可以 push
capture index。`vaBeginPicture` 换目标、`vaDestroySurfaces` 都不得再 push。
