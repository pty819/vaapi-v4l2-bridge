# v4l2stateless_vp8.c

ffmpeg VA 给的 payload **已经剥掉** uncompressed header（key 3 字节 /
inter 10 字节）。hantro `cfg_parts()` 仍会自己 skip 那一段，所以：

- `first_part_size` 要加上 header 长度
- `dct_part_sizes[]` 按 **带 header 的码流** 填

OUTPUT `sizeimage` 约等于 luma 大小。节点与 MPEG-2 同为 hantro
`/dev/video2`，靠 fourcc 区分。
