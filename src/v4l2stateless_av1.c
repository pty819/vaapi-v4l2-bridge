/*
 * v4l2stateless — AV1 parameter translation
 *
 * Translates VA-API AV1 buffers to V4L2 stateless controls:
 * - VAPictureParameterBufferAV1 -> V4L2_CID_STATELESS_AV1_SEQUENCE + FRAME
 * - Tile group entry and film grain
 */
#include "v4l2stateless.h"

VAStatus v4l2sl_av1_translate(struct v4l2sl_context *ctx,
                              struct v4l2sl_buffer **buffers,
                              int num_buffers)
{
    /* TODO: Phase 4 */
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
