/*
 * v4l2stateless — MPEG-2 parameter translation
 *
 * Maps VA-API MPEG-2 picture / IQ / slice buffers onto the split
 * V4L2 stateless controls (sequence + picture + quantisation) and
 * submits a decode request on hantro (MG2S).
 *
 * ffmpeg vaapi_mpeg2 concatenates Annex B slices (start code included).
 * Sequence headers are not a VA buffer — reconstruct them from the
 * picture parameter + advertised profile. Sequence is a global control
 * (same pattern as HEVC SPS on this kernel).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <poll.h>
#include <unistd.h>

#include <va/va.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

#include "v4l2stateless.h"

static const uint8_t mpeg2_default_intra[64] = {
    8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};

void v4l2sl_mpeg2_fill_sequence(struct v4l2_ctrl_mpeg2_sequence *seq,
                                const VAPictureParameterBufferMPEG2 *pic,
                                VAProfile profile)
{
    memset(seq, 0, sizeof(*seq));
    seq->horizontal_size = pic->horizontal_size;
    seq->vertical_size = pic->vertical_size;
    /* Kernel uses this as a byte budget; width*height is enough. */
    seq->vbv_buffer_size = (uint32_t)pic->horizontal_size * pic->vertical_size;
    if (seq->vbv_buffer_size < 16u * 1024u)
        seq->vbv_buffer_size = 16u * 1024u;
    /* profile (bits 6:4) + level Main=8 (bits 3:1). Simple=5, Main=4. */
    if (profile == VAProfileMPEG2Simple)
        seq->profile_and_level_indication = (5 << 4) | (8 << 1);
    else
        seq->profile_and_level_indication = (4 << 4) | (8 << 1);
    seq->chroma_format = 1; /* 4:2:0 — capture is NV12 */
    if (pic->picture_coding_extension.bits.progressive_frame)
        seq->flags |= V4L2_MPEG2_SEQ_FLAG_PROGRESSIVE;
}

void v4l2sl_mpeg2_fill_picture(struct v4l2_ctrl_mpeg2_picture *vpic,
                               const VAPictureParameterBufferMPEG2 *pic,
                               struct v4l2sl_driver_data *dd)
{
    uint32_t f = (uint32_t)pic->f_code;
    uint32_t ext = pic->picture_coding_extension.bits.picture_structure;

    memset(vpic, 0, sizeof(*vpic));
    vpic->forward_ref_ts = v4l2sl_surface_ts(dd, pic->forward_reference_picture);
    vpic->backward_ref_ts = v4l2sl_surface_ts(dd, pic->backward_reference_picture);
    vpic->f_code[0][0] = (f >> 12) & 0xf;
    vpic->f_code[0][1] = (f >> 8) & 0xf;
    vpic->f_code[1][0] = (f >> 4) & 0xf;
    vpic->f_code[1][1] = f & 0xf;
    vpic->picture_coding_type = (uint8_t)pic->picture_coding_type;
    if (ext == 1)
        vpic->picture_structure = V4L2_MPEG2_PIC_TOP_FIELD;
    else if (ext == 2)
        vpic->picture_structure = V4L2_MPEG2_PIC_BOTTOM_FIELD;
    else
        vpic->picture_structure = V4L2_MPEG2_PIC_FRAME;
    vpic->intra_dc_precision =
        (uint8_t)pic->picture_coding_extension.bits.intra_dc_precision;

    if (pic->picture_coding_extension.bits.top_field_first)
        vpic->flags |= V4L2_MPEG2_PIC_FLAG_TOP_FIELD_FIRST;
    if (pic->picture_coding_extension.bits.frame_pred_frame_dct)
        vpic->flags |= V4L2_MPEG2_PIC_FLAG_FRAME_PRED_DCT;
    if (pic->picture_coding_extension.bits.concealment_motion_vectors)
        vpic->flags |= V4L2_MPEG2_PIC_FLAG_CONCEALMENT_MV;
    if (pic->picture_coding_extension.bits.q_scale_type)
        vpic->flags |= V4L2_MPEG2_PIC_FLAG_Q_SCALE_TYPE;
    if (pic->picture_coding_extension.bits.intra_vlc_format)
        vpic->flags |= V4L2_MPEG2_PIC_FLAG_INTRA_VLC;
    if (pic->picture_coding_extension.bits.alternate_scan)
        vpic->flags |= V4L2_MPEG2_PIC_FLAG_ALT_SCAN;
    if (pic->picture_coding_extension.bits.repeat_first_field)
        vpic->flags |= V4L2_MPEG2_PIC_FLAG_REPEAT_FIRST;
    if (pic->picture_coding_extension.bits.progressive_frame)
        vpic->flags |= V4L2_MPEG2_PIC_FLAG_PROGRESSIVE;
}

void v4l2sl_mpeg2_fill_quant(struct v4l2_ctrl_mpeg2_quantisation *q,
                             const VAIQMatrixBufferMPEG2 *iq)
{
    memset(q, 0, sizeof(*q));
    if (iq && iq->load_intra_quantiser_matrix)
        memcpy(q->intra_quantiser_matrix, iq->intra_quantiser_matrix, 64);
    else
        memcpy(q->intra_quantiser_matrix, mpeg2_default_intra, 64);

    if (iq && iq->load_non_intra_quantiser_matrix)
        memcpy(q->non_intra_quantiser_matrix, iq->non_intra_quantiser_matrix, 64);
    else
        memset(q->non_intra_quantiser_matrix, 16, 64);

    /* H.262 6.3.7: untransmitted chroma matrices inherit the luma values
     * (or their defaults) — the kernel programs these tables verbatim, so
     * the fallback is ours to apply. */
    if (iq && iq->load_chroma_intra_quantiser_matrix)
        memcpy(q->chroma_intra_quantiser_matrix,
               iq->chroma_intra_quantiser_matrix, 64);
    else
        memcpy(q->chroma_intra_quantiser_matrix, q->intra_quantiser_matrix, 64);
    if (iq && iq->load_chroma_non_intra_quantiser_matrix)
        memcpy(q->chroma_non_intra_quantiser_matrix,
               iq->chroma_non_intra_quantiser_matrix, 64);
    else
        memcpy(q->chroma_non_intra_quantiser_matrix,
               q->non_intra_quantiser_matrix, 64);
}

VAStatus v4l2sl_mpeg2_translate(struct v4l2sl_context *ctx,
                                struct v4l2sl_buffer **buffers,
                                int num_buffers)
{
    VAPictureParameterBufferMPEG2 *pic_param = NULL;
    VAIQMatrixBufferMPEG2 *iq = NULL;
    const uint8_t *slice_datas[256];
    uint32_t slice_sizes[256];
    int n_slice_data = 0;
    struct v4l2_ctrl_mpeg2_sequence seq;
    struct v4l2_ctrl_mpeg2_picture vpic;
    struct v4l2_ctrl_mpeg2_quantisation quant;
    int request_fd, v4l2_fd;
    int out_buf_idx, done_cap;
    uint64_t timestamp;
    size_t prefixes[256];
    size_t total = 0;

    for (int i = 0; i < num_buffers; i++) {
        struct v4l2sl_buffer *buf = buffers[i];
        if (!buf || !buf->data)
            continue;
        switch (buf->type) {
        case VAPictureParameterBufferType:
            pic_param = buf->data;
            break;
        case VAIQMatrixBufferType:
            iq = buf->data;
            break;
        case VASliceDataBufferType:
            if (n_slice_data < 256) {
                slice_datas[n_slice_data] = buf->data;
                slice_sizes[n_slice_data] = buf->size;
                n_slice_data++;
            }
            break;
        default:
            break;
        }
    }

    if (!pic_param) {
        fprintf(stderr, "v4l2stateless: MPEG-2 decode missing picture params\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (n_slice_data == 0) {
        fprintf(stderr, "v4l2stateless: MPEG-2 decode missing slice data\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    v4l2sl_mpeg2_fill_sequence(&seq, pic_param, ctx->profile);
    v4l2sl_mpeg2_fill_picture(&vpic, pic_param, ctx->driver_data);
    v4l2sl_mpeg2_fill_quant(&quant, iq);

    request_fd = ctx->request_fd;
    v4l2_fd = ctx->v4l2_fd;
    if (request_fd < 0) {
        fprintf(stderr, "v4l2stateless: MPEG-2 no request fd available\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Sequence is also needed on the request: hantro_get_ctrl() reads
     * request-bound values. A global-only SEQUENCE leaves flags=0 and the
     * G1 then sets PIC_INTERLACE_E on a progressive clip. */
    {
        struct v4l2_ext_control gctrl = { 0 };
        struct v4l2_ext_controls gctrls = { 0 };

        gctrl.id = V4L2_CID_STATELESS_MPEG2_SEQUENCE;
        gctrl.ptr = &seq;
        gctrl.size = sizeof(seq);
        gctrls.controls = &gctrl;
        gctrls.count = 1;
        if (v4l2sl_set_global_controls(v4l2_fd, &gctrls) < 0)
            fprintf(stderr, "v4l2stateless: warning: MPEG-2 global sequence failed\n");
    }

    if (v4l2sl_ensure_capture(ctx, pic_param->horizontal_size,
                              pic_param->vertical_size, V4L2_PIX_FMT_NV12) < 0) {
        fprintf(stderr, "v4l2stateless: MPEG-2 capture reconfig failed\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (!ctx->streamed) {
        if (v4l2sl_streamon(v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0 ||
            v4l2sl_streamon(v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) {
            fprintf(stderr, "v4l2stateless: MPEG-2 STREAMON failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        ctx->streamed = 1;
    }

    {
        struct v4l2_ext_control ctrls_arr[3];
        struct v4l2_ext_controls ctrls = { 0 };

        memset(ctrls_arr, 0, sizeof(ctrls_arr));
        ctrls_arr[0].id = V4L2_CID_STATELESS_MPEG2_SEQUENCE;
        ctrls_arr[0].ptr = &seq;
        ctrls_arr[0].size = sizeof(seq);
        ctrls_arr[1].id = V4L2_CID_STATELESS_MPEG2_PICTURE;
        ctrls_arr[1].ptr = &vpic;
        ctrls_arr[1].size = sizeof(vpic);
        ctrls_arr[2].id = V4L2_CID_STATELESS_MPEG2_QUANTISATION;
        ctrls_arr[2].ptr = &quant;
        ctrls_arr[2].size = sizeof(quant);
        ctrls.controls = ctrls_arr;
        ctrls.count = 3;
        if (v4l2sl_set_request_controls(request_fd, v4l2_fd, &ctrls) < 0) {
            fprintf(stderr, "v4l2stateless: failed to set MPEG-2 request controls\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    if (ctx->n_free_out == 0) {
        fprintf(stderr, "v4l2stateless: MPEG-2 no free output buffer\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    out_buf_idx = ctx->free_out_bufs[--ctx->n_free_out];

    timestamp = ctx->current_surface ? ctx->current_surface->timestamp : 0;

    for (int i = 0; i < n_slice_data; i++) {
        prefixes[i] = 0;
        if (!(slice_sizes[i] >= 3 && slice_datas[i][0] == 0 &&
              slice_datas[i][1] == 0 &&
              (slice_datas[i][2] == 1 ||
               (slice_sizes[i] >= 4 && slice_datas[i][2] == 0 &&
                slice_datas[i][3] == 1))))
            prefixes[i] = 4;
        total += prefixes[i] + slice_sizes[i];
    }
    if (total > ctx->output_buf_size) {
        fprintf(stderr, "v4l2stateless: MPEG-2 slice data too large\n");
        v4l2sl_out_pool_push(ctx, out_buf_idx);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (ctx->output_buf_ptr[out_buf_idx]) {
        uint8_t *dst = (uint8_t *)ctx->output_buf_ptr[out_buf_idx];
        size_t off = 0;
        for (int i = 0; i < n_slice_data; i++) {
            if (prefixes[i]) {
                dst[off] = 0;
                dst[off + 1] = 0;
                dst[off + 2] = 0;
                dst[off + 3] = 1;
            }
            memcpy(dst + off + prefixes[i], slice_datas[i], slice_sizes[i]);
            off += prefixes[i] + slice_sizes[i];
        }
    }

    /* On failure decode_submit resets both queues — do not push back. */
    done_cap = v4l2sl_decode_submit(ctx, out_buf_idx, (uint32_t)total, timestamp);
    if (done_cap == -2) {
        /* Corrupt frame (V4L2_BUF_FLAG_ERROR): mark and succeed — a failed
         * entrypoint would be cached by Chrome for the whole session. */
        if (ctx->current_surface)
            ctx->current_surface->status = VASurfaceSkipped;
        return VA_STATUS_SUCCESS;
    }
    if (done_cap < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    {
        struct v4l2sl_surface *surf = ctx->current_surface;

        if (surf) {
            if (v4l2sl_surface_pull_capture(ctx, surf, done_cap) < 0) {
                fprintf(stderr, "v4l2stateless: MPEG-2 pull capture failed\n");
                v4l2sl_cap_pool_push(ctx, done_cap);
            }
        } else {
            v4l2sl_cap_pool_push(ctx, done_cap);
        }
    }

    return VA_STATUS_SUCCESS;
}
