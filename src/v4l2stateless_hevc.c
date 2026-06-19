/*
 * v4l2stateless — HEVC parameter translation
 *
 * Translates VA-API HEVC buffers to V4L2 stateless controls:
 * - VAPictureParameterBufferHEVC -> V4L2_CID_STATELESS_HEVC_SPS + PPS
 * - Includes RPS (Reference Picture Set) for VDPU381
 */
#include "v4l2stateless.h"

VAStatus v4l2sl_hevc_translate(struct v4l2sl_context *ctx,
                               struct v4l2sl_buffer **buffers,
                               int num_buffers)
{
    /* TODO: Phase 3 */
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
