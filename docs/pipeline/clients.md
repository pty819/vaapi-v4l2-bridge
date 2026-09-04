# 客户端怎么把图拿走

```{mermaid}
flowchart LR
  S[surface has_pic]
  S --> E[vaExportSurfaceHandle]
  S --> D[vaDeriveImage]
  S --> G[vaGetImage]
  E --> C[Chrome GLES EXPBUF 零拷贝]
  D --> F[Firefox / 部分 VA 客户端]
  G --> FF[ffmpeg hwdownload]
```

## Chrome（官方 .deb，VA-API）

- 包装脚本 `scripts/google-chrome-vaapi` → `/usr/local/bin/google-chrome-stable`
- 强制：Wayland、ANGLE GLES、**关 Vulkan**、`--render-node-override=/dev/dri/renderD128`
- GPU sandbox 关（gpu-process 要 `open(/dev/videoN)`）
- 默认绑大核 4–7（`CHROME_PIN_CPUS` 可改）
- **必须保持** `AcceleratedVideoDecodeLinuxZeroCopyGL`。关掉会进不存在的
  ImageProcessor，整段会话掉 `FFmpegVideoDecoder`
- 第一次 VA 失败会缓存到进程退出。换 `.so` 后要重启实例
- 杀进程只用 SIGTERM。`gc-dbg` 是主 profile 的 bind mount，先 `mount | grep`
  再声称丢失
- 默认 Export = VPU `VIDIOC_EXPBUF`。ioctl_interpose 应看到
  `PRIME_FD_TO_HANDLE target=/dmabuf:`，不是 `memfd:v4l2sl-surf`

发行版 Chromium 走原生 V4L2，**不会**加载本 `.so`。看 gpu-process 的
maps：出现 `v4l2stateless_drv_video.so` 才是这条桥。

## ffmpeg

```
export LIBVA_DRIVER_NAME=v4l2stateless
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device /dev/dri/renderD128 -i FILE \
  -vf "hwdownload,format=nv12" -pix_fmt yuv420p -f framemd5 -
```

没有 `hwdownload` 的 framemd5 可能是软解。High10 用 `format=p010le`。
GetImage 读 capture mmap（`cap_view`），不是空的 create-time memfd。

B 站下载的 BILIAV1 文件走这条会花：ffmpeg 只交裸 tile，VA 没有
`refresh_frame_flags`。网页 Chrome 交 OBU span，不受影响。详见 README。

## Firefox

系统 ffmpeg 的 VA hwaccel + `scripts/firefox-vaapi-user.js`。
`media.hardware-video-decoding.force-enabled` 保持 false（VP9/10-bit HDR
硬开过会把 VPU 打挂）。

## 会话粘性（所有 VA 客户端里 Chrome 最严重）

| 事件 | 后果 |
|---|---|
| `vaEndPicture` 失败 | 该 entrypoint 本会话不再试 |
| 驱动 `.so` 更新 | 旧进程仍用旧映射；重启 |
| AV1 池抽干（copy-out 前） | 回退 HEVC，会话内不回 AV1 |

所以验证 AV1 必须新起 Chrome。
