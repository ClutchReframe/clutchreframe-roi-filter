# ClutchReframe ROI Filter IPC Reference

This document describes the public IPC contract needed to build, inspect, or modify the OBS plugin. It intentionally documents only the named pipe wire format and consumer behavior; it does not describe proprietary ROI detection or strategy internals.

## Transport

- Transport: Windows named pipe.
- Default pipe name: `\\.\pipe\ClutchReframe_ROI_default`.
- Direction: ROI producer process to OBS plugin consumer.
- Message mode: fixed-size `FrameMsgV1` messages.
- Runtime path: `pipe` only. WebSocket paths are historical and are not part of the current plugin runtime.

The plugin can be loaded and configured without a producer. With no producer connected, it keeps the filter loaded and remains in no-input/frozen behavior.

## FrameMsgV1

All fields are little-endian and explicitly serialized. Do not rely on compiler struct packing for the wire format.

| Offset | Field | Type | Meaning |
|---:|---|---|---|
| 0 | `magic` | `uint32` | Fixed magic `0x31494F52` (`ROI1`) |
| 4 | `schemaVersion` | `uint16` | `1` for FrameMsgV1 |
| 6 | `headerSize` | `uint16` | `12` |
| 8 | `messageSize` | `uint16` | `68` |
| 10 | `reservedHeader` | `uint16` | Must be `0` in v1 |
| 12 | `streamId` | `uint32` | Stream identity |
| 16 | `epoch` | `uint32` | Producer reset/version epoch |
| 20 | `seq` | `uint64` | Monotonic sequence within `streamId + epoch` |
| 28 | `qpcFrequency` | `uint64` | QPC ticks per second |
| 36 | `tQpc` | `int64` | Planned ROI effective time in QPC ticks |
| 44 | `x` | `float32` | Normalized source-space ROI x |
| 48 | `y` | `float32` | Normalized source-space ROI y |
| 52 | `w` | `float32` | Normalized source-space ROI width |
| 56 | `h` | `float32` | Normalized source-space ROI height |
| 60 | `valid` | `uint8` | `1` if ROI geometry should be consumed |
| 61 | `flags` | `uint8` | Bitset described below |
| 62 | `reserved0` | `uint16` | Reserved |
| 64 | `reserved1` | `uint32` | Reserved |

Total message size is 68 bytes.

## Flags

| Bit | Name | Meaning |
|---:|---|---|
| `0x01` | `FLAG_WARMUP_OR_NOT_ENOUGH_DATA` | Producer is warming up or lacks enough data; usually `valid=0` |
| `0x02` | `FLAG_STRATEGY_INVALID_ROI` | Producer has no valid ROI; usually `valid=0` |
| `0x04` | `FLAG_TQPC_EFFECTIVE_TIME` | `tQpc` is already the planned effective apply time; consumer must not add another timing offset |
| `0x08` | `FLAG_FRAME_STREAM` | Producer sends per-frame ROI; consumer must not apply an extra interpolation layer |
| `0x10` | `FLAG_PRESEND_V2` | Producer may send frames ahead of their effective `tQpc`; consumer buffers and applies them only when due |

Undefined bits are reserved. Consumers ignore unknown bits.

## Consumer Rules

- Reject unsupported `schemaVersion`.
- Reject invalid `magic`, `headerSize`, `messageSize`, or truncated messages.
- Reject non-finite ROI floats.
- Drop non-monotonic `seq` within the same `streamId + epoch`.
- Treat `epoch` changes as stream reset and clear interpolation state.
- Treat `valid=0` messages as keepalive and freeze the last valid ROI.
- Treat producer silence past timeout as timeout freeze.
- `roiApplyOffset` must have one owner only. Current mainline expects the producer to set `FLAG_TQPC_EFFECTIVE_TIME`.

## Boundary

This protocol contains simple POD fields only. It does not carry strategy objects, detection state, product readiness state, profile internals, or proprietary companion implementation details.
