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
