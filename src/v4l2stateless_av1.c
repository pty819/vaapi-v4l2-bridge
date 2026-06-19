/*
 * v4l2stateless — AV1 parameter translation
 *
 * Maps VA-API AV1 buffers to V4L2 stateless controls.
 * Note: VA-API uses VADecPictureParameterBufferAV1 (not VAPictureParameterBufferAV1).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <va/va.h>
#include <va/va_dec_av1.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

#include "v4l2stateless.h"

static void av1_fill_sequence_params(struct v4l2_ctrl_av1_sequence *seq,
                                     const VADecPictureParameterBufferAV1 *pic)
{
    memset(seq, 0, sizeof(*seq));

    seq->seq_profile = pic->profile;
    seq->max_frame_width_minus_1 = pic->frame_width_minus1;
    seq->max_frame_height_minus_1 = pic->frame_height_minus1;
    seq->order_hint_bits = pic->order_hint_bits_minus_1 + 1;

    seq->flags = 0;
    if (pic->seq_info_fields.fields.still_picture)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_STILL_PICTURE;
    if (pic->seq_info_fields.fields.use_128x128_superblock)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_USE_128X128_SUPERBLOCK;
    if (pic->seq_info_fields.fields.enable_filter_intra)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_FILTER_INTRA;
    if (pic->seq_info_fields.fields.enable_intra_edge_filter)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTRA_EDGE_FILTER;
    if (pic->seq_info_fields.fields.enable_interintra_compound)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTERINTRA_COMPOUND;
    if (pic->seq_info_fields.fields.enable_masked_compound)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_MASKED_COMPOUND;
    if (pic->seq_info_fields.fields.enable_dual_filter)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_DUAL_FILTER;
    if (pic->seq_info_fields.fields.enable_order_hint)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_ORDER_HINT;
    if (pic->seq_info_fields.fields.enable_jnt_comp)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_JNT_COMP;
    if (pic->seq_info_fields.fields.enable_cdef)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_CDEF;
    if (pic->seq_info_fields.fields.film_grain_params_present)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_FILM_GRAIN_PARAMS_PRESENT;
}

static void av1_fill_frame_params(struct v4l2_ctrl_av1_frame *frame,
                                  const VADecPictureParameterBufferAV1 *pic)
{
    memset(frame, 0, sizeof(*frame));

    frame->frame_type = pic->pic_info_fields.bits.frame_type;
    frame->order_hint = pic->order_hint;
    frame->superres_denom = pic->superres_scale_denominator;
    frame->primary_ref_frame = pic->primary_ref_frame;
    frame->refresh_frame_flags = 0; /* TODO: get from bitstream */

    for (int i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++)
        frame->ref_frame_idx[i] = pic->ref_frame_idx[i];

    frame->flags = 0;
    if (pic->pic_info_fields.bits.show_frame)
        frame->flags |= V4L2_AV1_FRAME_FLAG_SHOW_FRAME;
    if (pic->pic_info_fields.bits.error_resilient_mode)
        frame->flags |= V4L2_AV1_FRAME_FLAG_ERROR_RESILIENT_MODE;
    if (pic->pic_info_fields.bits.disable_cdf_update)
        frame->flags |= V4L2_AV1_FRAME_FLAG_DISABLE_CDF_UPDATE;
    if (pic->pic_info_fields.bits.allow_screen_content_tools)
        frame->flags |= V4L2_AV1_FRAME_FLAG_ALLOW_SCREEN_CONTENT_TOOLS;
    if (pic->pic_info_fields.bits.force_integer_mv)
        frame->flags |= V4L2_AV1_FRAME_FLAG_FORCE_INTEGER_MV;
    if (pic->pic_info_fields.bits.allow_intrabc)
        frame->flags |= V4L2_AV1_FRAME_FLAG_ALLOW_INTRABC;
    if (pic->pic_info_fields.bits.use_ref_frame_mvs)
        frame->flags |= V4L2_AV1_FRAME_FLAG_USE_REF_FRAME_MVS;
    if (pic->pic_info_fields.bits.allow_warped_motion)
        frame->flags |= V4L2_AV1_FRAME_FLAG_ALLOW_WARPED_MOTION;
    if (pic->pic_info_fields.bits.is_motion_mode_switchable)
        frame->flags |= V4L2_AV1_FRAME_FLAG_IS_MOTION_MODE_SWITCHABLE;
    if (pic->pic_info_fields.bits.disable_frame_end_update_cdf)
        frame->flags |= V4L2_AV1_FRAME_FLAG_DISABLE_FRAME_END_UPDATE_CDF;
}

VAStatus v4l2sl_av1_translate(struct v4l2sl_context *ctx,
                              struct v4l2sl_buffer **buffers,
                              int num_buffers)
{
    VADecPictureParameterBufferAV1 *pic_param = NULL;
    uint8_t *tile_data = NULL;
    uint32_t tile_data_size = 0;

    for (int i = 0; i < num_buffers; i++) {
        struct v4l2sl_buffer *buf = buffers[i];
        if (!buf || !buf->data)
            continue;
        switch (buf->type) {
        case VAPictureParameterBufferType: pic_param = buf->data; break;
        case VASliceDataBufferType: tile_data = buf->data; tile_data_size = buf->size; break;
        default: break;
        }
    }

    if (!pic_param) {
        fprintf(stderr, "v4l2stateless: AV1 decode missing picture params\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    struct v4l2_ctrl_av1_sequence seq;
    struct v4l2_ctrl_av1_frame frame;

    av1_fill_sequence_params(&seq, pic_param);
    av1_fill_frame_params(&frame, pic_param);

    /* TODO: VIDIOC_S_EXT_CTRLS with V4L2_CTRL_WHICH_REQUEST_VAL + queue buffers */

    fprintf(stderr, "v4l2stateless: AV1 params translated (pic=%dx%d, type=%d, order=%d)\n",
            pic_param->frame_width_minus1 + 1,
            pic_param->frame_height_minus1 + 1,
            pic_param->pic_info_fields.bits.frame_type,
            pic_param->order_hint);

    return VA_STATUS_SUCCESS;
}
