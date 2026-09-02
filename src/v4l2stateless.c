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
#include <va/va_backend_vpp.h>
#include <va/va_drmcommon.h>
#include <drm_fourcc.h>

#include "v4l2stateless.h"
#include "v4l2stateless_probe.h"

/* Global driver-wide lock — statically initialized, immune to struct-layout
 * or init-order issues. libva dispatches to the backend without serializing
 * client threads (its threading model makes thread safety a backend
 * requirement), so every stateful vtable entry takes this lock. Decode
 * (vaEndPicture) holds it across the synchronous request — callers on other
 * threads may block up to the 3 s decode timeout. */
static pthread_mutex_t g_v4l2sl_lock = PTHREAD_MUTEX_INITIALIZER;

/* Supported profiles */
static const VAProfile v4l2sl_profiles[] = {
    VAProfileH264ConstrainedBaseline,
    VAProfileH264Main,
    VAProfileH264High,
    VAProfileH264High10,
    VAProfileH264High422,
    VAProfileHEVCMain,
    VAProfileHEVCMain10,
    /* AV1 (VPU981) is not advertised: short VA-API sessions reopen
     * /dev/video4 and can hang the SoC. Desktop AV1 stays on
     * Chromium/GStreamer native V4L2. */
    VAProfileVP8Version0_3,
    VAProfileMPEG2Simple,
    VAProfileMPEG2Main,
    VAProfileJPEGBaseline,
    VAProfileNone,
};

#define NUM_PROFILES (sizeof(v4l2sl_profiles) / sizeof(v4l2sl_profiles[0]))

/* RT formats per profile (bitmask) */
static const struct {
    VAProfile profile;
    unsigned int rt_format;
} profile_rt_formats[] = {
    { VAProfileH264ConstrainedBaseline, VA_RT_FORMAT_YUV420 },
    { VAProfileH264Main,       VA_RT_FORMAT_YUV420 },
    { VAProfileH264High,       VA_RT_FORMAT_YUV420 },
    { VAProfileH264High10,     VA_RT_FORMAT_YUV420_10 },
    { VAProfileH264High422,    VA_RT_FORMAT_YUV422 | VA_RT_FORMAT_YUV422_10 },
    { VAProfileHEVCMain,       VA_RT_FORMAT_YUV420 },
    { VAProfileHEVCMain10,     VA_RT_FORMAT_YUV420_10 },
    { VAProfileVP8Version0_3,  VA_RT_FORMAT_YUV420 },
    { VAProfileMPEG2Simple,    VA_RT_FORMAT_YUV420 },
    { VAProfileMPEG2Main,      VA_RT_FORMAT_YUV420 },
    { VAProfileJPEGBaseline,   VA_RT_FORMAT_YUV420 },
    { VAProfileNone,           VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV422 | VA_RT_FORMAT_RGB32 },
};

/* Map VA profile to codec. Device path is resolved by OUTPUT fourcc. */
static const struct {
    VAProfile profile;
    enum v4l2sl_codec codec;
} profile_codec_map[] = {
    { VAProfileH264ConstrainedBaseline, V4L2SL_CODEC_H264 },
    { VAProfileH264Main,    V4L2SL_CODEC_H264 },
    { VAProfileH264High,    V4L2SL_CODEC_H264 },
    { VAProfileH264High10,  V4L2SL_CODEC_H264 },
    { VAProfileH264High422, V4L2SL_CODEC_H264 },
    { VAProfileHEVCMain,    V4L2SL_CODEC_HEVC },
    { VAProfileHEVCMain10,  V4L2SL_CODEC_HEVC },
    { VAProfileVP8Version0_3, V4L2SL_CODEC_VP8 },
    { VAProfileMPEG2Simple, V4L2SL_CODEC_MPEG2 },
    { VAProfileMPEG2Main,   V4L2SL_CODEC_MPEG2 },
    { VAProfileJPEGBaseline, V4L2SL_CODEC_JPEG_ENC },
    { VAProfileNone,        V4L2SL_CODEC_VPP },
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
    case V4L2SL_CODEC_H264:  p = dd->dev_h264;  break;
    case V4L2SL_CODEC_HEVC:  p = dd->dev_hevc;  break;
    case V4L2SL_CODEC_AV1:   p = dd->dev_av1;   break;
    case V4L2SL_CODEC_VP8:      p = dd->dev_vp8;      break;
    case V4L2SL_CODEC_MPEG2:    p = dd->dev_mpeg2;    break;
    case V4L2SL_CODEC_JPEG_ENC: p = dd->dev_jpeg_enc; break;
    case V4L2SL_CODEC_VPP:      p = dd->dev_vpp;      break;
    default: break;
    }
    return (p && p[0]) ? p : NULL;
}

/*
 * VA-API driver VTable implementations
 */

static void destroy_surface_locked(struct v4l2sl_driver_data *dd,
                                   struct v4l2sl_surface *s, VASurfaceID id);

static VAStatus
v4l2sl_terminate(VADriverContextP ctx)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    int i;

    if (!driver_data)
        return VA_STATUS_SUCCESS;

    pthread_mutex_lock(&g_v4l2sl_lock);
    while (driver_data->contexts) {
        struct v4l2sl_context *context = driver_data->contexts;
        struct v4l2sl_buffer *buf = context->buffers;

        driver_data->contexts = context->next;
        while (buf) {
            struct v4l2sl_buffer *next = buf->next;

            if (buf->mmapped)
                munmap(buf->data, buf->size);
            else
                free(buf->data);
            free(buf);
            buf = next;
        }
        v4l2sl_release_context_device(context);
        free(context->render_targets);
        free(context);
    }
    /* Clients that terminate without destroying their surfaces (session
     * teardown paths) still own them kernel-resource-wise: free the whole
     * table so fds and mappings cannot leak per VA session cycle. */
    for (i = 0; i < V4L2SL_MAX_SURFACES; i++) {
        struct v4l2sl_surface *s = driver_data->surfaces[i];

        if (s)
            destroy_surface_locked(driver_data, s, (VASurfaceID)i);
    }
    while (driver_data->orphan_buffers) {
        struct v4l2sl_buffer *buf = driver_data->orphan_buffers;

        driver_data->orphan_buffers = buf->next;
        if (buf->mmapped)
            munmap(buf->data, buf->size);
        else
            free(buf->data);
        free(buf);
    }
    while (driver_data->configs) {
        struct v4l2sl_config *config = driver_data->configs;

        driver_data->configs = config->next;
        free(config);
    }
    pthread_mutex_unlock(&g_v4l2sl_lock);

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
    /* Firefox vaapitest passes *num_profiles = 0 after allocating
     * vaMaxNumProfiles() slots. Treat 0 as "use max_profiles". */
    int cap = (num_profiles && *num_profiles > 0) ? *num_profiles
                                                  : (int)NUM_PROFILES;

    if (!profiles)
        cap = 0;

    for (unsigned i = 0; i < NUM_PROFILES; i++) {
        enum v4l2sl_codec codec;

        if (codec_for_profile(v4l2sl_profiles[i], &codec) < 0)
            continue;
        if (!cached_device(driver_data, codec))
            continue;
        if (count < cap)
            profiles[count] = v4l2sl_profiles[i];
        count++;
    }

    if (num_profiles)
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

    VAEntrypoint ep = VAEntrypointVLD;
    if (profile == VAProfileJPEGBaseline)
        ep = VAEntrypointEncPicture;
    else if (profile == VAProfileNone)
        ep = VAEntrypointVideoProc;

    if (entrypoints)
        entrypoints[0] = ep;
    if (num_entrypoints)
        *num_entrypoints = 1;
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
        case VAConfigAttribEncPackedHeaders:
            attrib_list[i].value = 0;
            break;
        case VAConfigAttribEncQualityRange:
            attrib_list[i].value = 100;
            break;
        case VAConfigAttribMaxPictureWidth:
            attrib_list[i].value = 8192;
            break;
        case VAConfigAttribMaxPictureHeight:
            attrib_list[i].value = 8192;
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

    if (entrypoint != VAEntrypointVLD &&
        entrypoint != VAEntrypointEncPicture &&
        entrypoint != VAEntrypointVideoProc)
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

    if (codec_for_profile(profile, &codec) < 0)
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (entrypoint == VAEntrypointEncPicture)
        codec = V4L2SL_CODEC_JPEG_ENC;
    if (entrypoint == VAEntrypointVideoProc)
        codec = V4L2SL_CODEC_VPP;

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

    /* Parse RT format from attribs. Leave 8-bit NV12 until the first
     * picture: rkvdec only programs 10-bit/422 after a matching SPS
     * (HEVC Main10 does NV12 here, then NV15 post-SPS). */
    config->rt_format = VA_RT_FORMAT_YUV420;
    for (int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VAConfigAttribRTFormat)
            config->rt_format = attrib_list[i].value;
    }

    fprintf(stderr, "v4l2stateless: %s config uses %s (VA profile %d rt=0x%x)\n",
            v4l2sl_codec_name(codec), device_path, (int)profile, config->rt_format);

    /* Simple ID allocation */
    pthread_mutex_lock(&g_v4l2sl_lock);
    config->config_id = ++driver_data->next_config_id;
    config->next = driver_data->configs;
    driver_data->configs = config;
    pthread_mutex_unlock(&g_v4l2sl_lock);

    *config_id = config->config_id;
    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_destroy_config(VADriverContextP ctx, VAConfigID config_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_config **pp = &driver_data->configs;

    pthread_mutex_lock(&g_v4l2sl_lock);
    while (*pp) {
        if ((*pp)->config_id == config_id) {
            struct v4l2sl_config *config = *pp;
            *pp = config->next;
            free(config);
            pthread_mutex_unlock(&g_v4l2sl_lock);
            return VA_STATUS_SUCCESS;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_v4l2sl_lock);

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
    VAProfile prof;
    VAEntrypoint entry;
    unsigned int rt;
    VAConfigAttrib attribs[8];
    int count = 0;

    pthread_mutex_lock(&g_v4l2sl_lock);
    while (config && config->config_id != config_id)
        config = config->next;
    if (!config) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    /* Snapshot the config fields under the lock (vaDestroyConfig frees
     * nodes holding it), then build the reply unlocked. */
    prof = config->profile;
    entry = config->entrypoint;
    rt = config->rt_format;
    pthread_mutex_unlock(&g_v4l2sl_lock);

    if (profile)
        *profile = prof;
    if (entrypoint)
        *entrypoint = entry;

    /*
     * Chrome's FillProfileInfo_Locked allocates vaMaxNumConfigAttributes()
     * slots and passes an uninitialized *num_attribs used only as an out
     * parameter. Do not treat *num_attribs as a buffer capacity.
     */
    attribs[count].type = VAConfigAttribRTFormat;
    attribs[count].value = rt;
    count++;
    attribs[count].type = VAConfigAttribDecSliceMode;
    attribs[count].value = VA_DEC_SLICE_MODE_NORMAL;
    count++;
    attribs[count].type = VAConfigAttribMaxPictureWidth;
    attribs[count].value = 8192;
    count++;
    attribs[count].type = VAConfigAttribMaxPictureHeight;
    attribs[count].value = 8192;
    count++;
    attribs[count].type = VAConfigAttribEncPackedHeaders;
    attribs[count].value = 0;
    count++;

    if (attrib_list)
        memcpy(attrib_list, attribs, (size_t)count * sizeof(attribs[0]));
    if (num_attribs)
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
    unsigned int rt;

    if (!num_attribs)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&g_v4l2sl_lock);
    while (config && config->config_id != config_id)
        config = config->next;
    if (!config) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    rt = config->rt_format;
    pthread_mutex_unlock(&g_v4l2sl_lock);

    VASurfaceAttrib attribs[16];
    unsigned int count = 0;
    uint32_t pix[4];
    unsigned npix = 0, p;

    pix[npix++] = VA_FOURCC_NV12;
    if (rt & VA_RT_FORMAT_YUV420_10)
        pix[npix++] = VA_FOURCC_P010;
    if (rt & (VA_RT_FORMAT_YUV422 | VA_RT_FORMAT_YUV422_10))
        pix[npix++] = VA_FOURCC_YUY2;
    if (rt & VA_RT_FORMAT_RGB32)
        pix[npix++] = VA_FOURCC_BGRX;

    for (p = 0; p < npix; p++) {
        attribs[count].type          = VASurfaceAttribPixelFormat;
        attribs[count].flags         = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
        attribs[count].value.type    = VAGenericValueTypeInteger;
        attribs[count].value.value.i = (int)pix[p];
        count++;
    }

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

    attribs[count].type          = VASurfaceAttribMemoryType;
    attribs[count].flags         = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    attribs[count].value.type    = VAGenericValueTypeInteger;
    attribs[count].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
#ifdef VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2
                                   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 |
#endif
                                   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
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

    if (format == VA_RT_FORMAT_YUV420 || format == 0)
        format = VA_FOURCC_NV12;
    else if (format == VA_RT_FORMAT_YUV420_10)
        format = VA_FOURCC_P010;
    else if (format == VA_RT_FORMAT_YUV422 || format == VA_RT_FORMAT_YUV422_10)
        format = VA_FOURCC_YUY2;
    else if (format == VA_RT_FORMAT_RGB32)
        format = VA_FOURCC_BGRX;

    pthread_mutex_lock(&g_v4l2sl_lock);
    int created = 0;
    for (int i = 0; i < num_surfaces; i++) {
        struct v4l2sl_surface *surface = calloc(1, sizeof(*surface));
        VASurfaceID id;

        if (!surface)
            goto fail;

        /* Recycle an ID or take the next free one; never index past the
         * fixed table. */
        if (driver_data->n_free_surface_ids > 0)
            id = driver_data->free_surface_ids[--driver_data->n_free_surface_ids];
        else if (driver_data->next_surface_id + 1 < V4L2SL_MAX_SURFACES)
            id = ++driver_data->next_surface_id;
        else {
            free(surface);
            goto fail;
        }
        if (driver_data->surfaces[id]) {
            free(surface);
            goto fail;
        }

        surface->width = width;
        surface->height = height;
        surface->format = format;
        surface->status = VASurfaceReady;
        surface->buf_index = -1;
        surface->dma_buf_fd = -1;
        surface->cpu_stride = v4l2sl_default_image_stride(format, width);
        surface->cpu_size = v4l2sl_va_image_size(format, surface->cpu_stride, height);
        if (surface->cpu_size) {
            surface->cpu_ptr = calloc(1, surface->cpu_size);
            if (!surface->cpu_ptr) {
                free(surface);
                goto fail;
            }
        }
        /* dma-buf at create so Chrome/Firefox DRM-PRIME export works
         * before the first picture. V4L2 EXPBUF replaces this after REQBUFS. */
        if (v4l2sl_surface_alloc_export_fd(surface) < 0)
            fprintf(stderr, "v4l2stateless: surface memfd backing failed\n");

        surfaces[i] = id;
        driver_data->surfaces[id] = surface;
        created++;
    }

    pthread_mutex_unlock(&g_v4l2sl_lock);
    return VA_STATUS_SUCCESS;

fail:
    /* Cleanup stays under the lock and only touches surfaces this call
     * actually created — surfaces[] entries past `created` are the
     * caller's uninitialized memory, never scan them. */
    for (int j = 0; j < created; j++) {
        struct v4l2sl_surface *s = v4l2sl_surface_by_id(driver_data, surfaces[j]);

        if (s) {
            if (s->dma_buf_fd >= 0)
                close(s->dma_buf_fd);
            free(s->cpu_ptr);
            free(s);
            driver_data->surfaces[surfaces[j]] = NULL;
            v4l2sl_surface_id_push(driver_data, surfaces[j]);
            surfaces[j] = VA_INVALID_ID;
        }
    }
    pthread_mutex_unlock(&g_v4l2sl_lock);
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
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

    if (format == VA_RT_FORMAT_YUV420_10)
        fourcc = VA_FOURCC_P010;
    else if (format == VA_RT_FORMAT_YUV422 || format == VA_RT_FORMAT_YUV422_10)
        fourcc = VA_FOURCC_YUY2;
    else if (format == VA_RT_FORMAT_RGB32)
        fourcc = VA_FOURCC_BGRX;

    for (unsigned int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VASurfaceAttribPixelFormat &&
            attrib_list[i].value.type == VAGenericValueTypeInteger)
            fourcc = attrib_list[i].value.value.i;
    }

    if (fourcc != VA_FOURCC_NV12 && fourcc != VA_FOURCC_P010 &&
        fourcc != VA_FOURCC_YUY2 && fourcc != VA_FOURCC_Y210 &&
        fourcc != VA_FOURCC_I420 &&
        fourcc != VA_FOURCC_ARGB && fourcc != VA_FOURCC_BGRA &&
        fourcc != VA_FOURCC_BGRX)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    return v4l2sl_create_surfaces(ctx, width, height, fourcc,
                                  num_surfaces, surfaces);
}

/* Free one surface and recycle its ID. Caller holds g_v4l2sl_lock and
 * has already detached the surface from any context (C1 does that for
 * the explicit destroy path; terminate frees contexts first). */
static void
destroy_surface_locked(struct v4l2sl_driver_data *dd,
                       struct v4l2sl_surface *s, VASurfaceID id)
{
    if (s->dma_buf_fd >= 0)
        close(s->dma_buf_fd);
    v4l2sl_gbm_surface_destroy(s);
    free(s->cpu_ptr);
    free(s);
    dd->surfaces[id] = NULL;
    v4l2sl_surface_id_push(dd, id);
}

static VAStatus
v4l2sl_destroy_surfaces(VADriverContextP ctx,
                        VASurfaceID *surfaces,
                        int num_surfaces)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;

    pthread_mutex_lock(&g_v4l2sl_lock);
    for (int i = 0; i < num_surfaces; i++) {
        struct v4l2sl_surface *surface = v4l2sl_surface_by_id(driver_data,
                                                              surfaces[i]);
        if (surface)
            destroy_surface_locked(driver_data, surface, surfaces[i]);
    }
    pthread_mutex_unlock(&g_v4l2sl_lock);

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

    pthread_mutex_lock(&g_v4l2sl_lock);

    context->config_id = config_id;
    context->driver_data = driver_data;
    context->width = picture_width;
    context->height = picture_height;
    context->num_render_targets = num_render_targets;
    context->request_fd = -1;
    context->render_targets = calloc(num_render_targets, sizeof(VASurfaceID));
    if (!context->render_targets) {
        free(context);
        pthread_mutex_unlock(&g_v4l2sl_lock);
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
        context->profile = config->profile;
        context->entrypoint = config->entrypoint;
        context->rt_format = config->rt_format;
        context->device_path = config->device_path;
    }
    context->v4l2_fd = -1;
    context->media_fd = -1;
    context->request_fd = -1;

    /* Open V4L2 video device */
    context->v4l2_fd = v4l2sl_open_device(context->device_path);
    if (context->v4l2_fd < 0) {
        fprintf(stderr, "v4l2stateless: failed to open %s\n", context->device_path);
        goto fail;
    }

    /* JPEG encode and RGA VPP are stateful M2M — no request API. */
    if (context->codec == V4L2SL_CODEC_JPEG_ENC ||
        context->codec == V4L2SL_CODEC_VPP) {
        *context_id = ++driver_data->next_context_id;
        context->context_id = *context_id;
        context->next = driver_data->contexts;
        driver_data->contexts = context;
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_SUCCESS;
    }

    /* Open media device for request API (sysfs, not /dev/mediaN == videoN) */
    context->media_fd = v4l2sl_open_media_for_device(context->device_path);
    if (context->media_fd < 0) {
        fprintf(stderr, "v4l2stateless: no media request node for %s\n",
                context->device_path);
        goto fail;
    }

    /* Determine V4L2 codec format */
    uint32_t v4l2_format;
    switch (context->codec) {
    case V4L2SL_CODEC_H264:  v4l2_format = V4L2_PIX_FMT_H264_SLICE; break;
    case V4L2SL_CODEC_HEVC:  v4l2_format = V4L2_PIX_FMT_HEVC_SLICE; break;
    case V4L2SL_CODEC_AV1:   v4l2_format = V4L2_PIX_FMT_AV1_FRAME; break;
    case V4L2SL_CODEC_VP8:   v4l2_format = V4L2_PIX_FMT_VP8_FRAME; break;
    case V4L2SL_CODEC_MPEG2: v4l2_format = V4L2_PIX_FMT_MPEG2_SLICE; break;
    default: v4l2_format = V4L2_PIX_FMT_H264_SLICE; break;
    }

    /* Setup output queue (compressed input) */
    int n_out = v4l2sl_setup_output_queue(context->v4l2_fd, v4l2_format,
                                          picture_width, picture_height);
    if (n_out <= 0) {
        fprintf(stderr, "v4l2stateless: failed to setup output queue\n");
        goto fail;
    }
    context->output_bufs_allocd = n_out;
    /* Mmap the output buffers for writing compressed data */
    if (v4l2sl_mmap_output_buffers(context->v4l2_fd, n_out,
                                   context->output_buf_ptr,
                                   &context->output_buf_size) < 0) {
        fprintf(stderr, "v4l2stateless: warning: failed to mmap output buffers\n");
        /* Non-fatal: we'll fall back to per-frame mmap */
    }

    /* Capture: S_FMT + REQBUFS (degrades the buffer count under CMA
     * pressure). Failing the context outright beats a zombie fd that
     * errors on every frame — ffmpeg/Chrome fall back to software.
     *
     * H.264 defers capture until the first picture: rkvdec's SPS s_ctrl
     * updates image_fmt (8-bit vs NV15) and returns EBUSY if capture
     * buffers already exist. GStreamer REQBUFS(0)s first, then SPS,
     * then NV15. HEVC keeps the create-time queue because its SPS is
     * global-only and already sequenced that way. */
    if (context->codec != V4L2SL_CODEC_H264) {
        uint32_t cap = v4l2sl_capture_fourcc_from_rt(context->rt_format);
        int ok = v4l2sl_ensure_capture(context, picture_width, picture_height,
                                       cap) == 0;

        if (!ok && cap != V4L2_PIX_FMT_NV12) {
            /* SPS not set yet — fall back to NV12, first picture will
             * renegotiate once bit depth is known. */
            ok = v4l2sl_ensure_capture(context, picture_width, picture_height,
                                       V4L2_PIX_FMT_NV12) == 0;
        }
        if (!ok) {
            fprintf(stderr, "v4l2stateless: capture queue setup failed\n");
            goto fail;
        }
    }

    /*
     * Start streaming on both queues — QBUF is EPERM before this.
     * HEVC/AV1/MPEG-2 defer STREAMON until the first picture: rkvdec
     * wants the SPS (global) first. H.264 High10 must do the same —
     * STREAMON on NV12 then renegotiating to NV15 leaves rkvdec
     * producing empty 10-bit frames.
     */
    if (context->codec != V4L2SL_CODEC_HEVC &&
        context->codec != V4L2SL_CODEC_AV1 &&
        context->codec != V4L2SL_CODEC_MPEG2 &&
        context->codec != V4L2SL_CODEC_H264 &&
        context->output_bufs_allocd > 0 &&
        context->capture_bufs_allocd > 0) {
        if (v4l2sl_streamon(context->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0 ||
            v4l2sl_streamon(context->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0)
            fprintf(stderr, "v4l2stateless: warning: STREAMON failed, decode will not work\n");
        else
            context->streamed = 1;
    }

    /* All output buffers start free. Capture free list is filled by
     * v4l2sl_ensure_capture. */
    for (int i = 0; i < context->output_bufs_allocd &&
                    i < V4L2SL_NUM_OUTPUT_BUFS; i++)
        v4l2sl_out_pool_push(context, i);

    *context_id = ++driver_data->next_context_id;
    context->context_id = *context_id;
    context->next = driver_data->contexts;
    driver_data->contexts = context;

    pthread_mutex_unlock(&g_v4l2sl_lock);
    return VA_STATUS_SUCCESS;

fail:
    v4l2sl_release_context_device(context);
    free(context->render_targets);
    free(context);
    pthread_mutex_unlock(&g_v4l2sl_lock);
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

static VAStatus
v4l2sl_destroy_context(VADriverContextP ctx, VAContextID context_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_context **pp = &driver_data->contexts;

    pthread_mutex_lock(&g_v4l2sl_lock);
    while (*pp) {
        if ((*pp)->context_id == context_id) {
            struct v4l2sl_context *context = *pp;
            *pp = context->next;

            v4l2sl_release_context_device(context);
            free(context->render_targets);
            free(context);
            pthread_mutex_unlock(&g_v4l2sl_lock);
            return VA_STATUS_SUCCESS;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_v4l2sl_lock);

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
    if (type == VAEncCodedBufferType) {
        /* Layout: VACodedBufferSegment header followed by payload. */
        size_t payload = (size_t)size * (num_elements ? num_elements : 1);
        size_t wrap = sizeof(VACodedBufferSegment) + payload;
        VACodedBufferSegment *seg;
        buf->data = calloc(1, wrap);
        if (!buf->data) {
            free(buf);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        buf->size = (unsigned int)payload;
        seg = buf->data;
        seg->buf = (uint8_t *)buf->data + sizeof(*seg);
        seg->size = 0;
        seg->next = NULL;
    } else {
        buf->data = malloc(size * num_elements);
        if (!buf->data) {
            free(buf);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        if (data)
            memcpy(buf->data, data, size * num_elements);
    }

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
    struct v4l2sl_context *context;
    VAStatus st = VA_STATUS_ERROR_INVALID_BUFFER;

    pthread_mutex_lock(&g_v4l2sl_lock);
    context = driver_data->contexts;
    while (context) {
        struct v4l2sl_buffer *buf = context->buffers;
        while (buf) {
            if (buf->buffer_id == buf_id) {
                if (num_elements > buf->num_elements) {
                    void *new_data = realloc(buf->data, buf->size * num_elements);
                    if (!new_data) {
                        pthread_mutex_unlock(&g_v4l2sl_lock);
                        return VA_STATUS_ERROR_ALLOCATION_FAILED;
                    }
                    buf->data = new_data;
                }
                buf->num_elements = num_elements;
                st = VA_STATUS_SUCCESS;
                goto out;
            }
            buf = buf->next;
        }
        context = context->next;
    }

out:
    pthread_mutex_unlock(&g_v4l2sl_lock);
    return st;
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
    struct v4l2sl_surface *surface;
    struct v4l2sl_context *context;
    VAStatus st;

    pthread_mutex_lock(&g_v4l2sl_lock);
    context = driver_data->contexts;
    while (context && context->context_id != context_id)
        context = context->next;

    if (!context) {
        st = VA_STATUS_ERROR_INVALID_CONTEXT;
        goto out;
    }

    surface = v4l2sl_surface_by_id(driver_data, render_target);
    if (!surface) {
        st = VA_STATUS_ERROR_INVALID_SURFACE;
        goto out;
    }

    context->current_surface = surface;
    context->current_surface_id = render_target;
    context->num_pending_buffers = 0;

    /* If this surface still holds a decoded capture buffer, return it to the
     * free pool — being re-targeted means its previous frame is obsolete
     * (and the client has synced it already, per VA-API contract). */
    if (surface->buf_index >= 0) {
        /* Userspace bookkeeping only: hand the buffer back to the free pool.
         * The kernel QBUF happens exactly once, in the decode path. */
        if (surface->buf_index < context->capture_bufs_allocd)
            v4l2sl_cap_pool_push(context, surface->buf_index);
        surface->buf_index = -1;
        /* Keep memfd so DRM-PRIME clients retain a stable fd. */
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

    st = VA_STATUS_SUCCESS;

out:
    pthread_mutex_unlock(&g_v4l2sl_lock);
    return st;
}

static VAStatus
v4l2sl_render_picture(VADriverContextP ctx,
                      VAContextID context_id,
                      VABufferID *buffers,
                      int num_buffers)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_context *context;
    VAStatus st;

    pthread_mutex_lock(&g_v4l2sl_lock);
    context = driver_data->contexts;
    while (context && context->context_id != context_id)
        context = context->next;

    if (!context) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    /* Collect buffer references for this picture */
    for (int i = 0; i < num_buffers; i++) {
        /* Find the buffer in context */
        struct v4l2sl_buffer *b = context->buffers;
        while (b) {
            if (b->buffer_id == buffers[i]) {
                if (context->num_pending_buffers < 256) {
                    context->pending_buffers[context->num_pending_buffers++] = b;
                } else {
                    fprintf(stderr, "v4l2stateless: pending buffer overflow\n");
                    pthread_mutex_unlock(&g_v4l2sl_lock);
                    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
                }
                break;
            }
            b = b->next;
        }

        if (!b) {
            pthread_mutex_unlock(&g_v4l2sl_lock);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
    }

    st = VA_STATUS_SUCCESS;
    pthread_mutex_unlock(&g_v4l2sl_lock);
    return st;
}

/*
 * Decode pipeline — the per-frame V4L2 submit/poll/DQBUF lives in
 * v4l2sl_decode_submit (v4l2stateless_device.c); vaEndPicture calls it via
 * the codec translators.
 */
static VAStatus
v4l2sl_end_picture(VADriverContextP ctx,
                   VAContextID context_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_context *context;
    VAStatus va_status;

    pthread_mutex_lock(&g_v4l2sl_lock);
    context = driver_data->contexts;
    while (context && context->context_id != context_id)
        context = context->next;

    if (!context) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (!context->current_surface) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (context->codec == V4L2SL_CODEC_JPEG_ENC) {
        va_status = v4l2sl_jpeg_encode(context, context->pending_buffers,
                                       context->num_pending_buffers);
        context->current_surface->status =
            (va_status == VA_STATUS_SUCCESS) ? VASurfaceReady : VASurfaceSkipped;
        context->current_surface = NULL;
        context->num_pending_buffers = 0;
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return va_status;
    }
    if (context->codec == V4L2SL_CODEC_VPP) {
        va_status = v4l2sl_vpp_run(context, context->pending_buffers,
                                   context->num_pending_buffers);
        context->current_surface->status =
            (va_status == VA_STATUS_SUCCESS) ? VASurfaceReady : VASurfaceSkipped;
        context->current_surface = NULL;
        context->num_pending_buffers = 0;
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return va_status;
    }

    if (context->v4l2_fd < 0 || context->request_fd < 0) {
        /* V4L2 device not available — just mark surface as ready (stub mode) */
        context->current_surface->status = VASurfaceReady;
        context->current_surface = NULL;
        context->num_pending_buffers = 0;
        if (context->request_fd >= 0) {
            close(context->request_fd);
            context->request_fd = -1;
        }
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_SUCCESS;
    }

    /* Call codec-specific translation */
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
    case V4L2SL_CODEC_VP8:
        va_status = v4l2sl_vp8_translate(context,
                                         context->pending_buffers,
                                         context->num_pending_buffers);
        break;
    case V4L2SL_CODEC_MPEG2:
        va_status = v4l2sl_mpeg2_translate(context,
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
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return va_status;
    }

    /* Mark surface as rendering (will become Ready after sync) */
    context->current_surface->status = VASurfaceRendering;
    context->current_surface = NULL;
    context->num_pending_buffers = 0;

    pthread_mutex_unlock(&g_v4l2sl_lock);
    /* request_fd lifecycle is owned by decode_submit / begin_picture */
    return VA_STATUS_SUCCESS;
}

/*
 * Surface sync/status — wait for V4L2 decode to complete
 */
static VAStatus
v4l2sl_sync_surface(VADriverContextP ctx, VASurfaceID render_target)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_surface *surface;

    pthread_mutex_lock(&g_v4l2sl_lock);
    surface = v4l2sl_surface_by_id(driver_data, render_target);
    if (!surface) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    /* Decoding runs synchronously inside vaEndPicture (single request in
     * flight), so by the time the client syncs, the frame — if it decoded —
     * is already attached to the surface as a capture-buffer DMA-BUF. */
    surface->status = VASurfaceReady;

    pthread_mutex_unlock(&g_v4l2sl_lock);
    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_query_surface_status(VADriverContextP ctx,
                            VASurfaceID render_target,
                            VASurfaceStatus *status)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_surface *surface;
    VAStatus st = VA_STATUS_ERROR_INVALID_SURFACE;

    pthread_mutex_lock(&g_v4l2sl_lock);
    surface = v4l2sl_surface_by_id(driver_data, render_target);
    if (surface) {
        *status = surface->status;
        st = VA_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&g_v4l2sl_lock);
    return st;
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
    static const struct {
        uint32_t fourcc;
        int depth;
        int bpp;
    } fmts[] = {
        { VA_FOURCC_NV12, 12, 12 },
        { VA_FOURCC_P010, 24, 24 },
        { VA_FOURCC_YUY2, 16, 16 },
        { VA_FOURCC_Y210, 20, 32 },
        { VA_FOURCC_I420, 12, 12 },
        { VA_FOURCC_BGRX, 32, 32 },
        { VA_FOURCC_BGRA, 32, 32 },
        { VA_FOURCC_ARGB, 32, 32 },
    };
    int count = 0;
    unsigned i;
    int cap = num_formats ? *num_formats : 0;

    for (i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        if (format_list && count < cap) {
            memset(&format_list[count], 0, sizeof(VAImageFormat));
            format_list[count].fourcc = fmts[i].fourcc;
            format_list[count].depth = fmts[i].depth;
            format_list[count].bits_per_pixel = fmts[i].bpp;
        }
        count++;
    }

    if (num_formats)
        *num_formats = count;
    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_derive_image(VADriverContextP ctx,
                    VASurfaceID surface,
                    VAImage *image)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_surface *surf;

    pthread_mutex_lock(&g_v4l2sl_lock);
    surf = v4l2sl_surface_by_id(driver_data, surface);

    if (!surf) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if ((surf->dma_buf_fd < 0 || surf->buf_index < 0) && !surf->cpu_ptr) {
        fprintf(stderr, "v4l2stateless: derive_image: surface %d has no decoded frame\n",
                surface);
        pthread_mutex_unlock(&g_v4l2sl_lock);
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
    uint32_t cap_fcc = surf->cap_fourcc ? surf->cap_fourcc :
                       ((c && c->cap_pixelformat) ? c->cap_pixelformat : V4L2_PIX_FMT_NV12);
    uint32_t va_fcc = v4l2sl_va_fourcc_for_capture(cap_fcc);
    uint32_t data_size;
    void *map;
    int mmapped = 0;

    if (surf->cpu_ptr && surf->buf_index < 0) {
        map = surf->cpu_ptr;
        stride = surf->cpu_stride;
        aligned_h = surf->height;
        va_fcc = surf->format ? surf->format : VA_FOURCC_NV12;
        data_size = surf->cpu_size;
        mmapped = 2; /* borrowed cpu backing — do not free on DestroyImage */
    } else {
        v4l2sl_surface_ensure_memfd(surf);
        data_size = v4l2sl_capture_plane_size(cap_fcc, stride, aligned_h);
        map = mmap(NULL, data_size, PROT_READ, MAP_SHARED, surf->dma_buf_fd, 0);
        if (map == MAP_FAILED) {
            fprintf(stderr, "v4l2stateless: derive_image: mmap dmabuf failed: %s\n",
                    strerror(errno));
            pthread_mutex_unlock(&g_v4l2sl_lock);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        mmapped = 1;
        /* Derive reports the native V4L2 layout so zero-copy clients can
         * consume NV15/NV16; GetImage converts to P010/YUY2. */
        if (cap_fcc == V4L2_PIX_FMT_NV12)
            va_fcc = VA_FOURCC_NV12;
        else if (cap_fcc == V4L2_PIX_FMT_NV15)
            va_fcc = VA_FOURCC_P010;
        else
            va_fcc = VA_FOURCC_YUY2;
    }

    struct v4l2sl_buffer *ib = calloc(1, sizeof(*ib));
    if (!ib) {
        munmap(map, data_size);
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    VABufferID id = ++driver_data->next_buffer_id;
    ib->buffer_id = id;
    ib->type = VAImageBufferType;
    ib->size = data_size;
    ib->data = map;
    ib->mmapped = mmapped;
    ib->next = driver_data->orphan_buffers;
    driver_data->orphan_buffers = ib;

    memset(image, 0, sizeof(*image));
    image->image_id = id;
    image->buf = id;               /* vaMapBuffer(image.buf) returns the mapping */
    image->format.fourcc = va_fcc;
    image->width = surf->width;    /* display size */
    image->height = surf->height;
    if (va_fcc == VA_FOURCC_YUY2 || va_fcc == VA_FOURCC_BGRX ||
        va_fcc == VA_FOURCC_BGRA || va_fcc == VA_FOURCC_ARGB) {
        image->num_planes = 1;
        image->pitches[0] = (va_fcc == VA_FOURCC_YUY2) ? stride : stride;
        image->offsets[0] = 0;
    } else {
        image->num_planes = 2;
        image->pitches[0] = stride;
        image->pitches[1] = stride;
        image->offsets[0] = 0;
        image->offsets[1] = stride * aligned_h;
    }
    image->data_size = data_size;

    pthread_mutex_unlock(&g_v4l2sl_lock);
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
            if (ib->mmapped == 1)
                munmap(ib->data, ib->size);
            else if (ib->mmapped == 0)
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
    struct v4l2sl_buffer *ib;
    uint32_t stride, aligned_h, data_size;
    VABufferID id;

    if (!format)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (format->fourcc != VA_FOURCC_NV12 && format->fourcc != VA_FOURCC_P010 &&
        format->fourcc != VA_FOURCC_YUY2 && format->fourcc != VA_FOURCC_Y210 &&
        format->fourcc != VA_FOURCC_I420 &&
        format->fourcc != VA_FOURCC_BGRX && format->fourcc != VA_FOURCC_BGRA &&
        format->fourcc != VA_FOURCC_ARGB)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (width <= 0 || height <= 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    stride = v4l2sl_default_image_stride(format->fourcc, width);
    aligned_h = height;
    data_size = v4l2sl_va_image_size(format->fourcc, stride, aligned_h);

    ib = calloc(1, sizeof(*ib));
    if (!ib)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    ib->data = malloc(data_size);
    if (!ib->data) {
        free(ib);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    pthread_mutex_lock(&g_v4l2sl_lock);
    id = ++driver_data->next_buffer_id;
    ib->buffer_id = id;
    ib->type = VAImageBufferType;
    ib->size = data_size;
    ib->fourcc = format->fourcc;
    ib->next = driver_data->orphan_buffers;
    driver_data->orphan_buffers = ib;
    pthread_mutex_unlock(&g_v4l2sl_lock);

    memset(image, 0, sizeof(*image));
    image->image_id = id;
    image->buf = id;
    image->format = *format;
    image->width = width;
    image->height = height;
    if (format->fourcc == VA_FOURCC_YUY2 || format->fourcc == VA_FOURCC_Y210 ||
        format->fourcc == VA_FOURCC_BGRX ||
        format->fourcc == VA_FOURCC_BGRA || format->fourcc == VA_FOURCC_ARGB) {
        image->num_planes = 1;
        image->pitches[0] = stride;
        image->offsets[0] = 0;
    } else {
        image->num_planes = 2;
        image->pitches[0] = stride;
        image->pitches[1] = stride;
        image->offsets[0] = 0;
        image->offsets[1] = stride * aligned_h;
    }
    image->data_size = data_size;

    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_get_image(VADriverContextP ctx, VASurfaceID surface,
                 int x, int y, unsigned int width, unsigned int height,
                 VAImageID image_id)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_surface *surf;
    struct v4l2sl_buffer *ib;

    pthread_mutex_lock(&g_v4l2sl_lock);
    surf = v4l2sl_surface_by_id(driver_data, surface);
    if (!surf) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (x != 0 || y != 0) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    ib = driver_data->orphan_buffers;
    while (ib && ib->buffer_id != image_id)
        ib = ib->next;
    if (!ib) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    struct v4l2sl_context *c = context_for_surface(driver_data, surface);
    uint32_t src_stride = surf->stride ? surf->stride :
                          ((c && c->cap_stride) ? c->cap_stride : surf->width);
    uint32_t src_alh = surf->aligned_h ? surf->aligned_h :
                       ((c && c->cap_height) ? c->cap_height : surf->height);
    uint32_t cap_fcc = surf->cap_fourcc ? surf->cap_fourcc :
                       ((c && c->cap_pixelformat) ? c->cap_pixelformat : V4L2_PIX_FMT_NV12);
    uint8_t *dst = ib->data;
    const uint8_t *src;
    void *mapped = NULL;
    size_t map_size = 0;
    int copy_w = (int)width < surf->width ? (int)width : surf->width;
    int copy_h = (int)height < surf->height ? (int)height : surf->height;
    uint32_t dst_fourcc = ib->fourcc;
    uint32_t dst_stride;

    if (!dst_fourcc) {
        /* Legacy default when the image was made without a recorded fourcc */
        if (cap_fcc == V4L2_PIX_FMT_NV15)
            dst_fourcc = VA_FOURCC_P010;
        else if (cap_fcc == V4L2_PIX_FMT_NV16 || cap_fcc == V4L2_PIX_FMT_NV20)
            dst_fourcc = VA_FOURCC_YUY2;
        else
            dst_fourcc = VA_FOURCC_NV12;
    }
    dst_stride = v4l2sl_default_image_stride(dst_fourcc, copy_w);

    /* Lazy memfd: bo-backed surfaces skip the per-frame memfd copy;
     * refill it from the bo now that someone is reading back. */
    if (surf->buf_index >= 0)
        v4l2sl_surface_ensure_memfd(surf);

    if (surf->dma_buf_fd >= 0 && surf->buf_index >= 0) {
        map_size = v4l2sl_capture_plane_size(cap_fcc, src_stride, src_alh);
        mapped = mmap(NULL, map_size, PROT_READ, MAP_SHARED, surf->dma_buf_fd, 0);
        if (mapped == MAP_FAILED) {
            fprintf(stderr, "v4l2stateless: get_image: mmap dmabuf failed: %s\n",
                    strerror(errno));
            pthread_mutex_unlock(&g_v4l2sl_lock);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        src = mapped;
    } else if (surf->cpu_ptr) {
        src = surf->cpu_ptr;
        src_stride = surf->cpu_stride;
        src_alh = surf->height;
        cap_fcc = V4L2_PIX_FMT_NV12;
        dst_fourcc = VA_FOURCC_NV12;
        dst_stride = v4l2sl_default_image_stride(VA_FOURCC_NV12, copy_w);
    } else {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (dst_fourcc == VA_FOURCC_P010 && cap_fcc == V4L2_PIX_FMT_NV15)
        v4l2sl_nv15_to_p010(dst, dst_stride, src, src_stride, src_alh, copy_w, copy_h);
    else if (dst_fourcc == VA_FOURCC_Y210 && cap_fcc == V4L2_PIX_FMT_NV20)
        v4l2sl_nv20_to_y210(dst, dst_stride, src, src_stride, src_alh, copy_w, copy_h);
    else if (dst_fourcc == VA_FOURCC_YUY2 && cap_fcc == V4L2_PIX_FMT_NV20)
        v4l2sl_nv20_to_yuy2(dst, dst_stride, src, src_stride, src_alh, copy_w, copy_h);
    else if (dst_fourcc == VA_FOURCC_YUY2 && cap_fcc == V4L2_PIX_FMT_NV16)
        v4l2sl_nv16_to_yuy2(dst, dst_stride, src, src_stride, src_alh, copy_w, copy_h);
    else if (dst_fourcc == VA_FOURCC_NV12 && cap_fcc == V4L2_PIX_FMT_NV12)
        v4l2sl_copy_nv12(dst, dst_stride, src, src_stride, src_alh, copy_w, copy_h);
    else {
        fprintf(stderr, "v4l2stateless: get_image: no path cap=%.4s -> %.4s\n",
                (char *)&cap_fcc, (char *)&dst_fourcc);
        pthread_mutex_unlock(&g_v4l2sl_lock);
        if (mapped)
            munmap(mapped, map_size);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    if (mapped)
        munmap(mapped, map_size);
    pthread_mutex_unlock(&g_v4l2sl_lock);
    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_put_image(VADriverContextP ctx,
                 VASurfaceID surface,
                 VAImageID image,
                 int src_x, int src_y,
                 unsigned int src_width, unsigned int src_height,
                 int dest_x, int dest_y,
                 unsigned int dest_width, unsigned int dest_height)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_surface *surf;
    struct v4l2sl_buffer *ib;
    uint8_t *src;
    int copy_w, copy_h;

    (void)dest_width;
    (void)dest_height;

    pthread_mutex_lock(&g_v4l2sl_lock);
    surf = v4l2sl_surface_by_id(driver_data, surface);
    if (!surf) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (src_x || src_y || dest_x || dest_y) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    if (!surf->cpu_ptr) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    ib = driver_data->orphan_buffers;
    while (ib && ib->buffer_id != image)
        ib = ib->next;
    if (!ib) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    src = ib->data;
    copy_w = (int)src_width < surf->width ? (int)src_width : surf->width;
    copy_h = (int)src_height < surf->height ? (int)src_height : surf->height;
    v4l2sl_copy_nv12(surf->cpu_ptr, surf->cpu_stride, src,
                     (uint32_t)copy_w, copy_h, copy_w, copy_h);
    surf->gbm_src = 1;
    if (surf->gbm_bo)
        v4l2sl_gbm_surface_upload(surf, surf->cpu_ptr, surf->cpu_stride,
                                  surf->height);
    pthread_mutex_unlock(&g_v4l2sl_lock);
    return VA_STATUS_SUCCESS;
}

static VAStatus
v4l2sl_vpp_query_filters_wrap(VADriverContextP ctx, VAContextID context,
                              VAProcFilterType *filters, unsigned int *num_filters)
{
    (void)ctx;
    (void)context;
    return v4l2sl_vpp_query_filters(filters, num_filters);
}

static VAStatus
v4l2sl_vpp_query_filter_caps_wrap(VADriverContextP ctx, VAContextID context,
                                  VAProcFilterType type, void *filter_caps,
                                  unsigned int *num_filter_caps)
{
    (void)ctx;
    (void)context;
    return v4l2sl_vpp_query_filter_caps(type, filter_caps, num_filter_caps);
}

static VAStatus
v4l2sl_vpp_query_pipeline_caps_wrap(VADriverContextP ctx, VAContextID context,
                                    VABufferID *filters, unsigned int num_filters,
                                    VAProcPipelineCaps *pipeline_caps)
{
    (void)ctx;
    (void)context;
    (void)filters;
    (void)num_filters;
    return v4l2sl_vpp_query_pipeline_caps(pipeline_caps);
}

static VAStatus
v4l2sl_export_surface_handle(VADriverContextP ctx, VASurfaceID surface_id,
                             uint32_t mem_type, uint32_t flags, void *descriptor)
{
    struct v4l2sl_driver_data *driver_data = ctx->pDriverData;
    struct v4l2sl_surface *surf;
    struct v4l2sl_context *c;
    VAStatus st;

    pthread_mutex_lock(&g_v4l2sl_lock);
    surf = v4l2sl_surface_by_id(driver_data, surface_id);

    if (!surf) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (!descriptor) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 &&
        mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    /*
     * Decode surfaces: VPU capture buffers must never be exported (EXPBUF
     * + GPU import is a chip bug), and handing the memfd out as DRM_PRIME
     * is the black-frame lie. Export the driver-owned linear GBM copy
     * instead - a real dma-buf in the single-object NV12 shape Chrome's
     * zero-copy GL path imports. Decode surface = attached capture buffer
     * or member of a VLD context (cpu_ptr is NOT a discriminator:
     * create_surfaces callocs it for every surface). Falls back to
     * UNIMPLEMENTED when GBM is unavailable or the format is not NV12.
     */
    c = context_for_surface(driver_data, surface_id);
    if (surf->buf_index >= 0 ||
        (c && c->entrypoint == VAEntrypointVLD) ||
        surf->format == VA_FOURCC_NV12) {
        if (v4l2sl_gbm_surface_ensure(surf) < 0) {
            pthread_mutex_unlock(&g_v4l2sl_lock);
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        st = v4l2sl_surface_fill_prime_gbm(surf, flags, descriptor);
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return st;
    }
    if (surf->dma_buf_fd < 0)
        v4l2sl_surface_alloc_export_fd(surf);
    if (surf->dma_buf_fd < 0) {
        pthread_mutex_unlock(&g_v4l2sl_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    st = v4l2sl_surface_fill_prime(surf, c, flags, descriptor);
    pthread_mutex_unlock(&g_v4l2sl_lock);
    return st;
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

    ctx->pDriverData = driver_data;

    v4l2sl_scan_all_cached(driver_data->dev_h264, driver_data->dev_hevc,
                           driver_data->dev_av1, driver_data->dev_vp8,
                           driver_data->dev_mpeg2,
                           driver_data->dev_jpeg_enc, driver_data->dev_vpp,
                           sizeof(driver_data->dev_h264));
    fprintf(stderr, "v4l2stateless: probe H.264  -> %s\n",
            driver_data->dev_h264[0] ? driver_data->dev_h264 : "(none)");
    fprintf(stderr, "v4l2stateless: probe HEVC   -> %s\n",
            driver_data->dev_hevc[0] ? driver_data->dev_hevc : "(none)");
    /* Do not publish AV1 to libva clients. Probe still sees the node. */
    driver_data->dev_av1[0] = 0;
    fprintf(stderr, "v4l2stateless: probe AV1    -> (not advertised)\n");
    fprintf(stderr, "v4l2stateless: probe VP8    -> %s\n",
            driver_data->dev_vp8[0] ? driver_data->dev_vp8 : "(none)");
    fprintf(stderr, "v4l2stateless: probe MPEG-2 -> %s\n",
            driver_data->dev_mpeg2[0] ? driver_data->dev_mpeg2 : "(none)");
    fprintf(stderr, "v4l2stateless: probe JPEG   -> %s\n",
            driver_data->dev_jpeg_enc[0] ? driver_data->dev_jpeg_enc : "(none)");
    fprintf(stderr, "v4l2stateless: probe VPP    -> %s\n",
            driver_data->dev_vpp[0] ? driver_data->dev_vpp : "(none)");

    /* Set context limits. Chrome sizes vaQueryConfigAttributes from
     * vaMaxNumConfigAttributes() == max_attributes. */
    ctx->max_profiles = NUM_PROFILES;
    ctx->max_entrypoints = 4;
    ctx->max_attributes = 32;
    ctx->max_image_formats = 8;
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
    vtable->vaPutImage                = v4l2sl_put_image;
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
    vtable->vaExportSurfaceHandle     = v4l2sl_export_surface_handle;

    if (ctx->vtable_vpp) {
        ctx->vtable_vpp->version = VA_DRIVER_VTABLE_VPP_VERSION;
        ctx->vtable_vpp->vaQueryVideoProcFilters = v4l2sl_vpp_query_filters_wrap;
        ctx->vtable_vpp->vaQueryVideoProcFilterCaps = v4l2sl_vpp_query_filter_caps_wrap;
        ctx->vtable_vpp->vaQueryVideoProcPipelineCaps = v4l2sl_vpp_query_pipeline_caps_wrap;
    }

    return VA_STATUS_SUCCESS;
}

/*
 * libva entry point
 */
VAStatus __vaDriverInit_1_20(VADriverContextP ctx)
{
    return v4l2sl_init(ctx);
}
