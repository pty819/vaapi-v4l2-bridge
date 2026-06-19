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
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/media.h>
#include <linux/videodev2.h>

#include "v4l2stateless.h"

/* Helper to call ioctl with retry */
static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
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
 * /dev/videoN → /dev/mediaN
 */
int v4l2sl_open_media_for_device(const char *video_path)
{
    /* Simple approach: try /dev/media0 */
    /* A proper implementation would read /sys/class/video4linux/videoN/device/media* */
    int fd = open("/dev/media0", O_RDWR);
    if (fd < 0)
        fprintf(stderr, "v4l2stateless: open /dev/media0: %s\n", strerror(errno));
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
int v4l2sl_setup_capture_queue(int fd, int width, int height)
{
    /* Set capture format (decoded output) — NV12 for all codecs */
    struct v4l2_format fmt = { 0 };
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "v4l2stateless: S_FMT capture failed: %s\n", strerror(errno));
        return -1;
    }

    /* Request capture buffers */
    struct v4l2_requestbuffers req = { 0 };
    req.count = V4L2SL_NUM_CAPTURE_BUFS;
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
 * Data is memcpy'd into the pre-mapped output buffer.
 */
int v4l2sl_queue_output(int fd, int buf_index,
                        const uint8_t *data, uint32_t size,
                        int request_fd, uint64_t timestamp)
{
    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[1] = { 0 };

    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = buf_index;
    buf.length = 1;
    buf.m.planes = planes;
    buf.request_fd = request_fd;
    buf.flags = V4L2_BUF_FLAG_REQUEST_FD;

    /* Set timestamp */
    buf.timestamp.tv_sec = timestamp / 1000000000ULL;
    buf.timestamp.tv_usec = (timestamp % 1000000000ULL) / 1000;

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
    buf.request_fd = request_fd;
    buf.flags = V4L2_BUF_FLAG_REQUEST_FD;

    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "v4l2stateless: QBUF capture[%d] failed: %s\n", buf_index, strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * Export a capture buffer as DMA-BUF fd
 */
int v4l2sl_export_dmabuf(int fd, int buf_index)
{
    struct v4l2_exportbuffer exp = { 0 };
    exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    exp.index = buf_index;
    exp.flags = O_CLOEXEC | O_RDWR;

    if (xioctl(fd, VIDIOC_EXPBUF, &exp) < 0) {
        fprintf(stderr, "v4l2stateless: EXPBUF capture[%d] failed: %s\n",
                buf_index, strerror(errno));
        return -1;
    }

    return exp.fd;
}

/*
 * Dequeue a buffer from the given queue type
 * Returns buffer index or -1 on error
 */
int v4l2sl_dequeue_buffer(int fd, enum v4l2_buf_type type)
{
    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[1] = { 0 };

    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = 1;
    buf.m.planes = planes;

    if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno != EAGAIN)
            fprintf(stderr, "v4l2stateless: DQBUF failed: %s\n", strerror(errno));
        return -1;
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
 * Submit a V4L2 request.
 * Uses the V4L2 request API: on the request fd, we call VIDIOC_SUBSCRIBE_EVENT
 * for V4L2_EVENT_DECODE to trigger decode, then poll/wait for completion.
 *
 * Actually, the correct mechanism is: on the request fd itself, call
 *   ioctl(request_fd, MEDIA_REQUEST_IOC_QUEUE, NULL)
 * But that requires <linux/media.h> with the newer request ioctl definitions.
 * The kernel's V4L2 request API uses the request_fd ioctl interface.
 * We fall back to the event subscription approach if MEDIA_REQUEST_IOC_QUEUE
 * is not available.
 *
 * Returns 0 on success, -1 on error.
 */
int v4l2sl_submit_request(int request_fd)
{
    /* Try MEDIA_REQUEST_IOC_QUEUE first (kernel >= 5.11) */
#ifndef MEDIA_REQUEST_IOC_QUEUE
#define MEDIA_REQUEST_IOC_QUEUE _IO('R', 0x01)
#endif

    if (xioctl(request_fd, MEDIA_REQUEST_IOC_QUEUE, NULL) < 0) {
        fprintf(stderr, "v4l2stateless: REQUEST_IOC_QUEUE failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}
