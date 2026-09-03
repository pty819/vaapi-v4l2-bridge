/*
 * Pixel-format helpers: V4L2 capture fourcc ↔ VA image fourcc, size, convert.
 *
 * rkvdec exposes NV12 / NV15 / NV16 / NV20. VA-API clients want NV12 / P010 /
 * YUY2. NV15 is packed 10-bit 4:2:0 (4 samples / 5 bytes, little-endian);
 * P010 stores 10 bits in the high end of a uint16 (sample << 6).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va.h>
#include <linux/videodev2.h>
#include <drm_fourcc.h>

#include "v4l2stateless.h"

uint32_t v4l2sl_capture_fourcc_from_rt(unsigned int rt_format)
{
    if (rt_format & VA_RT_FORMAT_YUV422_10)
        return V4L2_PIX_FMT_NV20;
    if (rt_format & VA_RT_FORMAT_YUV420_10)
        return V4L2_PIX_FMT_NV15;
    if (rt_format & VA_RT_FORMAT_YUV422)
        return V4L2_PIX_FMT_NV16;
    return V4L2_PIX_FMT_NV12;
}

uint32_t v4l2sl_capture_fourcc_from_sps(int bit_depth_minus8, int chroma_format_idc)
{
    int ten = bit_depth_minus8 >= 2;
    if (chroma_format_idc == 2)
        return ten ? V4L2_PIX_FMT_NV20 : V4L2_PIX_FMT_NV16;
    return ten ? V4L2_PIX_FMT_NV15 : V4L2_PIX_FMT_NV12;
}

uint32_t v4l2sl_va_fourcc_for_capture(uint32_t v4l2_fourcc)
{
    switch (v4l2_fourcc) {
    case V4L2_PIX_FMT_NV15:
        return VA_FOURCC_P010;
    case V4L2_PIX_FMT_NV16:
    case V4L2_PIX_FMT_NV20:
        return VA_FOURCC_YUY2;
    default:
        return VA_FOURCC_NV12;
    }
}

uint32_t v4l2sl_drm_fourcc_for_capture(uint32_t v4l2_fourcc)
{
    switch (v4l2_fourcc) {
    case V4L2_PIX_FMT_NV15:
        return DRM_FORMAT_NV15;
    case V4L2_PIX_FMT_NV16:
        return DRM_FORMAT_NV16;
    case V4L2_PIX_FMT_NV20:
        return DRM_FORMAT_NV20;
    default:
        return DRM_FORMAT_NV12;
    }
}

uint32_t v4l2sl_capture_plane_size(uint32_t fourcc, uint32_t stride, uint32_t aligned_h)
{
    if (fourcc == V4L2_PIX_FMT_NV16 || fourcc == V4L2_PIX_FMT_NV20)
        return stride * aligned_h * 2;
    /* NV12 / NV15: Y + UV/2 */
    return stride * aligned_h * 3 / 2;
}

/*
 * Shared per-row 10-bit unpack scratch: heap, grows on demand, sized for
 * one Y row + one UV row. Every caller runs under g_v4l2sl_lock
 * (get_image / put_image / vpp), so one buffer serves all converters and
 * wide frames never hit a silent width clamp. Returns NULL on OOM.
 */
static uint16_t *g_row_scratch;
static uint32_t g_row_scratch_samples;

static uint16_t *row_scratch_get(int samples)
{
    if (samples <= 0)
        return NULL;
    if (samples > (int)g_row_scratch_samples) {
        uint16_t *p = realloc(g_row_scratch,
                              (size_t)samples * 2 * sizeof(uint16_t));

        if (!p)
            return NULL;
        g_row_scratch = p;
        g_row_scratch_samples = (uint32_t)samples;
    }
    return g_row_scratch;
}

static void unpack_le40_to_p010(const uint8_t *src, uint16_t *dst, int samples)
{
    int i = 0;

    while (i + 4 <= samples) {
        uint64_t v = (uint64_t)src[0]
                   | ((uint64_t)src[1] << 8)
                   | ((uint64_t)src[2] << 16)
                   | ((uint64_t)src[3] << 24)
                   | ((uint64_t)src[4] << 32);
        dst[0] = (uint16_t)((v & 0x3ffu) << 6);
        dst[1] = (uint16_t)(((v >> 10) & 0x3ffu) << 6);
        dst[2] = (uint16_t)(((v >> 20) & 0x3ffu) << 6);
        dst[3] = (uint16_t)(((v >> 30) & 0x3ffu) << 6);
        src += 5;
        dst += 4;
        i += 4;
    }
    if (i < samples) {
        uint64_t v = 0;
        int b, n = samples - i;
        for (b = 0; b < 5; b++)
            v |= (uint64_t)src[b] << (8 * b);
        for (b = 0; b < n; b++)
            dst[b] = (uint16_t)(((v >> (10 * b)) & 0x3ffu) << 6);
    }
}

void v4l2sl_nv15_to_p010(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h,
                         int width, int height)
{
    int y;
    int rows = height;
    const uint8_t *src_uv = src + (size_t)src_stride * src_aligned_h;
    uint8_t *dst_uv = dst + (size_t)dst_stride * height;

    if (rows > (int)src_aligned_h)
        rows = (int)src_aligned_h;

    for (y = 0; y < rows; y++)
        unpack_le40_to_p010(src + (size_t)y * src_stride,
                            (uint16_t *)(dst + (size_t)y * dst_stride),
                            width);

    rows = height / 2;
    if (rows > (int)src_aligned_h / 2)
        rows = (int)src_aligned_h / 2;
    for (y = 0; y < rows; y++)
        unpack_le40_to_p010(src_uv + (size_t)y * src_stride,
                            (uint16_t *)(dst_uv + (size_t)y * dst_stride),
                            width);
}

void v4l2sl_copy_nv12(uint8_t *dst, uint32_t dst_stride,
                      const uint8_t *src, uint32_t src_stride,
                      uint32_t src_aligned_h,
                      int width, int height)
{
    int y;
    int rows = height;
    const uint8_t *src_uv = src + (size_t)src_stride * src_aligned_h;
    uint8_t *dst_uv = dst + (size_t)dst_stride * height;

    if (rows > (int)src_aligned_h)
        rows = (int)src_aligned_h;
    for (y = 0; y < rows; y++)
        memcpy(dst + (size_t)y * dst_stride,
               src + (size_t)y * src_stride, (size_t)width);
    rows = height / 2;
    if (rows > (int)src_aligned_h / 2)
        rows = (int)src_aligned_h / 2;
    for (y = 0; y < rows; y++)
        memcpy(dst_uv + (size_t)y * dst_stride,
               src_uv + (size_t)y * src_stride, (size_t)width);
}

void v4l2sl_nv16_to_yuy2(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h,
                         int width, int height)
{
    int y, x;
    int rows = height;
    const uint8_t *src_uv = src + (size_t)src_stride * src_aligned_h;

    if (rows > (int)src_aligned_h)
        rows = (int)src_aligned_h;

    for (y = 0; y < rows; y++) {
        const uint8_t *ys = src + (size_t)y * src_stride;
        const uint8_t *uv = src_uv + (size_t)y * src_stride;
        uint8_t *d = dst + (size_t)y * dst_stride;
        for (x = 0; x + 1 < width; x += 2) {
            d[0] = ys[x];
            d[1] = uv[x];
            d[2] = ys[x + 1];
            d[3] = uv[x + 1];
            d += 4;
        }
        if (x < width) {
            d[0] = ys[x];
            d[1] = uv[x];
            d[2] = ys[x];
            d[3] = uv[x];
        }
    }
}

void v4l2sl_nv20_to_yuy2(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h,
                         int width, int height)
{
    /* Drop to 8-bit YUY2; VA has no common 10-bit 4:2:2 fourcc. */
    int y, x;
    int rows = height;
    const uint8_t *src_uv = src + (size_t)src_stride * src_aligned_h;
    uint16_t *y10, *uv10;

    if (rows > (int)src_aligned_h)
        rows = (int)src_aligned_h;
    y10 = row_scratch_get(width);
    if (!y10) {
        fprintf(stderr, "v4l2stateless: 10-bit row scratch OOM at width %d\n",
                width);
        return;
    }
    uv10 = y10 + width;

    for (y = 0; y < rows; y++) {
        uint8_t *d = dst + (size_t)y * dst_stride;
        unpack_le40_to_p010(src + (size_t)y * src_stride, y10, width);
        unpack_le40_to_p010(src_uv + (size_t)y * src_stride, uv10, width);
        for (x = 0; x + 1 < width; x += 2) {
            d[0] = (uint8_t)(y10[x] >> 8);
            d[1] = (uint8_t)(uv10[x] >> 8);
            d[2] = (uint8_t)(y10[x + 1] >> 8);
            d[3] = (uint8_t)(uv10[x + 1] >> 8);
            d += 4;
        }
    }
}

void v4l2sl_nv20_to_y210(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h, int width, int height)
{
    /* NV20 (packed 10-bit, Y plane + full-height UV plane) -> Y210
     * (Y0 U Y1 V in little-endian 16-bit containers, value << 6). */
    int y, x;
    int rows = height;
    const uint8_t *src_uv = src + (size_t)src_stride * src_aligned_h;
    uint16_t *y10, *uv10;

    if (rows > (int)src_aligned_h)
        rows = (int)src_aligned_h;
    y10 = row_scratch_get(width);
    if (!y10) {
        fprintf(stderr, "v4l2stateless: 10-bit row scratch OOM at width %d\n",
                width);
        return;
    }
    uv10 = y10 + width;

    for (y = 0; y < rows; y++) {
        uint16_t *d = (uint16_t *)(dst + (size_t)y * dst_stride);
        unpack_le40_to_p010(src + (size_t)y * src_stride, y10, width);
        unpack_le40_to_p010(src_uv + (size_t)y * src_stride, uv10, width);
        for (x = 0; x + 1 < width; x += 2) {
            d[0] = y10[x];
            d[1] = uv10[x];
            d[2] = y10[x + 1];
            d[3] = uv10[x + 1];
            d += 4;
        }
        if (x < width) {
            d[0] = y10[x];
            d[1] = uv10[x];
            d[2] = y10[x];
            d[3] = uv10[x];
        }
    }
}

uint32_t v4l2sl_va_image_size(uint32_t va_fourcc, uint32_t stride, uint32_t height)
{
    switch (va_fourcc) {
    case VA_FOURCC_P010:
        return stride * height * 3 / 2;
    case VA_FOURCC_Y210:
    case VA_FOURCC_YUY2:
        return stride * height;
    case VA_FOURCC_ARGB:
    case VA_FOURCC_BGRA:
    case VA_FOURCC_BGRX:
        return stride * height;
    default:
        return stride * height * 3 / 2;
    }
}

static int annexb_missing_prefix(const uint8_t *d, uint32_t sz)
{
    return !(sz >= 3 && d[0] == 0 && d[1] == 0 &&
             (d[2] == 1 || (sz >= 4 && d[2] == 0 && d[3] == 1)));
}

/* Concatenate slice NALs into a pre-mapped Annex-B bitstream. A NAL that
 * already carries a start code is copied verbatim; one without gets a
 * `prefix_len`-byte start code (3 or 4, per codec convention). Returns
 * the total byte count (0 when it does not fit dst_cap; a NULL dst only
 * sizes the result, matching the anon-buffer test mode). */
size_t v4l2sl_annexb_concat(const uint8_t * const *datas, const uint32_t *sizes,
                            int n, int prefix_len, uint8_t *dst, size_t dst_cap)
{
    size_t total = 0, off = 0;
    int i;

    for (i = 0; i < n; i++)
        total += annexb_missing_prefix(datas[i], sizes[i]) ?
                 (size_t)prefix_len + sizes[i] : sizes[i];
    if (total > dst_cap)
        return 0;
    if (!dst)
        return total;
    for (i = 0; i < n; i++) {
        if (annexb_missing_prefix(datas[i], sizes[i])) {
            if (prefix_len == 4)
                dst[off++] = 0;
            dst[off++] = 0;
            dst[off++] = 0;
            dst[off++] = 1;
        }
        memcpy(dst + off, datas[i], sizes[i]);
        off += sizes[i];
    }
    return total;
}

uint32_t v4l2sl_default_image_stride(uint32_t va_fourcc, int width)
{
    uint32_t stride;

    switch (va_fourcc) {
    case VA_FOURCC_P010:
    case VA_FOURCC_YUY2:
        stride = (uint32_t)width * 2;
        break;
    case VA_FOURCC_Y210:
    case VA_FOURCC_ARGB:
    case VA_FOURCC_BGRA:
    case VA_FOURCC_BGRX:
        stride = (uint32_t)width * 4;
        break;
    default:
        stride = (uint32_t)width;
        break;
    }
    /* 64-byte alignment keeps every row copy (put/get_image, VPP, gbm
     * upload) on NEON-friendly aligned rows. Allocation sizes and the
     * pitches reported to clients derive from this same value. */
    return (stride + 63) & ~63u;
}
