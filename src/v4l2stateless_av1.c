/*
 * v4l2stateless — AV1 parameter translation
 *
 * Maps VA-API AV1 buffers to V4L2 stateless controls and submits
 * a decode request.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <poll.h>
#include <unistd.h>

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

    /* bit_depth_idx: 0/1/2 -> 8/10/12 bits */
    seq->bit_depth = 8 + pic->bit_depth_idx * 2;

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
    if (pic->seq_info_fields.fields.mono_chrome)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_MONO_CHROME;
    if (pic->seq_info_fields.fields.color_range)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_COLOR_RANGE;
    if (pic->seq_info_fields.fields.subsampling_x)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X;
    if (pic->seq_info_fields.fields.subsampling_y)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_Y;
    if (pic->seq_info_fields.fields.film_grain_params_present)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_FILM_GRAIN_PARAMS_PRESENT;
    if (pic->pic_info_fields.bits.use_superres)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_SUPERRES;
    /* Warped motion and loop restoration live in different VA sub-structs */
    if (pic->pic_info_fields.bits.allow_warped_motion)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_WARPED_MOTION;
    if (pic->pic_info_fields.bits.use_ref_frame_mvs)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_REF_FRAME_MVS;
    if (pic->loop_restoration_fields.bits.yframe_restoration_type ||
        pic->loop_restoration_fields.bits.cbframe_restoration_type ||
        pic->loop_restoration_fields.bits.crframe_restoration_type)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_RESTORATION;
    /* VA's seq_info_fields exposes no sequence-level separate_uv_delta_q /
     * warped-motion / ref-frame-mvs bits — map only what VA actually
     * reports (the per-picture bits above). Never force sequence tools on
     * from unrelated signals; the kernel trusts this control verbatim. */
}

static uint32_t av1_surface_order_hint(struct v4l2sl_driver_data *dd,
                                      VASurfaceID sid)
{
    struct v4l2sl_surface *s;

    s = v4l2sl_surface_by_id(dd, sid);
    return s ? s->order_hint : 0;
}

/* Spec 7.8 get_relative_dist. VA stores order_hint_bits_minus_1. */
static int av1_relative_dist(unsigned a, unsigned b, unsigned bits_minus_1)
{
    unsigned diff = a - b;
    unsigned m = 1u << bits_minus_1;

    return (int)((diff & (m - 1)) - (diff & m));
}

/*
 * Spec 5.9.22 skip_mode_params — VA only gives skip_mode_present.
 * skip_mode_frame[] are LAST..ALTREF type ids (1..7), not DPB slots.
 */
static void av1_fill_skip_mode(struct v4l2_ctrl_av1_frame *frame,
                               const VADecPictureParameterBufferAV1 *pic,
                               struct v4l2sl_driver_data *dd)
{
    int forward_idx = -1, backward_idx = -1;
    int forward_hint = 0, backward_hint = 0;
    int second_forward_idx = -1, second_forward_hint = 0;
    unsigned bits_minus_1 = pic->order_hint_bits_minus_1;
    int i;

    frame->skip_mode_frame[0] = 0;
    frame->skip_mode_frame[1] = 0;

    if (pic->pic_info_fields.bits.frame_type == V4L2_AV1_KEY_FRAME ||
        pic->pic_info_fields.bits.frame_type == V4L2_AV1_INTRA_ONLY_FRAME)
        return;
    if (!pic->mode_control_fields.bits.reference_select)
        return;
    if (!pic->seq_info_fields.fields.enable_order_hint)
        return;

    for (i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
        uint8_t slot = pic->ref_frame_idx[i];
        VASurfaceID sid;
        uint32_t ref_hint;
        int dist;

        if (slot >= V4L2_AV1_TOTAL_REFS_PER_FRAME)
            continue;
        sid = pic->ref_frame_map[slot];
        ref_hint = av1_surface_order_hint(dd, sid);
        dist = av1_relative_dist(ref_hint, pic->order_hint, bits_minus_1);
        if (dist < 0) {
            if (forward_idx < 0 ||
                av1_relative_dist(ref_hint, (unsigned)forward_hint, bits_minus_1) > 0) {
                forward_idx = i;
                forward_hint = (int)ref_hint;
            }
        } else if (dist > 0) {
            if (backward_idx < 0 ||
                av1_relative_dist(ref_hint, (unsigned)backward_hint, bits_minus_1) < 0) {
                backward_idx = i;
                backward_hint = (int)ref_hint;
            }
        }
    }

    if (forward_idx < 0)
        return;

    if (backward_idx >= 0) {
        int lo = forward_idx < backward_idx ? forward_idx : backward_idx;
        int hi = forward_idx > backward_idx ? forward_idx : backward_idx;

        frame->skip_mode_frame[0] = (uint8_t)(V4L2_AV1_REF_LAST_FRAME + lo);
        frame->skip_mode_frame[1] = (uint8_t)(V4L2_AV1_REF_LAST_FRAME + hi);
        return;
    }

    for (i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
        uint8_t slot = pic->ref_frame_idx[i];
        VASurfaceID sid;
        uint32_t ref_hint;

        if (slot >= V4L2_AV1_TOTAL_REFS_PER_FRAME)
            continue;
        sid = pic->ref_frame_map[slot];
        ref_hint = av1_surface_order_hint(dd, sid);
        if (av1_relative_dist(ref_hint, (unsigned)forward_hint, bits_minus_1) < 0) {
            if (second_forward_idx < 0 ||
                av1_relative_dist(ref_hint, (unsigned)second_forward_hint,
                                  bits_minus_1) > 0) {
                second_forward_idx = i;
                second_forward_hint = (int)ref_hint;
            }
        }
    }

    if (second_forward_idx < 0)
        return;

    {
        int lo = forward_idx < second_forward_idx ? forward_idx : second_forward_idx;
        int hi = forward_idx > second_forward_idx ? forward_idx : second_forward_idx;

        frame->skip_mode_frame[0] = (uint8_t)(V4L2_AV1_REF_LAST_FRAME + lo);
        frame->skip_mode_frame[1] = (uint8_t)(V4L2_AV1_REF_LAST_FRAME + hi);
    }
}

/*
 * VA-API does not expose refresh_frame_flags. Reconstruct the bitmask
 * from the DPB occupancy ffmpeg *does* give us (ref_frame_map +
 * per-surface order_hint), matching GStreamer/libaom for both
 * last-only and lag-in-frames pyramid GOPs.
 */
static uint8_t av1_surface_level1(struct v4l2sl_driver_data *dd, VASurfaceID sid)
{
    struct v4l2sl_surface *s;

    s = v4l2sl_surface_by_id(dd, sid);
    return s ? s->av1_level1 : 0;
}

#define AV1_STYLE_UNKNOWN    0
#define AV1_STYLE_LIBAOM     1
#define AV1_STYLE_SVT        2
#define AV1_STYLE_LIBAOM_RTC 3

static int av1_slot_is_dup(const VASurfaceID sids[8], int i)
{
    int j;

    if (sids[i] == VA_INVALID_SURFACE)
        return 1;
    for (j = 0; j < i; j++) {
        if (sids[j] == sids[i])
            return 1;
    }
    return 0;
}

static int av1_unique_sid_count(const VASurfaceID sids[8])
{
    int n = 0, i, j;

    for (i = 0; i < 8; i++) {
        if (sids[i] == VA_INVALID_SURFACE)
            continue;
        for (j = 0; j < i; j++) {
            if (sids[j] == sids[i])
                break;
        }
        if (j == i)
            n++;
    }
    return n;
}

/* First slot of the most-duplicated surface (KEY copies after 0xff). */
static uint8_t av1_most_dup_first(const VASurfaceID sids[8],
                                 const uint32_t hints[8])
{
    int i, j, best = -1, best_count = 1, best_oh = 0x7fffffff;

    for (i = 0; i < 8; i++) {
        int count;

        if (sids[i] == VA_INVALID_SURFACE)
            continue;
        for (j = 0; j < i; j++) {
            if (sids[j] == sids[i])
                break;
        }
        if (j != i)
            continue;
        count = 1;
        for (j = i + 1; j < 8; j++) {
            if (sids[j] == sids[i])
                count++;
        }
        if (count > best_count ||
            (count == best_count && count > 1 && (int)hints[i] < best_oh)) {
            best_count = count;
            best_oh = (int)hints[i];
            best = i;
        }
    }
    if (best < 0 || best_count <= 1)
        return 0;
    return (uint8_t)(1u << best);
}

static uint8_t av1_first_dup_from(const VASurfaceID sids[8], int start)
{
    int i, j;

    for (i = start; i < 8; i++) {
        if (sids[i] == VA_INVALID_SURFACE)
            return (uint8_t)(1u << i);
        for (j = 0; j < i; j++) {
            if (sids[j] == sids[i])
                return (uint8_t)(1u << i);
        }
    }
    return 0;
}

/* SVT RA temporal layer from order_hint modulo mini-GOP. */
static int av1_svt_tl(uint32_t oh, uint32_t gop)
{
    unsigned pos, x, v, logg, g;

    if (!gop)
        return 0;
    pos = oh % gop;
    if (pos == 0)
        return 0;
    g = gop;
    logg = 0;
    while (g > 1) {
        g >>= 1;
        logg++;
    }
    x = pos;
    v = 0;
    while ((x & 1) == 0) {
        x >>= 1;
        v++;
    }
    return (int)logg - (int)v;
}

static uint8_t av1_get_refresh_idx(const VASurfaceID sids[8],
                                  const uint32_t hints[8],
                                  const uint8_t level1[8],
                                  uint32_t cur, unsigned bits_minus_1,
                                  int update_arf)
{
    int i;
    int arf_count = 0;
    int oldest_arf_order = 0x7fffffff, oldest_arf_idx = -1;
    int oldest_order = 0x7fffffff, oldest_idx = -1;

    for (i = 0; i < 8; i++) {
        int order, dist;

        if (sids[i] == VA_INVALID_SURFACE)
            continue;
        order = (int)hints[i];
        dist = av1_relative_dist((unsigned)order, cur, bits_minus_1);
        /* Keep future frames and three closest previous. */
        if (dist > -3)
            continue;
        if (level1[i]) {
            if (order < oldest_arf_order) {
                oldest_arf_order = order;
                oldest_arf_idx = i;
            }
            arf_count++;
            continue;
        }
        if (order < oldest_order) {
            oldest_order = order;
            oldest_idx = i;
        }
    }
    if (update_arf && arf_count > 2 && oldest_arf_idx >= 0)
        return (uint8_t)(1u << oldest_arf_idx);
    if (oldest_idx >= 0)
        return (uint8_t)(1u << oldest_idx);
    if (oldest_arf_idx >= 0)
        return (uint8_t)(1u << oldest_arf_idx);
    return 0;
}

static uint8_t av1_infer_refresh_libaom(const VASurfaceID sids[8],
                                       const uint32_t hints[8],
                                       const uint8_t level1[8],
                                       uint32_t cur, unsigned bits_minus_1,
                                       int show, int skip)
{
    uint8_t r;

    r = av1_first_dup_from(sids, 1);
    if (r)
        return r;
    r = av1_get_refresh_idx(sids, hints, level1, cur, bits_minus_1,
                            !show && !skip);
    return r ? r : 0x01;
}

static uint8_t av1_infer_refresh_svt(const VASurfaceID sids[8],
                                    const uint32_t hints[8],
                                    const uint8_t level1[8],
                                    uint32_t cur, unsigned bits_minus_1,
                                    int show, int skip,
                                    struct v4l2sl_context *ctx)
{
    uint8_t r;

    /* SVT RA: shown pictures are non-reference leaves or overlays.
     * Only hidden ARFs write the DPB (GStreamer refresh=0 on every show). */
    if (show)
        return 0;

    if (ctx && !ctx->av1.have_first_arf) {
        ctx->av1.have_first_arf = 1;
        /* Distance from the GOP's KEY — the absolute order_hint only
         * equals it in the first GOP (cur=16, key_oh=0). At later GOPs
         * storing `cur` (48...) broke every layer computation after it. */
        ctx->av1.gop = (uint8_t)(cur > ctx->av1.key_oh ? cur - ctx->av1.key_oh
                                                       : (cur ? cur : 8));
        ctx->av1.l0_oh = cur;
        ctx->av1.prev_l0_oh = ctx->av1.key_oh;
        ctx->av1.l0_toggle = 1;
        ctx->av1.l1_toggle = 0;
        r = av1_most_dup_first(sids, hints);
        return r ? r : 0x01;
    }
    /* EOS / cut-short pyramids: each new max even ARF is L0 (16,24,28,30). */
    if (ctx && cur > ctx->av1.l0_oh) {
        uint8_t slot = ctx->av1.l0_toggle;
        ctx->av1.prev_l0_oh = ctx->av1.l0_oh;
        ctx->av1.l0_oh = cur;
        ctx->av1.l0_toggle = (uint8_t)((slot + 1) % 3);
        return (uint8_t)(1u << slot);
    }
    /* Layer map uses the current mini-GOP length (last L0 minus previous). */
    if (ctx && ctx->av1.gop >= 16) {
        uint32_t g = ctx->av1.gop;
        if (ctx->av1.prev_l0_oh && ctx->av1.l0_oh > ctx->av1.prev_l0_oh)
            g = ctx->av1.l0_oh - ctx->av1.prev_l0_oh;
        else if (cur > ctx->av1.gop)
            g /= 2;
        if (g < 4)
            g = 4;
        {
            int tl = av1_svt_tl(cur, g);
            uint8_t slot;
            switch (tl) {
            case 0:
                slot = ctx->av1.l0_toggle;
                ctx->av1.l0_toggle = (uint8_t)((ctx->av1.l0_toggle + 1) % 3);
                return (uint8_t)(1u << slot);
            case 1:
                slot = (uint8_t)(3 + ctx->av1.l1_toggle);
                ctx->av1.l1_toggle ^= 1;
                return (uint8_t)(1u << slot);
            case 2:
                return 0x20;
            case 3:
                return 0x40;
            default:
                return 0x80;
            }
        }
    }

    /* Cut-short / 8-frame mini-GOP: occupancy. */
    if (av1_unique_sid_count(sids) == 2 && av1_slot_is_dup(sids, 3))
        return 0x08;
    r = av1_most_dup_first(sids, hints);
    if (r)
        return r;
    r = av1_get_refresh_idx(sids, hints, level1, cur, bits_minus_1,
                            !show && !skip);
    return r ? r : 0x01;
}

/*
 * VA-API does not expose refresh_frame_flags. Reconstruct the bitmask
 * from DPB occupancy (ref_frame_map + per-surface order_hint).
 *
 * libaom pyramids: free duplicate slots starting at index 1 (KEY stays
 * in slot 0), then get_refresh_idx.
 * SVT-AV1 RA pyramids: first hidden ARF overwrites slot 0, non-ref
 * shown leaves have refresh=0, second ARF uses GOLDEN (slot 3).
 * Style is latched from the first inter after a KEY.
 */
static uint8_t av1_infer_refresh_flags(const VADecPictureParameterBufferAV1 *pic,
                                       struct v4l2sl_driver_data *dd,
                                       struct v4l2sl_context *ctx)
{
    uint8_t ft = pic->pic_info_fields.bits.frame_type;
    VASurfaceID sids[8];
    uint32_t hints[8];
    uint8_t level1[8];
    unsigned bits_minus_1 = pic->order_hint_bits_minus_1;
    uint32_t cur = pic->order_hint;
    int i;
    int show = pic->pic_info_fields.bits.show_frame;
    int skip = pic->mode_control_fields.bits.skip_mode_present;

    if (ft == V4L2_AV1_KEY_FRAME ||
        ft == V4L2_AV1_SWITCH_FRAME ||
        ft == V4L2_AV1_INTRA_ONLY_FRAME) {
        if (ctx) {
            ctx->av1.style = AV1_STYLE_UNKNOWN;
            ctx->av1.gop = 0;
            ctx->av1.l0_toggle = 0;
            ctx->av1.l1_toggle = 0;
            ctx->av1.have_first_arf = 0;
            ctx->av1.l0_oh = 0;
            ctx->av1.prev_l0_oh = 0;
            ctx->av1.key_oh = cur;
        }
        return 0xff;
    }

    for (i = 0; i < 8; i++) {
        sids[i] = pic->ref_frame_map[i];
        hints[i] = av1_surface_order_hint(dd, sids[i]);
        level1[i] = av1_surface_level1(dd, sids[i]);
    }

    /* Overlay / show-existing of an ARF already sitting in the DPB. */
    for (i = 0; i < 8; i++) {
        if (sids[i] != VA_INVALID_SURFACE && hints[i] == cur)
            return 0;
    }

    if (ctx && ctx->av1.style == AV1_STYLE_UNKNOWN &&
        ft == V4L2_AV1_INTER_FRAME) {
        if (show) {
            /* libaom --usage=realtime / low-delay: first inter is shown.
             * LAST cycles through map slots 0..5 (order_hint % 6); slots
             * 6-7 stay KEY. Pyramid GOPs hide the first ARF instead. */
            ctx->av1.style = AV1_STYLE_LIBAOM_RTC;
        } else if (pic->pic_info_fields.bits.showable_frame &&
                   pic->primary_ref_frame != 7) {
            /* SVT RA: first picture after KEY is a showable hidden ARF
             * that still uses LAST's CDF. */
            ctx->av1.style = AV1_STYLE_SVT;
        } else {
            ctx->av1.style = AV1_STYLE_LIBAOM;
        }
    }

    if (ctx && ctx->av1.style == AV1_STYLE_LIBAOM_RTC)
        return (uint8_t)(1u << (cur % 6));
    if (ctx && ctx->av1.style == AV1_STYLE_SVT)
        return av1_infer_refresh_svt(sids, hints, level1, cur, bits_minus_1,
                                     show, skip, ctx);
    return av1_infer_refresh_libaom(sids, hints, level1, cur, bits_minus_1,
                                    show, skip);
}


/*
 * refresh_frame_flags: VA-API does not carry it and the slot policy is
 * encoder-defined (bilibili's "BILIAV1" assigns slots in an order no
 * order-hint heuristic reproduces). Clients that submit the whole OBU
 * span (Chrome) carry the frame OBU's uncompressed header in the same
 * buffer the tile offsets point into: parse the true 8-bit field there
 * and verify frame_type / show_frame / order_hint / primary_ref_frame
 * against VA's own picture parameters before trusting it. Any mismatch
 * — including raw-tile clients like ffmpeg, whose buffer starts
 * mid-OBU — falls back to the heuristic above.
 */
struct av1_br { const uint8_t *d; size_t n; size_t bit; };

static unsigned av1_br_f(struct av1_br *br, unsigned nb)
{
    unsigned v = 0;
    while (nb--) {
        size_t byte = br->bit >> 3;
        if (byte >= br->n) {
            br->bit = (size_t)-1;
            return 0;
        }
        v = (v << 1) | (unsigned)((br->d[byte] >> (7 - (br->bit & 7))) & 1u);
        br->bit++;
    }
    return v;
}

static int av1_parse_hdr_refresh(const uint8_t *span, size_t span_n,
                                 const VADecPictureParameterBufferAV1 *pic,
                                 uint8_t *refresh_out)
{
    size_t pos = 0;
    unsigned ohb = (unsigned)pic->order_hint_bits_minus_1 + 1;
    int eoh = pic->seq_info_fields.fields.enable_order_hint;
    int cand;

    if (pic->seq_info_fields.fields.still_picture)
        return 0;

    /* Chrome submits a whole run of OBUs as one slice buffer; the current
     * frame's header is often NOT the first frame OBU in it. Walk every
     * frame OBU and keep the one that reproduces VA's picture params.
     * libva does not expose the sequence's screen-content / integer-MV
     * modes, so each header is tried with / without those optional bits. */
    pos = 0;
    while (pos + 1 < span_n) {
        uint8_t h = span[pos];
        unsigned type = (h >> 3) & 0xf;
        int has_size = (h >> 1) & 1;
        size_t sz;

        pos += 1;
        if (h & 0x4)
            pos += 1;
        if (has_size) {
            size_t shift = 0;
            sz = 0;
            while (pos < span_n) {
                uint8_t b = span[pos++];
                sz |= (size_t)(b & 0x7f) << shift;
                shift += 7;
                if (!(b & 0x80))
                    break;
                if (shift > 28)
                    return 0;
            }
        } else {
            sz = span_n - pos;
        }
        if (sz > span_n - pos)
            return 0;
        if (type == 6 || type == 3) {
            for (cand = 0; cand < 4; cand++) {
                int sct_bit = cand & 1;
                struct av1_br br = { span + pos, sz, 0 };
                unsigned sef, ft, show, er = 1, allow_sct = 0, oh = 0, prf, refresh;

                sef = av1_br_f(&br, 1);
                if (br.bit == (size_t)-1 || sef)
                    continue;
                ft = av1_br_f(&br, 2);
                show = av1_br_f(&br, 1);
                if (!show)
                    av1_br_f(&br, 1);
                if (!((ft == 0 && show) || ft == 2))
                    er = av1_br_f(&br, 1);
                av1_br_f(&br, 1);
                if (sct_bit) {
                    allow_sct = av1_br_f(&br, 1);
                    if (allow_sct)
                        av1_br_f(&br, 1);
                }
                if (ft != 2)
                    av1_br_f(&br, 1);
                if (eoh)
                    oh = av1_br_f(&br, ohb);
                prf = (!er && ft == 1) ? av1_br_f(&br, 3) : 7;
                if (ft == 2 || (ft == 0 && show))
                    refresh = 0xff;
                else
                    refresh = av1_br_f(&br, 8);
                if (br.bit == (size_t)-1)
                    continue;
                if (ft != pic->pic_info_fields.bits.frame_type)
                    continue;
                if (show != pic->pic_info_fields.bits.show_frame)
                    continue;
                if (eoh && oh != pic->order_hint)
                    continue;
                if (prf != pic->primary_ref_frame)
                    continue;
                if (sct_bit && allow_sct != !!pic->pic_info_fields.bits.allow_screen_content_tools)
                    continue;
                *refresh_out = (uint8_t)refresh;
                return 1;
            }
        }
        pos += sz;
    }
    return 0;
}


static void av1_fill_frame_params(struct v4l2_ctrl_av1_frame *frame,
                                  const VADecPictureParameterBufferAV1 *pic,
                                  struct v4l2sl_context *ctx,
                                  const uint8_t *span, size_t span_n,
                                  const uint8_t * const *all_spans,
                                  const uint32_t *all_sizes)
{
    struct v4l2sl_driver_data *dd = ctx ? ctx->driver_data : NULL;
    memset(frame, 0, sizeof(*frame));

    /* Super-res: VA gives the DISPLAY size and the denominator; the coded
     * size (what the tile grid and SB layout live on) must be derived.
     * Equal when denominator == 8, which is why non-superres streams
     * never noticed the two fields being filled identically. */
    uint32_t disp_w = (uint32_t)pic->frame_width_minus1 + 1;
    uint32_t disp_h = (uint32_t)pic->frame_height_minus1 + 1;
    uint32_t sr_denom = pic->superres_scale_denominator ?
                        pic->superres_scale_denominator : 8;
    uint32_t coded_w = sr_denom > 8 ?
                       (disp_w * 8 + sr_denom / 2) / sr_denom : disp_w;

    /* Tile info */
    frame->tile_info.flags = 0;
    if (pic->pic_info_fields.bits.uniform_tile_spacing_flag)
        frame->tile_info.flags |= V4L2_AV1_TILE_INFO_FLAG_UNIFORM_TILE_SPACING;
    frame->tile_info.context_update_tile_id = pic->context_update_tile_id;
    frame->tile_info.tile_cols = pic->tile_cols;
    frame->tile_info.tile_rows = pic->tile_rows;
    {
        /* Superblock size is 64 or 128 luma samples; MI units are 4x4, so
         * one SB is 16 or 32 MI. The kernel needs mi_*_starts to locate
         * tiles — VA-API only gives width/height in superblocks. */
        int cols = pic->tile_cols;
        int rows = pic->tile_rows;
        if (cols > 64) cols = 64;
        if (rows > 64) rows = 64;
        uint32_t mi_cols = (sr_denom > 8 ? (coded_w + 3) : (disp_w + 3)) / 4;
        uint32_t mi_rows = (pic->frame_height_minus1 + 4) / 4;
        /* Uniform spacing: split the MI grid evenly. Non-uniform: honour
         * the superblock widths VA already computed. */
        frame->tile_info.mi_col_starts[0] = 0;
        frame->tile_info.mi_row_starts[0] = 0;
        int sb_mi = pic->seq_info_fields.fields.use_128x128_superblock ? 32 : 16;
        if (pic->pic_info_fields.bits.uniform_tile_spacing_flag ||
            (cols <= 1 && rows <= 1)) {
            for (int i = 0; i < cols; i++)
                frame->tile_info.mi_col_starts[i] = mi_cols * i / cols;
            frame->tile_info.mi_col_starts[cols] = mi_cols;
            for (int i = 0; i < rows; i++)
                frame->tile_info.mi_row_starts[i] = mi_rows * i / rows;
            frame->tile_info.mi_row_starts[rows] = mi_rows;
            /* Tile sizes in superblocks are DERIVED for uniform spacing;
             * VA-API clients do not fill width/height_in_sbs_minus_1
             * there (Chrome leaves zeros, ffmpeg happens to compute
             * them). Derive from the grid we just built so the kernel
             * gets a consistent tile config either way. */
            for (int i = 0; i < cols && i < 64; i++)
                frame->tile_info.width_in_sbs_minus_1[i] =
                    (frame->tile_info.mi_col_starts[i + 1] -
                     frame->tile_info.mi_col_starts[i] + sb_mi - 1) / sb_mi - 1;
            for (int i = 0; i < rows && i < 64; i++)
                frame->tile_info.height_in_sbs_minus_1[i] =
                    (frame->tile_info.mi_row_starts[i + 1] -
                     frame->tile_info.mi_row_starts[i] + sb_mi - 1) / sb_mi - 1;
        } else {
            uint32_t col = 0, row = 0;
            for (int i = 0; i < cols; i++) {
                frame->tile_info.width_in_sbs_minus_1[i] = pic->width_in_sbs_minus_1[i];
                col += (uint32_t)(pic->width_in_sbs_minus_1[i] + 1) * sb_mi;
                frame->tile_info.mi_col_starts[i + 1] = col;
            }
            for (int i = 0; i < rows; i++) {
                frame->tile_info.height_in_sbs_minus_1[i] = pic->height_in_sbs_minus_1[i];
                row += (uint32_t)(pic->height_in_sbs_minus_1[i] + 1) * sb_mi;
                frame->tile_info.mi_row_starts[i + 1] = row;
            }
        }
    }

    /* Quantization */
    frame->quantization.flags = 0;
    if (pic->qmatrix_fields.bits.using_qmatrix)
        frame->quantization.flags |= V4L2_AV1_QUANTIZATION_FLAG_USING_QMATRIX;
    if (pic->mode_control_fields.bits.delta_q_present_flag)
        frame->quantization.flags |= V4L2_AV1_QUANTIZATION_FLAG_DELTA_Q_PRESENT;
    if (pic->u_dc_delta_q != pic->v_dc_delta_q ||
        pic->u_ac_delta_q != pic->v_ac_delta_q)
        frame->quantization.flags |= V4L2_AV1_QUANTIZATION_FLAG_DIFF_UV_DELTA;
    frame->quantization.base_q_idx = pic->base_qindex;
    frame->quantization.delta_q_y_dc = pic->y_dc_delta_q;
    frame->quantization.delta_q_u_dc = pic->u_dc_delta_q;
    frame->quantization.delta_q_u_ac = pic->u_ac_delta_q;
    frame->quantization.delta_q_v_dc = pic->v_dc_delta_q;
    frame->quantization.delta_q_v_ac = pic->v_ac_delta_q;
    frame->quantization.qm_y = pic->qmatrix_fields.bits.qm_y;
    frame->quantization.qm_u = pic->qmatrix_fields.bits.qm_u;
    frame->quantization.qm_v = pic->qmatrix_fields.bits.qm_v;
    frame->quantization.delta_q_res = pic->mode_control_fields.bits.log2_delta_q_res;

    frame->superres_denom = pic->superres_scale_denominator;
    if (frame->superres_denom == 0)
        frame->superres_denom = 8;

    /* Segmentation */
    frame->segmentation.flags = 0;
    if (pic->seg_info.segment_info_fields.bits.enabled)
        frame->segmentation.flags |= V4L2_AV1_SEGMENTATION_FLAG_ENABLED;
    if (pic->seg_info.segment_info_fields.bits.update_map)
        frame->segmentation.flags |= V4L2_AV1_SEGMENTATION_FLAG_UPDATE_MAP;
    if (pic->seg_info.segment_info_fields.bits.temporal_update)
        frame->segmentation.flags |= V4L2_AV1_SEGMENTATION_FLAG_TEMPORAL_UPDATE;
    if (pic->seg_info.segment_info_fields.bits.update_data)
        frame->segmentation.flags |= V4L2_AV1_SEGMENTATION_FLAG_UPDATE_DATA;
    memcpy(frame->segmentation.feature_enabled, pic->seg_info.feature_mask,
           sizeof(frame->segmentation.feature_enabled));
    for (int s = 0; s < 8; s++) {
        for (int f = 0; f < 8 && f < V4L2_AV1_SEG_LVL_MAX; f++)
            frame->segmentation.feature_data[s][f] = pic->seg_info.feature_data[s][f];
        if (pic->seg_info.feature_mask[s])
            frame->segmentation.last_active_seg_id = (uint8_t)s;
    }

    /* Loop filter */
    frame->loop_filter.flags = 0;
    if (pic->loop_filter_info_fields.bits.mode_ref_delta_enabled)
        frame->loop_filter.flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_ENABLED;
    if (pic->loop_filter_info_fields.bits.mode_ref_delta_update)
        frame->loop_filter.flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_UPDATE;
    if (pic->mode_control_fields.bits.delta_lf_present_flag)
        frame->loop_filter.flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_PRESENT;
    if (pic->mode_control_fields.bits.delta_lf_multi)
        frame->loop_filter.flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_MULTI;
    frame->loop_filter.level[0] = pic->filter_level[0];
    frame->loop_filter.level[1] = pic->filter_level[1];
    frame->loop_filter.level[2] = pic->filter_level_u;
    frame->loop_filter.level[3] = pic->filter_level_v;
    frame->loop_filter.sharpness = pic->loop_filter_info_fields.bits.sharpness_level;
    frame->loop_filter.delta_lf_res = pic->mode_control_fields.bits.log2_delta_lf_res;
    memcpy(frame->loop_filter.ref_deltas, pic->ref_deltas, sizeof(frame->loop_filter.ref_deltas));
    memcpy(frame->loop_filter.mode_deltas, pic->mode_deltas, sizeof(frame->loop_filter.mode_deltas));

    /* CDEF */
    frame->cdef.damping_minus_3 = pic->cdef_damping_minus_3;
    frame->cdef.bits = pic->cdef_bits;
    for (int i = 0; i < 8; i++) {
        frame->cdef.y_pri_strength[i] = pic->cdef_y_strengths[i] >> 2;
        frame->cdef.y_sec_strength[i] = pic->cdef_y_strengths[i] & 0x03;
        frame->cdef.uv_pri_strength[i] = pic->cdef_uv_strengths[i] >> 2;
        frame->cdef.uv_sec_strength[i] = pic->cdef_uv_strengths[i] & 0x03;
    }

    av1_fill_skip_mode(frame, pic, dd);
    frame->primary_ref_frame = pic->primary_ref_frame;

    /* Loop restoration */
    frame->loop_restoration.flags = 0;
    frame->loop_restoration.lr_unit_shift = pic->loop_restoration_fields.bits.lr_unit_shift;
    frame->loop_restoration.lr_uv_shift = pic->loop_restoration_fields.bits.lr_uv_shift;
    frame->loop_restoration.frame_restoration_type[0] =
        pic->loop_restoration_fields.bits.yframe_restoration_type;
    frame->loop_restoration.frame_restoration_type[1] =
        pic->loop_restoration_fields.bits.cbframe_restoration_type;
    frame->loop_restoration.frame_restoration_type[2] =
        pic->loop_restoration_fields.bits.crframe_restoration_type;
    if (frame->loop_restoration.frame_restoration_type[0])
        frame->loop_restoration.flags |= V4L2_AV1_LOOP_RESTORATION_FLAG_USES_LR;
    if (frame->loop_restoration.frame_restoration_type[1] ||
        frame->loop_restoration.frame_restoration_type[2])
        frame->loop_restoration.flags |= V4L2_AV1_LOOP_RESTORATION_FLAG_USES_CHROMA_LR;
    if (frame->loop_restoration.flags) {
        uint32_t luma = 256u >> frame->loop_restoration.lr_unit_shift;
        uint32_t chroma = luma >> frame->loop_restoration.lr_uv_shift;
        frame->loop_restoration.loop_restoration_size[0] = luma;
        frame->loop_restoration.loop_restoration_size[1] = chroma;
        frame->loop_restoration.loop_restoration_size[2] = chroma;
    }

    /* Global motion: VA wm[0..6] is LAST..ALTREF. INTRA (slot 0) is identity. */
    for (int i = 0; i < 7; i++) {
        int ref = i + 1;
        uint8_t type = (uint8_t)pic->wm[i].wmtype;
        frame->global_motion.type[ref] = type;
        memcpy(frame->global_motion.params[ref], pic->wm[i].wmmat,
               6 * sizeof(int32_t));
        if (pic->wm[i].invalid)
            frame->global_motion.invalid |= (uint8_t)(1u << ref);
        if (type != 0)
            frame->global_motion.flags[ref] |= V4L2_AV1_GLOBAL_MOTION_FLAG_IS_GLOBAL;
        if (type == 2)
            frame->global_motion.flags[ref] |= V4L2_AV1_GLOBAL_MOTION_FLAG_IS_ROT_ZOOM;
        if (type == 1)
            frame->global_motion.flags[ref] |= V4L2_AV1_GLOBAL_MOTION_FLAG_IS_TRANSLATION;
    }

    /* Frame basics */
    frame->frame_type = pic->pic_info_fields.bits.frame_type;
    frame->interpolation_filter = pic->interp_filter;
    frame->tx_mode = pic->mode_control_fields.bits.tx_mode;
    frame->order_hint = pic->order_hint;
    frame->upscaled_width = disp_w;
    frame->frame_width_minus_1 = coded_w - 1;
    frame->frame_height_minus_1 = disp_h - 1;
    frame->render_width_minus_1 = disp_w - 1;
    frame->render_height_minus_1 = disp_h - 1;

    /* Reference frames: VA surface ids -> our V4L2 timestamps */
    for (int i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++) {
        VASurfaceID sid = pic->ref_frame_map[i];
        uint64_t ts = 0;
        struct v4l2sl_surface *s = v4l2sl_surface_by_id(dd, sid);

        if (s)
            ts = s->timestamp;
        frame->reference_frame_ts[i] = ts;
    }
    for (int i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++)
        frame->ref_frame_idx[i] = pic->ref_frame_idx[i];

    /* Spec OrderHints: index by ref type LAST..ALTREF, not DPB slot.
     * Kernel copies this onto frame_refs[] for temporal-MV overlay. */
    frame->order_hints[V4L2_AV1_REF_INTRA_FRAME] = 0;
    for (int i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
        uint8_t slot = pic->ref_frame_idx[i];
        VASurfaceID sid = VA_INVALID_SURFACE;

        if (slot < V4L2_AV1_TOTAL_REFS_PER_FRAME)
            sid = pic->ref_frame_map[slot];
        frame->order_hints[V4L2_AV1_REF_LAST_FRAME + i] =
            av1_surface_order_hint(dd, sid);
    }

    {
        uint8_t parsed = 0;
        int parsed_ok = (span && av1_parse_hdr_refresh(span, span_n, pic,
                                                       &parsed));
        if (!parsed_ok) {
            /* Chrome may split the submission into several slice buffers;
             * only some carry the OBU framing. Try them all. */
            int si;
            for (si = 0; si < V4L2SL_MAX_SLICE_DATAS && !parsed_ok; si++)
                if (all_spans[si] &&
                    av1_parse_hdr_refresh(all_spans[si], all_sizes[si],
                                          pic, &parsed)) {
                    parsed_ok = 1;
                    break;
                }
        }
        if (parsed_ok) {
            /* OBU truth available: the slot model may own buffer release.
             * The order-hint heuristic is NOT trusted for that — SVT-style
             * pyramids mismatch it, and early recycling then exposes the
             * kernel's equally-wrong slot table as corruption. */
            ctx->av1.model_active = 1;
            frame->refresh_frame_flags = parsed;
        } else
            frame->refresh_frame_flags = av1_infer_refresh_flags(pic, dd, ctx);
    }

    frame->flags = 0;
    if (pic->pic_info_fields.bits.show_frame)
        frame->flags |= V4L2_AV1_FRAME_FLAG_SHOW_FRAME;
    if (pic->pic_info_fields.bits.showable_frame)
        frame->flags |= V4L2_AV1_FRAME_FLAG_SHOWABLE_FRAME;
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
    if (pic->pic_info_fields.bits.use_superres)
        frame->flags |= V4L2_AV1_FRAME_FLAG_USE_SUPERRES;
    if (pic->pic_info_fields.bits.allow_high_precision_mv)
        frame->flags |= V4L2_AV1_FRAME_FLAG_ALLOW_HIGH_PRECISION_MV;
    if (pic->mode_control_fields.bits.reference_select)
        frame->flags |= V4L2_AV1_FRAME_FLAG_REFERENCE_SELECT;
    if (pic->mode_control_fields.bits.reduced_tx_set_used)
        frame->flags |= V4L2_AV1_FRAME_FLAG_REDUCED_TX_SET;
    if (pic->mode_control_fields.bits.skip_mode_present)
        frame->flags |= V4L2_AV1_FRAME_FLAG_SKIP_MODE_PRESENT;
    if (frame->skip_mode_frame[0] > 0)
        frame->flags |= V4L2_AV1_FRAME_FLAG_SKIP_MODE_ALLOWED;

    if (ctx && ctx->current_surface) {
        ctx->current_surface->order_hint = pic->order_hint;
        ctx->current_surface->av1_level1 =
            (pic->pic_info_fields.bits.frame_type == V4L2_AV1_KEY_FRAME) ||
            (!pic->pic_info_fields.bits.show_frame &&
             !pic->mode_control_fields.bits.skip_mode_present);
    }
}

/*
 * AV1 decode pipeline — translate and submit
 */
/* AV1 copy-out release. pull_capture has already snapshotted this frame
 * (GBM bo for display surfaces, memfd otherwise), so a capture buffer's
 * only remaining duty is being a kernel DPB reference. Track the slots
 * with the same refresh_frame_flags we submit to the kernel and hand
 * buffers back as soon as their slot is overwritten; non-reference
 * frames (refresh == 0) return immediately. This is what bounds the
 * pool against zero-copy clients that queue one surface per buffered
 * frame (Chrome WebCodecs/MSE decode-ahead) instead of the buffer
 * lifetime following the surface lifetime. */
void v4l2sl_av1_dpb_model_reset(struct v4l2sl_context *ctx)
{
    int i;

    if (!ctx)
        return;
    ctx->av1.model_active = 0;
    for (i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++)
        ctx->av1.slot_buf[i] = -1;
    for (i = 0; i < V4L2SL_MAX_CAPTURE_BUFS; i++)
        ctx->av1.buf_owner[i] = VA_INVALID_SURFACE;
}

static void av1_release_unrefd(struct v4l2sl_context *ctx,
                               struct v4l2sl_surface *surf,
                               int buf, uint8_t refresh)
{
    int s, t, still;

    if (!ctx || !surf || buf < 0 || buf >= V4L2SL_MAX_CAPTURE_BUFS)
        return;
    ctx->av1.buf_owner[buf] = surf->surface_id;

    if (!refresh) {
        if (v4l2sl_debug)
            fprintf(stderr, "v4l2stateless: AV1 rel buf=%d (nonref)\n", buf);
        surf->buf_index = -1;
        ctx->av1.buf_owner[buf] = VA_INVALID_SURFACE;
        v4l2sl_cap_pool_push(ctx, buf);
        return;
    }
    for (s = 0; s < V4L2_AV1_TOTAL_REFS_PER_FRAME; s++) {
        int old;

        if (!(refresh & (1u << s)))
            continue;
        old = ctx->av1.slot_buf[s];
        ctx->av1.slot_buf[s] = buf;
        if (old < 0 || old == buf)
            continue;
        still = 0;
        for (t = 0; t < V4L2_AV1_TOTAL_REFS_PER_FRAME; t++)
            if (ctx->av1.slot_buf[t] == old)
                still = 1;
        if (still)
            continue;
        {
            VASurfaceID id = ctx->av1.buf_owner[old];
            struct v4l2sl_surface *os = (id != VA_INVALID_SURFACE)
                ? v4l2sl_surface_by_id(ctx->driver_data, id) : NULL;
            if (os && os->buf_index == old)
                os->buf_index = -1;
            if (v4l2sl_debug)
                fprintf(stderr, "v4l2stateless: AV1 rel buf=%d (slot %d -> %d)\n",
                        old, s, buf);
            ctx->av1.buf_owner[old] = VA_INVALID_SURFACE;
            v4l2sl_cap_pool_push(ctx, old);
        }
    }
}

VAStatus v4l2sl_av1_translate(struct v4l2sl_context *ctx,
                              struct v4l2sl_buffer **buffers,
                              int num_buffers)
{
    VADecPictureParameterBufferAV1 *pic_param = NULL;
    VASliceParameterBufferAV1 *tile_params[32];
    int n_tiles = 0;
    uint8_t *tile_data = NULL;
    uint32_t tile_data_size = 0;

    struct v4l2sl_collected cb;
    int i;

    v4l2sl_collect_decode_buffers(buffers, num_buffers, &cb);
    pic_param = cb.pic;
    for (i = 0; i < cb.n_slice_params && i < 32; i++)
        tile_params[i] = cb.slice_params[i];
    n_tiles = cb.n_slice_params > 32 ? 32 : cb.n_slice_params;
    /* Largest slice-data buffer — ffmpeg's AV1 VAAPI hwaccel submits the
     * whole OBU in one buffer. */
    tile_data = (uint8_t *)cb.largest;
    tile_data_size = cb.largest_size;
    if (!pic_param) {
        fprintf(stderr, "v4l2stateless: AV1 decode missing picture params\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (!tile_data || tile_data_size == 0) {
        fprintf(stderr, "v4l2stateless: AV1 decode missing tile data\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    struct v4l2_ctrl_av1_sequence seq;
    struct v4l2_ctrl_av1_frame frame;

    av1_fill_sequence_params(&seq, pic_param);
    av1_fill_frame_params(&frame, pic_param, ctx,
                          (const uint8_t *)cb.largest, cb.largest_size,
                          cb.slice_datas, cb.slice_sizes);

    int request_fd = ctx->request_fd;
    int v4l2_fd = ctx->v4l2_fd;

    if (request_fd < 0) {
        fprintf(stderr, "v4l2stateless: AV1 no request fd available\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Set AV1 sequence params */
    struct v4l2_ext_control seq_ctrl = { 0 };
    seq_ctrl.id = V4L2_CID_STATELESS_AV1_SEQUENCE;
    seq_ctrl.p_av1_sequence = &seq;
    seq_ctrl.size = sizeof(seq);

    struct v4l2_ext_controls seq_ctrls = { 0 };
    seq_ctrls.controls = &seq_ctrl;
    seq_ctrls.count = 1;

    /* Sequence is a GLOBAL control on hantro AV1 — request-scoped
     * submissions succeed the ioctl but leave the device unconfigured,
     * so the subsequent OUTPUT QBUF returns EINVAL. */
    /* Sequence-level control: resubmit only when the payload changed. */
    _Static_assert(sizeof(seq) <= sizeof(ctx->g_ctrl_payload), "grow cache");
    if (!ctx->g_ctrl_valid ||
        memcmp(ctx->g_ctrl_payload, &seq, sizeof(seq)) != 0) {
        if (v4l2sl_set_global_controls(v4l2_fd, &seq_ctrls) < 0) {
            fprintf(stderr, "v4l2stateless: failed to set AV1 sequence params\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        memcpy(ctx->g_ctrl_payload, &seq, sizeof(seq));
        ctx->g_ctrl_valid = 1;
    }

    {
        int w = (int)pic_param->frame_width_minus1 + 1;
        int h = (int)pic_param->frame_height_minus1 + 1;
        int chroma_idc = pic_param->seq_info_fields.fields.subsampling_x ? 1 : 2;
        uint32_t cap = v4l2sl_capture_fourcc_from_sps(
            pic_param->bit_depth_idx * 2, chroma_idc);

        if (v4l2sl_ensure_capture(ctx, w, h, cap) < 0) {
            fprintf(stderr, "v4l2stateless: AV1 capture reconfig failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    if (!ctx->streamed) {
        if (v4l2sl_streamon(v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0 ||
            v4l2sl_streamon(v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) {
            fprintf(stderr, "v4l2stateless: AV1 STREAMON failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        ctx->streamed = 1;
    }

    /* Film grain — kernel expects the control even when unused. */
    struct v4l2_ctrl_av1_film_grain grain;
    memset(&grain, 0, sizeof(grain));
    {
        const VAFilmGrainStructAV1 *fg = &pic_param->film_grain_info;
        if (fg->film_grain_info_fields.bits.apply_grain)
            grain.flags |= V4L2_AV1_FILM_GRAIN_FLAG_APPLY_GRAIN;
        if (fg->film_grain_info_fields.bits.chroma_scaling_from_luma)
            grain.flags |= V4L2_AV1_FILM_GRAIN_FLAG_CHROMA_SCALING_FROM_LUMA;
        if (fg->film_grain_info_fields.bits.overlap_flag)
            grain.flags |= V4L2_AV1_FILM_GRAIN_FLAG_OVERLAP;
        if (fg->film_grain_info_fields.bits.clip_to_restricted_range)
            grain.flags |= V4L2_AV1_FILM_GRAIN_FLAG_CLIP_TO_RESTRICTED_RANGE;
        grain.cr_mult = fg->cr_mult;
        grain.grain_seed = fg->grain_seed;
        grain.num_y_points = fg->num_y_points;
        memcpy(grain.point_y_value, fg->point_y_value, sizeof(fg->point_y_value));
        memcpy(grain.point_y_scaling, fg->point_y_scaling, sizeof(fg->point_y_scaling));
        grain.num_cb_points = fg->num_cb_points;
        memcpy(grain.point_cb_value, fg->point_cb_value, sizeof(fg->point_cb_value));
        memcpy(grain.point_cb_scaling, fg->point_cb_scaling, sizeof(fg->point_cb_scaling));
        grain.num_cr_points = fg->num_cr_points;
        memcpy(grain.point_cr_value, fg->point_cr_value, sizeof(fg->point_cr_value));
        memcpy(grain.point_cr_scaling, fg->point_cr_scaling, sizeof(fg->point_cr_scaling));
        grain.grain_scaling_minus_8 = fg->film_grain_info_fields.bits.grain_scaling_minus_8;
        grain.ar_coeff_lag = fg->film_grain_info_fields.bits.ar_coeff_lag;
        for (int i = 0; i < 24; i++)
            grain.ar_coeffs_y_plus_128[i] = (uint8_t)(fg->ar_coeffs_y[i] + 128);
        for (int i = 0; i < 25; i++) {
            grain.ar_coeffs_cb_plus_128[i] = (uint8_t)(fg->ar_coeffs_cb[i] + 128);
            grain.ar_coeffs_cr_plus_128[i] = (uint8_t)(fg->ar_coeffs_cr[i] + 128);
        }
        grain.ar_coeff_shift_minus_6 = fg->film_grain_info_fields.bits.ar_coeff_shift_minus_6;
        grain.grain_scale_shift = fg->film_grain_info_fields.bits.grain_scale_shift;
        grain.cb_mult = fg->cb_mult;
        grain.cb_luma_mult = fg->cb_luma_mult;
        grain.cr_luma_mult = fg->cr_luma_mult;
        grain.cb_offset = fg->cb_offset;
        grain.cr_offset = fg->cr_offset;
    }
    struct v4l2_ext_control grain_ctrl = { 0 };
    grain_ctrl.id = V4L2_CID_STATELESS_AV1_FILM_GRAIN;
    grain_ctrl.ptr = &grain;
    grain_ctrl.size = sizeof(grain);
    struct v4l2_ext_controls grain_ctrls = { 0 };
    grain_ctrls.controls = &grain_ctrl;
    grain_ctrls.count = 1;
    if (v4l2sl_set_request_controls(request_fd, v4l2_fd, &grain_ctrls) < 0)
        fprintf(stderr, "v4l2stateless: warning: AV1 film grain control failed\n");

    /* Set AV1 frame params */
    struct v4l2_ext_control frame_ctrl = { 0 };
    frame_ctrl.id = V4L2_CID_STATELESS_AV1_FRAME;
    frame_ctrl.p_av1_frame = &frame;
    frame_ctrl.size = sizeof(frame);

    struct v4l2_ext_controls frame_ctrls = { 0 };
    frame_ctrls.controls = &frame_ctrl;
    frame_ctrls.count = 1;

    if (v4l2sl_set_request_controls(request_fd, v4l2_fd, &frame_ctrls) < 0) {
        fprintf(stderr, "v4l2stateless: failed to set AV1 frame params\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }


    /*
     * Synchronous decode pipeline - mirrors H.264/HEVC: pop from pools,
     * queue, submit, wait, attach the frame to the surface.
     */
    if (ctx->n_free_out == 0) {
        fprintf(stderr, "v4l2stateless: AV1 no free output buffer\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    int out_buf_idx = ctx->free_out_bufs[--ctx->n_free_out];

    uint64_t timestamp = ctx->current_surface ? ctx->current_surface->timestamp : 0;

    /* ffmpeg's VAAPI AV1 hwaccel passes the same buffer it received from
     * av1dec (typically a TILE_GROUP / FRAME OBU) plus per-tile
     * slice_data_offset values relative to that buffer. Copy it verbatim
     * and honour those offsets — do not wrap another OBU. */
    /* This kernel's frame-based UAPI wants RAW TILE DATA in the OUTPUT
     * buffer — no OBU framing (ffmpeg submits it raw and is bit-exact).
     * Chrome submits the whole OBU span (sequence/frame OBUs) with each
     * tile's offset/size relative to that span. Extract just the tile
     * payloads and rebase the offsets onto the concatenated stream. */
    uint8_t *dst = ctx->output_buf_ptr[out_buf_idx];
    uint32_t total = 0;
    if (n_tiles > 0) {
        for (int i = 0; i < n_tiles; i++) {
            uint32_t off = tile_params[i]->slice_data_offset;
            uint32_t sz = tile_params[i]->slice_data_size;

            if (off > tile_data_size || sz > tile_data_size - off ||
                total + sz > ctx->output_buf_size) {
                fprintf(stderr, "v4l2stateless: AV1 tile %d out of range "
                        "(off=%u sz=%u buf=%u)\n", i, off, sz, tile_data_size);
                v4l2sl_out_pool_push(ctx, out_buf_idx);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            if (dst)
                memcpy(dst + total, tile_data + off, sz);
            total += sz;
        }
    } else {
        if (tile_data_size > ctx->output_buf_size) {
            fprintf(stderr, "v4l2stateless: AV1 tile data too large\n");
            v4l2sl_out_pool_push(ctx, out_buf_idx);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (dst)
            memcpy(dst, tile_data, tile_data_size);
        total = tile_data_size;
    }
    tile_data_size = total;
    if (!dst)
        fprintf(stderr, "v4l2stateless: AV1 output buffer not mapped\n");

    if (n_tiles > 0) {
        struct v4l2_ctrl_av1_tile_group_entry entries[32];
        uint32_t base = 0;
        for (int i = 0; i < n_tiles; i++) {
            memset(&entries[i], 0, sizeof(entries[i]));
            entries[i].tile_offset = base;
            entries[i].tile_size = tile_params[i]->slice_data_size;
            entries[i].tile_row = tile_params[i]->tile_row;
            entries[i].tile_col = tile_params[i]->tile_column;
            base += tile_params[i]->slice_data_size;
        }
        struct v4l2_ext_control tg_ctrl = { 0 };
        tg_ctrl.id = V4L2_CID_STATELESS_AV1_TILE_GROUP_ENTRY;
        tg_ctrl.ptr = entries;
        tg_ctrl.size = sizeof(entries[0]) * n_tiles;
        struct v4l2_ext_controls tg_ctrls = { 0 };
        tg_ctrls.controls = &tg_ctrl;
        tg_ctrls.count = 1;
        if (v4l2sl_set_request_controls(request_fd, v4l2_fd, &tg_ctrls) < 0) {
            fprintf(stderr, "v4l2stateless: failed to set AV1 tile group entries\n");
            v4l2sl_out_pool_push(ctx, out_buf_idx);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    /* On failure decode_submit resets both queues — do not push back. */
    int done_cap = v4l2sl_decode_submit(ctx, out_buf_idx, tile_data_size, timestamp);
    if (done_cap == -2) {
        /* Corrupt frame (V4L2_BUF_FLAG_ERROR): mark and succeed — a failed
         * entrypoint would be cached by Chrome for the whole session. */
        if (ctx->current_surface)
            ctx->current_surface->status = VASurfaceSkipped;
        return VA_STATUS_SUCCESS;
    }
    if (done_cap < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    struct v4l2sl_surface *surf = ctx->current_surface;
    if (surf) {
        if (v4l2sl_surface_pull_capture(ctx, surf, done_cap) < 0) {
            fprintf(stderr, "v4l2stateless: AV1 pull capture failed\n");
            v4l2sl_cap_pool_push(ctx, done_cap);
        } else {
            if (v4l2sl_debug)
                fprintf(stderr, "v4l2stateless: AV1 frame surf=%#x oh=%u buf=%d refresh=%02x type=%u\n",
                        surf->surface_id, pic_param->order_hint, done_cap,
                        frame.refresh_frame_flags,
                        pic_param->pic_info_fields.bits.frame_type);
            if (ctx->av1.model_active)
                av1_release_unrefd(ctx, surf, done_cap,
                                   frame.refresh_frame_flags);
        }
    } else {
        v4l2sl_cap_pool_push(ctx, done_cap);
    }

    return VA_STATUS_SUCCESS;
}
