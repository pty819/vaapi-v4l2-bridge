/*
 * Unit + live tests for v4l2sl_pick_device_for_codec.
 * The picker is the shipped function in src/v4l2stateless_probe.c.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "v4l2stateless_probe.h"

static int g_fail;

static void expect_eq(const char *got, const char *want, const char *tag)
{
    if (!want) {
        if (got) {
            fprintf(stderr, "FAIL %s: got %s, want NULL\n", tag, got);
            g_fail++;
        } else {
            printf("OK %s NULL\n", tag);
        }
        return;
    }
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got %s, want %s\n", tag,
                got ? got : "NULL", want);
        g_fail++;
        return;
    }
    printf("OK %s %s\n", tag, got);
}

static int node_has_fourcc(const char *path, uint32_t want)
{
    uint32_t fcs[V4L2SL_PROBE_MAX_FOURCCS];
    int fd, n, i;

    fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return 0;
    n = v4l2sl_enum_output_fourccs(fd, fcs, V4L2SL_PROBE_MAX_FOURCCS);
    close(fd);
    if (n < 0)
        return 0;
    for (i = 0; i < n; i++) {
        if (fcs[i] == want)
            return 1;
    }
    return 0;
}

static int run_unit(void)
{
    static const uint32_t rkvdec[] = {
        V4L2_PIX_FMT_HEVC_SLICE, V4L2_PIX_FMT_H264_SLICE
    };
    static const uint32_t av1[] = { V4L2_PIX_FMT_AV1_FRAME };
    static const uint32_t rga[] = { V4L2_PIX_FMT_NV12 };
    static const uint32_t mpegvp8[] = {
        v4l2_fourcc('M', 'G', '2', 'S'), v4l2_fourcc('V', 'P', '8', 'F')
    };
    struct v4l2sl_node_fmts current[] = {
        { "/dev/video0", rga, 1 },
        { "/dev/video1", rkvdec, 2 },
        { "/dev/video2", mpegvp8, 2 },
        { "/dev/video3", rga, 1 },
        { "/dev/video4", av1, 1 },
    };
    struct v4l2sl_node_fmts swapped[] = {
        { "/dev/video1", av1, 1 },
        { "/dev/video4", rkvdec, 2 },
    };
    struct v4l2sl_node_fmts none[] = {
        { "/dev/video0", rga, 1 },
    };
    uint32_t fcc;
    char tag[8];

    printf("== Orange Pi 5 current map ==\n");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_H264, current, 5),
              "/dev/video1", "h264-current");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_HEVC, current, 5),
              "/dev/video1", "hevc-current");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_AV1, current, 5),
              "/dev/video4", "av1-current");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_VP8, current, 5),
              "/dev/video2", "vp8-current");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_MPEG2, current, 5),
              "/dev/video2", "mpeg2-current");

    printf("== swapped /dev/video numbers ==\n");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_H264, swapped, 2),
              "/dev/video4", "h264-swapped");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_HEVC, swapped, 2),
              "/dev/video4", "hevc-swapped");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_AV1, swapped, 2),
              "/dev/video1", "av1-swapped");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_VP8, swapped, 2),
              NULL, "vp8-swapped-missing");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_MPEG2, swapped, 2),
              NULL, "mpeg2-swapped-missing");

    printf("== missing fourcc ==\n");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_H264, none, 1),
              NULL, "h264-missing");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_AV1, none, 1),
              NULL, "av1-missing");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_VP8, none, 1),
              NULL, "vp8-missing");
    expect_eq(v4l2sl_pick_device_for_codec(V4L2SL_CODEC_MPEG2, none, 1),
              NULL, "mpeg2-missing");

    if (v4l2sl_codec_coded_fourcc(V4L2SL_CODEC_H264, &fcc) != 0 ||
        fcc != V4L2_PIX_FMT_H264_SLICE) {
        fprintf(stderr, "FAIL coded fourcc H.264\n");
        g_fail++;
    } else {
        v4l2sl_fourcc_to_str(fcc, tag);
        printf("OK coded-fourcc H.264 %s\n", tag);
    }
    if (v4l2sl_codec_coded_fourcc(V4L2SL_CODEC_VP8, &fcc) != 0 ||
        fcc != V4L2_PIX_FMT_VP8_FRAME) {
        fprintf(stderr, "FAIL coded fourcc VP8\n");
        g_fail++;
    } else {
        v4l2sl_fourcc_to_str(fcc, tag);
        printf("OK coded-fourcc VP8 %s\n", tag);
    }
    if (v4l2sl_codec_coded_fourcc(V4L2SL_CODEC_MPEG2, &fcc) != 0 ||
        fcc != V4L2_PIX_FMT_MPEG2_SLICE) {
        fprintf(stderr, "FAIL coded fourcc MPEG-2\n");
        g_fail++;
    } else {
        v4l2sl_fourcc_to_str(fcc, tag);
        printf("OK coded-fourcc MPEG-2 %s\n", tag);
    }
    return g_fail ? 1 : 0;
}

static int run_live(void)
{
    char h264[64], hevc[64], av1[64], vp8[64], mpeg2[64];
    uint32_t fcc;

    v4l2sl_scan_decoder_paths_ex(h264, hevc, av1, vp8, mpeg2, 64);
    printf("live H.264  -> %s\n", h264[0] ? h264 : "(none)");
    printf("live HEVC   -> %s\n", hevc[0] ? hevc : "(none)");
    printf("live AV1    -> %s\n", av1[0] ? av1 : "(none)");
    printf("live VP8    -> %s\n", vp8[0] ? vp8 : "(none)");
    printf("live MPEG-2 -> %s\n", mpeg2[0] ? mpeg2 : "(none)");

    if (v4l2sl_codec_coded_fourcc(V4L2SL_CODEC_H264, &fcc) == 0) {
        if (!h264[0] || !node_has_fourcc(h264, fcc)) {
            fprintf(stderr, "FAIL live H.264 node %s lacks S264\n",
                    h264[0] ? h264 : "(none)");
            g_fail++;
        } else {
            printf("OK live H.264 %s has S264\n", h264);
        }
    }
    if (v4l2sl_codec_coded_fourcc(V4L2SL_CODEC_HEVC, &fcc) == 0) {
        if (!hevc[0] || !node_has_fourcc(hevc, fcc)) {
            fprintf(stderr, "FAIL live HEVC node %s lacks S265\n",
                    hevc[0] ? hevc : "(none)");
            g_fail++;
        } else {
            printf("OK live HEVC %s has S265\n", hevc);
        }
    }
    if (v4l2sl_codec_coded_fourcc(V4L2SL_CODEC_AV1, &fcc) == 0) {
        if (!av1[0] || !node_has_fourcc(av1, fcc)) {
            fprintf(stderr, "FAIL live AV1 node %s lacks AV1F\n",
                    av1[0] ? av1 : "(none)");
            g_fail++;
        } else {
            printf("OK live AV1 %s has AV1F\n", av1);
        }
    }
    if (v4l2sl_codec_coded_fourcc(V4L2SL_CODEC_VP8, &fcc) == 0) {
        if (!vp8[0] || !node_has_fourcc(vp8, fcc)) {
            fprintf(stderr, "FAIL live VP8 node %s lacks VP8F\n",
                    vp8[0] ? vp8 : "(none)");
            g_fail++;
        } else {
            printf("OK live VP8 %s has VP8F\n", vp8);
        }
    }
    if (v4l2sl_codec_coded_fourcc(V4L2SL_CODEC_MPEG2, &fcc) == 0) {
        if (!mpeg2[0] || !node_has_fourcc(mpeg2, fcc)) {
            fprintf(stderr, "FAIL live MPEG-2 node %s lacks MG2S\n",
                    mpeg2[0] ? mpeg2 : "(none)");
            g_fail++;
        } else {
            printf("OK live MPEG-2 %s has MG2S\n", mpeg2);
        }
    }
    if (vp8[0] && mpeg2[0] && strcmp(vp8, mpeg2) != 0) {
        fprintf(stderr, "FAIL VP8 and MPEG-2 resolved to different nodes %s vs %s\n",
                vp8, mpeg2);
        g_fail++;
    }
    /* Must not have bound H.264 to the AV1 node. */
    if (h264[0] && av1[0] && strcmp(h264, av1) == 0) {
        fprintf(stderr, "FAIL H.264 and AV1 resolved to the same node %s\n", h264);
        g_fail++;
    }
    return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--live") == 0)
        return run_live();
    return run_unit();
}
