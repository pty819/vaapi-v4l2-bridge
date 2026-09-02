#!/usr/bin/env python3
"""Unpack a raw NV16_10LE40 (V4L2 NV20) dump into planar yuv422p10le.

The rkvdec capture buffer for 4:2:2 10-bit carries Y and full-height UV
planes of tight packed-10-bit rows (stride = width * 10 / 8) plus a
colmv metadata tail appended by the kernel; this tool strips the tail.

usage: nv20_unpack.py <in.raw> <out.raw> <frames> <width> <height> <frame_size>
"""
import struct
import sys


def unpack_row(b, samples):
    vals = []
    for i in range(0, (samples // 4) * 5, 5):
        v = int.from_bytes(b[i:i + 5], "little")
        vals += [(v >> (10 * k)) & 0x3ff for k in range(4)]
    return vals


def main():
    src, dst = sys.argv[1], sys.argv[2]
    frames, w, h, fs = (int(x) for x in sys.argv[3:7])
    d = open(src, "rb").read()
    stride = w * 10 // 8
    plane = stride * h
    if plane * 2 > fs:
        raise SystemExit("frame_size %d too small for %dx%d" % (fs, w, h))
    out = bytearray()
    for f in range(frames):
        base = f * fs
        yp = d[base:base + plane]
        uvp = d[base + plane:base + 2 * plane]
        ybuf, ubuf, vbuf = [], [], []
        for r in range(h):
            ybuf += unpack_row(yp[r * stride:(r + 1) * stride], w)
            uv = unpack_row(uvp[r * stride:(r + 1) * stride], w)
            ubuf += uv[0::2]
            vbuf += uv[1::2]
        out += struct.pack("<%dH" % len(ybuf), *ybuf)
        out += struct.pack("<%dH" % len(ubuf), *ubuf)
        out += struct.pack("<%dH" % len(vbuf), *vbuf)
    open(dst, "wb").write(bytes(out))


if __name__ == "__main__":
    main()
