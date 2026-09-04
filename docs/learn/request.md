# Media Request API：无状态解码的粘合剂

无状态 VPU 每帧需要三样东西同时到达硬件：

1. 码流（OUTPUT buffer）
2. 解完放哪（CAPTURE buffer）
3. 这一帧的控制块（SPS/PPS/frame header 的 V4L2 版）

若 3 是「设备上的当前值」，两帧并发或应用填错顺序就会用到上一帧的 SPS。**Request API** 把 1+3 绑成原子「这一帧」。

## 对象

- `/dev/mediaN`：media controller 设备，和 `/dev/videoN` 是兄弟（sysfs `device/media*`）
- `MEDIA_IOC_REQUEST_ALLOC` → `request_fd`
- `VIDIOC_S_EXT_CTRLS` 时 `which = V4L2_CTRL_WHICH_REQUEST_VAL | request_fd`
- OUTPUT `QBUF` 带 `request_fd`
- `MEDIA_REQUEST_IOC_QUEUE` 提交
- 完成后 `MEDIA_REQUEST_IOC_REINIT` 复用（比 close+realloc 少两次 syscall）

对照：`v4l2sl_request_alloc`、`v4l2sl_set_request_controls`、`v4l2sl_queue_output`、`v4l2sl_submit_request`、`decode_submit` 末尾的 REINIT。

## CAPTURE 不进 request

V4L2 无状态规范：**CAPTURE buffer 禁止放进 request**（否则 `EPERM`）。捕获队列是「谁空谁上」，和「这一帧的语法」分开。桥 `v4l2sl_queue_capture` 不传 request fd。

直觉：控制+码流定义「算什么」；CAPTURE 只是「结果写哪块内存」。参考帧是靠 OUTPUT 上的 timestamp 对上的，不是靠 CAPTURE 进 request。

## 一帧在桥里的顺序

```{mermaid}
sequenceDiagram
  participant T as translator
  participant D as device.c
  participant K as kernel
  T->>D: S_EXT_CTRLS on request_fd
  T->>D: pop OUTPUT, memcpy tiles
  T->>D: decode_submit
  D->>K: QBUF OUTPUT + request
  D->>K: QBUF CAPTURE  (bare)
  D->>K: MEDIA_REQUEST_IOC_QUEUE
  D->>K: poll CAPTURE
  D->>K: DQBUF CAPTURE, DQBUF OUTPUT
  D->>K: MEDIA_REQUEST_IOC_REINIT
```

QUEUE 失败或 poll 超时：`v4l2sl_decode_reset` 两边 STREAMOFF。不要把还在内核里的 index push 回 free 池。

## 和 VA 三拍的对应

| VA | Request API |
|---|---|
| BeginPicture | 选 surface、打 timestamp（尚未 QUEUE） |
| RenderPicture | 填即将放进 request 的 control 结构 |
| EndPicture | S_CTRL + QBUF + QUEUE + 等待 |

VA 的 surface id 在内核不可见。桥把 surface 的 `timestamp` 写到 OUTPUT，H.264 DPB / AV1 `reference_frame_ts[]` 用同一套时间戳。这是「用户态 DPB」的全部含义：你告诉硬件「参考帧是当时 OUTPUT 时间戳为 T 的那次解码」。

## 探测 request 能力

不是每个广告了 `AV1_FRAME` 的 `/dev/video*` 都能 ALLOC request（RK3588 上有 stub 节点）。`v4l2sl_video_has_request_api` 打开 sibling media 试一次 ALLOC。probe 单元测试专门覆盖「stub 在前、真节点在后」。

## 自己练

```bash
ls /sys/class/video4linux/video4/device/media*
# 常见 → ../../media3 → /dev/media3
media-ctl -d /dev/media3 -p
```

看 pad、entity 名字（`rk3588-av1-vpu-dec`）。桥日志 `media for /dev/video4 is /dev/media3` 就是这一步。
