/*
 * Drive shipped surface-export and capture-reconfigure paths.
 * Export must succeed without DQBUF; small→large recapture must STREAMOFF,
 * drop exported fds, REQBUFS(0), then allocate a larger capture set.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <va/va.h>
#include <va/va_dec_av1.h>
#include <va/va_dec_hevc.h>
#include <va/va_dec_vp8.h>
#include <va/va_drmcommon.h>

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

static void test_export_before_decode(void)
{
    struct v4l2sl_surface s;
    VADRMPRIMESurfaceDescriptor desc;
    VAStatus st;

    memset(&s, 0, sizeof(s));
    memset(&desc, 0, sizeof(desc));
    s.width = 320;
    s.height = 240;
    s.format = VA_FOURCC_NV12;
    s.buf_index = -1;
    s.dma_buf_fd = -1;
    s.cpu_stride = 320;

    expect_true(v4l2sl_surface_alloc_export_fd(&s) == 0, "alloc-export-fd");
    expect_true(s.dma_buf_fd >= 0, "pre-decode-dmabuf");

    st = v4l2sl_surface_fill_prime(&s, NULL, 0, &desc);
    expect_true(st == VA_STATUS_SUCCESS, "export-no-dqbuf-status");
    expect_true(desc.objects[0].fd >= 0, "export-no-dqbuf-fd");
    expect_true(desc.width == 320 && desc.height == 240, "export-size");
    expect_true(desc.num_objects == 1, "export-objects");

    if (desc.objects[0].fd >= 0)
        close(desc.objects[0].fd);
    if (s.dma_buf_fd >= 0)
        close(s.dma_buf_fd);
}

static char g_ops[32][24];
static int g_nops;
static uint32_t g_last_cap_w, g_last_cap_h;
static int g_streamoff, g_reqbufs0, g_reqbufs_n, g_sfmt_cap;
static int g_fds_live_at_reqbufs0;
static struct v4l2sl_context *g_ctx;

static void log_op(const char *op)
{
    if (g_nops < 32) {
        snprintf(g_ops[g_nops], sizeof(g_ops[0]), "%s", op);
        g_nops++;
    }
}

static int op_index(const char *op)
{
    int i;
    for (i = 0; i < g_nops; i++) {
        if (strcmp(g_ops[i], op) == 0)
            return i;
    }
    return -1;
}

static int mock_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;

    if (request == VIDIOC_STREAMOFF) {
        g_streamoff++;
        log_op("STREAMOFF");
        return 0;
    }
    if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *req = arg;

        if (req->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && req->count == 0) {
            int i;

            g_reqbufs0++;
            g_fds_live_at_reqbufs0 = 0;
            if (g_ctx && g_ctx->driver_data) {
                for (i = 0; i < g_ctx->num_render_targets; i++) {
                    VASurfaceID id = g_ctx->render_targets[i];
                    struct v4l2sl_surface *s;

                    if (id == VA_INVALID_ID || (unsigned)id >= 4096)
                        continue;
                    s = g_ctx->driver_data->surfaces[id];
                    if (s && s->dma_buf_fd >= 0)
                        g_fds_live_at_reqbufs0++;
                }
            }
            log_op("REQBUFS0");
            return 0;
        }
        if (req->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            g_reqbufs_n++;
            if (req->count > 4)
                req->count = 4;
            log_op("REQBUFSN");
            return 0;
        }
        return 0;
    }
    if (request == VIDIOC_S_FMT) {
        struct v4l2_format *fmt = arg;

        if (fmt->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            g_sfmt_cap++;
            g_last_cap_w = fmt->fmt.pix_mp.width;
            g_last_cap_h = fmt->fmt.pix_mp.height;
            log_op("S_FMT_CAP");
        }
        return 0;
    }
    if (request == VIDIOC_G_FMT) {
        struct v4l2_format *fmt = arg;

        if (fmt->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            fmt->fmt.pix_mp.width = g_last_cap_w ? g_last_cap_w : 320;
            fmt->fmt.pix_mp.height = g_last_cap_h ? g_last_cap_h : 240;
            fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
            fmt->fmt.pix_mp.num_planes = 1;
            fmt->fmt.pix_mp.plane_fmt[0].bytesperline = fmt->fmt.pix_mp.width;
            fmt->fmt.pix_mp.plane_fmt[0].sizeimage =
                fmt->fmt.pix_mp.width * fmt->fmt.pix_mp.height * 3 / 2;
        } else {
            fmt->fmt.pix_mp.width = 320;
            fmt->fmt.pix_mp.height = 240;
            fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264_SLICE;
            fmt->fmt.pix_mp.num_planes = 1;
        }
        return 0;
    }
    if (request == VIDIOC_EXPBUF) {
        struct v4l2_exportbuffer *exp = arg;
        int mfd = memfd_create("v4l2sl-exp", MFD_CLOEXEC);

        if (mfd < 0)
            return -1;
        exp->fd = mfd;
        log_op("EXPBUF");
        return 0;
    }
    if (request == VIDIOC_STREAMON) {
        log_op("STREAMON");
        return 0;
    }
    if (request == VIDIOC_S_EXT_CTRLS) {
        log_op("S_EXT_CTRLS");
        return 0;
    }
    if (request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *buf = arg;

        if (buf->m.planes)
            buf->m.planes[0].length = 65536;
        return 0;
    }
    if (request == VIDIOC_QBUF)
        return 0;
    if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *buf = arg;

        buf->index = 0;
        return 0;
    }
#ifdef MEDIA_REQUEST_IOC_QUEUE
    if (request == MEDIA_REQUEST_IOC_QUEUE)
        return 0;
#endif
    return 0;
}

static void test_recapture_small_to_large(void)
{
    struct v4l2sl_driver_data dd;
    struct v4l2sl_context ctx;
    struct v4l2sl_surface *s1, *s2;
    VASurfaceID targets[2] = { 1, 2 };
    int i0, i1, i2, i3, nexp;

    memset(&dd, 0, sizeof(dd));
    memset(&ctx, 0, sizeof(ctx));
    s1 = calloc(1, sizeof(*s1));
    s2 = calloc(1, sizeof(*s2));
    if (!s1 || !s2) {
        fprintf(stderr, "FAIL calloc surfaces\n");
        g_fail++;
        free(s1);
        free(s2);
        return;
    }

    s1->width = 320;
    s1->height = 240;
    s1->format = VA_FOURCC_NV12;
    s1->buf_index = -1;
    s1->dma_buf_fd = -1;
    s1->cpu_stride = 320;
    s2->width = 320;
    s2->height = 240;
    s2->format = VA_FOURCC_NV12;
    s2->buf_index = -1;
    s2->dma_buf_fd = -1;
    s2->cpu_stride = 320;
    expect_true(v4l2sl_surface_alloc_export_fd(s1) == 0, "recap-s1-memfd");
    expect_true(v4l2sl_surface_alloc_export_fd(s2) == 0, "recap-s2-memfd");

    dd.surfaces[1] = s1;
    dd.surfaces[2] = s2;

    ctx.driver_data = &dd;
    ctx.v4l2_fd = 7;
    ctx.media_fd = -1;
    ctx.request_fd = -1;
    ctx.width = 320;
    ctx.height = 240;
    ctx.cap_width = 320;
    ctx.cap_height = 240;
    ctx.cap_stride = 320;
    ctx.cap_sizeimage = 320 * 240 * 3 / 2;
    ctx.cap_pixelformat = V4L2_PIX_FMT_NV12;
    ctx.capture_bufs_allocd = 24;
    ctx.output_bufs_allocd = 2;
    ctx.streamed = 1;
    ctx.n_free_cap = 24;
    ctx.render_targets = targets;
    ctx.num_render_targets = 2;

    g_ctx = &ctx;
    g_nops = 0;
    g_streamoff = g_reqbufs0 = g_reqbufs_n = g_sfmt_cap = 0;
    g_fds_live_at_reqbufs0 = -1;
    g_last_cap_w = 320;
    g_last_cap_h = 240;
    v4l2sl_set_ioctl_hook(mock_ioctl);

    expect_true(v4l2sl_ensure_capture(&ctx, 1920, 1080, V4L2_PIX_FMT_NV12) == 0,
                "ensure-capture-4k");
    expect_true(g_streamoff >= 2, "streamoff-both-queues");
    expect_true(g_reqbufs0 == 1, "reqbufs0-once");
    expect_true(g_fds_live_at_reqbufs0 == 0, "fds-dropped-before-reqbufs0");
    expect_true(g_sfmt_cap >= 1, "sfmt-capture");
    expect_true(g_reqbufs_n >= 1, "reqbufs-new");
    expect_true(ctx.cap_width >= 1920 && ctx.cap_height >= 1080,
                "capture-holds-larger");
    expect_true(ctx.streamed == 0, "streamed-cleared");

    i0 = op_index("STREAMOFF");
    i1 = op_index("REQBUFS0");
    i2 = op_index("S_FMT_CAP");
    i3 = op_index("REQBUFSN");
    expect_true(i0 >= 0 && i1 > i0, "streamoff-before-reqbufs0");
    expect_true(i1 >= 0 && i2 > i1, "reqbufs0-before-sfmt");
    expect_true(i2 >= 0 && i3 > i2, "sfmt-before-reqbufsn");

    nexp = 0;
    for (i0 = 0; i0 < g_nops; i0++) {
        if (strcmp(g_ops[i0], "EXPBUF") == 0)
            nexp++;
    }
    expect_true(nexp >= 2, "expbuf-after-recapture");
    expect_true(s1->dma_buf_fd >= 0, "s1-rebound");
    expect_true(s2->dma_buf_fd >= 0, "s2-rebound");
    expect_true(op_index("EXPBUF") > i3, "expbuf-after-new-reqbufs");

    if (s1->dma_buf_fd >= 0)
        close(s1->dma_buf_fd);
    if (s2->dma_buf_fd >= 0)
        close(s2->dma_buf_fd);
    free(s1);
    free(s2);
    v4l2sl_set_ioctl_hook(NULL);
    g_ctx = NULL;
}

static void test_av1_translate_recapture(void)
{
    struct v4l2sl_driver_data dd;
    struct v4l2sl_context ctx;
    struct v4l2sl_surface *surf;
    VASurfaceID targets[1] = { 1 };
    VADecPictureParameterBufferAV1 pic;
    uint8_t tile[64];
    uint8_t outbuf[4096];
    struct v4l2sl_buffer pic_buf, data_buf;
    struct v4l2sl_buffer *bufs[2];
    int sp[2];
    int i0, i1, i2, i3, i4;
    VAStatus st;

    memset(&dd, 0, sizeof(dd));
    memset(&ctx, 0, sizeof(ctx));
    memset(&pic, 0, sizeof(pic));
    memset(tile, 0xab, sizeof(tile));
    surf = calloc(1, sizeof(*surf));
    if (!surf) {
        fprintf(stderr, "FAIL calloc av1 surface\n");
        g_fail++;
        return;
    }
    if (pipe(sp) < 0) {
        fprintf(stderr, "FAIL pipe\n");
        g_fail++;
        free(surf);
        return;
    }
    if (write(sp[1], "x", 1) != 1) {
        fprintf(stderr, "FAIL pipe write\n");
        g_fail++;
        close(sp[0]);
        close(sp[1]);
        free(surf);
        return;
    }

    surf->width = 432;
    surf->height = 240;
    surf->format = VA_FOURCC_NV12;
    surf->buf_index = -1;
    surf->dma_buf_fd = -1;
    surf->cpu_stride = 432;
    expect_true(v4l2sl_surface_alloc_export_fd(surf) == 0, "av1-surf-memfd");
    dd.surfaces[1] = surf;

    pic.frame_width_minus1 = 1919;
    pic.frame_height_minus1 = 1079;
    pic.bit_depth_idx = 0;
    pic.tile_cols = 1;
    pic.tile_rows = 1;
    pic.seq_info_fields.fields.subsampling_x = 1;
    pic.seq_info_fields.fields.subsampling_y = 1;
    pic.pic_info_fields.bits.frame_type = 0; /* key */
    pic.pic_info_fields.bits.show_frame = 1;
    pic.pic_info_fields.bits.uniform_tile_spacing_flag = 1;

    memset(&pic_buf, 0, sizeof(pic_buf));
    memset(&data_buf, 0, sizeof(data_buf));
    pic_buf.type = VAPictureParameterBufferType;
    pic_buf.data = &pic;
    pic_buf.size = sizeof(pic);
    data_buf.type = VASliceDataBufferType;
    data_buf.data = tile;
    data_buf.size = sizeof(tile);
    bufs[0] = &pic_buf;
    bufs[1] = &data_buf;

    ctx.driver_data = &dd;
    ctx.v4l2_fd = sp[0];
    ctx.media_fd = -1;
    ctx.request_fd = sp[1];
    ctx.width = 432;
    ctx.height = 240;
    ctx.cap_width = 432;
    ctx.cap_height = 240;
    ctx.cap_stride = 432;
    ctx.cap_sizeimage = 432 * 240 * 3 / 2;
    ctx.cap_pixelformat = V4L2_PIX_FMT_NV12;
    ctx.capture_bufs_allocd = 24;
    ctx.output_bufs_allocd = 2;
    ctx.streamed = 1;
    ctx.n_free_out = 1;
    ctx.free_out_bufs[0] = 0;
    ctx.output_buf_ptr[0] = outbuf;
    ctx.output_buf_size = sizeof(outbuf);
    ctx.render_targets = targets;
    ctx.num_render_targets = 1;
    ctx.current_surface = surf;
    ctx.codec = V4L2SL_CODEC_AV1;

    g_ctx = &ctx;
    g_nops = 0;
    g_streamoff = g_reqbufs0 = g_reqbufs_n = g_sfmt_cap = 0;
    g_fds_live_at_reqbufs0 = -1;
    g_last_cap_w = 432;
    g_last_cap_h = 240;
    v4l2sl_set_ioctl_hook(mock_ioctl);

    st = v4l2sl_av1_translate(&ctx, bufs, 2);
    expect_true(st == VA_STATUS_SUCCESS, "av1-translate-status");
    expect_true(g_streamoff >= 2, "av1-streamoff");
    expect_true(g_reqbufs0 == 1, "av1-reqbufs0");
    expect_true(g_fds_live_at_reqbufs0 == 0, "av1-fds-dropped-before-reqbufs0");
    expect_true(ctx.cap_width >= 1920 && ctx.cap_height >= 1080,
                "av1-capture-holds-4k");
    expect_true(g_last_cap_w == 1920 && g_last_cap_h == 1080,
                "av1-sfm-coded-size");

    i0 = op_index("STREAMOFF");
    i1 = op_index("REQBUFS0");
    i2 = op_index("S_FMT_CAP");
    i3 = op_index("REQBUFSN");
    i4 = op_index("STREAMON");
    expect_true(i0 >= 0 && i1 > i0, "av1-streamoff-before-reqbufs0");
    expect_true(i1 >= 0 && i2 > i1, "av1-reqbufs0-before-sfmt");
    expect_true(i2 >= 0 && i3 > i2, "av1-sfmt-before-reqbufsn");
    expect_true(i4 > i3, "av1-streamon-after-recapture");

    if (surf->dma_buf_fd >= 0)
        close(surf->dma_buf_fd);
    free(surf);
    if (ctx.request_fd >= 0)
        close(ctx.request_fd);
    close(sp[0]);
    v4l2sl_set_ioctl_hook(NULL);
    g_ctx = NULL;
}

struct recap_env {
    struct v4l2sl_driver_data dd;
    struct v4l2sl_context ctx;
    struct v4l2sl_surface *surf;
    VASurfaceID targets[1];
    uint8_t outbuf[8192];
    int sp[2];
};

static int recap_env_open(struct recap_env *e, int cap_w, int cap_h)
{
    memset(e, 0, sizeof(*e));
    e->sp[0] = e->sp[1] = -1;
    e->surf = calloc(1, sizeof(*e->surf));
    if (!e->surf)
        return -1;
    if (pipe(e->sp) < 0) {
        free(e->surf);
        e->surf = NULL;
        return -1;
    }
    if (write(e->sp[1], "x", 1) != 1) {
        close(e->sp[0]);
        close(e->sp[1]);
        free(e->surf);
        e->surf = NULL;
        return -1;
    }
    e->surf->width = cap_w;
    e->surf->height = cap_h;
    e->surf->format = VA_FOURCC_NV12;
    e->surf->buf_index = -1;
    e->surf->dma_buf_fd = -1;
    e->surf->cpu_stride = (uint32_t)cap_w;
    if (v4l2sl_surface_alloc_export_fd(e->surf) < 0)
        return -1;
    e->targets[0] = 1;
    e->dd.surfaces[1] = e->surf;
    e->ctx.driver_data = &e->dd;
    e->ctx.v4l2_fd = e->sp[0];
    e->ctx.request_fd = e->sp[1];
    e->ctx.media_fd = -1;
    e->ctx.width = cap_w;
    e->ctx.height = cap_h;
    e->ctx.cap_width = (uint32_t)cap_w;
    e->ctx.cap_height = (uint32_t)cap_h;
    e->ctx.cap_stride = (uint32_t)cap_w;
    e->ctx.cap_sizeimage = (uint32_t)cap_w * (uint32_t)cap_h * 3 / 2;
    e->ctx.cap_pixelformat = V4L2_PIX_FMT_NV12;
    e->ctx.capture_bufs_allocd = 24;
    e->ctx.output_bufs_allocd = 2;
    e->ctx.streamed = 1;
    e->ctx.n_free_out = 1;
    e->ctx.free_out_bufs[0] = 0;
    e->ctx.output_buf_ptr[0] = e->outbuf;
    e->ctx.output_buf_size = sizeof(e->outbuf);
    e->ctx.render_targets = e->targets;
    e->ctx.num_render_targets = 1;
    e->ctx.current_surface = e->surf;
    g_ctx = &e->ctx;
    g_nops = 0;
    g_streamoff = g_reqbufs0 = g_reqbufs_n = g_sfmt_cap = 0;
    g_fds_live_at_reqbufs0 = -1;
    g_last_cap_w = (uint32_t)cap_w;
    g_last_cap_h = (uint32_t)cap_h;
    v4l2sl_set_ioctl_hook(mock_ioctl);
    return 0;
}

static void recap_env_close(struct recap_env *e)
{
    if (e->surf) {
        if (e->surf->dma_buf_fd >= 0)
            close(e->surf->dma_buf_fd);
        free(e->surf);
        e->surf = NULL;
    }
    if (e->ctx.request_fd >= 0)
        close(e->ctx.request_fd);
    if (e->sp[0] >= 0)
        close(e->sp[0]);
    e->ctx.request_fd = -1;
    e->sp[0] = e->sp[1] = -1;
    v4l2sl_set_ioctl_hook(NULL);
    g_ctx = NULL;
}

static void expect_grew_to(struct recap_env *e, int w, int h, const char *tag)
{
    char t[80];
    int i0, i1, i2, i3, i4;

    snprintf(t, sizeof(t), "%s-status-geom", tag);
    expect_true(e->ctx.cap_width >= (uint32_t)w && e->ctx.cap_height >= (uint32_t)h, t);
    snprintf(t, sizeof(t), "%s-reqbufs0", tag);
    expect_true(g_reqbufs0 == 1, t);
    snprintf(t, sizeof(t), "%s-fds-dropped", tag);
    expect_true(g_fds_live_at_reqbufs0 == 0, t);
    i0 = op_index("STREAMOFF");
    i1 = op_index("REQBUFS0");
    i2 = op_index("S_FMT_CAP");
    i3 = op_index("REQBUFSN");
    i4 = op_index("STREAMON");
    snprintf(t, sizeof(t), "%s-order", tag);
    expect_true(i0 >= 0 && i1 > i0 && i2 > i1 && i3 > i2 && i4 > i3, t);
}

static void test_ensure_noop_shrink_fmt(void)
{
    struct v4l2sl_context ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.v4l2_fd = 7;
    ctx.cap_width = 1920;
    ctx.cap_height = 1080;
    ctx.cap_pixelformat = V4L2_PIX_FMT_NV12;
    ctx.capture_bufs_allocd = 4;
    ctx.streamed = 1;
    g_ctx = &ctx;
    g_nops = 0;
    g_streamoff = g_reqbufs0 = g_reqbufs_n = g_sfmt_cap = 0;
    g_last_cap_w = 1920;
    g_last_cap_h = 1080;
    v4l2sl_set_ioctl_hook(mock_ioctl);

    expect_true(v4l2sl_ensure_capture(&ctx, 1280, 720, V4L2_PIX_FMT_NV12) == 0,
                "shrink-ok");
    expect_true(g_reqbufs0 == 0 && g_streamoff == 0, "shrink-no-recapture");
    expect_true(ctx.cap_width == 1920 && ctx.cap_height == 1080, "shrink-keeps-large");
    expect_true(v4l2sl_ensure_capture(&ctx, 1920, 1080, V4L2_PIX_FMT_NV12) == 0,
                "same-ok");
    expect_true(g_reqbufs0 == 0, "same-no-recapture");
    expect_true(v4l2sl_ensure_capture(&ctx, 1920, 1080, V4L2_PIX_FMT_NV15) == 0,
                "fmt-change-ok");
    expect_true(g_reqbufs0 == 1, "fmt-reqbufs0");
    expect_true(ctx.cap_pixelformat == V4L2_PIX_FMT_NV15, "fmt-nv15");
    v4l2sl_set_ioctl_hook(NULL);
    g_ctx = NULL;
}

static void test_h264_translate_recapture(void)
{
    struct recap_env e;
    VAPictureParameterBufferH264 pic;
    uint8_t nal[] = { 0, 0, 0, 1, 0x65, 0, 0, 0 };
    struct v4l2sl_buffer pic_buf, data_buf;
    struct v4l2sl_buffer *bufs[2];
    int i;

    if (recap_env_open(&e, 432, 240) < 0) {
        fprintf(stderr, "FAIL recap_env h264\n");
        g_fail++;
        return;
    }
    memset(&pic, 0, sizeof(pic));
    for (i = 0; i < 16; i++)
        pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    pic.picture_width_in_mbs_minus1 = 119;
    pic.picture_height_in_mbs_minus1 = 67;
    pic.seq_fields.bits.chroma_format_idc = 1;
    pic.seq_fields.bits.frame_mbs_only_flag = 1;
    pic.pic_fields.bits.reference_pic_flag = 1;
    e.ctx.profile = VAProfileH264High;
    e.ctx.codec = V4L2SL_CODEC_H264;
    memset(&pic_buf, 0, sizeof(pic_buf));
    memset(&data_buf, 0, sizeof(data_buf));
    pic_buf.type = VAPictureParameterBufferType;
    pic_buf.data = &pic;
    pic_buf.size = sizeof(pic);
    data_buf.type = VASliceDataBufferType;
    data_buf.data = nal;
    data_buf.size = sizeof(nal);
    bufs[0] = &pic_buf;
    bufs[1] = &data_buf;
    expect_true(v4l2sl_h264_translate(&e.ctx, bufs, 2) == VA_STATUS_SUCCESS,
                "h264-translate-status");
    expect_grew_to(&e, 1920, 1088, "h264");
    recap_env_close(&e);
}

static void test_hevc_translate_recapture(void)
{
    struct recap_env e;
    VAPictureParameterBufferHEVC pic;
    uint8_t nal[] = { 0, 0, 0, 1, 0x26, 0, 0, 0 };
    struct v4l2sl_buffer pic_buf, data_buf;
    struct v4l2sl_buffer *bufs[2];
    int i;

    if (recap_env_open(&e, 432, 240) < 0) {
        fprintf(stderr, "FAIL recap_env hevc\n");
        g_fail++;
        return;
    }
    memset(&pic, 0, sizeof(pic));
    for (i = 0; i < 15; i++)
        pic.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    pic.pic_width_in_luma_samples = 1920;
    pic.pic_height_in_luma_samples = 1080;
    pic.pic_fields.bits.chroma_format_idc = 1;
    e.ctx.codec = V4L2SL_CODEC_HEVC;
    memset(&pic_buf, 0, sizeof(pic_buf));
    memset(&data_buf, 0, sizeof(data_buf));
    pic_buf.type = VAPictureParameterBufferType;
    pic_buf.data = &pic;
    pic_buf.size = sizeof(pic);
    data_buf.type = VASliceDataBufferType;
    data_buf.data = nal;
    data_buf.size = sizeof(nal);
    bufs[0] = &pic_buf;
    bufs[1] = &data_buf;
    expect_true(v4l2sl_hevc_translate(&e.ctx, bufs, 2) == VA_STATUS_SUCCESS,
                "hevc-translate-status");
    expect_grew_to(&e, 1920, 1080, "hevc");
    recap_env_close(&e);
}

static void test_vp8_translate_recapture(void)
{
    struct recap_env e;
    VAPictureParameterBufferVP8 pic;
    VASliceParameterBufferVP8 slice;
    uint8_t data[64];
    struct v4l2sl_buffer pic_buf, sl_buf, data_buf;
    struct v4l2sl_buffer *bufs[3];

    if (recap_env_open(&e, 320, 240) < 0) {
        fprintf(stderr, "FAIL recap_env vp8\n");
        g_fail++;
        return;
    }
    memset(&pic, 0, sizeof(pic));
    memset(&slice, 0, sizeof(slice));
    memset(data, 0x11, sizeof(data));
    pic.frame_width = 1280;
    pic.frame_height = 720;
    pic.last_ref_frame = VA_INVALID_ID;
    pic.golden_ref_frame = VA_INVALID_ID;
    pic.alt_ref_frame = VA_INVALID_ID;
    pic.pic_fields.bits.key_frame = 0;
    slice.num_of_partitions = 2;
    slice.partition_size[0] = 8;
    slice.partition_size[1] = 8;
    e.ctx.codec = V4L2SL_CODEC_VP8;
    memset(&pic_buf, 0, sizeof(pic_buf));
    memset(&sl_buf, 0, sizeof(sl_buf));
    memset(&data_buf, 0, sizeof(data_buf));
    pic_buf.type = VAPictureParameterBufferType;
    pic_buf.data = &pic;
    pic_buf.size = sizeof(pic);
    sl_buf.type = VASliceParameterBufferType;
    sl_buf.data = &slice;
    sl_buf.size = sizeof(slice);
    data_buf.type = VASliceDataBufferType;
    data_buf.data = data;
    data_buf.size = sizeof(data);
    bufs[0] = &pic_buf;
    bufs[1] = &sl_buf;
    bufs[2] = &data_buf;
    expect_true(v4l2sl_vp8_translate(&e.ctx, bufs, 3) == VA_STATUS_SUCCESS,
                "vp8-translate-status");
    expect_grew_to(&e, 1280, 720, "vp8");
    recap_env_close(&e);
}

static void test_mpeg2_translate_recapture(void)
{
    struct recap_env e;
    VAPictureParameterBufferMPEG2 pic;
    uint8_t nal[] = { 0, 0, 1, 0x00, 0x00 };
    struct v4l2sl_buffer pic_buf, data_buf;
    struct v4l2sl_buffer *bufs[2];

    if (recap_env_open(&e, 720, 480) < 0) {
        fprintf(stderr, "FAIL recap_env mpeg2\n");
        g_fail++;
        return;
    }
    memset(&pic, 0, sizeof(pic));
    pic.horizontal_size = 1920;
    pic.vertical_size = 1080;
    pic.forward_reference_picture = VA_INVALID_ID;
    pic.backward_reference_picture = VA_INVALID_ID;
    pic.picture_coding_type = 1;
    pic.picture_coding_extension.bits.picture_structure = 3;
    pic.picture_coding_extension.bits.progressive_frame = 1;
    e.ctx.codec = V4L2SL_CODEC_MPEG2;
    e.ctx.profile = VAProfileMPEG2Main;
    memset(&pic_buf, 0, sizeof(pic_buf));
    memset(&data_buf, 0, sizeof(data_buf));
    pic_buf.type = VAPictureParameterBufferType;
    pic_buf.data = &pic;
    pic_buf.size = sizeof(pic);
    data_buf.type = VASliceDataBufferType;
    data_buf.data = nal;
    data_buf.size = sizeof(nal);
    bufs[0] = &pic_buf;
    bufs[1] = &data_buf;
    expect_true(v4l2sl_mpeg2_translate(&e.ctx, bufs, 2) == VA_STATUS_SUCCESS,
                "mpeg2-translate-status");
    expect_grew_to(&e, 1920, 1080, "mpeg2");
    recap_env_close(&e);
}

static void test_translate_missing_params(void)
{
    struct v4l2sl_context ctx;
    struct v4l2sl_buffer *none[1] = { NULL };

    memset(&ctx, 0, sizeof(ctx));
    ctx.v4l2_fd = -1;
    ctx.request_fd = -1;
    expect_true(v4l2sl_h264_translate(&ctx, none, 0) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "h264-missing-pic");
    expect_true(v4l2sl_hevc_translate(&ctx, none, 0) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "hevc-missing-pic");
    expect_true(v4l2sl_av1_translate(&ctx, none, 0) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "av1-missing-pic");
    expect_true(v4l2sl_vp8_translate(&ctx, none, 0) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "vp8-missing-pic");
    expect_true(v4l2sl_mpeg2_translate(&ctx, none, 0) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "mpeg2-missing-pic");
}

int main(void)
{
    test_export_before_decode();
    test_recapture_small_to_large();
    test_ensure_noop_shrink_fmt();
    test_av1_translate_recapture();
    test_h264_translate_recapture();
    test_hevc_translate_recapture();
    test_vp8_translate_recapture();
    test_mpeg2_translate_recapture();
    test_translate_missing_params();
    return g_fail ? 1 : 0;
}
