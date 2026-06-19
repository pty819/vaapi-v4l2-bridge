/*
 * v4l2stateless — H.264 parameter translation
 *
 * Maps VA-API H.264 buffers to V4L2 stateless controls:
 *
 * VAPictureParameterBufferH264 → v4l2_ctrl_h264_sps + v4l2_ctrl_h264_pps + v4l2_ctrl_h264_decode_params
 * VAIQMatrixBufferH264 → v4l2_ctrl_h264_scaling_matrix
 * VASliceParameterBufferH264 → v4l2_ctrl_h264_slice_params
 * VASliceDataBufferH264 → output buffer (compressed bitstream)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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
    if (pic->pic_fields.bits.reference_pic_flag)
        flags |= V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT; /* only if IQ matrix provided */

    return flags;
}

/*
 * Translate VAPictureH264 reference frame info to V4L2 DPB entry
 */
static void h264_fill_dpb_entry(struct v4l2_h264_dpb_entry *dpb,
                                const VAPictureH264 *ref,
                                int dpb_idx)
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

    /* reference_ts will be set from the surface's V4L2 timestamp */
    dpb->reference_ts = 0; /* TODO: map from picture_id to buffer timestamp */
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

    sps->profile_idc = pic->seq_fields.bits.chroma_format_idc; /* TODO: actual profile_idc */
    sps->constraint_set_flags = 0; /* TODO: derive from profile */
    sps->level_idc = 0; /* TODO: derive from level */
    sps->seq_parameter_set_id = 0;
    sps->chroma_format_idc = pic->seq_fields.bits.chroma_format_idc;
    sps->bit_depth_luma_minus8 = pic->bit_depth_luma_minus8;
    sps->bit_depth_chroma_minus8 = pic->bit_depth_chroma_minus8;
    sps->log2_max_frame_num_minus4 = pic->seq_fields.bits.log2_max_frame_num_minus4;
    sps->pic_order_cnt_type = pic->seq_fields.bits.pic_order_cnt_type;
    sps->log2_max_pic_order_cnt_lsb_minus4 = pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
    sps->max_num_ref_frames = pic->num_ref_frames;
    sps->pic_width_in_mbs_minus1 = pic->picture_width_in_mbs_minus1;
    sps->pic_height_in_map_units_minus1 = pic->picture_height_in_mbs_minus1;
    sps->flags = h264_sps_flags(pic);
}

/*
 * Translate VA-API picture parameters to V4L2 H.264 PPS
 */
void h264_fill_pps(struct v4l2_ctrl_h264_pps *pps,
                   const VAPictureParameterBufferH264 *pic)
{
    memset(pps, 0, sizeof(*pps));

    pps->pic_parameter_set_id = 0;
    pps->seq_parameter_set_id = 0;
    pps->num_slice_groups_minus1 = 0;
    pps->num_ref_idx_l0_default_active_minus1 = 0; /* set from slice params */
    pps->num_ref_idx_l1_default_active_minus1 = 0;
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
                             const VAPictureParameterBufferH264 *pic)
{
    memset(dec, 0, sizeof(*dec));

    /* Fill DPB from reference frames */
    for (int i = 0; i < 16; i++) {
        const VAPictureH264 *ref = &pic->ReferenceFrames[i];
        if (ref->flags & VA_PICTURE_H264_INVALID)
            continue;
        h264_fill_dpb_entry(&dec->dpb[i], ref, i);
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
    /* IDR detection: frame_num == 0 for IDR pictures */
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
                            const VAPictureParameterBufferH264 *pic)
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

    /* Fill reference lists */
    for (int i = 0; i <= slice->num_ref_idx_l0_active_minus1 && i < V4L2_H264_REF_LIST_LEN; i++) {
        const VAPictureH264 *ref = &slice->RefPicList0[i];
        if (ref->flags & VA_PICTURE_H264_INVALID)
            continue;
        sp->ref_pic_list0[i].fields = V4L2_H264_FRAME_REF;
        sp->ref_pic_list0[i].index = i; /* TODO: map to DPB index */
    }

    for (int i = 0; i <= slice->num_ref_idx_l1_active_minus1 && i < V4L2_H264_REF_LIST_LEN; i++) {
        const VAPictureH264 *ref = &slice->RefPicList1[i];
        if (ref->flags & VA_PICTURE_H264_INVALID)
            continue;
        sp->ref_pic_list1[i].fields = V4L2_H264_FRAME_REF;
        sp->ref_pic_list1[i].index = i; /* TODO: map to DPB index */
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

    /* Fill V4L2 controls */
    struct v4l2_ctrl_h264_sps sps;
    struct v4l2_ctrl_h264_pps pps;
    struct v4l2_ctrl_h264_decode_params dec;
    struct v4l2_ctrl_h264_scaling_matrix sm;
    struct v4l2_ctrl_h264_slice_params sp;

    h264_fill_sps(&sps, pic_param);
    h264_fill_pps(&pps, pic_param);
    h264_fill_decode_params(&dec, pic_param);

    if (iq_matrix)
        h264_fill_scaling_matrix(&sm, iq_matrix);
    else
        memset(&sm, 0, sizeof(sm));

    if (slice_param)
        h264_fill_slice_params(&sp, slice_param, pic_param);
    else
        memset(&sp, 0, sizeof(sp));

    /* TODO: Set controls via VIDIOC_S_EXT_CTRLS with V4L2_CTRL_WHICH_REQUEST_VAL */
    /* TODO: Queue output buffer with slice_data */
    /* TODO: Queue capture buffer */
    /* TODO: Submit request (VIDIOC_SUBSCRIBE_EVENT or request fd ioctl) */
    /* TODO: Dequeue output and capture buffers */
    /* TODO: Export DMA-BUF from capture buffer */

    fprintf(stderr, "v4l2stateless: H.264 params translated (pic=%dx%d, %d slices)\n",
            (pic_param->picture_width_in_mbs_minus1 + 1) * 16,
            (pic_param->picture_height_in_mbs_minus1 + 1) * 16,
            slice_param ? 1 : 0);

    return VA_STATUS_SUCCESS;
}
