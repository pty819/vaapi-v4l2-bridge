/*
 * VPP via rockchip RGA (/dev/video0): scale, CSC, rotate, flip.
 * Stateful M2M. Filters the kernel does not expose (denoise, deinterlace)
 * are reported as unsupported.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>
#include <va/va.h>
#include <va/va_vpp.h>

#include "v4l2stateless.h"

static int xioctl(int fd, unsigned long r, void *p)
{
    int n;
    do {
        n = ioctl(fd, r, p);
    } while (n < 0 && errno == EINTR);
    return n;
}

static uint32_t va_to_v4l2_raw(uint32_t fourcc)
{
    switch (fourcc) {
    case VA_FOURCC_NV12:
        return V4L2_PIX_FMT_NV12;
    case VA_FOURCC_YUY2:
        return V4L2_PIX_FMT_YUYV;
    case VA_FOURCC_I420:
        return V4L2_PIX_FMT_YUV420;
    case VA_FOURCC_ARGB:
        return V4L2_PIX_FMT_ARGB32;
    case VA_FOURCC_BGRA:
        return V4L2_PIX_FMT_ABGR32;
    case VA_FOURCC_BGRX:
        return V4L2_PIX_FMT_XBGR32;
    default:
        return V4L2_PIX_FMT_NV12;
    }
}

static struct v4l2sl_surface *
surf(struct v4l2sl_driver_data *dd, VASurfaceID id)
{
    if (!dd || id == VA_INVALID_ID || (unsigned)id >= 4096)
        return NULL;
    return dd->surfaces[id];
}

static const uint32_t vpp_pixel_formats[] = {
    VA_FOURCC_NV12,
    VA_FOURCC_YUY2,
    VA_FOURCC_I420,
    VA_FOURCC_BGRX,
    VA_FOURCC_BGRA,
    VA_FOURCC_ARGB,
};

static const VAProcColorStandardType vpp_color_in[] = {
    VAProcColorStandardBT601,
    VAProcColorStandardBT709,
};
static const VAProcColorStandardType vpp_color_out[] = {
    VAProcColorStandardBT601,
    VAProcColorStandardBT709,
};

VAStatus v4l2sl_vpp_query_filters(VAProcFilterType *filters, unsigned int *n)
{
    /* RGA has rotate/flip as controls, not VAProcFilter* types. */
    if (!n)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!filters) {
        *n = 0;
        return VA_STATUS_SUCCESS;
    }
    *n = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus v4l2sl_vpp_query_filter_caps(VAProcFilterType type, void *caps,
                                      unsigned int *n)
{
    (void)type;
    (void)caps;
    if (n)
        *n = 0;
    return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
}

VAStatus v4l2sl_vpp_query_pipeline_caps(VAProcPipelineCaps *caps)
{
    if (!caps)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    memset(caps, 0, sizeof(*caps));
    caps->rotation_flags = (1u << VA_ROTATION_NONE) | (1u << VA_ROTATION_90) |
                           (1u << VA_ROTATION_180) | (1u << VA_ROTATION_270);
    caps->mirror_flags = VA_MIRROR_HORIZONTAL | VA_MIRROR_VERTICAL;
    caps->num_input_color_standards = 2;
    caps->input_color_standards = (VAProcColorStandardType *)vpp_color_in;
    caps->num_output_color_standards = 2;
    caps->output_color_standards = (VAProcColorStandardType *)vpp_color_out;
    caps->num_input_pixel_formats = (uint32_t)(sizeof(vpp_pixel_formats) / sizeof(vpp_pixel_formats[0]));
    caps->input_pixel_format = (uint32_t *)vpp_pixel_formats;
    caps->num_output_pixel_formats = caps->num_input_pixel_formats;
    caps->output_pixel_format = (uint32_t *)vpp_pixel_formats;
    caps->min_input_width = 2;
    caps->min_input_height = 2;
    caps->max_input_width = 8192;
    caps->max_input_height = 8192;
    caps->min_output_width = 2;
    caps->min_output_height = 2;
    caps->max_output_width = 8192;
    caps->max_output_height = 8192;
    return VA_STATUS_SUCCESS;
}

static int mmap_copy_plane(int fd, struct v4l2_plane *pl, int write_not_read,
                           uint8_t *cpu, uint32_t cpu_stride, int width, int height,
                           int bpp)
{
    void *p = mmap(NULL, pl->length,
                   write_not_read ? PROT_READ | PROT_WRITE : PROT_READ,
                   MAP_SHARED, fd, pl->m.mem_offset);
    int y;
    uint32_t bpl = (uint32_t)width * (uint32_t)bpp;
    if (p == MAP_FAILED)
        return -1;
    if (write_not_read) {
        for (y = 0; y < height; y++)
            memcpy((uint8_t *)p + (size_t)y * bpl,
                   cpu + (size_t)y * cpu_stride, (size_t)width * bpp);
    } else {
        for (y = 0; y < height; y++)
            memcpy(cpu + (size_t)y * cpu_stride,
                   (uint8_t *)p + (size_t)y * bpl, (size_t)width * bpp);
    }
    munmap(p, pl->length);
    return 0;
}

VAStatus v4l2sl_vpp_run(struct v4l2sl_context *ctx,
                        struct v4l2sl_buffer **buffers, int num_buffers)
{
    VAProcPipelineParameterBuffer *pipe = NULL;
    struct v4l2sl_surface *src, *dst;
    int fd = ctx->v4l2_fd;
    int i, rotate = 0, hflip = 0, vflip = 0;
    int sw, sh, dw, dh;
    uint32_t src_fcc, dst_fcc;
    struct v4l2_format ofmt, cfmt;
    struct v4l2_requestbuffers oreq, creq;
    struct v4l2_buffer obuf, cbuf;
    struct v4l2_plane opl[1], cpl[1];
    struct v4l2_control ctrl;
    struct pollfd pfd;
    const uint8_t *srcp = NULL;
    void *src_map = NULL;
    size_t src_map_sz = 0;
    uint32_t src_stride, src_alh;
    uint8_t *dstp;
    int bpp_src = 1, bpp_dst = 1;
    VAStatus st;

    for (i = 0; i < num_buffers; i++) {
        if (buffers[i] && buffers[i]->type == VAProcPipelineParameterBufferType)
            pipe = buffers[i]->data;
    }
    if (!pipe)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    src = surf(ctx->driver_data, pipe->surface);
    dst = ctx->current_surface;
    if (!src || !dst)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    sw = src->width;
    sh = src->height;
    dw = dst->width;
    dh = dst->height;
    if (pipe->surface_region) {
        sw = pipe->surface_region->width;
        sh = pipe->surface_region->height;
    }
    if (pipe->output_region) {
        dw = pipe->output_region->width;
        dh = pipe->output_region->height;
    }
    if (pipe->rotation_state == VA_ROTATION_90 ||
        pipe->rotation_state == VA_ROTATION_270)
        rotate = (pipe->rotation_state == VA_ROTATION_90) ? 90 : 270;
    else if (pipe->rotation_state == VA_ROTATION_180)
        rotate = 180;
    if (pipe->mirror_state & VA_MIRROR_HORIZONTAL)
        hflip = 1;
    if (pipe->mirror_state & VA_MIRROR_VERTICAL)
        vflip = 1;

    src_fcc = src->format ? src->format : VA_FOURCC_NV12;
    dst_fcc = dst->format ? dst->format : VA_FOURCC_NV12;
    if (src_fcc == VA_FOURCC_P010)
        src_fcc = VA_FOURCC_NV12; /* RGA has no 10-bit; caller should convert */
    bpp_src = (src_fcc == VA_FOURCC_YUY2) ? 2 :
              (src_fcc == VA_FOURCC_ARGB || src_fcc == VA_FOURCC_BGRA ||
               src_fcc == VA_FOURCC_BGRX) ? 4 : 1;
    bpp_dst = (dst_fcc == VA_FOURCC_YUY2) ? 2 :
              (dst_fcc == VA_FOURCC_ARGB || dst_fcc == VA_FOURCC_BGRA ||
               dst_fcc == VA_FOURCC_BGRX) ? 4 : 1;

    if (src->dma_buf_fd >= 0 && src->buf_index >= 0 && src->stride) {
        src_map_sz = v4l2sl_capture_plane_size(
            src->cap_fourcc ? src->cap_fourcc : V4L2_PIX_FMT_NV12,
            src->stride, src->aligned_h ? src->aligned_h : src->height);
        src_map = mmap(NULL, src_map_sz, PROT_READ, MAP_SHARED, src->dma_buf_fd, 0);
        if (src_map == MAP_FAILED)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        srcp = src_map;
        src_stride = src->stride;
        src_alh = src->aligned_h ? src->aligned_h : src->height;
    } else if (src->cpu_ptr) {
        srcp = src->cpu_ptr;
        src_stride = src->cpu_stride ? src->cpu_stride : src->width * bpp_src;
        src_alh = src->height;
    } else {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (!dst->cpu_ptr) {
        dst->cpu_stride = v4l2sl_default_image_stride(dst_fcc, dst->width);
        dst->cpu_size = v4l2sl_va_image_size(dst_fcc, dst->cpu_stride, dst->height);
        dst->cpu_ptr = calloc(1, dst->cpu_size);
        if (!dst->cpu_ptr) {
            if (src_map)
                munmap(src_map, src_map_sz);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        dst->format = dst_fcc;
    }
    dstp = dst->cpu_ptr;

    memset(&ofmt, 0, sizeof(ofmt));
    ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ofmt.fmt.pix_mp.width = src->width;
    ofmt.fmt.pix_mp.height = src->height;
    ofmt.fmt.pix_mp.pixelformat = va_to_v4l2_raw(src_fcc);
    ofmt.fmt.pix_mp.num_planes = 1;
    ofmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &ofmt) < 0) {
        fprintf(stderr, "v4l2stateless: VPP S_FMT out: %s\n", strerror(errno));
        st = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }
    memset(&cfmt, 0, sizeof(cfmt));
    cfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cfmt.fmt.pix_mp.width = dw;
    cfmt.fmt.pix_mp.height = dh;
    cfmt.fmt.pix_mp.pixelformat = va_to_v4l2_raw(dst_fcc);
    cfmt.fmt.pix_mp.num_planes = 1;
    cfmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &cfmt) < 0) {
        fprintf(stderr, "v4l2stateless: VPP S_FMT cap: %s\n", strerror(errno));
        st = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }

    ctrl.id = V4L2_CID_ROTATE;
    ctrl.value = rotate;
    xioctl(fd, VIDIOC_S_CTRL, &ctrl);
    ctrl.id = V4L2_CID_HFLIP;
    ctrl.value = hflip;
    xioctl(fd, VIDIOC_S_CTRL, &ctrl);
    ctrl.id = V4L2_CID_VFLIP;
    ctrl.value = vflip;
    xioctl(fd, VIDIOC_S_CTRL, &ctrl);

    memset(&oreq, 0, sizeof(oreq));
    oreq.count = 1;
    oreq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    oreq.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &oreq) < 0) {
        st = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }
    memset(&creq, 0, sizeof(creq));
    creq.count = 1;
    creq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    creq.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &creq) < 0) {
        st = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }

    memset(&obuf, 0, sizeof(obuf));
    memset(opl, 0, sizeof(opl));
    obuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    obuf.memory = V4L2_MEMORY_MMAP;
    obuf.index = 0;
    obuf.length = 1;
    obuf.m.planes = opl;
    xioctl(fd, VIDIOC_QUERYBUF, &obuf);
    {
        int copy_h = src->height;
        if (src_fcc == VA_FOURCC_NV12 || src_fcc == VA_FOURCC_I420)
            copy_h = src->height * 3 / 2;
        mmap_copy_plane(fd, &opl[0], 1, (uint8_t *)srcp, src_stride,
                        src->width, copy_h, bpp_src);
        (void)src_alh;
        (void)sw;
        (void)sh;
    }

    memset(&cbuf, 0, sizeof(cbuf));
    memset(cpl, 0, sizeof(cpl));
    cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cbuf.memory = V4L2_MEMORY_MMAP;
    cbuf.index = 0;
    cbuf.length = 1;
    cbuf.m.planes = cpl;
    xioctl(fd, VIDIOC_QUERYBUF, &cbuf);

    {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        xioctl(fd, VIDIOC_STREAMON, &t);
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(fd, VIDIOC_STREAMON, &t);
    }
    memset(&cbuf, 0, sizeof(cbuf));
    memset(cpl, 0, sizeof(cpl));
    cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cbuf.memory = V4L2_MEMORY_MMAP;
    cbuf.index = 0;
    cbuf.length = 1;
    cbuf.m.planes = cpl;
    xioctl(fd, VIDIOC_QBUF, &cbuf);
    memset(&obuf, 0, sizeof(obuf));
    memset(opl, 0, sizeof(opl));
    obuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    obuf.memory = V4L2_MEMORY_MMAP;
    obuf.index = 0;
    obuf.length = 1;
    obuf.m.planes = opl;
    xioctl(fd, VIDIOC_QBUF, &obuf);

    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;
    if (poll(&pfd, 1, 2000) <= 0) {
        fprintf(stderr, "v4l2stateless: VPP timeout\n");
        st = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }
    memset(&cbuf, 0, sizeof(cbuf));
    memset(cpl, 0, sizeof(cpl));
    cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cbuf.memory = V4L2_MEMORY_MMAP;
    cbuf.length = 1;
    cbuf.m.planes = cpl;
    if (xioctl(fd, VIDIOC_DQBUF, &cbuf) < 0) {
        fprintf(stderr, "v4l2stateless: VPP DQBUF: %s\n", strerror(errno));
        st = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }
    {
        int copy_h = dh;
        if (dst_fcc == VA_FOURCC_NV12 || dst_fcc == VA_FOURCC_I420)
            copy_h = dh * 3 / 2;
        mmap_copy_plane(fd, &cpl[0], 0, dstp,
                        dst->cpu_stride ? dst->cpu_stride : (uint32_t)dw * bpp_dst,
                        dw, copy_h, bpp_dst);
    }

    st = VA_STATUS_SUCCESS;

out:
    if (src_map)
        munmap(src_map, src_map_sz);
    /* Always tear down: leaving RGA streaming wedges every later call on
     * this fd (S_FMT/REQBUFS fail with EBUSY). */
    {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        xioctl(fd, VIDIOC_STREAMOFF, &t);
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(fd, VIDIOC_STREAMOFF, &t);
        oreq.count = 0;
        xioctl(fd, VIDIOC_REQBUFS, &oreq);
        creq.count = 0;
        xioctl(fd, VIDIOC_REQBUFS, &creq);
    }
    if (st == VA_STATUS_SUCCESS)
        fprintf(stderr, "v4l2stateless: VPP %dx%d -> %dx%d rot=%d\n",
                src->width, src->height, dw, dh, rotate);
    return st;
}
