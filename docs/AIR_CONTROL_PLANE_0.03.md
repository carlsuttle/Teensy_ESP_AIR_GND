# ESP_AIR Control Plane for Release 0.03

This document defines the `0.03` AIR-owned control register and control-manager model.

Intent:

- make `ESP_AIR` the authoritative owner of control state and control results
- keep serial, ESP-NOW, and websocket adapters thin
- route SD/file operations through the SD API
- centralize mode gating and result reporting for recording, replay, file, and fusion control

Non-goals:

- no schema redesign
- no Teensy fast-path redesign
- no replay timing redesign
- no live schema switching

## Ownership

Authoritative control plane:

- `ESP_AIR/src/control_plane.h`
- `ESP_AIR/src/control_plane.cpp`

Adapters that should consume it:

- AIR radio command handler in `ESP_AIR/src/radio_link.cpp`
- AIR serial/console command handler in `ESP_AIR/src/main.cpp`
- future websocket or other transport-facing handlers on AIR

Underlying business backends owned elsewhere:

- SD/file backend: `ESP_AIR/src/sd_file_api.*`
- logging backend: `ESP_AIR/src/log_store.*`
- replay backend: `ESP_AIR/src/replay_bridge.*`
- Teensy command transport / fusion settings: `ESP_AIR/src/teensy_link.*`
- time-validity state: `ESP_AIR/src/time_service.*`

Rule:

- transport adapters submit requests
- the control plane dispatches to backends
- adapters return control-plane results
- adapters do not directly delete files, start replay, or mutate fusion settings once migrated

## Request / Result Protocol

The `0.03` control plane now follows a two-phase request/result model:

1. immediate request disposition
2. final completion result for accepted requests only

Immediate disposition is one of:

- `accepted`
- `busy`
- `rejected`

Final completion is one of:

- `completed_ok`
- `completed_error`

Rule:

- if a request is `busy` or `rejected`, there is no later completion result
- if a request is `accepted`, a final completion result must exist

Current implementation note:

- execution is still synchronous inside AIR in this phase
- the control plane still records the immediate and final phases separately so adapters do not collapse acceptance and completion semantics
- only one pending request is supported at a time

## Register Model

### State Registers

Authoritative readable state snapshot:

- `request_pending`
- `pending_request_id`
- `pending_source`
- `pending_category`
- `pending_action`
- `system_mode`
- `last_request_id`
- `last_completed_request_id`
- `last_result_code`
- `last_source`
- `recording_requested`
- `recording_session_id`
- `recording_last_command`
- `recording_last_change_ms`
- `recording_state`
- `replay_state`
- `replay_file_open`
- `replay_paused`
- `selected_file`
- `sd_ready`
- `sd_mounted`
- `sd_media_present`
- `file_list_state`
- `file_list_valid`
- `file_list_generation`
- `fusion_settings_current`
- `time_state`
- `time_source`
- `time_flags`

### Request Registers

Each request carries:

- `request_id`
- `source`
- `category`
- `action`
- `command_id`
- bounded arguments/payload
- optional transport provenance:
  - `seq`
  - `t_us`
  - `apply_flags`

### Result Registers

Each completed request returns:

- `request_id`
- immediate:
  - `disposition`
  - `disposition_code`
- final:
  - `completion`
  - `completion_code`
- compatibility fields:
  - `accepted`
  - `completed`
  - `ok`
  - `code`
- optional payloads:
  - log status
  - replay status
  - storage status
  - file-list page
  - fusion settings

## Categories and Actions

### Recording

- `record_start`
- `record_stop`
- `record_status`

Backend:

- `log_store`

### Replay

- `replay_start_latest`
- `replay_start_file`
- `replay_stop`
- `replay_pause`
- `replay_seek_relative`
- `replay_status`

Backend:

- `replay_bridge`

### File / SD

- `file_list_page`
- `storage_status`
- `mount_media`
- `eject_media`
- `delete_file`
- `rename_file`
- `export_csv`
- `set_record_prefix`

Backend:

- `sd_file_api`

### Fusion

- `fusion_set`
- `fusion_get`

Backend:

- `teensy_link`

## Result-Code Policy

The control plane uses explicit AIR control/result codes.

Current defined meanings include:

- `ok`
- `busy_request`
- `busy_recording`
- `busy_replay`
- `sd_not_ready`
- `invalid_argument`
- `not_supported`
- `file_not_found`
- `internal_error`
- `backend_failed`

Implementation note:

- SD/file completion results reuse the existing AIR/SD numeric status codes where possible
- `busy_request` is AIR-control-plane specific
- adapters should treat `disposition_code` as the reason for `busy` or `rejected`
- adapters should treat `completion_code` as the reason for `completed_error`

## Mode Policy

Mode gating lives in AIR backend logic, not in adapters.

Current policy:

- SD/file mode inhibition comes from `sd_file_api`
- replay backend owns replay start/stop/pause/seek acceptance
- logging backend owns record start/stop acceptance
- fusion set/get is owned by Teensy command transport

The control plane is responsible for:

- routing to the correct backend
- exposing the result consistently
- surfacing inhibition/rejection codes
- rejecting incompatible requests before execution when the mode rule is already known

## Adapter Responsibilities

Thin adapter responsibilities only:

1. parse request
2. populate control-plane request registers
3. submit request to the AIR control plane
4. emit immediate disposition
5. if disposition is `accepted`, emit final completion result
6. emit transport-specific payloads tied to the completed request

Explicitly out of scope for adapters after migration:

- direct `log_store::startSession/stopSession`
- direct `replay_bridge::start/stop/pause/seek`
- direct `sd_file_api::*`
- direct `teensy_link::sendSetFusionSettings` for user-facing fusion control

## Current Migration Target

This phase migrates:

- AIR radio command handler
- AIR main console commands for:
  - recording
  - replay
  - file / SD
  - fusion

This phase does not require migration of:

- benchmark helpers
- replay proof scripts
- low-level `teensy_api` helper internals
- standalone internal test flows

Those may remain direct temporarily and should be documented as remaining direct paths.

## Known Limits

- dispatch is synchronous in this phase
- only one pending request is supported
- there is one last-result register, not a multi-request history
- internal benchmark/test helpers may still bypass the control plane until migrated deliberately

## Current 2026-04-02 Migration Status

Implemented through the shared AIR control plane now:

- AIR radio command handling for:
  - recording start / stop / status
  - replay start / stop / pause / seek / status
  - file list / storage status / mount / eject / delete / rename / export / record prefix
  - fusion get / set
- AIR console commands for:
  - `sdmount`
  - `sdeject`
  - `sdstate`
  - `sdrename`
  - `sddelete`
  - `logstart`
  - `logstartid`
  - `logstop`
  - `logprefix`
  - `csvfile`

Still direct by design in this phase:

- AIR console proof/benchmark helpers
- AIR console replay-analysis helpers such as replay-capture/compare flows
- AIR console `logfiles` text dump
- low-level `teensy_api` proof surface and capture-setting helpers
- stream-rate and radio-mode transport controls in `radio_link.cpp`

These remaining direct paths are intentionally left for a later focused migration so this refactor does not disturb the validated replay/logging path.

## Extension Rule

Any new user-facing control surface should:

1. define a control-plane action first
2. route through the AIR control plane
3. use SD API for file operations
4. avoid interface-local mode logic
