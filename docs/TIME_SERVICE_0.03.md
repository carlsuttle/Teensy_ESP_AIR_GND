# ESP_AIR GPS-Derived UTC Time Service for Release 0.03

This document defines the ESP_AIR wall-clock service used for filesystem metadata and browser-visible time validity.

## Ownership

- ESP_AIR owns wall-clock UTC for SD/file metadata.
- Teensy only provides GPS-derived calendar fields through the active telemetry schema.
- GND and browser consume AIR-reported status; they do not infer filesystem time validity themselves.

## Separation of Time Types

- Monotonic/internal timing remains the source for replay pacing, freshness, queue timing, and scheduling.
- Wall-clock UTC is used only for:
  - filesystem timestamps
  - file-list metadata display
  - browser-visible time-validity status

No timezone or DST offset is applied. All wall-clock values remain UTC.

## Time States

- `unset`
  - no trusted wall clock has been established
- `gps_tentative`
  - GPS calendar is present/plausible but not yet trusted enough to set system time
- `gps_valid`
  - system time has been set from trusted GPS UTC
- `holdover`
  - GPS was previously trusted, but live GPS time is currently unavailable; ESP32 clock continues running

## Trust Rule

GPS time becomes trusted only when all of the following are true:

- `fixType >= 3`
- GPS calendar fields are present
- date/time fields are sane
- year is within `2024..2099`
- at least `3` consecutive plausible updates are observed

Plausible consecutive updates may repeat the same second or move by one second, which matches the current telemetry cadence.

Replay-derived state (`kStateFlagReplayOutput`) does not drive the ESP_AIR wall clock.

## Set / Resync Policy

- first trusted GPS UTC sample sequence sets ESP32 system time with `settimeofday(...)`
- once valid, the service does not resync more often than every `60 s`
- resync happens only when the observed error is at least `2 s`

This keeps filesystem time stable and avoids noisy per-packet clock resets.

## Holdover Policy

If GPS time disappears after a valid set:

- ESP_AIR does not clear the system clock
- state transitions to `holdover`
- filesystem timestamps continue using ESP32 wall-clock time
- browser status reports holdover explicitly

## Browser / Status Fields

The live AIR -> GND status path exposes:

- `time_state`
- `time_source`
- `gps_calendar_present`
- `gps_time_valid`
- `system_time_set`
- `system_time_utc_s`
- `time_last_set_age_ms`
- `time_sync_count`

The storage-status path exposes the same time-validity fields for SD/UI inspection.

## File Metadata

Managed file listings now include:

- `name`
- `size_bytes`
- `mtime_utc_s`

`mtime_utc_s` is taken from ESP_AIR filesystem metadata (`File::getLastWrite()`), not inferred from filenames.

## Startup / Recording Policy

- no GPS lock is required for startup
- recording remains allowed before GPS-valid state exists
- file operations remain available subject to the SD API mode rules
- before GPS-valid, timestamps may be placeholder or otherwise untrusted
- active files are not renamed when GPS later becomes valid

## Diagnostics

ESP_AIR emits lightweight transition diagnostics only for:

- first GPS calendar seen
- GPS-valid initial sync
- periodic resync
- entry to holdover
- regain from holdover
- rejection of a recent wild GPS jump

Console helpers:

- `timestat`
- `timeselftest`

## Current Limits

- second-level UTC resolution only
- no RTC, NTP, timezone, or DST support
- no live user-settable wall clock in this phase
