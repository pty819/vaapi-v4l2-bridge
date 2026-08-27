/*
 * Pick a V4L2 stateless decoder node by OUTPUT_MPLANE fourcc.
 * Matching is path-agnostic so /dev/videoN can move after reboot.
 */

#ifndef V4L2STATELESS_PROBE_H
#define V4L2STATELESS_PROBE_H

#include <stdint.h>
#include "v4l2stateless.h"

#define V4L2SL_PROBE_MAX_FOURCCS 32
#define V4L2SL_PROBE_MAX_NODES   64

struct v4l2sl_node_fmts {
    const char *path;
    const uint32_t *fourccs;
    unsigned n_fourccs;
};

int v4l2sl_codec_coded_fourcc(enum v4l2sl_codec codec, uint32_t *fourcc_out);
const char *v4l2sl_codec_name(enum v4l2sl_codec codec);
void v4l2sl_fourcc_to_str(uint32_t fourcc, char out[5]);

/* First node whose OUTPUT fourcc list contains the codec's coded format.
 * Returns NULL if none match. Does not open devices. */
const char *v4l2sl_pick_device_for_codec(enum v4l2sl_codec codec,
                                         const struct v4l2sl_node_fmts *nodes,
                                         unsigned n_nodes);

int v4l2sl_enum_output_fourccs(int fd, uint32_t *out, unsigned max);

/* Scan /dev/video0..63. Empty string if that codec is absent.
 * out_len is the size of each destination buffer. */
int v4l2sl_scan_decoder_paths(char *h264_out, char *hevc_out, char *av1_out,
                              unsigned out_len);
int v4l2sl_scan_decoder_paths_ex(char *h264_out, char *hevc_out, char *av1_out,
                                 char *vp8_out, char *mpeg2_out,
                                 unsigned out_len);

#endif
