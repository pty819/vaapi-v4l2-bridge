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

#define xioctl(fd, req, arg) v4l2sl_xioctl((fd), (req), (arg))

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

/* Borrowed NV12 view of a surface: cpu_ptr directly, or the surface's
 * persistent memfd mapping (never unmapped here). */
static const uint8_t *surface_nv12(struct v4l2sl_surface *s,
                                   uint32_t *stride, uint32_t *alh)
{
    /* Placeholder memfd (buf_index < 0) is for DRM-PRIME export; pixels live
     * in cpu_ptr until a V4L2 capture buffer is bound. */
    if (s->cpu_ptr && s->buf_index < 0) {
        *stride = s->cpu_stride ? s->cpu_stride : s->width;
        *alh = s->height;
        return s->cpu_ptr;
    }
    if (s->memfd_fd >= 0 && s->stride && s->aligned_h) {
        size_t sz = v4l2sl_capture_plane_size(
            s->cap_fourcc ? s->cap_fourcc : V4L2_PIX_FMT_NV12,
            s->stride, s->aligned_h);
        uint8_t *p = v4l2sl_surface_map_memfd(s, sz);

        if (!p)
            return NULL;
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
    uint32_t src_stride = 0, src_alh = 0;
    int w, h;
    struct v4l2_format ofmt, cfmt;
    struct v4l2_requestbuffers oreq, creq;
    struct v4l2_buffer obuf, cbuf;
    struct v4l2_plane oplanes[8], cplanes[8];
    struct v4l2_control ctrl;
    struct pollfd pfd;
    int jpeg_size = 0;
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

    nv12 = surface_nv12(src, &src_stride, &src_alh);
    if (!nv12) {
        fprintf(stderr, "v4l2stateless: JPEG encode: no NV12 source\n");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    {
        struct v4l2sl_m2m_state *q = &ctx->jpeg_q;
        uint32_t key[8] = { (uint32_t)w, (uint32_t)h, (uint32_t)quality,
                            0, 0, 0, 0, 0 };
        int renegot = !q->valid || memcmp(q->key, key, sizeof(key));
        uint32_t ybpl, ubpl;
        int row;

        if (renegot) {
            v4l2sl_m2m_teardown(ctx, q);

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
                    fprintf(stderr, "v4l2stateless: JPEG S_FMT output: %s\n",
                            strerror(errno));
                    st = VA_STATUS_ERROR_OPERATION_FAILED;
                    goto fail;
                }
            }
            xioctl(fd, VIDIOC_G_FMT, &ofmt);
            q->ofmt = ofmt;

            memset(&cfmt, 0, sizeof(cfmt));
            cfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            cfmt.fmt.pix_mp.width = w;
            cfmt.fmt.pix_mp.height = h;
            cfmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
            cfmt.fmt.pix_mp.num_planes = 1;
            if (xioctl(fd, VIDIOC_S_FMT, &cfmt) < 0) {
                fprintf(stderr, "v4l2stateless: JPEG S_FMT capture: %s\n",
                        strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }

            ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
            ctrl.value = quality;
            xioctl(fd, VIDIOC_S_CTRL, &ctrl);
            /* Re-read AFTER the capture side is set: this kernel reports a
             * different OUTPUT plane count once VEPU has both formats
             * (NV12M-out/JPEG-cap queues come back 3-plane). */
            xioctl(fd, VIDIOC_G_FMT, &ofmt);
            q->ofmt = ofmt;

            memset(&oreq, 0, sizeof(oreq));
            oreq.count = 2;
            oreq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            oreq.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_REQBUFS, &oreq) < 0) {
                fprintf(stderr, "v4l2stateless: JPEG REQBUFS out: %s\n",
                        strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            memset(&creq, 0, sizeof(creq));
            creq.count = 2;
            creq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            creq.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_REQBUFS, &creq) < 0) {
                fprintf(stderr, "v4l2stateless: JPEG REQBUFS cap: %s\n",
                        strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }

            memset(&obuf, 0, sizeof(obuf));
            memset(oplanes, 0, sizeof(oplanes));
            obuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            obuf.memory = V4L2_MEMORY_MMAP;
            obuf.index = 0;
            obuf.length = 8;
            obuf.m.planes = oplanes;
            if (xioctl(fd, VIDIOC_QUERYBUF, &obuf) < 0) {
                fprintf(stderr, "v4l2stateless: JPEG QUERYBUF out: %s nplanes=%u\n",
                        strerror(errno), ofmt.fmt.pix_mp.num_planes);
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            /* The kernel overwrites buf.length with the queue's real plane
             * count on QUERYBUF — that is the value QBUF will enforce. */
            if (obuf.length > 0 && obuf.length <= 8)
                q->out_planes = obuf.length;
            q->out_len[0] = oplanes[0].length;
            q->out_map[0] = mmap(NULL, oplanes[0].length,
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                 oplanes[0].m.mem_offset);
            /* Pixels only ever live in planes 0 (Y) and 1 (UV); a third
             * plane slot is left unmapped, as before. */
            if (ofmt.fmt.pix_mp.num_planes >= 2) {
                q->out_len[1] = oplanes[1].length;
                q->out_map[1] = mmap(NULL, oplanes[1].length,
                                     PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                     oplanes[1].m.mem_offset);
            }
            if (q->out_map[0] == MAP_FAILED ||
                (ofmt.fmt.pix_mp.num_planes >= 2 &&
                 q->out_map[1] == MAP_FAILED)) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }

            memset(&cbuf, 0, sizeof(cbuf));
            memset(cplanes, 0, sizeof(cplanes));
            cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            cbuf.memory = V4L2_MEMORY_MMAP;
            cbuf.index = 0;
            cbuf.length = 1;
            cbuf.m.planes = cplanes;
            if (xioctl(fd, VIDIOC_QUERYBUF, &cbuf) < 0) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            q->cap_len = cplanes[0].length;
            q->cap_map = mmap(NULL, cplanes[0].length, PROT_READ,
                              MAP_SHARED, fd, cplanes[0].m.mem_offset);
            if (q->cap_map == MAP_FAILED) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            /* out_planes holds the QUERYBUF writeback (authoritative);
             * fall back to the G_FMT value only if that was unavailable. */
            if (q->out_planes <= 0)
                q->out_planes = ofmt.fmt.pix_mp.num_planes;
            memcpy(q->key, key, sizeof(key));
            q->valid = 1;
        }

        ofmt = q->ofmt;
        ybpl = ofmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        if (!ybpl)
            ybpl = w;
        for (row = 0; row < h; row++)
            memcpy((uint8_t *)q->out_map[0] + (size_t)row * ybpl,
                   nv12 + (size_t)row * src_stride, (size_t)w);
        if (q->out_planes >= 2) {
            ubpl = ofmt.fmt.pix_mp.plane_fmt[1].bytesperline;
            if (!ubpl)
                ubpl = w;
            for (row = 0; row < h / 2; row++)
                memcpy((uint8_t *)q->out_map[1] + (size_t)row * ubpl,
                       nv12 + (size_t)src_stride * src_alh +
                           (size_t)row * src_stride,
                       (size_t)w);
        } else {
            const uint8_t *suv = nv12 + (size_t)src_stride * src_alh;
            uint8_t *duv = (uint8_t *)q->out_map[0] + (size_t)ybpl * h;

            for (row = 0; row < h / 2; row++)
                memcpy(duv + (size_t)row * ybpl,
                       suv + (size_t)row * src_stride, (size_t)w);
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
            goto fail;
        }
        memset(&obuf, 0, sizeof(obuf));
        memset(oplanes, 0, sizeof(oplanes));
        obuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        obuf.memory = V4L2_MEMORY_MMAP;
        obuf.index = 0;
        obuf.length = q->out_planes;
        obuf.m.planes = oplanes;
        oplanes[0].bytesused = ofmt.fmt.pix_mp.plane_fmt[0].sizeimage
            ? ofmt.fmt.pix_mp.plane_fmt[0].sizeimage : q->out_len[0];
        oplanes[0].length = q->out_len[0];
        if (q->out_planes >= 2) {
            oplanes[1].bytesused = ofmt.fmt.pix_mp.plane_fmt[1].sizeimage
                ? ofmt.fmt.pix_mp.plane_fmt[1].sizeimage : q->out_len[1];
            oplanes[1].length = q->out_len[1];
        }
        if (xioctl(fd, VIDIOC_QBUF, &obuf) < 0) {
            fprintf(stderr, "v4l2stateless: JPEG QBUF out: %s (planes=%d yused=%u)\n",
                    strerror(errno), q->out_planes, oplanes[0].bytesused);
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }
        {
            enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

            if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) {
                fprintf(stderr, "v4l2stateless: JPEG STREAMON out: %s\n",
                        strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) {
                fprintf(stderr, "v4l2stateless: JPEG STREAMON cap: %s\n",
                        strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
        }

        pfd.fd = fd;
        pfd.events = POLLIN | POLLOUT;
        if (v4l2sl_poll_intr(&pfd, 1, 2000) <= 0) {
            fprintf(stderr, "v4l2stateless: JPEG encode timeout\n");
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
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
            goto fail;
        }
        jpeg_size = (int)cplanes[0].bytesused;
        {
            uint32_t cap = coded->size;

            if (sizeof(*seg) < cap)
                cap -= (uint32_t)sizeof(*seg);
            if (jpeg_size > (int)cap)
                jpeg_size = (int)cap;
            memcpy(seg->buf, q->cap_map, (size_t)jpeg_size);
            seg->size = (uint32_t)jpeg_size;
            seg->bit_offset = 0;
            seg->status = 0;
            seg->next = NULL;
        }
    }

    st = VA_STATUS_SUCCESS;
    /* Keep the queue: per-frame STREAMOFF returns the buffers while the
     * allocation and mappings stay valid. */
    {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

        xioctl(fd, VIDIOC_STREAMOFF, &t);
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(fd, VIDIOC_STREAMOFF, &t);
    }
    if (getenv("V4L2SL_DEBUG"))
        fprintf(stderr, "v4l2stateless: JPEG encoded %dx%d quality=%d bytes=%d planes=%d\n",
                w, h, quality, jpeg_size, ctx->jpeg_q.out_planes);
    return st;

fail:
    v4l2sl_m2m_teardown(ctx, &ctx->jpeg_q);
    return st;
}
