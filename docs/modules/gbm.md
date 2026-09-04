# v4l2stateless_gbm.c — 显示拷贝（回退路径）

Chrome 默认零拷贝走 {doc}`/pipeline/expbuf`（VPU `VIDIOC_EXPBUF`）。
本文件是 **`V4L2SL_EXPBUF_EXPORT=0` 或 EXPBUF ioctl 失败** 时的
R8 假 NV12 GBM bo。见 {doc}`/pipeline/pixels`。

## 生命周期

`v4l2sl_gbm_surface_ensure` 在需要 GBM 回退的 surface 上建 bo。
opt-out 的 `pull_capture` 每帧 `gbm_surface_upload`。销毁 surface 时
`gbm_surface_destroy`。

全局一个 `gbm_device*`，render 节点默认 `/dev/dri/renderD128`，
`V4L2SL_RENDER_NODE` 可改。`gbm_create_device` 失败则
`g_gbm_failed=1`，之后 Export 干净失败（软解），不把 decode 弄崩。

## 描述符

`v4l2sl_surface_fill_prime_gbm`：

- `gbm_bo_get_fd_for_plane(bo, 0)` 一个 fd
- size = `pitch * (h + ceil(h/2))`
- `v4l2sl_fill_prime_layers(..., VA_FOURCC_NV12, DRM_FORMAT_NV12, R8, GR88)`

Chrome `vaapi_wrapper` 要求 `num_objects == 1`。两个 fd 的真 NV12 会被拒。
panthor `gbm_bo_import` 真 NV12 fourcc 仍是 `EINVAL`；只有加高的 R8 能 import。
