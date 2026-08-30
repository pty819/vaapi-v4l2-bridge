/*
 * Drive shipped pixel-format helpers and VPP capability queries.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <va/va.h>
#include <va/va_vpp.h>
#include <linux/videodev2.h>
#include <drm_fourcc.h>

#include "v4l2stateless.h"

static int g_fail;

static void expect_true(int cond, const char *tag)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", tag);
        g_fail++;
    } else {
        printf("OK %s\n", tag);
    }
}

static void test_fourcc_map(void)
{
    expect_true(v4l2sl_capture_fourcc_from_rt(VA_RT_FORMAT_YUV420) == V4L2_PIX_FMT_NV12,
                "rt-420-nv12");
    expect_true(v4l2sl_capture_fourcc_from_rt(VA_RT_FORMAT_YUV420_10) == V4L2_PIX_FMT_NV15,
                "rt-42010-nv15");
    expect_true(v4l2sl_capture_fourcc_from_rt(VA_RT_FORMAT_YUV422) == V4L2_PIX_FMT_NV16,
                "rt-422-nv16");
    expect_true(v4l2sl_capture_fourcc_from_rt(VA_RT_FORMAT_YUV422_10) == V4L2_PIX_FMT_NV20,
                "rt-42210-nv20");
    expect_true(v4l2sl_capture_fourcc_from_sps(0, 1) == V4L2_PIX_FMT_NV12, "sps-8-420");
    expect_true(v4l2sl_capture_fourcc_from_sps(2, 1) == V4L2_PIX_FMT_NV15, "sps-10-420");
    expect_true(v4l2sl_capture_fourcc_from_sps(0, 2) == V4L2_PIX_FMT_NV16, "sps-8-422");
    expect_true(v4l2sl_capture_fourcc_from_sps(2, 2) == V4L2_PIX_FMT_NV20, "sps-10-422");
    expect_true(v4l2sl_capture_is_10bit(V4L2_PIX_FMT_NV15), "nv15-10bit");
    expect_true(!v4l2sl_capture_is_10bit(V4L2_PIX_FMT_NV12), "nv12-8bit");
    expect_true(v4l2sl_capture_is_422(V4L2_PIX_FMT_NV16), "nv16-422");
    expect_true(!v4l2sl_capture_is_422(V4L2_PIX_FMT_NV12), "nv12-not-422");
    expect_true(v4l2sl_va_fourcc_for_capture(V4L2_PIX_FMT_NV15) == VA_FOURCC_P010,
                "va-p010");
    expect_true(v4l2sl_va_fourcc_for_capture(V4L2_PIX_FMT_NV12) == VA_FOURCC_NV12,
                "va-nv12");
    expect_true(v4l2sl_drm_fourcc_for_capture(V4L2_PIX_FMT_NV12) == DRM_FORMAT_NV12,
                "drm-nv12");
    expect_true(v4l2sl_capture_plane_size(V4L2_PIX_FMT_NV12, 16, 16) == 16 * 16 * 3 / 2,
                "plane-nv12");
    expect_true(v4l2sl_capture_plane_size(V4L2_PIX_FMT_NV16, 16, 16) == 16 * 16 * 2,
                "plane-nv16");
    expect_true(v4l2sl_va_image_size(VA_FOURCC_NV12, 320, 240) == 320 * 240 * 3 / 2,
                "image-nv12");
    expect_true(v4l2sl_va_image_size(VA_FOURCC_YUY2, 640, 240) == 640 * 240,
                "image-yuy2");
    expect_true(v4l2sl_default_image_stride(VA_FOURCC_P010, 128) == 256,
                "stride-p010");
    expect_true(v4l2sl_default_image_stride(VA_FOURCC_NV12, 128) == 128,
                "stride-nv12");
}

static void test_copy_nv12(void)
{
    uint8_t src[12];
    uint8_t dst[24];
    int i;

    memset(src, 0, sizeof(src));
    memset(dst, 0xaa, sizeof(dst));
    /* 2x2 NV12, stride 2, aligned_h 2: Y=4, UV=2 */
    src[0] = 10; src[1] = 11;
    src[2] = 12; src[3] = 13;
    src[4] = 20; src[5] = 21;
    v4l2sl_copy_nv12(dst, 4, src, 2, 2, 2, 2);
    expect_true(dst[0] == 10 && dst[1] == 11, "copy-y0");
    expect_true(dst[4] == 12 && dst[5] == 13, "copy-y1");
    expect_true(dst[8] == 20 && dst[9] == 21, "copy-uv");
    for (i = 2; i < 4; i++)
        expect_true(dst[i] == 0xaa, "copy-y0-pad");
}

static void test_nv15_to_p010(void)
{
    uint8_t src[15];
    uint16_t dst[4 * 2 + 4]; /* 2 Y rows + 1 UV row at stride 8 bytes = 4 u16 */
    uint8_t *d8 = (uint8_t *)dst;

    memset(src, 0xff, sizeof(src));
    memset(dst, 0, sizeof(dst));
    /* width=4, height=2, NV15 stride=5, aligned_h=2 */
    v4l2sl_nv15_to_p010(d8, 8, src, 5, 2, 4, 2);
    expect_true(dst[0] == (uint16_t)(0x3ffu << 6), "p010-y0");
}

static void test_nv16_to_yuy2(void)
{
    uint8_t src[8];
    uint8_t dst[8];

    memset(src, 0, sizeof(src));
    src[0] = 1; src[1] = 2; /* Y */
    src[2] = 3; src[3] = 4;
    src[4] = 10; src[5] = 11; /* UV row 0 */
    src[6] = 12; src[7] = 13;
    v4l2sl_nv16_to_yuy2(dst, 4, src, 2, 2, 2, 2);
    expect_true(dst[0] == 1 && dst[1] == 10 && dst[2] == 2 && dst[3] == 11,
                "nv16-yuy2-row0");
}

static void test_vpp_query(void)
{
    unsigned n = 7;
    VAProcFilterType filters[4];
    VAProcPipelineCaps caps;

    expect_true(v4l2sl_vpp_query_filters(NULL, &n) == VA_STATUS_SUCCESS && n == 0,
                "vpp-filters-count");
    n = 4;
    expect_true(v4l2sl_vpp_query_filters(filters, &n) == VA_STATUS_SUCCESS && n == 0,
                "vpp-filters-empty");
    expect_true(v4l2sl_vpp_query_filter_caps(VAProcFilterNoiseReduction, NULL, &n)
                == VA_STATUS_ERROR_UNSUPPORTED_FILTER,
                "vpp-denoise-unsupported");
    expect_true(v4l2sl_vpp_query_pipeline_caps(NULL) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "vpp-caps-null");
    expect_true(v4l2sl_vpp_query_pipeline_caps(&caps) == VA_STATUS_SUCCESS, "vpp-caps");
    expect_true(caps.max_input_width == 8192 && caps.max_output_width == 8192,
                "vpp-max-size");
    expect_true(caps.rotation_flags & (1u << VA_ROTATION_90), "vpp-rot90");
    expect_true(caps.mirror_flags & VA_MIRROR_HORIZONTAL, "vpp-mirror");
}

int main(void)
{
    test_fourcc_map();
    test_copy_nv12();
    test_nv15_to_p010();
    test_nv16_to_yuy2();
    test_vpp_query();
    return g_fail ? 1 : 0;
}
