/*
 * Copyright (C) 2026 ClutchReframe
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "roi_interpolator.h"

#include <obs-module.h>

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

enum {
    ROI_INTERPOLATOR_WARMUP_VALID_FRAMES_MIN = 1,
    ROI_INTERPOLATOR_WARMUP_VALID_FRAMES_MAX = 8,
    ROI_INTERPOLATOR_ANCHOR_BUFFER_CAP = 32
};

static const float ROI_INTERPOLATOR_ROI_EPSILON = 1e-6f;

typedef struct roi_interpolator_anchor {
    uint64_t seq;
    int64_t t_qpc;
    roi_interpolator_roi_t roi;
} roi_interpolator_anchor_t;

struct roi_interpolator {
    bool debug_log;
    uint32_t freeze_timeout_ms;
    uint32_t warmup_required_valid_frames;

    bool has_stream_state;
    bool has_stream_mode;
    bool frame_stream_mode;
    bool presend_v2_mode;
    bool mode_mismatch_active;
    uint32_t stream_id;
    uint32_t epoch;
    uint64_t qpc_frequency;
    bool has_last_seq;
    uint64_t last_seq;

    bool warmup_active;
    uint32_t warmup_valid_frame_count;
    bool warmup_due_to_stream_reset;
    bool in_timeout_freeze;

    bool has_prev_anchor;
    roi_interpolator_anchor_t prev_anchor;
    bool has_next_anchor;
    roi_interpolator_anchor_t next_anchor;
    size_t anchor_buffer_count;
    roi_interpolator_anchor_t anchor_buffer[ROI_INTERPOLATOR_ANCHOR_BUFFER_CAP];

    bool has_last_applied_roi;
    roi_interpolator_roi_t last_applied_roi;

    bool has_last_input_receive_qpc;
    int64_t last_input_receive_qpc;

    roi_interpolator_stats_t stats;
};

static uint32_t roi_interpolator_clamp_freeze_timeout_ms(uint32_t timeout_ms)
{
    if (timeout_ms > ROI_INTERPOLATOR_FREEZE_TIMEOUT_MAX_MS) {
        return ROI_INTERPOLATOR_FREEZE_TIMEOUT_MAX_MS;
    }
    return timeout_ms;
}

static uint32_t roi_interpolator_clamp_warmup_valid_frames(uint32_t frame_count)
{
    if (frame_count < ROI_INTERPOLATOR_WARMUP_VALID_FRAMES_MIN) {
        return ROI_INTERPOLATOR_WARMUP_VALID_FRAMES_MIN;
    }
    if (frame_count > ROI_INTERPOLATOR_WARMUP_VALID_FRAMES_MAX) {
        return ROI_INTERPOLATOR_WARMUP_VALID_FRAMES_MAX;
    }
    return frame_count;
}

static void roi_interpolator_clear_anchors(struct roi_interpolator *interpolator)
{
    interpolator->has_prev_anchor = false;
    interpolator->has_next_anchor = false;
    memset(&interpolator->prev_anchor, 0, sizeof(interpolator->prev_anchor));
    memset(&interpolator->next_anchor, 0, sizeof(interpolator->next_anchor));
    interpolator->anchor_buffer_count = 0;
    memset(interpolator->anchor_buffer, 0, sizeof(interpolator->anchor_buffer));
}

static void roi_interpolator_enter_warmup(struct roi_interpolator *interpolator,
                                          bool due_to_stream_reset)
{
    roi_interpolator_clear_anchors(interpolator);
    interpolator->warmup_active = true;
    interpolator->warmup_valid_frame_count = 0;
    interpolator->warmup_due_to_stream_reset = due_to_stream_reset;
    interpolator->in_timeout_freeze = false;
    interpolator->stats.warmup_enter_count++;

    if (interpolator->debug_log) {
        blog(LOG_DEBUG,
             "[ClutchReframe] Interpolator warmup enter (%s).",
             due_to_stream_reset ? "stream-reset" : "stream-init");
    }
}

static void roi_interpolator_try_exit_warmup(struct roi_interpolator *interpolator)
{
    if (!interpolator->warmup_active) {
        return;
    }
    if (interpolator->warmup_valid_frame_count < interpolator->warmup_required_valid_frames) {
        return;
    }

    interpolator->warmup_active = false;
    interpolator->warmup_due_to_stream_reset = false;
    interpolator->stats.warmup_exit_count++;
    if (interpolator->debug_log) {
        blog(LOG_DEBUG,
             "[ClutchReframe] Interpolator warmup exit (frames=%u threshold=%u).",
             (unsigned int)interpolator->warmup_valid_frame_count,
             (unsigned int)interpolator->warmup_required_valid_frames);
    }
}

static bool roi_interpolator_try_normalize_roi(float x,
                                               float y,
                                               float w,
                                               float h,
                                               roi_interpolator_roi_t *out_roi)
{
    float max_w = 0.0f;
    float max_h = 0.0f;

    if (out_roi == NULL) {
        return false;
    }
    if (!isfinite((double)x) || !isfinite((double)y) ||
        !isfinite((double)w) || !isfinite((double)h)) {
        return false;
    }
    if (w <= 0.0f || h <= 0.0f) {
        return false;
    }
    if (x < -ROI_INTERPOLATOR_ROI_EPSILON || y < -ROI_INTERPOLATOR_ROI_EPSILON) {
        return false;
    }
    if (x > 1.0f + ROI_INTERPOLATOR_ROI_EPSILON ||
        y > 1.0f + ROI_INTERPOLATOR_ROI_EPSILON) {
        return false;
    }
    if (x + w > 1.0f + ROI_INTERPOLATOR_ROI_EPSILON ||
        y + h > 1.0f + ROI_INTERPOLATOR_ROI_EPSILON) {
        return false;
    }

    if (x < 0.0f) {
        x = 0.0f;
    }
    if (y < 0.0f) {
        y = 0.0f;
    }
    if (x > 1.0f) {
        x = 1.0f;
    }
    if (y > 1.0f) {
        y = 1.0f;
    }

    max_w = 1.0f - x;
    max_h = 1.0f - y;
    if (w > max_w) {
        w = max_w;
    }
    if (h > max_h) {
        h = max_h;
    }
    if (w <= ROI_INTERPOLATOR_ROI_EPSILON || h <= ROI_INTERPOLATOR_ROI_EPSILON) {
        return false;
    }

    out_roi->x = x;
    out_roi->y = y;
    out_roi->w = w;
    out_roi->h = h;
    return true;
}

static bool roi_interpolator_try_extract_valid_roi(const roi_frame_msg_v1_t *frame,
                                                   roi_interpolator_roi_t *out_roi)
{
    if (frame == NULL || out_roi == NULL || frame->valid == 0) {
        return false;
    }
    return roi_interpolator_try_normalize_roi(frame->x, frame->y, frame->w, frame->h, out_roi);
}

static void roi_interpolator_set_anchor_from_frame(roi_interpolator_anchor_t *anchor,
                                                   const roi_frame_msg_v1_t *frame,
                                                   const roi_interpolator_roi_t *roi)
{
    if (anchor == NULL || frame == NULL || roi == NULL) {
        return;
    }

    anchor->seq = frame->seq;
    anchor->t_qpc = frame->t_qpc;
    anchor->roi = *roi;
}

static void roi_interpolator_anchor_buffer_prune_before(struct roi_interpolator *interpolator,
                                                        size_t index)
{
    size_t remaining = 0;

    if (interpolator == NULL) {
        return;
    }
    if (index == 0) {
        return;
    }
    if (index >= interpolator->anchor_buffer_count) {
        interpolator->anchor_buffer_count = 0;
        memset(interpolator->anchor_buffer, 0, sizeof(interpolator->anchor_buffer));
        return;
    }

    remaining = interpolator->anchor_buffer_count - index;
    memmove(&interpolator->anchor_buffer[0],
            &interpolator->anchor_buffer[index],
            remaining * sizeof(interpolator->anchor_buffer[0]));
    interpolator->anchor_buffer_count = remaining;
}

static void roi_interpolator_push_anchor_buffered(struct roi_interpolator *interpolator,
                                                  const roi_frame_msg_v1_t *frame,
                                                  const roi_interpolator_roi_t *roi)
{
    roi_interpolator_anchor_t anchor;
    size_t last_index = 0;

    if (interpolator == NULL || frame == NULL || roi == NULL) {
        return;
    }

    memset(&anchor, 0, sizeof(anchor));
    roi_interpolator_set_anchor_from_frame(&anchor, frame, roi);

    if (interpolator->anchor_buffer_count == 0) {
        interpolator->anchor_buffer[0] = anchor;
        interpolator->anchor_buffer_count = 1;
        if (interpolator->anchor_buffer_count > (size_t)interpolator->stats.anchor_buffer_peak) {
            interpolator->stats.anchor_buffer_peak =
                (uint32_t)interpolator->anchor_buffer_count;
        }
        return;
    }

    last_index = interpolator->anchor_buffer_count - 1;
    if (anchor.t_qpc < interpolator->anchor_buffer[last_index].t_qpc) {
        /* Non-monotonic t_qpc: treat as rollback/reset within the same stream. */
        interpolator->anchor_buffer[0] = anchor;
        interpolator->anchor_buffer_count = 1;
        return;
    }
    if (anchor.t_qpc == interpolator->anchor_buffer[last_index].t_qpc) {
        /* Coalesce updates targeting the same effective time. */
        interpolator->anchor_buffer[last_index] = anchor;
        return;
    }

    if (interpolator->anchor_buffer_count >= ROI_INTERPOLATOR_ANCHOR_BUFFER_CAP) {
        roi_interpolator_anchor_buffer_prune_before(interpolator, 1);
        interpolator->stats.anchor_buffer_drop_count++;
    }
    if (interpolator->anchor_buffer_count < ROI_INTERPOLATOR_ANCHOR_BUFFER_CAP) {
        interpolator->anchor_buffer[interpolator->anchor_buffer_count] = anchor;
        interpolator->anchor_buffer_count++;
        if (interpolator->anchor_buffer_count > (size_t)interpolator->stats.anchor_buffer_peak) {
            interpolator->stats.anchor_buffer_peak =
                (uint32_t)interpolator->anchor_buffer_count;
        }
    }
}

static int64_t roi_interpolator_compute_timeout_ticks(const struct roi_interpolator *interpolator)
{
    uint64_t timeout_ticks_u64 = 0;
    uint64_t numerator = 0;

    if (interpolator == NULL) {
        return 0;
    }
    if (interpolator->freeze_timeout_ms == 0 || interpolator->qpc_frequency == 0) {
        return 0;
    }

    if (interpolator->qpc_frequency > 0 &&
        (uint64_t)interpolator->freeze_timeout_ms > UINT64_MAX / interpolator->qpc_frequency) {
        return INT64_MAX;
    }

    numerator = (uint64_t)interpolator->freeze_timeout_ms * interpolator->qpc_frequency;
    timeout_ticks_u64 = numerator / 1000u;
    if (timeout_ticks_u64 == 0u) {
        timeout_ticks_u64 = 1u;
    }
    if (timeout_ticks_u64 > (uint64_t)INT64_MAX) {
        return INT64_MAX;
    }
    return (int64_t)timeout_ticks_u64;
}

static bool roi_interpolator_is_timeout_freeze_active(const struct roi_interpolator *interpolator,
                                                      int64_t render_qpc)
{
    int64_t timeout_ticks = 0;
    int64_t elapsed_ticks = 0;

    if (interpolator == NULL || render_qpc <= 0) {
        return false;
    }
    if (!interpolator->has_last_input_receive_qpc || interpolator->last_input_receive_qpc <= 0) {
        return false;
    }
    if (render_qpc <= interpolator->last_input_receive_qpc) {
        return false;
    }

    timeout_ticks = roi_interpolator_compute_timeout_ticks(interpolator);
    if (timeout_ticks <= 0) {
        return false;
    }

    elapsed_ticks = render_qpc - interpolator->last_input_receive_qpc;
    return elapsed_ticks >= timeout_ticks;
}

static void roi_interpolator_record_sample_mode(struct roi_interpolator *interpolator,
                                                roi_interpolator_sample_mode_t mode)
{
    interpolator->stats.sample_output_count++;

    switch (mode) {
    case ROI_INTERPOLATOR_SAMPLE_MODE_INTERPOLATED:
        interpolator->stats.interpolated_count++;
        break;
    case ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_PREV:
        interpolator->stats.hold_prev_count++;
        break;
    case ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_LAST:
        interpolator->stats.hold_last_count++;
        break;
    case ROI_INTERPOLATOR_SAMPLE_MODE_TIMEOUT_FREEZE:
        interpolator->stats.hold_last_count++;
        interpolator->stats.timeout_freeze_sample_count++;
        break;
    case ROI_INTERPOLATOR_SAMPLE_MODE_NONE:
    default:
        break;
    }
}

static bool roi_interpolator_publish_sample(struct roi_interpolator *interpolator,
                                            const roi_interpolator_roi_t *roi,
                                            roi_interpolator_sample_mode_t mode,
                                            roi_interpolator_sample_reason_t reason,
                                            bool warmup_active,
                                            bool timeout_freeze,
                                            roi_interpolator_sample_t *out_sample)
{
    if (interpolator == NULL || roi == NULL || out_sample == NULL) {
        return false;
    }

    out_sample->has_roi = true;
    out_sample->warmup_active = warmup_active;
    out_sample->timeout_freeze = timeout_freeze;
    out_sample->mode = mode;
    out_sample->reason = reason;
    out_sample->roi = *roi;

    interpolator->last_applied_roi = *roi;
    interpolator->has_last_applied_roi = true;
    roi_interpolator_record_sample_mode(interpolator, mode);
    return true;
}

static bool roi_interpolator_lerp_between_anchors(const roi_interpolator_anchor_t *left,
                                                  const roi_interpolator_anchor_t *right,
                                                  int64_t render_qpc,
                                                  roi_interpolator_roi_t *out_roi)
{
    double alpha = 0.0;
    double denominator = 0.0;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    if (left == NULL || right == NULL || out_roi == NULL) {
        return false;
    }
    if (right->t_qpc <= left->t_qpc) {
        return roi_interpolator_try_normalize_roi(right->roi.x,
                                                  right->roi.y,
                                                  right->roi.w,
                                                  right->roi.h,
                                                  out_roi);
    }

    denominator = (double)(right->t_qpc - left->t_qpc);
    alpha = (double)(render_qpc - left->t_qpc) / denominator;
    if (alpha < 0.0) {
        alpha = 0.0;
    } else if (alpha > 1.0) {
        alpha = 1.0;
    }

    x = (float)(left->roi.x + (right->roi.x - left->roi.x) * alpha);
    y = (float)(left->roi.y + (right->roi.y - left->roi.y) * alpha);
    w = (float)(left->roi.w + (right->roi.w - left->roi.w) * alpha);
    h = (float)(left->roi.h + (right->roi.h - left->roi.h) * alpha);
    return roi_interpolator_try_normalize_roi(x, y, w, h, out_roi);
}

static void roi_interpolator_try_advance_anchor_window(struct roi_interpolator *interpolator,
                                                       int64_t render_qpc)
{
    if (interpolator == NULL || !interpolator->has_next_anchor) {
        return;
    }
    if (render_qpc < interpolator->next_anchor.t_qpc) {
        return;
    }

    interpolator->prev_anchor = interpolator->next_anchor;
    interpolator->has_next_anchor = false;
    memset(&interpolator->next_anchor, 0, sizeof(interpolator->next_anchor));
}

struct roi_interpolator *roi_interpolator_create(void)
{
    struct roi_interpolator *interpolator = bzalloc(sizeof(*interpolator));
    if (interpolator == NULL) {
        return NULL;
    }

    interpolator->freeze_timeout_ms = ROI_INTERPOLATOR_FREEZE_TIMEOUT_DEFAULT_MS;
    interpolator->warmup_required_valid_frames = ROI_INTERPOLATOR_WARMUP_VALID_FRAMES_DEFAULT;
    return interpolator;
}

void roi_interpolator_destroy(struct roi_interpolator *interpolator)
{
    if (interpolator == NULL) {
        return;
    }
    bfree(interpolator);
}

void roi_interpolator_reset(struct roi_interpolator *interpolator, bool keep_last_applied)
{
    if (interpolator == NULL) {
        return;
    }

    interpolator->has_stream_state = false;
    interpolator->has_stream_mode = false;
    interpolator->frame_stream_mode = false;
    interpolator->presend_v2_mode = false;
    interpolator->mode_mismatch_active = false;
    interpolator->stream_id = 0;
    interpolator->epoch = 0;
    interpolator->qpc_frequency = 0;
    interpolator->has_last_seq = false;
    interpolator->last_seq = 0;
    interpolator->warmup_active = false;
    interpolator->warmup_valid_frame_count = 0;
    interpolator->warmup_due_to_stream_reset = false;
    interpolator->in_timeout_freeze = false;
    interpolator->has_last_input_receive_qpc = false;
    interpolator->last_input_receive_qpc = 0;
    roi_interpolator_clear_anchors(interpolator);

    if (!keep_last_applied) {
        interpolator->has_last_applied_roi = false;
        memset(&interpolator->last_applied_roi, 0, sizeof(interpolator->last_applied_roi));
    }
}

void roi_interpolator_set_debug_log(struct roi_interpolator *interpolator, bool debug_log)
{
    if (interpolator == NULL) {
        return;
    }
    interpolator->debug_log = debug_log;
}

void roi_interpolator_set_freeze_timeout_ms(struct roi_interpolator *interpolator,
                                            uint32_t timeout_ms)
{
    if (interpolator == NULL) {
        return;
    }
    interpolator->freeze_timeout_ms = roi_interpolator_clamp_freeze_timeout_ms(timeout_ms);
}

void roi_interpolator_set_warmup_valid_frames(struct roi_interpolator *interpolator,
                                              uint32_t frame_count)
{
    if (interpolator == NULL) {
        return;
    }
    interpolator->warmup_required_valid_frames =
        roi_interpolator_clamp_warmup_valid_frames(frame_count);
}

bool roi_interpolator_push_frame(struct roi_interpolator *interpolator,
                                 const roi_frame_msg_v1_t *frame,
                                 int64_t receive_qpc)
{
    bool stream_changed = false;
    bool stream_reset = false;
    bool frame_stream_flag = false;
    bool presend_v2_flag = false;
    roi_interpolator_roi_t roi;

    if (interpolator == NULL || frame == NULL) {
        return false;
    }

    if (receive_qpc > 0) {
        interpolator->has_last_input_receive_qpc = true;
        interpolator->last_input_receive_qpc = receive_qpc;
    }

    if (!interpolator->has_stream_state) {
        interpolator->has_stream_state = true;
        interpolator->stream_id = frame->stream_id;
        interpolator->epoch = frame->epoch;
        interpolator->has_last_seq = false;
        stream_changed = true;
    } else if (frame->stream_id != interpolator->stream_id ||
               frame->epoch != interpolator->epoch) {
        interpolator->stats.stream_reset_count++;
        interpolator->stream_id = frame->stream_id;
        interpolator->epoch = frame->epoch;
        interpolator->has_last_seq = false;
        stream_changed = true;
        stream_reset = true;
    }

    if (frame->qpc_frequency > 0) {
        if (interpolator->qpc_frequency > 0 &&
            interpolator->qpc_frequency != frame->qpc_frequency &&
            interpolator->debug_log) {
            blog(LOG_WARNING,
                 "[ClutchReframe] Interpolator qpcFrequency changed in-stream (%llu -> %llu).",
                 (unsigned long long)interpolator->qpc_frequency,
                 (unsigned long long)frame->qpc_frequency);
        }
        interpolator->qpc_frequency = frame->qpc_frequency;
    }

    if (stream_changed) {
        interpolator->has_stream_mode = false;
        interpolator->frame_stream_mode = false;
        interpolator->presend_v2_mode = false;
        interpolator->mode_mismatch_active = false;
        roi_interpolator_enter_warmup(interpolator, stream_reset);
    }

    frame_stream_flag = (frame->flags & ROI_FRAME_MSG_V1_FLAG_FRAME_STREAM) != 0u;
    presend_v2_flag = (frame->flags & ROI_FRAME_MSG_V1_FLAG_PRESEND_V2) != 0u;
    if (!interpolator->has_stream_mode) {
        interpolator->has_stream_mode = true;
        interpolator->frame_stream_mode = frame_stream_flag;
        interpolator->presend_v2_mode = presend_v2_flag;
        interpolator->mode_mismatch_active = false;
        blog(LOG_INFO,
             "[ClutchReframe] Interpolator stream mode=%s (FLAG_FRAME_STREAM=%u FLAG_PRESEND_V2=%u).",
             interpolator->frame_stream_mode ? "frame-stream(apply-direct)" : "keyframe(interp)",
             (unsigned int)(interpolator->frame_stream_mode ? 1u : 0u),
             (unsigned int)(interpolator->presend_v2_mode ? 1u : 0u));
    } else if (frame_stream_flag != interpolator->frame_stream_mode ||
               presend_v2_flag != interpolator->presend_v2_mode) {
        interpolator->stats.dropped_invalid_frame_count++;
        if (!interpolator->mode_mismatch_active) {
            interpolator->mode_mismatch_active = true;
            roi_interpolator_enter_warmup(interpolator, true);
            blog(LOG_WARNING,
                 "[ClutchReframe] Interpolator stream mode mismatch; expected FLAG_FRAME_STREAM=%u FLAG_PRESEND_V2=%u but got %u/%u. Freeze ROI updates until stream reset (stream=%u epoch=%u seq=%llu flags=0x%02X).",
                 (unsigned int)(interpolator->frame_stream_mode ? 1u : 0u),
                 (unsigned int)(interpolator->presend_v2_mode ? 1u : 0u),
                 (unsigned int)(frame_stream_flag ? 1u : 0u),
                 (unsigned int)(presend_v2_flag ? 1u : 0u),
                 (unsigned int)frame->stream_id,
                 (unsigned int)frame->epoch,
                 (unsigned long long)frame->seq,
                 (unsigned int)frame->flags);
        }
    }

    if (interpolator->has_last_seq && frame->seq <= interpolator->last_seq) {
        interpolator->stats.dropped_invalid_frame_count++;
        if (interpolator->debug_log) {
            blog(LOG_DEBUG,
                 "[ClutchReframe] Interpolator dropped non-monotonic seq frame (prev=%llu now=%llu).",
                 (unsigned long long)interpolator->last_seq,
                 (unsigned long long)frame->seq);
        }
        return false;
    }
    interpolator->has_last_seq = true;
    interpolator->last_seq = frame->seq;

    if (interpolator->mode_mismatch_active) {
        /* Fail-fast: keep consuming as keepalive (avoid timeout-freeze), but freeze ROI updates. */
        if (frame->valid == 0u) {
            interpolator->stats.valid_zero_count++;
        }
        return true;
    }

    if (frame->valid == 0u) {
        interpolator->stats.valid_zero_count++;
        return true;
    }

    if (!roi_interpolator_try_extract_valid_roi(frame, &roi)) {
        interpolator->stats.dropped_invalid_geometry_count++;
        if (interpolator->debug_log) {
            blog(LOG_DEBUG,
                 "[ClutchReframe] Interpolator dropped invalid ROI geometry (seq=%llu).",
                 (unsigned long long)frame->seq);
        }
        return false;
    }

    if (interpolator->presend_v2_mode) {
        roi_interpolator_push_anchor_buffered(interpolator, frame, &roi);
    } else {
        if (!interpolator->has_prev_anchor) {
            roi_interpolator_set_anchor_from_frame(&interpolator->prev_anchor, frame, &roi);
            interpolator->has_prev_anchor = true;
            interpolator->has_next_anchor = false;
        } else if (frame->t_qpc <= interpolator->prev_anchor.t_qpc) {
            roi_interpolator_set_anchor_from_frame(&interpolator->prev_anchor, frame, &roi);
            interpolator->has_next_anchor = false;
            memset(&interpolator->next_anchor, 0, sizeof(interpolator->next_anchor));
        } else if (!interpolator->has_next_anchor) {
            roi_interpolator_set_anchor_from_frame(&interpolator->next_anchor, frame, &roi);
            interpolator->has_next_anchor = true;
        } else if (frame->t_qpc <= interpolator->next_anchor.t_qpc) {
            roi_interpolator_set_anchor_from_frame(&interpolator->next_anchor, frame, &roi);
        } else {
            interpolator->prev_anchor = interpolator->next_anchor;
            roi_interpolator_set_anchor_from_frame(&interpolator->next_anchor, frame, &roi);
            interpolator->has_next_anchor = true;
        }
    }

    interpolator->stats.accepted_valid_frame_count++;
    if (interpolator->warmup_active && interpolator->warmup_valid_frame_count < UINT32_MAX) {
        interpolator->warmup_valid_frame_count++;
    }
    roi_interpolator_try_exit_warmup(interpolator);
    return true;
}

bool roi_interpolator_sample(struct roi_interpolator *interpolator,
                             int64_t render_qpc,
                             roi_interpolator_sample_t *out_sample)
{
    roi_interpolator_roi_t sampled_roi;
    const roi_interpolator_sample_reason_t warmup_reason =
        (interpolator != NULL && interpolator->warmup_due_to_stream_reset)
            ? ROI_INTERPOLATOR_SAMPLE_REASON_STREAM_RESET
            : ROI_INTERPOLATOR_SAMPLE_REASON_WARMUP;

    if (out_sample == NULL) {
        return false;
    }
    memset(out_sample, 0, sizeof(*out_sample));
    out_sample->mode = ROI_INTERPOLATOR_SAMPLE_MODE_NONE;
    out_sample->reason = ROI_INTERPOLATOR_SAMPLE_REASON_NONE;

    if (interpolator == NULL) {
        return false;
    }
    interpolator->stats.sample_call_count++;

    if (roi_interpolator_is_timeout_freeze_active(interpolator, render_qpc)) {
        if (!interpolator->in_timeout_freeze) {
            interpolator->stats.timeout_freeze_count++;
            if (interpolator->debug_log) {
                blog(LOG_DEBUG, "[ClutchReframe] Interpolator timeout freeze entered.");
            }
        }
        interpolator->in_timeout_freeze = true;

        if (interpolator->has_last_applied_roi) {
            return roi_interpolator_publish_sample(interpolator,
                                                   &interpolator->last_applied_roi,
                                                   ROI_INTERPOLATOR_SAMPLE_MODE_TIMEOUT_FREEZE,
                                                   ROI_INTERPOLATOR_SAMPLE_REASON_TIMEOUT_FREEZE,
                                                   interpolator->warmup_active,
                                                   true,
                                                   out_sample);
        }
        if (interpolator->has_prev_anchor) {
            return roi_interpolator_publish_sample(interpolator,
                                                   &interpolator->prev_anchor.roi,
                                                   ROI_INTERPOLATOR_SAMPLE_MODE_TIMEOUT_FREEZE,
                                                   ROI_INTERPOLATOR_SAMPLE_REASON_TIMEOUT_FREEZE,
                                                   interpolator->warmup_active,
                                                   true,
                                                   out_sample);
        }

        interpolator->stats.sample_no_output_count++;
        out_sample->warmup_active = interpolator->warmup_active;
        out_sample->timeout_freeze = true;
        return false;
    }
    interpolator->in_timeout_freeze = false;

    if (interpolator->presend_v2_mode) {
        const roi_interpolator_anchor_t *left = NULL;
        const roi_interpolator_anchor_t *right = NULL;
        bool has_left = false;
        bool has_right = false;
        bool too_close = false;
        size_t left_index = 0;
        size_t right_index = 0;

        if (interpolator->anchor_buffer_count > 0) {
            size_t i = 0;
            const size_t anchor_count = interpolator->anchor_buffer_count;
            bool found_right = false;
            size_t last_left = 0;
            bool found_left = false;
            const int64_t newest_t_qpc =
                interpolator->anchor_buffer[anchor_count - 1].t_qpc;

            if (render_qpc > 0 && newest_t_qpc <= render_qpc) {
                too_close = true;
            }

            for (i = 0; i < anchor_count; ++i) {
                if (interpolator->anchor_buffer[i].t_qpc <= render_qpc) {
                    found_left = true;
                    last_left = i;
                } else {
                    found_right = true;
                    right_index = i;
                    break;
                }
            }

            if (found_left) {
                left_index = last_left;
                if (left_index > 0) {
                    roi_interpolator_anchor_buffer_prune_before(interpolator, left_index);
                    if (found_right) {
                        right_index -= left_index;
                    }
                    left_index = 0;
                }
                left = &interpolator->anchor_buffer[left_index];
                has_left = true;
            }
            if (found_right && right_index < interpolator->anchor_buffer_count) {
                right = &interpolator->anchor_buffer[right_index];
                has_right = true;
            }
        }

        interpolator->stats.buffered_last_next_lead_ticks = 0;
        if (has_right && right != NULL && render_qpc > 0) {
            interpolator->stats.buffered_last_next_lead_ticks =
                right->t_qpc - render_qpc;
        }
        if (has_right && !has_left) {
            interpolator->stats.buffered_span_missing_left_count++;
        }
        if (has_left && has_right) {
            interpolator->stats.buffered_span_count++;
        }

        if (interpolator->warmup_active) {
            if (interpolator->has_last_applied_roi) {
                return roi_interpolator_publish_sample(interpolator,
                                                       &interpolator->last_applied_roi,
                                                       ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_LAST,
                                                       warmup_reason,
                                                       true,
                                                       false,
                                                       out_sample);
            }
            if (has_left && left != NULL) {
                return roi_interpolator_publish_sample(interpolator,
                                                       &left->roi,
                                                       ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_PREV,
                                                       warmup_reason,
                                                       true,
                                                       false,
                                                       out_sample);
            }
            interpolator->stats.sample_no_output_count++;
            out_sample->warmup_active = true;
            return false;
        }

        if (has_left && has_right && left != NULL && right != NULL) {
            if (interpolator->has_stream_mode && interpolator->frame_stream_mode) {
                return roi_interpolator_publish_sample(interpolator,
                                                       &left->roi,
                                                       ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_PREV,
                                                       ROI_INTERPOLATOR_SAMPLE_REASON_FRAME_STREAM,
                                                       false,
                                                       false,
                                                       out_sample);
            }
            if (roi_interpolator_lerp_between_anchors(left, right, render_qpc, &sampled_roi)) {
                return roi_interpolator_publish_sample(interpolator,
                                                       &sampled_roi,
                                                       ROI_INTERPOLATOR_SAMPLE_MODE_INTERPOLATED,
                                                       ROI_INTERPOLATOR_SAMPLE_REASON_NONE,
                                                       false,
                                                       false,
                                                       out_sample);
            }

            if (interpolator->has_last_applied_roi) {
                return roi_interpolator_publish_sample(interpolator,
                                                       &interpolator->last_applied_roi,
                                                       ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_LAST,
                                                       ROI_INTERPOLATOR_SAMPLE_REASON_SPAN_INVALID,
                                                       false,
                                                       false,
                                                       out_sample);
            }
            interpolator->stats.sample_no_output_count++;
            return false;
        }

        if (has_left && left != NULL) {
            return roi_interpolator_publish_sample(interpolator,
                                                   &left->roi,
                                                   ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_PREV,
                                                   too_close ? ROI_INTERPOLATOR_SAMPLE_REASON_TOO_CLOSE
                                                             : ROI_INTERPOLATOR_SAMPLE_REASON_MISS_RIGHT,
                                                   false,
                                                   false,
                                                   out_sample);
        }

        if (interpolator->has_last_applied_roi) {
            roi_interpolator_sample_reason_t reason = ROI_INTERPOLATOR_SAMPLE_REASON_NONE;
            if (has_right && !has_left) {
                reason = ROI_INTERPOLATOR_SAMPLE_REASON_MISS_LEFT;
            } else if (too_close) {
                reason = ROI_INTERPOLATOR_SAMPLE_REASON_TOO_CLOSE;
            } else {
                reason = ROI_INTERPOLATOR_SAMPLE_REASON_MISS_RIGHT;
            }
            return roi_interpolator_publish_sample(interpolator,
                                                   &interpolator->last_applied_roi,
                                                   ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_LAST,
                                                   reason,
                                                   false,
                                                   false,
                                                   out_sample);
        }

        interpolator->stats.sample_no_output_count++;
        return false;
    }

    const bool legacy_next_too_close =
        interpolator->has_next_anchor && render_qpc >= interpolator->next_anchor.t_qpc;
    roi_interpolator_try_advance_anchor_window(interpolator, render_qpc);

    if (interpolator->warmup_active) {
        if (interpolator->has_last_applied_roi) {
            return roi_interpolator_publish_sample(interpolator,
                                                   &interpolator->last_applied_roi,
                                                   ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_LAST,
                                                   warmup_reason,
                                                   true,
                                                   false,
                                                   out_sample);
        }
        if (interpolator->has_prev_anchor) {
            return roi_interpolator_publish_sample(interpolator,
                                                   &interpolator->prev_anchor.roi,
                                                   ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_PREV,
                                                   warmup_reason,
                                                   true,
                                                   false,
                                                   out_sample);
        }
        interpolator->stats.sample_no_output_count++;
        out_sample->warmup_active = true;
        return false;
    }

    if (interpolator->has_prev_anchor && interpolator->has_next_anchor) {
        if (interpolator->has_stream_mode && interpolator->frame_stream_mode) {
            return roi_interpolator_publish_sample(interpolator,
                                                   &interpolator->prev_anchor.roi,
                                                   ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_PREV,
                                                   ROI_INTERPOLATOR_SAMPLE_REASON_FRAME_STREAM,
                                                   false,
                                                   false,
                                                   out_sample);
        }
        if (roi_interpolator_lerp_between_anchors(&interpolator->prev_anchor,
                                                  &interpolator->next_anchor,
                                                  render_qpc,
                                                  &sampled_roi)) {
            return roi_interpolator_publish_sample(interpolator,
                                                   &sampled_roi,
                                                   ROI_INTERPOLATOR_SAMPLE_MODE_INTERPOLATED,
                                                   ROI_INTERPOLATOR_SAMPLE_REASON_NONE,
                                                   false,
                                                   false,
                                                   out_sample);
        }

        if (interpolator->has_last_applied_roi) {
            return roi_interpolator_publish_sample(interpolator,
                                                   &interpolator->last_applied_roi,
                                                   ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_LAST,
                                                   ROI_INTERPOLATOR_SAMPLE_REASON_SPAN_INVALID,
                                                   false,
                                                   false,
                                                   out_sample);
        }
        interpolator->stats.sample_no_output_count++;
        return false;
    }

    if (interpolator->has_prev_anchor) {
        return roi_interpolator_publish_sample(interpolator,
                                               &interpolator->prev_anchor.roi,
                                               ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_PREV,
                                               legacy_next_too_close ? ROI_INTERPOLATOR_SAMPLE_REASON_TOO_CLOSE
                                                                     : ROI_INTERPOLATOR_SAMPLE_REASON_MISS_RIGHT,
                                               false,
                                               false,
                                               out_sample);
    }

    if (interpolator->has_last_applied_roi) {
        return roi_interpolator_publish_sample(interpolator,
                                               &interpolator->last_applied_roi,
                                               ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_LAST,
                                               ROI_INTERPOLATOR_SAMPLE_REASON_MISS_LEFT,
                                               false,
                                               false,
                                               out_sample);
    }

    interpolator->stats.sample_no_output_count++;
    return false;
}

void roi_interpolator_get_stats(const struct roi_interpolator *interpolator,
                                roi_interpolator_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }

    memset(out_stats, 0, sizeof(*out_stats));
    if (interpolator == NULL) {
        return;
    }

    *out_stats = interpolator->stats;
    out_stats->has_stream_state = interpolator->has_stream_state;
    out_stats->warmup_active = interpolator->warmup_active;
    out_stats->stream_id = interpolator->stream_id;
    out_stats->epoch = interpolator->epoch;
    out_stats->qpc_frequency = interpolator->qpc_frequency;
    out_stats->frame_stream_mode = interpolator->frame_stream_mode;
    out_stats->presend_v2_mode = interpolator->presend_v2_mode;
    out_stats->anchor_buffer_count = (uint32_t)interpolator->anchor_buffer_count;
}
