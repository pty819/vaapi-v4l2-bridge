/*
 * v4l2stateless — VA-API to V4L2 Request API bridge
 * Internal data structures and function declarations
 */

#ifndef V4L2STATELESS_H
#define V4L2STATELESS_H

#include <stdint.h>
#include <va/va.h>
#include <va/va_backend.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

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
    int buf_index;           /* V4L2 capture buffer index, -1 if not allocated */
    int dma_buf_fd;          /* DMA-BUF fd for export */
    uint64_t timestamp;      /* V4L2 timestamp for reference tracking */
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

/* Number of output/capture buffer slots */
#define V4L2SL_NUM_OUTPUT_BUFS  4
#define V4L2SL_NUM_CAPTURE_BUFS 8

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

    /* Collected buffers for current picture */
    struct v4l2sl_buffer *pending_buffers[32];
    int num_pending_buffers;

    struct v4l2sl_buffer *buffers;  /* Attached parameter buffers */
    struct v4l2sl_context *next;

    /* Output (bitstream) buffer management */
    int output_bufs_allocd;
    uint32_t output_buf_size;
    void  *output_buf_ptr[V4L2SL_NUM_OUTPUT_BUFS];

    /* Capture (decoded frame) buffer management */
    int capture_bufs_allocd;

    /* Frame counter for V4L2 timestamps (DPB reference matching) */
    uint64_t frame_count;
};

/* Driver global state */
struct v4l2sl_driver_data {
    int media_fd;                /* /dev/media0 */

    struct v4l2sl_config *configs;
    VAConfigID next_config_id;

    /* Simple surface table */
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
int v4l2sl_request_alloc(int media_fd);
int v4l2sl_setup_output_queue(int fd, uint32_t codec_format, int width, int height);
int v4l2sl_setup_capture_queue(int fd, int width, int height);
int v4l2sl_queue_output(int fd, int buf_index, const uint8_t *data, uint32_t size,
                        int request_fd, uint64_t timestamp);
int v4l2sl_queue_capture(int fd, int buf_index, int request_fd);
int v4l2sl_export_dmabuf(int fd, int buf_index);
int v4l2sl_dequeue_buffer(int fd, enum v4l2_buf_type type);
int v4l2sl_mmap_output_buffers(int fd, int count, void **ptrs, uint32_t *size_out);
int v4l2sl_set_request_controls(int request_fd, int v4l2_fd, struct v4l2_ext_controls *ctrls);
int v4l2sl_submit_request(int request_fd);

/* H.264 translation (v4l2stateless_h264.c) */
void h264_fill_sps(struct v4l2_ctrl_h264_sps *sps, const VAPictureParameterBufferH264 *pic);
void h264_fill_pps(struct v4l2_ctrl_h264_pps *pps, const VAPictureParameterBufferH264 *pic);
void h264_fill_decode_params(struct v4l2_ctrl_h264_decode_params *dec,
                             const VAPictureParameterBufferH264 *pic,
                             struct v4l2sl_driver_data *dd);
void h264_fill_scaling_matrix(struct v4l2_ctrl_h264_scaling_matrix *sm,
                              const VAIQMatrixBufferH264 *iq);
void h264_fill_slice_params(struct v4l2_ctrl_h264_slice_params *sp,
                            const VASliceParameterBufferH264 *slice,
                            const VAPictureParameterBufferH264 *pic,
                            struct v4l2sl_driver_data *dd);
VAStatus v4l2sl_h264_translate(struct v4l2sl_context *ctx,
                               struct v4l2sl_buffer **buffers,
                               int num_buffers);

/* HEVC translation (v4l2stateless_hevc.c) */
VAStatus v4l2sl_hevc_translate(struct v4l2sl_context *ctx,
                               struct v4l2sl_buffer **buffers,
                               int num_buffers);

/* AV1 translation (v4l2stateless_av1.c) */
VAStatus v4l2sl_av1_translate(struct v4l2sl_context *ctx,
                              struct v4l2sl_buffer **buffers,
                              int num_buffers);

#endif /* V4L2STATELESS_H */
