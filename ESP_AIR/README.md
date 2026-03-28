# ESP_AIR

Aircraft-side ESP32 project for the `Teensy_ESP_AIR_GND` split architecture.

## Role

`ESP_AIR` is the flight-side bridge between the Teensy avionics processor and the ground unit.

Current responsibilities:
- ingest Teensy telemetry and ACK/NACK traffic over UART
- forward telemetry and control/status to `ESP_GND` over bidirectional `ESP-NOW`
- receive commands from `ESP_GND` and relay them to the Teensy
- publish AIR link metadata such as recorder state and approximate GND AP RSSI

`ESP_AIR` is no longer the phone-facing web server.

## Current Behavior

Implemented transport behavior:
- `WiFi` runs in `WIFI_STA` mode on channel `6`
- peer discovery uses `LINK_HELLO`
- telemetry/control transport is `ESP-NOW`
- command flow is bidirectional
- normal mixed mode sends only 2 AIR->GND packet classes:
  - telemetry state at the configured downlink rate
  - combined control/status at `2 Hz`
- stress testing can still switch AIR into state-only mode for rate characterization

Implemented UART/bridge behavior:
- UART bridge uses `UART1`
- RX pin `3`, TX pin `4`
- baud `921600`
- Teensy telemetry is treated as the source of truth for state and fusion settings
- Teensy capture/source rate is independent of AIR->GND download rate

Implemented metadata behavior:
- AIR reports whether radio is ready
- AIR reports whether a GND peer is known
- AIR reports recorder enabled/disabled
- AIR samples approximate GND AP RSSI by scanning for the configured AP SSID on channel `6`

Current defaults:
- AP SSID hint: `Telemetry`
- AP password hint: `telemetry`
- capture/source rate: `50 Hz`
- download/radio telemetry rate: `30 Hz`
- mixed-mode control/status rate: `2 Hz`
- stress mode: off

## Logging

The AIR logger is now integrated with a shared microSD backend.

Current state:
- `log_store` writes binary `.tlog` sessions to microSD instead of `LittleFS`
- the AIR logger, `sdprobe`, `sdwrite`, and `sdcap1m` all use the same shared SD backend
- recorder state is exposed to GND through the radio link with real:
  - active/inactive state
  - backend/media presence
  - session id
  - bytes written
  - free bytes
- file logging is still disabled by default at boot because there is still no finished quota/rotation/download workflow
- `kEnableAirFileLogging` is currently `false`

Shared SD backend details:
- SPI pins use raw GPIO numbers:
  - `CS=2`
  - `SCK=7`
  - `MISO=8`
  - `MOSI=9`
- SD init tries `40 MHz` first
- SD init falls back to `26 MHz` if `40 MHz` does not mount

AIR logger details:
- one file per session under `/logs`
- block-based writer instead of per-record filesystem writes
- current writer block size: `10000` bytes
- current block count: `4`
- writer task is pinned to core `0`
- start/stop/status commands work both from GND and from the AIR USB console

## Serial Console

USB serial provides bench commands.

Current AIR commands:
- `help`
- `tapi help`
- `getfusion`
- `kickteensy`
- `sdprobe`
- `sdwrite`
- `base1m`
- `sdcap1m`
- `sdcapstop`
- `sdcapstat`
- `logstart`
- `logstop`
- `logstat`
- `resendrate`
- `tx1`
- `linkclear`
- `linkopen`
- `wifidrop`
- `wifioffon`
- `relink`
- `resetnet`
- `setfusion <gain> <accelRej> <magRej> <recovery>`
- `stats`
- `x`

Stable Teensy API proof commands:
- `tapi status`
- `tapi getfusion`
- `tapi setcap <hz>`
- `tapi setstream <ws_hz> [log_hz]`
- `tapi setfusion <gain> <accelRej> <magRej> <recovery>`
- `tapi carry [count]`
- `tapi carrycsv [duration_ms] [window]`
- `tapi replaybench [duration_ms] [batch_hz] [records_per_batch]`
- `tapi selftest [hz] [count]`

The `tapi` command family is backed by:
- [teensy_api.h](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\teensy_api.h)
- [teensy_api.cpp](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\teensy_api.cpp)

These wrappers provide a stable proof surface over the lower-level Teensy transport and are intended for:
- control-path verification
- carry-through verification
- sequence/timestamp integrity checks
- automated regression scripts

Current default:
- Teensy standby serial mode defaults to `quiet on`
- this keeps the Teensy USB console quiet unless a bench operator explicitly turns it back on

Useful boot/status lines:
- `AIR READY radio channel=6`
- `AIR READY teensy_link ...`
- `AIR READY gnd_link peer=...`
- `AIR INFO recorder=off`

Useful SD bench lines:
- `SDPROBE ...`
- `SDCAP ...`
- `AIRLOG ...`

Useful SD bench flow:
1. `sdprobe`
2. `sdwrite`
3. `logstart`
4. let telemetry run
5. `logstat`
6. `logstop`
7. `logstat`

## Teensy API Exerciser

The PowerShell exerciser for the new Teensy API proof surface is:

- [run_teensy_api_exerciser.ps1](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_exerciser.ps1)

Run it from PowerShell with:

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_exerciser.ps1 -ComPort COM7
```

Expected exercised path:
1. `bench on`
2. `quiet on`
3. `tapi status`
4. `tapi getfusion`
5. `tapi setcap 1600`
6. `tapi setstream 1600 1600`
7. `tapi carry 8`
8. `tapi carrycsv 2000 16`
9. `tapi selftest 1600 8`

Expected final result:
- `FINAL SUMMARY passed=9 failed=0 port=COM7`

One note:
- the exerciser now forces `bench on` before `quiet on`, so radio/web activity is disabled during the proof run
- if `AIRTX ...` chatter appears before `quiet on` lands, that is expected; the exerciser drains startup serial and then quiets the console before running the proof steps.

## Replay Benchmark

Replay benchmark script:

- [run_teensy_api_replay_benchmark.ps1](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_replay_benchmark.ps1)

Run command:

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_replay_benchmark.ps1 -AirComPort COM7 -TeensyComPort COM10
```

The replay benchmark script forces:
- `bench on`
- then `quiet on`

Expected AIR bench response:
- `BENCH standalone=1 radio=0 wifi=off`

How to decode the benchmark:
- each replay/source record is `160 bytes`
- `batch_hz` is the batch rate
- `records_per_batch` is the number of `160-byte` records sent per batch
- nominal records/s:
  - `batch_hz * records_per_batch`

Examples:
- `100 Hz x 24`
  - `24` records every `10 ms`
  - nominally `2400 records/s`
- `50 Hz x 48`
  - `48` records every `20 ms`
  - nominally `2400 records/s`

Pass criteria:
- AIR:
  - `ok=1`
  - `fail=0`
  - `timeout=0`
- Teensy:
  - no RX overflow / CRC / type errors
  - `outq_max < 64`
  - `replay_rx_free_min > 0`
  - `state_tx_free_min > 0`

Measured soak-proven maxima:
- `100 Hz x 24`
  - about `2399.3 validated records/s`
  - `outq_max=48/64`
  - `replay_rx_occ_max=48/511`
  - `state_tx_occ_max=253/511`
- `50 Hz x 48`
  - about `2399.3 validated records/s`
  - `outq_max=48/64`
  - `replay_rx_occ_max=48/511`
  - `state_tx_occ_max=80/511`

Interpretation:
- both settings reach essentially the same maximum validated bidirectional replay rate
- `50 Hz x 48` keeps the same throughput with half the batch cadence
- if `20 ms` transport latency is acceptable, `50 Hz x 48` is a strong operating point because it reduces transaction rate while keeping performance

## Build And Upload

From this folder:

```powershell
pio run
pio run -t upload
pio device monitor -b 115200
```

`ESP_AIR/data/` currently contains no active web assets, so there is normally no reason to run `uploadfs` here.

## Bench Notes

Observed on the current bench setup:
- cold starts recover without manual intervention
- AIR power cycles relink cleanly to GND
- GND power cycles relink cleanly once the AP returns
- repeated reflashes/resets have not shown bad transport behavior
- normal mixed mode is intended for flight use
- state-only mode is retained as a bench/stress tool

## Related Projects

- [../Teensy](../Teensy)
- [../ESP_GND](../ESP_GND)
