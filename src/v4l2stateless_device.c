/*
 * v4l2stateless — V4L2 Request API device and buffer management
 *
 * Handles:
 * - V4L2 device open and capability check
 * - Output queue (compressed bitstream) setup + mmap
 * - Capture queue (decoded frames) setup
 * - MEDIA_IOC_REQUEST_ALLOC
 * - Buffer queue/dequeue
 * - DMA-BUF export from capture buffers
 * - Request submission (ext ctrls + VIDIOC_SUBSCRIBE_EVENT)
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <va/va_drmcommon.h>
#include <drm_fourcc.h>

#include "v4l2stateless.h"
#include "v4l2stateless_probe.h"

static v4l2sl_ioctl_fn g_ioctl_hook;

void v4l2sl_set_ioctl_hook(v4l2sl_ioctl_fn fn)
{
    g_ioctl_hook = fn;
}

/* Helper to call ioctl with retry (shared via the header). */
int v4l2sl_xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = g_ioctl_hook ? g_ioctl_hook(fd, request, arg)
                         : ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

#define xioctl(fd, req, arg) v4l2sl_xioctl((fd), (req), (arg))

/* poll() with EINTR retry — a stray signal (profiler, SIGWINCH) must never
 * be mistaken for a decode timeout; the decode path turns timeouts into
 * full STREAMOFF/STREAMON queue resets. */
int v4l2sl_poll_intr(struct pollfd *fds, nfds_t n, int timeout)
{
    int r;

    do {
        r = poll(fds, n, timeout);
    } while (r < 0 && errno == EINTR);
    return r;
}

/*
 * Open a V4L2 video device and verify it's a stateless decoder
 */
int v4l2sl_open_device(const char *path)
{
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "v4l2stateless: open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* Verify capabilities */
    struct v4l2_capability cap = { 0 };
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "v4l2stateless: QUERYCAP failed on %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
        fprintf(stderr, "v4l2stateless: %s is not M2M mplane device\n", path);
        close(fd);
        return -1;
    }

    fprintf(stderr, "v4l2stateless: opened %s (%s)\n", path, cap.card);
    return fd;
}

/*
 * Open the media device associated with a V4L2 video device
 * video node → matching media node via sysfs (numbers need not match)
 */
int v4l2sl_open_media_for_device(const char *video_path)
{
    /* /dev/videoN lives next to /dev/mediaM on the same platform device.
     * Walk sysfs — do not assume mediaN == videoN (AV1 has been video4 → media3). */
    char media_path[128];
    int fd;

    if (v4l2sl_find_media_path(video_path, media_path, sizeof(media_path)) < 0) {
        fprintf(stderr, "v4l2stateless: no sysfs media* for %s\n", video_path);
        return -1;
    }

    fd = open(media_path, O_RDWR);
    if (fd < 0)
        fprintf(stderr, "v4l2stateless: open %s (for %s): %s\n",
                media_path, video_path, strerror(errno));
    else
        fprintf(stderr, "v4l2stateless: media for %s is %s\n", video_path, media_path);
    return fd;
}

/*
 * Allocate a V4L2 request
 */
int v4l2sl_request_alloc(int media_fd)
{
    int request_fd = -1;
    if (xioctl(media_fd, MEDIA_IOC_REQUEST_ALLOC, &request_fd) < 0) {
        fprintf(stderr, "v4l2stateless: REQUEST_ALLOC failed: %s\n", strerror(errno));
        return -1;
    }
    return request_fd;
}

/*
 * Setup output queue (compressed bitstream input)
 * Returns 0 on success, -1 on error
 */
int v4l2sl_setup_output_queue(int fd, uint32_t codec_format, int width, int height)
{
    /* Set output format (compressed input) */
    struct v4l2_format fmt = { 0 };
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = codec_format;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    /* hantro coded formats want sizeimage ~= luma size, not a 4MB guess.
     * GStreamer sizes H.264 OUTPUT as (width*height*pixel_bitdepth)/8;
     * 4:2:0 8-bit=12, 10-bit=15. A luma-sized buffer is too small for
     * 10-bit High10 and rkvdec then writes an empty NV15 capture. */
    if (codec_format == V4L2_PIX_FMT_AV1_FRAME ||
        codec_format == V4L2_PIX_FMT_VP8_FRAME ||
        codec_format == V4L2_PIX_FMT_MPEG2_SLICE)
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage = (uint32_t)width * (uint32_t)height;
    else if (codec_format == V4L2_PIX_FMT_H264_SLICE)
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage =
            (uint32_t)width * (uint32_t)height * 15 / 8;

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "v4l2stateless: S_FMT output failed: %s\n", strerror(errno));
        return -1;
    }

    /* Set codec-specific controls */
    struct v4l2_control ctrl = { 0 };

    if (codec_format == V4L2_PIX_FMT_H264_SLICE) {
        ctrl.id = V4L2_CID_STATELESS_H264_DECODE_MODE;
        ctrl.value = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED;
        if (xioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0)
            fprintf(stderr, "v4l2stateless: set H264 decode mode failed: %s\n", strerror(errno));

        ctrl.id = V4L2_CID_STATELESS_H264_START_CODE;
        ctrl.value = V4L2_STATELESS_H264_START_CODE_ANNEX_B;
        if (xioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0)
            fprintf(stderr, "v4l2stateless: set H264 start code failed: %s\n", strerror(errno));
    } else if (codec_format == V4L2_PIX_FMT_HEVC_SLICE) {
        ctrl.id = V4L2_CID_STATELESS_HEVC_DECODE_MODE;
        ctrl.value = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED;
        if (xioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0)
            fprintf(stderr, "v4l2stateless: set HEVC decode mode failed: %s\n", strerror(errno));

        ctrl.id = V4L2_CID_STATELESS_HEVC_START_CODE;
        ctrl.value = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B;
        if (xioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0)
            fprintf(stderr, "v4l2stateless: set HEVC start code failed: %s\n", strerror(errno));
    }

    /* Request output buffers */
    struct v4l2_requestbuffers req = { 0 };
    req.count = V4L2SL_NUM_OUTPUT_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "v4l2stateless: REQBUFS output failed: %s\n", strerror(errno));
        return -1;
    }

    fprintf(stderr, "v4l2stateless: output queue: %d buffers\n", req.count);
    return req.count;  /* return number of allocated buffers */
}

/*
 * Setup capture queue (decoded frames output)
 * Returns 0 on success, -1 on error
 */
int v4l2sl_setup_capture_queue(int fd, int width, int height, uint32_t pixelformat)
{
    return v4l2sl_setup_capture_queue_count(fd, width, height, pixelformat,
                                            V4L2SL_NUM_CAPTURE_BUFS);
}

int v4l2sl_setup_capture_queue_count(int fd, int width, int height,
                                     uint32_t pixelformat, int count)
{
    /* Set capture format (decoded output). Default NV12; 10-bit/422 use
     * NV15/NV16/NV20 which rkvdec only accepts after a matching SPS. */
    struct v4l2_format fmt = { 0 };
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = pixelformat ? pixelformat : V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "v4l2stateless: S_FMT capture failed: %s\n", strerror(errno));
        return -1;
    }

    /* Request capture buffers */
    struct v4l2_requestbuffers req = { 0 };
    req.count = count > 0 ? count : V4L2SL_NUM_CAPTURE_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "v4l2stateless: REQBUFS capture failed: %s\n", strerror(errno));
        return -1;
    }

    fprintf(stderr, "v4l2stateless: capture queue: %d buffers\n", req.count);
    return req.count;  /* return number of allocated buffers */
}

/*
 * Start streaming on a queue — required after REQBUFS and before QBUF
 * Returns 0 on success, -1 on error
 */
int v4l2sl_streamon(int fd, enum v4l2_buf_type type)
{
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "v4l2stateless: STREAMON(type=%u) failed: %s\n",
                type, strerror(errno));
        return -1;
    }
    return 0;
}

int v4l2sl_streamoff(int fd, enum v4l2_buf_type type)
{
    if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
        fprintf(stderr, "v4l2stateless: STREAMOFF(type=%u) failed: %s\n",
                type, strerror(errno));
        return -1;
    }
    return 0;
}

static void release_ctx_capture_surfaces(struct v4l2sl_context *ctx);
static void v4l2sl_unmap_capture_buffers(struct v4l2sl_context *ctx);
static int v4l2sl_mmap_one_capture(struct v4l2sl_context *ctx, int index);

static int reqbufs_zero(int fd, enum v4l2_buf_type type)
{
    struct v4l2_requestbuffers req = { 0 };

    req.count = 0;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "v4l2stateless: REQBUFS(0) type=%u failed: %s\n",
                type, strerror(errno));
        return -1;
    }
    return 0;
}

void v4l2sl_m2m_teardown(struct v4l2sl_context *ctx, struct v4l2sl_m2m_state *q)
{
    int i;

    if (!ctx || !q || !q->valid)
        return;
    if (ctx->v4l2_fd >= 0) {
        v4l2sl_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        v4l2sl_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        reqbufs_zero(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        reqbufs_zero(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    }
    for (i = 0; i < 3; i++) {
        if (q->out_map[i] && q->out_map[i] != MAP_FAILED)
            munmap(q->out_map[i], q->out_len[i]);
    }
    if (q->cap_map && q->cap_map != MAP_FAILED)
        munmap(q->cap_map, q->cap_len);
    memset(q, 0, sizeof(*q));
}

void v4l2sl_release_context_device(struct v4l2sl_context *ctx)
{
    int i;

    if (!ctx)
        return;

    /* Persistent M2M queues hold V4L2 MMAP mappings — release them while
     * the device fd is still open. */
    v4l2sl_m2m_teardown(ctx, &ctx->vpp_q);
    v4l2sl_m2m_teardown(ctx, &ctx->jpeg_q);

    if (ctx->v4l2_fd >= 0 && ctx->streamed) {
        v4l2sl_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        v4l2sl_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        ctx->streamed = 0;
    }

    if (ctx->request_fd >= 0) {
        close(ctx->request_fd);
        ctx->request_fd = -1;
    }

    for (i = 0; i < ctx->output_bufs_allocd && i < V4L2SL_NUM_OUTPUT_BUFS; i++) {
        if (ctx->output_buf_ptr[i] && ctx->output_buf_ptr[i] != MAP_FAILED) {
            munmap(ctx->output_buf_ptr[i], ctx->output_buf_size);
            ctx->output_buf_ptr[i] = NULL;
        }
    }
    v4l2sl_unmap_capture_buffers(ctx);

    if (ctx->v4l2_fd >= 0) {
        if (ctx->output_bufs_allocd > 0) {
            reqbufs_zero(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
            ctx->output_bufs_allocd = 0;
            ctx->n_free_out = 0;
        }
        if (ctx->capture_bufs_allocd > 0) {
            reqbufs_zero(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
            ctx->capture_bufs_allocd = 0;
            ctx->n_free_cap = 0;
        }
        close(ctx->v4l2_fd);
        ctx->v4l2_fd = -1;
    }

    if (ctx->media_fd >= 0) {
        close(ctx->media_fd);
        ctx->media_fd = -1;
    }

    release_ctx_capture_surfaces(ctx);
}

static void release_ctx_capture_surfaces(struct v4l2sl_context *ctx)
{
    int i;

    if (!ctx || !ctx->driver_data)
        return;
    for (i = 0; i < ctx->num_render_targets; i++) {
        VASurfaceID id = ctx->render_targets[i];
        struct v4l2sl_surface *s;

        s = v4l2sl_surface_by_id(ctx->driver_data, id);
        if (!s)
            continue;
        /* Keep the create-time memfd. Closing a V4L2 EXPBUF fd while the
         * GPU still holds a dup hangs the RK3588 CMA pool. */
        s->buf_index = -1;
        s->cap_view = NULL;
    }
}

/* Rebuild both free pools from scratch. Capture indices still claimed by a
 * render-target surface (surf->buf_index) stay claimed — begin_picture
 * returns them when the surface is re-targeted. */
static void rebuild_free_pools(struct v4l2sl_context *ctx)
{
    int claimed[V4L2SL_MAX_CAPTURE_BUFS] = { 0 };
    int i;

    ctx->n_free_out = 0;
    for (i = 0; i < ctx->output_bufs_allocd && i < V4L2SL_NUM_OUTPUT_BUFS; i++)
        v4l2sl_out_pool_push(ctx, i);

    if (ctx->driver_data && ctx->render_targets) {
        for (i = 0; i < ctx->num_render_targets; i++) {
            struct v4l2sl_surface *s =
                v4l2sl_surface_by_id(ctx->driver_data, ctx->render_targets[i]);

            if (s && s->buf_index >= 0 && s->buf_index < V4L2SL_MAX_CAPTURE_BUFS)
                claimed[s->buf_index] = 1;
        }
    }

    ctx->n_free_cap = 0;
    for (i = 0; i < ctx->capture_bufs_allocd && i < V4L2SL_MAX_CAPTURE_BUFS; i++)
        if (!claimed[i])
            v4l2sl_cap_pool_push(ctx, i);
}

void v4l2sl_decode_reset(struct v4l2sl_context *ctx)
{
    if (!ctx || ctx->v4l2_fd < 0)
        return;

    fprintf(stderr, "v4l2stateless: resetting decode queues\n");

    if (ctx->streamed) {
        v4l2sl_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        v4l2sl_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        ctx->streamed = 0;
    }
    /* STREAMOFF also drops every kernel DPB reference. */
    v4l2sl_av1_dpb_model_reset(ctx);

    if (ctx->request_fd >= 0) {
        close(ctx->request_fd);
        ctx->request_fd = -1;
    }

    /* STREAMOFF hands every queued buffer back in ERROR state (rkvdec and
     * hantro both do this in stop_streaming), so from this point every
     * allocated index is userspace-owned again. */
    rebuild_free_pools(ctx);
}

int v4l2sl_decode_submit(struct v4l2sl_context *ctx, int out_buf_idx,
                         uint32_t bytesused, uint64_t timestamp)
{
    struct pollfd pfd;
    int cap_buf_idx, done_cap, done_out;

    if (!ctx || ctx->v4l2_fd < 0 || ctx->request_fd < 0)
        return -1;

    cap_buf_idx = -1;
    if (v4l2sl_expbuf_export_wanted() && ctx->current_surface &&
        ctx->current_surface->buf_index >= 0)
        cap_buf_idx = ctx->current_surface->buf_index;
    if (cap_buf_idx < 0) {
        if (ctx->n_free_cap == 0) {
            fprintf(stderr, "v4l2stateless: no free capture buffer\n");

            v4l2sl_out_pool_push(ctx, out_buf_idx);
            return -1;
        }
        cap_buf_idx = ctx->free_cap_bufs[--ctx->n_free_cap];
    }

    if (v4l2sl_queue_output(ctx, out_buf_idx, bytesused, timestamp) < 0) {
        fprintf(stderr, "v4l2stateless: QBUF output[%d] failed\n", out_buf_idx);
        v4l2sl_out_pool_push(ctx, out_buf_idx);
        v4l2sl_cap_pool_push(ctx, cap_buf_idx);
        return -1;
    }

    /* Capture is queued bare: the stateless decoder spec forbids CAPTURE
     * buffers in a request (vb2 answers EPERM for them). */
    if (v4l2sl_queue_capture(ctx->v4l2_fd, cap_buf_idx, ctx->request_fd) < 0) {
        fprintf(stderr, "v4l2stateless: QBUF capture[%d] failed\n", cap_buf_idx);
        v4l2sl_cap_pool_push(ctx, cap_buf_idx);
        /* Recover the bitstream buffer so the pool does not leak. */
        pfd.fd = ctx->v4l2_fd;
        pfd.events = POLLOUT;
        if (v4l2sl_poll_intr(&pfd, 1, 200) > 0) {
            done_out = v4l2sl_dequeue_buffer(ctx->v4l2_fd,
                                             V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                             NULL);
            if (done_out >= 0)
                v4l2sl_out_pool_push(ctx, done_out);
        }
        return -1;
    }

    if (v4l2sl_submit_request(ctx->request_fd) < 0) {
        fprintf(stderr, "v4l2stateless: MEDIA_REQUEST_IOC_QUEUE failed\n");
        /* The buffers stay queued kernel-side: full reset, not just a
         * pool push — a request that failed to queue produces nothing. */
        v4l2sl_decode_reset(ctx);
        return -1;
    }

    pfd.fd = ctx->v4l2_fd;
    pfd.events = POLLIN;
    if (v4l2sl_poll_intr(&pfd, 1, 3000) <= 0) {
        fprintf(stderr, "v4l2stateless: decode timeout, resetting queues\n");
        v4l2sl_decode_reset(ctx);
        return -1;
    }

    {
        int cap_err = 0;

        done_cap = v4l2sl_dequeue_buffer(ctx->v4l2_fd,
                                         V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                         &cap_err);
        if (done_cap < 0) {
            fprintf(stderr, "v4l2stateless: no completed capture buffer\n");
            v4l2sl_decode_reset(ctx);
            return -1;
        }
        if (cap_err) {
            /* Corrupt frame: recycle the slot, recover the bitstream buffer
             * via the shared tail, and report -2 ("skipped") — callers mark
             * the surface instead of failing the vaEndPicture entrypoint
             * (Chrome caches VA-API failures for the whole session). */
            v4l2sl_cap_pool_push(ctx, done_cap);
            done_cap = -2;
        }
    }

    done_out = v4l2sl_dequeue_buffer(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, NULL);
    if (done_out < 0) {
        pfd.events = POLLOUT;
        if (v4l2sl_poll_intr(&pfd, 1, 200) > 0)
            done_out = v4l2sl_dequeue_buffer(ctx->v4l2_fd,
                                             V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                             NULL);
    }
    if (done_out >= 0)
        v4l2sl_out_pool_push(ctx, done_out);

    /* Recycle the request instead of close+realloc every picture — two
     * syscalls per frame saved. EINVAL (ancient kernel) falls back to the
     * close path; begin_picture re-allocates. */
    if (xioctl(ctx->request_fd, MEDIA_REQUEST_IOC_REINIT, NULL) < 0 &&
        errno == EINVAL) {
        close(ctx->request_fd);
        ctx->request_fd = -1;
    }

    return done_cap;
}

/*
 * Reconfigure capture (and coded OUTPUT size) when bit-depth, chroma or
 * resolution changes. Safe to call before the first STREAMON.
 */
int v4l2sl_ensure_capture(struct v4l2sl_context *ctx, int width, int height,
                          uint32_t pixelformat)
{
    int n_cap;
    uint32_t fourcc = pixelformat ? pixelformat : V4L2_PIX_FMT_NV12;
    int size_changed, fmt_changed;

    if (!ctx || ctx->v4l2_fd < 0 || width <= 0 || height <= 0)
        return -1;

    /* Display size from vaCreateContext is often 1920x1080 while the
     * coded size is 1920x1088. Reconfig only if the current capture
     * buffer cannot hold the new frame, or the fourcc changed. */
    size_changed = (ctx->cap_width == 0) ||
                   (width > (int)ctx->cap_width) ||
                   (height > (int)ctx->cap_height);
    fmt_changed = (ctx->cap_pixelformat != fourcc) ||
                  (ctx->capture_bufs_allocd <= 0);

    if (!size_changed && !fmt_changed)
        return 0;

    fprintf(stderr,
            "v4l2stateless: renegotiate capture %dx%d %.4s -> %dx%d %.4s streamed=%d\n",
            ctx->width, ctx->height, (char *)&ctx->cap_pixelformat,
            width, height, (char *)&fourcc, ctx->streamed);

    if (ctx->streamed) {
        v4l2sl_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        v4l2sl_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        ctx->streamed = 0;
        v4l2sl_av1_dpb_model_reset(ctx);
    }

    release_ctx_capture_surfaces(ctx);
    v4l2sl_unmap_capture_buffers(ctx);

    if (ctx->capture_bufs_allocd > 0) {
        struct v4l2_requestbuffers req = { 0 };
        req.count = 0;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
            fprintf(stderr, "v4l2stateless: REQBUFS(0) capture failed: %s\n",
                    strerror(errno));
            return -1;
        }
        ctx->capture_bufs_allocd = 0;
        ctx->n_free_cap = 0;
    }

    if (size_changed) {
        /* A bigger coded size also means bigger bitstream buffers. vb2
         * refuses S_FMT(OUTPUT) with -EBUSY while buffers are allocated,
         * so the OUTPUT queue has to be rebuilt too (spec: dynamic
         * resolution change re-runs the OUTPUT S_FMT step). Best effort:
         * if the re-mmap fails we keep going with empty pointers — the
         * next QBUF will still carry the right bytesused. */
        struct v4l2_format ofmt = { 0 };
        struct v4l2_format cur = { 0 };
        int i, n_out;
        uint32_t codec_fcc = 0;

        ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        cur.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (ctx->output_bufs_allocd > 0 &&
            xioctl(ctx->v4l2_fd, VIDIOC_G_FMT, &cur) == 0 &&
            (cur.fmt.pix_mp.width < (uint32_t)width ||
             cur.fmt.pix_mp.height < (uint32_t)height)) {
            codec_fcc = cur.fmt.pix_mp.pixelformat;

            for (i = 0; i < ctx->output_bufs_allocd &&
                        i < V4L2SL_NUM_OUTPUT_BUFS; i++) {
                if (ctx->output_buf_ptr[i] && ctx->output_buf_ptr[i] != MAP_FAILED) {
                    munmap(ctx->output_buf_ptr[i], ctx->output_buf_size);
                    ctx->output_buf_ptr[i] = NULL;
                }
            }
            if (reqbufs_zero(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) == 0) {
                ctx->output_bufs_allocd = 0;
                ctx->n_free_out = 0;

                ofmt = cur;
                ofmt.fmt.pix_mp.width = width;
                ofmt.fmt.pix_mp.height = height;
                if (xioctl(ctx->v4l2_fd, VIDIOC_S_FMT, &ofmt) < 0)
                    fprintf(stderr, "v4l2stateless: S_FMT output resize failed: %s\n",
                            strerror(errno));
                n_out = v4l2sl_setup_output_queue(ctx->v4l2_fd, codec_fcc,
                                                  width, height);
                if (n_out > 0) {
                    int j;

                    ctx->output_bufs_allocd = n_out;
                    if (v4l2sl_mmap_output_buffers(ctx->v4l2_fd, n_out,
                                                   ctx->output_buf_ptr,
                                                   &ctx->output_buf_size) < 0)
                        fprintf(stderr,
                                "v4l2stateless: warning: output re-mmap failed\n");
                    for (j = 0; j < n_out && j < V4L2SL_NUM_OUTPUT_BUFS; j++)
                        v4l2sl_out_pool_push(ctx, j);
                } else {
                    fprintf(stderr,
                            "v4l2stateless: warning: output queue rebuild failed\n");
                }
            }
        }
    }

    /* Capture re-allocation with graceful degradation: CMA can be nearly
     * exhausted (desktop GBM), and a smaller pool beats a dead context.
     * The spec puts the reference-depth burden on the client — 24 covers
     * every profile; a degraded pool only hurts deep B-pyramids. */
    {
        int want_cap = ctx->codec == V4L2SL_CODEC_AV1 ?
                       V4L2SL_NUM_CAPTURE_BUFS_AV1 : V4L2SL_NUM_CAPTURE_BUFS;
        n_cap = v4l2sl_setup_capture_queue_count(ctx->v4l2_fd, width, height,
                                                 fourcc, want_cap);
    }
    if (n_cap <= 0 && V4L2SL_NUM_CAPTURE_BUFS > 24) {
        fprintf(stderr, "v4l2stateless: retrying capture REQBUFS with 24 buffers\n");
        n_cap = v4l2sl_setup_capture_queue_count(ctx->v4l2_fd, width, height,
                                                 fourcc, 24);
    }
    if (n_cap <= 0 && V4L2SL_NUM_CAPTURE_BUFS > 8) {
        fprintf(stderr, "v4l2stateless: retrying capture REQBUFS with 8 buffers\n");
        n_cap = v4l2sl_setup_capture_queue_count(ctx->v4l2_fd, width, height,
                                                 fourcc, 8);
    }
    if (n_cap <= 0 && V4L2SL_NUM_CAPTURE_BUFS > 4) {
        fprintf(stderr, "v4l2stateless: retrying capture REQBUFS with 4 buffers\n");
        n_cap = v4l2sl_setup_capture_queue_count(ctx->v4l2_fd, width, height,
                                                 fourcc, 4);
    }
    if (n_cap <= 0)
        return -1;

    ctx->capture_bufs_allocd = n_cap;
    if (v4l2sl_debug)
        fprintf(stderr, "v4l2stateless: capture pool n=%d\n", n_cap);
    ctx->n_free_cap = 0;
    {
        int i;
        for (i = 0; i < n_cap && i < V4L2SL_NUM_CAPTURE_BUFS; i++)
            v4l2sl_cap_pool_push(ctx, i);
    }
    ctx->width = width;
    ctx->height = height;
    ctx->cap_pixelformat = fourcc;

    if (v4l2sl_get_capture_geometry(ctx->v4l2_fd, &ctx->cap_width, &ctx->cap_height,
                                    &ctx->cap_stride, &ctx->cap_sizeimage) == 0) {
        fprintf(stderr, "v4l2stateless: capture geometry %ux%u stride=%u size=%u fourcc=%.4s\n",
                ctx->cap_width, ctx->cap_height, ctx->cap_stride, ctx->cap_sizeimage,
                (char *)&fourcc);
    }
    /* Map capture slots lazily after DQBUF. Mapping all 24 CMA
     * buffers here pins ~75MB and hangs the RK3588. */
    v4l2sl_bind_capture_export(ctx);
    return 0;
}

/*
 * Read back the driver-chosen capture geometry.
 * The stride can be larger than the width (alignment) — image export must
 * use these values, not the display dimensions.
 */
int v4l2sl_get_capture_geometry(int fd, uint32_t *w, uint32_t *h,
                                uint32_t *stride, uint32_t *sizeimage)
{
    struct v4l2_format fmt = { 0 };
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        fprintf(stderr, "v4l2stateless: G_FMT capture failed: %s\n", strerror(errno));
        return -1;
    }

    *w = fmt.fmt.pix_mp.width;
    *h = fmt.fmt.pix_mp.height;
    *stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    *sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    return 0;
}



static void v4l2sl_unmap_capture_buffers(struct v4l2sl_context *ctx)
{
    int i;

    if (!ctx)
        return;
    for (i = 0; i < V4L2SL_NUM_CAPTURE_BUFS; i++) {
        if (!ctx->capture_buf_ptr[i] || ctx->capture_buf_ptr[i] == MAP_FAILED) {
            ctx->capture_buf_ptr[i] = NULL;
            continue;
        }
        if (ctx->capture_buf_anon)
            free(ctx->capture_buf_ptr[i]);
        else
            munmap(ctx->capture_buf_ptr[i], ctx->capture_buf_size);
        ctx->capture_buf_ptr[i] = NULL;
    }
    ctx->capture_buf_size = 0;
    ctx->capture_buf_anon = 0;
}

static int v4l2sl_mmap_one_capture(struct v4l2sl_context *ctx, int index)
{
    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[1] = { 0 };
    void *ptr;
    uint32_t length;
    int anon;

    if (!ctx || ctx->v4l2_fd < 0 || index < 0 ||
        index >= ctx->capture_bufs_allocd || index >= V4L2SL_NUM_CAPTURE_BUFS)
        return -1;
    if (ctx->capture_buf_ptr[index] && ctx->capture_buf_ptr[index] != MAP_FAILED)
        return 0;

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    buf.length = 1;
    buf.m.planes = planes;
    if (xioctl(ctx->v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) {
        fprintf(stderr, "v4l2stateless: QUERYBUF capture[%d] failed: %s\n",
                index, strerror(errno));
        return -1;
    }
    length = planes[0].length;
    if (!length)
        return -1;
    anon = g_ioctl_hook ? 1 : 0;
    if (anon) {
        ptr = calloc(1, length);
        if (!ptr)
            return -1;
    } else {
        ptr = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
                   ctx->v4l2_fd, planes[0].m.mem_offset);
        if (ptr == MAP_FAILED) {
            fprintf(stderr, "v4l2stateless: mmap capture[%d] failed: %s\n",
                    index, strerror(errno));
            return -1;
        }
        if (v4l2sl_debug)
            fprintf(stderr, "v4l2stateless: capture mmap idx=%d size=%u\n",
                    index, length);
    }
    ctx->capture_buf_ptr[index] = ptr;
    ctx->capture_buf_size = length;
    ctx->capture_buf_anon = anon;
    return 0;
}

/*
 * Mmap all output buffers after REQBUFS
 * ptrs[] must be at least V4L2SL_NUM_OUTPUT_BUFS
 * Returns number of mmap'd buffers or -1 on error
 */
int v4l2sl_mmap_output_buffers(int fd, int count, void **ptrs, uint32_t *size_out)
{
    if (count > V4L2SL_NUM_OUTPUT_BUFS)
        count = V4L2SL_NUM_OUTPUT_BUFS;

    for (int i = 0; i < count; i++) {
        struct v4l2_buffer buf = { 0 };
        struct v4l2_plane planes[1] = { 0 };

        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = planes;

        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "v4l2stateless: QUERYBUF output[%d] failed: %s\n", i, strerror(errno));
            return -1;
        }

        void *ptr = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, planes[0].m.mem_offset);
        if (ptr == MAP_FAILED) {
            fprintf(stderr, "v4l2stateless: mmap output[%d] failed: %s\n", i, strerror(errno));
            return -1;
        }

        ptrs[i] = ptr;
        if (i == 0 && size_out)
            *size_out = planes[0].length;
    }

    return count;
}

/*
 * Queue a compressed bitstream buffer to the output queue.
 * The bitstream was already memcpy'd into the pre-mapped output buffer by
 * the caller — only its length travels with the QBUF.
 */
int v4l2sl_queue_output(struct v4l2sl_context *ctx, int buf_index,
                        uint32_t size, uint64_t timestamp)
{
    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[1] = { 0 };
    int fd = ctx->v4l2_fd;
    int request_fd = ctx->request_fd;

    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = buf_index;
    buf.length = 1;
    buf.m.planes = planes;
    buf.request_fd = request_fd;
    buf.flags = V4L2_BUF_FLAG_REQUEST_FD;

    /* Timestamp is in nanoseconds (µs-aligned). vb2 stores the timeval
     * internally as ns again, so this must round-trip losslessly to match
     * DPB reference_ts values — which live in the same ns domain. */
    buf.timestamp.tv_sec = timestamp / 1000000000ULL;
    buf.timestamp.tv_usec = (timestamp % 1000000000ULL) / 1000ULL;

    /* hantro AV1 rejects QBUF unless plane length matches the mapped size.
     * Lengths are fixed per REQBUFS cycle — memoize per index. */
    if (buf_index >= 0 && buf_index < V4L2SL_NUM_OUTPUT_BUFS &&
        ctx->output_plane_len[buf_index]) {
        planes[0].length = ctx->output_plane_len[buf_index];
    } else {
        struct v4l2_buffer q = { 0 };
        struct v4l2_plane qp[1] = { 0 };
        q.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        q.memory = V4L2_MEMORY_MMAP;
        q.index = buf_index;
        q.length = 1;
        q.m.planes = qp;
        if (xioctl(fd, VIDIOC_QUERYBUF, &q) == 0) {
            planes[0].length = qp[0].length;
            if (buf_index >= 0 && buf_index < V4L2SL_NUM_OUTPUT_BUFS)
                ctx->output_plane_len[buf_index] = qp[0].length;
        }
    }
    planes[0].bytesused = size;

    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "v4l2stateless: QBUF output[%d] failed: %s\n", buf_index, strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * Queue a capture buffer (decoded frame output)
 */
int v4l2sl_queue_capture(int fd, int buf_index, int request_fd)
{
    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[1] = { 0 };

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = buf_index;
    buf.length = 1;
    buf.m.planes = planes;
    /* request_fd intentionally NOT set: only OUTPUT (bitstream) buffers are
     * request objects. The CAPTURE queue does not support requests — binding
     * one makes vb2 fail QBUF with EPERM. Capture buffers are queued bare;
     * the decoder fills whichever capture buffer is available when the
     * request on the OUTPUT side completes. */
    (void)request_fd;

    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "v4l2stateless: QBUF capture[%d] failed: %s\n", buf_index, strerror(errno));
        return -1;
    }

    return 0;
}

int v4l2sl_surface_alloc_export_fd(struct v4l2sl_surface *s)
{
    uint32_t stride, h, sz;
    int fd;

    if (!s || s->width == 0 || s->height == 0)
        return -1;
    if (s->memfd_fd >= 0)
        return 0;

    stride = s->cpu_stride ? s->cpu_stride : s->width;
    h = s->height;
    sz = v4l2sl_va_image_size(s->format ? s->format : VA_FOURCC_NV12, stride, h);
    if (!sz)
        sz = stride * h * 3 / 2;

    fd = memfd_create("v4l2sl-surf", MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)sz) < 0) {
        close(fd);
        return -1;
    }
    s->memfd_fd = fd;
    s->memfd_size = sz;
    if (!s->stride)
        s->stride = stride;
    if (!s->aligned_h)
        s->aligned_h = h;
    if (!s->cap_fourcc)
        s->cap_fourcc = V4L2_PIX_FMT_NV12;
    return 0;
}

int v4l2sl_surface_ensure_cpu(struct v4l2sl_surface *s)
{
    if (!s || !s->cpu_size)
        return -1;
    if (!s->cpu_ptr) {
        s->cpu_ptr = calloc(1, s->cpu_size);
        if (!s->cpu_ptr)
            return -1;
    }
    return 0;
}

/* Grow-only: a smaller `size` (resolution step-down) must never ftruncate —
 * clients may still hold mappings or dup'd fds of the larger memfd, and
 * shrinking drops pages under them (SIGBUS). */
int v4l2sl_surface_grow_memfd(struct v4l2sl_surface *s, uint32_t size)
{
    if (!s)
        return -1;
    if (s->memfd_fd < 0 && v4l2sl_surface_alloc_export_fd(s) < 0)
        return -1;
    if (size == 0 || s->memfd_fd < 0 || size <= s->memfd_size)
        return 0;
    if (ftruncate(s->memfd_fd, (off_t)size) < 0)
        return -1;
    s->memfd_size = size;
    return 0;
}

/* Persistent per-surface mapping of memfd_fd (RW shared). Grow-only:
 * when `need` exceeds the cached length the mapping is grown — via
 * mremap, or when derived images still borrow the current mapping (a
 * moving remap would pull the pages from under them) by parking the old
 * one as "retired" until the last borrower releases it. Callers must
 * NOT munmap the result — the surface owns it. NULL on failure. */
void *v4l2sl_surface_map_memfd(struct v4l2sl_surface *s, uint32_t need)
{
    void *nm;

    if (!s || s->memfd_fd < 0 || need == 0)
        return NULL;
    if (s->memfd_map && s->memfd_map_size >= need)
        return s->memfd_map;
    if (need > s->memfd_size && v4l2sl_surface_grow_memfd(s, need) < 0)
        return NULL;
    if (s->memfd_map) {
        if (s->memfd_borrows) {
            if (s->memfd_retired)
                munmap(s->memfd_retired, s->memfd_retired_size);
            s->memfd_retired = s->memfd_map;
            s->memfd_retired_size = s->memfd_map_size;
            nm = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED,
                      s->memfd_fd, 0);
        } else {
            nm = mremap(s->memfd_map, s->memfd_map_size, need,
                        MREMAP_MAYMOVE);
        }
        if (nm == MAP_FAILED)
            return NULL;
    } else {
        nm = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED,
                  s->memfd_fd, 0);
        if (nm == MAP_FAILED)
            return NULL;
    }
    s->memfd_map = nm;
    s->memfd_map_size = need;
    return s->memfd_map;
}

int v4l2sl_surface_pull_capture(struct v4l2sl_context *ctx,
                                struct v4l2sl_surface *surf, int buf_index)
{
    uint32_t sz, stride, alh, fcc;
    void *src, *dst;

    if (!ctx || !surf || ctx->v4l2_fd < 0 || buf_index < 0)
        return -1;
    if (v4l2sl_mmap_one_capture(ctx, buf_index) < 0)
        return -1;
    if (!ctx->capture_buf_ptr[buf_index] ||
        ctx->capture_buf_ptr[buf_index] == MAP_FAILED)
        return -1;

    stride = ctx->cap_stride ? ctx->cap_stride : (uint32_t)ctx->width;
    alh = ctx->cap_height ? ctx->cap_height : (uint32_t)ctx->height;
    fcc = ctx->cap_pixelformat ? ctx->cap_pixelformat : V4L2_PIX_FMT_NV12;
    sz = ctx->capture_buf_size;
    if (ctx->cap_sizeimage && ctx->cap_sizeimage < sz)
        sz = ctx->cap_sizeimage;
    if (!sz)
        return -1;

    src = ctx->capture_buf_ptr[buf_index];

    if (v4l2sl_expbuf_export_wanted()) {
        /* Skip capture→GBM/memfd memcpy. Chrome EXPBUFs this index.
         * GetImage/Derive read capture_buf_ptr[buf_index] while it is live. */
        surf->has_pic = 1;
        surf->buf_index = buf_index;
        surf->stride = stride;
        surf->aligned_h = alh;
        surf->cap_fourcc = fcc;
        surf->cap_view = src;
        surf->last_writer = V4L2SL_WRITER_MEMFD;
        return 0;
    }

    /*
     * Lazy memfd: when this surface has a GBM display bo, the bo is the
     * per-frame snapshot and the memfd copy is skipped — CPU-readback
     * callers (vaGetImage / vaDeriveImage / VPP source) refill it on
     * demand via v4l2sl_surface_ensure_memfd(). Halves the per-frame
     * memcpy traffic for zero-copy clients (Chrome never reads back).
     * On upload failure fall back to the classic memfd snapshot so the
     * frame is never lost (the capture buffer recycles right after this).
     */
    if (surf->gbm_bo) {
        if (v4l2sl_gbm_surface_upload(surf, src, stride, alh) == 0) {
            surf->has_pic = 1;
            surf->buf_index = buf_index;
            surf->stride = stride;
            surf->aligned_h = alh;
            surf->cap_fourcc = fcc;
            surf->last_writer = V4L2SL_WRITER_BO;
            return 0;
        }
        fprintf(stderr,
                "v4l2stateless: gbm upload failed, using memfd snapshot\n");
    }

    if (v4l2sl_surface_grow_memfd(surf, sz) < 0)
        return -1;
    dst = v4l2sl_surface_map_memfd(surf, sz);
    if (!dst)
        return -1;
    memcpy(dst, src, sz);

    surf->has_pic = 1;
    surf->buf_index = buf_index;
    surf->stride = stride;
    surf->aligned_h = alh;
    surf->cap_fourcc = fcc;
    surf->last_writer = V4L2SL_WRITER_MEMFD;
    return 0;
}

int v4l2sl_bind_capture_export(struct v4l2sl_context *ctx)
{
    int i, n;
    uint32_t sz;

    if (!ctx || !ctx->driver_data || ctx->capture_bufs_allocd <= 0)
        return -1;

    sz = ctx->cap_sizeimage;
    if (!sz)
        sz = v4l2sl_capture_plane_size(ctx->cap_pixelformat ? ctx->cap_pixelformat
                                                            : V4L2_PIX_FMT_NV12,
                                       ctx->cap_stride, ctx->cap_height);

    n = ctx->num_render_targets;
    for (i = 0; i < n; i++) {
        VASurfaceID id;
        struct v4l2sl_surface *s;

        if (!ctx->render_targets)
            break;
        id = ctx->render_targets[i];
        s = v4l2sl_surface_by_id(ctx->driver_data, id);
        if (!s)
            continue;
        if (v4l2sl_surface_grow_memfd(s, sz) < 0)
            continue;
        s->stride = ctx->cap_stride;
        s->aligned_h = ctx->cap_height;
        s->cap_fourcc = ctx->cap_pixelformat;
    }
    return 0;
}

void v4l2sl_fill_prime_layers(VADRMPRIMESurfaceDescriptor *desc,
                              int fd, uint32_t object_size,
                              uint64_t modifier, uint32_t va_fourcc,
                              uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t chroma_row,
                              uint32_t combined_fmt, uint32_t luma_fmt,
                              uint32_t chroma_fmt, uint32_t flags)
{
    memset(desc, 0, sizeof(*desc));
    desc->fourcc = va_fourcc;
    desc->width = width;
    desc->height = height;
    desc->num_objects = 1;
    desc->objects[0].fd = fd;
    desc->objects[0].size = object_size;
    desc->objects[0].drm_format_modifier = modifier;

    if (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) {
        desc->num_layers = 2;
        desc->layers[0].drm_format = luma_fmt;
        desc->layers[0].num_planes = 1;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = pitch;
        desc->layers[1].drm_format = chroma_fmt;
        desc->layers[1].num_planes = 1;
        desc->layers[1].object_index[0] = 0;
        desc->layers[1].offset[0] = pitch * chroma_row;
        desc->layers[1].pitch[0] = pitch;
    } else {
        desc->num_layers = 1;
        desc->layers[0].drm_format = combined_fmt;
        desc->layers[0].num_planes = 2;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].object_index[1] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].offset[1] = pitch * chroma_row;
        desc->layers[0].pitch[0] = pitch;
        desc->layers[0].pitch[1] = pitch;
    }
}

VAStatus v4l2sl_surface_fill_prime(const struct v4l2sl_surface *surf,
                                   const struct v4l2sl_context *c,
                                   uint32_t flags,
                                   void *descriptor)
{
    VADRMPRIMESurfaceDescriptor *desc = descriptor;
    uint32_t stride, alh, cap_fcc, drm_fcc, plane_size;
    int fd;

    if (!surf || !descriptor)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (surf->memfd_fd < 0)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    stride = surf->stride ? surf->stride :
             (c && c->cap_stride ? c->cap_stride : surf->width);
    alh = surf->aligned_h ? surf->aligned_h :
          (c && c->cap_height ? c->cap_height : surf->height);
    cap_fcc = surf->cap_fourcc ? surf->cap_fourcc :
              (c && c->cap_pixelformat ? c->cap_pixelformat : V4L2_PIX_FMT_NV12);
    drm_fcc = v4l2sl_drm_fourcc_for_capture(cap_fcc);
    plane_size = v4l2sl_capture_plane_size(cap_fcc, stride, alh);

    fd = dup(surf->memfd_fd);
    if (fd < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    v4l2sl_fill_prime_layers(desc, fd, plane_size, DRM_FORMAT_MOD_LINEAR,
                             (cap_fcc == V4L2_PIX_FMT_NV12) ?
                                 VA_FOURCC_NV12 :
                                 v4l2sl_va_fourcc_for_capture(cap_fcc),
                             surf->width, surf->height, stride, alh,
                             drm_fcc,
                             (cap_fcc == V4L2_PIX_FMT_NV15) ?
                                 DRM_FORMAT_R16 : DRM_FORMAT_R8,
                             (cap_fcc == V4L2_PIX_FMT_NV15) ?
                                 DRM_FORMAT_GR1616 : DRM_FORMAT_GR88,
                             flags);
    return VA_STATUS_SUCCESS;
}

/*
 * Dequeue a buffer from the given queue type
 * Returns buffer index or -1 on error
 */
/* flag_error (optional) is set when the dequeued buffer carried
 * V4L2_BUF_FLAG_ERROR — the index is still returned so the caller can
 * recycle the slot; the data itself must not be used. */
int v4l2sl_dequeue_buffer(int fd, enum v4l2_buf_type type, int *flag_error)
{
    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[1] = { 0 };

    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = 1;
    buf.m.planes = planes;

    if (flag_error)
        *flag_error = 0;
    if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno != EAGAIN)
            fprintf(stderr, "v4l2stateless: DQBUF failed: %s\n", strerror(errno));
        return -1;
    }
    if (buf.flags & V4L2_BUF_FLAG_ERROR) {
        fprintf(stderr, "v4l2stateless: DQBUF type=%u idx=%u ERROR flags=0x%x bytesused=%u\n",
                (unsigned)type, buf.index, buf.flags, planes[0].bytesused);
        if (flag_error)
            *flag_error = 1;
    }

    return buf.index;
}

/*
 * Set V4L2 extended controls bound to a request.
 * Uses V4L2_CTRL_WHICH_REQUEST_VAL so the controls apply to this specific request
 * rather than being global defaults.
 *
 * request_fd: from MEDIA_IOC_REQUEST_ALLOC
 * v4l2_fd:    the video device fd (required by V4L2 to identify the device)
 * ctrls:      caller-allocated v4l2_ext_controls (may contain multiple controls)
 *
 * Returns 0 on success, -1 on error.
 */
int v4l2sl_set_request_controls(int request_fd, int v4l2_fd,
                                struct v4l2_ext_controls *ctrls)
{
    /* The request_fd goes into the which field when using request API */
    ctrls->which = V4L2_CTRL_WHICH_REQUEST_VAL | request_fd;

    if (xioctl(v4l2_fd, VIDIOC_S_EXT_CTRLS, ctrls) < 0) {
        fprintf(stderr, "v4l2stateless: S_EXT_CTRLS (request_fd=%d) failed: %s "
                "(%d controls, first_id=0x%x)\n",
                request_fd, strerror(errno),
                ctrls->count, ctrls->count > 0 ? ctrls->controls[0].id : 0);
        return -1;
    }

    return 0;
}

/*
 * Set extended controls as plain (non-request) values. Some codecs bind
 * stream-level controls globally — e.g. the rkvdec HEVC SPS rejects the
 * request-scoped variant with EINVAL.
 */
int v4l2sl_set_global_controls(int v4l2_fd, struct v4l2_ext_controls *ctrls)
{
    ctrls->which = 0;  /* V4L2_CTRL_WHICH_CUR_VAL */

    if (xioctl(v4l2_fd, VIDIOC_S_EXT_CTRLS, ctrls) < 0) {
        fprintf(stderr, "v4l2stateless: S_EXT_CTRLS (global) failed: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

/* Queue the request; the decoder consumes the controls bound to it.
 * Returns 0 on success, -1 on error. */
int v4l2sl_submit_request(int request_fd)
{
    if (xioctl(request_fd, MEDIA_REQUEST_IOC_QUEUE, NULL) < 0) {
        fprintf(stderr, "v4l2stateless: REQUEST_IOC_QUEUE failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

void v4l2sl_explog(const char *fmt, ...)
{
    va_list ap;

    if (!v4l2sl_debug)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

int v4l2sl_expbuf_export_wanted(void)
{
    static int once, wanted;
    if (!once) {
        const char *e = getenv("V4L2SL_EXPBUF_EXPORT");

        /* Default ON after Chrome zero-copy proof. Opt out with =0. */
        wanted = 1;
        if (e && e[0] == '0')
            wanted = 0;
        once = 1;
        if (v4l2sl_debug)
            fprintf(stderr, "v4l2stateless: EXPBUF wanted=%d pid=%d\n",
                    wanted, getpid());
    }
    return wanted;
}

int v4l2sl_capture_expbuf(struct v4l2sl_context *ctx, int buf_index)
{
    struct v4l2_exportbuffer exp;

    if (!ctx || ctx->v4l2_fd < 0 || buf_index < 0)
        return -1;
    memset(&exp, 0, sizeof(exp));
    exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    exp.index = (unsigned)buf_index;
    exp.plane = 0;
    exp.flags = O_CLOEXEC | O_RDWR;
    if (v4l2sl_xioctl(ctx->v4l2_fd, VIDIOC_EXPBUF, &exp) < 0) {
        fprintf(stderr, "v4l2stateless: EXPBUF capture[%d] failed: %s\n",
                buf_index, strerror(errno));
        return -1;
    }
    return exp.fd;
}

int v4l2sl_claim_capture_for_export(struct v4l2sl_context *ctx,
                                    struct v4l2sl_surface *surf)
{
    int idx, w, h;
    uint32_t fourcc;

    if (!ctx || !surf || ctx->v4l2_fd < 0)
        return -1;
    w = ctx->width > 0 ? ctx->width : (int)surf->width;
    h = ctx->height > 0 ? ctx->height : (int)surf->height;
    fourcc = ctx->cap_pixelformat ? ctx->cap_pixelformat : V4L2_PIX_FMT_NV12;
    if (v4l2sl_ensure_capture(ctx, w, h, fourcc) < 0)
        return -1;
    if (surf->buf_index >= 0)
        return 0;
    if (ctx->n_free_cap == 0) {
        v4l2sl_explog("v4l2stateless: claim capture: no free slot surf=%u\n",
                      (unsigned)surf->surface_id);
        return -1;
    }
    idx = ctx->free_cap_bufs[--ctx->n_free_cap];
    surf->buf_index = idx;
    surf->stride = ctx->cap_stride;
    surf->aligned_h = ctx->cap_height;
    surf->cap_fourcc = ctx->cap_pixelformat;
    /* Do not mmap here: mapping the whole Chrome pool pins ~75MB CMA and
     * has hung this SoC. EXPBUF does not need a userspace mapping. */
    v4l2sl_explog("v4l2stateless: claim capture idx=%d surf=%u\n",
                  idx, (unsigned)surf->surface_id);
    return 0;
}
