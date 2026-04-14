# AIR Serial Control Protocol for Release 0.03

This document defines the machine-readable serial control protocol exposed by `ESP_AIR`.

Scope:

- serial is a thin AIR control-plane adapter
- serial requests submit to `ESP_AIR/src/control_plane.*`
- serial results mirror the AIR two-phase request/result model
- protocol is intended for PowerShell validation and engineering control, not GUI use

Non-goals:

- no Teensy fast-path redesign
- no schema redesign
- no new replay semantics
- no interface-local business logic

## Ownership

Authoritative backend:

- `ESP_AIR/src/control_plane.h`
- `ESP_AIR/src/control_plane.cpp`

Serial adapter:

- `ESP_AIR/src/serial_control.h`
- `ESP_AIR/src/serial_control.cpp`
- console loop integration in `ESP_AIR/src/main.cpp`

Rule:

- serial decodes request text
- serial submits one AIR control-plane request
- AIR control plane validates mode and arguments
- AIR control plane dispatches to recording, replay, SD, or fusion backend
- serial emits machine-readable immediate and final results
- serial does not directly start logging, start replay, or mutate fusion settings

Implementation note:

- serial control keeps the shared AIR control-plane contract unchanged
- SD/file requests dispatched by the control plane now execute FATFS/VFS work on the dedicated AIR `sd_worker` task rather than on `loopTask`

## Request Format

Each request is one JSON object on one line.

Required fields:

- `req_id`
- `category`
- `action`

Examples:

```json
{"req_id":1,"category":"state","action":"get"}
{"req_id":2,"category":"recording","action":"start"}
{"req_id":3,"category":"recording","action":"stop"}
{"req_id":4,"category":"replay","action":"start_latest"}
{"req_id":5,"category":"replay","action":"start_file","name":"LOG001.TLOG"}
{"req_id":6,"category":"replay","action":"stop"}
{"req_id":7,"category":"fusion","action":"get"}
{"req_id":8,"category":"fusion","action":"set","gain":0.5,"accelerationRejection":10.0,"magneticRejection":10.0,"recoveryTriggerPeriod":5000}
{"req_id":9,"category":"file","action":"storage_status"}
{"req_id":10,"category":"file","action":"list_page","offset":0,"limit":1}
```

## Immediate Result Format

Every request returns one immediate JSON result:

```json
{"type":"control_result","phase":"immediate","req_id":2,"category":"recording","action":"start","status":"accepted","code":"ok","code_id":0}
```

Immediate `status` is one of:

- `accepted`
- `busy`
- `rejected`

If immediate `status` is `busy` or `rejected`, there is no later final result for that `req_id`.

## Final Result Format

Accepted requests return one final JSON result:

```json
{"type":"control_result","phase":"final","req_id":2,"category":"recording","action":"start","status":"completed_ok","code":"ok","code_id":0,"ok":true,"state":{...},"recording":{...}}
```

Final `status` is one of:

- `completed_ok`
- `completed_error`

Every final result includes:

- `req_id`
- `status`
- `code`
- `code_id`
- `state`

The `state` object is the AIR authoritative state snapshot and includes at minimum:

- `mode`
- `request_pending`
- `pending_request_id`
- `recording_active`
- `recording_busy`
- `replay_active`
- `replay_busy`
- `sd_ready`
- `sd_mounted`
- `sd_media_present`
- `time_state`
- `time_source`
- `has_fusion_settings`
- `fusion`
- `last_result_code`
- `last_result_code_id`
- `last_completed_request_id`

Operation-specific payloads appear only when relevant:

- `recording`
- `replay`
- `storage`
- `file_list`
- `fusion`

## Supported Categories and Actions

### `state`

- `get`

### `recording`

- `start`
- `stop`
- `status`

Optional request field for `start`:

- `session_id`

### `replay`

- `start_latest`
- `start_file`
- `start`
  `start` without `name` maps to `start_latest`
  `start` with `name` maps to `start_file`
- `stop`
- `pause`
- `seek_relative`
- `status`

Optional request field:

- `name`
- `delta_records` for `seek_relative`

### `fusion`

- `get`
- `set`

Required `set` fields:

- `gain`
- `accelerationRejection`
- `magneticRejection`
- `recoveryTriggerPeriod`

### `file`

- `storage_status`
- `status`
  `status` is an alias for `storage_status`
- `list_page`
- `list`
  `list` is an alias for `list_page`
- `list_json`
- `mount_media`
- `eject_media`
- `delete`
- `rename`
- `set_record_prefix`

Optional `list_page` fields:

- `offset`
- `limit`

Optional file fields:

- `sort_key` = `name | size | date` for `list_json`
- `sort_dir` = `ascending | descending | asc | desc` for `list_json`
- `name` for `delete`
- `name` and `aux_name` for `rename`
- `prefix` for `set_record_prefix`

## Mode-Gating Rules

Mode gating is enforced only by the AIR control plane.

Current required behavior:

- replay start during recording returns immediate `busy` with `code=busy_recording`
- recording start during replay returns immediate `busy` with `code=busy_replay`
- file-list requests during recording return immediate `busy` with `code=busy_recording`
- file-list requests during replay return immediate `busy` with `code=busy_replay`
- storage status remains readable while recording or replay is active

Replay state rule:

- exported replay state must remain non-idle whenever replay file-open state would still block other requests

Paused replay contract:

- a successful `replay/pause` intentionally clears `replay.active`
- paused replay remains operationally occupied via:
  - `replay.paused = true`
  - `replay.file_open = true`
  - `replay.at_eof = false`
  - state snapshot `replay_state = "busy"`
  - state snapshot `mode = "replay_busy"`
- this means `active` answers "is AIR currently feeding replay records?" and not
  "does replay still own the subsystem?"
- clients must treat `paused=true` plus `file_open=true` as a paused-but-still-open replay session
- tests should prove replay was active before pause is issued, then expect the paused final state above

Fusion get rule:

- `fusion/get` final success means a fresh post-request fusion-settings sample was observed from Teensy
- queueing the request alone is not sufficient for `completed_ok`
- timeout or missing fresh response returns `completed_error`

## Validation Scripts

PowerShell validation scripts using this protocol:

- `scripts/run_air_serial_control_basic.ps1`
- `scripts/run_air_serial_control_mode_gating.ps1`
- `scripts/run_air_serial_control_performance.ps1`

Current usage intent:

- basic functional control validation
- mode-gating validation
- replay-performance comparison with and without serial control activity

The performance script intentionally reuses the existing `tapi replaybench` proof methodology for the validated operating envelope, then injects serial control-plane requests around that benchmark to check for material regression.

## 2026-04-08 Validation Status

The serial protocol is now a valid RC2 proof surface.

Live proof logs:

- `scripts/air_serial_control_basic_20260408_154301.log`
- `scripts/air_serial_control_storage_probe_20260408_154353.log`
- `scripts/air_serial_control_replay_probe_20260408_154405.log`
- `scripts/air_serial_control_mode_gating_20260408_154459.log`
- `scripts/air_serial_control_perf_manual_20260408_155219.log`

Observed functional result:

- basic control checks passed `12/12`
- recording start / stop passed
- replay start-latest / stop passed
- storage status passed
- fusion get / set / readback passed

Observed mode-gating result:

- mode-gating checks passed `3/3`
- recording blocks replay start and file-list requests
- replay blocks recording start and file-list requests

Observed performance result:

- baseline replaybench: `validated_rps=2392.3`
- control-around-run replaybench: `validated_rps=2396.2`
- `replay_rx_occ_max=48` and `replay_rx_free_min=463` remained unchanged
- `rx_overflows=0`, `crc_err=0`, and `type_err=0` remained unchanged

Method rule:

- the valid benchmark method is serial control activity immediately before and after `tapi replaybench`
- same-port in-band JSON injection during `tapi replaybench` is not valid because the benchmark occupies the AIR console command loop until completion

Standalone benchmark rule:

- when `standalone_bench` is enabled, replaybench must skip radio poll/publish activity
- this prevents an invalid re-entry into `radio_link::initEspNow()` while standalone replaybench has WiFi/radio disabled
