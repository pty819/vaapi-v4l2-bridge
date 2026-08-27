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
}

int main(void)
{
    test_vp8_key_polarity();
    test_vp8_quant_deltas();
    test_mpeg2_fcode_and_defaults();
    return g_fail ? 1 : 0;
}
