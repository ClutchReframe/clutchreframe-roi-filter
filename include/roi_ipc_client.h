/*
 * Copyright (C) 2026 ClutchReframe
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RIFTREFRAME_ROI_IPC_CLIENT_H
#define RIFTREFRAME_ROI_IPC_CLIENT_H

#include "roi_frame_msg_v1.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct roi_ipc_client;

typedef struct roi_ipc_client_stats {
    bool connected;
    bool has_stream_state;
    uint32_t last_stream_id;
    uint32_t last_epoch;
    uint64_t last_seq;
    uint64_t latest_generation;
    uint64_t queued_frame_count;
    uint64_t queue_drop_count;
    uint32_t queue_capacity;
    uint64_t connect_count;
    uint64_t reconnect_count;
    uint64_t message_accept_count;
    uint64_t decode_error_count;
    uint64_t version_mismatch_count;
    uint64_t out_of_order_drop_count;
    uint64_t stream_reset_count;
    uint64_t reserved_header_warn_count;
} roi_ipc_client_stats_t;

struct roi_ipc_client *roi_ipc_client_create(void);
void roi_ipc_client_destroy(struct roi_ipc_client *client);

void roi_ipc_client_set_pipe_name(struct roi_ipc_client *client, const char *pipe_name);
void roi_ipc_client_set_connect_retry_ms(struct roi_ipc_client *client, uint32_t retry_ms);
void roi_ipc_client_set_debug_log(struct roi_ipc_client *client, bool debug_log);
void roi_ipc_client_set_queue_cap(struct roi_ipc_client *client, uint32_t queue_cap);

bool roi_ipc_client_start(struct roi_ipc_client *client);
void roi_ipc_client_stop(struct roi_ipc_client *client);
bool roi_ipc_client_is_connected(struct roi_ipc_client *client);

bool roi_ipc_client_try_consume_latest(struct roi_ipc_client *client,
                                       roi_frame_msg_v1_t *out_message,
                                       uint64_t *out_generation);
bool roi_ipc_client_try_consume_next(struct roi_ipc_client *client,
                                     roi_frame_msg_v1_t *out_message,
                                     uint64_t *out_generation);
void roi_ipc_client_get_stats(struct roi_ipc_client *client,
                              roi_ipc_client_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* RIFTREFRAME_ROI_IPC_CLIENT_H */
