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

The AIR logging pipeline still exists, but file logging is currently disabled in firmware.

Current state:
- `log_store` code remains in place
- recorder state is exposed to GND/web UI
- `kEnableAirFileLogging` is currently `false`
- start/stop/status commands already flow through the radio link for later SD-card integration

Reason:
- there is still no finished quota/rotation/download workflow for onboard log files

## Serial Console

USB serial provides bench commands.

Current AIR commands:
- `help`
- `getfusion`
- `kickteensy`
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
