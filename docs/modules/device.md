# v4l2stateless_device.c — Request API 引擎

所有无状态 codec 的 ioctl 都经这里。测试可用
`v4l2sl_set_ioctl_hook` 把 `xioctl` 换成假实现（`test_export_recapture`）。

## 打开设备

- `v4l2sl_open_device`：`O_RDWR | O_CLOEXEC`
- `v4l2sl_open_media_for_device`：sysfs
  `/sys/class/video4linux/<videoN>/device/media*` → `/dev/mediaM`
  （AV1 是 media3，**不是**想当然的 media0）
- `v4l2sl_request_alloc`：`MEDIA_IOC_REQUEST_ALLOC`

## 队列

OUTPUT：`V4L2SL_NUM_OUTPUT_BUFS = 4`，码流平面，mmap 常驻。

CAPTURE：默认 24，AV1 40，硬顶 `V4L2SL_MAX_CAPTURE_BUFS = 40`。
`REQBUFS` 失败会 40→24→8→4 降级（CMA 压力）。AV1 capture **不来自 CMA**，
所以 40 槽是便宜的。

`v4l2sl_mmap_one_capture` 懒 mmap，结果缓存。`V4L2SL_DEBUG` 时打
`capture mmap idx=`。

## ensure_capture

几何或 fourcc 变了：

1. STREAMOFF 两边
2. `v4l2sl_av1_dpb_model_reset`（STREAMOFF 丢掉内核 DPB 引用）
3. `release_ctx_capture_surfaces`（surface 上的 buf_index 摘掉）
4. 新 `S_FMT` + `REQBUFS` + 重建 free 池

HEVC/H.264 High10 常见：先按 NV12 建队列，SPS 来了再 NV15。

## decode_submit

见 {doc}`/pipeline/decode`。CAPTURE **禁止**放进 request。

超时或 QUEUE 失败必须 `v4l2sl_decode_reset`，不能只把 index push
回去——内核还握着那些 buffer，再 QBUF 会 EBUSY 或用错槽。

## pull_capture

1. mmap 该 CAPTURE index
2. 若 `surf->gbm_bo`：`v4l2sl_gbm_surface_upload`，`last_writer = BO`，
   `has_pic = 1`
3. 否则 grow memfd + memcpy，`last_writer = MEMFD`，`has_pic = 1`
4. 记下 `stride` / `aligned_h` / `cap_fourcc`

GBM 上传失败会打日志并回退 memfd，帧不能丢（capture 马上要回收）。

## 导出

`v4l2sl_surface_fill_prime`：memfd 的单 object 描述符（软路径）。
GBM 版在 gbm.c。`v4l2sl_fill_prime_layers` 是两者共用的填表函数
（SEPARATE_LAYERS vs 合并层）。
