/*
 * Copyright (C) 2026 ClutchReframe
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <obs-module.h>

#include "roi_ipc_client.h"
#include "roi_interpolator.h"
#include "roi_frame_msg_v1.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef _WIN32
#error "roi_filter_source requires Windows QPC APIs."
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum {
    ROI_FILTER_PIPE_NAME_MAX = 260,
    ROI_FILTER_CONNECT_RETRY_MIN_MS = 50,
    ROI_FILTER_CONNECT_RETRY_MAX_MS = 5000,
    ROI_FILTER_CONNECT_RETRY_DEFAULT_MS = 500,
    ROI_FILTER_IPC_QUEUE_CAP_MIN = 1,
    ROI_FILTER_IPC_QUEUE_CAP_MAX = 64,
    ROI_FILTER_IPC_QUEUE_CAP_DEFAULT = 12,
    ROI_FILTER_STALE_DROP_MIN_MS = 0,
    ROI_FILTER_STALE_DROP_MAX_MS = 5000,
    ROI_FILTER_STALE_DROP_DEFAULT_MS = 120,
    ROI_FILTER_ROI_APPLY_OFFSET_MIN_MS = -2000,
    ROI_FILTER_ROI_APPLY_OFFSET_MAX_MS = 2000
};

static const char *ROI_FILTER_DEFAULT_PIPE_NAME = "\\\\.\\pipe\\ClutchReframe_ROI_default";

typedef enum roi_filter_presend_v2_consumer_mode {
    ROI_FILTER_PRESEND_V2_CONSUMER_MODE_AUTO = 0,
    ROI_FILTER_PRESEND_V2_CONSUMER_MODE_FORCE_LEGACY = 1,
    ROI_FILTER_PRESEND_V2_CONSUMER_MODE_FORCE_BUFFERED = 2
} roi_filter_presend_v2_consumer_mode_t;

struct roi_filter_source {
    obs_source_t *context;
    bool enabled;
    bool preserve_aspect;
    bool debug_log;
    uint32_t connect_retry_ms;
    uint32_t freeze_timeout_ms;
    uint32_t ipc_queue_cap;
    uint32_t stale_drop_ms;
    uint32_t presend_v2_consumer_mode;
    int32_t roi_apply_offset_ms;
    char pipe_name[ROI_FILTER_PIPE_NAME_MAX];
    struct roi_ipc_client *ipc_client;
    struct roi_interpolator *interpolator;
    bool has_last_connected_state;
    bool last_connected_state;
    bool has_last_consumed_frame;
    uint64_t last_consumed_generation;
    roi_frame_msg_v1_t last_consumed_frame;
    bool has_last_consumed_valid_roi;
    roi_interpolator_roi_t last_consumed_valid_roi;
    uint64_t consumed_frame_count;
    uint64_t consumed_valid_roi_count;
    uint64_t consumed_roi_changed_count;
    uint64_t consumed_roi_unchanged_count;
    bool has_last_sample;
    roi_interpolator_sample_t last_sample;
    bool has_last_metrics_snapshot;
    float metrics_log_elapsed_s;
    roi_ipc_client_stats_t last_metrics_snapshot;
    bool has_last_interpolator_metrics_snapshot;
    roi_interpolator_stats_t last_interpolator_metrics_snapshot;
    bool has_last_cause_snapshot;
    uint64_t last_cause_accept_count;
    uint64_t last_cause_interpolated_count;
    uint64_t last_cause_hold_prev_count;
    uint64_t last_cause_hold_last_count;
    uint64_t last_cause_timeout_freeze_sample_count;
    uint64_t last_cause_render_apply_count;
    uint64_t last_cause_consumed_valid_roi_count;
    uint64_t last_cause_consumed_roi_changed_count;
    uint64_t last_cause_consumed_roi_unchanged_count;
    uint64_t cause_upstream_no_new_observation_count;
    uint64_t cause_roi_unchanged_count;
    uint64_t cause_apply_suppressed_count;
    uint64_t cause_timeout_freeze_count;
    uint64_t qpc_offset_apply_error_count;
    bool tqpc_effective_time_offset_conflict_active;
    uint64_t tqpc_effective_time_offset_conflict_count;
    uint64_t stale_drop_count;
    uint64_t stale_drop_valid_count;
    int64_t stale_drop_max_age_ms;
    uint64_t arrival_lead_sample_count;
    uint64_t arrival_lead_positive_count;
    uint64_t arrival_lead_negative_count;
    int64_t arrival_lead_min_ticks;
    int64_t arrival_lead_max_ticks;
    uint64_t render_apply_count;
    uint64_t render_fallback_count;
    uint64_t render_invalid_geometry_count;
    uint64_t render_texrender_error_count;
    gs_texrender_t *render_texrender;
};

typedef struct roi_filter_subregion_px {
    uint32_t x;
    uint32_t y;
    uint32_t cx;
    uint32_t cy;
} roi_filter_subregion_px_t;

typedef enum roi_filter_root_cause {
    ROI_FILTER_ROOT_CAUSE_HEALTHY = 0,
    ROI_FILTER_ROOT_CAUSE_UPSTREAM_NO_NEW_OBSERVATION,
    ROI_FILTER_ROOT_CAUSE_ROI_UNCHANGED,
    ROI_FILTER_ROOT_CAUSE_APPLY_SUPPRESSED,
    ROI_FILTER_ROOT_CAUSE_TIMEOUT_FREEZE
} roi_filter_root_cause_t;

static void roi_filter_copy_pipe_name(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;

    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL || src[0] == '\0') {
        src = ROI_FILTER_DEFAULT_PIPE_NAME;
    }

    for (i = 0; i + 1 < dst_size && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static uint32_t roi_filter_clamp_connect_retry_ms(int64_t value)
{
    if (value < ROI_FILTER_CONNECT_RETRY_MIN_MS) {
        return ROI_FILTER_CONNECT_RETRY_MIN_MS;
    }
    if (value > ROI_FILTER_CONNECT_RETRY_MAX_MS) {
        return ROI_FILTER_CONNECT_RETRY_MAX_MS;
    }
    return (uint32_t)value;
}

static uint32_t roi_filter_clamp_freeze_timeout_ms(int64_t value)
{
    if (value < ROI_INTERPOLATOR_FREEZE_TIMEOUT_MIN_MS) {
        return ROI_INTERPOLATOR_FREEZE_TIMEOUT_MIN_MS;
    }
    if (value > ROI_INTERPOLATOR_FREEZE_TIMEOUT_MAX_MS) {
        return ROI_INTERPOLATOR_FREEZE_TIMEOUT_MAX_MS;
    }
    return (uint32_t)value;
}

static uint32_t roi_filter_clamp_ipc_queue_cap(int64_t value)
{
    if (value < ROI_FILTER_IPC_QUEUE_CAP_MIN) {
        return ROI_FILTER_IPC_QUEUE_CAP_MIN;
    }
    if (value > ROI_FILTER_IPC_QUEUE_CAP_MAX) {
        return ROI_FILTER_IPC_QUEUE_CAP_MAX;
    }
    return (uint32_t)value;
}

static uint32_t roi_filter_clamp_stale_drop_ms(int64_t value)
{
    if (value < ROI_FILTER_STALE_DROP_MIN_MS) {
        return ROI_FILTER_STALE_DROP_MIN_MS;
    }
    if (value > ROI_FILTER_STALE_DROP_MAX_MS) {
        return ROI_FILTER_STALE_DROP_MAX_MS;
    }
    return (uint32_t)value;
}

static int32_t roi_filter_clamp_roi_apply_offset_ms(int64_t value)
{
    if (value < ROI_FILTER_ROI_APPLY_OFFSET_MIN_MS) {
        return ROI_FILTER_ROI_APPLY_OFFSET_MIN_MS;
    }
    if (value > ROI_FILTER_ROI_APPLY_OFFSET_MAX_MS) {
        return ROI_FILTER_ROI_APPLY_OFFSET_MAX_MS;
    }
    return (int32_t)value;
}

static roi_filter_presend_v2_consumer_mode_t roi_filter_clamp_presend_v2_consumer_mode(int64_t value)
{
    if (value <= (int64_t)ROI_FILTER_PRESEND_V2_CONSUMER_MODE_AUTO) {
        return ROI_FILTER_PRESEND_V2_CONSUMER_MODE_AUTO;
    }
    if (value >= (int64_t)ROI_FILTER_PRESEND_V2_CONSUMER_MODE_FORCE_BUFFERED) {
        return ROI_FILTER_PRESEND_V2_CONSUMER_MODE_FORCE_BUFFERED;
    }
    return (roi_filter_presend_v2_consumer_mode_t)value;
}

static const char *roi_filter_presend_v2_consumer_mode_name(uint32_t mode)
{
    switch ((roi_filter_presend_v2_consumer_mode_t)mode) {
    case ROI_FILTER_PRESEND_V2_CONSUMER_MODE_AUTO:
        return "auto";
    case ROI_FILTER_PRESEND_V2_CONSUMER_MODE_FORCE_LEGACY:
        return "force_legacy";
    case ROI_FILTER_PRESEND_V2_CONSUMER_MODE_FORCE_BUFFERED:
        return "force_buffered";
    default:
        return "unknown";
    }
}

static bool roi_filter_should_log_count(uint64_t count)
{
    return count <= 3u || (count % 120u) == 0u;
}

static uint64_t roi_filter_delta_u64(uint64_t current, uint64_t previous)
{
    if (current >= previous) {
        return current - previous;
    }
    return current;
}

static bool roi_filter_roi_nearly_equal(const roi_interpolator_roi_t *lhs,
                                        const roi_interpolator_roi_t *rhs)
{
    const float epsilon = 1e-5f;
    if (lhs == NULL || rhs == NULL) {
        return false;
    }
    return fabsf(lhs->x - rhs->x) <= epsilon &&
           fabsf(lhs->y - rhs->y) <= epsilon &&
           fabsf(lhs->w - rhs->w) <= epsilon &&
           fabsf(lhs->h - rhs->h) <= epsilon;
}

static const char *roi_filter_root_cause_to_string(roi_filter_root_cause_t cause)
{
    switch (cause) {
    case ROI_FILTER_ROOT_CAUSE_UPSTREAM_NO_NEW_OBSERVATION:
        return "upstream-no-new-observation";
    case ROI_FILTER_ROOT_CAUSE_ROI_UNCHANGED:
        return "roi-unchanged";
    case ROI_FILTER_ROOT_CAUSE_APPLY_SUPPRESSED:
        return "apply-suppressed(inferred)";
    case ROI_FILTER_ROOT_CAUSE_TIMEOUT_FREEZE:
        return "timeout-freeze";
    case ROI_FILTER_ROOT_CAUSE_HEALTHY:
    default:
        return "healthy";
    }
}

static bool roi_filter_try_query_now_qpc(int64_t *out_qpc)
{
    LARGE_INTEGER qpc_now;

    if (out_qpc == NULL) {
        return false;
    }
    if (!QueryPerformanceCounter(&qpc_now) || qpc_now.QuadPart <= 0) {
        return false;
    }

    *out_qpc = qpc_now.QuadPart;
    return true;
}

static bool roi_filter_try_apply_qpc_offset_ticks(roi_frame_msg_v1_t *message, int32_t offset_ms)
{
    int64_t qpc_frequency = 0;
    int64_t offset_ticks = 0;
    int64_t abs_offset_ms = 0;

    if (message == NULL || offset_ms == 0) {
        return message != NULL;
    }
    if (message->qpc_frequency == 0 || message->qpc_frequency > (uint64_t)INT64_MAX) {
        return false;
    }

    qpc_frequency = (int64_t)message->qpc_frequency;
    abs_offset_ms = offset_ms < 0 ? -(int64_t)offset_ms : (int64_t)offset_ms;

    if (abs_offset_ms > 0 && qpc_frequency > INT64_MAX / abs_offset_ms) {
        return false;
    }

    offset_ticks = (qpc_frequency * abs_offset_ms) / 1000;
    if (offset_ms < 0) {
        offset_ticks = -offset_ticks;
    }

    if ((offset_ticks > 0 && message->t_qpc > INT64_MAX - offset_ticks) ||
        (offset_ticks < 0 && message->t_qpc < INT64_MIN - offset_ticks)) {
        return false;
    }

    message->t_qpc += offset_ticks;
    return true;
}

static bool roi_filter_try_compute_stale_threshold_ticks(uint32_t stale_drop_ms,
                                                         uint64_t qpc_frequency,
                                                         int64_t *out_threshold_ticks)
{
    int64_t frequency = 0;
    int64_t threshold_ticks = 0;

    if (out_threshold_ticks == NULL || stale_drop_ms == 0u) {
        return false;
    }
    if (qpc_frequency == 0u || qpc_frequency > (uint64_t)INT64_MAX) {
        return false;
    }

    frequency = (int64_t)qpc_frequency;
    if (frequency > INT64_MAX / (int64_t)stale_drop_ms) {
        return false;
    }

    threshold_ticks = (frequency * (int64_t)stale_drop_ms) / 1000;
    if (threshold_ticks <= 0) {
        threshold_ticks = 1;
    }

    *out_threshold_ticks = threshold_ticks;
    return true;
}

static int64_t roi_filter_ticks_to_ms_clamped(int64_t ticks, uint64_t qpc_frequency)
{
    int64_t frequency = 0;

    if (ticks <= 0 || qpc_frequency == 0u || qpc_frequency > (uint64_t)INT64_MAX) {
        return 0;
    }

    frequency = (int64_t)qpc_frequency;
    if (ticks > INT64_MAX / 1000) {
        return INT64_MAX;
    }

    return (ticks * 1000) / frequency;
}

static int64_t roi_filter_ticks_to_ms_signed_clamped(int64_t ticks, uint64_t qpc_frequency)
{
    int64_t abs_ticks = 0;
    int64_t ms = 0;

    if (ticks == 0) {
        return 0;
    }
    if (ticks == INT64_MIN) {
        abs_ticks = INT64_MAX;
    } else {
        abs_ticks = ticks < 0 ? -ticks : ticks;
    }

    ms = roi_filter_ticks_to_ms_clamped(abs_ticks, qpc_frequency);
    if (ticks < 0) {
        return -ms;
    }
    return ms;
}

static const char *roi_filter_sample_mode_to_string(roi_interpolator_sample_mode_t mode)
{
    switch (mode) {
    case ROI_INTERPOLATOR_SAMPLE_MODE_INTERPOLATED:
        return "interpolated";
    case ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_PREV:
        return "hold-prev";
    case ROI_INTERPOLATOR_SAMPLE_MODE_HOLD_LAST:
        return "hold-last";
    case ROI_INTERPOLATOR_SAMPLE_MODE_TIMEOUT_FREEZE:
        return "timeout-freeze";
    case ROI_INTERPOLATOR_SAMPLE_MODE_NONE:
    default:
        return "none";
    }
}

static const char *roi_filter_sample_reason_to_string(roi_interpolator_sample_reason_t reason)
{
    switch (reason) {
    case ROI_INTERPOLATOR_SAMPLE_REASON_MISS_RIGHT:
        return "missRight";
    case ROI_INTERPOLATOR_SAMPLE_REASON_MISS_LEFT:
        return "missLeft";
    case ROI_INTERPOLATOR_SAMPLE_REASON_SPAN_INVALID:
        return "spanInvalid";
    case ROI_INTERPOLATOR_SAMPLE_REASON_TOO_CLOSE:
        return "tooClose";
    case ROI_INTERPOLATOR_SAMPLE_REASON_STREAM_RESET:
        return "streamReset";
    case ROI_INTERPOLATOR_SAMPLE_REASON_WARMUP:
        return "warmup";
    case ROI_INTERPOLATOR_SAMPLE_REASON_FRAME_STREAM:
        return "frameStream";
    case ROI_INTERPOLATOR_SAMPLE_REASON_TIMEOUT_FREEZE:
        return "timeoutFreeze";
    case ROI_INTERPOLATOR_SAMPLE_REASON_NONE:
    default:
        return "none";
    }
}

static bool roi_filter_try_compute_subregion_px(const roi_interpolator_roi_t *roi,
                                                uint32_t source_width,
                                                uint32_t source_height,
                                                roi_filter_subregion_px_t *out_subregion)
{
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    int64_t x = 0;
    int64_t y = 0;
    int64_t right_px = 0;
    int64_t bottom_px = 0;
    float roi_x = 0.0f;
    float roi_y = 0.0f;
    float roi_w = 0.0f;
    float roi_h = 0.0f;
    float max_w = 0.0f;
    float max_h = 0.0f;

    if (roi == NULL || out_subregion == NULL || source_width == 0 || source_height == 0) {
        return false;
    }
    if (!isfinite((double)roi->x) || !isfinite((double)roi->y) ||
        !isfinite((double)roi->w) || !isfinite((double)roi->h)) {
        return false;
    }

    roi_x = roi->x;
    roi_y = roi->y;
    roi_w = roi->w;
    roi_h = roi->h;

    if (roi_w <= 0.0f || roi_h <= 0.0f) {
        return false;
    }

    if (roi_x < 0.0f) {
        roi_x = 0.0f;
    }
    if (roi_y < 0.0f) {
        roi_y = 0.0f;
    }
    if (roi_x > 1.0f) {
        roi_x = 1.0f;
    }
    if (roi_y > 1.0f) {
        roi_y = 1.0f;
    }

    max_w = 1.0f - roi_x;
    max_h = 1.0f - roi_y;
    if (roi_w > max_w) {
        roi_w = max_w;
    }
    if (roi_h > max_h) {
        roi_h = max_h;
    }
    if (roi_w <= 0.0f || roi_h <= 0.0f) {
        return false;
    }

    left = (double)roi_x * (double)source_width;
    top = (double)roi_y * (double)source_height;
    right = (double)(roi_x + roi_w) * (double)source_width;
    bottom = (double)(roi_y + roi_h) * (double)source_height;

    x = (int64_t)floor(left);
    y = (int64_t)floor(top);
    right_px = (int64_t)ceil(right);
    bottom_px = (int64_t)ceil(bottom);

    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x > (int64_t)source_width) {
        x = (int64_t)source_width;
    }
    if (y > (int64_t)source_height) {
        y = (int64_t)source_height;
    }
    if (right_px < x) {
        right_px = x;
    }
    if (bottom_px < y) {
        bottom_px = y;
    }
    if (right_px > (int64_t)source_width) {
        right_px = (int64_t)source_width;
    }
    if (bottom_px > (int64_t)source_height) {
        bottom_px = (int64_t)source_height;
    }
    if (right_px <= x && x > 0) {
        x -= 1;
    }
    if (bottom_px <= y && y > 0) {
        y -= 1;
    }
    if (right_px <= x) {
        right_px = x + 1;
    }
    if (bottom_px <= y) {
        bottom_px = y + 1;
    }
    if (right_px > (int64_t)source_width) {
        right_px = (int64_t)source_width;
        x = right_px - 1;
    }
    if (bottom_px > (int64_t)source_height) {
        bottom_px = (int64_t)source_height;
        y = bottom_px - 1;
    }
    if (x < 0 || y < 0 || right_px <= x || bottom_px <= y) {
        return false;
    }

    out_subregion->x = (uint32_t)x;
    out_subregion->y = (uint32_t)y;
    out_subregion->cx = (uint32_t)(right_px - x);
    out_subregion->cy = (uint32_t)(bottom_px - y);
    return out_subregion->cx > 0 && out_subregion->cy > 0;
}

static void roi_filter_render_fallback(struct roi_filter_source *filter,
                                       const char *reason,
                                       int log_level)
{
    if (filter == NULL || filter->context == NULL) {
        return;
    }

    filter->render_fallback_count++;
    if (filter->debug_log || roi_filter_should_log_count(filter->render_fallback_count)) {
        blog(log_level,
             "[ClutchReframe] ROI render fallback (%s, count=%llu).",
             reason != NULL ? reason : "unknown",
             (unsigned long long)filter->render_fallback_count);
    }

    obs_source_skip_video_filter(filter->context);
}

static bool roi_filter_try_capture_target_texture(struct roi_filter_source *filter,
                                                  obs_source_t *target,
                                                  uint32_t target_width,
                                                  uint32_t target_height,
                                                  gs_texture_t **out_texture)
{
    struct vec4 clear_color = {0};

    if (filter == NULL || target == NULL || out_texture == NULL) {
        return false;
    }

    *out_texture = NULL;

    if (target_width == 0 || target_height == 0) {
        return false;
    }

    if (filter->render_texrender != NULL &&
        gs_texrender_get_format(filter->render_texrender) != GS_RGBA) {
        gs_texrender_destroy(filter->render_texrender);
        filter->render_texrender = NULL;
    }

    if (filter->render_texrender == NULL) {
        filter->render_texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        if (filter->render_texrender == NULL) {
            return false;
        }
    }

    gs_texrender_reset(filter->render_texrender);

    if (!gs_texrender_begin(filter->render_texrender, target_width, target_height)) {
        return false;
    }

    gs_blend_state_push();
    gs_blend_function_separate(GS_BLEND_SRCALPHA,
                               GS_BLEND_INVSRCALPHA,
                               GS_BLEND_ONE,
                               GS_BLEND_INVSRCALPHA);
    gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
    gs_ortho(0.0f,
             (float)target_width,
             0.0f,
             (float)target_height,
             -100.0f,
             100.0f);
    obs_source_video_render(target);
    gs_blend_state_pop();

    gs_texrender_end(filter->render_texrender);

    *out_texture = gs_texrender_get_texture(filter->render_texrender);
    return *out_texture != NULL;
}

static bool roi_filter_try_draw_subregion(gs_effect_t *effect,
                                          gs_texture_t *texture,
                                          const roi_filter_subregion_px_t *subregion,
                                          uint32_t output_width,
                                          uint32_t output_height,
                                          bool preserve_aspect)
{
    gs_effect_t *draw_effect = effect;
    gs_eparam_t *image_param = NULL;
    float draw_scale_x = 0.0f;
    float draw_scale_y = 0.0f;
    float draw_offset_x = 0.0f;
    float draw_offset_y = 0.0f;
    bool applied = false;

    if (texture == NULL || subregion == NULL || output_width == 0 || output_height == 0 ||
        subregion->cx == 0 || subregion->cy == 0) {
        return false;
    }

    if (draw_effect == NULL) {
        draw_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    }
    if (draw_effect == NULL) {
        return false;
    }

    image_param = gs_effect_get_param_by_name(draw_effect, "image");
    if (image_param == NULL) {
        return false;
    }

    if (gs_get_linear_srgb()) {
        gs_effect_set_texture_srgb(image_param, texture);
    } else {
        gs_effect_set_texture(image_param, texture);
    }

    draw_scale_x = (float)output_width / (float)subregion->cx;
    draw_scale_y = (float)output_height / (float)subregion->cy;
    if (preserve_aspect) {
        struct vec4 clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
        const float uniform_scale =
            draw_scale_x < draw_scale_y ? draw_scale_x : draw_scale_y;
        const float draw_width = (float)subregion->cx * uniform_scale;
        const float draw_height = (float)subregion->cy * uniform_scale;
        draw_scale_x = uniform_scale;
        draw_scale_y = uniform_scale;
        draw_offset_x = ((float)output_width - draw_width) * 0.5f;
        draw_offset_y = ((float)output_height - draw_height) * 0.5f;
        if (draw_offset_x < 0.0f) {
            draw_offset_x = 0.0f;
        }
        if (draw_offset_y < 0.0f) {
            draw_offset_y = 0.0f;
        }

        /*
         * Letterbox/pillarbox bars must be deterministic black, not
         * transparent backfill from downstream scene composition.
         */
        gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
    }

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    gs_matrix_push();
    gs_matrix_translate3f(draw_offset_x, draw_offset_y, 0.0f);
    gs_matrix_scale3f(draw_scale_x,
                      draw_scale_y,
                      1.0f);

    if (effect != NULL) {
        gs_draw_sprite_subregion(texture,
                                 0,
                                 subregion->x,
                                 subregion->y,
                                 subregion->cx,
                                 subregion->cy);
        applied = true;
    } else {
        while (gs_effect_loop(draw_effect, "Draw")) {
            gs_draw_sprite_subregion(texture,
                                     0,
                                     subregion->x,
                                     subregion->y,
                                     subregion->cx,
                                     subregion->cy);
            applied = true;
        }
    }

    gs_matrix_pop();
    gs_blend_state_pop();

    return applied;
}

static bool roi_filter_metrics_changed(const roi_ipc_client_stats_t *lhs,
                                       const roi_ipc_client_stats_t *rhs)
{
    if (lhs == NULL || rhs == NULL) {
        return true;
    }

    return lhs->connected != rhs->connected ||
           lhs->has_stream_state != rhs->has_stream_state ||
           lhs->last_stream_id != rhs->last_stream_id ||
           lhs->last_epoch != rhs->last_epoch ||
           lhs->last_seq != rhs->last_seq ||
           lhs->latest_generation != rhs->latest_generation ||
           lhs->queued_frame_count != rhs->queued_frame_count ||
           lhs->queue_drop_count != rhs->queue_drop_count ||
           lhs->connect_count != rhs->connect_count ||
           lhs->reconnect_count != rhs->reconnect_count ||
           lhs->message_accept_count != rhs->message_accept_count ||
           lhs->decode_error_count != rhs->decode_error_count ||
           lhs->version_mismatch_count != rhs->version_mismatch_count ||
           lhs->out_of_order_drop_count != rhs->out_of_order_drop_count ||
           lhs->stream_reset_count != rhs->stream_reset_count ||
           lhs->reserved_header_warn_count != rhs->reserved_header_warn_count;
}

static bool roi_filter_interpolator_metrics_changed(const roi_interpolator_stats_t *lhs,
                                                    const roi_interpolator_stats_t *rhs)
{
    if (lhs == NULL || rhs == NULL) {
        return true;
    }

    return lhs->has_stream_state != rhs->has_stream_state ||
           lhs->warmup_active != rhs->warmup_active ||
           lhs->stream_id != rhs->stream_id ||
           lhs->epoch != rhs->epoch ||
           lhs->qpc_frequency != rhs->qpc_frequency ||
           lhs->frame_stream_mode != rhs->frame_stream_mode ||
           lhs->presend_v2_mode != rhs->presend_v2_mode ||
           lhs->anchor_buffer_count != rhs->anchor_buffer_count ||
           lhs->anchor_buffer_peak != rhs->anchor_buffer_peak ||
           lhs->anchor_buffer_drop_count != rhs->anchor_buffer_drop_count ||
           lhs->accepted_valid_frame_count != rhs->accepted_valid_frame_count ||
           lhs->dropped_invalid_frame_count != rhs->dropped_invalid_frame_count ||
           lhs->dropped_invalid_geometry_count != rhs->dropped_invalid_geometry_count ||
           lhs->valid_zero_count != rhs->valid_zero_count ||
           lhs->stream_reset_count != rhs->stream_reset_count ||
           lhs->warmup_enter_count != rhs->warmup_enter_count ||
           lhs->warmup_exit_count != rhs->warmup_exit_count ||
           lhs->sample_call_count != rhs->sample_call_count ||
           lhs->sample_output_count != rhs->sample_output_count ||
           lhs->sample_no_output_count != rhs->sample_no_output_count ||
           lhs->timeout_freeze_count != rhs->timeout_freeze_count ||
           lhs->timeout_freeze_sample_count != rhs->timeout_freeze_sample_count ||
           lhs->interpolated_count != rhs->interpolated_count ||
           lhs->hold_prev_count != rhs->hold_prev_count ||
           lhs->hold_last_count != rhs->hold_last_count ||
           lhs->buffered_span_count != rhs->buffered_span_count ||
           lhs->buffered_span_missing_left_count != rhs->buffered_span_missing_left_count ||
           lhs->buffered_last_next_lead_ticks != rhs->buffered_last_next_lead_ticks;
}

static const char *roi_filter_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "ClutchReframe ROI Filter";
}

static void roi_filter_update(void *data, obs_data_t *settings)
{
    struct roi_filter_source *filter = data;
    const uint32_t prev_presend_v2_consumer_mode =
        filter != NULL ? filter->presend_v2_consumer_mode : ROI_FILTER_PRESEND_V2_CONSUMER_MODE_AUTO;
    if (filter == NULL) {
        return;
    }

    if (settings != NULL) {
        filter->enabled = obs_data_get_bool(settings, "enabled");
        if (obs_data_has_user_value(settings, "preserve_aspect")) {
            filter->preserve_aspect = obs_data_get_bool(settings, "preserve_aspect");
        } else {
            filter->preserve_aspect = true;
        }
        roi_filter_copy_pipe_name(filter->pipe_name,
                                  sizeof(filter->pipe_name),
                                  obs_data_get_string(settings, "pipe_name"));
        filter->connect_retry_ms =
            roi_filter_clamp_connect_retry_ms(obs_data_get_int(settings, "connect_retry_ms"));
        filter->freeze_timeout_ms =
            roi_filter_clamp_freeze_timeout_ms(obs_data_get_int(settings, "freeze_timeout_ms"));
        if (obs_data_has_user_value(settings, "ipc_queue_cap")) {
            filter->ipc_queue_cap =
                roi_filter_clamp_ipc_queue_cap(obs_data_get_int(settings, "ipc_queue_cap"));
        } else {
            filter->ipc_queue_cap = ROI_FILTER_IPC_QUEUE_CAP_DEFAULT;
        }
        if (obs_data_has_user_value(settings, "stale_drop_ms")) {
            filter->stale_drop_ms =
                roi_filter_clamp_stale_drop_ms(obs_data_get_int(settings, "stale_drop_ms"));
        } else {
            filter->stale_drop_ms = ROI_FILTER_STALE_DROP_DEFAULT_MS;
        }
        if (obs_data_has_user_value(settings, "presend_v2_consumer_mode")) {
            filter->presend_v2_consumer_mode =
                roi_filter_clamp_presend_v2_consumer_mode(
                    obs_data_get_int(settings, "presend_v2_consumer_mode"));
        } else {
            filter->presend_v2_consumer_mode = ROI_FILTER_PRESEND_V2_CONSUMER_MODE_AUTO;
        }
        filter->roi_apply_offset_ms =
            roi_filter_clamp_roi_apply_offset_ms(obs_data_get_int(settings, "roi_apply_offset_ms"));
        filter->debug_log = obs_data_get_bool(settings, "debug_log");
    }

    if (filter->interpolator != NULL) {
        roi_interpolator_set_debug_log(filter->interpolator, filter->debug_log);
        roi_interpolator_set_freeze_timeout_ms(filter->interpolator, filter->freeze_timeout_ms);
        if (!filter->enabled) {
            roi_interpolator_reset(filter->interpolator, true);
            filter->has_last_sample = false;
        }
    }

    if (filter->presend_v2_consumer_mode != prev_presend_v2_consumer_mode) {
        blog(LOG_INFO,
             "[ClutchReframe] Presend V2 (Consumer) changed: %s(%lu) -> %s(%lu).",
             roi_filter_presend_v2_consumer_mode_name(prev_presend_v2_consumer_mode),
             (unsigned long)prev_presend_v2_consumer_mode,
             roi_filter_presend_v2_consumer_mode_name(filter->presend_v2_consumer_mode),
             (unsigned long)filter->presend_v2_consumer_mode);
        if (filter->interpolator != NULL) {
            roi_interpolator_reset(filter->interpolator, true);
            filter->has_last_sample = false;
        }
    }

    if (filter->ipc_client != NULL) {
        roi_ipc_client_set_pipe_name(filter->ipc_client, filter->pipe_name);
        roi_ipc_client_set_connect_retry_ms(filter->ipc_client, filter->connect_retry_ms);
        roi_ipc_client_set_queue_cap(filter->ipc_client, filter->ipc_queue_cap);
        roi_ipc_client_set_debug_log(filter->ipc_client, filter->debug_log);

        if (filter->enabled) {
            if (!roi_ipc_client_start(filter->ipc_client)) {
                blog(LOG_WARNING, "[ClutchReframe] IPC start failed.");
            }
        } else {
            roi_ipc_client_stop(filter->ipc_client);
        }
    }
}

static void *roi_filter_create(obs_data_t *settings, obs_source_t *source)
{
    struct roi_filter_source *filter = bzalloc(sizeof(*filter));
    filter->context = source;
    filter->enabled = true;
    filter->preserve_aspect = true;
    filter->debug_log = false;
    filter->connect_retry_ms = ROI_FILTER_CONNECT_RETRY_DEFAULT_MS;
    filter->freeze_timeout_ms = ROI_INTERPOLATOR_FREEZE_TIMEOUT_DEFAULT_MS;
    filter->ipc_queue_cap = ROI_FILTER_IPC_QUEUE_CAP_DEFAULT;
    filter->stale_drop_ms = ROI_FILTER_STALE_DROP_DEFAULT_MS;
    filter->presend_v2_consumer_mode = ROI_FILTER_PRESEND_V2_CONSUMER_MODE_AUTO;
    filter->roi_apply_offset_ms = 0;
    roi_filter_copy_pipe_name(filter->pipe_name,
                              sizeof(filter->pipe_name),
                              ROI_FILTER_DEFAULT_PIPE_NAME);
    filter->ipc_client = roi_ipc_client_create();
    if (filter->ipc_client == NULL) {
        blog(LOG_WARNING, "[ClutchReframe] IPC client create failed; filter runs in bypass mode.");
    }
    filter->interpolator = roi_interpolator_create();
    if (filter->interpolator == NULL) {
        blog(LOG_WARNING,
             "[ClutchReframe] Interpolator create failed; filter keeps queue consume only.");
    }

    roi_filter_update(filter, settings);
    blog(LOG_INFO,
         "[ClutchReframe] ROI filter created (pipe=%s retry_ms=%lu freeze_timeout_ms=%lu ipc_queue_cap=%lu stale_drop_ms=%lu offset_ms=%ld preserve_aspect=%u presend_v2_consumer_mode=%s(%lu)).",
         filter->pipe_name,
         (unsigned long)filter->connect_retry_ms,
         (unsigned long)filter->freeze_timeout_ms,
         (unsigned long)filter->ipc_queue_cap,
         (unsigned long)filter->stale_drop_ms,
         (long)filter->roi_apply_offset_ms,
         (unsigned int)(filter->preserve_aspect ? 1u : 0u),
         roi_filter_presend_v2_consumer_mode_name(filter->presend_v2_consumer_mode),
         (unsigned long)filter->presend_v2_consumer_mode);
    return filter;
}

static void roi_filter_destroy(void *data)
{
    struct roi_filter_source *filter = data;
    if (filter == NULL) {
        return;
    }

    if (filter->ipc_client != NULL) {
        roi_ipc_client_stop(filter->ipc_client);
        roi_ipc_client_destroy(filter->ipc_client);
        filter->ipc_client = NULL;
    }
    if (filter->interpolator != NULL) {
        roi_interpolator_destroy(filter->interpolator);
        filter->interpolator = NULL;
    }
    if (filter->render_texrender != NULL) {
        obs_enter_graphics();
        gs_texrender_destroy(filter->render_texrender);
        obs_leave_graphics();
        filter->render_texrender = NULL;
    }

    blog(LOG_INFO, "[ClutchReframe] ROI filter destroyed.");
    bfree(filter);
}

static void roi_filter_video_tick(void *data, float seconds)
{
    struct roi_filter_source *filter = data;
    roi_frame_msg_v1_t queued_message;
    roi_frame_msg_v1_t frame_for_interpolator;
    uint64_t queued_generation = 0;
    roi_ipc_client_stats_t ipc_stats = {0};
    roi_interpolator_stats_t interpolator_stats = {0};
    int64_t now_qpc = 0;
    bool has_now_qpc = false;
    bool has_ipc_stats = false;
    bool has_interpolator_stats = false;
    uint64_t delta_accept = 0;
    uint64_t delta_interpolated = 0;
    uint64_t delta_hold_prev = 0;
    uint64_t delta_hold_last = 0;
    uint64_t delta_timeout_freeze_sample = 0;
    uint64_t delta_render_apply = 0;
    uint64_t delta_consumed_valid_roi = 0;
    uint64_t delta_consumed_roi_changed = 0;
    uint64_t delta_consumed_roi_unchanged = 0;
    bool has_cause_delta = false;
    bool inferred_upstream_no_new_observation = false;
    bool inferred_roi_unchanged = false;
    bool inferred_apply_suppressed = false;
    bool inferred_timeout_freeze = false;
    roi_filter_root_cause_t root_cause = ROI_FILTER_ROOT_CAUSE_HEALTHY;

    if (filter == NULL) {
        return;
    }

    has_now_qpc = roi_filter_try_query_now_qpc(&now_qpc);

    if (filter->tqpc_effective_time_offset_conflict_active &&
        filter->roi_apply_offset_ms == 0) {
        filter->tqpc_effective_time_offset_conflict_active = false;
        blog(LOG_INFO,
             "[ClutchReframe] roi_apply_offset_ms cleared; resume ROI stream (FLAG_TQPC_EFFECTIVE_TIME compatible).");
    }

    /*
     * P3-1-3 keeps render thread zero-blocking: no pipe read or blocking waits,
     * only fast queue-drain consumption and state observation.
     */
    if (filter->ipc_client != NULL) {
        const bool connected = roi_ipc_client_is_connected(filter->ipc_client);
        if (!filter->has_last_connected_state || connected != filter->last_connected_state) {
            filter->has_last_connected_state = true;
            filter->last_connected_state = connected;
            blog(LOG_INFO,
                 "[ClutchReframe] IPC state changed: %s",
                 connected ? "connected" : "disconnected");
        }

        while (roi_ipc_client_try_consume_next(filter->ipc_client,
                                               &queued_message,
                                               &queued_generation)) {
            bool should_drop_stale = false;
            bool should_log_stale_drop = false;
            unsigned long long stale_drop_count = 0;
            unsigned long long stale_drop_valid_count = 0;
            int64_t stale_age_ticks = 0;
            int64_t stale_age_ms = 0;
            int64_t stale_threshold_ticks = 0;

            if (filter->has_last_consumed_frame &&
                queued_generation == filter->last_consumed_generation) {
                continue;
            }

            frame_for_interpolator = queued_message;
            if (filter->presend_v2_consumer_mode ==
                ROI_FILTER_PRESEND_V2_CONSUMER_MODE_FORCE_LEGACY) {
                frame_for_interpolator.flags &= ~ROI_FRAME_MSG_V1_FLAG_PRESEND_V2;
            } else if (filter->presend_v2_consumer_mode ==
                       ROI_FILTER_PRESEND_V2_CONSUMER_MODE_FORCE_BUFFERED) {
                frame_for_interpolator.flags |= ROI_FRAME_MSG_V1_FLAG_PRESEND_V2;
            }
            if ((queued_message.flags & ROI_FRAME_MSG_V1_FLAG_TQPC_EFFECTIVE_TIME) != 0u &&
                filter->roi_apply_offset_ms != 0) {
                filter->tqpc_effective_time_offset_conflict_count++;
                if (!filter->tqpc_effective_time_offset_conflict_active) {
                    filter->tqpc_effective_time_offset_conflict_active = true;
                    if (filter->interpolator != NULL) {
                        roi_interpolator_reset(filter->interpolator, true);
                    }
                }
                if (roi_filter_should_log_count(
                        filter->tqpc_effective_time_offset_conflict_count)) {
                    blog(LOG_WARNING,
                         "[ClutchReframe] roi_apply_offset_ms conflict; upstream has FLAG_TQPC_EFFECTIVE_TIME, but local offset_ms=%ld. Freeze ROI updates (count=%llu stream=%u epoch=%u seq=%llu).",
                         (long)filter->roi_apply_offset_ms,
                         (unsigned long long)filter->tqpc_effective_time_offset_conflict_count,
                         (unsigned int)queued_message.stream_id,
                         (unsigned int)queued_message.epoch,
                         (unsigned long long)queued_message.seq);
                }

                frame_for_interpolator.valid = 0u;
                frame_for_interpolator.x = 0.0f;
                frame_for_interpolator.y = 0.0f;
                frame_for_interpolator.w = 0.0f;
                frame_for_interpolator.h = 0.0f;
            } else if (filter->roi_apply_offset_ms != 0 &&
                       !roi_filter_try_apply_qpc_offset_ticks(&frame_for_interpolator,
                                                             filter->roi_apply_offset_ms)) {
                filter->qpc_offset_apply_error_count++;
                if (filter->debug_log ||
                    roi_filter_should_log_count(filter->qpc_offset_apply_error_count)) {
                    blog(filter->debug_log ? LOG_DEBUG : LOG_WARNING,
                         "[ClutchReframe] roi_apply_offset fallback +1 (count=%llu offset_ms=%ld qpc=%llu tQpc=%lld).",
                         (unsigned long long)filter->qpc_offset_apply_error_count,
                         (long)filter->roi_apply_offset_ms,
                         (unsigned long long)queued_message.qpc_frequency,
                         (long long)queued_message.t_qpc);
                }
            }

            filter->has_last_consumed_frame = true;
            filter->last_consumed_generation = queued_generation;
            filter->last_consumed_frame = queued_message;

            if (has_now_qpc &&
                frame_for_interpolator.qpc_frequency > 0u &&
                frame_for_interpolator.t_qpc > 0 &&
                frame_for_interpolator.valid != 0u) {
                const int64_t arrival_lead_ticks = frame_for_interpolator.t_qpc - now_qpc;
                filter->arrival_lead_sample_count++;
                if (arrival_lead_ticks >= 0) {
                    filter->arrival_lead_positive_count++;
                } else {
                    filter->arrival_lead_negative_count++;
                }
                if (filter->arrival_lead_sample_count == 1u) {
                    filter->arrival_lead_min_ticks = arrival_lead_ticks;
                    filter->arrival_lead_max_ticks = arrival_lead_ticks;
                } else {
                    if (arrival_lead_ticks < filter->arrival_lead_min_ticks) {
                        filter->arrival_lead_min_ticks = arrival_lead_ticks;
                    }
                    if (arrival_lead_ticks > filter->arrival_lead_max_ticks) {
                        filter->arrival_lead_max_ticks = arrival_lead_ticks;
                    }
                }
            }

            if (filter->stale_drop_ms > 0u && has_now_qpc &&
                frame_for_interpolator.valid != 0u &&
                frame_for_interpolator.t_qpc > 0 &&
                frame_for_interpolator.t_qpc <= now_qpc &&
                roi_filter_try_compute_stale_threshold_ticks(filter->stale_drop_ms,
                                                             frame_for_interpolator.qpc_frequency,
                                                             &stale_threshold_ticks)) {
                stale_age_ticks = now_qpc - frame_for_interpolator.t_qpc;
                if (stale_age_ticks > stale_threshold_ticks) {
                    should_drop_stale = true;
                    stale_age_ms =
                        roi_filter_ticks_to_ms_clamped(stale_age_ticks,
                                                       frame_for_interpolator.qpc_frequency);
                    filter->stale_drop_count++;
                    if (frame_for_interpolator.valid != 0u) {
                        filter->stale_drop_valid_count++;
                    }
                    if (stale_age_ms > filter->stale_drop_max_age_ms) {
                        filter->stale_drop_max_age_ms = stale_age_ms;
                    }
                    stale_drop_count = (unsigned long long)filter->stale_drop_count;
                    stale_drop_valid_count = (unsigned long long)filter->stale_drop_valid_count;
                    should_log_stale_drop = roi_filter_should_log_count(filter->stale_drop_count);
                }
            }

            if (should_drop_stale) {
                if (filter->debug_log || should_log_stale_drop) {
                    blog(filter->debug_log ? LOG_DEBUG : LOG_INFO,
                         "[ClutchReframe] IPC stale drop +1 (count=%llu validCount=%llu ageMs=%lld thresholdMs=%lu stream=%u epoch=%u seq=%llu).",
                         stale_drop_count,
                         stale_drop_valid_count,
                         (long long)stale_age_ms,
                         (unsigned long)filter->stale_drop_ms,
                         (unsigned int)frame_for_interpolator.stream_id,
                         (unsigned int)frame_for_interpolator.epoch,
                         (unsigned long long)frame_for_interpolator.seq);
                }
                continue;
            }

            {
                roi_interpolator_roi_t consumed_roi;
                filter->consumed_frame_count++;

                if (frame_for_interpolator.valid != 0u &&
                    isfinite((double)frame_for_interpolator.x) &&
                    isfinite((double)frame_for_interpolator.y) &&
                    isfinite((double)frame_for_interpolator.w) &&
                    isfinite((double)frame_for_interpolator.h) &&
                    frame_for_interpolator.w > 0.0f &&
                    frame_for_interpolator.h > 0.0f) {
                    consumed_roi.x = frame_for_interpolator.x;
                    consumed_roi.y = frame_for_interpolator.y;
                    consumed_roi.w = frame_for_interpolator.w;
                    consumed_roi.h = frame_for_interpolator.h;
                    filter->consumed_valid_roi_count++;

                    if (!filter->has_last_consumed_valid_roi ||
                        !roi_filter_roi_nearly_equal(&consumed_roi,
                                                     &filter->last_consumed_valid_roi)) {
                        filter->consumed_roi_changed_count++;
                        filter->last_consumed_valid_roi = consumed_roi;
                        filter->has_last_consumed_valid_roi = true;
                    } else {
                        filter->consumed_roi_unchanged_count++;
                    }
                }
            }

            if (filter->interpolator != NULL) {
                if (!roi_interpolator_push_frame(filter->interpolator,
                                                 &frame_for_interpolator,
                                                 has_now_qpc ? now_qpc : 0) &&
                    filter->debug_log) {
                    blog(LOG_DEBUG,
                         "[ClutchReframe] Interpolator rejected frame gen=%llu stream=%u epoch=%u seq=%llu valid=%u.",
                         (unsigned long long)queued_generation,
                         (unsigned int)queued_message.stream_id,
                         (unsigned int)queued_message.epoch,
                         (unsigned long long)queued_message.seq,
                         (unsigned int)queued_message.valid);
                }
            }

            if (filter->debug_log) {
                blog(LOG_DEBUG,
                     "[ClutchReframe] queue consume gen=%llu stream=%u epoch=%u seq=%llu valid=%u",
                     (unsigned long long)queued_generation,
                     (unsigned int)queued_message.stream_id,
                     (unsigned int)queued_message.epoch,
                     (unsigned long long)queued_message.seq,
                     (unsigned int)queued_message.valid);
            }
        }

        roi_ipc_client_get_stats(filter->ipc_client, &ipc_stats);
        has_ipc_stats = true;
    }

    if (filter->interpolator != NULL) {
        roi_interpolator_sample_t sampled;

        if (has_now_qpc && roi_interpolator_sample(filter->interpolator, now_qpc, &sampled)) {
            const bool state_changed = !filter->has_last_sample ||
                                       sampled.mode != filter->last_sample.mode ||
                                       sampled.warmup_active != filter->last_sample.warmup_active ||
                                       sampled.timeout_freeze != filter->last_sample.timeout_freeze;
            filter->last_sample = sampled;
            filter->has_last_sample = true;

            if (state_changed &&
                (filter->debug_log || sampled.warmup_active || sampled.timeout_freeze)) {
                blog(LOG_INFO,
                     "[ClutchReframe] Interpolator sample mode=%s reason=%s warmup=%u timeoutFreeze=%u nowQpc=%lld "
                     "roi=(x=%.5f y=%.5f w=%.5f h=%.5f).",
                     roi_filter_sample_mode_to_string(sampled.mode),
                     roi_filter_sample_reason_to_string(sampled.reason),
                     (unsigned int)sampled.warmup_active,
                     (unsigned int)sampled.timeout_freeze,
                     (long long)now_qpc,
                     sampled.roi.x,
                     sampled.roi.y,
                     sampled.roi.w,
                     sampled.roi.h);
            }
        }

        roi_interpolator_get_stats(filter->interpolator, &interpolator_stats);
        has_interpolator_stats = true;
    }

    filter->metrics_log_elapsed_s += seconds;
    if (filter->metrics_log_elapsed_s >= 5.0f ||
        (has_ipc_stats && !filter->has_last_metrics_snapshot) ||
        (has_interpolator_stats && !filter->has_last_interpolator_metrics_snapshot)) {
        const bool ipc_changed =
            has_ipc_stats &&
            (!filter->has_last_metrics_snapshot ||
             roi_filter_metrics_changed(&ipc_stats, &filter->last_metrics_snapshot));
        const bool interpolator_changed =
            has_interpolator_stats &&
            (!filter->has_last_interpolator_metrics_snapshot ||
             roi_filter_interpolator_metrics_changed(&interpolator_stats,
                                                     &filter->last_interpolator_metrics_snapshot));

        if (filter->has_last_cause_snapshot) {
            has_cause_delta = true;
            delta_accept = has_ipc_stats
                               ? roi_filter_delta_u64(ipc_stats.message_accept_count,
                                                      filter->last_cause_accept_count)
                               : 0;
            delta_interpolated = has_interpolator_stats
                                     ? roi_filter_delta_u64(interpolator_stats.interpolated_count,
                                                            filter->last_cause_interpolated_count)
                                     : 0;
            delta_hold_prev = has_interpolator_stats
                                  ? roi_filter_delta_u64(interpolator_stats.hold_prev_count,
                                                         filter->last_cause_hold_prev_count)
                                  : 0;
            delta_hold_last = has_interpolator_stats
                                  ? roi_filter_delta_u64(interpolator_stats.hold_last_count,
                                                         filter->last_cause_hold_last_count)
                                  : 0;
            delta_timeout_freeze_sample = has_interpolator_stats
                                              ? roi_filter_delta_u64(interpolator_stats.timeout_freeze_sample_count,
                                                                     filter->last_cause_timeout_freeze_sample_count)
                                              : 0;
            delta_render_apply = roi_filter_delta_u64(filter->render_apply_count,
                                                      filter->last_cause_render_apply_count);
            delta_consumed_valid_roi = roi_filter_delta_u64(filter->consumed_valid_roi_count,
                                                            filter->last_cause_consumed_valid_roi_count);
            delta_consumed_roi_changed = roi_filter_delta_u64(filter->consumed_roi_changed_count,
                                                              filter->last_cause_consumed_roi_changed_count);
            delta_consumed_roi_unchanged = roi_filter_delta_u64(filter->consumed_roi_unchanged_count,
                                                                filter->last_cause_consumed_roi_unchanged_count);

            inferred_upstream_no_new_observation = has_ipc_stats && (delta_accept == 0u);
            inferred_timeout_freeze = has_interpolator_stats && (delta_timeout_freeze_sample > 0u);
            inferred_roi_unchanged = (delta_consumed_valid_roi > 0u) &&
                                     (delta_consumed_roi_changed == 0u) &&
                                     (delta_consumed_roi_unchanged > 0u);
            inferred_apply_suppressed = !inferred_timeout_freeze &&
                                        (delta_accept > 0u) &&
                                        (delta_consumed_valid_roi > 0u) &&
                                        (delta_consumed_roi_changed == 0u) &&
                                        (delta_interpolated == 0u) &&
                                        ((delta_hold_prev > 0u) || (delta_hold_last > 0u) || (delta_render_apply > 0u));

            if (inferred_upstream_no_new_observation) {
                filter->cause_upstream_no_new_observation_count++;
            }
            if (inferred_roi_unchanged) {
                filter->cause_roi_unchanged_count++;
            }
            if (inferred_apply_suppressed) {
                filter->cause_apply_suppressed_count++;
            }
            if (inferred_timeout_freeze) {
                filter->cause_timeout_freeze_count++;
            }

            if (inferred_timeout_freeze) {
                root_cause = ROI_FILTER_ROOT_CAUSE_TIMEOUT_FREEZE;
            } else if (inferred_upstream_no_new_observation) {
                root_cause = ROI_FILTER_ROOT_CAUSE_UPSTREAM_NO_NEW_OBSERVATION;
            } else if (inferred_apply_suppressed) {
                root_cause = ROI_FILTER_ROOT_CAUSE_APPLY_SUPPRESSED;
            } else if (inferred_roi_unchanged) {
                root_cause = ROI_FILTER_ROOT_CAUSE_ROI_UNCHANGED;
            } else {
                root_cause = ROI_FILTER_ROOT_CAUSE_HEALTHY;
            }
        }

        if (filter->debug_log ||
            ipc_changed ||
            interpolator_changed ||
            inferred_upstream_no_new_observation ||
            inferred_timeout_freeze ||
            inferred_apply_suppressed) {
            if (has_ipc_stats) {
                blog(LOG_INFO,
                     "[ClutchReframe] IPC stats: connected=%u accept=%llu decodeErr=%llu "
                     "verMis=%llu oooDrop=%llu streamReset=%llu reconnect=%llu gen=%llu "
                     "queued=%llu queueDrop=%llu queueCap=%u staleDrop=%llu staleDropValid=%llu staleDropMaxAgeMs=%lld staleDropMs=%lu stream=%u epoch=%u seq=%llu",
                     (unsigned int)ipc_stats.connected,
                     (unsigned long long)ipc_stats.message_accept_count,
                     (unsigned long long)ipc_stats.decode_error_count,
                     (unsigned long long)ipc_stats.version_mismatch_count,
                     (unsigned long long)ipc_stats.out_of_order_drop_count,
                     (unsigned long long)ipc_stats.stream_reset_count,
                     (unsigned long long)ipc_stats.reconnect_count,
                     (unsigned long long)ipc_stats.latest_generation,
                     (unsigned long long)ipc_stats.queued_frame_count,
                     (unsigned long long)ipc_stats.queue_drop_count,
                     (unsigned int)ipc_stats.queue_capacity,
                     (unsigned long long)filter->stale_drop_count,
                     (unsigned long long)filter->stale_drop_valid_count,
                     (long long)filter->stale_drop_max_age_ms,
                     (unsigned long)filter->stale_drop_ms,
                     (unsigned int)ipc_stats.last_stream_id,
                     (unsigned int)ipc_stats.last_epoch,
                     (unsigned long long)ipc_stats.last_seq);
            }

            if (has_interpolator_stats) {
                const uint64_t qpc_frequency =
                    interpolator_stats.qpc_frequency > 0u
                        ? interpolator_stats.qpc_frequency
                        : (filter->has_last_consumed_frame
                               ? filter->last_consumed_frame.qpc_frequency
                               : 0u);
                const int64_t next_lead_ms =
                    roi_filter_ticks_to_ms_clamped(interpolator_stats.buffered_last_next_lead_ticks,
                                                   qpc_frequency);
                const int64_t arrival_lead_min_ms =
                    roi_filter_ticks_to_ms_signed_clamped(filter->arrival_lead_min_ticks,
                                                          qpc_frequency);
                const int64_t arrival_lead_max_ms =
                    roi_filter_ticks_to_ms_signed_clamped(filter->arrival_lead_max_ticks,
                                                          qpc_frequency);
                blog(LOG_INFO,
                     "[ClutchReframe] Interpolator stats: streamState=%u stream=%u epoch=%u "
                     "warmup=%u qpcFreq=%llu nowQpc=%lld mode{frameStream=%u presendV2=%u} "
                     "anchorBuf{n=%u peak=%u drop=%llu span=%llu missLeft=%llu nextLeadMs=%lld} "
                     "arrivalLead{samples=%llu pos=%llu neg=%llu minMs=%lld maxMs=%lld} "
                     "accept=%llu valid0=%llu dropSeq=%llu dropGeom=%llu "
                     "streamReset=%llu warmupEnter=%llu warmupExit=%llu sampleCall=%llu "
                     "sampleOut=%llu sampleMiss=%llu timeoutFreezeEnter=%llu timeoutFreezeSample=%llu "
                     "interp=%llu holdPrev=%llu holdLast=%llu offsetFallback=%llu",
                     (unsigned int)interpolator_stats.has_stream_state,
                     (unsigned int)interpolator_stats.stream_id,
                     (unsigned int)interpolator_stats.epoch,
                     (unsigned int)interpolator_stats.warmup_active,
                     (unsigned long long)qpc_frequency,
                     (long long)(has_now_qpc ? now_qpc : 0),
                     (unsigned int)(interpolator_stats.frame_stream_mode ? 1u : 0u),
                     (unsigned int)(interpolator_stats.presend_v2_mode ? 1u : 0u),
                     (unsigned int)interpolator_stats.anchor_buffer_count,
                     (unsigned int)interpolator_stats.anchor_buffer_peak,
                     (unsigned long long)interpolator_stats.anchor_buffer_drop_count,
                     (unsigned long long)interpolator_stats.buffered_span_count,
                     (unsigned long long)interpolator_stats.buffered_span_missing_left_count,
                     (long long)next_lead_ms,
                     (unsigned long long)filter->arrival_lead_sample_count,
                     (unsigned long long)filter->arrival_lead_positive_count,
                     (unsigned long long)filter->arrival_lead_negative_count,
                     (long long)arrival_lead_min_ms,
                     (long long)arrival_lead_max_ms,
                     (unsigned long long)interpolator_stats.accepted_valid_frame_count,
                     (unsigned long long)interpolator_stats.valid_zero_count,
                     (unsigned long long)interpolator_stats.dropped_invalid_frame_count,
                     (unsigned long long)interpolator_stats.dropped_invalid_geometry_count,
                     (unsigned long long)interpolator_stats.stream_reset_count,
                     (unsigned long long)interpolator_stats.warmup_enter_count,
                     (unsigned long long)interpolator_stats.warmup_exit_count,
                     (unsigned long long)interpolator_stats.sample_call_count,
                     (unsigned long long)interpolator_stats.sample_output_count,
                     (unsigned long long)interpolator_stats.sample_no_output_count,
                     (unsigned long long)interpolator_stats.timeout_freeze_count,
                     (unsigned long long)interpolator_stats.timeout_freeze_sample_count,
                     (unsigned long long)interpolator_stats.interpolated_count,
                     (unsigned long long)interpolator_stats.hold_prev_count,
                     (unsigned long long)interpolator_stats.hold_last_count,
                     (unsigned long long)filter->qpc_offset_apply_error_count);
                blog(LOG_INFO,
                     "[ClutchReframe] Render stats: apply=%llu fallback=%llu invalidRoi=%llu "
                     "texrenderErr=%llu consumeFrame=%llu consumeValid=%llu consumeChanged=%llu consumeUnchanged=%llu",
                     (unsigned long long)filter->render_apply_count,
                     (unsigned long long)filter->render_fallback_count,
                     (unsigned long long)filter->render_invalid_geometry_count,
                     (unsigned long long)filter->render_texrender_error_count,
                     (unsigned long long)filter->consumed_frame_count,
                     (unsigned long long)filter->consumed_valid_roi_count,
                     (unsigned long long)filter->consumed_roi_changed_count,
                     (unsigned long long)filter->consumed_roi_unchanged_count);
            }

            if (has_cause_delta) {
                blog(LOG_INFO,
                     "[ClutchReframe] Cause inference: main=%s "
                     "window{upstreamNoNew=%u roiUnchanged=%u applySuppressed=%u timeoutFreeze=%u} "
                     "delta{accept=%llu consumeValid=%llu consumeChanged=%llu consumeUnchanged=%llu interp=%llu holdPrev=%llu holdLast=%llu timeoutFreezeSample=%llu renderApply=%llu} "
                     "cumulative{upstreamNoNew=%llu roiUnchanged=%llu applySuppressed=%llu timeoutFreeze=%llu}",
                     roi_filter_root_cause_to_string(root_cause),
                     (unsigned int)inferred_upstream_no_new_observation,
                     (unsigned int)inferred_roi_unchanged,
                     (unsigned int)inferred_apply_suppressed,
                     (unsigned int)inferred_timeout_freeze,
                     (unsigned long long)delta_accept,
                     (unsigned long long)delta_consumed_valid_roi,
                     (unsigned long long)delta_consumed_roi_changed,
                     (unsigned long long)delta_consumed_roi_unchanged,
                     (unsigned long long)delta_interpolated,
                     (unsigned long long)delta_hold_prev,
                     (unsigned long long)delta_hold_last,
                     (unsigned long long)delta_timeout_freeze_sample,
                     (unsigned long long)delta_render_apply,
                     (unsigned long long)filter->cause_upstream_no_new_observation_count,
                     (unsigned long long)filter->cause_roi_unchanged_count,
                     (unsigned long long)filter->cause_apply_suppressed_count,
                     (unsigned long long)filter->cause_timeout_freeze_count);
            }
        }

        if (has_ipc_stats) {
            filter->last_metrics_snapshot = ipc_stats;
            filter->has_last_metrics_snapshot = true;
        }
        if (has_interpolator_stats) {
            filter->last_interpolator_metrics_snapshot = interpolator_stats;
            filter->has_last_interpolator_metrics_snapshot = true;
        }
        filter->last_cause_accept_count = has_ipc_stats ? ipc_stats.message_accept_count : 0u;
        filter->last_cause_interpolated_count =
            has_interpolator_stats ? interpolator_stats.interpolated_count : 0u;
        filter->last_cause_hold_prev_count =
            has_interpolator_stats ? interpolator_stats.hold_prev_count : 0u;
        filter->last_cause_hold_last_count =
            has_interpolator_stats ? interpolator_stats.hold_last_count : 0u;
        filter->last_cause_timeout_freeze_sample_count =
            has_interpolator_stats ? interpolator_stats.timeout_freeze_sample_count : 0u;
        filter->last_cause_render_apply_count = filter->render_apply_count;
        filter->last_cause_consumed_valid_roi_count = filter->consumed_valid_roi_count;
        filter->last_cause_consumed_roi_changed_count = filter->consumed_roi_changed_count;
        filter->last_cause_consumed_roi_unchanged_count = filter->consumed_roi_unchanged_count;
        filter->has_last_cause_snapshot = true;
        filter->metrics_log_elapsed_s = 0.0f;
    }
}

static void roi_filter_video_render(void *data, gs_effect_t *effect)
{
    struct roi_filter_source *filter = data;
    roi_filter_subregion_px_t subregion;
    gs_texture_t *target_texture = NULL;
    obs_source_t *target = NULL;
    uint32_t target_width = 0;
    uint32_t target_height = 0;

    if (filter == NULL || filter->context == NULL) {
        return;
    }

    if (!filter->enabled || !filter->has_last_sample || !filter->last_sample.has_roi) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    target = obs_filter_get_target(filter->context);
    if (target == NULL) {
        roi_filter_render_fallback(filter, "missing-target", LOG_WARNING);
        return;
    }

    target_width = obs_source_get_base_width(target);
    target_height = obs_source_get_base_height(target);
    if (target_width == 0 || target_height == 0) {
        roi_filter_render_fallback(filter, "empty-target-dimension", LOG_WARNING);
        return;
    }

    if (!roi_filter_try_compute_subregion_px(&filter->last_sample.roi,
                                             target_width,
                                             target_height,
                                             &subregion)) {
        filter->render_invalid_geometry_count++;
        roi_filter_render_fallback(filter, "invalid-roi-subregion", LOG_WARNING);
        return;
    }

    if (!roi_filter_try_capture_target_texture(filter,
                                               target,
                                               target_width,
                                               target_height,
                                               &target_texture) ||
        target_texture == NULL) {
        filter->render_texrender_error_count++;
        roi_filter_render_fallback(filter, "capture-target-failed", LOG_WARNING);
        return;
    }

    if (!roi_filter_try_draw_subregion(effect,
                                       target_texture,
                                       &subregion,
                                       target_width,
                                       target_height,
                                       filter->preserve_aspect)) {
        roi_filter_render_fallback(filter, "draw-subregion-failed", LOG_WARNING);
        return;
    }

    filter->render_apply_count++;
}

static void roi_filter_defaults(obs_data_t *settings)
{
    obs_data_set_default_bool(settings, "enabled", true);
    obs_data_set_default_bool(settings, "preserve_aspect", true);
    obs_data_set_default_string(settings, "pipe_name", ROI_FILTER_DEFAULT_PIPE_NAME);
    obs_data_set_default_int(settings, "connect_retry_ms", ROI_FILTER_CONNECT_RETRY_DEFAULT_MS);
    obs_data_set_default_int(settings,
                             "freeze_timeout_ms",
                             ROI_INTERPOLATOR_FREEZE_TIMEOUT_DEFAULT_MS);
    obs_data_set_default_int(settings, "ipc_queue_cap", ROI_FILTER_IPC_QUEUE_CAP_DEFAULT);
    obs_data_set_default_int(settings, "stale_drop_ms", ROI_FILTER_STALE_DROP_DEFAULT_MS);
    obs_data_set_default_int(settings, "presend_v2_consumer_mode",
                             ROI_FILTER_PRESEND_V2_CONSUMER_MODE_AUTO);
    obs_data_set_default_int(settings, "roi_apply_offset_ms", 0);
    obs_data_set_default_bool(settings, "debug_log", false);
}

static obs_properties_t *roi_filter_properties(void *data)
{
    UNUSED_PARAMETER(data);

    obs_properties_t *properties = obs_properties_create();
    obs_properties_add_bool(properties, "enabled", "Enable Filter");
    obs_properties_add_bool(properties, "preserve_aspect", "Preserve Aspect (Letterbox/Pillarbox)");
    obs_properties_add_text(properties, "pipe_name", "Pipe Name", OBS_TEXT_DEFAULT);
    obs_properties_add_int(properties,
                           "connect_retry_ms",
                           "Connect Retry (ms)",
                           ROI_FILTER_CONNECT_RETRY_MIN_MS,
                           ROI_FILTER_CONNECT_RETRY_MAX_MS,
                           50);
    obs_properties_add_int(properties,
                           "freeze_timeout_ms",
                           "Freeze Timeout (ms)",
                           ROI_INTERPOLATOR_FREEZE_TIMEOUT_MIN_MS,
                           ROI_INTERPOLATOR_FREEZE_TIMEOUT_MAX_MS,
                           10);
    obs_properties_add_int(properties,
                           "ipc_queue_cap",
                           "IPC Queue Cap",
                           ROI_FILTER_IPC_QUEUE_CAP_MIN,
                           ROI_FILTER_IPC_QUEUE_CAP_MAX,
                           1);
    obs_properties_add_int(properties,
                           "stale_drop_ms",
                           "Stale Drop (ms)",
                           ROI_FILTER_STALE_DROP_MIN_MS,
                           ROI_FILTER_STALE_DROP_MAX_MS,
                           5);
    obs_property_t *presend_mode =
        obs_properties_add_list(properties,
                                "presend_v2_consumer_mode",
                                "Presend V2 (Consumer)",
                                OBS_COMBO_TYPE_LIST,
                                OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(presend_mode,
                              "Auto (by frame flag)",
                              ROI_FILTER_PRESEND_V2_CONSUMER_MODE_AUTO);
    obs_property_list_add_int(presend_mode,
                              "Force Legacy (ignore PRESEND_V2)",
                              ROI_FILTER_PRESEND_V2_CONSUMER_MODE_FORCE_LEGACY);
    obs_property_list_add_int(presend_mode,
                              "Force Buffered (treat as PRESEND_V2)",
                              ROI_FILTER_PRESEND_V2_CONSUMER_MODE_FORCE_BUFFERED);
    obs_properties_add_int(properties,
                           "roi_apply_offset_ms",
                           "ROI Apply Offset (ms)",
                           ROI_FILTER_ROI_APPLY_OFFSET_MIN_MS,
                           ROI_FILTER_ROI_APPLY_OFFSET_MAX_MS,
                           5);
    obs_properties_add_bool(properties, "debug_log", "Debug Log");
    return properties;
}

static struct obs_source_info g_roi_filter_source_info;

void riftreframe_register_roi_filter_source(void)
{
    g_roi_filter_source_info.id = "riftreframe_roi_filter";
    g_roi_filter_source_info.type = OBS_SOURCE_TYPE_FILTER;
    g_roi_filter_source_info.output_flags = OBS_SOURCE_VIDEO;
    g_roi_filter_source_info.get_name = roi_filter_get_name;
    g_roi_filter_source_info.create = roi_filter_create;
    g_roi_filter_source_info.destroy = roi_filter_destroy;
    g_roi_filter_source_info.update = roi_filter_update;
    g_roi_filter_source_info.video_tick = roi_filter_video_tick;
    g_roi_filter_source_info.video_render = roi_filter_video_render;
    g_roi_filter_source_info.get_defaults = roi_filter_defaults;
    g_roi_filter_source_info.get_properties = roi_filter_properties;

    obs_register_source(&g_roi_filter_source_info);
    blog(LOG_INFO, "[ClutchReframe] ROI filter source registered.");
}
