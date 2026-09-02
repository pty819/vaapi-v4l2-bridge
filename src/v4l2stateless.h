/*
 * v4l2stateless — VA-API to V4L2 Request API bridge
 * Internal data structures and function declarations
 */

#ifndef V4L2STATELESS_H
#define V4L2STATELESS_H

#include <stdint.h>
#include <pthread.h>
#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_dec_vp8.h>
#include <va/va_vpp.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

/* Codec identifiers */
enum v4l2sl_codec {
    V4L2SL_CODEC_H264,
    V4L2SL_CODEC_HEVC,
    V4L2SL_CODEC_AV1,
    V4L2SL_CODEC_VP8,
    V4L2SL_CODEC_MPEG2,
    V4L2SL_CODEC_JPEG_ENC,
    V4L2SL_CODEC_VPP,
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
    unsigned int format;     /* VA fourcc (NV12/P010/YUY2/ARGB) */
    unsigned int rt_format;
    VASurfaceStatus status;
    int buf_index;           /* V4L2 capture buffer index, -1 if not allocated */
    int dma_buf_fd;          /* DMA-BUF fd for export */
    uint64_t timestamp;      /* V4L2 timestamp for reference tracking */
    uint32_t order_hint;     /* AV1 OrderHint of the frame last decoded here */
    uint8_t av1_level1;      /* AV1 KEY / level-1 ARF (hidden skip=0,0) */
    uint32_t stride;         /* negotiated capture geometry, set at decode */
    uint32_t aligned_h;
    uint32_t cap_fourcc;     /* V4L2 capture fourcc of attached decode buf */
    void *cpu_ptr;           /* software backing for upload / encode / VPP src */
    uint32_t cpu_size;
    uint32_t cpu_stride;
};

/* Parameter buffer */
struct v4l2sl_buffer {
    VABufferID buffer_id;
    VABufferType type;
    unsigned int size;
    unsigned int num_elements;
    void *data;
    int mmapped;             /* data from mmap (derive_image) vs malloc */
    uint32_t fourcc;         /* VAImageBufferType: fourcc of the image */
    struct v4l2sl_buffer *next;
};

/* Number of output/capture buffer slots */
#define V4L2SL_NUM_OUTPUT_BUFS  4
#define V4L2SL_NUM_CAPTURE_BUFS 24

/* Surface table size; IDs are recycled via a free stack so a long-lived
 * process can never index past the table. */
#define V4L2SL_MAX_SURFACES 4096

/* Per-context state (one decode session) */
struct v4l2sl_context {
    VAContextID context_id;
    VAConfigID config_id;
    enum v4l2sl_codec codec;
    VAProfile profile;
    VAEntrypoint entrypoint;
    unsigned int rt_format;
    const char *device_path;
    struct v4l2sl_driver_data *driver_data;  /* back-pointer for surface lookup */
    int width;
    int height;

    int v4l2_fd;                 /* V4L2 video device fd */
    int media_fd;                /* V4L2 media device fd (for request alloc) */
    int request_fd;              /* Current V4L2 request */

    VASurfaceID *render_targets;
    int num_render_targets;

    struct v4l2sl_surface *current_surface;
    VASurfaceID current_surface_id;

    /* Collected buffers for current picture. MPEG-2 can be one slice
     * per MB row plus param/IQ buffers, so 32 is not enough. */
    struct v4l2sl_buffer *pending_buffers[256];
    int num_pending_buffers;

    struct v4l2sl_buffer *buffers;  /* Attached parameter buffers */
    struct v4l2sl_context *next;

    /* Output (bitstream) buffer management */
    int output_bufs_allocd;
    uint32_t output_buf_size;
    uint32_t output_buf_length[V4L2SL_NUM_OUTPUT_BUFS];
    void  *output_buf_ptr[V4L2SL_NUM_OUTPUT_BUFS];
    int free_out_bufs[V4L2SL_NUM_OUTPUT_BUFS];
    int n_free_out;

    /* Capture (decoded frame) buffer management */
    int capture_bufs_allocd;
    int streamed;         /* STREAMON done (HEVC defers it past SPS) */
    void *capture_buf_ptr[V4L2SL_NUM_CAPTURE_BUFS];
    uint32_t capture_buf_size;
    int capture_buf_anon; /* calloc stub when ioctl hook is installed */
    int free_cap_bufs[V4L2SL_NUM_CAPTURE_BUFS];
    int n_free_cap;
    uint32_t cap_width;        /* driver-chosen capture geometry */
    uint32_t cap_height;       /* aligned height (e.g. 1088 for 1080p) */
    uint32_t cap_stride;       /* bytes per row, may exceed width */
    uint32_t cap_sizeimage;    /* per-buffer plane size */
    uint32_t cap_pixelformat;  /* V4L2 capture fourcc (NV12/NV15/NV16/NV20) */

    /* Frame counter for V4L2 timestamps (DPB reference matching) */
    uint64_t frame_count;

    /* AV1 refresh_frame_flags inference: VA does not expose the bitmask.
     * 0 = unknown, 1 = libaom-style (free slots from index 1),
     * 2 = SVT-AV1 RA pyramid. */
    uint8_t av1_style;
    uint8_t av1_gop;           /* first hidden ARF order_hint (mini-GOP) */
    uint8_t av1_l0_toggle;     /* SVT L0 slots 0-1-2 */
    uint8_t av1_l1_toggle;     /* SVT L1 slots 3-4 */
    uint8_t av1_have_first_arf;
    uint32_t av1_l0_oh;        /* last SVT L0 ARF order_hint */
    uint32_t av1_prev_l0_oh;   /* previous L0 ARF (mini-GOP length) */
};

/* Driver global state */
struct v4l2sl_driver_data {
    int media_fd;                /* unused leftover; decode uses per-context media_fd */
    pthread_mutex_t lock;        /* unused — see g_v4l2sl_lock in v4l2stateless.c */

    struct v4l2sl_config *configs;
    VAConfigID next_config_id;

    /* Simple surface table */
    struct v4l2sl_surface *surfaces[V4L2SL_MAX_SURFACES];
    VASurfaceID next_surface_id;
    VASurfaceID free_surface_ids[V4L2SL_MAX_SURFACES];
    int n_free_surface_ids;

    struct v4l2sl_context *contexts;
    VAContextID next_context_id;

    VABufferID next_buffer_id;
    struct v4l2sl_buffer *orphan_buffers;

    /* Resolved at init by OUTPUT_MPLANE fourcc (empty if missing). */
    char dev_h264[64];
    char dev_hevc[64];
    char dev_av1[64];
    char dev_vp8[64];
    char dev_mpeg2[64];
    char dev_jpeg_enc[64];
    char dev_vpp[64];
};

/* Bounds-checked surface lookup. Every surfaces[id] access must go through
 * this — the table is fixed-size and IDs come from the client. */
static inline struct v4l2sl_surface *
v4l2sl_surface_by_id(struct v4l2sl_driver_data *dd, VASurfaceID id)
{
    if (!dd || id == VA_INVALID_ID || (unsigned)id >= V4L2SL_MAX_SURFACES)
        return NULL;
    return dd->surfaces[id];
}

static inline uint64_t v4l2sl_surface_ts(struct v4l2sl_driver_data *dd, VASurfaceID id)
{
    struct v4l2sl_surface *s = v4l2sl_surface_by_id(dd, id);

    return s ? s->timestamp : 0;
}

/* Bounded, duplicate-free pool pushes. A leaked index is better than a
 * write past the end of free_*_bufs[], which used to corrupt the context. */
static inline void v4l2sl_out_pool_push(struct v4l2sl_context *ctx, int idx)
{
    int i;

    if (!ctx || idx < 0 || idx >= V4L2SL_NUM_OUTPUT_BUFS)
        return;
    if (ctx->n_free_out >= V4L2SL_NUM_OUTPUT_BUFS)
        return;
    for (i = 0; i < ctx->n_free_out; i++)
        if (ctx->free_out_bufs[i] == idx)
            return;
    ctx->free_out_bufs[ctx->n_free_out++] = idx;
}

static inline void v4l2sl_cap_pool_push(struct v4l2sl_context *ctx, int idx)
{
    int i;

    if (!ctx || idx < 0 || idx >= V4L2SL_NUM_CAPTURE_BUFS)
        return;
    if (ctx->n_free_cap >= V4L2SL_NUM_CAPTURE_BUFS)
        return;
    for (i = 0; i < ctx->n_free_cap; i++)
        if (ctx->free_cap_bufs[i] == idx)
            return;
    ctx->free_cap_bufs[ctx->n_free_cap++] = idx;
}

/* Device helpers (v4l2stateless_device.c) */
int v4l2sl_open_device(const char *path);
int v4l2sl_open_media_for_device(const char *video_path);
int v4l2sl_request_alloc(int media_fd);
int v4l2sl_setup_output_queue(int fd, uint32_t codec_format, int width, int height);
int v4l2sl_setup_capture_queue(int fd, int width, int height, uint32_t pixelformat);
int v4l2sl_setup_capture_queue_count(int fd, int width, int height,
                                     uint32_t pixelformat, int count);
int v4l2sl_streamon(int fd, enum v4l2_buf_type type);
int v4l2sl_streamoff(int fd, enum v4l2_buf_type type);
void v4l2sl_release_context_device(struct v4l2sl_context *ctx);
int v4l2sl_get_capture_geometry(int fd, uint32_t *w, uint32_t *h,
                                uint32_t *stride, uint32_t *sizeimage);
int v4l2sl_ensure_capture(struct v4l2sl_context *ctx, int width, int height,
                          uint32_t pixelformat);
int v4l2sl_queue_output(int fd, int buf_index, uint32_t size,
                        int request_fd, uint64_t timestamp);
int v4l2sl_queue_capture(int fd, int buf_index, int request_fd);
/*
 * Submit one synchronous decode: pops a capture buffer, QBUFs the given
 * OUTPUT buffer (caller already memcpy'd the bitstream into it), queues the
 * request, waits for the decoded frame and recycles the bitstream buffer.
 * Returns the decoded capture buffer index, or -1 with both pools fully
 * restored (STREAMOFF reset) — the caller must NOT push back out_buf_idx
 * when this fails.
 */
int v4l2sl_decode_submit(struct v4l2sl_context *ctx, int out_buf_idx,
                         uint32_t bytesused, uint64_t timestamp);
/* STREAMOFF both queues and rebuild the free pools from scratch. Used to
 * recover from decode timeouts so a wedged job is never left in the kernel
 * (rkvdec returns every queued buffer in ERROR state on STREAMOFF). */
void v4l2sl_decode_reset(struct v4l2sl_context *ctx);
int v4l2sl_export_dmabuf(int fd, int buf_index);
int v4l2sl_surface_alloc_export_fd(struct v4l2sl_surface *s);
int v4l2sl_surface_grow_memfd(struct v4l2sl_surface *s, uint32_t size);
int v4l2sl_surface_pull_capture(struct v4l2sl_context *ctx,
                                struct v4l2sl_surface *surf, int buf_index);
int v4l2sl_bind_capture_export(struct v4l2sl_context *ctx);
VAStatus v4l2sl_surface_fill_prime(const struct v4l2sl_surface *surf,
                                   const struct v4l2sl_context *c,
                                   uint32_t flags,
                                   void *descriptor);
typedef int (*v4l2sl_ioctl_fn)(int fd, unsigned long request, void *arg);
void v4l2sl_set_ioctl_hook(v4l2sl_ioctl_fn fn);
int v4l2sl_dequeue_buffer(int fd, enum v4l2_buf_type type);
int v4l2sl_mmap_output_buffers(int fd, int count, void **ptrs, uint32_t *size_out);
int v4l2sl_set_request_controls(int request_fd, int v4l2_fd, struct v4l2_ext_controls *ctrls);
int v4l2sl_set_global_controls(int v4l2_fd, struct v4l2_ext_controls *ctrls);
int v4l2sl_submit_request(int request_fd);

/* H.264 translation (v4l2stateless_h264.c) */
void h264_fill_sps(struct v4l2_ctrl_h264_sps *sps, const VAPictureParameterBufferH264 *pic,
                    VAProfile profile);
void h264_fill_pps(struct v4l2_ctrl_h264_pps *pps, const VAPictureParameterBufferH264 *pic,
                    const VASliceParameterBufferH264 *slice);
void h264_fill_decode_params(struct v4l2_ctrl_h264_decode_params *dec,
                             const VAPictureParameterBufferH264 *pic,
                             struct v4l2sl_driver_data *dd,
                             const VASliceParameterBufferH264 *slice);
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

/* VP8 translation (v4l2stateless_vp8.c) */
void v4l2sl_vp8_fill_frame(struct v4l2_ctrl_vp8_frame *frame,
                           const VAPictureParameterBufferVP8 *pic,
                           const VASliceParameterBufferVP8 *slice,
                           const VAProbabilityDataBufferVP8 *prob,
                           const VAIQMatrixBufferVP8 *iq,
                           struct v4l2sl_driver_data *dd);
VAStatus v4l2sl_vp8_translate(struct v4l2sl_context *ctx,
                              struct v4l2sl_buffer **buffers,
                              int num_buffers);

/* MPEG-2 translation (v4l2stateless_mpeg2.c) */
void v4l2sl_mpeg2_fill_sequence(struct v4l2_ctrl_mpeg2_sequence *seq,
                                const VAPictureParameterBufferMPEG2 *pic,
                                VAProfile profile);
void v4l2sl_mpeg2_fill_picture(struct v4l2_ctrl_mpeg2_picture *vpic,
                               const VAPictureParameterBufferMPEG2 *pic,
                               struct v4l2sl_driver_data *dd);
void v4l2sl_mpeg2_fill_quant(struct v4l2_ctrl_mpeg2_quantisation *q,
                             const VAIQMatrixBufferMPEG2 *iq);
VAStatus v4l2sl_mpeg2_translate(struct v4l2sl_context *ctx,
                                struct v4l2sl_buffer **buffers,
                                int num_buffers);

/* JPEG encode / RGA VPP */
VAStatus v4l2sl_jpeg_encode(struct v4l2sl_context *ctx,
                            struct v4l2sl_buffer **buffers, int num_buffers);
VAStatus v4l2sl_vpp_run(struct v4l2sl_context *ctx,
                        struct v4l2sl_buffer **buffers, int num_buffers);
VAStatus v4l2sl_vpp_query_filters(VAProcFilterType *filters, unsigned int *n);
VAStatus v4l2sl_vpp_query_filter_caps(VAProcFilterType type, void *caps,
                                      unsigned int *n);
VAStatus v4l2sl_vpp_query_pipeline_caps(VAProcPipelineCaps *caps);

/* Pixel format helpers (v4l2stateless_format.c) */
uint32_t v4l2sl_capture_fourcc_from_rt(unsigned int rt_format);
uint32_t v4l2sl_capture_fourcc_from_sps(int bit_depth_minus8, int chroma_format_idc);
int v4l2sl_capture_is_10bit(uint32_t fourcc);
int v4l2sl_capture_is_422(uint32_t fourcc);
uint32_t v4l2sl_va_fourcc_for_capture(uint32_t v4l2_fourcc);
uint32_t v4l2sl_drm_fourcc_for_capture(uint32_t v4l2_fourcc);
uint32_t v4l2sl_capture_plane_size(uint32_t fourcc, uint32_t stride, uint32_t aligned_h);
uint32_t v4l2sl_va_image_size(uint32_t va_fourcc, uint32_t stride, uint32_t height);
uint32_t v4l2sl_default_image_stride(uint32_t va_fourcc, int width);
void v4l2sl_nv15_to_p010(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h, int width, int height);
void v4l2sl_copy_nv12(uint8_t *dst, uint32_t dst_stride,
                      const uint8_t *src, uint32_t src_stride,
                      uint32_t src_aligned_h, int width, int height);
void v4l2sl_nv16_to_yuy2(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h, int width, int height);
void v4l2sl_nv20_to_yuy2(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h, int width, int height);
void v4l2sl_nv20_to_y210(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h, int width, int height);

#endif /* V4L2STATELESS_H */
