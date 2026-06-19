/*
 * v4l2stateless — VA-API to V4L2 Request API bridge driver
 *
 * Copyright 2026 — MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_drmcommon.h>

#include "v4l2stateless.h"

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

/* Map VA profile to V4L2 codec and device path */
static const struct {
    VAProfile profile;
    const char *device_path;
    enum v4l2sl_codec codec;
} profile_device_map[] = {
    { VAProfileH264Main,    "/dev/video1", V4L2SL_CODEC_H264 },
    { VAProfileH264High,    "/dev/video1", V4L2SL_CODEC_H264 },
    { VAProfileHEVCMain,    "/dev/video1", V4L2SL_CODEC_HEVC },
    { VAProfileHEVCMain10,  "/dev/video1", V4L2SL_CODEC_HEVC },
    { VAProfileAV1Profile0, "/dev/video4", V4L2SL_CODEC_AV1  },
};

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
    int count = 0;

    for (unsigned i = 0; i < NUM_PROFILES && count < *num_profiles; i++)
        profiles[count++] = v4l2sl_profiles[i];

    *num_profiles = count;
    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_query_config_entrypoints(VADriverContextP ctx,
                                VAProfile profile,
                                VAEntrypoint *entrypoints,
                                int *num_entrypoints)
{
    /* Check if profile is supported */
    int supported = 0;
    for (unsigned i = 0; i < NUM_PROFILES; i++) {
        if (v4l2sl_profiles[i] == profile) {
            supported = 1;
            break;
        }
    }

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

    /* Find the codec and device for this profile */
    const char *device_path = NULL;
    enum v4l2sl_codec codec = V4L2SL_CODEC_H264;

    for (unsigned i = 0; i < sizeof(profile_device_map)/sizeof(profile_device_map[0]); i++) {
        if (profile_device_map[i].profile == profile) {
            device_path = profile_device_map[i].device_path;
            codec = profile_device_map[i].codec;
            break;
        }
    }

    if (!device_path)
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

    config = calloc(1, sizeof(*config));
    if (!config)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    config->profile = profile;
    config->entrypoint = entrypoint;
    config->codec = codec;
    config->device_path = device_path;

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

        /* Allocate ID */
        surfaces[i] = ++driver_data->next_surface_id;
        driver_data->surfaces[surfaces[i]] = surface;
    }

    return VA_STATUS_SUCCESS;
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
            /* TODO: release V4L2 buffer if allocated */
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
    context->width = picture_width;
    context->height = picture_height;
    context->num_render_targets = num_render_targets;
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

    /* Open media device for request API */
    context->media_fd = v4l2sl_open_media_for_device(context->device_path);

    /* Determine V4L2 codec format */
    uint32_t v4l2_format;
    switch (context->codec) {
    case V4L2SL_CODEC_H264: v4l2_format = V4L2_PIX_FMT_H264_SLICE; break;
    case V4L2SL_CODEC_HEVC: v4l2_format = V4L2_PIX_FMT_HEVC_SLICE; break;
    case V4L2SL_CODEC_AV1:  v4l2_format = V4L2_PIX_FMT_AV1_FRAME; break;
    default: v4l2_format = V4L2_PIX_FMT_H264_SLICE; break;
    }

    /* Setup output queue (compressed input) */
    if (v4l2sl_setup_output_queue(context->v4l2_fd, v4l2_format,
                                  picture_width, picture_height) < 0) {
        fprintf(stderr, "v4l2stateless: failed to setup output queue\n");
        close(context->v4l2_fd);
        context->v4l2_fd = -1;
        /* Non-fatal for skeleton — decode not yet implemented */
    }

    /* Setup capture queue (decoded output) */
    if (context->v4l2_fd >= 0) {
        if (v4l2sl_setup_capture_queue(context->v4l2_fd,
                                       picture_width, picture_height) < 0) {
            fprintf(stderr, "v4l2stateless: failed to setup capture queue\n");
        }
    }

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

    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_destroy_buffer(VADriverContextP ctx, VABufferID buf_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

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
            return VA_STATUS_SUCCESS;
        }
        pp = &(*pp)->next;
    }

    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus
v4l2sl_map_buffer(VADriverContextP ctx, VABufferID buf_id, void **pbuff)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    /* Search all contexts + orphans */
    struct v4l2sl_context *context = driver_data->contexts;
    while (context) {
        struct v4l2sl_buffer *buf = context->buffers;
        while (buf) {
            if (buf->buffer_id == buf_id) {
                *pbuff = buf->data;
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
            return VA_STATUS_SUCCESS;
        }
        buf = buf->next;
    }

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

    /* Allocate a V4L2 request for this picture */
    if (context->media_fd >= 0)
        context->request_fd = v4l2sl_request_alloc(context->media_fd);

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

    if (va_status != VA_STATUS_SUCCESS)
        fprintf(stderr, "v4l2stateless: decode translate failed: %d\n", va_status);

    context->current_surface->status = VASurfaceReady;
    context->current_surface = NULL;
    context->num_pending_buffers = 0;

    /* Close request fd */
    if (context->request_fd >= 0) {
        close(context->request_fd);
        context->request_fd = -1;
    }

    return VA_STATUS_SUCCESS;
}

/*
 * Surface sync/status
 */

static VAStatus
v4l2sl_sync_surface(VADriverContextP ctx, VASurfaceID render_target)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_surface *surface = driver_data->surfaces[render_target];

    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    /* TODO: Wait for V4L2 decode to complete */
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

    /* TODO: Export DMA-BUF from V4L2 capture buffer */
    /* For now, return a stub */
    image->format.fourcc = VA_FOURCC_NV12;
    image->width = surf->width;
    image->height = surf->height;
    image->buf = VA_INVALID_ID;
    image->image_id = VA_INVALID_ID;

    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
v4l2sl_destroy_image(VADriverContextP ctx, VAImageID image_id)
{
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
    ctx->pDriverData = driver_data;

    /* Open /dev/media0 for V4L2 request API */
    driver_data->media_fd = open("/dev/media0", O_RDWR);
    if (driver_data->media_fd < 0) {
        fprintf(stderr, "v4l2stateless: failed to open /dev/media0: %s\n",
                strerror(errno));
        /* Non-fatal — we'll open per-device later */
    }

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
    vtable->vaQuerySurfaceStatus      = v4l2sl_query_surface_status;
    vtable->vaQuerySurfaceError       = v4l2sl_query_surface_error;
    vtable->vaQueryImageFormats       = v4l2sl_query_image_formats;
    vtable->vaCreateImage             = (void *)v4l2sl_stub_unimplemented;
    vtable->vaDeriveImage             = v4l2sl_derive_image;
    vtable->vaDestroyImage            = v4l2sl_destroy_image;
    vtable->vaSetImagePalette         = (void *)v4l2sl_stub_unimplemented;
    vtable->vaGetImage                = (void *)v4l2sl_stub_unimplemented;
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
