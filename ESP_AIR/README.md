# ESP_AIR

Air-side ESP32 project for the new `Teensy_ESP_AIR_GND` structure.

This folder was seeded from the current working `esp32_air_unit_ws_bridge` project and is the starting point for the aircraft-side ESP role.

## Intended Role

`ESP_AIR` is intended to own:
- Teensy UART telemetry ingest
- authoritative onboard logging
- air-to-ground telemetry transmission
- command relay between ground ESP and Teensy

It is not intended to remain the long-term iPhone web server.

## Current State

Right now this folder is still a copied working bridge baseline, so it still contains:
- the current UART ingest path
- the current logger pipeline
- the current websocket/web UI code

That browser-serving code is present only because this is the fastest safe starting point.

## Immediate Next Refactor

The expected next steps in this folder are:
- keep UART ingest and logging
- remove browser-serving responsibilities over time
- replace direct phone-facing websocket behavior with air-to-ground transport behavior

## Source of the Seed

Copied from:
- [esp32_air_unit_ws_bridge](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge)

## Related Folders

- [Teensy](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/Teensy)
- [ESP_GND](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND)
