/*
 * v4l2stateless — locate decoder nodes by OUTPUT_MPLANE fourcc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <linux/media.h>

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
    case V4L2SL_CODEC_H264:     return "H.264";
    case V4L2SL_CODEC_HEVC:     return "HEVC";
    case V4L2SL_CODEC_AV1:      return "AV1";
    case V4L2SL_CODEC_VP8:      return "VP8";
    case V4L2SL_CODEC_MPEG2:    return "MPEG-2";
    case V4L2SL_CODEC_JPEG_ENC: return "JPEG-enc";
    case V4L2SL_CODEC_VPP:      return "VPP";
    default:                    return "unknown";
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
    case V4L2SL_CODEC_VP8:
        *fourcc_out = V4L2_PIX_FMT_VP8_FRAME;
        return 0;
    case V4L2SL_CODEC_MPEG2:
        *fourcc_out = V4L2_PIX_FMT_MPEG2_SLICE;
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
        if (nodes[i].request_api == V4L2SL_REQAPI_NO)
            continue;
        for (j = 0; j < nodes[i].n_fourccs; j++) {
            if (nodes[i].fourccs[j] == want)
                return nodes[i].path;
        }
    }
    return NULL;
}

int v4l2sl_find_media_path(const char *video_path, char *out, unsigned out_len)
{
    const char *base;
    char sysdir[128];
    DIR *d;
    struct dirent *de;
    int found = 0;

    if (!out || out_len == 0)
        return -1;
    out[0] = 0;
    if (!video_path)
        return -1;

    base = strrchr(video_path, '/');
    base = base ? base + 1 : video_path;
    snprintf(sysdir, sizeof(sysdir), "/sys/class/video4linux/%s/device", base);
    d = opendir(sysdir);
    if (!d)
        return -1;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "media", 5) != 0)
            continue;
        snprintf(out, out_len, "/dev/%s", de->d_name);
        found = 1;
        break;
    }
    closedir(d);
    return found ? 0 : -1;
}

int v4l2sl_video_has_request_api(const char *video_path)
{
    char media_path[128];
    int mfd, reqfd = -1;

    if (v4l2sl_find_media_path(video_path, media_path, sizeof(media_path)) < 0)
        return 0;
    mfd = open(media_path, O_RDWR);
    if (mfd < 0)
        return 0;
    if (ioctl(mfd, MEDIA_IOC_REQUEST_ALLOC, &reqfd) < 0) {
        close(mfd);
        return 0;
    }
    if (reqfd >= 0)
        close(reqfd);
    close(mfd);
    return 1;
}

static int fourcc_is_coded(uint32_t fcc)
{
    return fcc == V4L2_PIX_FMT_H264_SLICE ||
           fcc == V4L2_PIX_FMT_HEVC_SLICE ||
           fcc == V4L2_PIX_FMT_AV1_FRAME ||
           fcc == V4L2_PIX_FMT_VP8_FRAME ||
           fcc == V4L2_PIX_FMT_MPEG2_SLICE;
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
    return v4l2sl_scan_decoder_paths_ex(h264_out, hevc_out, av1_out,
                                        NULL, NULL, out_len);
}

int v4l2sl_scan_decoder_paths_ex(char *h264_out, char *hevc_out, char *av1_out,
                                 char *vp8_out, char *mpeg2_out,
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
    if (vp8_out && out_len)
        vp8_out[0] = 0;
    if (mpeg2_out && out_len)
        mpeg2_out[0] = 0;

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
        nodes[n_nodes].request_api = v4l2sl_video_has_request_api(paths[n_nodes])
            ? V4L2SL_REQAPI_YES : V4L2SL_REQAPI_NO;
        if (nodes[n_nodes].request_api == V4L2SL_REQAPI_NO) {
            unsigned k;
            int coded = 0;

            for (k = 0; k < nodes[n_nodes].n_fourccs; k++) {
                if (fourcc_is_coded(fourccs[n_nodes][k]))
                    coded = 1;
            }
            if (coded)
                fprintf(stderr,
                        "v4l2stateless: skip %s (coded fourcc, no media request)\n",
                        paths[n_nodes]);
        }
        n_nodes++;
    }

    p = v4l2sl_pick_device_for_codec(V4L2SL_CODEC_H264, nodes, n_nodes);
    copy_path(h264_out, out_len, p);
    p = v4l2sl_pick_device_for_codec(V4L2SL_CODEC_HEVC, nodes, n_nodes);
    copy_path(hevc_out, out_len, p);
    p = v4l2sl_pick_device_for_codec(V4L2SL_CODEC_AV1, nodes, n_nodes);
    copy_path(av1_out, out_len, p);
    p = v4l2sl_pick_device_for_codec(V4L2SL_CODEC_VP8, nodes, n_nodes);
    copy_path(vp8_out, out_len, p);
    p = v4l2sl_pick_device_for_codec(V4L2SL_CODEC_MPEG2, nodes, n_nodes);
    copy_path(mpeg2_out, out_len, p);

    return 0;
}

int v4l2sl_enum_capture_fourccs(int fd, uint32_t *out, unsigned max)
{
    struct v4l2_fmtdesc fmt;
    unsigned n = 0;

    if (fd < 0 || !out || max == 0)
        return -1;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    for (fmt.index = 0; n < max; fmt.index++) {
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0)
            break;
        out[n++] = fmt.pixelformat;
    }
    return (int)n;
}

static int fourcc_in(const uint32_t *list, unsigned n, uint32_t fcc)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (list[i] == fcc)
            return 1;
    return 0;
}

int v4l2sl_scan_aux_paths(char *jpeg_enc_out, char *vpp_out, unsigned out_len)
{
    int i;

    if (jpeg_enc_out && out_len)
        jpeg_enc_out[0] = 0;
    if (vpp_out && out_len)
        vpp_out[0] = 0;

    for (i = 0; i < 64; i++) {
        char path[64];
        uint32_t out_fcc[V4L2SL_PROBE_MAX_FOURCCS];
        uint32_t cap_fcc[V4L2SL_PROBE_MAX_FOURCCS];
        struct v4l2_capability cap;
        struct v4l2_queryctrl qc;
        int fd, nout, ncap;
        int has_jpeg, has_coded, has_rotate;

        snprintf(path, sizeof(path), "/dev/video%d", i);
        fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
            !(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
            close(fd);
            continue;
        }
        nout = v4l2sl_enum_output_fourccs(fd, out_fcc, V4L2SL_PROBE_MAX_FOURCCS);
        ncap = v4l2sl_enum_capture_fourccs(fd, cap_fcc, V4L2SL_PROBE_MAX_FOURCCS);
        memset(&qc, 0, sizeof(qc));
        qc.id = V4L2_CID_ROTATE;
        has_rotate = ioctl(fd, VIDIOC_QUERYCTRL, &qc) == 0;
        close(fd);
        if (nout <= 0 || ncap <= 0)
            continue;

        has_jpeg = fourcc_in(cap_fcc, (unsigned)ncap, V4L2_PIX_FMT_JPEG);
        has_coded = fourcc_in(out_fcc, (unsigned)nout, V4L2_PIX_FMT_H264_SLICE) ||
                    fourcc_in(out_fcc, (unsigned)nout, V4L2_PIX_FMT_HEVC_SLICE) ||
                    fourcc_in(out_fcc, (unsigned)nout, V4L2_PIX_FMT_AV1_FRAME) ||
                    fourcc_in(out_fcc, (unsigned)nout, V4L2_PIX_FMT_VP8_FRAME) ||
                    fourcc_in(out_fcc, (unsigned)nout, V4L2_PIX_FMT_MPEG2_SLICE);

        if (has_jpeg && jpeg_enc_out && out_len && !jpeg_enc_out[0])
            copy_path(jpeg_enc_out, out_len, path);
        if (has_rotate && !has_coded && !has_jpeg && vpp_out && out_len && !vpp_out[0])
            copy_path(vpp_out, out_len, path);
    }
    return 0;
}
