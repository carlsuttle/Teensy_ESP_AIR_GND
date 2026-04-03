# Stage 2 Field Contract for Release 0.03

This file defines the branch-local field contract for:

- the authoritative engineering schema
- the AIR -> GND live instrumentation subset
- the browser snapshot fields
- the browser-originated control message set

Intent:

- preserve the full engineering schema for record, replay, logging, and analysis
- reduce unnecessary ESP-NOW payload for the current simple GUI scope
- keep browser fields and browser controls narrow and explicit

Non-goals:

- no live schema switching
- no change to schema-v2 record/replay meaning
- no change to the Teensy fast path
- no multi-client expansion

## Layering

1. Engineering schema

- authoritative `TelemetryStateRecord`, replay records, log metadata, and command payloads
- used for logging, replay, deterministic analysis, and proof work

2. Instrumentation subset

- maintained AIR -> GND projection of the engineering schema
- used for live GUI display, freshness/stale handling, and a few link diagnostics

3. Browser snapshot

- GND-derived JSON view at `10 Hz`
- reduced display payload for the current simple GUI

4. Browser control

- narrow websocket control envelope
- limited to `recording`, `file`, and `fusion`
- replay remains read-only in the browser for this branch stage

## Engineering Field Ownership

Fields kept authoritative in `TelemetryStateRecord`:

| Field | Engineering | Instrumentation | Browser | Canonical | Main use |
|---|---|---:|---:|---|---|
| `roll_deg`, `pitch_deg`, `yaw_deg`, `mag_heading_deg` | yes | yes | yes | canonical | live attitude display and replay/logging |
| `iTOW_ms`, `fixType`, `numSV` | yes | yes | yes | canonical | live GPS state and replay/logging |
| `lat_1e7`, `lon_1e7` | yes | yes | yes | canonical | canonical live position form |
| `hMSL_mm`, `gSpeed_mms`, `headMot_1e5deg` | yes | yes | yes | canonical | canonical live nav form |
| `hAcc_mm`, `sAcc_mms` | yes | yes | yes | canonical | GPS quality |
| `gps_parse_errors` | yes | no | no | canonical | engineering/debug only |
| `mirror_tx_ok`, `mirror_drop_count` | yes | no | no | canonical | transport/debug only |
| `last_gps_ms`, `last_imu_ms`, `last_baro_ms` | yes | yes | no | canonical | instrumentation freshness/inclusion logic |
| `baro_temp_c`, `baro_press_hpa`, `baro_alt_m`, `baro_vsi_mps` | yes | yes | yes | canonical | live baro display |
| `fusion_gain`, `fusion_accel_rej`, `fusion_mag_rej`, `fusion_recovery_period` | yes | yes | yes | canonical | fusion status/control |
| `flags` | yes | yes | yes | canonical | state/fusion status |
| `accel_*_mps2`, `gyro_*_dps`, `mag_*_uT` | yes | no | no | canonical | engineering analysis/logging only for current GUI scope |
| `raw_present_mask` | yes | yes | yes | canonical | sensor availability/status |
| `gps_year/month/day/hour/min/sec` | yes in v2 | yes | yes | canonical | GPS calendar/time |

Derived or redundant values that should not be shipped live:

| Derived value | Canonical source | Keep live? | Reason |
|---|---|---:|---|
| `lat_deg`, `lon_deg` | `lat_1e7`, `lon_1e7` | no | derive on browser |
| GPS `alt_m` | `hMSL_mm` | no | derive on browser |
| `groundspeed_mps` | `gSpeed_mms` | no | derive on browser |
| `course_deg` | `headMot_1e5deg` | no | derive on browser |
| `has_fix` | `fixType` | no | derive on browser |

## Instrumentation Subset

Current cleaned AIR -> GND instrumentation sections:

1. `DownlinkFastStateV1`

- always included
- attitude, baro, `flags`, and IMU/baro freshness timestamps

2. `DownlinkGpsStateV1`

- GPS/nav subset on the GPS cadence
- includes `last_gps_ms`

3. `DownlinkExtendedStateV2`

- fusion settings
- `raw_present_mask`
- GPS calendar/time

4. `DownlinkStatusV1`

- live recording status only:
  - `log_flags`
  - `log_session_id`
  - `log_bytes_written`

Explicit instrumentation exclusions:

- raw accel/gyro/mag vectors
- `gps_parse_errors`
- `mirror_tx_ok`
- `mirror_drop_count`
- full `ControlStatusPayloadV1`
- file lists and storage inventory

## Browser Snapshot Fields

The simple GUI snapshot should expose only:

Link / diagnostics:

- `schema_id`
- `schema_version`
- `ws_seq`
- `seq`
- `source_t_us`
- `replay_source_seq`
- `replay_source_t_us`
- `fresh`
- `age_ms`
- `radio_rtt_ms`
- `drop`
- `len_err`
- `unknown_msg`
- `state_gap`
- `state_rewind`

Replay status:

- `replay_active`
- `replay_paused`
- `replay_file_open`
- `replay_at_eof`
- `replay_teensy_seen`
- `replay_session_id`
- `replay_records_sent`
- `replay_records_total`
- `replay_last_error`
- `replay_last_command`
- `replay_current_file`

Recording status:

- `recording_active`
- `recording_busy`
- `recording_session_id`
- `recording_bytes_written`

Navigation:

- `gps_itow_ms`
- `gps_fix_type`
- `gps_num_sv`
- `lat_1e7`
- `lon_1e7`
- `hMSL_mm`
- `gSpeed_mms`
- `headMot_1e5deg`
- `hAcc_mm`
- `sAcc_mms`
- `gps_calendar_valid`
- `gps_year`
- `gps_month`
- `gps_day`
- `gps_hour`
- `gps_min`
- `gps_sec`

Attitude / baro / fusion:

- `roll_deg`
- `pitch_deg`
- `yaw_deg`
- `mag_heading_deg`
- `baro_temp_c`
- `baro_press_hpa`
- `baro_alt_m`
- `baro_vsi_mps`
- `fusion_gain`
- `fusion_accel_rej`
- `fusion_mag_rej`
- `fusion_recovery_period`
- `flags`
- `raw_present_mask`

Explicit browser snapshot exclusions:

- raw sensor vectors
- mirror counters
- per-sensor `last_*_ms`
- GPS parse error counters
- redundant float/degree convenience fields that can be derived locally
- file and storage listings inside the `10 Hz` snapshot

Auxiliary browser messages outside the periodic snapshot:

- `hello`
- `ack`
- `files`
- `storage`

## Browser Control Message Set

Browser websocket control messages use one explicit envelope:

```json
{
  "type": "control",
  "req_id": 1,
  "category": "recording|file|fusion",
  "action": "..."
}
```

Supported categories:

### `recording`

```json
{"type":"control","req_id":1,"category":"recording","action":"start"}
{"type":"control","req_id":2,"category":"recording","action":"stop"}
```

### `file`

```json
{"type":"control","req_id":3,"category":"file","action":"refresh"}
{"type":"control","req_id":4,"category":"file","action":"delete","name":"LOG001.BIN"}
{"type":"control","req_id":5,"category":"file","action":"export_csv","name":"LOG001.BIN"}
```

### `fusion`

```json
{
  "type":"control",
  "req_id":6,
  "category":"fusion",
  "action":"set",
  "gain":0.5,
  "accelerationRejection":10.0,
  "magneticRejection":10.0,
  "recoveryTriggerPeriod":5000
}
```

Out of scope for the browser control surface in this cleanup pass:

- replay controls
- radio mode controls
- storage mount/eject
- record-prefix editing
- schema selection

Current mode restrictions formalized for validation:

- browser replay start/stop is unavailable by design in the current websocket control contract
- file delete/export are expected to fail cleanly while recording is active because the logger owns the SD path
- storage/media-changing operations are backend-owned and remain outside the browser control surface for this pass

## Before / After Live Payload

Before cleanup:

- fast section: `50 B`
- GPS section: `42 B`
- extended section: `61 B`
- control section: `64 B`
- max unified payload: `217 B`

After cleanup:

- fast section: `50 B`
- GPS section: `42 B`
- extended section: `24 B`
- status section: `12 B`
- max unified payload: `128 B`

Net reduction at the largest unified payload:

- about `89 B`
- about `41%` smaller than the pre-cleanup live payload
