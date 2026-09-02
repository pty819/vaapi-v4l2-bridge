# HANDOFF — VA-API → V4L2-stateless 桥接层

写于 **2026-08-27**，刷新 **2026-08-29**。机器：Orange Pi 5 NAS `192.168.1.21`，Armbian 26.8.3 resolute，kernel **7.1.8-edge-rockchip64**。

全量矩阵 `tests/run_full_matrix.sh`：**PASS=25 FAIL=0**（新增 h26410 + h264422×4 条目）。Git 仓库在 NAS：`https://github.com/pty819/vaapi-v4l2-bridge.git`。

安装 `.so`：`/usr/lib/aarch64-linux-gnu/dri/v4l2stateless_drv_video.so`。

---

## 一句话

C 驱动 `v4l2stateless_drv_video.so` 把 ffmpeg VA-API 硬解接到主线 V4L2-stateless。节点按 OUTPUT fourcc 选，不写死 `/dev/videoN`。

| 编码 | 设备 | 状态 |
|---|---|---|
| H.264 CB / Main / High | rkvdec `/dev/video1` | **完成**：含 B / 全 P / 4-slice / 4K / QCIF |
| H.264 High10 | 同上，capture NV15 | **完成**：hw==sw framemd5；根因=ffmpeg VA 的 pic_init_qp_minus26 含 10bit QpBdOffsetY(+12)，内核要裸值，剥掉即好 |
| HEVC 8-bit Main | 同上 | **完成**：Main + WPP + 4K |
| HEVC Main10 | 同上，NV15 → P010 | **完成**：`hwdownload,format=p010le` 对软解 |
| AV1 8-bit Profile0 | hantro `/dev/video4` + `/dev/media3` | **完成**：libaom、libaom realtime、SVT-AV1 RA、4K |
| VP8 | hantro `/dev/video2` | **完成**：480p / 720p vs ffmpeg SW |
| MPEG-2 Simple / Main | 同上 `/dev/video2` | **完成**：vs GStreamer `v4l2slmpeg2dec`（hantro IDCT ≠ ffmpeg SW） |
| JPEG Baseline encode | VEPU121 `/dev/video3` | **完成**：`mjpeg_vaapi`（stateful M2M） |
| VPP | RGA `/dev/video0` | **完成**：`scale_vaapi` |

约束没变：只走主线 edge，禁止 vendor/BSP/MPP；成功判定必须是 `hwdownload` 后的 framemd5（MPEG-2 对 GST），禁止把静默软解当硬解成功。

---

## 路径与部署

| 角色 | 路径 |
|---|---|
| NAS 工程 | `/home/liyifan/vaapi-v4l2-bridge/` |
| NAS 源码 | `~/vaapi-v4l2-bridge/src/` |
| NAS 构建 | `~/vaapi-v4l2-bridge/builddir/`（meson + ninja） |
| 已安装 .so | `/usr/lib/aarch64-linux-gnu/dri/v4l2stateless_drv_video.so` |
| Mac 工作副本 | `~/v4l2bridge-dev/`（改完 scp 到 NAS 再 ninja） |
| 探针 / dump | Mac `~/v4l2bridge-dev/{ioctlspy.c,ctrldiff.py,av1probe.c,dumpdec.c}` |
| ffmpeg | **apt** `/usr/bin/ffmpeg` `7:8.0.1-3ubuntu2`，不是本地编的 |
| 驱动选择 | `~/.profile` 与 `~/.config/environment.d/90-libva.conf`：`LIBVA_DRIVER_NAME=v4l2stateless` |
| SSH | `liyifan@192.168.1.21`（密钥） |
| git | NAS 仓库 `master` → `origin` = `https://github.com/pty819/vaapi-v4l2-bridge.git` |

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

全量矩阵（写 `verify/`，已 gitignore）：

```bash
bash ~/vaapi-v4l2-bridge/tests/run_full_matrix.sh
```

---

## 验证方法（唯一有效）

不要用裸 `-f framemd5`。ffmpeg 硬解失败时会静默回软解，hash 会对、硬件没干活。矩阵脚本还会拒绝 `hardware accelerator failed` / `Failed to query surface attributes`，并要求日志里出现 `v4l2stateless: .* config uses /dev/video`。

```bash
LIBVA_DRIVER_NAME=v4l2stateless /usr/bin/ffmpeg \
  -hwaccel vaapi -hwaccel_output_format vaapi -vaapi_device /dev/dri/renderD128 \
  -i FILE -vf "hwdownload,format=nv12" -pix_fmt yuv420p -frames:v N \
  -f framemd5 -y /tmp/hw.md5

/usr/bin/ffmpeg -i FILE -pix_fmt yuv420p -frames:v N -f framemd5 -y /tmp/sw.md5
diff -u /tmp/sw.md5 /tmp/hw.md5
```

MPEG-2 对 GStreamer，不要对 ffmpeg SW：

```bash
gst-launch-1.0 filesrc location=FILE ! parsebin ! v4l2slmpeg2dec ! \
  videoconvert ! video/x-raw,format=I420 ! \
  checksumsink eos-after=N hash=md5
```

`derive_image: surface N has no decoded frame` 在第一帧前出现过一次，不影响后续比对。

测试流生成在 NAS `~/vaapi-v4l2-bridge/verify/clips/`（gitignored）。

---

## 已完成：H.264 / HEVC

两路都走 rkvdec **frame-based** UAPI（kernel 7.0+），不是 slice-based。

关键运行时：

- 两边队列 `VIDIOC_STREAMON`（HEVC/AV1 要等全局 SPS/sequence 之后）
- Capture QBUF **不能**带 `V4L2_BUF_FLAG_REQUEST_FD`（带了 vb2 直接 EPERM）
- OUTPUT 4 槽 + CAPTURE 24 槽的池，禁止写死 index 0
- 同步解码：S_EXT_CTRLS → QBUF out（带 request）→ QBUF cap（裸）→ `MEDIA_REQUEST_IOC_QUEUE` → poll → DQBUF
- surface timestamp：纳秒、1µs 步进；QBUF timeval 必须 `tv_sec = ns/1e9`，`tv_usec = (ns%1e9)/1000`；DPB `reference_ts` 用同一套 ns
- buffer ID 在 `g_v4l2sl_lock` 下 **先自增**（`++next_buffer_id`），ffmpeg 多线程会并发进 VA
- recycle 只改用户态池，不再 QBUF 一次（否则和 decode 路径第二次 QBUF 撞 EINVAL）
- Capture 几何写回 surface（HEVC 1080 显示、1088 对齐，`vaGetImage` 用错高度会 chroma 错 15360 字节）

H.264 特有：DPB `VALID|ACTIVE`；`FLAG_PFRAME`/`FLAG_BFRAME`/`IDR`（I + `frame_num==0`，这套 libva **没有** `idr_pic_flag`）；PPS `num_ref_idx` 来自 slice；DPB 按 `frame_num` 插入排序；Annex B start code；多 slice 拼接。已 advertise Constrained Baseline。

HEVC 特有：SPS 是 **全局** control（`which=0`），绑 request 会 ioctl 成功但设备没配上；PCM 关闭时字段必须是 0 不是 0xff；`chroma_format_idc` 来自 `pic_fields`；`UNIFORM_SPACING`；scaling 填全 16；DPB 按 POC 分 StCurrBefore/After。

---

## AV1（refresh_frame_flags 推断）

VA **不暴露** `refresh_frame_flags`。驱动按第一帧 INTER 锁风格，再填 bitmask：

| 风格 | 判定（KEY 之后第一帧 INTER） | refresh |
|---|---|---|
| `LIBAOM_RTC` | 第一帧 INTER **shown** | `1u << (order_hint % 6)`，槽 6–7 留 KEY |
| `SVT` | 第一帧 INTER hidden + `showable` + `primary_ref_frame != 7` | shown 叶 = 0；第一个 hidden ARF 记下 GOP/L0；之后 `cur > av1_l0_oh` 为 L0；其余 hidden 用 `g = l0_oh - prev_l0_oh` 的层图 |
| `LIBAOM` | 其它（filtered ARF：not showable，`primary_ref=7`） | DPB 占用 + `get_free_ref_map_index` / `get_refresh_idx`（8 槽 first_dup） |

`order_hints[]` 按 **ref type**（LAST=1…ALTREF=7）填。`skip_mode_frame[]` 按 spec 5.9.22 / ffmpeg `skip_mode_params()` 推。

不要再为 ffmpeg 裸 tile 包 TILE_GROUP OBU。`tx_mode` 必须来自 VA `mode_control_fields`（留 0 = ONLY_4X4，关键帧也灰）。AV1 media 节点从 sysfs 解，**不是** `/dev/media0`：

```
/sys/class/video4linux/video4/device/media*  →  /dev/media3
```

---

## VP8

`src/v4l2stateless_vp8.c`。ffmpeg VA 给的是 **已经剥掉 uncompressed header** 的 payload（key 3 字节 / inter 10 字节）。hantro `cfg_parts()` 仍会自己 skip 那一段，所以驱动要把 header 长度加回 `first_part_size`，并按 *带 header 的码流* 填 `dct_part_sizes[]`。OUTPUT sizeimage ≈ luma。

---

## MPEG-2

`src/v4l2stateless_mpeg2.c`。ffmpeg 每个 MB 行一片 slice；`pending_buffers` 必须 ≥ 1080p 的行数，以前 32 会静默丢片、I 帧花。现为 **256**。

hantro IDCT 与 ffmpeg SW 不对。验收只对同机 `v4l2slmpeg2dec`。framemd5 比 hash、不要比 SAR（GST 写 1/1，ffmpeg 常写 0/1）。

---

## 设备地图（这颗 RK3588）

| 节点 | 角色 |
|---|---|
| `/dev/video0` | RGA VPP |
| `/dev/video1` | rkvdec H.264 / HEVC（frame-based） |
| `/dev/video2` | hantro VP8 + MPEG-2 |
| `/dev/video3` | VEPU121 JPEG encode |
| `/dev/video4` | hantro AV1 |
| `/dev/media3` | AV1 的 media request 节点（sysfs 跟 video4，不要写死 media0） |
| `/dev/dri/renderD128` | VA-API render 节点（panthor，platform DRM，不是 PCI） |

RK3588 主线到 7.1：解码这条能用。缺摄像头、编码器、DDR 变频、suspend。不要为了编解码去刷 vendor kernel。

---

## 上游调研（2026-08-26）：为什么必须自己做

结论：**上游不存在可用的 VA-API ↔ V4L2-stateless 翻译驱动**。

- [bootlin/libva-v4l2-request](https://github.com/bootlin/libva-v4l2-request)：停在 **2019-05**，只 Allwinner Cedrus MPEG2/H.264/H.265。
- FFmpeg 官方 v4l2-request hwaccel：Jonas Karlman (Kwiboo) [WIP PR #20847](https://code.ffmpeg.org/FFmpeg/FFmpeg/pulls/20847)，未合并。
- 字段对照：[Kwiboo FFmpeg fork](https://github.com/Kwiboo/FFmpeg) `v4l2-request-n7.1.3`（不走 VA-API，参数从码流直出）。
- Chromium 直连 V4L2 stateless；GStreamer `v4l2codecs` 是 Collabora 用户态。vendor 的 libmpp + BSP 违反主线-only。

**4K30 CMA**：hantro AV1 每帧约 13MB 连续内存，`cma=256M` 碎片化后分配失败会静默回软解。`/etc/default/u-boot`（及 extlinux.conf）`cma=256M`→`cma=1G` 后 `u-boot-update`。

---



桌面应用（Chrome 包装脚本、Firefox user.js、VLC vaapi）的可复制说明在仓库 [APPS.md](APPS.md)，包装脚本在 `scripts/google-chrome-vaapi`。

## 官方 Chrome（VA-API，不是 Chromium 那条 V4L2 直连）

Debian/XtraDeb 的 arm64 Chromium 编的是 `use_v4l2_codec`，直连 `/dev/video*`。官方 Linux arm64 Chrome 编的是 `use_vaapi`，路径是：

`Chrome → libva → LIBVA_DRIVER_NAME=v4l2stateless → v4l2stateless_drv_video.so → /dev/video*`

驱动里 `vaQueryConfigAttributes` 已按 libva 规范把 `*num_attribs` 当**纯输出**（Chrome `FillProfileInfo_Locked` 传入时未初始化）。缺 `VAConfigAttribRTFormat` 的 YUV420 会把所有 decode profile 划掉。`max_attributes` 为 32。提交 `ae4337f`。

21 上菜单入口走仓库脚本 `scripts/google-chrome-vaapi`（安装为 `/usr/local/bin/google-chrome-stable`，用户 `.desktop` 覆盖系统菜单项）。Firefox 为 Mozilla `.deb` + `scripts/firefox-vaapi-user.js`。细节见 [APPS.md](APPS.md)。

## 明确还没做 / 已知限制

- **VP9**：本机 hantro 无 VP9 fourcc，没做
- **浏览器强制硬解 VP9 / 10-bit HDR**：不要开 `media.hardware-video-decoding.force-enabled`，这颗 VPU 会被打挂
- **浏览器中途改分辨率**：驱动会对 capture 做 STREAMOFF / S_FMT / REQBUFS renegotiate；Chrome/Firefox 这条没有矩阵覆盖
- **H.264 High422 已验收（09-02 晚）**：8/10bit hw==sw bit-exact。ffmpeg 到不了桥是上游双门控（h264 get_pixel_format 只给 4:2:0 加 VAAPI 候选 + vaapi profile map 无 H264_HIGH_422），不是桥的问题；VA 路径用 tests/va_h264422_client.c 全链路驱动，内核路径 GST v4l2slh264dec 对 avdec_h264（10-bit 经 tests/nv20_unpack.py 解包含 colmv 尾）。NV20 capture stride=1600、帧尾 460800B 是 colmv；GST videoconvert 的 10bit 解包有 ±1 舍入，勿用它做位级判据
- 官方 Chrome **必须**走包装脚本（`scripts/google-chrome-vaapi` / `/usr/local/bin/google-chrome-stable`）。裸跑 `/usr/bin/google-chrome-stable` 会跳过非 PCI 的 panthor

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
- AV1 P 帧 `refresh_frame_flags=0` → CDF 永不更新
- libaom realtime 用 8 槽 first_dup → 第 7 帧错；应锁 RTC `order_hint % 6`
- SVT shown 叶 refresh 非 0、或把 shrinking mini-GOP（oh 24/28/30）当普通 GOLDEN → 第 15 帧后错
- MPEG-2 `pending_buffers[32]` → 1080p I 帧丢 slice
- VP8 按 ffmpeg 已剥 header 的 `first_part_size` 原样下发 → hantro 再 skip 一次，分区全错
- 诊断 NAS SSH 失败时怪 Clash TUN：实际是 macOS Local Network 权限
- ffmpeg **不是**源码树，不要对着不存在的本地 build 排
- 不要清主线去装 BSP/MPP「先看能不能解」

---

## 源文件职责

| 文件 | 作用 |
|---|---|
| `src/v4l2stateless.c` | libva vtable、surface、image、context、buffer ID、timestamp、EndPicture 分发 |
| `src/v4l2stateless.h` | 结构、池大小（pending 256）、helper 声明 |
| `src/v4l2stateless_device.c` | open、STREAMON、QBUF、request、sysfs media 查找 |
| `src/v4l2stateless_probe.c` | 按 OUTPUT fourcc 选节点（`scan_decoder_paths_ex` 含 VP8/MPEG-2） |
| `src/v4l2stateless_h264.c` | H.264 翻译 + 同步提交 |
| `src/v4l2stateless_hevc.c` | HEVC 翻译 + 同步提交 |
| `src/v4l2stateless_av1.c` | AV1 翻译 + refresh 推断 |
| `src/v4l2stateless_vp8.c` | VP8 翻译 |
| `src/v4l2stateless_mpeg2.c` | MPEG-2 翻译 |
| `src/v4l2stateless_jpeg.c` | JPEG Baseline 编码（stateful M2M） |
| `src/v4l2stateless_vpp.c` | RGA VPP |
| `src/v4l2stateless_format.c` | NV12/NV15/P010/YUY2 与 PRIME fourcc |
| `scripts/google-chrome-vaapi` | 官方 Chrome 包装（render-node + 关 GPU sandbox） |
| `scripts/firefox-vaapi-user.js` | Firefox VA-API prefs |
| `APPS.md` | Chrome / Firefox / VLC 接法 |
| `tests/run_full_matrix.sh` | 主机全量 hwdownload 矩阵 |
| `tests/test_video_probe.c` | fourcc → 节点（含 live） |
| `tests/test_vp8_mpeg2_fill.c` | VP8/MPEG-2 control 填充单测 |

GStreamer AV1 对照：`gstv4l2codecav1dec.c`。内核：`drivers/media/platform/verisilicon/rockchip_vpu981_hw_av1_dec.c`。
- **7ec4e39 的 ZeroCopyGL disable 曾把 Chrome 硬解修坏（09-02 深夜定位并已修复）**：真实根因不是流——包装脚本  把 VaapiVideoDecoder 逼进 ImageProcessor 输出路径，本平台没有 ImageProcessor（vmodule 实锤："Unable to find ImageProcessor to convert format" + CroStatus 6 → PIPELINE_ERROR_DECODE），Chrome 静默降级 FFmpeg 软解且整个会话不再重试。ZeroCopyGL 保持默认开。（**此句 09-03 起过时**：GBM 显示 surface 落地后零拷贝导入每帧成功、零 eglCreateImage 错误，见下方 09-03 段；当时的“失败刷噪音+回退 CPU 拷贝”描述作废。）包装脚本已改回只禁 Vulkan 并写了长注释防再犯；bilibili 直播实测 VaapiVideoDecoder 稳定、GPU 进程 61 个解码 surface。更早的 mid-GDP 首帧理论作废

## 2026-09-03 — GBM display surfaces (Chrome hw decode + visible picture)

- Chrome zero-copy GL path = decode surface --VAProc blit--> OUTPUT surface
  --> vaExportSurfaceHandle. Exported surfaces have `buf_index=-1`, no decode
  context; gate on `format == NV12` as well. Symptom when wrong: GPU-process
  `eglCreateImage failed 0x3003` every frame + black video (canvas avg 0).
- Fastest black-screen triage: LD_PRELOAD `tests/ioctl_interpose.so` into
  Chrome and read `/tmp/chrome_main.log` INTERPOSE lines — shows exactly
  which fd PRIME-import receives (`memfd:v4l2sl-surf` = lie path,
  `/dmabuf:` = real).
- Mesa 26.0.8 panfrost: GR88 single-plane dmabuf import samples (0,0); NV12
  single-object imports are bit-exact. panthor gbm: no multiplanar YUV bos.
- `git push` from the NAS to GitHub is flaky over https (GnuTLS reset) and
  port-22/443 SSH is intercepted by the router proxy (28.0.0.x closes);
  retrying https a few times with 25 s gaps works.
