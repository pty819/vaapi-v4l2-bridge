# 测试与安装

默认 **release / `-O3 -DNDEBUG`**（`meson.build` 的 `buildtype=release`）。
不要用 `meson setup builddir --buildtype=debug` 来装系统 `.so`。

```
meson setup builddir
ninja -C builddir
sudo cp -f builddir/v4l2stateless_drv_video.so \
  /usr/lib/aarch64-linux-gnu/dri/
export LIBVA_DRIVER_NAME=v4l2stateless
bash tests/run_full_matrix.sh
```

旧 `builddir` 若仍是 `debug`（`optimization=0`），先：

```
meson configure builddir -Dbuildtype=release -Doptimization=3 -Db_ndebug=if-release
```

矩阵加载 **已安装** 的 `.so`，不是 `builddir/` 里那份。

## 单元（无设备也可跑一部分）

| 目标 | 测什么 |
|---|---|
| `test_video_probe` | fourcc 选节点、stub AV1、video 号对调 |
| `test_vp8_mpeg2_fill` | header 字段 vs 金样 |
| `test_format` | NV15/NV20 转换 |
| `test_export_recapture` | export 后池 index 不泄漏 |
| `gbm_probe` | 本机是否允许 R8 假 NV12 那套 |

## 矩阵覆盖（主机）

H.264（含 High10、High422 8/10）、HEVC Main/Main10/WPP/4K、AV1
（aom-8、aom-49、svt-32、4K、default）、VP8、MPEG-2 vs GST、JPEG、
VPP、vainfo。绿状态 **PASS=32 FAIL=0**。

AV1 SVT 用例只解 **前 32 帧**。GOP2 的启发式 bug 它看不见——全片
hw-vs-sw 才是完整验证器。

## 不要拿去硬解

`verify/clips/av1_40tiles.mp4`：8×5 tile，驱动会直接拒绝（修过之后）。
修之前截断会把 SoC 复位。
