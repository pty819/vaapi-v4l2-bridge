/*
 * JPEG encode via hantro VEPU121 (/dev/video3). Stateful M2M, not request API.
 *
 * Input is the BeginPicture surface (NV12 cpu backing or decode dma-buf).
 * Output lands in VAEncCodedBufferType as a VACodedBufferSegment list.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>
#include <va/va.h>
#include <va/va_enc_jpeg.h>

#include "v4l2stateless.h"

static int xioctl(int fd, unsigned long r, void *p)
{
    int n;
    do {
        n = ioctl(fd, r, p);
    } while (n < 0 && errno == EINTR);
    return n;
}

static struct v4l2sl_buffer *
buffer_by_id(struct v4l2sl_context *ctx, VABufferID id)
{
    struct v4l2sl_buffer *b = ctx->buffers;
    while (b) {
        if (b->buffer_id == id)
            return b;
        b = b->next;
    }
    return NULL;
}

static const uint8_t *surface_nv12(struct v4l2sl_surface *s, size_t *map_size,
                                   void **to_unmap, uint32_t *stride, uint32_t *alh)
{
    *to_unmap = NULL;
    *map_size = 0;
    /* Placeholder memfd (buf_index < 0) is for DRM-PRIME export; pixels live
     * in cpu_ptr until a V4L2 capture buffer is bound. */
    if (s->cpu_ptr && s->buf_index < 0) {
        *stride = s->cpu_stride ? s->cpu_stride : s->width;
        *alh = s->height;
        return s->cpu_ptr;
    }
    if (s->dma_buf_fd >= 0 && s->stride && s->aligned_h) {
        size_t sz = v4l2sl_capture_plane_size(
            s->cap_fourcc ? s->cap_fourcc : V4L2_PIX_FMT_NV12,
            s->stride, s->aligned_h);
        uint8_t *p = mmap(NULL, sz, PROT_READ, MAP_SHARED, s->dma_buf_fd, 0);
        if (p == MAP_FAILED)
            return NULL;
        *to_unmap = p;
        *map_size = sz;
        *stride = s->stride;
        *alh = s->aligned_h;
        return p;
    }
    if (s->cpu_ptr) {
        *stride = s->cpu_stride ? s->cpu_stride : s->width;
        *alh = s->height;
        return s->cpu_ptr;
    }
    return NULL;
}

VAStatus v4l2sl_jpeg_encode(struct v4l2sl_context *ctx,
                            struct v4l2sl_buffer **buffers, int num_buffers)
{
    VAEncPictureParameterBufferJPEG *pic = NULL;
    int i, quality = 50;
    int fd = ctx->v4l2_fd;
    struct v4l2sl_surface *src = ctx->current_surface;
    struct v4l2sl_buffer *coded = NULL;
    VACodedBufferSegment *seg;
    const uint8_t *nv12;
    void *mapped = NULL;
    size_t map_size = 0;
    uint32_t src_stride = 0, src_alh = 0;
    int w, h;
    struct v4l2_format ofmt, cfmt;
    struct v4l2_requestbuffers oreq, creq;
    struct v4l2_buffer obuf, cbuf;
    struct v4l2_plane oplanes[8], cplanes[8];
    struct v4l2_control ctrl;
    struct pollfd pfd;
    void *y_ptr, *uv_ptr;
    uint32_t y_len, uv_len;
    int jpeg_size;
    VAStatus st;

    for (i = 0; i < num_buffers; i++) {
        if (!buffers[i] || !buffers[i]->data)
            continue;
        if (buffers[i]->type == VAEncPictureParameterBufferType)
            pic = buffers[i]->data;
        else if (buffers[i]->type == VAEncMiscParameterBufferType) {
            VAEncMiscParameterBuffer *m = buffers[i]->data;
            if (m->type == VAEncMiscParameterTypeQualityLevel) {
                VAEncMiscParameterBufferQualityLevel *q =
                    (VAEncMiscParameterBufferQualityLevel *)m->data;
                if (q->quality_level)
                    quality = (int)q->quality_level;
            }
        }
    }
    if (!pic || !src) {
        fprintf(stderr, "v4l2stateless: JPEG missing pic=%p src=%p nbuf=%d\n",
                (void *)pic, (void *)src, num_buffers);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (pic->quality)
        quality = pic->quality;
    if (quality < 5)
        quality = 5;
    if (quality > 100)
        quality = 100;

    coded = buffer_by_id(ctx, pic->coded_buf);
    if (!coded || !coded->data) {
        struct v4l2sl_buffer *b;
        fprintf(stderr, "v4l2stateless: JPEG coded_buf %u not found (ctx buffers:",
                pic->coded_buf);
        for (b = ctx->buffers; b; b = b->next)
            fprintf(stderr, " %u/t%u", b->buffer_id, b->type);
        fprintf(stderr, ")\n");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    seg = coded->data;

    w = pic->picture_width ? pic->picture_width : src->width;
    h = pic->picture_height ? pic->picture_height : src->height;

    nv12 = surface_nv12(src, &map_size, &mapped, &src_stride, &src_alh);
    if (!nv12) {
        fprintf(stderr, "v4l2stateless: JPEG encode: no NV12 source\n");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    memset(&ofmt, 0, sizeof(ofmt));
    ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ofmt.fmt.pix_mp.width = w;
    ofmt.fmt.pix_mp.height = h;
    ofmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
    ofmt.fmt.pix_mp.num_planes = 2;
    ofmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &ofmt) < 0) {
        ofmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        ofmt.fmt.pix_mp.num_planes = 1;
        if (xioctl(fd, VIDIOC_S_FMT, &ofmt) < 0) {
            fprintf(stderr, "v4l2stateless: JPEG S_FMT output: %s\n", strerror(errno));
            if (mapped)
                munmap(mapped, map_size);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    xioctl(fd, VIDIOC_G_FMT, &ofmt);

    memset(&cfmt, 0, sizeof(cfmt));
    cfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cfmt.fmt.pix_mp.width = w;
    cfmt.fmt.pix_mp.height = h;
    cfmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
    cfmt.fmt.pix_mp.num_planes = 1;
    if (xioctl(fd, VIDIOC_S_FMT, &cfmt) < 0) {
        fprintf(stderr, "v4l2stateless: JPEG S_FMT capture: %s\n", strerror(errno));
        if (mapped)
            munmap(mapped, map_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
    ctrl.value = quality;
    xioctl(fd, VIDIOC_S_CTRL, &ctrl);

    xioctl(fd, VIDIOC_G_FMT, &ofmt);
    memset(&oreq, 0, sizeof(oreq));
    oreq.count = 2;
    oreq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    oreq.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &oreq) < 0) {
        fprintf(stderr, "v4l2stateless: JPEG REQBUFS out: %s\n", strerror(errno));
        if (mapped)
            munmap(mapped, map_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    memset(&creq, 0, sizeof(creq));
    creq.count = 2;
    creq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    creq.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &creq) < 0) {
        fprintf(stderr, "v4l2stateless: JPEG REQBUFS cap: %s\n", strerror(errno));
        if (mapped)
            munmap(mapped, map_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    memset(&obuf, 0, sizeof(obuf));
    memset(oplanes, 0, sizeof(oplanes));
    obuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    obuf.memory = V4L2_MEMORY_MMAP;
    obuf.index = 0;
    obuf.length = 8;
    obuf.m.planes = oplanes;
    if (xioctl(fd, VIDIOC_QUERYBUF, &obuf) < 0) {
        fprintf(stderr, "v4l2stateless: JPEG QUERYBUF out: %s nplanes=%u req=%d\n",
                strerror(errno), ofmt.fmt.pix_mp.num_planes, oreq.count);
        if (mapped)
            munmap(mapped, map_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    y_len = oplanes[0].length;
    y_ptr = mmap(NULL, y_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 oplanes[0].m.mem_offset);
    uv_ptr = NULL;
    uv_len = 0;
    if (ofmt.fmt.pix_mp.num_planes >= 2) {
        uv_len = oplanes[1].length;
        uv_ptr = mmap(NULL, uv_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                      oplanes[1].m.mem_offset);
    }
    if (y_ptr == MAP_FAILED || (ofmt.fmt.pix_mp.num_planes >= 2 && uv_ptr == MAP_FAILED)) {
        if (y_ptr != MAP_FAILED)
            munmap(y_ptr, y_len);
        if (mapped)
            munmap(mapped, map_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    {
        uint32_t ybpl = ofmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        int row;
        if (!ybpl)
            ybpl = w;
        for (row = 0; row < h; row++)
            memcpy((uint8_t *)y_ptr + (size_t)row * ybpl,
                   nv12 + (size_t)row * src_stride, (size_t)w);
        if (uv_ptr) {
            uint32_t ubpl = ofmt.fmt.pix_mp.plane_fmt[1].bytesperline;
            const uint8_t *suv = nv12 + (size_t)src_stride * src_alh;
            if (!ubpl)
                ubpl = w;
            for (row = 0; row < h / 2; row++)
                memcpy((uint8_t *)uv_ptr + (size_t)row * ubpl,
                       suv + (size_t)row * src_stride, (size_t)w);
        } else {
            const uint8_t *suv = nv12 + (size_t)src_stride * src_alh;
            uint8_t *duv = (uint8_t *)y_ptr + (size_t)ybpl * h;
            for (row = 0; row < h / 2; row++)
                memcpy(duv + (size_t)row * ybpl,
                       suv + (size_t)row * src_stride, (size_t)w);
        }
    }

    memset(&cbuf, 0, sizeof(cbuf));
    memset(cplanes, 0, sizeof(cplanes));
    cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cbuf.memory = V4L2_MEMORY_MMAP;
    cbuf.index = 0;
    cbuf.length = 1;
    cbuf.m.planes = cplanes;
    if (xioctl(fd, VIDIOC_QUERYBUF, &cbuf) < 0) {
        munmap(y_ptr, y_len);
        if (uv_ptr)
            munmap(uv_ptr, uv_len);
        if (mapped)
            munmap(mapped, map_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    memset(&cbuf, 0, sizeof(cbuf));
    memset(cplanes, 0, sizeof(cplanes));
    cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cbuf.memory = V4L2_MEMORY_MMAP;
    cbuf.index = 0;
    cbuf.length = 1;
    cbuf.m.planes = cplanes;
    if (xioctl(fd, VIDIOC_QBUF, &cbuf) < 0) {
        fprintf(stderr, "v4l2stateless: JPEG QBUF cap: %s\n", strerror(errno));
        st = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }
    memset(&obuf, 0, sizeof(obuf));
    memset(oplanes, 0, sizeof(oplanes));
    obuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    obuf.memory = V4L2_MEMORY_MMAP;
    obuf.index = 0;
    obuf.length = ofmt.fmt.pix_mp.num_planes;
    obuf.m.planes = oplanes;
    oplanes[0].bytesused = ofmt.fmt.pix_mp.plane_fmt[0].sizeimage
        ? ofmt.fmt.pix_mp.plane_fmt[0].sizeimage : y_len;
    oplanes[0].length = y_len;
    if (ofmt.fmt.pix_mp.num_planes >= 2) {
        oplanes[1].bytesused = ofmt.fmt.pix_mp.plane_fmt[1].sizeimage
            ? ofmt.fmt.pix_mp.plane_fmt[1].sizeimage : uv_len;
        oplanes[1].length = uv_len;
    }
    if (xioctl(fd, VIDIOC_QBUF, &obuf) < 0) {
        fprintf(stderr, "v4l2stateless: JPEG QBUF out: %s (planes=%u yused=%u)\n",
                strerror(errno), ofmt.fmt.pix_mp.num_planes, oplanes[0].bytesused);
        st = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }
    {
        enum v4l2_buf_type t;
        t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) {
            fprintf(stderr, "v4l2stateless: JPEG STREAMON out: %s\n", strerror(errno));
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto out;
        }
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) {
            fprintf(stderr, "v4l2stateless: JPEG STREAMON cap: %s\n", strerror(errno));
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto out;
        }
    }

    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;
    if (v4l2sl_poll_intr(&pfd, 1, 2000) <= 0) {
        fprintf(stderr, "v4l2stateless: JPEG encode timeout\n");
        st = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }

    memset(&cbuf, 0, sizeof(cbuf));
    memset(cplanes, 0, sizeof(cplanes));
    cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cbuf.memory = V4L2_MEMORY_MMAP;
    cbuf.length = 1;
    cbuf.m.planes = cplanes;
    if (xioctl(fd, VIDIOC_DQBUF, &cbuf) < 0) {
        fprintf(stderr, "v4l2stateless: JPEG DQBUF: %s\n", strerror(errno));
        st = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }
    jpeg_size = (int)cplanes[0].bytesused;
    {
        void *jp = mmap(NULL, cplanes[0].length, PROT_READ, MAP_SHARED, fd,
                        cplanes[0].m.mem_offset);
        uint32_t cap = coded->size;
        if (sizeof(*seg) < cap)
            cap -= (uint32_t)sizeof(*seg);
        if (jp == MAP_FAILED) {
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto out;
        }
        if (jpeg_size > (int)cap)
            jpeg_size = (int)cap;
        memcpy(seg->buf, jp, (size_t)jpeg_size);
        seg->size = (uint32_t)jpeg_size;
        seg->bit_offset = 0;
        seg->status = 0;
        seg->next = NULL;
        munmap(jp, cplanes[0].length);
    }

    st = VA_STATUS_SUCCESS;

out:
    if (y_ptr && y_ptr != MAP_FAILED)
        munmap(y_ptr, y_len);
    if (uv_ptr && uv_ptr != MAP_FAILED)
        munmap(uv_ptr, uv_len);
    if (mapped)
        munmap(mapped, map_size);
    /* Always tear down: leaving VEPU streaming wedges every later call on
     * this fd. */
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
        fprintf(stderr, "v4l2stateless: JPEG encoded %dx%d quality=%d bytes=%d planes=%u\n",
                w, h, quality, jpeg_size, ofmt.fmt.pix_mp.num_planes);
    return st;
}
