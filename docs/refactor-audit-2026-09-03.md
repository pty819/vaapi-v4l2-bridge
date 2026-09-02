# vaapi-v4l2-bridge 全量代码审计（重构前置）

- 审计对象：NAS `~/vaapi-v4l2-bridge` @ a2a41a9（2026-09-03 本地镜像，与 origin 同步）
- 方法：两路独立 fresh-eyes 子代理逐行审（核心层 7 文件 / 编解码层 8 文件，均带平台约束上下文防误报）+ 本地全文精读 tests/scripts/meson/docs + 所有无 trimmed 重指控逐条人工点验代码确认
- 规模：src 7,212 行 / tests+scripts+build 3,131 行 / 文档 ~720 行
- 结论计数：**P0×3、P1×14、P2×38、P3×25**（去重后 ~80 项）

平台约束（审阅时已排除，重构时不可"顺手修掉"）：
1. 单对象 R8 bo = 唯一导出形状（panthor 拒绝多平面 YUV bo）
2. Chromium 要求 num_objects==1（crbug 974438）
3. VPU 缓冲 EXPBUF 全面禁止（芯片级 CMA/IOMMU 挂死）；每帧 1 次 CPU 拷贝是下限
4. 惰性 memfd：bo 为逐帧快照，memfd 仅在读回需求时回填
5. 对 Chrome 实际使用的形状永不返回失败（Chrome 会话级缓存失败）

---

## 一、P0 — 崩溃 / 内存安全级（已点验确认）

**P0-1 `vaCreateSurfaces` 失败路径可释放无关存活 surface** — `v4l2stateless.c:580-597`
fail 分支先 `pthread_mutex_unlock` 再循环改 `driver_data->surfaces[]` 与 `free_surface_ids`（锁外改共享状态）；更糟的是循环扫 `surfaces[0..num_surfaces)`，但失败点之后的条目是**调用方栈上未初始化垃圾**——垃圾值恰好落在 [0,4096) 且槽位非空时，会 close+free 一个正在使用的无关 surface（双重释放/UAF）。且 `free_surface_ids[n++]` 无上界检查（头文件承诺的 bounded push 没用上）。修法：记录实际 created 数、锁内清理、用 bounded push。

**P0-2 `vaTerminate` 泄漏几乎一切** — `v4l2stateless.c:130-155`
只释放 contexts；泄漏：configs 链表、orphan_buffers（内含 vaDeriveImage 的**活跃 mmap**、malloc 的图像缓冲、buffer ID）、surfaces[4096] 全表（各带 cpu_ptr/memfd/gbm_bo）。浏览器/ffmpeg 在同进程反复建拆 VA display → fd 与内存持续流失直到耗尽。`close(media_fd)` 是死代码（恒 -1）。

**P0-3 VPP `output_region` 未钳制 → 堆越界写** — `vpp.c:194-197 → 238-248 / 372-379`
`dw/dh` 直接来自客户端 `pipe->output_region`，无 `min(dw, dst->width)` 钳制就按 `dh*3/2` 行写 `dst->cpu_ptr`。output_region 大于目标 surface（API 允许的客户端 bug）= 堆溢出。同文件 vaEndPicture(VPP) 的 VA blit 路径（v4l2stateless.c:1759）已有钳制示范。潜在触发（当前无已知客户端这么干），但必须修。

## 二、P1 — 正确性（潜在踩雷，已点验/高置信）

| # | 位置 | 问题 |
|---|---|---|
| C1 | `v4l2stateless.c:643-659` | `vaDestroySurfaces` 不归还 `buf_index` 到 free_cap 池 → Chrome 表面池 churn 后 24 个捕获槽耗尽，之后每帧 "no free capture buffer"（正是 Chrome 缓存失败最怕的场景）。render_targets[] 残留旧 ID + ID 复用可致误认；current_surface 可悬挂 |
| C2 | `v4l2stateless.c:373, 427-434` | QueryConfig/SurfaceAttributes 遍历 config 链表**不持锁**，与持锁的 DestroyConfig 竞争 → UAF（违反本文件自己的锁契约） |
| C3 | `v4l2stateless.c:702-722` | config_id 无效时 `device_path=NULL` 传给 open() + NULL `%s` 打印（非 glibc UB）；且 device_path 别名 config->device_path，DestroyConfig 后悬挂 |
| C4 | `device.c:892-903` | `grow_memfd` 的 ftruncate 可**缩小**客户端仍映射/导出的 memfd → 分辨率降档时 SIGBUS（bind_capture_export/pull_capture 传新 cap_sizeimage）。只许增长 |
| C5 | `hevc.c:228-247, 331` | HEVC 从不收集 `VAIQMatrixBufferType`，scaling matrix 恒 memset-16 → 带 scaling list 的 HEVC 流全错（ffmpeg 一定发该缓冲）。测试片没用该特性所以矩阵全绿 |
| C6 | `mpeg2.c:115-120` | MPEG-2 chroma 量化矩阵无加载时保持全零，按 H.262 应回退 luma 矩阵。注：现有 gst 对照全绿，说明 rkvdec 内核侧大概率不消费这些矩阵——属"规范正确性"修复，非当前画质 bug |
| C7 | `v4l2stateless.c:1663` | `vaGetImage` 的 dst_stride 按本次 blit 宽度重算，而非 image 分配时的 stride → 子矩形/窄面读取时行距错位、花屏却返回 SUCCESS |
| C8 | `v4l2stateless.c:1758-1762` | `vaPutImage` 无视 image fourcc（I420/BGRA 静默按 NV12 解释）且假定 pitch==width |
| C9 | `device.c:1091-1095` | DQBUF 带 `V4L2_BUF_FLAG_ERROR` 的缓冲当好帧返回 → 垃圾数据 + SUCCESS |
| C10 | `device.c:446 等 / vpp.c:356` | `poll()` 无 EINTR 重试：信号（性能采样器/SIGWINCH）被当超时 → 全量 decode_reset（STREAMOFF/重建池），周期性吞吐塌方 |
| C11 | `h264.c:385 / hevc.c:239` | slice data 超 32 个静默丢弃（pending_buffers 有 256 槽却不用）→ 无诊断的坏帧 |
| C12 | `av1.c:966-973` | film grain 缺 `update_grain` 标志映射 → 内核无法区分"沿用上帧 grain"与"中途关闭" |
| C13 | `av1.c:84-87` | 篡改事实的启发式：enable_order_hint 时强制置 REF_FRAME_MVS/WARPED_MOTION，覆盖 72-75 行已映射的真值；SEPARATE_UV_DELTA_Q 同族 |
| C14 | `format.c:200-235` | 10bit 4:2:2 转换器 32KB 栈缓冲 + width>4096 静默截断，但驱动宣称 MaxWidth 8192 |

## 三、P1 — 性能（每帧热路径的系统性浪费）

**Chrome 解码热路径（1080p 每帧，可削减的 syscall ~7-8 个）：**

| 位置 | 现状 | 修法 | 收益/帧 |
|---|---|---|---|
| `v4l2stateless.c:1101-1105` | 每帧 close 旧 request fd + MEDIA_REQUEST_ALLOC 新建 | 持久 request + `MEDIA_REQUEST_IOC_REINIT` | −2 syscall |
| `h264.c:473` (+hevc:277/av1:922/mpeg2:187 同病 ×4) | 全局 SPS/sequence 控制每帧重发（内容逐字节不变） | ctx 内缓存 + memcmp 变更才发 | −1 syscall ×4 编码 |
| `h264.c:504/519/534/550` | SPS/PPS/decode_params/scaling 各自独立 S_EXT_CTRLS | 单 ext_control[] 批量提交 + 变更检测 | −2~3 syscall |
| `device.c:791-801` | 每次 QBUF 前 QUERYBUF 读常量长度 | REQBUFS 时缓存每槽 length | −1 syscall |
| `device.c:955-959` / `gbm.c:85-95` / `v4l2stateless.c:1446,1671` | memfd/bo 的 mmap+munmap 每帧进出（围绕那次必要拷贝） | surface 持久映射复用 | −2 mmap/munmap（gbm_bo_map 底层还可能各带 ioctl） |

**非 Chrome 路径（ffmpeg 类客户端，收益更大）：**

- `vpp.c:251-401`：VPP 每帧完整 M2M 搭拆（S_FMT×2、S_CTRL×3、REQBUFS×2、QUERYBUF×2、2 mmap、STREAMON×2、QBUF×2、STREAMOFF×2、REQBUFS(0)×2 ≈ **18 ioctl+2 mmap/帧**）→ 格式不变时持久队列只 QBUF（注释自述 RGA fd 会楔死才全拆——可保留 STREAMOFF、免掉 REQBUFS 与 mmap 重建）
- `jpeg.c:196-398`：JPEG 编码每帧 ~12 ioctl + 3 mmap/3 munmap 同病
- `v4l2stateless.c:561-567`：**每个 surface 创建即 calloc 全帧 cpu_ptr**（1080p ~3MB ×20 surface 池 ≈ 60MB 纯浪费；4K 240MB）；解码数据从不经过它。VPP 已示范惰性分配（vpp.c:238-248）——移到首个写入者
- `format.c:274-290`：image stride 无对齐（stride==width），所有逐行拷贝走非对齐路径；创建时对齐 + pitches[] 如实上报是全局小赢
- `vpp.c:402-404` / `v4l2stateless.c:1703`：热路径 fprintf 未设门（VPP 成功日志每帧打；GetImage 不支持格式每帧刷屏）——统一 gate 到 V4L2SL_DEBUG

## 四、P2/P3 — 质量、重复、死代码（重构主战场）

**状态模型（头文件）：**
- `dma_buf_fd` 实为 memfd —— 命名谎言，改 `memfd_fd`
- `gbm_src`+`memfd_stale` 双标记编码同一状态机且有写漂移（`gbm.c:186-195`：从 memfd 上传 bo 后不置 gbm_src=3，快路径永不命中；VPP 写 cpu_ptr 只置 gbm_src=1 不清 memfd_stale）→ 单一 `enum last_writer {CPU, MEMFD, BO}`，staleness 派生
- 死字段：`driver_data->media_fd`、`lock`（注释自认 unused）、`output_buf_length[]`（全树零引用）
- AV1 启发式 7 个字段长在通用 context 上 → 收进子结构
- 3 个空壳文件 buffer/config/context.c（6 月 19 日的"Phase 2"占位 TODO）——要么删掉要么真的把 2000 行的 v4l2stateless.c 拆进去

**重复聚类（跨文件 ×N）：**
1. `xioctl` EINTR 包装 ×3（device.c:39 / vpp.c:22 / jpeg.c:24，vpp 版还绕过 ioctl hook）
2. surface 查找绕过 `v4l2sl_surface_by_id` 硬编码 4096 ×6（device.c:332,992 / vpp.c:54 / av1.c:90,210,792 / jpeg.c:33）
3. PRIME descriptor 填充 ×2（device.c:1033 vs gbm.c:222，~60 行同构且已分叉）
4. `context_for_surface` 在同文件内复制 ×2（1412 vs 1530）
5. Annex-B 扫描+拼接 ×3（h264:585 / hevc:377 / mpeg2:245，前缀长度 3/4 无故不一）
6. buffer 收集循环 ×5（各 codec translate 开头）
7. 属性表 ×2 互相矛盾（config 属性说 8192、SurfaceAttrib 说 4096）
8. probe 扫描器三份实现（scan_decoder_paths_ex / scan_aux_paths / scan_all_uncached，~150 行重叠且接受准则漂移 ：240 vs :508）
9. find_ref_timestamp 包装 ×2 重复内联 `v4l2sl_surface_ts`（h264:83 / hevc:153）
10. 行拷贝助手 ×3（gbm.c copy_plane / format.c 转换器 / vpp.c mmap_copy_plane）
11. 转换器尾部像素处理不一致（nv20_to_yuy2 奇宽丢尾像素；unpack_le40 余数循环可越 1 字节）

**死代码清单（直接删，~500-600 行）：**
- `h264.c:434` `if (0 && ...)` 调用 NULL×4 的实验残骸（一开就崩）
- `h264_fill_slice_params` 全函数（唯一调用者是上面的 if(0)，体内还是占位符）
- `av1.c:509-526` 不可达 SVT refresh 块 + `av1_svt_layer_slot`(410) + `av1_svt_pending_arf`(305) 两死函数
- `device.c:843-857` `v4l2sl_export_dmabuf`（EXPBUF 禁令的 API 化身，删掉防未来"修复"）
- `probe.c` `scan_decoder_paths`/`scan_aux_paths` 死公共 API（~50 行）
- `format.c:36-44` `capture_is_10bit/422` 零调用
- `jpeg.c:33-39` `(void)surface_by_id;` 骗 -Wunused 的死函数
- `device.c:1143-1170` 描述不存在机制的注释块 + 重复 define guard
- 头文件 3 死字段 + v4l2stateless.c 死 `close(media_fd)`
- 过时注释：h264.c:419（说传 NULL 实际传 dd）、vpp.c:82-87 冗余分支、`h264 level_idc=40` 硬编码（4K 应 51/52）

**其他值得记的：**
- `vpp.c:161,392-401` 早退路径用未初始化 oreq/creq 去 REQBUFS(0)（栈垃圾进 ioctl）
- `vpp.c` 大小写 bpp 表达式文本相同待提取；I420 走 1-plane mplane 的 RGA 路径未验证（建议从支持表撤下）
- `v4l2stateless.c:1305` vaSyncSurface 无条件置 Ready（Skipped 帧状态自相矛盾）
- `:210` num_entrypoints 无 NULL 防护；`:1488` 无效三元；`:1966-1980` vtable 桩 (void*) 转换 UB
- derive_image 的 VAImage.format 缺 byte_order/depth/bits_per_pixel 字段
- `vaDestroyImage` 未知 ID 返回 SUCCESS（与 DestroyBuffer 契约相反）

## 五、tests / scripts / meson（本人逐行）

1. **smoke.sh:36 已被设计漂移打断**：仍要求 `VAProfileAV1Profile0`（AV1 已刻意下架）→ 此脚本现在必然 FAIL；av1_default.mp4 生成也早已注释。同步矩阵的 AV1 决策或删掉该检查
2. **三份 shell 测试互相抄**：run_full_matrix.sh / smoke.sh / vaapi_hwdownload.sh 各带一份 forbidden()/used_hw/pair 实现，已三处分叉（vaapi_hwdownload.sh 还硬编码 /tmp/av1.mp4 等早期临时路径——**删除候选**）
3. run_full_matrix.sh 缩进层积混乱（182-191、234-324 tab/sp 混杂）；`enc()` 打印 exit 却不判失败（编码失败→下游莫名 HW_EXIT）；h26410/hevc10 手抄 pair_sw（应参数化 pix_fmt）
4. 测试断言 grep 驱动日志字符串（"config uses /dev/video"、"fourcc=NV15"）——驱动改文案就翻车，建议驱动输出机器可读标记或测试侧统一变量
5. test_export_recapture.c：`recap_env_open` 抽取后，`test_av1_translate_recapture`(310-440) 与 `test_recapture_small_to_large`(208-308) 两个旧测试未迁移（~200 行重复搭建）
6. va_export_client.c / va_h264422_client.c / va_export_client 的 pp/iq 表与解码循环三处近似复制（可提公共测试助手）；W/H 硬编码 1280x720 与片源耦合
7. wl_import_probe.c:135-142 诊断打印 exts 字段标错（实为 dmabuf_import 的重复值）
8. meson：test 目标手工罗列源文件清单，4 份清单互相重叠且已漂移（test_format 拉进 vpp.c 只为链接 ensure_memfd 链）——考虑一个静态库目标 `libv4l2sl_core` 供测试与 .so 共用，杜绝"ninja 漏 stale 二进制"这一本会踩过的坑
9. firefox-vaapi-user.js 注释仍写 AV1 8-bit 支持（已下架，注释过时）

## 六、重构提案（按风险递增排序，每阶段矩阵全绿再进下一阶段）

**Phase 0 — 安全网（半天）**
修 smoke.sh AV1 漂移；合并三份 shell 测试为 matrix+smoke 两层；meson 改静态库共享源；可选：ASan 编译目标跑单元测试（P0-1/P0-2 可被它立刻抓现行）。

**Phase 1 — 纯删除（低风险，-500~600 行）**
死代码清单全删 + 3 空壳文件处理 + 过时注释清理。零行为变化，矩阵即回归。

**Phase 2 — 内存安全（高价值）**
P0-1/2/3 + C1-C4（锁覆盖、capture 槽归还、grow_memfd 只增、create_context 早退）。每项能配单测的配单测（mock ioctl 体系现成）。

**Phase 3 — 去重聚类（-300~400 行，纯移动）**
xioctl 共享、surface_by_id 全面收口、buffer 收集循环提取、Annex-B 拼接助手、descriptor 填充合并、属性表合一、probe 扫描器合一、gbm_src/memfd_stale → last_writer 枚举、dma_buf_fd→memfd_fd、AV1 状态收进子结构。

**Phase 4 — 性能（逐项测量进、逐项可回退）**
顺序按收益/风险：① 持久 request fd（REINIT）② SPS/sequence 变更检测缓存 ③ 控制批量提交 ④ QUERYBUF 长度缓存 ⑤ surface 持久映射（memfd+bo）⑥ cpu_ptr 惰性分配 ⑦ VPP/JPEG 持久队列 ⑧ image stride 对齐。
验收：strace 统计每帧 syscall 数（当前基线 ~13 可减半）+ Chrome 实机 GPU 进程 CPU% 对照 + 矩阵 MD5 全绿。
预期：Chrome 路径 CPU 再省约 1-2 个百分点（同惰性 memfd 量级）+ 延迟抖动收敛；ffmpeg VPP/JPEG 路径 ioctl 降 ~70%。

**Phase 5 — 结构（可选，最后做）**
v4l2stateless.c（2000 行）按空壳文件的许诺拆分：config/context/surface/buffer 各归其位；导出门户与 GBM 模块保持不动（那是踩了芯片坑换来的，注释里写清楚）。

**总计预期**：净删 ~900-1000 行（src 的 12-14%），P0 清零，每帧 syscall 约减半。

## 附：与既往提交的对照

- 惰性 memfd（a2a41a9）引入的 `gbm_src=3` 快路径设计与本审计兼容，但其状态标记的双字段表达是过渡态 → Phase 3 枚举化
- 钉核（2c8f22b）与本审计无关（wrapper 层）
- Chrome ops 文档（7393318）无需随审计改动（除非 Phase 4 改变日志文案——见测试断言耦合项）
