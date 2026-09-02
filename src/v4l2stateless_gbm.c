/*
 * v4l2stateless_gbm.c — GBM-backed display surfaces.
 *
 * The VPU capture buffers must never be exported (EXPBUF + GPU import is a
 * RK3588 CMA/IOMMU hang). Instead every exported decode surface gets a
 * driver-owned linear GBM bo on the render node (panthor shmem: ordinary
 * system memory). pull_capture copies each decoded frame into it; the
 * export path hands out the classic single-object NV12 dmabuf descriptor
 * (Y at offset 0, UV at offset stride*h) that Chromium's zero-copy GL
 * path requires (vaapi_wrapper rejects num_objects != 1).
 *
 * Platform notes (verified by tests/gbm_probe.c on this box):
 *   - panthor refuses multiplanar YUV gbm bos, so the bo is R8 geometry
 *     with h + ceil(h/2) rows and the NV12 planes live at byte offsets
 *   - Mesa 26.0.8 per-plane GR88 import samples black; never hand out a
 *     GR88-only image as the primary shape
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include "v4l2stateless.h"

static struct gbm_device *g_gbm_dev;
static int g_gbm_fd = -1;
static int g_gbm_failed;

static struct gbm_device *v4l2sl_gbm_device(void)
{
    const char *node;

    if (g_gbm_dev)
        return g_gbm_dev;
    if (g_gbm_failed)
        return NULL;
    node = getenv("V4L2SL_RENDER_NODE");
    if (!node)
        node = "/dev/dri/renderD128";
    g_gbm_fd = open(node, O_RDWR | O_CLOEXEC);
    if (g_gbm_fd < 0) {
        fprintf(stderr, "v4l2stateless: gbm: open %s failed: %s\n",
                node, strerror(errno));
        g_gbm_failed = 1;
        return NULL;
    }
    g_gbm_dev = gbm_create_device(g_gbm_fd);
    if (!g_gbm_dev) {
        fprintf(stderr, "v4l2stateless: gbm_create_device(%s) failed\n", node);
        close(g_gbm_fd);
        g_gbm_fd = -1;
        g_gbm_failed = 1;
    }
    return g_gbm_dev;
}

static void copy_plane(uint8_t *dst, uint32_t dst_stride, const uint8_t *src,
                       uint32_t src_stride, uint32_t width, uint32_t rows)
{
    if (dst_stride == src_stride) {
        memcpy(dst, src, (size_t)dst_stride * rows);
        return;
    }
    for (uint32_t y = 0; y < rows; y++)
        memcpy(dst + (size_t)dst_stride * y, src + (size_t)src_stride * y,
               width);
}

int v4l2sl_gbm_surface_upload(struct v4l2sl_surface *s, const void *src,
                              uint32_t src_stride, uint32_t src_alh)
{
    uint32_t w = s->width, h = s->height;
    uint8_t *dst;
    void *map_data = NULL;
    uint32_t mstride = 0;

    if (!s || !s->gbm_bo || !src || !src_stride)
        return -1;
    dst = gbm_bo_map(s->gbm_bo, 0, 0, w, h + h / 2,
                     GBM_BO_TRANSFER_WRITE, &mstride, &map_data);
    if (!dst) {
        fprintf(stderr, "v4l2stateless: gbm: bo map failed\n");
        return -1;
    }
    copy_plane(dst, mstride, src, src_stride, w, h);
    copy_plane(dst + (size_t)mstride * h, mstride,
               (const uint8_t *)src + (size_t)src_stride * src_alh,
               src_stride, w, h / 2);
    gbm_bo_unmap(s->gbm_bo, map_data);
    return 0;
}

int v4l2sl_gbm_surface_ensure(struct v4l2sl_surface *s)
{
    struct gbm_device *dev;
    uint32_t rows;

    if (!s || s->width == 0 || s->height == 0)
        return -1;
    if (s->gbm_bo)
        return 0;
    /* First release: NV12 only; everything else keeps the clean fallback. */
    if (s->cap_fourcc && s->cap_fourcc != V4L2_PIX_FMT_NV12)
        return -1;
    dev = v4l2sl_gbm_device();
    if (!dev)
        return -1;
    rows = s->height + (s->height + 1) / 2;
    s->gbm_bo = gbm_bo_create(dev, s->width, rows, GBM_FORMAT_R8,
                              GBM_BO_USE_LINEAR);
    if (!s->gbm_bo) {
        fprintf(stderr, "v4l2stateless: gbm: bo create %ux%u failed\n",
                s->width, rows);
        return -1;
    }
    s->gbm_stride = gbm_bo_get_stride_for_plane(s->gbm_bo, 0);
    /* Bring the bo up to date: by export time pull_capture has filled the
     * memfd for this surface (export happens after vaSyncSurface). */
    if (s->dma_buf_fd >= 0 && s->stride && s->aligned_h) {
        uint32_t sz = v4l2sl_capture_plane_size(V4L2_PIX_FMT_NV12,
                                                s->stride, s->aligned_h);
        void *m = mmap(NULL, sz, PROT_READ, MAP_SHARED, s->dma_buf_fd, 0);
        if (m != MAP_FAILED) {
            v4l2sl_gbm_surface_upload(s, m, s->stride, s->aligned_h);
            munmap(m, sz);
        }
    }
    return 0;
}

void v4l2sl_gbm_surface_destroy(struct v4l2sl_surface *s)
{
    if (s && s->gbm_bo) {
        gbm_bo_destroy(s->gbm_bo);
        s->gbm_bo = NULL;
    }
}

VAStatus v4l2sl_surface_fill_prime_gbm(const struct v4l2sl_surface *surf,
                                       uint32_t flags, void *descriptor)
{
    VADRMPRIMESurfaceDescriptor *desc = descriptor;
    uint32_t h, pitch;
    int fd;

    if (!surf || !surf->gbm_bo || !descriptor)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    fd = gbm_bo_get_fd_for_plane(surf->gbm_bo, 0);
    if (fd < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    h = surf->height;
    pitch = surf->gbm_stride;
    memset(desc, 0, sizeof(*desc));
    desc->fourcc = VA_FOURCC_NV12;
    desc->width = surf->width;
    desc->height = h;
    desc->num_objects = 1;
    desc->objects[0].fd = fd;
    desc->objects[0].size = pitch * (h + (h + 1) / 2);
    desc->objects[0].drm_format_modifier = gbm_bo_get_modifier(surf->gbm_bo);

    if (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) {
        desc->num_layers = 2;
        desc->layers[0].drm_format = DRM_FORMAT_R8;
        desc->layers[0].num_planes = 1;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = pitch;
        desc->layers[1].drm_format = DRM_FORMAT_GR88;
        desc->layers[1].num_planes = 1;
        desc->layers[1].object_index[0] = 0;
        desc->layers[1].offset[0] = pitch * h;
        desc->layers[1].pitch[0] = pitch;
    } else {
        desc->num_layers = 1;
        desc->layers[0].drm_format = DRM_FORMAT_NV12;
        desc->layers[0].num_planes = 2;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].object_index[1] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].offset[1] = pitch * h;
        desc->layers[0].pitch[0] = pitch;
        desc->layers[0].pitch[1] = pitch;
    }
    return VA_STATUS_SUCCESS;
}
