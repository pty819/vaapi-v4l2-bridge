/*
 * v4l2stateless — HEVC parameter translation
 *
 * Maps VA-API HEVC buffers to V4L2 stateless controls and submits
 * a decode request. Special: RK3588 VDPU381 requires explicit RPS
 * via hevc_ext_sps_lt_rps/hevc_ext_sps_st_rps.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <va/va.h>
#include <va/va_dec_hevc.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

#include "v4l2stateless.h"

static void hevc_fill_sps(struct v4l2_ctrl_hevc_sps *sps,
                          const VAPictureParameterBufferHEVC *pic)
{
    memset(sps, 0, sizeof(*sps));

    sps->pic_width_in_luma_samples = pic->pic_width_in_luma_samples;
    sps->pic_height_in_luma_samples = pic->pic_height_in_luma_samples;
    sps->bit_depth_luma_minus8 = pic->bit_depth_luma_minus8;
    sps->bit_depth_chroma_minus8 = pic->bit_depth_chroma_minus8;
    sps->log2_max_pic_order_cnt_lsb_minus4 = pic->log2_max_pic_order_cnt_lsb_minus4;
    sps->sps_max_dec_pic_buffering_minus1 = pic->sps_max_dec_pic_buffering_minus1;
    sps->log2_min_luma_coding_block_size_minus3 = pic->log2_min_luma_coding_block_size_minus3;
    sps->log2_diff_max_min_luma_coding_block_size = pic->log2_diff_max_min_luma_coding_block_size;
    sps->log2_min_luma_transform_block_size_minus2 = pic->log2_min_transform_block_size_minus2;
    sps->log2_diff_max_min_luma_transform_block_size = pic->log2_diff_max_min_transform_block_size;
    sps->max_transform_hierarchy_depth_inter = pic->max_transform_hierarchy_depth_inter;
    sps->max_transform_hierarchy_depth_intra = pic->max_transform_hierarchy_depth_intra;
    sps->pcm_sample_bit_depth_luma_minus1 = pic->pcm_sample_bit_depth_luma_minus1;
    sps->pcm_sample_bit_depth_chroma_minus1 = pic->pcm_sample_bit_depth_chroma_minus1;
    sps->log2_min_pcm_luma_coding_block_size_minus3 = pic->log2_min_pcm_luma_coding_block_size_minus3;
    sps->log2_diff_max_min_pcm_luma_coding_block_size = pic->log2_diff_max_min_pcm_luma_coding_block_size;
    sps->num_short_term_ref_pic_sets = pic->num_short_term_ref_pic_sets;
    sps->num_long_term_ref_pics_sps = pic->num_long_term_ref_pic_sps;

    sps->flags = 0;
    if (pic->pic_fields.bits.pcm_enabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_PCM_ENABLED;
    if (pic->pic_fields.bits.pcm_loop_filter_disabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED;
    if (pic->pic_fields.bits.scaling_list_enabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED;
    if (pic->pic_fields.bits.amp_enabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_AMP_ENABLED;
    if (pic->pic_fields.bits.strong_intra_smoothing_enabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED;
    if (pic->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED;
    if (pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT;
    if (pic->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET;
}

static void hevc_fill_pps(struct v4l2_ctrl_hevc_pps *pps,
                          const VAPictureParameterBufferHEVC *pic)
{
    memset(pps, 0, sizeof(*pps));

    pps->num_extra_slice_header_bits = pic->num_extra_slice_header_bits;
    pps->num_ref_idx_l0_default_active_minus1 = pic->num_ref_idx_l0_default_active_minus1;
    pps->num_ref_idx_l1_default_active_minus1 = pic->num_ref_idx_l1_default_active_minus1;
    pps->init_qp_minus26 = pic->init_qp_minus26;
    pps->diff_cu_qp_delta_depth = pic->diff_cu_qp_delta_depth;
    pps->pps_cb_qp_offset = pic->pps_cb_qp_offset;
    pps->pps_cr_qp_offset = pic->pps_cr_qp_offset;
    pps->num_tile_columns_minus1 = pic->num_tile_columns_minus1;
    pps->num_tile_rows_minus1 = pic->num_tile_rows_minus1;
    pps->log2_parallel_merge_level_minus2 = pic->log2_parallel_merge_level_minus2;
    pps->pps_beta_offset_div2 = pic->pps_beta_offset_div2;
    pps->pps_tc_offset_div2 = pic->pps_tc_offset_div2;

    for (int i = 0; i < 19; i++)
        pps->column_width_minus1[i] = pic->column_width_minus1[i];
    for (int i = 0; i < 21; i++)
        pps->row_height_minus1[i] = pic->row_height_minus1[i];

    pps->flags = 0;
    if (pic->pic_fields.bits.sign_data_hiding_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED;
    if (pic->pic_fields.bits.constrained_intra_pred_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED;
    if (pic->pic_fields.bits.transform_skip_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED;
    if (pic->pic_fields.bits.cu_qp_delta_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED;
    if (pic->pic_fields.bits.weighted_pred_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED;
    if (pic->pic_fields.bits.weighted_bipred_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED;
    if (pic->pic_fields.bits.transquant_bypass_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED;
    if (pic->pic_fields.bits.tiles_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
    if (pic->pic_fields.bits.entropy_coding_sync_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED;
    if (pic->pic_fields.bits.loop_filter_across_tiles_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED;
    if (pic->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED;
    if (pic->slice_parsing_fields.bits.cabac_init_present_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT;
    if (pic->slice_parsing_fields.bits.output_flag_present_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT;
    if (pic->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED;
    if (pic->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED;
    if (pic->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER;
    if (pic->slice_parsing_fields.bits.lists_modification_present_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT;
    if (pic->slice_parsing_fields.bits.slice_segment_header_extension_present_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT;
}

static void hevc_fill_decode_params(struct v4l2_ctrl_hevc_decode_params *dec,
                                    const VAPictureParameterBufferHEVC *pic)
{
    memset(dec, 0, sizeof(*dec));

    for (int i = 0; i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++) {
        const VAPictureHEVC *ref = &pic->ReferenceFrames[i];
        if (ref->flags & VA_PICTURE_HEVC_INVALID)
            continue;
        dec->dpb[i].pic_order_cnt_val = ref->pic_order_cnt;
        dec->dpb[i].flags = 0;
        if (ref->flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE)
            dec->dpb[i].flags |= V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE;
    }

    dec->pic_order_cnt_val = pic->CurrPic.pic_order_cnt;
    dec->short_term_ref_pic_set_size = pic->st_rps_bits;

    dec->flags = 0;
    if (pic->slice_parsing_fields.bits.IdrPicFlag)
        dec->flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC;
    if (pic->slice_parsing_fields.bits.RapPicFlag)
        dec->flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC;
}

/*
 * HEVC decode pipeline — translate and submit
 */
VAStatus v4l2sl_hevc_translate(struct v4l2sl_context *ctx,
                               struct v4l2sl_buffer **buffers,
                               int num_buffers)
{
    VAPictureParameterBufferHEVC *pic_param = NULL;
    VASliceParameterBufferHEVC *slice_param = NULL;
    uint8_t *slice_data = NULL;
    uint32_t slice_data_size = 0;

    for (int i = 0; i < num_buffers; i++) {
        struct v4l2sl_buffer *buf = buffers[i];
        if (!buf || !buf->data)
            continue;
        switch (buf->type) {
        case VAPictureParameterBufferType: pic_param = buf->data; break;
        case VASliceParameterBufferType: slice_param = buf->data; break;
        case VASliceDataBufferType: slice_data = buf->data; slice_data_size = buf->size; break;
        default: break;
        }
    }

    if (!pic_param) {
        fprintf(stderr, "v4l2stateless: HEVC decode missing picture params\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (!slice_data || slice_data_size == 0) {
        fprintf(stderr, "v4l2stateless: HEVC decode missing slice data\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    struct v4l2_ctrl_hevc_sps sps;
    struct v4l2_ctrl_hevc_pps pps;
    struct v4l2_ctrl_hevc_decode_params dec;

    hevc_fill_sps(&sps, pic_param);
    hevc_fill_pps(&pps, pic_param);
    hevc_fill_decode_params(&dec, pic_param);

    int request_fd = ctx->request_fd;
    int v4l2_fd = ctx->v4l2_fd;

    if (request_fd < 0) {
        fprintf(stderr, "v4l2stateless: HEVC no request fd available\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Set HEVC SPS */
    struct v4l2_ext_control sps_ctrl = { 0 };
    sps_ctrl.id = V4L2_CID_STATELESS_HEVC_SPS;
    sps_ctrl.p_hevc_sps = &sps;
    sps_ctrl.size = sizeof(sps);

    struct v4l2_ext_controls sps_ctrls = { 0 };
    sps_ctrls.controls = &sps_ctrl;
    sps_ctrls.count = 1;

    if (v4l2sl_set_request_controls(request_fd, v4l2_fd, &sps_ctrls) < 0) {
        fprintf(stderr, "v4l2stateless: failed to set HEVC SPS\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Set HEVC PPS */
    struct v4l2_ext_control pps_ctrl = { 0 };
    pps_ctrl.id = V4L2_CID_STATELESS_HEVC_PPS;
    pps_ctrl.p_hevc_pps = &pps;
    pps_ctrl.size = sizeof(pps);

    struct v4l2_ext_controls pps_ctrls = { 0 };
    pps_ctrls.controls = &pps_ctrl;
    pps_ctrls.count = 1;

    if (v4l2sl_set_request_controls(request_fd, v4l2_fd, &pps_ctrls) < 0) {
        fprintf(stderr, "v4l2stateless: failed to set HEVC PPS\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Set HEVC decode params (includes RPS for VDPU381) */
    struct v4l2_ext_control dec_ctrl = { 0 };
    dec_ctrl.id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS;
    dec_ctrl.p_hevc_decode_params = &dec;
    dec_ctrl.size = sizeof(dec);

    struct v4l2_ext_controls dec_ctrls = { 0 };
    dec_ctrls.controls = &dec_ctrl;
    dec_ctrls.count = 1;

    if (v4l2sl_set_request_controls(request_fd, v4l2_fd, &dec_ctrls) < 0) {
        fprintf(stderr, "v4l2stateless: failed to set HEVC decode params\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Queue output (compressed bitstream) */
    uint64_t timestamp = ctx->current_surface ? ctx->current_surface->timestamp : 0;

    if (ctx->output_buf_ptr[0] && slice_data_size <= ctx->output_buf_size) {
        memcpy(ctx->output_buf_ptr[0], slice_data, slice_data_size);
    }

    if (v4l2sl_queue_output(v4l2_fd, 0,
                            ctx->output_buf_ptr[0] ? ctx->output_buf_ptr[0] : slice_data,
                            slice_data_size, request_fd, timestamp) < 0) {
        fprintf(stderr, "v4l2stateless: failed to queue HEVC output buffer\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Queue capture (decoded frame) */
    if (v4l2sl_queue_capture(v4l2_fd, 0, request_fd) < 0) {
        fprintf(stderr, "v4l2stateless: failed to queue HEVC capture buffer\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Submit request */
    if (v4l2sl_submit_request(request_fd) < 0) {
        fprintf(stderr, "v4l2stateless: failed to submit HEVC request\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    fprintf(stderr, "v4l2stateless: HEVC request submitted (pic=%dx%d, POC=%d, slice=%u bytes)\n",
            pic_param->pic_width_in_luma_samples,
            pic_param->pic_height_in_luma_samples,
            pic_param->CurrPic.pic_order_cnt,
            slice_data_size);

    return VA_STATUS_SUCCESS;
}
