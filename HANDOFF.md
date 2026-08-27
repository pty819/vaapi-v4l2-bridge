# HANDOFF — VA-API → V4L2-stateless 桥接层

写于 **2026-08-27 10:25 +0800**。机器：Orange Pi 5 NAS `192.168.1.21`，Armbian 26.8.3，kernel **7.1.8-edge-rockchip64**。

AV1 inter 已接上：`/tmp/av1.mp4` 前 8 帧（含 P）与前 49 帧 VA-API `hwdownload` framemd5 对软解；同机 GStreamer `v4l2slav1dec` 这 49 帧也对齐。4K 8-bit H.264 High（3840×2160，8 帧）同样 bit-exact。H.264/HEVC 1080p 120 帧回归仍绿。

安装 `.so`：`/usr/lib/aarch64-linux-gnu/dri/v4l2stateless_drv_video.so`（2026-08-27 10:18）。回归脚本：`tests/vaapi_hwdownload.sh`。

---

## 一句话

C 驱动 `v4l2stateless_drv_video.so` 已经能把 ffmpeg VA-API 硬解接到主线 V4L2-stateless：

| 编码 | 设备 | 状态 |
|---|---|---|
| H.264 | rkvdec `/dev/video1` | **完成**：三路 1080p 各 120/120 framemd5 |
| HEVC 8-bit Main | 同上 | **完成**：Main + WPP 各 120/120 |
| AV1 8-bit | hantro `/dev/video4` + `/dev/media3` | **完成**：`/tmp/av1.mp4` 前 8 帧与前 49 帧 hwdownload framemd5 均对软解 |

约束没变：只走主线 edge，禁止 vendor/BSP/MPP；成功判定必须是 `hwdownload` 后的 framemd5，禁止把静默软解当硬解成功。

---

## 路径与部署

| 角色 | 路径 |
|---|---|
| NAS 工程 | `/home/liyifan/vaapi-v4l2-bridge/` |
| NAS 源码 | `~/vaapi-v4l2-bridge/src/` |
| NAS 构建 | `~/vaapi-v4l2-bridge/builddir/`（meson + ninja） |
| 已安装 .so | `/usr/lib/aarch64-linux-gnu/dri/v4l2stateless_drv_video.so`（2026-08-27 09:26，206784 bytes） |
| Mac 工作副本 | `~/v4l2bridge-dev/`（改完 scp 到 NAS 再 ninja） |
| 探针 / dump | Mac `~/v4l2bridge-dev/{ioctlspy.c,ctrldiff.py,av1probe.c,dumpdec.c}` |
| ffmpeg | **apt** `/usr/bin/ffmpeg` `7:8.0.1-3ubuntu2`，不是本地编的 |
| 驱动选择 | `~/.profile`：`export LIBVA_DRIVER_NAME=v4l2stateless` |
| SSH | `liyifan@192.168.1.21`（密钥）；sudo 密码 `liyifan` |

构建 / 安装：

```bash
cd ~/vaapi-v4l2-bridge/builddir
ninja
sudo cp -f v4l2stateless_drv_video.so /usr/lib/aarch64-linux-gnu/dri/
```

Mac → NAS：

```bash
scp ~/v4l2bridge-dev/v4l2stateless_av1.c liyifan@192.168.1.21:~/vaapi-v4l2-bridge/src/
```

git：仓库在 NAS 上，`master`，工作树脏（C 驱动整段是未提交改动）。最近提交仍是六月骨架文档，**不要把 git log 当当前实现**。

---

## 验证方法（唯一有效）

不要用裸 `-f framemd5`。ffmpeg 硬解失败时会静默回软解，hash 会对、硬件没干活。

```bash
LIBVA_DRIVER_NAME=v4l2stateless /usr/bin/ffmpeg \
  -hwaccel vaapi -hwaccel_output_format vaapi -vaapi_device /dev/dri/renderD128 \
  -i FILE.mp4 -vf "hwdownload,format=nv12" -pix_fmt yuv420p -frames:v N \
  -f framemd5 -y /tmp/hw.md5

/usr/bin/ffmpeg -i FILE.mp4 -pix_fmt yuv420p -frames:v N -f framemd5 -y /tmp/sw.md5
diff -u /tmp/sw.md5 /tmp/hw.md5
```

同时看 stderr：不能出现 `hardware accelerator failed` / `Failed to query surface attributes`。`derive_image: surface N has no decoded frame` 在第一帧前出现过一次，目前不影响后续比对。

测试流（都在 NAS `/tmp/`）：

| 文件 | 内容 |
|---|---|
| `hs_test.mp4` | H.264 1080p30 High + B 帧 |
| `allp.mp4` | H.264 1080p30 High，无 B |
| `ms.mp4` | H.264 1080p30 4-slice |
| `hevc.mp4` | HEVC 1080p30 Main |
| `hevcwpp.mp4` | HEVC 1080p30 WPP |
| `av1.mp4` | AV1 1080p30，GOP = I 后全 P |

---

## 已完成：H.264 / HEVC

两路都走 rkvdec **frame-based** UAPI（kernel 7.0+），不是 slice-based。

关键运行时（六月骨架里缺的，后来补上的）：

- 两边队列 `VIDIOC_STREAMON`（HEVC/AV1 要等全局 SPS/sequence 之后）
- Capture QBUF **不能**带 `V4L2_BUF_FLAG_REQUEST_FD`（带了 vb2 直接 EPERM）
- OUTPUT 4 槽 + CAPTURE 24 槽的池，禁止写死 index 0
- 同步解码：S_EXT_CTRLS → QBUF out（带 request）→ QBUF cap（裸）→ `MEDIA_REQUEST_IOC_QUEUE` → poll → DQBUF
- surface timestamp：纳秒、1µs 步进；QBUF timeval 必须 `tv_sec = ns/1e9`，`tv_usec = (ns%1e9)/1000`；DPB `reference_ts` 用同一套 ns
- buffer ID 在 `g_v4l2sl_lock` 下 **先自增**（`++next_buffer_id`），ffmpeg 多线程会并发进 VA
- recycle 只改用户态池，不再 QBUF 一次（否则和 decode 路径第二次 QBUF 撞 EINVAL）
- Capture 几何写回 surface（HEVC 1080 显示、1088 对齐，`vaGetImage` 用错高度会 chroma 错 15360 字节）

H.264 特有：DPB `VALID|ACTIVE`；`FLAG_PFRAME`/`FLAG_BFRAME`/`IDR`（I + `frame_num==0`，这套 libva **没有** `idr_pic_flag`）；PPS `num_ref_idx` 来自 slice；DPB 按 `frame_num` 插入排序；Annex B start code；多 slice 拼接。

HEVC 特有：SPS 是 **全局** control（`which=0`），绑 request 会 ioctl 成功但设备没配上；PCM 关闭时字段必须是 0 不是 0xff；`chroma_format_idc` 来自 `pic_fields`；`UNIFORM_SPACING`；scaling 填全 16；DPB 按 POC 分 StCurrBefore/After。

---

## AV1（已完成到 49 帧 / 本测试片）

VA 不暴露 `refresh_frame_flags`。驱动用 DPB 占用（`ref_frame_map` + 每 surface 的 `order_hint` / `av1_level1`）重建 bitmask，对齐 libaom `get_free_ref_map_index`（KEY 填满 8 槽后的重复 surface 当空槽）和 `get_refresh_idx`（跳过未来帧和最近 3 个 previous，LEVEL-1 ARF 单独计）。KEY/SWITCH=`0xff`；overlay（当前 `order_hint` 已在 DPB）=`0`。

`order_hints[]` 按 **ref type**（LAST=1…ALTREF=7）填，不是 DPB slot 下标。`skip_mode_frame[]` 按 spec 5.9.22 / ffmpeg `skip_mode_params()` 从 LAST..ALTREF 的 order hint 推，`skip_mode_frame[0]>0` 时置 `SKIP_MODE_ALLOWED`。

不要再为 ffmpeg 裸 tile 包 TILE_GROUP OBU。4K 请用 **High/Main** 编码；Constrained Baseline 本驱动没有 advertise，ffmpeg 会 `hwaccel initialisation returned error`。

---

## 进行中：AV1（历史，保留对照）

设备：hantro `rockchip,rk3588-av1-vpu-dec` = `/dev/video4`，media 必须从 sysfs 解，**不是** `/dev/media0`：

```
/sys/class/video4linux/video4/device/media*  →  /dev/media3
```

硬编码 media0 时 request 分配在错误节点，OUTPUT QBUF 会 EINVAL。

### 2026-08-27 09:26 实测（`/tmp/av1.mp4` 前 8 帧）

| 帧 | 软解 | 硬解 | |
|---|---|---|---|
| 0 (I) | `693ddf68750a001fb6c39be072726c68` | 同左 | **bit-exact** |
| 1–7 (P) | 各不相同 | 各不相同 | 全错 |

管线 120 帧都能跑完、无 ioctl 失败。以前整段近灰（Y≈0x80 vs 0x51）是 `tx_mode` 留 0（`ONLY_4X4`）。补 VA `mode_control_fields` 之后关键帧对了。

stderr 仍有一行：`v4l2stateless: derive_image: surface 1 has no decoded frame`（ffmpeg 在第一帧解码前 derive）。后面帧不再刷。

同机 GStreamer `v4l2slav1dec` 曾作为 ioctl 对照：前 49 帧对软解、从 49 开始分叉。那是 **GStreamer 自己的上限**，不是「我们也可以只做到 48」。本桥的验收仍是 ffmpeg VA-API 路径至少关键帧 + 随后若干 P 帧对软解。

### 已填进 `v4l2stateless_av1.c` 的字段

- sequence：profile / 尺寸 / bit_depth / 大部分 seq flags；`ENABLE_WARPED_MOTION` / `ENABLE_REF_FRAME_MVS` 在 `enable_order_hint` 时强制打开（VA sequence 没有这两位）
- frame：`tx_mode`、`interpolation_filter`、show / showable / superres / high-precision-mv / reference_select / reduced_tx_set / skip_mode_present
- tile_info：uniform 时按 MI 均分；`mi_rows = ceil(h/4)`，1080 → 270
- quant / loop filter（含 `ref_deltas`/`mode_deltas`/`delta_q`/`delta_lf`）
- CDEF packed strengths
- segmentation（flags + `feature_mask` + `feature_data`）
- loop restoration + `loop_restoration_size`（256 >> `lr_unit_shift`）
- global motion：VA `wm[0..6]` → V4L2 slot `1..7`（INTRA=0 保持 identity）
- film grain：从 `VAFilmGrainStructAV1` 填，不再永远 memset 0
- bitstream：ffmpeg 给的是 **裸 tile payload**（`raw_tile_group->tile_data`），`tile_offset = slice_data_offset`（通常 0）。GStreamer 走完整 TILE_GROUP OBU（约 14 字节头，`tile_offset=14`）。曾经加过自制 4 字节 OBU wrap，画面更灰，已撤回。

### 还没填 / 填错，且足够让 P 帧烂掉

内核 `rockchip_vpu981_hw_av1_dec.c` 用这些字段管参考和 CDF：

1. **`refresh_frame_flags`** — VA **不暴露** bitmask。现在 KEY/SWITCH 填 `0xff`，P 帧填 **0**。内核 `rockchip_vpu981_av1_dec_update_prob()` / `store_cdfs()` 靠这个 bit 把熵上下文写进参考槽。P 帧 0 等于从不更新 CDF → 第 1 帧开始必错。libaom 常见 inter 只 refresh LAST（`0x01`），不要猜 `0xff`。
2. **`order_hints[8]`** — VA 只有当前帧 `order_hint`。内核 `get_order_hint()` 从自己的 `frame_refs[]` 读，但 `av1_dec_frame_ref()` 仍会把 `frame->order_hints[]` 存进每个参考槽。应用侧应在 surface 上记住每帧的 hint，填 `order_hints[ref]`。
3. **`skip_mode_frame[2]`** — 仍是 `{0,0}`。GStreamer 在 `skip_mode_frame[0] > 0` 时置 `SKIP_MODE_ALLOWED`。
4. **surface `order_hint` 未存** — `v4l2sl_surface` 只有 `timestamp`。
5. **TILE_GROUP OBU** — 关键帧裸 payload 已经对上，说明 ffmpeg 路径 **不需要** 14 字节头。不要再为了对齐 GStreamer dump 去包一层。P 帧若仍错，先查 refresh/CDF，再查码流。

VA 也没有 `current_frame_id` / `buffer_removal_time` / `tile_size_bytes`。单 tile 1080p 目前看起来不是瓶颈（内核在 cols/rows log2 都为 0 时把 `tile_size_mag` 写成 3）。

### 建议的下一步（按这个顺序，不要并行乱改）

1. 给 `struct v4l2sl_surface` 加 `uint32_t order_hint`。在 `av1_fill_frame_params` 末尾把当前 `pic->order_hint` 写到 `current_surface`。
2. 填 `frame->order_hints[i]`：对每个 `ref_frame_map[i]` 取该 surface 记下的 hint。
3. 处理 `refresh_frame_flags`：
   - 先试 inter = `0x01`（只 LAST）+ KEY/SWITCH = `0xff`
   - 对照 GStreamer 同一 clip 的 FRAME control dump（`ioctlspy`）看真实 bitmask
   - 不要继续留 0
4. 按 spec 从 LAST/GOLDEN 的 order_hint 推 `skip_mode_frame[]`。
5. 再跑 `/tmp/av1.mp4` 前 8 帧，然后 49 帧。目标：ffmpeg 硬解至少覆盖 GStreamer 能对上的那段。
6. 只有 1 仍失败时才回头看 tile OBU / `tile_size_bytes`。

ioctl 对照：

```c
// ~/v4l2bridge-dev/ioctlspy.c  — LD_PRELOAD，stderr 前缀 SPY:
```

GStreamer 源码对照（填 FRAME 的权威实现）：

`https://github.com/GStreamer/gstreamer/blob/master/subprojects/gst-plugins-bad/sys/v4l2codecs/gstv4l2codecav1dec.c`

内核对照：

`drivers/media/platform/verisilicon/rockchip_vpu981_hw_av1_dec.c`

ffmpeg VA-API AV1 怎么填 picture/slice（本机副本 `/tmp/vaapi_av1.c`，Mac 上也有）：裸 tile + `tile_offset` 相对该 buffer。

---

## 设备地图（这颗 RK3588）

| 节点 | 角色 |
|---|---|
| `/dev/video1` | rkvdec H.264 / HEVC（frame-based） |
| `/dev/video4` | hantro AV1 |
| `/dev/media3` | AV1 的 media request 节点 |
| `/dev/dri/renderD128` | VA-API render 节点（桥假装成 libva 驱动） |

RK3588 主线到 7.1：解码这条能用。缺摄像头、编码器、DDR 变频、suspend。不要为了编解码去刷 vendor kernel。

---

## 明确还没做

- AV1 P 帧（见上）
- HEVC Main10（驱动 advertises，没有 10-bit 测试流）
- 中途改分辨率（没有 renegotiate）
- Firefox `about:support` Hardware decoding：profile `vaapi.default-release` 已开 `media.ffmpeg.vaapi.enabled` + `gfx.webrender.all`，需要用户登录且 source 了 `~/.profile` 再确认
- VP8 / MPEG2：vtable 里有枚举，没有 translate 实现

---

## 踩过的坑（不要再走）

- STREAMON 没做 → QBUF EPERM
- Capture 带 REQUEST_FD → EPERM
- `h264_find_ref_timestamp(dd=NULL)` → 没把 `ctx->driver_data` 接上
- timestamp 用「小整数 / 1e9」→ timeval 全 0 → DPB 对不上
- buffer ID 后置自增 + 无锁 → 两线程撞 ID → `av_image_copy` SIGSEGV
- recycle 先 QBUF 再 decode QBUF 同一 capture index → EINVAL
- HEVC SPS 放进 request → ioctl OK、设备没配置、随后 QBUF EINVAL
- AV1 request 建在 `/dev/media0` → 应为 `/dev/media3`
- 给 ffmpeg 的裸 AV1 tile 再包一层自制 OBU → 更灰
- `tx_mode` 留 0 → 关键帧也灰
- 诊断 NAS SSH 失败时怪 Clash TUN：实际是 macOS Local Network 权限
- ffmpeg **不是**源码树，不要对着不存在的本地 build 排
- 不要清主线去装 BSP/MPP「先看能不能解」

---

## 源文件职责

| 文件 | 作用 |
|---|---|
| `src/v4l2stateless.c` | libva vtable、surface、image、context、buffer ID、timestamp 分配 |
| `src/v4l2stateless.h` | 结构、池大小、helper 声明 |
| `src/v4l2stateless_device.c` | open、STREAMON、QBUF、request、sysfs media 查找 |
| `src/v4l2stateless_h264.c` | H.264 翻译 + 同步提交 |
| `src/v4l2stateless_hevc.c` | HEVC 翻译 + 同步提交 |
| `src/v4l2stateless_av1.c` | AV1 翻译 + 同步提交（P 帧未完成） |
| `src/v4l2stateless_{buffer,config,context}.c` | 几乎空的占位，逻辑都在上面几个文件 |

接活的人：先读这份，再打开 `v4l2stateless_av1.c` 的 `av1_fill_frame_params()`，从 `refresh_frame_flags` 往下做。H.264/HEVC 不要为了 AV1 去改运行时，除非新证据表明公共路径坏了。
