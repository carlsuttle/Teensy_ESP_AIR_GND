# ESP_GND

Ground-side ESP32 project for the new `Teensy_ESP_AIR_GND` structure.

Current role in this scaffold:
- host the browser UI for iPhone
- receive telemetry frames from `ESP_AIR` over UDP
- publish the latest telemetry snapshot to the browser over `/ws_state`
- keep browser control on `/ws_ctrl`

Current implementation status:
- PlatformIO project created
- browser assets copied from `ESP_AIR`
- websocket/http server ported to the ground side
- UDP ingest added for `TELEM_FULL_STATE`, `ACK`, and fusion settings frames
- logging/download APIs are stubbed for now

Next expected work:
- define the air-to-ground UDP packet contract completely
- make `ESP_AIR` transmit to `ESP_GND`
- relay control commands from `ESP_GND` back to `ESP_AIR`
- move or redesign logging responsibilities if needed
