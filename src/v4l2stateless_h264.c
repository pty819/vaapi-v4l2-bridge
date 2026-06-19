/*
 * v4l2stateless — H.264 parameter translation
 *
 * Translates VA-API H.264 buffers to V4L2 stateless controls:
 * - VAPictureParameterBufferH264 -> V4L2_CID_STATELESS_H264_SPS + PPS
 * - VASliceParameterBufferH264 -> V4L2_CID_STATELESS_H264_SLICE_PARAMS
 */
#include "v4l2stateless.h"

VAStatus v4l2sl_h264_translate(struct v4l2sl_context *ctx,
                               struct v4l2sl_buffer **buffers,
                               int num_buffers)
{
    /* TODO: Phase 2 */
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
