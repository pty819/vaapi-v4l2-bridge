/*
 * v4l2stateless — locate decoder nodes by OUTPUT_MPLANE fourcc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include "v4l2stateless_probe.h"

void v4l2sl_fourcc_to_str(uint32_t fourcc, char out[5])
{
    out[0] = (char)(fourcc & 0xff);
    out[1] = (char)((fourcc >> 8) & 0xff);
    out[2] = (char)((fourcc >> 16) & 0xff);
    out[3] = (char)((fourcc >> 24) & 0xff);
    out[4] = 0;
}

const char *v4l2sl_codec_name(enum v4l2sl_codec codec)
{
    switch (codec) {
    case V4L2SL_CODEC_H264:  return "H.264";
    case V4L2SL_CODEC_HEVC:  return "HEVC";
    case V4L2SL_CODEC_AV1:   return "AV1";
    case V4L2SL_CODEC_VP8:   return "VP8";
    case V4L2SL_CODEC_MPEG2: return "MPEG-2";
    default:                 return "unknown";
    }
}

int v4l2sl_codec_coded_fourcc(enum v4l2sl_codec codec, uint32_t *fourcc_out)
{
    if (!fourcc_out)
        return -1;
    switch (codec) {
    case V4L2SL_CODEC_H264:
        *fourcc_out = V4L2_PIX_FMT_H264_SLICE;
        return 0;
    case V4L2SL_CODEC_HEVC:
        *fourcc_out = V4L2_PIX_FMT_HEVC_SLICE;
        return 0;
    case V4L2SL_CODEC_AV1:
        *fourcc_out = V4L2_PIX_FMT_AV1_FRAME;
        return 0;
    default:
        return -1;
    }
}

const char *v4l2sl_pick_device_for_codec(enum v4l2sl_codec codec,
                                         const struct v4l2sl_node_fmts *nodes,
                                         unsigned n_nodes)
{
    uint32_t want;
    unsigned i, j;

    if (!nodes || v4l2sl_codec_coded_fourcc(codec, &want) < 0)
        return NULL;

    for (i = 0; i < n_nodes; i++) {
        if (!nodes[i].path || !nodes[i].fourccs)
            continue;
        for (j = 0; j < nodes[i].n_fourccs; j++) {
            if (nodes[i].fourccs[j] == want)
                return nodes[i].path;
        }
    }
    return NULL;
}

int v4l2sl_enum_output_fourccs(int fd, uint32_t *out, unsigned max)
{
    struct v4l2_fmtdesc fmt;
    unsigned n = 0;

    if (fd < 0 || !out || max == 0)
        return -1;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    for (fmt.index = 0; n < max; fmt.index++) {
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0)
            break;
        out[n++] = fmt.pixelformat;
    }
    return (int)n;
}

static void copy_path(char *dst, unsigned dst_len, const char *src)
{
    if (!dst || dst_len == 0)
        return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = 0;
}

int v4l2sl_scan_decoder_paths(char *h264_out, char *hevc_out, char *av1_out,
                              unsigned out_len)
{
    struct v4l2sl_node_fmts nodes[V4L2SL_PROBE_MAX_NODES];
    uint32_t fourccs[V4L2SL_PROBE_MAX_NODES][V4L2SL_PROBE_MAX_FOURCCS];
    char paths[V4L2SL_PROBE_MAX_NODES][64];
    unsigned n_nodes = 0;
    int i;
    const char *p;

    if (h264_out && out_len)
        h264_out[0] = 0;
    if (hevc_out && out_len)
        hevc_out[0] = 0;
    if (av1_out && out_len)
        av1_out[0] = 0;

    for (i = 0; i < 64 && n_nodes < V4L2SL_PROBE_MAX_NODES; i++) {
        struct v4l2_capability cap;
        int fd, nfmt;

        snprintf(paths[n_nodes], sizeof(paths[n_nodes]), "/dev/video%d", i);
        fd = open(paths[n_nodes], O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;

        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
            !(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
            close(fd);
            continue;
        }

        nfmt = v4l2sl_enum_output_fourccs(fd, fourccs[n_nodes],
                                          V4L2SL_PROBE_MAX_FOURCCS);
        close(fd);
        if (nfmt <= 0)
            continue;

        nodes[n_nodes].path = paths[n_nodes];
        nodes[n_nodes].fourccs = fourccs[n_nodes];
        nodes[n_nodes].n_fourccs = (unsigned)nfmt;
        n_nodes++;
    }

    p = v4l2sl_pick_device_for_codec(V4L2SL_CODEC_H264, nodes, n_nodes);
    copy_path(h264_out, out_len, p);
    p = v4l2sl_pick_device_for_codec(V4L2SL_CODEC_HEVC, nodes, n_nodes);
    copy_path(hevc_out, out_len, p);
    p = v4l2sl_pick_device_for_codec(V4L2SL_CODEC_AV1, nodes, n_nodes);
    copy_path(av1_out, out_len, p);

    return 0;
}
