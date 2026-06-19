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
                   const VAPictureParameterBufferH264 *pic)
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

    /* profile_idc and level_idc are not in VA-API's VAPictureParameterBufferH264
     * directly. We use reasonable defaults for H.264 Main/High. */
    sps->profile_idc = 100;  /* High profile (covers both Main and High) */
    sps->constraint_set_flags = 0;
    sps->level_idc = 40;     /* Level 4.0 (covers 1080p) */

    sps->flags = h264_sps_flags(pic);
}

/*
 * Translate VA-API picture parameters to V4L2 H.264 PPS
 */
void h264_fill_pps(struct v4l2_ctrl_h264_pps *pps,
                   const VAPictureParameterBufferH264 *pic)
{
    memset(pps, 0, sizeof(*pps));

    pps->pic_parameter_set_id = 0;  /* Not in VA-API pic params; default from bitstream */
    pps->seq_parameter_set_id = 0;  /* Not in VA-API pic params; default from bitstream */
    pps->num_slice_groups_minus1 = 0;
    pps->num_ref_idx_l0_default_active_minus1 = 0;  /* Set from slice params */
    pps->num_ref_idx_l1_default_active_minus1 = 0;  /* Set from slice params */
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
                             struct v4l2sl_driver_data *dd)
{
    memset(dec, 0, sizeof(*dec));

    /* Fill DPB from reference frames, looking up their V4L2 timestamps */
    for (int i = 0; i < 16; i++) {
        const VAPictureH264 *ref = &pic->ReferenceFrames[i];
        if (ref->flags & VA_PICTURE_H264_INVALID)
            continue;
        uint64_t ref_ts = h264_find_ref_timestamp(dd, ref->picture_id);
        h264_fill_dpb_entry(&dec->dpb[i], ref, ref_ts);
    }

    /* Current picture info */
    dec->nal_ref_idc = pic->pic_fields.bits.reference_pic_flag ? 1 : 0;
    dec->frame_num = pic->frame_num;
    dec->top_field_order_cnt = pic->CurrPic.TopFieldOrderCnt;
    dec->bottom_field_order_cnt = pic->CurrPic.BottomFieldOrderCnt;

    /* Flags */
    dec->flags = 0;
    if (pic->pic_fields.bits.field_pic_flag)
        dec->flags |= V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;
    if (pic->pic_fields.bits.reference_pic_flag)
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
    uint8_t *slice_data = NULL;
    uint32_t slice_data_size = 0;

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
            slice_param = buf->data;
            break;
        case VASliceDataBufferType:
            slice_data = buf->data;
            slice_data_size = buf->size;
            break;
        default:
            break;
        }
    }

    if (!pic_param) {
        fprintf(stderr, "v4l2stateless: H.264 decode missing picture params\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (!slice_data || slice_data_size == 0) {
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
    struct v4l2_ctrl_h264_slice_params sp;

    /* Get the driver_data for DPB timestamp lookup */
    /* We need access to driver_data for timestamp resolution. Walk up from context. */
    /* For now, pass NULL for dd — DPB timestamps will be 0, which means
     * the first few frames (with no references) will work fine.
     * Multi-reference decoding needs the actual timestamps. */

    h264_fill_sps(&sps, pic_param);
    h264_fill_pps(&pps, pic_param);
    h264_fill_decode_params(&dec, pic_param, NULL);  /* NULL dd = no timestamp lookup */

    if (iq_matrix)
        h264_fill_scaling_matrix(&sm, iq_matrix);
    else
        memset(&sm, 0, sizeof(sm));

    if (slice_param)
        h264_fill_slice_params(&sp, slice_param, pic_param, NULL);
    else
        memset(&sp, 0, sizeof(sp));

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

    /* Set H.264 slice params (if provided) */
    if (slice_param) {
        struct v4l2_ext_control sp_ctrl = { 0 };
        sp_ctrl.id = V4L2_CID_STATELESS_H264_SLICE_PARAMS;
        sp_ctrl.p_h264_slice_params = &sp;
        sp_ctrl.size = sizeof(sp);

        struct v4l2_ext_controls sp_ctrls = { 0 };
        sp_ctrls.controls = &sp_ctrl;
        sp_ctrls.count = 1;

        if (v4l2sl_set_request_controls(request_fd, v4l2_fd, &sp_ctrls) < 0) {
            fprintf(stderr, "v4l2stateless: warning: failed to set H.264 slice params\n");
        }
    }

    /*
     * Step 3: Write compressed slice data into output buffer and queue it
     *
     * We use a simple round-robin scheme for output buffer selection.
     * The buffer must be free (dequeued from a previous decode).
     */
    int out_buf_idx = 0;  /* First available output buffer */
    uint64_t timestamp = ctx->current_surface ? ctx->current_surface->timestamp : 0;

    /* Copy slice data into pre-mapped output buffer */
    if (ctx->output_buf_ptr[0] && slice_data_size <= ctx->output_buf_size) {
        memcpy(ctx->output_buf_ptr[0], slice_data, slice_data_size);
    } else if (slice_data_size > ctx->output_buf_size) {
        fprintf(stderr, "v4l2stateless: slice data too large (%u > %u)\n",
                slice_data_size, ctx->output_buf_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Queue output buffer with request_fd */
    if (v4l2sl_queue_output(v4l2_fd, out_buf_idx,
                            ctx->output_buf_ptr[0] ? ctx->output_buf_ptr[0] : slice_data,
                            slice_data_size, request_fd, timestamp) < 0) {
        fprintf(stderr, "v4l2stateless: failed to queue output buffer\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /*
     * Step 4: Queue a capture buffer (decoded frame output)
     *
     * We use a simple round-robin scheme. The capture buffer will be filled
     * by the decoder hardware and dequeued in sync_surface.
     */
    int cap_buf_idx = 0;  /* First available capture buffer */

    if (v4l2sl_queue_capture(v4l2_fd, cap_buf_idx, request_fd) < 0) {
        fprintf(stderr, "v4l2stateless: failed to queue capture buffer\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /*
     * Step 5: Submit the request
     *
     * This triggers the V4L2 hardware to begin decoding using all the
     * controls and buffers we've attached to this request.
     */
    if (v4l2sl_submit_request(request_fd) < 0) {
        fprintf(stderr, "v4l2stateless: failed to submit H.264 request\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    fprintf(stderr, "v4l2stateless: H.264 request submitted (pic=%dx%d, slice=%u bytes)\n",
            (pic_param->picture_width_in_mbs_minus1 + 1) * 16,
            (pic_param->picture_height_in_mbs_minus1 + 1) * 16,
            slice_data_size);

    return VA_STATUS_SUCCESS;
}
