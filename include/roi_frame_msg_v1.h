/*
 * Copyright (C) 2026 ClutchReframe
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RIFTREFRAME_ROI_FRAME_MSG_V1_H
#define RIFTREFRAME_ROI_FRAME_MSG_V1_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ROI_FRAME_MSG_V1_MAGIC = 0x31494F52u, /* "ROI1" in little-endian bytes */
    ROI_FRAME_MSG_V1_SCHEMA_VERSION = 1u,
    ROI_FRAME_MSG_V1_HEADER_SIZE = 12u,
    ROI_FRAME_MSG_V1_MESSAGE_SIZE = 68u
};

/* flags bitset (v1): docs/live_obs_plugin/IPC_v1_Spec.md §4.1 */
enum {
    ROI_FRAME_MSG_V1_FLAG_WARMUP_OR_NOT_ENOUGH_DATA = 0x01u,
    ROI_FRAME_MSG_V1_FLAG_STRATEGY_INVALID_ROI = 0x02u,
    ROI_FRAME_MSG_V1_FLAG_TQPC_EFFECTIVE_TIME = 0x04u,
    ROI_FRAME_MSG_V1_FLAG_FRAME_STREAM = 0x08u,
    ROI_FRAME_MSG_V1_FLAG_PRESEND_V2 = 0x10u
};

enum {
    ROI_FRAME_MSG_V1_OFFSET_MAGIC = 0u,
    ROI_FRAME_MSG_V1_OFFSET_SCHEMA_VERSION = 4u,
    ROI_FRAME_MSG_V1_OFFSET_HEADER_SIZE = 6u,
    ROI_FRAME_MSG_V1_OFFSET_MESSAGE_SIZE = 8u,
    ROI_FRAME_MSG_V1_OFFSET_RESERVED_HEADER = 10u,
    ROI_FRAME_MSG_V1_OFFSET_STREAM_ID = 12u,
    ROI_FRAME_MSG_V1_OFFSET_EPOCH = 16u,
    ROI_FRAME_MSG_V1_OFFSET_SEQ = 20u,
    ROI_FRAME_MSG_V1_OFFSET_QPC_FREQUENCY = 28u,
    ROI_FRAME_MSG_V1_OFFSET_T_QPC = 36u,
    ROI_FRAME_MSG_V1_OFFSET_X = 44u,
    ROI_FRAME_MSG_V1_OFFSET_Y = 48u,
    ROI_FRAME_MSG_V1_OFFSET_W = 52u,
    ROI_FRAME_MSG_V1_OFFSET_H = 56u,
    ROI_FRAME_MSG_V1_OFFSET_VALID = 60u,
    ROI_FRAME_MSG_V1_OFFSET_FLAGS = 61u,
    ROI_FRAME_MSG_V1_OFFSET_RESERVED0 = 62u,
    ROI_FRAME_MSG_V1_OFFSET_RESERVED1 = 64u
};

typedef enum roi_frame_msg_v1_decode_result {
    ROI_FRAME_MSG_V1_DECODE_OK = 0,
    ROI_FRAME_MSG_V1_DECODE_NULL_ARGUMENT,
    ROI_FRAME_MSG_V1_DECODE_TRUNCATED,
    ROI_FRAME_MSG_V1_DECODE_BAD_MAGIC,
    ROI_FRAME_MSG_V1_DECODE_UNSUPPORTED_VERSION,
    ROI_FRAME_MSG_V1_DECODE_BAD_HEADER_SIZE,
    ROI_FRAME_MSG_V1_DECODE_BAD_MESSAGE_SIZE,
    ROI_FRAME_MSG_V1_DECODE_NONFINITE_ROI
} roi_frame_msg_v1_decode_result_t;

typedef struct roi_frame_msg_v1 {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t header_size;
    uint16_t message_size;
    uint16_t reserved_header;
    uint32_t stream_id;
    uint32_t epoch;
    uint64_t seq;
    uint64_t qpc_frequency;
    int64_t t_qpc;
    float x;
    float y;
    float w;
    float h;
    uint8_t valid;
    uint8_t flags;
    uint16_t reserved0;
    uint32_t reserved1;
} roi_frame_msg_v1_t;

static inline uint16_t roi_frame_msg_v1_read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static inline uint32_t roi_frame_msg_v1_read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)((uint32_t)bytes[0] |
                      ((uint32_t)bytes[1] << 8) |
                      ((uint32_t)bytes[2] << 16) |
                      ((uint32_t)bytes[3] << 24));
}

static inline uint64_t roi_frame_msg_v1_read_u64_le(const uint8_t *bytes)
{
    return (uint64_t)bytes[0] |
           ((uint64_t)bytes[1] << 8) |
           ((uint64_t)bytes[2] << 16) |
           ((uint64_t)bytes[3] << 24) |
           ((uint64_t)bytes[4] << 32) |
           ((uint64_t)bytes[5] << 40) |
           ((uint64_t)bytes[6] << 48) |
           ((uint64_t)bytes[7] << 56);
}

static inline int64_t roi_frame_msg_v1_read_i64_le(const uint8_t *bytes)
{
    return (int64_t)roi_frame_msg_v1_read_u64_le(bytes);
}

static inline float roi_frame_msg_v1_read_f32_le(const uint8_t *bytes)
{
    uint32_t bits = roi_frame_msg_v1_read_u32_le(bytes);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline bool roi_frame_msg_v1_is_finite_roi(const roi_frame_msg_v1_t *msg)
{
    return isfinite((double)msg->x) &&
           isfinite((double)msg->y) &&
           isfinite((double)msg->w) &&
           isfinite((double)msg->h);
}

static inline roi_frame_msg_v1_decode_result_t roi_frame_msg_v1_decode(
    const uint8_t *bytes,
    size_t bytes_len,
    roi_frame_msg_v1_t *out_msg)
{
    uint16_t schema_version;
    uint16_t header_size;
    uint16_t message_size;

    if (bytes == NULL || out_msg == NULL) {
        return ROI_FRAME_MSG_V1_DECODE_NULL_ARGUMENT;
    }
    if (bytes_len < ROI_FRAME_MSG_V1_MESSAGE_SIZE) {
        return ROI_FRAME_MSG_V1_DECODE_TRUNCATED;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->magic = roi_frame_msg_v1_read_u32_le(bytes + ROI_FRAME_MSG_V1_OFFSET_MAGIC);
    if (out_msg->magic != ROI_FRAME_MSG_V1_MAGIC) {
        return ROI_FRAME_MSG_V1_DECODE_BAD_MAGIC;
    }

    schema_version =
        roi_frame_msg_v1_read_u16_le(bytes + ROI_FRAME_MSG_V1_OFFSET_SCHEMA_VERSION);
    if (schema_version != ROI_FRAME_MSG_V1_SCHEMA_VERSION) {
        return ROI_FRAME_MSG_V1_DECODE_UNSUPPORTED_VERSION;
    }
    out_msg->schema_version = schema_version;

    header_size =
        roi_frame_msg_v1_read_u16_le(bytes + ROI_FRAME_MSG_V1_OFFSET_HEADER_SIZE);
    if (header_size != ROI_FRAME_MSG_V1_HEADER_SIZE) {
        return ROI_FRAME_MSG_V1_DECODE_BAD_HEADER_SIZE;
    }
    out_msg->header_size = header_size;

    message_size =
        roi_frame_msg_v1_read_u16_le(bytes + ROI_FRAME_MSG_V1_OFFSET_MESSAGE_SIZE);
    if (message_size != ROI_FRAME_MSG_V1_MESSAGE_SIZE || bytes_len != message_size) {
        return ROI_FRAME_MSG_V1_DECODE_BAD_MESSAGE_SIZE;
    }
    out_msg->message_size = message_size;

    out_msg->reserved_header =
        roi_frame_msg_v1_read_u16_le(bytes + ROI_FRAME_MSG_V1_OFFSET_RESERVED_HEADER);
    out_msg->stream_id = roi_frame_msg_v1_read_u32_le(bytes + ROI_FRAME_MSG_V1_OFFSET_STREAM_ID);
    out_msg->epoch = roi_frame_msg_v1_read_u32_le(bytes + ROI_FRAME_MSG_V1_OFFSET_EPOCH);
    out_msg->seq = roi_frame_msg_v1_read_u64_le(bytes + ROI_FRAME_MSG_V1_OFFSET_SEQ);
    out_msg->qpc_frequency =
        roi_frame_msg_v1_read_u64_le(bytes + ROI_FRAME_MSG_V1_OFFSET_QPC_FREQUENCY);
    out_msg->t_qpc = roi_frame_msg_v1_read_i64_le(bytes + ROI_FRAME_MSG_V1_OFFSET_T_QPC);
    out_msg->x = roi_frame_msg_v1_read_f32_le(bytes + ROI_FRAME_MSG_V1_OFFSET_X);
    out_msg->y = roi_frame_msg_v1_read_f32_le(bytes + ROI_FRAME_MSG_V1_OFFSET_Y);
    out_msg->w = roi_frame_msg_v1_read_f32_le(bytes + ROI_FRAME_MSG_V1_OFFSET_W);
    out_msg->h = roi_frame_msg_v1_read_f32_le(bytes + ROI_FRAME_MSG_V1_OFFSET_H);
    out_msg->valid = bytes[ROI_FRAME_MSG_V1_OFFSET_VALID];
    out_msg->flags = bytes[ROI_FRAME_MSG_V1_OFFSET_FLAGS];
    out_msg->reserved0 =
        roi_frame_msg_v1_read_u16_le(bytes + ROI_FRAME_MSG_V1_OFFSET_RESERVED0);
    out_msg->reserved1 =
        roi_frame_msg_v1_read_u32_le(bytes + ROI_FRAME_MSG_V1_OFFSET_RESERVED1);

    if (!roi_frame_msg_v1_is_finite_roi(out_msg)) {
        return ROI_FRAME_MSG_V1_DECODE_NONFINITE_ROI;
    }

    return ROI_FRAME_MSG_V1_DECODE_OK;
}

_Static_assert(ROI_FRAME_MSG_V1_MESSAGE_SIZE == 68u, "FrameMsgV1 message size drifted.");
_Static_assert(ROI_FRAME_MSG_V1_OFFSET_RESERVED1 + sizeof(uint32_t) ==
                   ROI_FRAME_MSG_V1_MESSAGE_SIZE,
               "FrameMsgV1 field offset drifted.");

#ifdef __cplusplus
}
#endif

#endif /* RIFTREFRAME_ROI_FRAME_MSG_V1_H */
