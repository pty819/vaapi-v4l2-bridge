# v4l2stateless_av1.c

本仓库最厚的翻译器。VA-API 的 AV1 面缺三样内核要的东西，全在这里补。

## 载荷形状

内核 `AV1_FRAME` OUTPUT = **裸 tile 字节**，无 OBU。

| 客户端 | 提交 | 驱动 |
|---|---|---|
| ffmpeg | 已经是裸 tile + slice_data_offset | 按 offset 拷 |
| Chrome | 整段 OBU span，offset 指向 span 内部 | 抽各 tile 载荷拼接，TILE_GROUP_ENTRY 重定基 |

`uniform_tile_spacing`：`width/height_in_sbs_minus_1` 从自建
`mi_col/row_starts` **推导**。Chrome 在该分支填 0；照抄 VA 数组会和
网格矛盾，内核逐帧拒绝（绿屏→白屏、声音还在）。

## refresh_frame_flags

VA 结构体没有这个字段。

**真值路径** `av1_parse_hdr_refresh`：

- 遍历 span 里每一个 frame OBU（type 3 或 6）
- 按 dav1d 字段序解 uncompressed header
- 四个候选布局：可选的 screen-content / integer-MV 位（libva 不暴露 sequence 级开关）
- 校验 `frame_type` / `show_frame` / `order_hint` / `primary_ref_frame` 对得上 VA 才采纳
- 成功 ⇒ `ctx->av1.model_active = 1`

手撕坑：`order_hint_bits = f(3)+1`；两个尺寸长度前缀**连读**；开头
`show_existing_frame` 位；KEY && !show 也读 f(8)。

**启发式路径**（ffmpeg 裸 tile）：按第一帧 INTER 锁 libaom-RTC / SVT /
libaom。SVT 的 GOP 长度必须是距 KEY 的 **距离**（`key_oh`），存绝对
order hint 会在第二 GOP 崩（矩阵只解 32 帧看不见）。

BILIAV1 的槽位策略启发式推不出。Chrome 走解析器没事；ffmpeg VA 解 B 站
下载文件会花——真值没过 API。

## DPB 模型 / copy-out

`av1_release_unrefd(ctx, surf, buf, refresh)`：

- `refresh == 0`（shown leaf）：立即还 capture 槽
- 否则对每个置位的 slot：记下新 buf，若旧 buf 不再被任何 slot 引用则还池
- 清旧 surface 的 `buf_index`，保留 `has_pic`

只在 `model_active` 时运行。BeginPicture / Destroy 不得再 push。
模型在 create_context、decode_reset、ensure_capture 里 reset。

WebCodecs 每个 queued VideoFrame 一张 surface。没有 copy-out 时 40
槽大约 5 秒抽干，Chrome 回退 HEVC 且本会话不再试 AV1。

## 超 32 tile

`tile_cols * tile_rows > 32` 或 `n_slice_params_seen > 32` →
`INVALID_PARAMETER`，**任何 ioctl 之前**。截断成 32 仍提交 8×5 网格
会把 hantro 卡死、SoC 硬复位。电影流到不了这个分支。不要 VA 解
`verify/clips/av1_40tiles.mp4`。

## 控制提交

Sequence：GLOBAL + memcmp 缓存。Request-scoped 的 sequence ioctl
会成功然后 OUTPUT QBUF EINVAL。

每帧一次 request `S_EXT_CTRLS`：FILM_GRAIN + FRAME + 可选
TILE_GROUP_ENTRY。`av1_aom.mp4` strace：186 → 66 次该 ioctl。

内核即便 `apply_grain=0` 也要这个 control。libva 没有 `update_grain`
字段（2.23 和上游 master 都没有），C12 不可修。

## 其它

- super-res：`coded_w = (disp_w * 8 + denom/2) / denom`，
  `frame_width` 与 `upscaled_width` 分开填。本机 hantro 仍非 bit-exact，
  记为硬件限制。
- lossless ffmpeg `-lossless 1`：60/60 bit-exact。
- 捕获池 40，不占 CMA。
