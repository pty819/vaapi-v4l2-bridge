/*
 * v4l2stateless — V4L2 Request API device and buffer management
 *
 * Handles:
 * - V4L2 device open and capability check
 * - Output queue (compressed bitstream) setup
 * - Capture queue (decoded frames) setup
 * - MEDIA_IOC_REQUEST_ALLOC
 * - Buffer queue/dequeue
 * - DMA-BUF export from capture buffers
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

    /* Set decode mode to frame-based */
    struct v4l2_control ctrl = { 0 };
    ctrl.id = V4L2_CID_STATELESS_H264_DECODE_MODE;
    ctrl.value = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED;
    if (xioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        fprintf(stderr, "v4l2stateless: set decode mode failed: %s\n", strerror(errno));
        /* Non-fatal for non-H264 */
    }

    /* Set start code to Annex B */
    ctrl.id = V4L2_CID_STATELESS_H264_START_CODE;
    ctrl.value = V4L2_STATELESS_H264_START_CODE_ANNEX_B;
    if (xioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        fprintf(stderr, "v4l2stateless: set start code failed: %s\n", strerror(errno));
    }

    /* Request buffers */
    struct v4l2_requestbuffers req = { 0 };
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "v4l2stateless: REQBUFS output failed: %s\n", strerror(errno));
        return -1;
    }

    fprintf(stderr, "v4l2stateless: output queue: %d buffers\n", req.count);
    return 0;
}

/*
 * Setup capture queue (decoded frames output)
 * Returns 0 on success, -1 on error
 */
int v4l2sl_setup_capture_queue(int fd, int width, int height)
{
    /* Set capture format (decoded output) */
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
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "v4l2stateless: REQBUFS capture failed: %s\n", strerror(errno));
        return -1;
    }

    fprintf(stderr, "v4l2stateless: capture queue: %d buffers\n", req.count);
    return 0;
}

/*
 * Queue a compressed bitstream buffer to the output queue
 */
int v4l2sl_queue_output(int fd, int buf_index,
                        const uint8_t *data, uint32_t size,
                        int request_fd, uint64_t timestamp)
{
    /* Set request fd on the output buffer */
    struct v4l2_streamparm parm = { 0 };
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[1] = { 0 };

    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = buf_index;
    buf.length = 1;
    buf.m.planes = planes;
    buf.request_fd = request_fd;
    buf.flags = V4L2_BUF_FLAG_REQUEST_FD;

    /* Get the buffer */
    if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
        fprintf(stderr, "v4l2stateless: QUERYBUF output failed: %s\n", strerror(errno));
        return -1;
    }

    /* Map and copy data */
    void *ptr = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, planes[0].m.mem_offset);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "v4l2stateless: mmap output failed: %s\n", strerror(errno));
        return -1;
    }

    memcpy(ptr, data, size);
    planes[0].bytesused = size;
    munmap(ptr, planes[0].length);

    /* Set timestamp */
    struct timeval tv;
    tv.tv_sec = timestamp / 1000000000ULL;
    tv.tv_usec = (timestamp % 1000000000ULL) / 1000;
    buf.timestamp = tv;

    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "v4l2stateless: QBUF output failed: %s\n", strerror(errno));
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
        fprintf(stderr, "v4l2stateless: QBUF capture failed: %s\n", strerror(errno));
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
        fprintf(stderr, "v4l2stateless: EXPBUF failed: %s\n", strerror(errno));
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
