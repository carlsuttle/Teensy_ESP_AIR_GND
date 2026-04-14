# AIR to GND ESP-NOW Protocol for Release 0.03

This document defines the AIR-originated ESP-NOW radio protocol consumed by `ESP_GND`.

Scope:

- `ESP_AIR` owns the authoritative source and control state
- `ESP_GND` decodes AIR packets, tracks freshness, and reshapes a reduced browser snapshot
- live and replay use the same AIR -> GND instrumentation packet shape
- browser JSON remains a separate protocol layered on top of the GND decoder

Non-goals:

- no schema-v2 redesign
- no Teensy fast-path redesign
- no browser protocol redesign
- no GND-owned mode gating or backend business logic

## Ownership

Authoritative producer:

- `ESP_AIR/src/radio_link.cpp`
- `ESP_AIR/src/types_shared.h`

Thin receiver/decoder:

- `ESP_GND/src/radio_link.cpp`
- `ESP_GND/src/radio_link.h`

Rule:

- AIR decides what mode the system is in
- AIR decides whether the source is live or replay
- AIR decides recording/replay/storage/fusion state
- GND decodes and forwards what AIR reports
- GND must not infer backend state from packet timing or browser requests alone

## Frame Envelope

Every ESP-NOW frame begins with `telem::FrameHeader`.

Header fields:

- `magic`
  - must equal `telem::kMagic`
- `version`
  - current packet version is `telem::kVersion`
- `msg_type`
  - one of the AIR -> GND telemetry/control-result packet types defined below
- `payload_len`
  - payload bytes following the header
- `reserved`
  - transmit as zero in this phase
- `seq`
  - AIR-originated frame sequence for this packet stream
- `t_us`
  - AIR monotonic transmit timestamp for this frame

GND receive rules:

- reject frames with wrong `magic`
- reject frames with wrong `version`
- reject frames whose `payload_len` does not match the packet type
- track sequence gap and rewind at the AIR frame level
- do not reinterpret `seq` as record/replay ordering metadata

## AIR -> GND Packet Types

Current AIR -> GND packet types sent over ESP-NOW:

- `TELEM_UNIFIED_DOWNLINK`
- `TELEM_REPLAY_STATUS`
- `TELEM_STORAGE_STATUS`
- `TELEM_LOG_FILE_LIST`
- `TELEM_CONTROL_STATUS`
- `TELEM_META`
- `TELEM_FUSION_SETTINGS`
- `ACK`
- `NACK`
- `LINK_HELLO`

The required `0.03` receive path should treat these as two logical groups:

1. AIR instrumentation/state stream
2. AIR control/result sideband

### 1. AIR instrumentation/state stream

Primary packet:

- `TELEM_UNIFIED_DOWNLINK`

Payload layout:

- `telem::UnifiedDownlinkBaseV1`
- optional `telem::DownlinkGpsStateV1`
- optional `telem::DownlinkExtendedStateV2`
- optional `telem::DownlinkStatusV1`

Presence is controlled by `section_flags`:

- `kUnifiedDownlinkFlagHasGps`
- `kUnifiedDownlinkFlagHasStatus`
- `kUnifiedDownlinkFlagHasExtended`

Authoritative AIR-owned instrumentation fields include:

- fast attitude/baro section from `DownlinkFastStateV1`
- GPS/navigation section from `DownlinkGpsStateV1`
- fusion/raw-present/time-adjacent fields from `DownlinkExtendedStateV2`
- recording/live status section from `DownlinkStatusV1`

Live/replay rule:

- `TELEM_UNIFIED_DOWNLINK` is the same packet shape for live and replay output
- replay origin is indicated by AIR-owned state, especially `kStateFlagReplayOutput`
- GND must not invent a separate replay instrumentation schema

### 2. AIR control/result sideband

Auxiliary AIR -> GND packets:

- `TELEM_REPLAY_STATUS`
- `TELEM_STORAGE_STATUS`
- `TELEM_LOG_FILE_LIST`
- `TELEM_CONTROL_STATUS`
- `TELEM_FUSION_SETTINGS`
- `ACK`
- `NACK`
- `LINK_HELLO`

These packets are AIR-authored status/result publications and must be treated as backend truth, not as hints for GND-local behavior.

## Source Mode

AIR is authoritative for source mode.

Canonical source mode values for GND-facing logic:

- `idle`
- `live`
- `replay`

Mapping rule in this phase:

- `replay` when AIR reports active replay state or `kStateFlagReplayOutput`
- `live` when state packets are fresh and replay is not active
- `idle` when neither replay nor fresh live state is currently available

Important:

- this mapping is documentation for the decoded snapshot only
- GND must not use source mode to allow or deny AIR control requests
- GND must not infer replay completion or logging safety from freshness alone

## Sequence, Timestamp, and Freshness Semantics

There are three distinct timing/ordering concepts.

### Frame transport sequence

- `FrameHeader.seq`
- owned by AIR radio transport
- used by GND for packet freshness diagnostics, gap detection, and rewind detection

### Frame transport timestamp

- `FrameHeader.t_us`
- AIR monotonic timestamp for the transmitted frame
- used by GND to compute age/freshness

### Source snapshot sequence

- `UnifiedDownlinkBaseV1.source_seq`
- sequence of the underlying AIR source snapshot
- may remain meaningful across different radio publish rates

GND freshness responsibilities:

- retain latest valid decoded snapshot
- mark it stale when age exceeds the local freshness threshold
- compute browser-facing `fresh` and `age_ms`

GND must not:

- reinterpret stale data as a mode change
- assume missing packets imply replay stop, record stop, or SD failure

## Instrumentation Payload Fields

The AIR -> GND instrumentation subset is intentionally smaller than the full engineering schema.

Canonical field contract is shared with:

- `docs/STAGE2_FIELD_CONTRACT_0.03.md`

This radio protocol carries the instrumentation subset only, not full record/replay/log payloads.

Required field groups:

- attitude and baro state
- GPS navigation state
- fusion configuration/status
- recording/live status
- source sequence and transport timestamp

Explicit exclusions from the periodic instrumentation packet:

- full raw sensor vectors
- file list payloads
- storage inventory
- browser-only derived fields
- AIR control-plane request/result registers

## Link Diagnostics

GND may surface diagnostics derived from radio receipt, including:

- sequence gap
- sequence rewind
- last receive age
- stale/fresh
- unknown message count
- length error count

AIR may also send diagnostics payloads in:

- `TELEM_CONTROL_STATUS`
- `TELEM_META`
- `LINK_HELLO`

Rule:

- diagnostics are observational only
- they do not authorize GND to mutate AIR state or enforce backend policy

## GND Receive Responsibilities

On receipt of a valid AIR packet, GND must:

1. validate `FrameHeader`
2. validate packet type and payload length
3. decode only the packet fields defined for that type
4. update the latest decoded snapshot or sideband cache
5. update freshness/stale tracking
6. expose a reduced browser snapshot or auxiliary browser message

Current browser-facing shaping remains a GND responsibility.

## GND Must Not Infer Or Own

GND must not:

- decide whether recording may start
- decide whether replay may start
- decide whether file operations are safe
- treat stale telemetry as permission to remount/eject/delete
- reinterpret instrumentation packets as full schema-v2 records
- invent local replay/source selection behavior

## Compatibility Rule

The AIR -> GND ESP-NOW protocol is not the browser protocol.

Separation rule:

- AIR -> GND radio packets are binary, schema-bound transport messages
- GND -> browser messages are reduced JSON snapshots and control/result reshaping
- browser field names and browser cadence must not define AIR radio packet structure

## Current Thin-Adapter Target

The intended `0.03` GND adapter responsibilities are:

- decode `TELEM_UNIFIED_DOWNLINK`
- decode AIR-owned control/result sideband packets
- track freshness and stale state
- expose reduced browser state
- forward browser-originated AIR control requests without taking ownership of backend rules

Anything beyond that should be treated as a temporary legacy helper surface, not as a new ownership boundary.
