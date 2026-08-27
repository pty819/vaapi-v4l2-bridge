/*
 * v4l2stateless — VA-API to V4L2 Request API bridge driver
 *
 * Copyright 2026 — MIT License
 *
 * This file implements the libva driver VTable. It translates VA-API calls
 * into V4L2 Request API operations on the RK3588 rkvdec/hantro hardware.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_drmcommon.h>

#include "v4l2stateless.h"
#include "v4l2stateless_probe.h"

/* Global driver-wide lock — statically initialized, immune to struct-layout
 * or init-order issues; libva does not serialize client threads. */
static pthread_mutex_t g_v4l2sl_lock = PTHREAD_MUTEX_INITIALIZER;

/* Supported profiles */
static const VAProfile v4l2sl_profiles[] = {
    VAProfileH264Main,
    VAProfileH264High,
    VAProfileHEVCMain,
    VAProfileHEVCMain10,
    VAProfileAV1Profile0,
};

#define NUM_PROFILES (sizeof(v4l2sl_profiles) / sizeof(v4l2sl_profiles[0]))

/* Supported entrypoints — decode only */
static const VAEntrypoint v4l2sl_entrypoints[] = {
    VAEntrypointVLD,
};

#define NUM_ENTRYPOINTS (sizeof(v4l2sl_entrypoints) / sizeof(v4l2sl_entrypoints[0]))

/* RT formats per profile */
static const struct {
    VAProfile profile;
    unsigned int rt_format;
} profile_rt_formats[] = {
    { VAProfileH264Main,       VA_RT_FORMAT_YUV420 },
    { VAProfileH264High,       VA_RT_FORMAT_YUV420 },
    { VAProfileHEVCMain,       VA_RT_FORMAT_YUV420 },
    { VAProfileHEVCMain10,     VA_RT_FORMAT_YUV420_10 },
    { VAProfileAV1Profile0,    VA_RT_FORMAT_YUV420 },
};

/* Map VA profile to codec. Device path is resolved by OUTPUT fourcc. */
static const struct {
    VAProfile profile;
    enum v4l2sl_codec codec;
} profile_codec_map[] = {
    { VAProfileH264Main,    V4L2SL_CODEC_H264 },
    { VAProfileH264High,    V4L2SL_CODEC_H264 },
    { VAProfileHEVCMain,    V4L2SL_CODEC_HEVC },
    { VAProfileHEVCMain10,  V4L2SL_CODEC_HEVC },
    { VAProfileAV1Profile0, V4L2SL_CODEC_AV1  },
};

static int codec_for_profile(VAProfile profile, enum v4l2sl_codec *codec)
{
    unsigned i;

    for (i = 0; i < sizeof(profile_codec_map) / sizeof(profile_codec_map[0]); i++) {
        if (profile_codec_map[i].profile == profile) {
            *codec = profile_codec_map[i].codec;
            return 0;
        }
    }
    return -1;
}

static const char *cached_device(struct v4l2sl_driver_data *dd, enum v4l2sl_codec codec)
{
    const char *p = NULL;

    switch (codec) {
    case V4L2SL_CODEC_H264: p = dd->dev_h264; break;
    case V4L2SL_CODEC_HEVC: p = dd->dev_hevc; break;
    case V4L2SL_CODEC_AV1:  p = dd->dev_av1;  break;
    default: break;
    }
    return (p && p[0]) ? p : NULL;
}

/*
 * VA-API driver VTable implementations
 */

static VAStatus
v4l2sl_terminate(VADriverContextP ctx)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    if (!driver_data)
        return VA_STATUS_SUCCESS;

    if (driver_data->media_fd >= 0)
        close(driver_data->media_fd);

    free(driver_data);
    ctx->pDriverData = NULL;

    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_query_config_profiles(VADriverContextP ctx,
                             VAProfile *profiles,
                             int *num_profiles)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    int count = 0;

    for (unsigned i = 0; i < NUM_PROFILES && count < *num_profiles; i++) {
        enum v4l2sl_codec codec;

        if (codec_for_profile(v4l2sl_profiles[i], &codec) < 0)
            continue;
        if (!cached_device(driver_data, codec))
            continue;
        profiles[count++] = v4l2sl_profiles[i];
    }

    *num_profiles = count;
    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_query_config_entrypoints(VADriverContextP ctx,
                                VAProfile profile,
                                VAEntrypoint *entrypoints,
                                int *num_entrypoints)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    enum v4l2sl_codec codec;
    int supported = 0;

    for (unsigned i = 0; i < NUM_PROFILES; i++) {
        if (v4l2sl_profiles[i] == profile) {
            supported = 1;
            break;
        }
    }
    if (supported && codec_for_profile(profile, &codec) == 0 &&
        !cached_device(driver_data, codec))
        supported = 0;

    if (!supported) {
        *num_entrypoints = 0;
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    int count = 0;
    for (unsigned i = 0; i < NUM_ENTRYPOINTS && count < *num_entrypoints; i++)
        entrypoints[count++] = v4l2sl_entrypoints[i];

    *num_entrypoints = count;
    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_get_config_attributes(VADriverContextP ctx,
                             VAProfile profile,
                             VAEntrypoint entrypoint,
                             VAConfigAttrib *attrib_list,
                             int num_attribs)
{
    for (int i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            attrib_list[i].value = VA_RT_FORMAT_YUV420;
            for (unsigned j = 0; j < sizeof(profile_rt_formats)/sizeof(profile_rt_formats[0]); j++) {
                if (profile_rt_formats[j].profile == profile) {
                    attrib_list[i].value = profile_rt_formats[j].rt_format;
                    break;
                }
            }
            break;
        case VAConfigAttribDecSliceMode:
            attrib_list[i].value = VA_DEC_SLICE_MODE_NORMAL;
            break;
        default:
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_create_config(VADriverContextP ctx,
                     VAProfile profile,
                     VAEntrypoint entrypoint,
                     VAConfigAttrib *attrib_list,
                     int num_attribs,
                     VAConfigID *config_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_config *config;

    const char *device_path;
    enum v4l2sl_codec codec;
    uint32_t fourcc;
    char fcc[5];

    if (codec_for_profile(profile, &codec) < 0)
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

    device_path = cached_device(driver_data, codec);
    if (!device_path) {
        if (v4l2sl_codec_coded_fourcc(codec, &fourcc) == 0)
            v4l2sl_fourcc_to_str(fourcc, fcc);
        else
            memcpy(fcc, "????", 5);
        fprintf(stderr,
                "v4l2stateless: no V4L2 stateless decoder advertises %s "
                "(OUTPUT fourcc %s)\n",
                v4l2sl_codec_name(codec), fcc);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    config = calloc(1, sizeof(*config));
    if (!config)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    config->profile = profile;
    config->entrypoint = entrypoint;
    config->codec = codec;
    config->device_path = device_path;
    fprintf(stderr, "v4l2stateless: %s config uses %s\n",
            v4l2sl_codec_name(codec), device_path);

    /* Parse RT format from attribs */
    config->rt_format = VA_RT_FORMAT_YUV420;
    for (int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VAConfigAttribRTFormat)
            config->rt_format = attrib_list[i].value;
    }

    /* Simple ID allocation */
    config->config_id = ++driver_data->next_config_id;
    config->next = driver_data->configs;
    driver_data->configs = config;

    *config_id = config->config_id;
    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_destroy_config(VADriverContextP ctx, VAConfigID config_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_config **pp = &driver_data->configs;

    while (*pp) {
        if ((*pp)->config_id == config_id) {
            struct v4l2sl_config *config = *pp;
            *pp = config->next;
            free(config);
            return VA_STATUS_SUCCESS;
        }
        pp = &(*pp)->next;
    }

    return VA_STATUS_ERROR_INVALID_CONFIG;
}

static VAStatus
v4l2sl_query_config_attributes(VADriverContextP ctx,
                               VAConfigID config_id,
                               VAProfile *profile,
                               VAEntrypoint *entrypoint,
                               VAConfigAttrib *attrib_list,
                               int *num_attribs)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_config *config = driver_data->configs;

    while (config && config->config_id != config_id)
        config = config->next;

    if (!config)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    *profile = config->profile;
    *entrypoint = config->entrypoint;

    int count = 0;
    if (count < *num_attribs) {
        attrib_list[count].type = VAConfigAttribRTFormat;
        attrib_list[count].value = config->rt_format;
        count++;
    }

    *num_attribs = count;
    return VA_STATUS_SUCCESS;
}

/*
 * vaQuerySurfaceAttributes — FFmpeg's VAAPI hwaccel refuses to initialise
 * when the driver leaves this vtable entry NULL. Reports NV12 output with
 * the VDPU381/Hantro 4K limits.
 */
static VAStatus
v4l2sl_query_surface_attributes(VADriverContextP ctx,
                                VAConfigID config_id,
                                VASurfaceAttrib *attrib_list,
                                unsigned int *num_attribs)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_config *config = driver_data->configs;

    if (!num_attribs)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    while (config && config->config_id != config_id)
        config = config->next;
    if (!config)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    VASurfaceAttrib attribs[5];
    unsigned int count = 0;

    attribs[count].type          = VASurfaceAttribPixelFormat;
    attribs[count].flags         = VA_SURFACE_ATTRIB_GETTABLE;
    attribs[count].value.type    = VAGenericValueTypeInteger;
    attribs[count].value.value.i = VA_FOURCC_NV12;
    count++;

    attribs[count].type          = VASurfaceAttribMinWidth;
    attribs[count].flags         = VA_SURFACE_ATTRIB_GETTABLE;
    attribs[count].value.type    = VAGenericValueTypeInteger;
    attribs[count].value.value.i = 16;
    count++;

    attribs[count].type          = VASurfaceAttribMinHeight;
    attribs[count].flags         = VA_SURFACE_ATTRIB_GETTABLE;
    attribs[count].value.type    = VAGenericValueTypeInteger;
    attribs[count].value.value.i = 16;
    count++;

    attribs[count].type          = VASurfaceAttribMaxWidth;
    attribs[count].flags         = VA_SURFACE_ATTRIB_GETTABLE;
    attribs[count].value.type    = VAGenericValueTypeInteger;
    attribs[count].value.value.i = 4096;
    count++;

    attribs[count].type          = VASurfaceAttribMaxHeight;
    attribs[count].flags         = VA_SURFACE_ATTRIB_GETTABLE;
    attribs[count].value.type    = VAGenericValueTypeInteger;
    attribs[count].value.value.i = 4096;
    count++;

    if (!attrib_list) {
        *num_attribs = count;
        return VA_STATUS_SUCCESS;
    }

    if (*num_attribs < count) {
        *num_attribs = count;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    memcpy(attrib_list, attribs, count * sizeof(attribs[0]));
    *num_attribs = count;
    return VA_STATUS_SUCCESS;
}

/*
 * Surfaces — backed by V4L2 buffers (DMA-BUF)
 */

static VAStatus
v4l2sl_create_surfaces(VADriverContextP ctx,
                       int width, int height,
                       int format,
                       int num_surfaces,
                       VASurfaceID *surfaces)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    for (int i = 0; i < num_surfaces; i++) {
        struct v4l2sl_surface *surface = calloc(1, sizeof(*surface));
        if (!surface) {
            /* Clean up already created surfaces */
            for (int j = 0; j < i; j++) {
                free(driver_data->surfaces[surfaces[j]]);
                driver_data->surfaces[surfaces[j]] = NULL;
            }
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }

        surface->width = width;
        surface->height = height;
        surface->format = format;
        surface->status = VASurfaceReady;
        surface->buf_index = -1;
        surface->dma_buf_fd = -1;

        /* Allocate ID */
        surfaces[i] = ++driver_data->next_surface_id;
        driver_data->surfaces[surfaces[i]] = surface;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_create_surfaces2(VADriverContextP ctx,
                        unsigned int format,
                        unsigned int width,
                        unsigned int height,
                        VASurfaceID *surfaces,
                        unsigned int num_surfaces,
                        VASurfaceAttrib *attrib_list,
                        unsigned int num_attribs)
{
    unsigned int fourcc = VA_FOURCC_NV12;

    for (unsigned int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VASurfaceAttribPixelFormat &&
            attrib_list[i].value.type == VAGenericValueTypeInteger)
            fourcc = attrib_list[i].value.value.i;
        /* MemoryType / UsageHint: accepted and ignored — our surfaces are
         * V4L2 capture buffers, DMA-BUF export happens via vaDeriveImage */
    }

    if (fourcc != VA_FOURCC_NV12)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    return v4l2sl_create_surfaces(ctx, width, height, fourcc,
                                  num_surfaces, surfaces);
}

static VAStatus
v4l2sl_destroy_surfaces(VADriverContextP ctx,
                        VASurfaceID *surfaces,
                        int num_surfaces)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    for (int i = 0; i < num_surfaces; i++) {
        struct v4l2sl_surface *surface = driver_data->surfaces[surfaces[i]];
        if (surface) {
            if (surface->dma_buf_fd >= 0)
                close(surface->dma_buf_fd);
            free(surface);
            driver_data->surfaces[surfaces[i]] = NULL;
        }
    }

    return VA_STATUS_SUCCESS;
}

/*
 * Context — one per decode session
 */

static VAStatus
v4l2sl_create_context(VADriverContextP ctx,
                      VAConfigID config_id,
                      int picture_width,
                      int picture_height,
                      int flag,
                      VASurfaceID *render_targets,
                      int num_render_targets,
                      VAContextID *context_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    struct v4l2sl_context *context = calloc(1, sizeof(*context));
    if (!context)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    context->config_id = config_id;
    context->driver_data = driver_data;
    context->width = picture_width;
    context->height = picture_height;
    context->num_render_targets = num_render_targets;
    context->request_fd = -1;
    context->render_targets = calloc(num_render_targets, sizeof(VASurfaceID));
    if (!context->render_targets) {
        free(context);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    memcpy(context->render_targets, render_targets,
           num_render_targets * sizeof(VASurfaceID));

    /* Find the config to get codec info */
    struct v4l2sl_config *config = driver_data->configs;
    while (config && config->config_id != config_id)
        config = config->next;

    if (config) {
        context->codec = config->codec;
        context->device_path = config->device_path;
    }

    /* Open V4L2 video device */
    context->v4l2_fd = v4l2sl_open_device(context->device_path);
    if (context->v4l2_fd < 0) {
        fprintf(stderr, "v4l2stateless: failed to open %s\n", context->device_path);
        free(context->render_targets);
        free(context);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Open media device for request API (sysfs, not /dev/mediaN == videoN) */
    context->media_fd = v4l2sl_open_media_for_device(context->device_path);
    if (context->media_fd < 0) {
        fprintf(stderr, "v4l2stateless: no media request node for %s\n",
                context->device_path);
        close(context->v4l2_fd);
        free(context->render_targets);
        free(context);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Determine V4L2 codec format */
    uint32_t v4l2_format;
    switch (context->codec) {
    case V4L2SL_CODEC_H264: v4l2_format = V4L2_PIX_FMT_H264_SLICE; break;
    case V4L2SL_CODEC_HEVC: v4l2_format = V4L2_PIX_FMT_HEVC_SLICE; break;
    case V4L2SL_CODEC_AV1:  v4l2_format = V4L2_PIX_FMT_AV1_FRAME; break;
    default: v4l2_format = V4L2_PIX_FMT_H264_SLICE; break;
    }

    /* Setup output queue (compressed input) */
    int n_out = v4l2sl_setup_output_queue(context->v4l2_fd, v4l2_format,
                                          picture_width, picture_height);
    if (n_out <= 0) {
        fprintf(stderr, "v4l2stateless: failed to setup output queue\n");
        close(context->v4l2_fd);
        context->v4l2_fd = -1;
        /* Continue — decode won't work but driver can still load */
    } else {
        context->output_bufs_allocd = n_out;
        /* Mmap the output buffers for writing compressed data */
        if (v4l2sl_mmap_output_buffers(context->v4l2_fd, n_out,
                                       context->output_buf_ptr,
                                       &context->output_buf_size) < 0) {
            fprintf(stderr, "v4l2stateless: warning: failed to mmap output buffers\n");
            /* Non-fatal: we'll fall back to per-frame mmap */
        }
    }

    /* Setup capture queue (decoded output) */
    if (context->v4l2_fd >= 0) {
        int n_cap = v4l2sl_setup_capture_queue(context->v4l2_fd,
                                               picture_width, picture_height);
        if (n_cap > 0)
            context->capture_bufs_allocd = n_cap;
    }

    /*
     * Start streaming on both queues — QBUF is EPERM before this.
     * HEVC defers STREAMON: the rkvdec kernel requires the SPS control
     * (global) before it accepts STREAMON on the OUTPUT queue, and the
     * SPS only becomes available at the first picture.
     */
    if (context->codec != V4L2SL_CODEC_HEVC &&
        context->codec != V4L2SL_CODEC_AV1 &&
        context->v4l2_fd >= 0 && context->output_bufs_allocd > 0 &&
        context->capture_bufs_allocd > 0) {
        if (v4l2sl_streamon(context->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0 ||
            v4l2sl_streamon(context->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0)
            fprintf(stderr, "v4l2stateless: warning: STREAMON failed, decode will not work\n");
        else
            context->streamed = 1;
    }

    /* Capture geometry as negotiated by the driver (stride may be padded) */
    if (context->v4l2_fd >= 0 && context->capture_bufs_allocd > 0 &&
        v4l2sl_get_capture_geometry(context->v4l2_fd,
                                    &context->cap_width, &context->cap_height,
                                    &context->cap_stride,
                                    &context->cap_sizeimage) == 0) {
        fprintf(stderr, "v4l2stateless: capture geometry %ux%u stride=%u size=%u\n",
                context->cap_width, context->cap_height,
                context->cap_stride, context->cap_sizeimage);
    }

    /* All buffers start free */
    for (int i = 0; i < context->output_bufs_allocd &&
                    i < V4L2SL_NUM_OUTPUT_BUFS; i++)
        context->free_out_bufs[context->n_free_out++] = i;
    for (int i = 0; i < context->capture_bufs_allocd &&
                    i < V4L2SL_NUM_CAPTURE_BUFS; i++)
        context->free_cap_bufs[context->n_free_cap++] = i;

    *context_id = ++driver_data->next_context_id;
    context->context_id = *context_id;
    context->next = driver_data->contexts;
    driver_data->contexts = context;

    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_destroy_context(VADriverContextP ctx, VAContextID context_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_context **pp = &driver_data->contexts;

    while (*pp) {
        if ((*pp)->context_id == context_id) {
            struct v4l2sl_context *context = *pp;
            *pp = context->next;

            /* Unmap output buffers */
            for (int i = 0; i < context->output_bufs_allocd; i++) {
                if (context->output_buf_ptr[i] && context->output_buf_ptr[i] != MAP_FAILED) {
                    munmap(context->output_buf_ptr[i], context->output_buf_size);
                    context->output_buf_ptr[i] = NULL;
                }
            }

            if (context->v4l2_fd >= 0)
                close(context->v4l2_fd);
            if (context->media_fd >= 0)
                close(context->media_fd);
            if (context->request_fd >= 0)
                close(context->request_fd);
            free(context->render_targets);
            free(context);
            return VA_STATUS_SUCCESS;
        }
        pp = &(*pp)->next;
    }

    return VA_STATUS_ERROR_INVALID_CONTEXT;
}

/*
 * Buffers — parameter buffers for decode
 */

static VAStatus
v4l2sl_create_buffer(VADriverContextP ctx,
                     VAContextID context_id,
                     VABufferType type,
                     unsigned int size,
                     unsigned int num_elements,
                     void *data,
                     VABufferID *buf_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    struct v4l2sl_buffer *buf = calloc(1, sizeof(*buf));
    if (!buf)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    buf->type = type;
    buf->size = size;
    buf->num_elements = num_elements;
    buf->data = malloc(size * num_elements);
    if (!buf->data) {
        free(buf);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (data)
        memcpy(buf->data, data, size * num_elements);

    pthread_mutex_lock(&g_v4l2sl_lock);
    *buf_id = ++driver_data->next_buffer_id;
    buf->buffer_id = *buf_id;

    /* Attach to context */
    struct v4l2sl_context *context = driver_data->contexts;
    while (context && context->context_id != context_id)
        context = context->next;

    if (context) {
        buf->next = context->buffers;
        context->buffers = buf;
    } else {
        /* Orphan buffer — store globally */
        buf->next = driver_data->orphan_buffers;
        driver_data->orphan_buffers = buf;
    }
    pthread_mutex_unlock(&g_v4l2sl_lock);

    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_destroy_buffer(VADriverContextP ctx, VABufferID buf_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    pthread_mutex_lock(&g_v4l2sl_lock);

    /* Search in all contexts */
    struct v4l2sl_context *context = driver_data->contexts;
    while (context) {
        struct v4l2sl_buffer **pp = &context->buffers;
        while (*pp) {
            if ((*pp)->buffer_id == buf_id) {
                struct v4l2sl_buffer *buf = *pp;
                *pp = buf->next;
                free(buf->data);
                free(buf);
                pthread_mutex_unlock(&g_v4l2sl_lock);
                return VA_STATUS_SUCCESS;
            }
            pp = &(*pp)->next;
        }
        context = context->next;
    }

    /* Search orphan buffers */
    struct v4l2sl_buffer **pp = &driver_data->orphan_buffers;
    while (*pp) {
        if ((*pp)->buffer_id == buf_id) {
            struct v4l2sl_buffer *buf = *pp;
            *pp = buf->next;
            free(buf->data);
            free(buf);
            pthread_mutex_unlock(&g_v4l2sl_lock);
            return VA_STATUS_SUCCESS;
        }
        pp = &(*pp)->next;
    }

    pthread_mutex_unlock(&g_v4l2sl_lock);
    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus
v4l2sl_map_buffer(VADriverContextP ctx, VABufferID buf_id, void **pbuff)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    /* Search all contexts + orphans */
    pthread_mutex_lock(&g_v4l2sl_lock);
    struct v4l2sl_context *context = driver_data->contexts;
    while (context) {
        struct v4l2sl_buffer *buf = context->buffers;
        while (buf) {
            if (buf->buffer_id == buf_id) {
                    *pbuff = buf->data;
                pthread_mutex_unlock(&g_v4l2sl_lock);
                return VA_STATUS_SUCCESS;
            }
            buf = buf->next;
        }
        context = context->next;
    }

    struct v4l2sl_buffer *buf = driver_data->orphan_buffers;
    while (buf) {
        if (buf->buffer_id == buf_id) {
            *pbuff = buf->data;
            pthread_mutex_unlock(&g_v4l2sl_lock);
            return VA_STATUS_SUCCESS;
        }
        buf = buf->next;
    }
    pthread_mutex_unlock(&g_v4l2sl_lock);

    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus
v4l2sl_unmap_buffer(VADriverContextP ctx, VABufferID buf_id)
{
    /* No-op — we use malloc'd memory */
    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_buffer_set_num_elements(VADriverContextP ctx,
                               VABufferID buf_id,
                               unsigned int num_elements)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    struct v4l2sl_context *context = driver_data->contexts;
    while (context) {
        struct v4l2sl_buffer *buf = context->buffers;
        while (buf) {
            if (buf->buffer_id == buf_id) {
                if (num_elements > buf->num_elements) {
                    void *new_data = realloc(buf->data, buf->size * num_elements);
                    if (!new_data)
                        return VA_STATUS_ERROR_ALLOCATION_FAILED;
                    buf->data = new_data;
                }
                buf->num_elements = num_elements;
                return VA_STATUS_SUCCESS;
            }
            buf = buf->next;
        }
        context = context->next;
    }

    return VA_STATUS_ERROR_INVALID_BUFFER;
}

/*
 * Decode pipeline — Begin/Render/End
 */

static VAStatus
v4l2sl_begin_picture(VADriverContextP ctx,
                     VAContextID context_id,
                     VASurfaceID render_target)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    /* Find context */
    struct v4l2sl_context *context = driver_data->contexts;
    while (context && context->context_id != context_id)
        context = context->next;

    if (!context)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    /* Find surface */
    struct v4l2sl_surface *surface = driver_data->surfaces[render_target];
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    context->current_surface = surface;
    context->current_surface_id = render_target;
    context->num_pending_buffers = 0;

    /* If this surface still holds a decoded capture buffer, return it to the
     * free pool — being re-targeted means its previous frame is obsolete
     * (and the client has synced it already, per VA-API contract). */
    if (surface->buf_index >= 0) {
        /* Userspace bookkeeping only: hand the buffer back to the free pool.
         * The kernel QBUF happens exactly once, in the decode path. */
        if (surface->buf_index < context->capture_bufs_allocd &&
            context->n_free_cap < V4L2SL_NUM_CAPTURE_BUFS)
            context->free_cap_bufs[context->n_free_cap++] = surface->buf_index;
        surface->buf_index = -1;
        if (surface->dma_buf_fd >= 0) {
            close(surface->dma_buf_fd);
            surface->dma_buf_fd = -1;
        }
    }

    /* Allocate a V4L2 request for this picture */
    if (context->media_fd >= 0) {
        if (context->request_fd >= 0)
            close(context->request_fd);
        context->request_fd = v4l2sl_request_alloc(context->media_fd);
    }

    /* Assign a V4L2 timestamp for this picture (used for DPB reference
     * matching). Unit: nanoseconds in 1µs steps, matching what vb2 stores
     * from the timeval we set on the OUTPUT buffer. */
    surface->timestamp = (uint64_t)context->frame_count++ * 1000;

    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_render_picture(VADriverContextP ctx,
                      VAContextID context_id,
                      VABufferID *buffers,
                      int num_buffers)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    struct v4l2sl_context *context = driver_data->contexts;
    while (context && context->context_id != context_id)
        context = context->next;

    if (!context)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    /* Collect buffer references for this picture */
    for (int i = 0; i < num_buffers; i++) {
        /* Find the buffer in context */
        struct v4l2sl_buffer *b = context->buffers;
        while (b) {
            if (b->buffer_id == buffers[i]) {
                if (context->num_pending_buffers < 32) {
                    context->pending_buffers[context->num_pending_buffers++] = b;
                }
                break;
            }
            b = b->next;
        }

        if (!b)
            return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    return VA_STATUS_SUCCESS;
}

/*
 * Wait for a V4L2 decode to complete on the given fd.
 * Uses poll() on the video device; dequeues the capture buffer when ready.
 * Returns the capture buffer index, or -1 on error/timeout.
 */
static int v4l2sl_wait_and_dequeue(int v4l2_fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = v4l2_fd, .events = POLLOUT };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        fprintf(stderr, "v4l2stateless: poll failed: %s\n", strerror(errno));
        return -1;
    }
    if (ret == 0) {
        fprintf(stderr, "v4l2stateless: decode timeout (%d ms)\n", timeout_ms);
        return -1;
    }

    /* Dequeue capture buffer (decoded frame) */
    return v4l2sl_dequeue_buffer(v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
}

static VAStatus
v4l2sl_end_picture(VADriverContextP ctx,
                   VAContextID context_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    struct v4l2sl_context *context = driver_data->contexts;
    while (context && context->context_id != context_id)
        context = context->next;

    if (!context)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    if (!context->current_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    if (context->v4l2_fd < 0 || context->request_fd < 0) {
        /* V4L2 device not available — just mark surface as ready (stub mode) */
        context->current_surface->status = VASurfaceReady;
        context->current_surface = NULL;
        context->num_pending_buffers = 0;
        if (context->request_fd >= 0) {
            close(context->request_fd);
            context->request_fd = -1;
        }
        return VA_STATUS_SUCCESS;
    }

    /* Call codec-specific translation */
    VAStatus va_status;
    switch (context->codec) {
    case V4L2SL_CODEC_H264:
        va_status = v4l2sl_h264_translate(context,
                                          context->pending_buffers,
                                          context->num_pending_buffers);
        break;
    case V4L2SL_CODEC_HEVC:
        va_status = v4l2sl_hevc_translate(context,
                                          context->pending_buffers,
                                          context->num_pending_buffers);
        break;
    case V4L2SL_CODEC_AV1:
        va_status = v4l2sl_av1_translate(context,
                                         context->pending_buffers,
                                         context->num_pending_buffers);
        break;
    default:
        va_status = VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        break;
    }

    if (va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "v4l2stateless: decode translate failed: %d\n", va_status);
        /* Clean up and return */
        context->current_surface->status = VASurfaceSkipped;
        context->current_surface = NULL;
        context->num_pending_buffers = 0;
        if (context->request_fd >= 0) {
            close(context->request_fd);
            context->request_fd = -1;
        }
        return va_status;
    }

    /* Mark surface as rendering (will become Ready after sync) */
    context->current_surface->status = VASurfaceRendering;
    context->current_surface = NULL;
    context->num_pending_buffers = 0;

    /* Don't close request_fd here — sync_surface will dequeue and close it */
    return VA_STATUS_SUCCESS;
}

/*
 * Surface sync/status — wait for V4L2 decode to complete
 */
static VAStatus
v4l2sl_sync_surface(VADriverContextP ctx, VASurfaceID render_target)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_surface *surface = driver_data->surfaces[render_target];

    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    /* Decoding runs synchronously inside vaEndPicture (single request in
     * flight), so by the time the client syncs, the frame — if it decoded —
     * is already attached to the surface as a capture-buffer DMA-BUF. */
    surface->status = VASurfaceReady;

    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_query_surface_status(VADriverContextP ctx,
                            VASurfaceID render_target,
                            VASurfaceStatus *status)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_surface *surface = driver_data->surfaces[render_target];

    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    *status = surface->status;
    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_query_surface_error(VADriverContextP ctx,
                           VASurfaceID render_target,
                           VAStatus error_status,
                           void **error_info)
{
    if (error_info)
        *error_info = NULL;
    return VA_STATUS_SUCCESS;
}

/*
 * Image / DMA-BUF access
 *
 * vaDeriveImage is the primary way for applications (like Firefox) to get
 * access to the decoded NV12 frame data. It returns a VAImage backed by
 * a DMA-BUF fd that was exported from the V4L2 capture buffer.
 */

static VAStatus
v4l2sl_query_image_formats(VADriverContextP ctx,
                           VAImageFormat *format_list,
                           int *num_formats)
{
    int count = 0;

    if (count < *num_formats) {
        memset(&format_list[count], 0, sizeof(VAImageFormat));
        format_list[count].fourcc = VA_FOURCC_NV12;
        format_list[count].depth = 12;
        format_list[count].bits_per_pixel = 12;
        count++;
    }

    *num_formats = count;
    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_derive_image(VADriverContextP ctx,
                    VASurfaceID surface,
                    VAImage *image)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_surface *surf = driver_data->surfaces[surface];

    if (!surf)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    if (surf->dma_buf_fd < 0 || surf->buf_index < 0) {
        fprintf(stderr, "v4l2stateless: derive_image: surface %d has no decoded frame\n",
                surface);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    /* Find the owning context to get the negotiated capture geometry —
     * the stride can be padded well beyond the display width. */
    struct v4l2sl_context *c = driver_data->contexts;
    while (c) {
        int hit = 0;
        for (int i = 0; i < c->num_render_targets; i++) {
            if (c->render_targets[i] == surface) {
                hit = 1;
                break;
            }
        }
        if (hit)
            break;
        c = c->next;
    }
    uint32_t stride = surf->stride ? surf->stride :
                      ((c && c->cap_stride) ? c->cap_stride : surf->width);
    uint32_t aligned_h = surf->aligned_h ? surf->aligned_h :
                          ((c && c->cap_height) ? c->cap_height : surf->height);
    uint32_t data_size = stride * aligned_h * 3 / 2;  /* NV12: Y + interleaved UV */

    /* Map the decoded frame once; the mapping lives until vaDestroyImage. */
    void *map = mmap(NULL, data_size, PROT_READ, MAP_SHARED, surf->dma_buf_fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "v4l2stateless: derive_image: mmap dmabuf failed: %s\n",
                strerror(errno));
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    struct v4l2sl_buffer *ib = calloc(1, sizeof(*ib));
    if (!ib) {
        munmap(map, data_size);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    pthread_mutex_lock(&g_v4l2sl_lock);
    VABufferID id = ++driver_data->next_buffer_id;
    ib->buffer_id = id;
    ib->type = VAImageBufferType;
    ib->size = data_size;
    ib->data = map;
    ib->mmapped = 1;
    ib->next = driver_data->orphan_buffers;
    driver_data->orphan_buffers = ib;
    pthread_mutex_unlock(&g_v4l2sl_lock);

    memset(image, 0, sizeof(*image));
    image->image_id = id;
    image->buf = id;               /* vaMapBuffer(image.buf) returns the mapping */
    image->format.fourcc = VA_FOURCC_NV12;
    image->width = surf->width;    /* display size */
    image->height = surf->height;
    image->num_planes = 2;
    image->pitches[0] = stride;    /* Y stride (driver-padded) */
    image->pitches[1] = stride;    /* UV stride */
    image->offsets[0] = 0;
    image->offsets[1] = stride * aligned_h;
    image->data_size = data_size;

    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_destroy_image(VADriverContextP ctx, VAImageID image_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    pthread_mutex_lock(&g_v4l2sl_lock);
    struct v4l2sl_buffer **pp = &driver_data->orphan_buffers;

    while (*pp) {
        if ((*pp)->buffer_id == image_id) {
            struct v4l2sl_buffer *ib = *pp;
            if (ib->mmapped)
                munmap(ib->data, ib->size);
            else
                free(ib->data);
            *pp = ib->next;
            free(ib);
            pthread_mutex_unlock(&g_v4l2sl_lock);
            return VA_STATUS_SUCCESS;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_v4l2sl_lock);

    return VA_STATUS_SUCCESS;
}

static struct v4l2sl_context *
context_for_surface(struct v4l2sl_driver_data *dd, VASurfaceID surface)
{
    for (struct v4l2sl_context *c = dd->contexts; c; c = c->next)
        for (int i = 0; i < c->num_render_targets; i++)
            if (c->render_targets[i] == surface)
                return c;
    return NULL;
}

/*
 * CPU-download path used by clients whose derive probe failed (e.g. ffmpeg
 * probes vaDeriveImage with a fresh surface before any decode, then falls
 * back to vaCreateImage + vaGetImage + vaMapBuffer).
 */
static VAStatus
v4l2sl_create_image(VADriverContextP ctx, VAImageFormat *format,
                    int width, int height, VAImage *image)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    if (!format || format->fourcc != VA_FOURCC_NV12)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (width <= 0 || height <= 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* Match the negotiated capture geometry when a decode session exists. */
    struct v4l2sl_context *c = driver_data->contexts;
    uint32_t stride = width, aligned_h = height;
    if (c && c->cap_stride) {
        stride = c->cap_stride;
        aligned_h = c->cap_height;
    }
    uint32_t data_size = stride * aligned_h * 3 / 2;

    struct v4l2sl_buffer *ib = calloc(1, sizeof(*ib));
    if (!ib)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    ib->data = malloc(data_size);
    if (!ib->data) {
        free(ib);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    pthread_mutex_lock(&g_v4l2sl_lock);
    VABufferID id = ++driver_data->next_buffer_id;
    ib->buffer_id = id;
    ib->type = VAImageBufferType;
    ib->size = data_size;
    ib->next = driver_data->orphan_buffers;
    driver_data->orphan_buffers = ib;
    pthread_mutex_unlock(&g_v4l2sl_lock);

    memset(image, 0, sizeof(*image));
    image->image_id = id;
    image->buf = id;
    image->format = *format;
    image->width = width;
    image->height = height;
    image->num_planes = 2;
    image->pitches[0] = stride;
    image->pitches[1] = stride;
    image->offsets[0] = 0;
    image->offsets[1] = stride * aligned_h;
    image->data_size = data_size;

    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_get_image(VADriverContextP ctx, VASurfaceID surface,
                 int x, int y, unsigned int width, unsigned int height,
                 VAImageID image_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_surface *surf = driver_data->surfaces[surface];

    if (!surf || surf->dma_buf_fd < 0)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (x != 0 || y != 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;  /* region reads unused by ffmpeg */

    pthread_mutex_lock(&g_v4l2sl_lock);
    struct v4l2sl_buffer *ib = driver_data->orphan_buffers;
    while (ib && ib->buffer_id != image_id)
        ib = ib->next;
    pthread_mutex_unlock(&g_v4l2sl_lock);
    if (!ib)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    struct v4l2sl_context *c = context_for_surface(driver_data, surface);
    uint32_t src_stride = surf->stride ? surf->stride :
                          ((c && c->cap_stride) ? c->cap_stride : surf->width);
    uint32_t src_alh = surf->aligned_h ? surf->aligned_h :
                       ((c && c->cap_height) ? c->cap_height : surf->height);
    size_t map_size = (size_t)src_stride * src_alh * 3 / 2;

    uint8_t *src = mmap(NULL, map_size, PROT_READ, MAP_SHARED,
                        surf->dma_buf_fd, 0);
    if (src == MAP_FAILED) {
        fprintf(stderr, "v4l2stateless: get_image: mmap dmabuf failed: %s\n",
                strerror(errno));
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    uint8_t *dst = ib->data;
    /* The image buffer comes from v4l2sl_create_image, which allocates with
     * the same negotiated geometry — destination stride matches the source. */
    uint32_t dst_stride = src_stride;

    unsigned int copy_w = width < (unsigned)surf->width ? width : surf->width;
    unsigned int copy_h = height < src_alh ? height : src_alh;

    for (unsigned int row = 0; row < copy_h; row++)
        memcpy(dst + (size_t)row * dst_stride,
               src + (size_t)row * src_stride, copy_w);
    for (unsigned int row = 0; row < copy_h / 2; row++)
        memcpy(dst + (size_t)dst_stride * src_alh + (size_t)row * dst_stride,
               src + (size_t)src_stride * src_alh + (size_t)row * src_stride,
               copy_w);

    munmap(src, map_size);
    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_put_surface(VADriverContextP ctx,
                   VASurfaceID surface,
                   void *draw,
                   short src_x, short src_y,
                   unsigned short src_w, unsigned short src_h,
                   short dest_x, short dest_y,
                   unsigned short dest_w, unsigned short dest_h,
                   VARectangle *cliprects,
                   unsigned int number_cliprects,
                   unsigned int flags)
{
    /* Not needed for decode-only driver */
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/*
 * Stub for unimplemented vtable entries
 */
static VAStatus
v4l2sl_stub_unimplemented(VADriverContextP ctx)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/*
 * Driver initialization
 */

static VAStatus
v4l2sl_init(VADriverContextP ctx)
{
    struct v4l2sl_driver_data *driver_data;

    driver_data = calloc(1, sizeof(*driver_data));
    if (!driver_data)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    driver_data->media_fd = -1;
    pthread_mutex_init(&driver_data->lock, NULL);
    ctx->pDriverData = driver_data;

    v4l2sl_scan_decoder_paths(driver_data->dev_h264, driver_data->dev_hevc,
                              driver_data->dev_av1, sizeof(driver_data->dev_h264));
    fprintf(stderr, "v4l2stateless: probe H.264 -> %s\n",
            driver_data->dev_h264[0] ? driver_data->dev_h264 : "(none)");
    fprintf(stderr, "v4l2stateless: probe HEVC  -> %s\n",
            driver_data->dev_hevc[0] ? driver_data->dev_hevc : "(none)");
    fprintf(stderr, "v4l2stateless: probe AV1   -> %s\n",
            driver_data->dev_av1[0] ? driver_data->dev_av1 : "(none)");

    /* Set context limits */
    ctx->max_profiles = NUM_PROFILES;
    ctx->max_entrypoints = NUM_ENTRYPOINTS;
    ctx->max_attributes = 4;
    ctx->max_image_formats = 1;
    ctx->max_subpic_formats = 1;
    ctx->max_display_attributes = 0;
    ctx->str_vendor = "v4l2stateless/vaapi-v4l2-bridge";

    /* Fill in VTable — ALL entries must be non-NULL */
    struct VADriverVTable *vtable = ctx->vtable;
    memset(vtable, 0, sizeof(*vtable));

    vtable->vaTerminate               = v4l2sl_terminate;
    vtable->vaQueryConfigProfiles     = v4l2sl_query_config_profiles;
    vtable->vaQueryConfigEntrypoints  = v4l2sl_query_config_entrypoints;
    vtable->vaGetConfigAttributes     = v4l2sl_get_config_attributes;
    vtable->vaCreateConfig            = v4l2sl_create_config;
    vtable->vaDestroyConfig           = v4l2sl_destroy_config;
    vtable->vaQueryConfigAttributes   = v4l2sl_query_config_attributes;
    vtable->vaCreateSurfaces          = v4l2sl_create_surfaces;
    vtable->vaCreateSurfaces2         = v4l2sl_create_surfaces2;
    vtable->vaDestroySurfaces         = v4l2sl_destroy_surfaces;
    vtable->vaCreateContext           = v4l2sl_create_context;
    vtable->vaDestroyContext          = v4l2sl_destroy_context;
    vtable->vaCreateBuffer            = v4l2sl_create_buffer;
    vtable->vaDestroyBuffer           = v4l2sl_destroy_buffer;
    vtable->vaMapBuffer               = v4l2sl_map_buffer;
    vtable->vaUnmapBuffer             = v4l2sl_unmap_buffer;
    vtable->vaBufferSetNumElements    = v4l2sl_buffer_set_num_elements;
    vtable->vaBeginPicture            = v4l2sl_begin_picture;
    vtable->vaRenderPicture           = v4l2sl_render_picture;
    vtable->vaEndPicture              = v4l2sl_end_picture;
    vtable->vaSyncSurface             = v4l2sl_sync_surface;
    vtable->vaQuerySurfaceAttributes  = v4l2sl_query_surface_attributes;
    vtable->vaQuerySurfaceStatus      = v4l2sl_query_surface_status;
    vtable->vaQuerySurfaceError       = v4l2sl_query_surface_error;
    vtable->vaQueryImageFormats       = v4l2sl_query_image_formats;
    vtable->vaCreateImage             = v4l2sl_create_image;
    vtable->vaDeriveImage             = v4l2sl_derive_image;
    vtable->vaDestroyImage            = v4l2sl_destroy_image;
    vtable->vaSetImagePalette         = (void *)v4l2sl_stub_unimplemented;
    vtable->vaGetImage                = v4l2sl_get_image;
    vtable->vaPutImage                = (void *)v4l2sl_stub_unimplemented;
    vtable->vaQuerySubpictureFormats  = (void *)v4l2sl_stub_unimplemented;
    vtable->vaCreateSubpicture        = (void *)v4l2sl_stub_unimplemented;
    vtable->vaDestroySubpicture       = (void *)v4l2sl_stub_unimplemented;
    vtable->vaSetSubpictureImage      = (void *)v4l2sl_stub_unimplemented;
    vtable->vaSetSubpictureChromakey  = (void *)v4l2sl_stub_unimplemented;
    vtable->vaSetSubpictureGlobalAlpha = (void *)v4l2sl_stub_unimplemented;
    vtable->vaAssociateSubpicture     = (void *)v4l2sl_stub_unimplemented;
    vtable->vaDeassociateSubpicture   = (void *)v4l2sl_stub_unimplemented;
    vtable->vaQueryDisplayAttributes  = (void *)v4l2sl_stub_unimplemented;
    vtable->vaGetDisplayAttributes    = (void *)v4l2sl_stub_unimplemented;
    vtable->vaSetDisplayAttributes    = (void *)v4l2sl_stub_unimplemented;
    vtable->vaPutSurface              = (void *)v4l2sl_put_surface;

    return VA_STATUS_SUCCESS;
}

/*
 * libva entry point
 */
VAStatus __vaDriverInit_1_20(VADriverContextP ctx)
{
    return v4l2sl_init(ctx);
}
