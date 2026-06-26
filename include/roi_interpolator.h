/*
 * Copyright (C) 2026 ClutchReframe
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RIFTREFRAME_ROI_INTERPOLATOR_H
#define RIFTREFRAME_ROI_INTERPOLATOR_H

#include "roi_frame_msg_v1.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ROI_INTERPOLATOR_FREEZE_TIMEOUT_DEFAULT_MS = 500,
    ROI_INTERPOLATOR_FREEZE_TIMEOUT_MIN_MS = 0,
    ROI_INTERPOLATOR_FREEZE_TIMEOUT_MAX_MS = 10000,
    ROI_INTERPOLATOR_WARMUP_VALID_FRAMES_DEFAULT = 2
};

struct roi_interpolator;

typedef struct roi_interpolator_roi {
    float x;
    float y;
    float w;
    float h;
} roi_interpolator_roi_t;

typedef enum roi_interpolator_sample_mode {
    ROI_INTERPOLATOR_SAMPLE_MODE_NONE = 0,
    ROI_INTERPOLATOR_SAMPLE_MODE_INTERPOLATED,
    ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_PREV,
    ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_LAST,
    ROI_INTERPOLATOR_SAMPLE_MODE_TIMEOUT_FREEZE
} roi_interpolator_sample_mode_t;

typedef enum roi_interpolator_sample_reason {
    ROI_INTERPOLATOR_SAMPLE_REASON_NONE = 0,
    ROI_INTERPOLATOR_SAMPLE_REASON_MISS_RIGHT,
    ROI_INTERPOLATOR_SAMPLE_REASON_MISS_LEFT,
    ROI_INTERPOLATOR_SAMPLE_REASON_SPAN_INVALID,
    ROI_INTERPOLATOR_SAMPLE_REASON_TOO_CLOSE,
    ROI_INTERPOLATOR_SAMPLE_REASON_STREAM_RESET,
    ROI_INTERPOLATOR_SAMPLE_REASON_WARMUP,
    ROI_INTERPOLATOR_SAMPLE_REASON_FRAME_STREAM,
    ROI_INTERPOLATOR_SAMPLE_REASON_TIMEOUT_FREEZE
} roi_interpolator_sample_reason_t;

typedef struct roi_interpolator_sample {
    bool has_roi;
    bool warmup_active;
    bool timeout_freeze;
    roi_interpolator_sample_mode_t mode;
    roi_interpolator_sample_reason_t reason;
    roi_interpolator_roi_t roi;
} roi_interpolator_sample_t;

typedef struct roi_interpolator_stats {
    bool has_stream_state;
    bool warmup_active;
    uint32_t stream_id;
    uint32_t epoch;
    uint64_t qpc_frequency;
    bool frame_stream_mode;
    bool presend_v2_mode;
    uint32_t anchor_buffer_count;
    uint32_t anchor_buffer_peak;
    uint64_t anchor_buffer_drop_count;
    uint64_t accepted_valid_frame_count;
    uint64_t dropped_invalid_frame_count;
    uint64_t dropped_invalid_geometry_count;
    uint64_t valid_zero_count;
    uint64_t stream_reset_count;
    uint64_t warmup_enter_count;
    uint64_t warmup_exit_count;
    uint64_t sample_call_count;
    uint64_t sample_output_count;
    uint64_t sample_no_output_count;
    uint64_t timeout_freeze_count;
    uint64_t timeout_freeze_sample_count;
    uint64_t interpolated_count;
    uint64_t hold_prev_count;
    uint64_t hold_last_count;
    uint64_t buffered_span_count;
    uint64_t buffered_span_missing_left_count;
    int64_t buffered_last_next_lead_ticks;
} roi_interpolator_stats_t;

struct roi_interpolator *roi_interpolator_create(void);
void roi_interpolator_destroy(struct roi_interpolator *interpolator);

void roi_interpolator_reset(struct roi_interpolator *interpolator, bool keep_last_applied);
void roi_interpolator_set_debug_log(struct roi_interpolator *interpolator, bool debug_log);
void roi_interpolator_set_freeze_timeout_ms(struct roi_interpolator *interpolator,
                                            uint32_t timeout_ms);
void roi_interpolator_set_warmup_valid_frames(struct roi_interpolator *interpolator,
                                              uint32_t frame_count);

bool roi_interpolator_push_frame(struct roi_interpolator *interpolator,
                                 const roi_frame_msg_v1_t *frame,
                                 int64_t receive_qpc);
bool roi_interpolator_sample(struct roi_interpolator *interpolator,
                             int64_t render_qpc,
                             roi_interpolator_sample_t *out_sample);

void roi_interpolator_get_stats(const struct roi_interpolator *interpolator,
                                roi_interpolator_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* RIFTREFRAME_ROI_INTERPOLATOR_H */
