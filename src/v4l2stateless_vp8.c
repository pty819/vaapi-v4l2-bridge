/*
 * v4l2stateless — VP8 parameter translation
 *
 * Maps VA-API VP8 buffers to V4L2_CID_STATELESS_VP8_FRAME and submits
 * a decode request on hantro (VP8F).
 *
 * ffmpeg's vaapi_vp8 hwaccel strips the 3/10-byte uncompressed chunk
 * before handing us slice data. The G1 kernel's cfg_parts() always
 * skips those bytes (10 on KEY, 3 otherwise) without parsing them, so
 * we prepend zeros of that length. first_part_size / dct_part_sizes
 * stay relative to the compressed payload, matching the frame tag.
 *
 * VA pic_fields.key_frame is inverted vs the bitstream: 0 = key frame.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <poll.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_dec_vp8.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

#include "v4l2stateless.h"

void v4l2sl_vp8_fill_frame(struct v4l2_ctrl_vp8_frame *frame,
                           const VAPictureParameterBufferVP8 *pic,
                           const VASliceParameterBufferVP8 *slice,
                           const VAProbabilityDataBufferVP8 *prob,
                           const VAIQMatrixBufferVP8 *iq,
                           struct v4l2sl_driver_data *dd)
{
    int is_key;
    unsigned i;

    memset(frame, 0, sizeof(*frame));

    is_key = !pic->pic_fields.bits.key_frame;

    frame->width = (uint16_t)pic->frame_width;
    frame->height = (uint16_t)pic->frame_height;
    frame->version = (uint8_t)pic->pic_fields.bits.version;
    frame->prob_skip_false = pic->prob_skip_false;
    frame->prob_intra = pic->prob_intra;
    frame->prob_last = pic->prob_last;
    frame->prob_gf = pic->prob_gf;

    frame->coder_state.range = pic->bool_coder_ctx.range;
    frame->coder_state.value = pic->bool_coder_ctx.value;
    frame->coder_state.bit_count = pic->bool_coder_ctx.count;

    memcpy(frame->entropy.y_mode_probs, pic->y_mode_probs, 4);
    memcpy(frame->entropy.uv_mode_probs, pic->uv_mode_probs, 3);
    memcpy(frame->entropy.mv_probs, pic->mv_probs, sizeof(pic->mv_probs));
    if (prob)
        memcpy(frame->entropy.coeff_probs, prob->dct_coeff_probs,
               sizeof(prob->dct_coeff_probs));

    frame->lf.sharpness_level = (uint8_t)pic->pic_fields.bits.sharpness_level;
    frame->lf.level = pic->pic_fields.bits.loop_filter_disable
                          ? 0 : pic->loop_filter_level[0];
    memcpy(frame->lf.ref_frm_delta, pic->loop_filter_deltas_ref_frame, 4);
    memcpy(frame->lf.mb_mode_delta, pic->loop_filter_deltas_mode, 4);
    if (pic->pic_fields.bits.filter_type)
        frame->lf.flags |= V4L2_VP8_LF_FILTER_TYPE_SIMPLE;
    if (pic->pic_fields.bits.loop_filter_adj_enable)
        frame->lf.flags |= V4L2_VP8_LF_ADJ_ENABLE;
    if (pic->pic_fields.bits.mode_ref_lf_delta_update)
        frame->lf.flags |= V4L2_VP8_LF_DELTA_UPDATE;

    if (iq) {
        int yac = iq->quantization_index[0][0];

        frame->quant.y_ac_qi = (uint8_t)yac;
        frame->quant.y_dc_delta = (int8_t)((int)iq->quantization_index[0][1] - yac);
        frame->quant.y2_dc_delta = (int8_t)((int)iq->quantization_index[0][2] - yac);
        frame->quant.y2_ac_delta = (int8_t)((int)iq->quantization_index[0][3] - yac);
        frame->quant.uv_dc_delta = (int8_t)((int)iq->quantization_index[0][4] - yac);
        frame->quant.uv_ac_delta = (int8_t)((int)iq->quantization_index[0][5] - yac);
        for (i = 0; i < 4; i++)
            frame->segment.quant_update[i] =
                (int8_t)iq->quantization_index[i][0];
    }

    memcpy(frame->segment.segment_probs, pic->mb_segment_tree_probs, 3);
    for (i = 0; i < 4; i++)
        frame->segment.lf_update[i] = (int8_t)pic->loop_filter_level[i];
    if (pic->pic_fields.bits.segmentation_enabled)
        frame->segment.flags |= V4L2_VP8_SEGMENT_FLAG_ENABLED;
    if (pic->pic_fields.bits.update_mb_segmentation_map)
        frame->segment.flags |= V4L2_VP8_SEGMENT_FLAG_UPDATE_MAP;
    if (pic->pic_fields.bits.update_segment_feature_data)
        frame->segment.flags |= V4L2_VP8_SEGMENT_FLAG_UPDATE_FEATURE_DATA;
    /* VA already applied absolute vs delta into loop_filter_level[] and
     * quantization_index[][]; feed those as absolute segment values. */

    if (slice) {
        unsigned header_bytes = (slice->macroblock_offset + 7) / 8;
        unsigned nparts = slice->num_of_partitions;

        frame->first_part_header_bits = slice->macroblock_offset;
        frame->first_part_size = slice->partition_size[0] + header_bytes;
        if (nparts > 0)
            nparts--;
        if (nparts == 0)
            nparts = 1;
        if (nparts > 8)
            nparts = 8;
        frame->num_dct_parts = (uint8_t)nparts;
        for (i = 0; i < 8; i++)
            frame->dct_part_sizes[i] = slice->partition_size[i + 1];
    } else {
        frame->num_dct_parts = 1;
    }

    frame->last_frame_ts = v4l2sl_surface_ts(dd, pic->last_ref_frame);
    frame->golden_frame_ts = v4l2sl_surface_ts(dd, pic->golden_ref_frame);
    frame->alt_frame_ts = v4l2sl_surface_ts(dd, pic->alt_ref_frame);

    if (is_key)
        frame->flags |= V4L2_VP8_FRAME_FLAG_KEY_FRAME;
    frame->flags |= V4L2_VP8_FRAME_FLAG_SHOW_FRAME;
    if (pic->pic_fields.bits.mb_no_coeff_skip)
        frame->flags |= V4L2_VP8_FRAME_FLAG_MB_NO_SKIP_COEFF;
    if (pic->pic_fields.bits.sign_bias_golden)
        frame->flags |= V4L2_VP8_FRAME_FLAG_SIGN_BIAS_GOLDEN;
    if (pic->pic_fields.bits.sign_bias_alternate)
        frame->flags |= V4L2_VP8_FRAME_FLAG_SIGN_BIAS_ALT;
}

VAStatus v4l2sl_vp8_translate(struct v4l2sl_context *ctx,
                              struct v4l2sl_buffer **buffers,
                              int num_buffers)
{
    VAPictureParameterBufferVP8 *pic_param = NULL;
    VASliceParameterBufferVP8 *slice_param = NULL;
    VAProbabilityDataBufferVP8 *prob = NULL;
    VAIQMatrixBufferVP8 *iq = NULL;
    const uint8_t *slice_data = NULL;
    uint32_t slice_size = 0;
    struct v4l2_ctrl_vp8_frame frame;
    size_t prefix, total;
    int request_fd, v4l2_fd;
    int out_buf_idx, cap_buf_idx;
    uint64_t timestamp;
    uint8_t *dst;

    for (int i = 0; i < num_buffers; i++) {
        struct v4l2sl_buffer *buf = buffers[i];
        if (!buf || !buf->data)
            continue;
        switch (buf->type) {
        case VAPictureParameterBufferType:
            pic_param = buf->data;
            break;
        case VASliceParameterBufferType:
            if (!slice_param)
                slice_param = buf->data;
            break;
        case VASliceDataBufferType:
            if (buf->size > slice_size) {
                slice_data = buf->data;
                slice_size = buf->size;
            }
            break;
        case VAProbabilityBufferType:
            prob = buf->data;
            break;
        case VAIQMatrixBufferType:
            iq = buf->data;
            break;
        default:
            break;
        }
    }

    if (!pic_param) {
        fprintf(stderr, "v4l2stateless: VP8 decode missing picture params\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (!slice_data || slice_size == 0) {
        fprintf(stderr, "v4l2stateless: VP8 decode missing slice data\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    v4l2sl_vp8_fill_frame(&frame, pic_param, slice_param, prob, iq,
                          ctx->driver_data);

    request_fd = ctx->request_fd;
    v4l2_fd = ctx->v4l2_fd;
    if (request_fd < 0) {
        fprintf(stderr, "v4l2stateless: VP8 no request fd available\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (v4l2sl_ensure_capture(ctx, pic_param->frame_width, pic_param->frame_height,
                              V4L2_PIX_FMT_NV12) < 0) {
        fprintf(stderr, "v4l2stateless: VP8 capture reconfig failed\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (!ctx->streamed) {
        if (v4l2sl_streamon(v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0 ||
            v4l2sl_streamon(v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) {
            fprintf(stderr, "v4l2stateless: VP8 STREAMON failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        ctx->streamed = 1;
    }

    {
        struct v4l2_ext_control ctrl = { 0 };
        struct v4l2_ext_controls ctrls = { 0 };

        ctrl.id = V4L2_CID_STATELESS_VP8_FRAME;
        ctrl.ptr = &frame;
        ctrl.size = sizeof(frame);
        ctrls.controls = &ctrl;
        ctrls.count = 1;
        if (v4l2sl_set_request_controls(request_fd, v4l2_fd, &ctrls) < 0) {
            fprintf(stderr, "v4l2stateless: failed to set VP8 frame params\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    if (ctx->n_free_out == 0) {
        fprintf(stderr, "v4l2stateless: VP8 no free output buffer\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    out_buf_idx = ctx->free_out_bufs[--ctx->n_free_out];

    if (ctx->n_free_cap == 0) {
        fprintf(stderr, "v4l2stateless: VP8 no free capture buffer\n");
        ctx->n_free_out++;
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    cap_buf_idx = ctx->free_cap_bufs[--ctx->n_free_cap];

    timestamp = ctx->current_surface ? ctx->current_surface->timestamp : 0;
    prefix = (frame.flags & V4L2_VP8_FRAME_FLAG_KEY_FRAME) ? 10 : 3;
    total = prefix + slice_size;

    dst = ctx->output_buf_ptr[out_buf_idx];
    if (!dst) {
        fprintf(stderr, "v4l2stateless: VP8 output buffer not mapped\n");
        ctx->n_free_out++;
        ctx->n_free_cap++;
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (total > ctx->output_buf_size) {
        fprintf(stderr, "v4l2stateless: VP8 slice data too large (%zu > %u)\n",
                total, ctx->output_buf_size);
        ctx->n_free_out++;
        ctx->n_free_cap++;
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    memset(dst, 0, prefix);
    memcpy(dst + prefix, slice_data, slice_size);

    if (v4l2sl_queue_output(v4l2_fd, out_buf_idx, dst, (uint32_t)total,
                            request_fd, timestamp) < 0) {
        fprintf(stderr, "v4l2stateless: failed to queue VP8 output buffer\n");
        ctx->n_free_out++;
        ctx->n_free_cap++;
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (v4l2sl_queue_capture(v4l2_fd, cap_buf_idx, request_fd) < 0) {
        fprintf(stderr, "v4l2stateless: failed to queue VP8 capture buffer\n");
        ctx->n_free_cap++;
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (v4l2sl_submit_request(request_fd) < 0) {
        fprintf(stderr, "v4l2stateless: failed to submit VP8 request\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    {
        struct pollfd pfd = { .fd = v4l2_fd, .events = POLLIN };
        int done_cap, done_out;
        struct v4l2sl_surface *surf;

        if (poll(&pfd, 1, 3000) <= 0) {
            fprintf(stderr, "v4l2stateless: VP8 decode timed out\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        done_cap = v4l2sl_dequeue_buffer(v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        if (done_cap < 0) {
            fprintf(stderr, "v4l2stateless: VP8 decode failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        done_out = v4l2sl_dequeue_buffer(v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        if (done_out < 0) {
            struct pollfd pout = { .fd = v4l2_fd, .events = POLLOUT };
            if (poll(&pout, 1, 200) > 0)
                done_out = v4l2sl_dequeue_buffer(v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        }
        if (done_out >= 0)
            ctx->free_out_bufs[ctx->n_free_out++] = done_out;

        surf = ctx->current_surface;
        if (surf) {
            if (surf->dma_buf_fd >= 0)
                close(surf->dma_buf_fd);
            surf->buf_index = done_cap;
            surf->dma_buf_fd = v4l2sl_export_dmabuf(v4l2_fd, done_cap);
            if (ctx->cap_stride) {
                surf->stride = ctx->cap_stride;
                surf->aligned_h = ctx->cap_height;
                surf->cap_fourcc = ctx->cap_pixelformat;
            }
        } else {
            ctx->free_cap_bufs[ctx->n_free_cap++] = done_cap;
        }
    }

    close(request_fd);
    if (ctx->request_fd == request_fd)
        ctx->request_fd = -1;

    return VA_STATUS_SUCCESS;
}
