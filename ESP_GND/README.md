# ESP_GND

Ground-side ESP32 project for the new `Teensy_ESP_AIR_GND` structure.

Current role in this scaffold:
- host the browser UI for iPhone
- receive telemetry frames from `ESP_AIR` over `ESP-NOW`
- publish the latest telemetry snapshot to the browser over `/ws_state`
- keep browser control on `/ws_ctrl`

Current implementation status:
- PlatformIO project created
- browser assets copied from `ESP_AIR`
- websocket/http server ported to the ground side
- bidirectional `ESP-NOW` ingest added for `TELEM_FULL_STATE`, `ACK`, fusion settings, and link metadata
- logging/download APIs are stubbed for now

Next expected work:
- harden long-run `ESP-NOW` link monitoring and soak testing
- surface more link health in the browser if needed
- move or redesign logging responsibilities if needed
