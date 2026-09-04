# v4l2stateless.c / .h — VA 前端

入口 `__vaDriverInit_1_20` → `v4l2sl_init`：

1. 分配 `v4l2sl_driver_data`
2. `v4l2sl_scan_all_cached` 填 `dev_h264` … `dev_vpp`
3. 填 `VADriverVTable`（未实现的 subpicture 等指向 stub）
4. `v4l2sl_debug = !!getenv("V4L2SL_DEBUG")`

`max_profiles` / `max_entrypoints` / `max_attributes` 按 Chrome
`FillProfileInfo_Locked` 的预期设，过小 Chrome 会认为驱动残缺。

## 锁

`static pthread_mutex_t g_v4l2sl_lock`。所有 stateful vtable 都拿它。
`vaEndPicture` 会跨整个 3s poll。不要再加第二把锁。

## Profile → codec → 节点

`src/v4l2stateless.c` 里两张表：

- `profile_codec_map`：`VAProfileH264High10 → V4L2SL_CODEC_H264` 等
- `cached_device(dd, codec)`：`V4L2SL_CODEC_AV1 → dd->dev_av1`

H.264 与 HEVC **共用** rkvdec 节点路径字符串，但每个 context **各自
open**，队列不共享。

## 关键 vtable 行为

| 入口 | 行为要点 |
|---|---|
| `vaQuerySurfaceAttributes` | ffmpeg 发现 NULL 会拒初始化。报告 NV12（及 P010 等） |
| `vaCreateSurfaces2` | Chrome 走这条；必须在 decode 前给出 export fd |
| `vaBeginPicture` | 见 {doc}`/pipeline/decode` |
| `vaEndPicture` | JPEG/VPP/video 三路 |
| `vaSyncSurface` | 只查表，decode 已完成 |
| `vaDeriveImage` | 映射 memfd；`has_pic` 门槛；borrow 计数 |
| `vaGetImage` | 调用 format.c 转换 |
| `vaExportSurfaceHandle` | 优先 GBM；失败则 memfd PRIME（Chrome 会当软路径） |
| `vaTerminate` | 拆 context、surface、config；修过 11MB/会话泄漏 |

## 不要放进这个文件的东西

码流解析、V4L2 ioctl 序列、GBM 创建、fourcc 转换。分别属于 av1.c /
device.c / gbm.c / format.c。前端只调度。
