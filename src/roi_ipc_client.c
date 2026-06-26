/*
 * Copyright (C) 2026 ClutchReframe
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <obs-module.h>

#include "roi_ipc_client.h"
#include "roi_frame_msg_v1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef _WIN32
#error "roi_ipc_client requires Windows Named Pipe APIs."
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum {
    ROI_IPC_PIPE_NAME_MAX = 260,
    ROI_IPC_CONNECT_RETRY_MIN_MS = 50,
    ROI_IPC_CONNECT_RETRY_MAX_MS = 5000,
    ROI_IPC_CONNECT_RETRY_DEFAULT_MS = 500,
    ROI_IPC_IDLE_POLL_MS = 50,
    ROI_IPC_READ_BUFFER_BYTES = 256,
    ROI_IPC_STREAM_BUFFER_BYTES = 4096,
    ROI_IPC_FRAME_QUEUE_CAP_MIN = 1,
    ROI_IPC_FRAME_QUEUE_CAP_DEFAULT = 12,
    ROI_IPC_FRAME_QUEUE_CAP_MAX = 64
};

enum {
    ROI_IPC_DECODE_REASON_NULL_ARGUMENT = 0,
    ROI_IPC_DECODE_REASON_TRUNCATED,
    ROI_IPC_DECODE_REASON_BAD_MAGIC,
    ROI_IPC_DECODE_REASON_UNSUPPORTED_VERSION,
    ROI_IPC_DECODE_REASON_BAD_HEADER_SIZE,
    ROI_IPC_DECODE_REASON_BAD_MESSAGE_SIZE,
    ROI_IPC_DECODE_REASON_NONFINITE_ROI,
    ROI_IPC_DECODE_REASON_UNKNOWN,
    ROI_IPC_DECODE_REASON_COUNT
};

static const char *ROI_IPC_DEFAULT_PIPE_NAME = "\\\\.\\pipe\\ClutchReframe_ROI_default";

struct roi_ipc_client {
    CRITICAL_SECTION lock;
    HANDLE stop_event;
    HANDLE worker_thread;
    HANDLE pipe_handle;
    char pipe_name[ROI_IPC_PIPE_NAME_MAX];
    DWORD connect_retry_ms;
    bool debug_log;
    bool running;
    bool connected;
    bool had_first_connect;

    uint64_t connect_count;
    uint64_t reconnect_count;
    uint64_t message_accept_count;
    uint64_t decode_error_count;
    uint64_t decode_error_reason_count[ROI_IPC_DECODE_REASON_COUNT];
    uint64_t version_mismatch_count;
    uint64_t out_of_order_drop_count;
    uint64_t stream_reset_count;
    uint64_t reserved_header_warn_count;

    bool has_stream_state;
    uint32_t last_stream_id;
    uint32_t last_epoch;
    uint64_t last_seq;

    roi_frame_msg_v1_t frame_queue[ROI_IPC_FRAME_QUEUE_CAP_MAX];
    uint64_t frame_generation_queue[ROI_IPC_FRAME_QUEUE_CAP_MAX];
    size_t frame_queue_head;
    size_t frame_queue_size;
    size_t frame_queue_cap;
    uint64_t latest_generation;
    uint64_t queue_drop_count;
};

static bool roi_ipc_is_disconnect_error(DWORD error_code)
{
    return error_code == ERROR_BROKEN_PIPE || error_code == ERROR_PIPE_NOT_CONNECTED ||
           error_code == ERROR_NO_DATA || error_code == ERROR_BAD_PIPE ||
           error_code == ERROR_INVALID_HANDLE;
}

static size_t roi_ipc_clamp_queue_cap(uint32_t cap)
{
    if (cap < ROI_IPC_FRAME_QUEUE_CAP_MIN) {
        return (size_t)ROI_IPC_FRAME_QUEUE_CAP_MIN;
    }
    if (cap > ROI_IPC_FRAME_QUEUE_CAP_MAX) {
        return (size_t)ROI_IPC_FRAME_QUEUE_CAP_MAX;
    }
    return (size_t)cap;
}

static void roi_ipc_copy_pipe_name(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;

    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL || src[0] == '\0') {
        src = ROI_IPC_DEFAULT_PIPE_NAME;
    }

    for (i = 0; i + 1 < dst_size && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void roi_ipc_close_pipe_locked(struct roi_ipc_client *client)
{
    if (client->pipe_handle != NULL && client->pipe_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(client->pipe_handle);
    }
    client->pipe_handle = INVALID_HANDLE_VALUE;
    client->connected = false;
}

static const char *roi_ipc_decode_result_to_string(roi_frame_msg_v1_decode_result_t result)
{
    switch (result) {
    case ROI_FRAME_MSG_V1_DECODE_OK:
        return "OK";
    case ROI_FRAME_MSG_V1_DECODE_NULL_ARGUMENT:
        return "NULL_ARGUMENT";
    case ROI_FRAME_MSG_V1_DECODE_TRUNCATED:
        return "TRUNCATED";
    case ROI_FRAME_MSG_V1_DECODE_BAD_MAGIC:
        return "BAD_MAGIC";
    case ROI_FRAME_MSG_V1_DECODE_UNSUPPORTED_VERSION:
        return "UNSUPPORTED_VERSION";
    case ROI_FRAME_MSG_V1_DECODE_BAD_HEADER_SIZE:
        return "BAD_HEADER_SIZE";
    case ROI_FRAME_MSG_V1_DECODE_BAD_MESSAGE_SIZE:
        return "BAD_MESSAGE_SIZE";
    case ROI_FRAME_MSG_V1_DECODE_NONFINITE_ROI:
        return "NONFINITE_ROI";
    default:
        return "UNKNOWN";
    }
}

static size_t roi_ipc_decode_reason_index(roi_frame_msg_v1_decode_result_t decode_result)
{
    switch (decode_result) {
    case ROI_FRAME_MSG_V1_DECODE_NULL_ARGUMENT:
        return ROI_IPC_DECODE_REASON_NULL_ARGUMENT;
    case ROI_FRAME_MSG_V1_DECODE_TRUNCATED:
        return ROI_IPC_DECODE_REASON_TRUNCATED;
    case ROI_FRAME_MSG_V1_DECODE_BAD_MAGIC:
        return ROI_IPC_DECODE_REASON_BAD_MAGIC;
    case ROI_FRAME_MSG_V1_DECODE_UNSUPPORTED_VERSION:
        return ROI_IPC_DECODE_REASON_UNSUPPORTED_VERSION;
    case ROI_FRAME_MSG_V1_DECODE_BAD_HEADER_SIZE:
        return ROI_IPC_DECODE_REASON_BAD_HEADER_SIZE;
    case ROI_FRAME_MSG_V1_DECODE_BAD_MESSAGE_SIZE:
        return ROI_IPC_DECODE_REASON_BAD_MESSAGE_SIZE;
    case ROI_FRAME_MSG_V1_DECODE_NONFINITE_ROI:
        return ROI_IPC_DECODE_REASON_NONFINITE_ROI;
    default:
        return ROI_IPC_DECODE_REASON_UNKNOWN;
    }
}

static bool roi_ipc_should_log_count(uint64_t count)
{
    return count <= 3u || (count % 120u) == 0u;
}

static void roi_ipc_record_decode_error(struct roi_ipc_client *client,
                                        roi_frame_msg_v1_decode_result_t decode_result,
                                        DWORD raw_len)
{
    bool debug_log = false;
    bool should_log = false;
    unsigned long long decode_error_count = 0;
    unsigned long long reason_count = 0;
    unsigned long long bad_magic_count = 0;
    unsigned long long bad_message_size_count = 0;
    unsigned long long truncated_count = 0;
    unsigned long long bad_header_size_count = 0;
    unsigned long long unsupported_version_count = 0;
    unsigned long long nonfinite_roi_count = 0;
    unsigned long long null_argument_count = 0;
    unsigned long long unknown_count = 0;
    size_t reason_index = roi_ipc_decode_reason_index(decode_result);

    EnterCriticalSection(&client->lock);
    client->decode_error_count++;
    if (reason_index >= ROI_IPC_DECODE_REASON_COUNT) {
        reason_index = ROI_IPC_DECODE_REASON_UNKNOWN;
    }
    client->decode_error_reason_count[reason_index]++;
    decode_error_count = (unsigned long long)client->decode_error_count;
    reason_count = (unsigned long long)client->decode_error_reason_count[reason_index];
    should_log = roi_ipc_should_log_count(client->decode_error_count);
    debug_log = client->debug_log;
    bad_magic_count = (unsigned long long)
        client->decode_error_reason_count[ROI_IPC_DECODE_REASON_BAD_MAGIC];
    bad_message_size_count = (unsigned long long)
        client->decode_error_reason_count[ROI_IPC_DECODE_REASON_BAD_MESSAGE_SIZE];
    truncated_count = (unsigned long long)
        client->decode_error_reason_count[ROI_IPC_DECODE_REASON_TRUNCATED];
    bad_header_size_count = (unsigned long long)
        client->decode_error_reason_count[ROI_IPC_DECODE_REASON_BAD_HEADER_SIZE];
    unsupported_version_count = (unsigned long long)
        client->decode_error_reason_count[ROI_IPC_DECODE_REASON_UNSUPPORTED_VERSION];
    nonfinite_roi_count = (unsigned long long)
        client->decode_error_reason_count[ROI_IPC_DECODE_REASON_NONFINITE_ROI];
    null_argument_count = (unsigned long long)
        client->decode_error_reason_count[ROI_IPC_DECODE_REASON_NULL_ARGUMENT];
    unknown_count = (unsigned long long)
        client->decode_error_reason_count[ROI_IPC_DECODE_REASON_UNKNOWN];
    LeaveCriticalSection(&client->lock);

    if (debug_log || should_log) {
        blog(debug_log ? LOG_DEBUG : LOG_WARNING,
             "[ClutchReframe] IPC DecodeError +1 (count=%llu reason=%s reasonCount=%llu raw_len=%lu).",
             decode_error_count,
             roi_ipc_decode_result_to_string(decode_result),
             reason_count,
             (unsigned long)raw_len);
    }

    if (should_log) {
        blog(LOG_INFO,
             "[ClutchReframe] IPC DecodeError stats: total=%llu badMagic=%llu badMessageSize=%llu truncated=%llu badHeaderSize=%llu unsupportedVersion=%llu nonfiniteRoi=%llu nullArgument=%llu unknown=%llu.",
             decode_error_count,
             bad_magic_count,
             bad_message_size_count,
             truncated_count,
             bad_header_size_count,
             unsupported_version_count,
             nonfinite_roi_count,
             null_argument_count,
             unknown_count);
    }
}

static void roi_ipc_record_version_mismatch(struct roi_ipc_client *client,
                                            uint16_t schema_version,
                                            DWORD raw_len)
{
    bool debug_log = false;
    bool should_log = false;
    unsigned long long version_mismatch_count = 0;

    EnterCriticalSection(&client->lock);
    client->version_mismatch_count++;
    version_mismatch_count = (unsigned long long)client->version_mismatch_count;
    should_log = roi_ipc_should_log_count(client->version_mismatch_count);
    debug_log = client->debug_log;
    LeaveCriticalSection(&client->lock);

    if (debug_log || should_log) {
        blog(debug_log ? LOG_DEBUG : LOG_WARNING,
             "[ClutchReframe] IPC VersionMismatch +1 (count=%llu schema=%u raw_len=%lu).",
             version_mismatch_count,
             (unsigned int)schema_version,
             (unsigned long)raw_len);
    }

    if (debug_log) {
        blog(LOG_DEBUG,
             "[ClutchReframe] Expected schema=%u.",
             (unsigned int)ROI_FRAME_MSG_V1_SCHEMA_VERSION);
    }
}

static bool roi_ipc_drain_oversized_message(HANDLE pipe_handle)
{
    uint8_t discard[ROI_IPC_READ_BUFFER_BYTES];
    DWORD bytes_read = 0;

    while (ReadFile(pipe_handle, discard, sizeof(discard), &bytes_read, NULL) == FALSE) {
        const DWORD read_error = GetLastError();
        if (read_error == ERROR_MORE_DATA) {
            continue;
        }
        return false;
    }
    return true;
}

static void roi_ipc_process_frame_bytes(struct roi_ipc_client *client,
                                        const uint8_t *bytes,
                                        DWORD bytes_read);

static size_t roi_ipc_find_magic_offset(const uint8_t *bytes, size_t bytes_len)
{
    size_t offset = 0;

    if (bytes == NULL || bytes_len < sizeof(uint32_t)) {
        return bytes_len;
    }

    for (offset = 0; offset + sizeof(uint32_t) <= bytes_len; ++offset) {
        if (roi_frame_msg_v1_read_u32_le(bytes + offset) == ROI_FRAME_MSG_V1_MAGIC) {
            return offset;
        }
    }
    return bytes_len;
}

static void roi_ipc_process_stream_bytes(struct roi_ipc_client *client,
                                         uint8_t *stream_buffer,
                                         size_t *inout_stream_bytes,
                                         const uint8_t *bytes,
                                         DWORD bytes_read)
{
    size_t stream_bytes = 0;
    const uint8_t *append_bytes = bytes;
    size_t append_len = (size_t)bytes_read;

    if (client == NULL || stream_buffer == NULL || inout_stream_bytes == NULL ||
        append_bytes == NULL || append_len == 0u) {
        return;
    }

    stream_bytes = *inout_stream_bytes;

    if (append_len > (size_t)ROI_IPC_STREAM_BUFFER_BYTES) {
        roi_ipc_record_decode_error(client,
                                    ROI_FRAME_MSG_V1_DECODE_TRUNCATED,
                                    (DWORD)append_len);
        append_bytes += append_len - (size_t)ROI_IPC_STREAM_BUFFER_BYTES;
        append_len = (size_t)ROI_IPC_STREAM_BUFFER_BYTES;
        stream_bytes = 0u;
    }

    if (stream_bytes + append_len > (size_t)ROI_IPC_STREAM_BUFFER_BYTES) {
        size_t overflow = stream_bytes + append_len - (size_t)ROI_IPC_STREAM_BUFFER_BYTES;
        if (overflow > stream_bytes) {
            overflow = stream_bytes;
        }
        if (overflow > 0u) {
            memmove(stream_buffer, stream_buffer + overflow, stream_bytes - overflow);
            stream_bytes -= overflow;
            roi_ipc_record_decode_error(client,
                                        ROI_FRAME_MSG_V1_DECODE_TRUNCATED,
                                        (DWORD)overflow);
        }
    }

    memcpy(stream_buffer + stream_bytes, append_bytes, append_len);
    stream_bytes += append_len;

    while (stream_bytes > 0u) {
        size_t magic_offset = 0u;
        uint16_t message_size = 0u;

        if (stream_bytes < sizeof(uint32_t)) {
            break;
        }

        magic_offset = roi_ipc_find_magic_offset(stream_buffer, stream_bytes);
        if (magic_offset == stream_bytes) {
            /* Preserve possible partial magic prefix for next ReadFile chunk. */
            const size_t keep = (stream_bytes < (sizeof(uint32_t) - 1u))
                                    ? stream_bytes
                                    : (sizeof(uint32_t) - 1u);
            const size_t drop = stream_bytes - keep;
            if (drop > 0u) {
                memmove(stream_buffer, stream_buffer + drop, keep);
                stream_bytes = keep;
                roi_ipc_record_decode_error(client,
                                            ROI_FRAME_MSG_V1_DECODE_BAD_MAGIC,
                                            (DWORD)drop);
            }
            break;
        }

        if (magic_offset > 0u) {
            memmove(stream_buffer, stream_buffer + magic_offset, stream_bytes - magic_offset);
            stream_bytes -= magic_offset;
            roi_ipc_record_decode_error(client,
                                        ROI_FRAME_MSG_V1_DECODE_BAD_MAGIC,
                                        (DWORD)magic_offset);
            continue;
        }

        if (stream_bytes < ROI_FRAME_MSG_V1_OFFSET_MESSAGE_SIZE + sizeof(uint16_t)) {
            break;
        }

        message_size =
            roi_frame_msg_v1_read_u16_le(stream_buffer + ROI_FRAME_MSG_V1_OFFSET_MESSAGE_SIZE);
        /* Resync by 1 byte when header looks invalid at current magic position. */
        if (message_size < ROI_FRAME_MSG_V1_MESSAGE_SIZE ||
            message_size > (uint16_t)ROI_IPC_STREAM_BUFFER_BYTES) {
            roi_ipc_record_decode_error(client,
                                        ROI_FRAME_MSG_V1_DECODE_BAD_MESSAGE_SIZE,
                                        (DWORD)stream_bytes);
            memmove(stream_buffer, stream_buffer + 1u, stream_bytes - 1u);
            stream_bytes -= 1u;
            continue;
        }

        if (stream_bytes < (size_t)message_size) {
            break;
        }

        roi_ipc_process_frame_bytes(client, stream_buffer, (DWORD)message_size);
        if (stream_bytes > (size_t)message_size) {
            memmove(stream_buffer,
                    stream_buffer + (size_t)message_size,
                    stream_bytes - (size_t)message_size);
        }
        stream_bytes -= (size_t)message_size;
    }

    *inout_stream_bytes = stream_bytes;
}

static void roi_ipc_process_decoded_message(struct roi_ipc_client *client,
                                            const roi_frame_msg_v1_t *message)
{
    bool debug_log = false;
    bool should_log_drop = false;
    bool should_log_stream_reset = false;
    bool should_log_queue_drop = false;
    bool dropped_out_of_order = false;
    bool stream_reset = false;
    bool reserved_header_warn = false;
    unsigned long long out_of_order_drop_count = 0;
    unsigned long long stream_reset_count = 0;
    unsigned long long reserved_warn_count = 0;
    unsigned long long accept_count = 0;
    unsigned long long queue_drop_count = 0;
    unsigned long long queue_depth = 0;
    unsigned long long queue_cap = 0;

    EnterCriticalSection(&client->lock);
    debug_log = client->debug_log;
    if (client->frame_queue_cap < (size_t)ROI_IPC_FRAME_QUEUE_CAP_MIN) {
        client->frame_queue_cap = (size_t)ROI_IPC_FRAME_QUEUE_CAP_MIN;
    }
    queue_cap = (unsigned long long)client->frame_queue_cap;

    if (message->reserved_header != 0u) {
        client->reserved_header_warn_count++;
        reserved_warn_count = (unsigned long long)client->reserved_header_warn_count;
        reserved_header_warn = true;
    }

    if (client->has_stream_state && message->stream_id == client->last_stream_id &&
        message->epoch == client->last_epoch && message->seq <= client->last_seq) {
        client->out_of_order_drop_count++;
        out_of_order_drop_count = (unsigned long long)client->out_of_order_drop_count;
        should_log_drop = roi_ipc_should_log_count(client->out_of_order_drop_count);
        dropped_out_of_order = true;
    } else {
        if (client->has_stream_state && message->epoch != client->last_epoch) {
            client->stream_reset_count++;
            stream_reset_count = (unsigned long long)client->stream_reset_count;
            should_log_stream_reset = roi_ipc_should_log_count(client->stream_reset_count);
            stream_reset = true;
        }

        client->has_stream_state = true;
        client->last_stream_id = message->stream_id;
        client->last_epoch = message->epoch;
        client->last_seq = message->seq;
        client->message_accept_count++;
        accept_count = (unsigned long long)client->message_accept_count;
        client->latest_generation++;

        if (stream_reset && client->frame_queue_size > 0u) {
            client->queue_drop_count += (uint64_t)client->frame_queue_size;
            client->frame_queue_size = 0u;
            client->frame_queue_head = 0u;
            should_log_queue_drop = true;
        }

        if (client->frame_queue_size >= client->frame_queue_cap) {
            client->frame_queue_head =
                (client->frame_queue_head + 1u) % client->frame_queue_cap;
            client->frame_queue_size--;
            client->queue_drop_count++;
            should_log_queue_drop = true;
        }

        {
            const size_t insert_index =
                (client->frame_queue_head + client->frame_queue_size) %
                client->frame_queue_cap;
            client->frame_queue[insert_index] = *message;
            client->frame_generation_queue[insert_index] = client->latest_generation;
            client->frame_queue_size++;
        }
        queue_drop_count = (unsigned long long)client->queue_drop_count;
        queue_depth = (unsigned long long)client->frame_queue_size;
        should_log_queue_drop = should_log_queue_drop &&
                                roi_ipc_should_log_count(client->queue_drop_count);
    }

    LeaveCriticalSection(&client->lock);

    if (reserved_header_warn && debug_log) {
        blog(LOG_DEBUG,
             "[ClutchReframe] IPC reservedHeader=%u (compat warn count=%llu).",
             (unsigned int)message->reserved_header,
             reserved_warn_count);
    }

    if (dropped_out_of_order) {
        if (debug_log || should_log_drop) {
            blog(debug_log ? LOG_DEBUG : LOG_WARNING,
                 "[ClutchReframe] IPC OutOfOrderDrop +1 (count=%llu stream=%u epoch=%u seq=%llu).",
                 out_of_order_drop_count,
                 (unsigned int)message->stream_id,
                 (unsigned int)message->epoch,
                 (unsigned long long)message->seq);
        }
        return;
    }

    if (stream_reset && (debug_log || should_log_stream_reset)) {
        blog(LOG_INFO,
             "[ClutchReframe] IPC StreamReset +1 (count=%llu stream=%u epoch=%u seq=%llu).",
             stream_reset_count,
             (unsigned int)message->stream_id,
             (unsigned int)message->epoch,
             (unsigned long long)message->seq);
    }

    if (debug_log) {
        blog(LOG_DEBUG,
             "[ClutchReframe] IPC frame accepted (count=%llu stream=%u epoch=%u seq=%llu valid=%u "
             "tQpc=%lld queueDepth=%llu).",
             accept_count,
             (unsigned int)message->stream_id,
             (unsigned int)message->epoch,
             (unsigned long long)message->seq,
             (unsigned int)message->valid,
             (long long)message->t_qpc,
             queue_depth);
    }

    if (should_log_queue_drop || (debug_log && queue_drop_count > 0u)) {
        blog(debug_log ? LOG_DEBUG : LOG_WARNING,
             "[ClutchReframe] IPC queue drop observed (count=%llu depth=%llu cap=%u).",
             queue_drop_count,
             queue_depth,
             (unsigned int)queue_cap);
    }
}

static void roi_ipc_process_frame_bytes(struct roi_ipc_client *client,
                                        const uint8_t *bytes,
                                        DWORD bytes_read)
{
    roi_frame_msg_v1_t message;
    roi_frame_msg_v1_decode_result_t decode_result =
        roi_frame_msg_v1_decode(bytes, (size_t)bytes_read, &message);

    if (decode_result == ROI_FRAME_MSG_V1_DECODE_OK) {
        roi_ipc_process_decoded_message(client, &message);
        return;
    }

    if (decode_result == ROI_FRAME_MSG_V1_DECODE_UNSUPPORTED_VERSION) {
        const uint16_t schema_version =
            (bytes_read >= ROI_FRAME_MSG_V1_OFFSET_HEADER_SIZE)
                ? roi_frame_msg_v1_read_u16_le(bytes + ROI_FRAME_MSG_V1_OFFSET_SCHEMA_VERSION)
                : 0u;
        roi_ipc_record_version_mismatch(client, schema_version, bytes_read);
        return;
    }

    roi_ipc_record_decode_error(client, decode_result, bytes_read);
}

static DWORD WINAPI roi_ipc_worker_thread_proc(LPVOID param)
{
    struct roi_ipc_client *client = param;
    uint8_t stream_buffer[ROI_IPC_STREAM_BUFFER_BYTES] = {0};
    size_t stream_bytes = 0u;

    while (WaitForSingleObject(client->stop_event, 0) != WAIT_OBJECT_0) {
        char pipe_name[ROI_IPC_PIPE_NAME_MAX];
        DWORD retry_ms = ROI_IPC_CONNECT_RETRY_DEFAULT_MS;
        bool debug_log = false;
        HANDLE pipe_handle = INVALID_HANDLE_VALUE;
        DWORD bytes_available = 0;

        EnterCriticalSection(&client->lock);
        roi_ipc_copy_pipe_name(pipe_name, sizeof(pipe_name), client->pipe_name);
        retry_ms = client->connect_retry_ms;
        debug_log = client->debug_log;
        pipe_handle = client->pipe_handle;
        LeaveCriticalSection(&client->lock);

        if (pipe_handle == NULL || pipe_handle == INVALID_HANDLE_VALUE) {
            HANDLE new_pipe = CreateFileA(pipe_name,
                                          GENERIC_READ | FILE_WRITE_ATTRIBUTES,
                                          0,
                                          NULL,
                                          OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL,
                                          NULL);
            if (new_pipe != INVALID_HANDLE_VALUE) {
                DWORD read_mode = PIPE_READMODE_MESSAGE;
                unsigned long long connect_index = 0;
                unsigned long long reconnect_index = 0;
                if (!SetNamedPipeHandleState(new_pipe, &read_mode, NULL, NULL)) {
                    const DWORD set_mode_error = GetLastError();
                    CloseHandle(new_pipe);
                    blog(LOG_WARNING,
                         "[ClutchReframe] IPC set pipe read mode failed (pipe=%s err=%lu), reconnecting.",
                         pipe_name,
                         (unsigned long)set_mode_error);
                    stream_bytes = 0u;
                    (void)WaitForSingleObject(client->stop_event, retry_ms);
                    continue;
                }

                EnterCriticalSection(&client->lock);
                client->pipe_handle = new_pipe;
                client->connected = true;
                client->connect_count++;
                connect_index = (unsigned long long)client->connect_count;
                if (client->had_first_connect) {
                    client->reconnect_count++;
                }
                reconnect_index = (unsigned long long)client->reconnect_count;
                client->had_first_connect = true;
                LeaveCriticalSection(&client->lock);

                blog(LOG_INFO,
                     "[ClutchReframe] IPC connected to pipe=%s (connect=%llu reconnect=%llu).",
                     pipe_name,
                     connect_index,
                     reconnect_index);
                stream_bytes = 0u;
                continue;
            }

            if (debug_log) {
                blog(LOG_DEBUG,
                     "[ClutchReframe] IPC connect pending for pipe=%s (err=%lu).",
                     pipe_name,
                     (unsigned long)GetLastError());
            }

            (void)WaitForSingleObject(client->stop_event, retry_ms);
            continue;
        }

        if (!PeekNamedPipe(pipe_handle, NULL, 0, NULL, &bytes_available, NULL)) {
            const DWORD pipe_error = GetLastError();

            EnterCriticalSection(&client->lock);
            roi_ipc_close_pipe_locked(client);
            LeaveCriticalSection(&client->lock);
            stream_bytes = 0u;

            blog(LOG_WARNING,
                 "[ClutchReframe] IPC pipe disconnected (err=%lu), reconnecting.",
                 (unsigned long)pipe_error);
            continue;
        }

        if (bytes_available == 0) {
            (void)WaitForSingleObject(client->stop_event, ROI_IPC_IDLE_POLL_MS);
            continue;
        }

        {
            uint8_t read_buffer[ROI_IPC_READ_BUFFER_BYTES];
            DWORD bytes_read = 0;
            BOOL read_ok = ReadFile(pipe_handle,
                                    read_buffer,
                                    (DWORD)sizeof(read_buffer),
                                    &bytes_read,
                                    NULL);
            if (!read_ok) {
                const DWORD read_error = GetLastError();
                if (read_error == ERROR_MORE_DATA) {
                    roi_ipc_record_decode_error(client,
                                                ROI_FRAME_MSG_V1_DECODE_BAD_MESSAGE_SIZE,
                                                bytes_read);
                    if (!roi_ipc_drain_oversized_message(pipe_handle)) {
                        const DWORD drain_error = GetLastError();
                        if (roi_ipc_is_disconnect_error(drain_error)) {
                            EnterCriticalSection(&client->lock);
                            roi_ipc_close_pipe_locked(client);
                            LeaveCriticalSection(&client->lock);
                            stream_bytes = 0u;
                        }
                    }
                    stream_bytes = 0u;
                    continue;
                }

                if (roi_ipc_is_disconnect_error(read_error)) {
                    EnterCriticalSection(&client->lock);
                    roi_ipc_close_pipe_locked(client);
                    LeaveCriticalSection(&client->lock);
                    stream_bytes = 0u;
                    blog(LOG_WARNING,
                         "[ClutchReframe] IPC read disconnected (err=%lu), reconnecting.",
                         (unsigned long)read_error);
                    continue;
                }

                if (debug_log) {
                    blog(LOG_DEBUG,
                         "[ClutchReframe] IPC read failed (err=%lu).",
                         (unsigned long)read_error);
                }
                continue;
            }

            roi_ipc_process_stream_bytes(client,
                                         stream_buffer,
                                         &stream_bytes,
                                         read_buffer,
                                         bytes_read);
        }
    }

    EnterCriticalSection(&client->lock);
    roi_ipc_close_pipe_locked(client);
    client->running = false;
    LeaveCriticalSection(&client->lock);
    return 0;
}

struct roi_ipc_client *roi_ipc_client_create(void)
{
    struct roi_ipc_client *client = bzalloc(sizeof(*client));
    if (client == NULL) {
        return NULL;
    }

    InitializeCriticalSection(&client->lock);
    client->stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (client->stop_event == NULL) {
        DeleteCriticalSection(&client->lock);
        bfree(client);
        return NULL;
    }

    client->worker_thread = NULL;
    client->pipe_handle = INVALID_HANDLE_VALUE;
    client->connect_retry_ms = ROI_IPC_CONNECT_RETRY_DEFAULT_MS;
    client->frame_queue_cap = roi_ipc_clamp_queue_cap(ROI_IPC_FRAME_QUEUE_CAP_DEFAULT);
    roi_ipc_copy_pipe_name(client->pipe_name, sizeof(client->pipe_name), ROI_IPC_DEFAULT_PIPE_NAME);
    return client;
}

void roi_ipc_client_destroy(struct roi_ipc_client *client)
{
    if (client == NULL) {
        return;
    }

    roi_ipc_client_stop(client);

    if (client->stop_event != NULL) {
        CloseHandle(client->stop_event);
        client->stop_event = NULL;
    }

    DeleteCriticalSection(&client->lock);
    bfree(client);
}

void roi_ipc_client_set_pipe_name(struct roi_ipc_client *client, const char *pipe_name)
{
    if (client == NULL) {
        return;
    }

    EnterCriticalSection(&client->lock);
    roi_ipc_copy_pipe_name(client->pipe_name, sizeof(client->pipe_name), pipe_name);
    if (client->connected) {
        roi_ipc_close_pipe_locked(client);
    }
    client->frame_queue_head = 0u;
    client->frame_queue_size = 0u;
    client->has_stream_state = false;
    LeaveCriticalSection(&client->lock);
}

void roi_ipc_client_set_connect_retry_ms(struct roi_ipc_client *client, uint32_t retry_ms)
{
    DWORD clamped_retry_ms = retry_ms;

    if (client == NULL) {
        return;
    }

    if (clamped_retry_ms < ROI_IPC_CONNECT_RETRY_MIN_MS) {
        clamped_retry_ms = ROI_IPC_CONNECT_RETRY_MIN_MS;
    }
    if (clamped_retry_ms > ROI_IPC_CONNECT_RETRY_MAX_MS) {
        clamped_retry_ms = ROI_IPC_CONNECT_RETRY_MAX_MS;
    }

    EnterCriticalSection(&client->lock);
    client->connect_retry_ms = clamped_retry_ms;
    LeaveCriticalSection(&client->lock);
}

void roi_ipc_client_set_debug_log(struct roi_ipc_client *client, bool debug_log)
{
    if (client == NULL) {
        return;
    }

    EnterCriticalSection(&client->lock);
    client->debug_log = debug_log;
    LeaveCriticalSection(&client->lock);
}

void roi_ipc_client_set_queue_cap(struct roi_ipc_client *client, uint32_t queue_cap)
{
    size_t clamped_cap = roi_ipc_clamp_queue_cap(queue_cap);

    if (client == NULL) {
        return;
    }

    EnterCriticalSection(&client->lock);
    if (client->frame_queue_cap != clamped_cap) {
        if (client->frame_queue_size > 0u) {
            client->queue_drop_count += (uint64_t)client->frame_queue_size;
        }
        client->frame_queue_cap = clamped_cap;
        client->frame_queue_head = 0u;
        client->frame_queue_size = 0u;
    }
    LeaveCriticalSection(&client->lock);
}

bool roi_ipc_client_start(struct roi_ipc_client *client)
{
    if (client == NULL) {
        return false;
    }

    EnterCriticalSection(&client->lock);
    if (client->running) {
        LeaveCriticalSection(&client->lock);
        return true;
    }

    ResetEvent(client->stop_event);
    client->frame_queue_head = 0u;
    client->frame_queue_size = 0u;
    client->has_stream_state = false;
    client->worker_thread = CreateThread(NULL, 0, roi_ipc_worker_thread_proc, client, 0, NULL);
    if (client->worker_thread == NULL) {
        LeaveCriticalSection(&client->lock);
        blog(LOG_WARNING, "[ClutchReframe] IPC worker thread create failed (err=%lu).",
             (unsigned long)GetLastError());
        return false;
    }

    client->running = true;
    LeaveCriticalSection(&client->lock);
    return true;
}

void roi_ipc_client_stop(struct roi_ipc_client *client)
{
    HANDLE worker_thread = NULL;

    if (client == NULL) {
        return;
    }

    EnterCriticalSection(&client->lock);
    worker_thread = client->worker_thread;
    if (worker_thread == NULL) {
        roi_ipc_close_pipe_locked(client);
        client->running = false;
        LeaveCriticalSection(&client->lock);
        return;
    }
    SetEvent(client->stop_event);
    LeaveCriticalSection(&client->lock);

    WaitForSingleObject(worker_thread, INFINITE);
    CloseHandle(worker_thread);

    EnterCriticalSection(&client->lock);
    client->worker_thread = NULL;
    client->running = false;
    roi_ipc_close_pipe_locked(client);
    client->frame_queue_head = 0u;
    client->frame_queue_size = 0u;
    client->has_stream_state = false;
    ResetEvent(client->stop_event);
    LeaveCriticalSection(&client->lock);
}

bool roi_ipc_client_try_consume_latest(struct roi_ipc_client *client,
                                       roi_frame_msg_v1_t *out_message,
                                       uint64_t *out_generation)
{
    bool has_latest = false;
    size_t frame_queue_cap = 0u;

    if (client == NULL || out_message == NULL) {
        return false;
    }

    EnterCriticalSection(&client->lock);
    if (client->frame_queue_cap < (size_t)ROI_IPC_FRAME_QUEUE_CAP_MIN) {
        client->frame_queue_cap = (size_t)ROI_IPC_FRAME_QUEUE_CAP_MIN;
    }
    frame_queue_cap = client->frame_queue_cap;
    if (client->frame_queue_size > 0u) {
        const size_t latest_index =
            (client->frame_queue_head + client->frame_queue_size - 1u) %
            frame_queue_cap;
        *out_message = client->frame_queue[latest_index];
        if (out_generation != NULL) {
            *out_generation = client->frame_generation_queue[latest_index];
        }
        client->frame_queue_size = 0u;
        client->frame_queue_head = 0u;
        has_latest = true;
    }
    LeaveCriticalSection(&client->lock);

    return has_latest;
}

bool roi_ipc_client_try_consume_next(struct roi_ipc_client *client,
                                     roi_frame_msg_v1_t *out_message,
                                     uint64_t *out_generation)
{
    bool has_next = false;
    size_t frame_queue_cap = 0u;

    if (client == NULL || out_message == NULL) {
        return false;
    }

    EnterCriticalSection(&client->lock);
    if (client->frame_queue_cap < (size_t)ROI_IPC_FRAME_QUEUE_CAP_MIN) {
        client->frame_queue_cap = (size_t)ROI_IPC_FRAME_QUEUE_CAP_MIN;
    }
    frame_queue_cap = client->frame_queue_cap;
    if (client->frame_queue_size > 0u) {
        const size_t head = client->frame_queue_head;
        *out_message = client->frame_queue[head];
        if (out_generation != NULL) {
            *out_generation = client->frame_generation_queue[head];
        }
        client->frame_queue_head = (head + 1u) % frame_queue_cap;
        client->frame_queue_size--;
        has_next = true;
    }
    LeaveCriticalSection(&client->lock);

    return has_next;
}

void roi_ipc_client_get_stats(struct roi_ipc_client *client,
                              roi_ipc_client_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }

    memset(out_stats, 0, sizeof(*out_stats));
    if (client == NULL) {
        return;
    }

    EnterCriticalSection(&client->lock);
    out_stats->connected = client->connected;
    out_stats->has_stream_state = client->has_stream_state;
    out_stats->last_stream_id = client->last_stream_id;
    out_stats->last_epoch = client->last_epoch;
    out_stats->last_seq = client->last_seq;
    out_stats->latest_generation = client->latest_generation;
    out_stats->queued_frame_count = (uint64_t)client->frame_queue_size;
    out_stats->queue_drop_count = client->queue_drop_count;
    out_stats->queue_capacity = (uint32_t)client->frame_queue_cap;
    out_stats->connect_count = client->connect_count;
    out_stats->reconnect_count = client->reconnect_count;
    out_stats->message_accept_count = client->message_accept_count;
    out_stats->decode_error_count = client->decode_error_count;
    out_stats->version_mismatch_count = client->version_mismatch_count;
    out_stats->out_of_order_drop_count = client->out_of_order_drop_count;
    out_stats->stream_reset_count = client->stream_reset_count;
    out_stats->reserved_header_warn_count = client->reserved_header_warn_count;
    LeaveCriticalSection(&client->lock);
}

bool roi_ipc_client_is_connected(struct roi_ipc_client *client)
{
    bool connected = false;

    if (client == NULL) {
        return false;
    }

    EnterCriticalSection(&client->lock);
    connected = client->connected;
    LeaveCriticalSection(&client->lock);
    return connected;
}
