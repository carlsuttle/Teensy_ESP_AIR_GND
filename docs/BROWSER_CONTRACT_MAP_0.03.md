# Browser Contract Map for Release 0.03

This file records the served `ESP_GND` browser's current field and control usage against the active `0.03` contracts.

Scope:

- restored later historical layout
- current single `/ws` browser path
- current shaped browser messages only
- no backend ownership in the browser

## Browser Field Usage

### Snapshot fields used directly

| Render use | Snapshot field(s) | Direct/derived | File / function |
|---|---|---|---|
| Header freshness / connection state | `fresh` | direct | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `updateStatus`, `sourceMode`, `renderHeader` |
| Header ping | `radio_rtt_ms` | direct | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `renderHeader` |
| Header D/U badges | `fresh`, `drop`, `state_gap`, `age_ms`, `radio_rtt_ms`, `len_err`, `unknown_msg` | browser-derived scoring from allowed fields | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `downlinkScore`, `uplinkScore`, `renderHeader` |
| Recorder header state | `recording_active` | direct | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `renderAll` |
| PFD attitude | `roll_deg`, `pitch_deg`, `mag_heading_deg`, `yaw_deg` | direct | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `renderPfdPanel` |
| PFD speed / altitude / VSI | `gSpeed_mms`, `baro_alt_m`, `hMSL_mm`, `baro_vsi_mps` | mixed direct + allowed local unit conversion fallback | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `renderPfdPanel` |
| PFD fix / satellites | `gps_fix_type`, `gps_num_sv` | direct | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `renderPfdPanel` |
| PFD course / HSI / lat / lon | `headMot_1e5deg`, `lat_1e7`, `lon_1e7` | browser-derived unit conversion from allowed fields | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `renderPfdPanel` |
| Position tab GPS | `gps_fix_type`, `gps_num_sv`, `lat_1e7`, `lon_1e7`, `gSpeed_mms`, `headMot_1e5deg`, `hAcc_mm`, `sAcc_mms` | browser-derived unit conversion from allowed fields | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `renderGpsPanel` |
| Position tab baro | `baro_alt_m`, `baro_vsi_mps`, `baro_press_hpa`, `baro_temp_c` | direct | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `renderBaroPanel` |
| Attitude tab text | `roll_deg`, `pitch_deg`, `yaw_deg`, `fusion_gain`, `fusion_accel_rej`, `fusion_mag_rej`, `fusion_recovery_period` | direct | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `renderAttPanel` |
| Fusion sliders / lights | `fusion_gain`, `fusion_accel_rej`, `fusion_mag_rej`, `fusion_recovery_period`, `flags` | direct | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `updateFusionUi` |
| Radio tab summary | `schema_id`, `schema_version`, `age_ms`, `radio_rtt_ms`, `state_gap`, `state_rewind`, `drop`, `len_err`, `unknown_msg`, `recording_active`, `replay_active`, `gps_calendar_valid`, `gps_year`, `gps_month`, `gps_day`, `gps_hour`, `gps_min`, `gps_sec` | mixed direct + allowed local formatting | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `renderLinkPanel`, `gpsDateTimeText` |
| Logs tab recording summary | `recording_active`, `recording_busy`, `recording_session_id`, `recording_bytes_written`, `gps_calendar_valid`, `gps_year`, `gps_month`, `gps_day`, `gps_hour`, `gps_min`, `gps_sec` | mixed direct + allowed local formatting | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `renderLogsPanel` |

### Auxiliary browser messages used directly

| Message | Field(s) used | Direct/derived | File / function |
|---|---|---|---|
| `files` | `files[].name`, `files[].size_bytes` | direct | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `renderLogsPanel`, websocket `files` handler |
| `storage` | `known`, `backend_ready`, `busy`, `file_count`, `record_prefix` | direct | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `renderLogsPanel`, websocket `storage` handler |
| `ack` | `req_id`, `op`, `ok`, `code`, `detail` | direct | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), websocket `ack` handler |
| `hello` | `snapshot_hz` | direct | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), websocket `hello` handler |

## Stale Browser Field Usage Removed

Removed in this cleanup:

- snapshot `has_state`
  - no longer emitted by `ESP_GND`
  - no longer used by the browser to infer `live`
- snapshot time-status fields on the periodic browser snapshot:
  - `time_state`
  - `time_source`
  - `gps_calendar_present`
  - `gps_time_valid`
  - `system_time_set`
  - `system_time_utc_s`
  - `time_last_set_age_ms`
  - `time_sync_count`
- nested snapshot `gps_calendar` object
- `files[].mtime_utc_s` in browser-facing file-list JSON

These removals preserve the current browser contract while keeping time-validity on the separate storage-status surface.

## Browser-Originated Actions

| UI action | Current browser request | Classification | File / function |
|---|---|---|---|
| Start Session | `{"type":"control","category":"recording","action":"start","req_id":...}` | valid current control request | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `startLogBtn` click |
| Stop Session | `{"type":"control","category":"recording","action":"stop","req_id":...}` | valid current control request | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `stopLogBtn` click |
| Refresh Log Status button | `{"type":"control","category":"file","action":"refresh","req_id":...}` | valid current file refresh request, historical label retained | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `refreshLogStatusBtn` click |
| Apply Fusion | `{"type":"control","category":"fusion","action":"set","req_id":...,"gain":...,"accelerationRejection":...,"magneticRejection":...,"recoveryTriggerPeriod":...}` | valid current control request | [app.js](/c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js), `fusionApplyBtn` click |

### Legacy helper controls intentionally disabled

The restored layout still shows several historical controls, but they remain disabled and have no browser-side backend logic:

- `Apply Capture Hz`
- `Reset AIR Link`
- `Download Diag CSV`
- `Download WS Event CSV`
- `Download Client Event CSV`
- `Reset Counters`

They are left as visible historical UI debt only and are not active parts of the current browser contract.
