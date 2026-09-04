# 不变量

打破这些会挂机、花屏，或 Chrome 整段会话掉软解。

1. **默认** `VIDIOC_EXPBUF` VPU capture buffer 给 Chrome 零拷贝。
   `V4L2SL_EXPBUF_EXPORT=0` 才回退 capture→GBM memcpy。
   EXPBUF ioctl 失败时驱动自己回退 GBM，不要再手写一条「禁止 EXPBUF」。
2. AV1 sequence 控制必须是 **GLOBAL**，不能 request-scoped。
3. AV1 tile 网格 >32：拒绝。禁止截断后仍提交原始 `tile_cols/rows`。
4. 不要 `pkill -9` Chrome；`gc-dbg` 是 bind mount。
5. ffmpeg 对错只看 `hwdownload` framemd5。
6. Chrome 保持 ZeroCopyGL **开启**。
7. AV1 `model_active` 时只有 DPB 模型能 `cap_pool_push`。
8. `surface_id` 在 create 时赋值。
9. `has_pic` 与 `buf_index` 不是一回事。EXPBUF 模式下 `buf_index`
   在 Chrome 导出期间必须保持（claim-at-export），`begin_picture`
   不得把槽还池。
10. 矩阵测的是 **系统目录** 里那份 `.so`：`ninja` 后必须 `sudo cp`。
11. Python 改 C 不要经 ssh heredoc 传 `\n`（会拆字符串）。写文件再 scp。
12. 换 `.so` 后重启 Chrome（会话缓存 VA 失败）。
