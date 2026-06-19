/*
 * v4l2stateless — VA-API to V4L2 Request API bridge
 * Internal data structures
 */

#ifndef V4L2STATELESS_H
#define V4L2STATELESS_H

#include <va/va.h>
#include <va/va_backend.h>

/* Codec identifiers */
enum v4l2sl_codec {
    V4L2SL_CODEC_H264,
    V4L2SL_CODEC_HEVC,
    V4L2SL_CODEC_AV1,
    V4L2SL_CODEC_VP8,
    V4L2SL_CODEC_MPEG2,
};

/* Per-config state */
struct v4l2sl_config {
    VAConfigID config_id;
    VAProfile profile;
    VAEntrypoint entrypoint;
    enum v4l2sl_codec codec;
    unsigned int rt_format;
    const char *device_path;
    struct v4l2sl_config *next;
};

/* Per-surface state */
struct v4l2sl_surface {
    VASurfaceID surface_id;
    unsigned int width;
    unsigned int height;
    unsigned int format;
    VASurfaceStatus status;
    int buf_index;           /* V4L2 buffer index, -1 if not allocated */
    int dma_buf_fd;          /* DMA-BUF fd for export */
};

/* Parameter buffer */
struct v4l2sl_buffer {
    VABufferID buffer_id;
    VABufferType type;
    unsigned int size;
    unsigned int num_elements;
    void *data;
    struct v4l2sl_buffer *next;
};

/* Per-context state (one decode session) */
struct v4l2sl_context {
    VAContextID context_id;
    VAConfigID config_id;
    enum v4l2sl_codec codec;
    const char *device_path;
    int width;
    int height;

    int v4l2_fd;                 /* V4L2 video device fd */
    int media_fd;                /* V4L2 media device fd (for request alloc) */
    int request_fd;              /* Current V4L2 request */

    VASurfaceID *render_targets;
    int num_render_targets;

    struct v4l2sl_surface *current_surface;
    VASurfaceID current_surface_id;
    struct v4l2sl_buffer *pending_buffers;

    struct v4l2sl_buffer *buffers;  /* Attached parameter buffers */
    struct v4l2sl_context *next;
};

/* Driver global state */
struct v4l2sl_driver_data {
    int media_fd;                /* /dev/media0 */

    struct v4l2sl_config *configs;
    VAConfigID next_config_id;

    /* Simple surface table (could use hash map for production) */
    struct v4l2sl_surface *surfaces[4096];
    VASurfaceID next_surface_id;

    struct v4l2sl_context *contexts;
    VAContextID next_context_id;

    VABufferID next_buffer_id;
    struct v4l2sl_buffer *orphan_buffers;
};

/* Device helpers (v4l2stateless_device.c) */
int v4l2sl_open_device(const char *path);
int v4l2sl_open_media_for_device(const char *video_path);

/* Codec-specific translation (v4l2stateless_h264.c, etc.) */
VAStatus v4l2sl_h264_translate(struct v4l2sl_context *ctx,
                               struct v4l2sl_buffer **buffers,
                               int num_buffers);
VAStatus v4l2sl_hevc_translate(struct v4l2sl_context *ctx,
                               struct v4l2sl_buffer **buffers,
                               int num_buffers);
VAStatus v4l2sl_av1_translate(struct v4l2sl_context *ctx,
                              struct v4l2sl_buffer **buffers,
                              int num_buffers);

#endif /* V4L2STATELESS_H */
