/*
 * Unit tests for the shipped VP8 / MPEG-2 VA→V4L2 fill functions.
 * Drives v4l2sl_vp8_fill_frame / v4l2sl_mpeg2_fill_* — not a reimplementation.
 */

#include <stdio.h>
#include <string.h>
#include <va/va.h>
#include <va/va_dec_vp8.h>
#include <linux/v4l2-controls.h>

#include "v4l2stateless.h"

static int g_fail;

static void expect_true(int cond, const char *tag)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", tag);
        g_fail++;
    } else {
        printf("OK %s\n", tag);
    }
}

static void test_vp8_key_polarity(void)
{
    VAPictureParameterBufferVP8 pic;
    VASliceParameterBufferVP8 slice;
    struct v4l2_ctrl_vp8_frame frame;

    memset(&pic, 0, sizeof(pic));
    memset(&slice, 0, sizeof(slice));
    pic.frame_width = 320;
    pic.frame_height = 240;
    pic.last_ref_frame = VA_INVALID_ID;
    pic.golden_ref_frame = VA_INVALID_ID;
    pic.alt_ref_frame = VA_INVALID_ID;
    /* VA: 0 means key frame */
    pic.pic_fields.bits.key_frame = 0;
    slice.num_of_partitions = 2;
    slice.macroblock_offset = 17;
    slice.partition_size[0] = 100;
    slice.partition_size[1] = 200;

    v4l2sl_vp8_fill_frame(&frame, &pic, &slice, NULL, NULL, NULL);
    expect_true(frame.flags & V4L2_VP8_FRAME_FLAG_KEY_FRAME, "vp8-va-key0-is-key");
    expect_true(frame.width == 320 && frame.height == 240, "vp8-size");
    expect_true(frame.first_part_header_bits == 17, "vp8-header-bits");
    expect_true(frame.first_part_size == 100 + (17 + 7) / 8, "vp8-first-part-size");
    expect_true(frame.num_dct_parts == 1, "vp8-num-dct-parts");
    expect_true(frame.dct_part_sizes[0] == 200, "vp8-dct-part0");

    pic.pic_fields.bits.key_frame = 1;
    v4l2sl_vp8_fill_frame(&frame, &pic, &slice, NULL, NULL, NULL);
    expect_true(!(frame.flags & V4L2_VP8_FRAME_FLAG_KEY_FRAME), "vp8-va-key1-is-inter");
}

static void test_vp8_quant_deltas(void)
{
    VAPictureParameterBufferVP8 pic;
    VAIQMatrixBufferVP8 iq;
    struct v4l2_ctrl_vp8_frame frame;

    memset(&pic, 0, sizeof(pic));
    memset(&iq, 0, sizeof(iq));
    pic.frame_width = 16;
    pic.frame_height = 16;
    pic.last_ref_frame = VA_INVALID_ID;
    pic.golden_ref_frame = VA_INVALID_ID;
    pic.alt_ref_frame = VA_INVALID_ID;
    pic.pic_fields.bits.key_frame = 1;
    iq.quantization_index[0][0] = 40;
    iq.quantization_index[0][1] = 36;
    iq.quantization_index[0][2] = 42;
    iq.quantization_index[0][3] = 44;
    iq.quantization_index[0][4] = 38;
    iq.quantization_index[0][5] = 41;

    v4l2sl_vp8_fill_frame(&frame, &pic, NULL, NULL, &iq, NULL);
    expect_true(frame.quant.y_ac_qi == 40, "vp8-yac");
    expect_true(frame.quant.y_dc_delta == (int8_t)(36 - 40), "vp8-ydc-delta");
    expect_true(frame.quant.y2_dc_delta == (int8_t)(42 - 40), "vp8-y2dc-delta");
}

static void test_mpeg2_fcode_and_defaults(void)
{
    VAPictureParameterBufferMPEG2 pic;
    struct v4l2_ctrl_mpeg2_sequence seq;
    struct v4l2_ctrl_mpeg2_picture vpic;
    struct v4l2_ctrl_mpeg2_quantisation q;

    memset(&pic, 0, sizeof(pic));
    pic.horizontal_size = 720;
    pic.vertical_size = 480;
    pic.forward_reference_picture = VA_INVALID_ID;
    pic.backward_reference_picture = VA_INVALID_ID;
    pic.picture_coding_type = 2; /* P */
    pic.f_code = (1 << 12) | (2 << 8) | (3 << 4) | 4;
    pic.picture_coding_extension.bits.picture_structure = 3;
    pic.picture_coding_extension.bits.progressive_frame = 1;
    pic.picture_coding_extension.bits.frame_pred_frame_dct = 1;
    pic.picture_coding_extension.bits.intra_dc_precision = 1;

    v4l2sl_mpeg2_fill_sequence(&seq, &pic, VAProfileMPEG2Main);
    expect_true(seq.horizontal_size == 720 && seq.vertical_size == 480,
                "mpeg2-seq-size");
    expect_true(seq.chroma_format == 1, "mpeg2-chroma-420");
    expect_true(seq.flags & V4L2_MPEG2_SEQ_FLAG_PROGRESSIVE, "mpeg2-seq-progressive");
    expect_true(seq.profile_and_level_indication == ((4 << 4) | (8 << 1)),
                "mpeg2-main-pl");

    v4l2sl_mpeg2_fill_sequence(&seq, &pic, VAProfileMPEG2Simple);
    expect_true(seq.profile_and_level_indication == ((5 << 4) | (8 << 1)),
                "mpeg2-simple-pl");

    v4l2sl_mpeg2_fill_picture(&vpic, &pic, NULL);
    expect_true(vpic.picture_coding_type == V4L2_MPEG2_PIC_CODING_TYPE_P,
                "mpeg2-ptype");
    expect_true(vpic.picture_structure == V4L2_MPEG2_PIC_FRAME, "mpeg2-frame");
    expect_true(vpic.f_code[0][0] == 1 && vpic.f_code[0][1] == 2 &&
                vpic.f_code[1][0] == 3 && vpic.f_code[1][1] == 4,
                "mpeg2-fcode-unpack");
    expect_true(vpic.intra_dc_precision == 1, "mpeg2-dc-prec");
    expect_true(vpic.flags & V4L2_MPEG2_PIC_FLAG_PROGRESSIVE, "mpeg2-pic-progressive");
    expect_true(vpic.flags & V4L2_MPEG2_PIC_FLAG_FRAME_PRED_DCT, "mpeg2-fpfdct");

    v4l2sl_mpeg2_fill_quant(&q, NULL);
    expect_true(q.intra_quantiser_matrix[0] == 8, "mpeg2-default-intra0");
    expect_true(q.non_intra_quantiser_matrix[0] == 16, "mpeg2-default-nonintra");
    /* H.262 6.3.7: untransmitted chroma matrices inherit the luma values. */
    expect_true(q.chroma_intra_quantiser_matrix[0] == 8, "mpeg2-chroma-inherits-intra");
    expect_true(q.chroma_non_intra_quantiser_matrix[0] == 16, "mpeg2-chroma-inherits-nonintra");
}

static void test_h264_sps_pps(void)
{
    VAPictureParameterBufferH264 pic;
    VASliceParameterBufferH264 slice;
    struct v4l2_ctrl_h264_sps sps;
    struct v4l2_ctrl_h264_pps pps;
    struct v4l2_ctrl_h264_decode_params dec;
    VAIQMatrixBufferH264 iq;
    struct v4l2_ctrl_h264_scaling_matrix sm;

    memset(&pic, 0, sizeof(pic));
    memset(&slice, 0, sizeof(slice));
    pic.picture_width_in_mbs_minus1 = 119;
    pic.picture_height_in_mbs_minus1 = 67;
    pic.bit_depth_luma_minus8 = 0;
    pic.bit_depth_chroma_minus8 = 0;
    pic.num_ref_frames = 4;
    pic.seq_fields.bits.chroma_format_idc = 1;
    pic.seq_fields.bits.frame_mbs_only_flag = 1;
    pic.seq_fields.bits.log2_max_frame_num_minus4 = 0;
    pic.pic_fields.bits.entropy_coding_mode_flag = 1;
    pic.pic_fields.bits.transform_8x8_mode_flag = 1;
    pic.pic_init_qp_minus26 = 0;
    slice.slice_type = 2;
    slice.num_ref_idx_l0_active_minus1 = 0;
    slice.num_ref_idx_l1_active_minus1 = 0;

    h264_fill_sps(&sps, &pic, VAProfileH264High);
    expect_true(sps.profile_idc == 100, "h264-high-idc");
    expect_true(sps.pic_width_in_mbs_minus1 == 119, "h264-mb-w");
    expect_true(sps.pic_height_in_map_units_minus1 == 67, "h264-mb-h");
    expect_true(sps.flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY, "h264-frame-mbs");

    h264_fill_sps(&sps, &pic, VAProfileH264ConstrainedBaseline);
    expect_true(sps.profile_idc == 66, "h264-baseline-idc");
    h264_fill_sps(&sps, &pic, VAProfileH264High10);
    expect_true(sps.profile_idc == 110, "h264-high10-idc");
    h264_fill_sps(&sps, &pic, VAProfileH264High422);
    expect_true(sps.profile_idc == 122, "h264-high422-idc");

    h264_fill_pps(&pps, &pic, &slice);
    expect_true(pps.flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE, "h264-cabac");
    expect_true(pps.flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE, "h264-8x8");

    pic.frame_num = 0;
    pic.pic_fields.bits.reference_pic_flag = 1;
    for (int i = 0; i < 16; i++)
        pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    h264_fill_decode_params(&dec, &pic, NULL, &slice);
    expect_true(dec.flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC, "h264-idr");
    expect_true(dec.frame_num == 0, "h264-fn0");

    memset(&iq, 0, sizeof(iq));
    iq.ScalingList4x4[0][0] = 16;
    iq.ScalingList8x8[0][0] = 16;
    h264_fill_scaling_matrix(&sm, &iq);
    expect_true(sm.scaling_list_4x4[0][0] == 16, "h264-scale4");
    expect_true(sm.scaling_list_8x8[0][0] == 16, "h264-scale8");
}

int main(void)
{
    test_vp8_key_polarity();
    test_vp8_quant_deltas();
    test_mpeg2_fcode_and_defaults();
    test_h264_sps_pps();
    return g_fail ? 1 : 0;
}
