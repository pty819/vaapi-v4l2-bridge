# v4l2stateless_mpeg2.c

一帧可以有很多 slice（一行宏块一个）。`pending_buffers[256]` 和
`V4L2SL_MAX_SLICE_DATAS` 就是为这个。

填充：sequence / picture / quantisation 三个 control。hantro IDCT 与
ffmpeg 软解不完全相同，矩阵对的是 GStreamer `v4l2slmpeg2dec`，不是
ffmpeg SW framemd5。
