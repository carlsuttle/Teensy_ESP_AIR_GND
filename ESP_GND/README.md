# ESP_GND

Ground-side ESP32 project for the `Teensy_ESP_AIR_GND` split architecture.

## Role

`ESP_GND` is both:
- the phone/tablet-facing Wi-Fi access point and web server
- the ground endpoint of the bidirectional `ESP-NOW` link to `ESP_AIR`

Current responsibilities:
- host the browser UI on `http://192.168.4.1/`
- receive telemetry, fusion settings, ACK/NACK, and link metadata from AIR
- push latest state to the browser on `/ws_state`
- accept browser control on `/ws_ctrl`
- send fusion/rate/reset commands back to AIR

## Network Model

Current network split:
- Browser/device <-> GND: normal Wi-Fi AP + HTTP/WebSocket
- AIR <-> GND: `ESP-NOW`

Current AP settings:
- SSID default: `Telemetry`
- password default: `telemetry`
- AP IP: `192.168.4.1`
- channel: `6`
- DHCP lease range: `192.168.4.50` to `192.168.4.100`

`ESP-NOW` stays in normal mode, not LR mode, so phone/tablet AP compatibility is preserved.

## Current Behavior

Implemented browser/control behavior:
- binary latest-state websocket on `/ws_state`
- control websocket on `/ws_ctrl`
- config endpoint on `/api/config`
- status endpoint on `/api/status`
- diagnostics CSV on `/api/diag`
- websocket event CSV on `/api/ws_events`

Current UI features:
- GPS, Attitude, Baro, and Radio tabs
- live fusion control sliders
- AIR link freshness and MAC display
- AIR recorder state display
- approximate AIR-side view of GND AP RSSI
- AIR link reset button

Current transport behavior:
- bidirectional `ESP-NOW` command/data link
- discovery/relink using `LINK_HELLO`
- command ACK/NACK preserved
- AIR reset and GND reset recovery verified on bench

Current limitations:
- file listing/download/delete APIs are still placeholders
- authoritative file logging on AIR remains disabled in firmware
- AIR RSSI shown in the UI is an approximation from AIR AP scanning, not native per-packet `ESP-NOW` RSSI

## Serial Console

USB serial provides GND bench commands.

Current GND commands:
- `help`
- `kickair`
- `resetair`
- `relink`
- `seelink`
- `stats`
- `x`

Useful boot/status lines:
- `GND READY ap ip=... channel=6 ...`
- `GND WAIT air_packets target=...`
- `GND READY air_link sender=...`
- `SET_FUSION tx_ok=... gain=... accRej=... magRej=... rec=...`

## Build And Upload

From this folder:

```powershell
pio run
pio run -t upload
pio run -t uploadfs
pio device monitor -b 115200
```

Run `uploadfs` whenever browser assets in `data/` change.

## Bench Notes

Observed on the current bench setup:
- full cold starts recover cleanly
- AIR power cycles recover cleanly
- GND power cycles recover cleanly
- short GND outages let the browser resume automatically
- longer GND outages may require the phone/tablet to rejoin the AP, after which data resumes immediately
- repeated reflashes and resets have shown no bad transport behavior

## Related Projects

- [../ESP_AIR](../ESP_AIR)
- [../Teensy](../Teensy)
