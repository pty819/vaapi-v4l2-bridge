# 桌面接线

细节逐步命令在仓库 [APPS.md](https://github.com/pty819/vaapi-v4l2-bridge/blob/master/APPS.md)。这里只记模块怎么接到各 App。

```{mermaid}
flowchart TB
  ENV["environment.d/90-libva.conf<br/>LIBVA_DRIVER_NAME=v4l2stateless"]
  ENV --> CH[官方 Chrome 包装脚本]
  ENV --> FX[Firefox user.js]
  ENV --> VLC[vlcrc avcodec-hw=vaapi]
  ENV --> FF[ffmpeg]
  CH --> SO[.so]
  FX --> SO
  VLC --> SO
  FF --> SO
  SO -->|Export GBM| GPU[panthor GLES]
  SO -->|GetImage| CPU[ffmpeg hwdownload]
```

图形会话必须能看见那条环境变量；只在 ssh 的 shell 里 export 对
桌面 Chrome 无效。

Chrome 包装脚本还负责：render-node、关 GPU sandbox、纯 Wayland GLES、
关 Vulkan、大核 pin。不要直接跑 `/usr/bin/google-chrome-stable`。
