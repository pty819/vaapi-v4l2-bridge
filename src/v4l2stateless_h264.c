/*
 * v4l2stateless — H.264 parameter translation
 *
 * Maps VA-API H.264 buffers to V4L2 stateless controls and submits
 * a decode request via the V4L2 Request API.
 *
 * Flow:
 *   1. Translate VAPictureParameterBufferH264 → SPS + PPS + decode_params
 *   2. Translate VAIQMatrixBufferH264 → scaling_matrix
 *   3. Translate VASliceParameterBufferH264 → slice_params
 *   4. Set all controls via VIDIOC_S_EXT_CTRLS (bound to request)
 *   5. Write slice data into output buffer (pre-mapped mmap)
 *   6. Queue output + capture buffers with request_fd
 *   7. Submit request via MEDIA_REQUEST_IOC_QUEUE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <va/va.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

#include "v4l2stateless.h"

/* Convert VA-API SPS fields to V4L2 SPS flags */
static uint32_t h264_sps_flags(const VAPictureParameterBufferH264 *pic)
{
    uint32_t flags = 0;

    if (pic->seq_fields.bits.frame_mbs_only_flag)
        flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
    if (pic->seq_fields.bits.mb_adaptive_frame_field_flag)
        flags |= V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
    if (pic->seq_fields.bits.direct_8x8_inference_flag)
        flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
    if (pic->seq_fields.bits.delta_pic_order_always_zero_flag)
        flags |= V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO;
    if (pic->seq_fields.bits.residual_colour_transform_flag)
        flags |= V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE;
    if (pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag)
        flags |= V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;

    return flags;
}

/* Convert VA-API PPS fields to V4L2 PPS flags */
static uint32_t h264_pps_flags(const VAPictureParameterBufferH264 *pic)
{
    uint32_t flags = 0;

    if (pic->pic_fields.bits.entropy_coding_mode_flag)
        flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
    if (pic->pic_fields.bits.pic_order_present_flag)
        flags |= V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;
    if (pic->pic_fields.bits.weighted_pred_flag)
        flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;
    if (pic->pic_fields.bits.transform_8x8_mode_flag)
        flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;
    if (pic->pic_fields.bits.constrained_intra_pred_flag)
        flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;
    if (pic->pic_fields.bits.deblocking_filter_control_present_flag)
        flags |= V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
    if (pic->pic_fields.bits.redundant_pic_cnt_present_flag)
        flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;

    return flags;
}

/*
 * Find the V4L2 timestamp for a VA-API reference picture.
 * Walk the driver's surface table looking for a surface whose
 * picture_id matches. Returns the surface's timestamp (which was
 * set during begin_picture when that picture was decoded).
 *
 * If the surface is not found (e.g., unused DPB slot), returns 0.
 */
static uint64_t h264_find_ref_timestamp(struct v4l2sl_driver_data *dd,
                                         VASurfaceID picture_id)
{
    if (picture_id == VA_INVALID_SURFACE || picture_id == 0xFFFFFFFF)
        return 0;

    struct v4l2sl_surface *surf = dd->surfaces[picture_id];
    if (surf)
        return surf->timestamp;

    return 0;
}

/*
 * Translate VAPictureH264 reference frame info to V4L2 DPB entry
 */
static void h264_fill_dpb_entry(struct v4l2_h264_dpb_entry *dpb,
                                const VAPictureH264 *ref,
                                uint64_t ref_ts)
{
    dpb->top_field_order_cnt = ref->TopFieldOrderCnt;
    dpb->bottom_field_order_cnt = ref->BottomFieldOrderCnt;
    dpb->frame_num = ref->frame_idx;

    /* Map flags */
    dpb->flags = 0;
    if (ref->flags & VA_PICTURE_H264_SHORT_TERM_REFERENCE)
        dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
    if (ref->flags & VA_PICTURE_H264_LONG_TERM_REFERENCE)
        dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;
    if (ref->flags & VA_PICTURE_H264_TOP_FIELD)
        dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_FIELD;

    dpb->reference_ts = ref_ts;
    dpb->pic_num = ref->frame_idx;
    dpb->fields = V4L2_H264_FRAME_REF;
}

/*
 * Translate VA-API picture parameters to V4L2 H.264 SPS
 */
void h264_fill_sps(struct v4l2_ctrl_h264_sps *sps,
                   const VAPictureParameterBufferH264 *pic,
                   VAProfile profile)
{
    memset(sps, 0, sizeof(*sps));

    sps->chroma_format_idc = pic->seq_fields.bits.chroma_format_idc;
    sps->bit_depth_luma_minus8 = pic->bit_depth_luma_minus8;
    sps->bit_depth_chroma_minus8 = pic->bit_depth_chroma_minus8;
    sps->log2_max_frame_num_minus4 = pic->seq_fields.bits.log2_max_frame_num_minus4;
    sps->pic_order_cnt_type = pic->seq_fields.bits.pic_order_cnt_type;
    sps->log2_max_pic_order_cnt_lsb_minus4 = pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
    sps->max_num_ref_frames = pic->num_ref_frames;
    sps->pic_width_in_mbs_minus1 = pic->picture_width_in_mbs_minus1;
    sps->pic_height_in_map_units_minus1 = pic->picture_height_in_mbs_minus1;

    /* profile_idc / constraint_set are not in VA picture params; take them
     * from the VA config profile ffmpeg selected for this stream. */
    if (profile == VAProfileH264ConstrainedBaseline) {
        sps->profile_idc = 66;  /* Baseline */
        sps->constraint_set_flags = V4L2_H264_SPS_CONSTRAINT_SET0_FLAG |
                                    V4L2_H264_SPS_CONSTRAINT_SET1_FLAG;
    } else if (profile == VAProfileH264Main) {
        sps->profile_idc = 77;
        sps->constraint_set_flags = 0;
    } else {
        sps->profile_idc = 100; /* High */
        sps->constraint_set_flags = 0;
    }
    sps->level_idc = 40;     /* Level 4.0 (covers 1080p) */

    sps->flags = h264_sps_flags(pic);
}

/*
 * Translate VA-API picture parameters to V4L2 H.264 PPS
 */
void h264_fill_pps(struct v4l2_ctrl_h264_pps *pps,
                   const VAPictureParameterBufferH264 *pic,
                   const VASliceParameterBufferH264 *slice)
{
    memset(pps, 0, sizeof(*pps));

    pps->pic_parameter_set_id = 0;  /* Not in VA-API pic params; default from bitstream */
    pps->seq_parameter_set_id = 0;  /* Not in VA-API pic params; default from bitstream */
    pps->num_slice_groups_minus1 = 0;
    /* The kernel's slice parser falls back to these defaults when the slice
     * does not override them — feed the effective active counts. */
    pps->num_ref_idx_l0_default_active_minus1 =
        slice ? slice->num_ref_idx_l0_active_minus1 : 0;
    pps->num_ref_idx_l1_default_active_minus1 =
        slice ? slice->num_ref_idx_l1_active_minus1 : 0;
    pps->weighted_bipred_idc = pic->pic_fields.bits.weighted_bipred_idc;
    pps->pic_init_qp_minus26 = pic->pic_init_qp_minus26;
    pps->pic_init_qs_minus26 = pic->pic_init_qs_minus26;
    pps->chroma_qp_index_offset = pic->chroma_qp_index_offset;
    pps->second_chroma_qp_index_offset = pic->second_chroma_qp_index_offset;
    pps->flags = h264_pps_flags(pic);
}

/*
 * Translate VA-API picture parameters to V4L2 H.264 decode params
 */
void h264_fill_decode_params(struct v4l2_ctrl_h264_decode_params *dec,
                             const VAPictureParameterBufferH264 *pic,
                             struct v4l2sl_driver_data *dd,
                             const VASliceParameterBufferH264 *slice)
{
    memset(dec, 0, sizeof(*dec));

    /* Fill DPB from reference frames, looking up their V4L2 timestamps */
    for (int i = 0; i < 16; i++) {
        const VAPictureH264 *ref = &pic->ReferenceFrames[i];
        if (ref->flags & VA_PICTURE_H264_INVALID)
            continue;
        uint64_t ref_ts = h264_find_ref_timestamp(dd, ref->picture_id);
        h264_fill_dpb_entry(&dec->dpb[i], ref, ref_ts);
        /* VALID marks the slot as populated — the kernel skips entries
         * without it. ACTIVE marks it usable as a reference. */
        dec->dpb[i].flags |= V4L2_H264_DPB_ENTRY_FLAG_VALID;
    }

    /* Order DPB slots ascending by frame_num, mirroring reference userspace */
    for (int i = 1; i < 16; i++) {
        struct v4l2_h264_dpb_entry key = dec->dpb[i];
        int j = i - 1;
        while (j >= 0 && dec->dpb[j].frame_num > key.frame_num) {
            dec->dpb[j + 1] = dec->dpb[j];
            j--;
        }
        dec->dpb[j + 1] = key;
    }

    /* Current picture info */
    int is_idr = 0;
    unsigned st = slice ? slice->slice_type % 5 : 2;
    if (st == 2 && pic->frame_num == 0)
        is_idr = 1;  /* no idr_pic_flag in this libva; I slice + frame_num 0 */
    dec->nal_ref_idc = is_idr ? 3 : (pic->pic_fields.bits.reference_pic_flag ? 2 : 0);
    dec->frame_num = pic->frame_num;
    dec->top_field_order_cnt = pic->CurrPic.TopFieldOrderCnt;
    dec->bottom_field_order_cnt = pic->CurrPic.BottomFieldOrderCnt;

    /*
     * Bit sizes the kernel needs to locate slice data inside the slice
     * header (rkvdec HW starts parsing at a computed bit offset).
     * dec_ref_pic_marking: IDR carries no_output_of_prior_pics_flag +
     * long_term_reference_flag (2 bits); a referenced non-IDR picture
     * carries adaptive_ref_pic_marking_mode_flag (1 bit, no entries in the
     * common case); a non-reference picture carries none.
     * pic_order: POC type 0 carries pic_order_cnt_lsb (log2_max bits),
     * types 1/2 carry none in the slice header.
     */
    if (is_idr) {
        dec->dec_ref_pic_marking_bit_size = 2;
    } else if (pic->pic_fields.bits.reference_pic_flag) {
        dec->dec_ref_pic_marking_bit_size = 1;
    }
    if (pic->seq_fields.bits.pic_order_cnt_type == 0)
        dec->pic_order_cnt_bit_size =
            pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4;

    /* Flags — the frame-based UAPI needs the picture type (PFRAME/BFRAME
     * configure the decoder mode). Slice type: 0=P, 1=B, 2=I (5-7 same). */
    dec->flags = 0;
    if (pic->pic_fields.bits.field_pic_flag)
        dec->flags |= V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;
    if (st == 0)
        dec->flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
    else if (st == 1)
        dec->flags |= V4L2_H264_DECODE_PARAM_FLAG_BFRAME;
    else if (is_idr)
        dec->flags |= V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
}

/*
 * Translate VA-API IQ matrix to V4L2 H.264 scaling matrix
 */
void h264_fill_scaling_matrix(struct v4l2_ctrl_h264_scaling_matrix *sm,
                              const VAIQMatrixBufferH264 *iq)
{
    memset(sm, 0, sizeof(*sm));

    /* 4x4 scaling lists: 6 lists, 16 elements each */
    memcpy(sm->scaling_list_4x4, iq->ScalingList4x4, sizeof(sm->scaling_list_4x4));
    /* VA-API exposes only two 8x8 lists; kernel has six — copy the two and
     * default the rest flat (they are only consulted when the bitstream
     * enables them). */

    /* 8x8 scaling lists: 6 lists, 64 elements each */
    /* VA-API only has 2 8x8 lists, V4L2 expects 6 */
    memcpy(sm->scaling_list_8x8[0], iq->ScalingList8x8[0], 64);
    memcpy(sm->scaling_list_8x8[1], iq->ScalingList8x8[1], 64);
    /* Remaining 4 are zeroed (flat) */
}

/*
 * Translate VA-API slice parameters to V4L2 H.264 slice params
 */
void h264_fill_slice_params(struct v4l2_ctrl_h264_slice_params *sp,
                            const VASliceParameterBufferH264 *slice,
                            const VAPictureParameterBufferH264 *pic,
                            struct v4l2sl_driver_data *dd)
{
    memset(sp, 0, sizeof(*sp));

    sp->header_bit_size = slice->slice_data_bit_offset;
    sp->first_mb_in_slice = slice->first_mb_in_slice;
    sp->slice_type = slice->slice_type;
    sp->colour_plane_id = 0;
    sp->redundant_pic_cnt = 0;
    sp->cabac_init_idc = slice->cabac_init_idc;
    sp->slice_qp_delta = slice->slice_qp_delta;
    sp->disable_deblocking_filter_idc = slice->disable_deblocking_filter_idc;
    sp->slice_alpha_c0_offset_div2 = slice->slice_alpha_c0_offset_div2;
    sp->slice_beta_offset_div2 = slice->slice_beta_offset_div2;
    sp->num_ref_idx_l0_active_minus1 = slice->num_ref_idx_l0_active_minus1;
    sp->num_ref_idx_l1_active_minus1 = slice->num_ref_idx_l1_active_minus1;

    /* Fill reference list 0 (P/B slice forward references) */
    for (int i = 0; i <= slice->num_ref_idx_l0_active_minus1 && i < V4L2_H264_REF_LIST_LEN; i++) {
        const VAPictureH264 *ref = &slice->RefPicList0[i];
        if (ref->flags & VA_PICTURE_H264_INVALID)
            continue;
        sp->ref_pic_list0[i].fields = V4L2_H264_FRAME_REF;
        /* Find this reference's timestamp in the DPB */
        sp->ref_pic_list0[i].index = i;  /* Simplified index mapping */
    }

    /* Fill reference list 1 (B slice backward references) */
    for (int i = 0; i <= slice->num_ref_idx_l1_active_minus1 && i < V4L2_H264_REF_LIST_LEN; i++) {
        const VAPictureH264 *ref = &slice->RefPicList1[i];
        if (ref->flags & VA_PICTURE_H264_INVALID)
            continue;
        sp->ref_pic_list1[i].fields = V4L2_H264_FRAME_REF;
        sp->ref_pic_list1[i].index = i;
    }

    /* Flags */
    sp->flags = 0;
    if (slice->direct_spatial_mv_pred_flag)
        sp->flags |= V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED;
}

/*
 * Main H.264 decode pipeline
 *
 * Called from v4l2sl_end_picture when all VA-API buffers have been collected.
 * Translates parameters, submits to V4L2, waits for result.
 *
 * This is the critical path: if this function works, H.264 decoding works.
 */
VAStatus v4l2sl_h264_translate(struct v4l2sl_context *ctx,
                               struct v4l2sl_buffer **buffers,
                               int num_buffers)
{
    /* Parse collected buffers */
    VAPictureParameterBufferH264 *pic_param = NULL;
    VAIQMatrixBufferH264 *iq_matrix = NULL;
    VASliceParameterBufferH264 *slice_param = NULL;
    /* A picture may consist of many slices — one VASliceDataBuffer each */
    const uint8_t *slice_datas[32];
    uint32_t slice_sizes[32];
    int n_slice_data = 0;

    for (int i = 0; i < num_buffers; i++) {
        struct v4l2sl_buffer *buf = buffers[i];
        if (!buf || !buf->data)
            continue;

        switch (buf->type) {
        case VAPictureParameterBufferType:
            pic_param = buf->data;
            break;
        case VAIQMatrixBufferType:
            iq_matrix = buf->data;
            break;
        case VASliceParameterBufferType:
            if (!slice_param)
                slice_param = buf->data;  /* first slice defines the picture */
            break;
        case VASliceDataBufferType:
            if (n_slice_data < 32) {
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
        fprintf(stderr, "v4l2stateless: H.264 decode missing picture params\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (n_slice_data == 0) {
        fprintf(stderr, "v4l2stateless: H.264 decode missing slice data\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    /*
     * Step 1: Translate VA-API parameters to V4L2 controls
     */
    struct v4l2_ctrl_h264_sps sps;
    struct v4l2_ctrl_h264_pps pps;
    struct v4l2_ctrl_h264_decode_params dec;
    struct v4l2_ctrl_h264_scaling_matrix sm;
    /*
     * Note: VDPU381 (kernel 7.0+ refactored rkvdec) uses the frame-based
     * UAPI — V4L2_CID_STATELESS_H264_SLICE_PARAMS is not registered and the
     * kernel parses the slice header itself from the Annex B bitstream.
     */

    /* Get the driver_data for DPB timestamp lookup */
    /* We need access to driver_data for timestamp resolution. Walk up from context. */
    /* For now, pass NULL for dd — DPB timestamps will be 0, which means
     * the first few frames (with no references) will work fine.
     * Multi-reference decoding needs the actual timestamps. */

    h264_fill_sps(&sps, pic_param, ctx->profile);
    h264_fill_pps(&pps, pic_param, slice_param);
    h264_fill_decode_params(&dec, pic_param, ctx->driver_data, slice_param);

    if (iq_matrix)
        h264_fill_scaling_matrix(&sm, iq_matrix);
    else
        memset(&sm, 0, sizeof(sm));

    if (0 && slice_param)
        h264_fill_slice_params(NULL, NULL, NULL, NULL);

    /*
     * Step 2: Set V4L2 ext controls bound to the request
     *
     * We set SPS, PPS, decode_params, scaling_matrix, and slice_params
     * as separate VIDIOC_S_EXT_CTRLS calls, each bound to the same request_fd.
     *
     * Note: V4L2 stateless controls are set via ext_controls, not regular
     * v4l2_control. Each control class (H264_SPS, H264_PPS, etc.) is set
     * independently.
     */

    int request_fd = ctx->request_fd;
    int v4l2_fd = ctx->v4l2_fd;

    if (request_fd < 0) {
        fprintf(stderr, "v4l2stateless: H.264 no request fd available\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Set H.264 SPS */
    struct v4l2_ext_control sps_ctrl = { 0 };
    sps_ctrl.id = V4L2_CID_STATELESS_H264_SPS;
    sps_ctrl.p_h264_sps = &sps;
    sps_ctrl.size = sizeof(sps);

    struct v4l2_ext_controls sps_ctrls = { 0 };
    sps_ctrls.controls = &sps_ctrl;
    sps_ctrls.count = 1;

    if (v4l2sl_set_request_controls(request_fd, v4l2_fd, &sps_ctrls) < 0) {
        fprintf(stderr, "v4l2stateless: failed to set H.264 SPS\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Set H.264 PPS */
    struct v4l2_ext_control pps_ctrl = { 0 };
    pps_ctrl.id = V4L2_CID_STATELESS_H264_PPS;
    pps_ctrl.p_h264_pps = &pps;
    pps_ctrl.size = sizeof(pps);

    struct v4l2_ext_controls pps_ctrls = { 0 };
    pps_ctrls.controls = &pps_ctrl;
    pps_ctrls.count = 1;

    if (v4l2sl_set_request_controls(request_fd, v4l2_fd, &pps_ctrls) < 0) {
        fprintf(stderr, "v4l2stateless: failed to set H.264 PPS\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Set H.264 decode params */
    struct v4l2_ext_control dec_ctrl = { 0 };
    dec_ctrl.id = V4L2_CID_STATELESS_H264_DECODE_PARAMS;
    dec_ctrl.p_h264_decode_params = &dec;
    dec_ctrl.size = sizeof(dec);

    struct v4l2_ext_controls dec_ctrls = { 0 };
    dec_ctrls.controls = &dec_ctrl;
    dec_ctrls.count = 1;

    if (v4l2sl_set_request_controls(request_fd, v4l2_fd, &dec_ctrls) < 0) {
        fprintf(stderr, "v4l2stateless: failed to set H.264 decode params\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Set H.264 scaling matrix (if provided) */
    if (iq_matrix) {
        struct v4l2_ext_control sm_ctrl = { 0 };
        sm_ctrl.id = V4L2_CID_STATELESS_H264_SCALING_MATRIX;
        sm_ctrl.p_h264_scaling_matrix = &sm;
        sm_ctrl.size = sizeof(sm);

        struct v4l2_ext_controls sm_ctrls = { 0 };
        sm_ctrls.controls = &sm_ctrl;
        sm_ctrls.count = 1;

        if (v4l2sl_set_request_controls(request_fd, v4l2_fd, &sm_ctrls) < 0) {
            fprintf(stderr, "v4l2stateless: warning: failed to set H.264 scaling matrix\n");
            /* Non-fatal: some streams don't need it */
        }
    }

    /*
     * VDPU381 frame-based UAPI: no slice-params control to set here —
     * the kernel parses the slice header from the bitstream we queue below.
     */

    /*
     * Step 3: write the bitstream, submit the request, and wait for it.
     *
     * VA-API clients call vaSyncSurface after vaEndPicture anyway, so we run
     * the pipeline synchronously with a single request in flight: every
     * buffer we queue is dequeued again before returning. This keeps buffer
     * ownership unambiguous — the free pools in the context never lie.
     */
    if (ctx->n_free_out == 0) {
        fprintf(stderr, "v4l2stateless: no free output buffer\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    int out_buf_idx = ctx->free_out_bufs[--ctx->n_free_out];

    if (ctx->n_free_cap == 0) {
        fprintf(stderr, "v4l2stateless: no free capture buffer\n");
        ctx->n_free_out++;
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    int cap_buf_idx = ctx->free_cap_bufs[--ctx->n_free_cap];

    uint64_t timestamp = ctx->current_surface ? ctx->current_surface->timestamp : 0;

    /*
     * Copy slice data into the pre-mapped output buffer. The VDPU381
     * frame-based UAPI expects Annex B start codes in the bitstream; each
     * slice NAL missing one gets 00 00 00 01 prepended. All slices of the
     * picture are concatenated — frame-based decode consumes the whole
     * frame in one request.
     */
    size_t prefixes[32];
    size_t total = 0;
    for (int i = 0; i < n_slice_data; i++) {
        prefixes[i] = 0;
        if (!(slice_sizes[i] >= 3 && slice_datas[i][0] == 0 && slice_datas[i][1] == 0 &&
              (slice_datas[i][2] == 1 ||
               (slice_sizes[i] >= 4 && slice_datas[i][2] == 0 && slice_datas[i][3] == 1))))
            prefixes[i] = 4;
        total += prefixes[i] + slice_sizes[i];
    }

    if (total > ctx->output_buf_size) {
        fprintf(stderr, "v4l2stateless: slice data too large (%zu > %u)\n",
                total, ctx->output_buf_size);
        ctx->n_free_out++;
        ctx->n_free_cap++;
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (ctx->output_buf_ptr[out_buf_idx]) {
        uint8_t *dst = (uint8_t *)ctx->output_buf_ptr[out_buf_idx];
        size_t off = 0;
        for (int i = 0; i < n_slice_data; i++) {
            if (prefixes[i]) {
                dst[off] = 0; dst[off + 1] = 0; dst[off + 2] = 0; dst[off + 3] = 1;
            }
            memcpy(dst + off + prefixes[i], slice_datas[i], slice_sizes[i]);
            off += prefixes[i] + slice_sizes[i];
        }
    }

    /* Queue output buffer with request_fd */
    if (v4l2sl_queue_output(v4l2_fd, out_buf_idx,
                            ctx->output_buf_ptr[out_buf_idx] ? ctx->output_buf_ptr[out_buf_idx] : slice_datas[0],
                            total, request_fd, timestamp) < 0) {
        fprintf(stderr, "v4l2stateless: failed to queue output buffer\n");
        ctx->n_free_out++;
        ctx->n_free_cap++;
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Queue a capture buffer (bare — capture is not a request object) */
    if (v4l2sl_queue_capture(v4l2_fd, cap_buf_idx, request_fd) < 0) {
        fprintf(stderr, "v4l2stateless: failed to queue capture buffer\n");
        ctx->n_free_cap++;
        /* The output buffer is already queued and will never be consumed by
         * a request — recover it so the pool does not leak. */
        struct pollfd pr = { .fd = v4l2_fd, .events = POLLOUT };
        if (poll(&pr, 1, 200) > 0) {
            int ro = v4l2sl_dequeue_buffer(v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
            if (ro >= 0)
                ctx->free_out_bufs[ctx->n_free_out++] = ro;
        }
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /*
     * Submit the request — this triggers the hardware decode using the
     * controls and bitstream bound to this request.
     */
    if (v4l2sl_submit_request(request_fd) < 0) {
        fprintf(stderr, "v4l2stateless: failed to submit H.264 request\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Wait for the decoded frame to be ready */
    struct pollfd pfd = { .fd = v4l2_fd, .events = POLLIN };
    if (poll(&pfd, 1, 3000) <= 0) {
        fprintf(stderr, "v4l2stateless: decode timed out waiting for capture buffer\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    int done_cap = v4l2sl_dequeue_buffer(v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    if (done_cap < 0) {
        fprintf(stderr, "v4l2stateless: decode failed, no capture buffer completed\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* The bitstream buffer is done once the request consumed it; recycle it. */
    int done_out = v4l2sl_dequeue_buffer(v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    if (done_out < 0) {
        struct pollfd pout = { .fd = v4l2_fd, .events = POLLOUT };
        if (poll(&pout, 1, 200) > 0)
            done_out = v4l2sl_dequeue_buffer(v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    }
    if (done_out >= 0)
        ctx->free_out_bufs[ctx->n_free_out++] = done_out;

    /* Attach the decoded frame to the target surface */
    struct v4l2sl_surface *surf = ctx->current_surface;
    if (surf) {
        if (surf->dma_buf_fd >= 0)
            close(surf->dma_buf_fd);
        surf->buf_index = done_cap;
        surf->dma_buf_fd = v4l2sl_export_dmabuf(v4l2_fd, done_cap);
        if (ctx->cap_stride) {
            surf->stride = ctx->cap_stride;
            surf->aligned_h = ctx->cap_height;
        }
    } else {
        /* No target surface — recycle the capture buffer */
        ctx->free_cap_bufs[ctx->n_free_cap++] = done_cap;
    }

    close(request_fd);
    if (ctx->request_fd == request_fd)
        ctx->request_fd = -1;

    return VA_STATUS_SUCCESS;
}
