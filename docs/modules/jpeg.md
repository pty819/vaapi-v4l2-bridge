# v4l2stateless_jpeg.c — VEPU 编码

`VAProfileJPEGBaseline` + `VAEntrypointEncPicture`。设备
`/dev/video3`，**有状态 M2M**，不走 Request API。

`v4l2sl_jpeg_encode`：

1. 从当前 surface 取像素（cpu_ptr 或 memfd）
2. 若 `jpeg_q` 的 setup key 变了：S_FMT / S_CTRL(quality) / REQBUFS / mmap
3. 否则直接把像素拷进已映射的 OUTPUT
4. QBUF / DQBUF，把 JPEG 码流写进 VACodedBuffer

稳态约 7 ioctl、0 mmap。失败则 `m2m_teardown`，下帧重谈。
