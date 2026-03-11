# FAST_Teensy_Avionics

Standalone avionics core for `Teensy 4.0` (`teensy40`) using the Arduino framework.

## Role

This project is the flight-side source of truth in the current bench architecture.

Current responsibilities:
- sensor acquisition
- AHRS/fusion update
- GPS and baro state generation
- CRSF telemetry generation
- binary mirror-link telemetry and command handling for the ESP bridge

Current system split:
- `Teensy`: sensors, state, fusion, CRSF
- `ESP_AIR`: UART bridge to Teensy and `ESP-NOW` link to ground
- `ESP_GND`: AP, web UI, browser control, and ground-side radio link

No Wi-Fi, browser, or web-server code lives here.

## Status

Current implemented behavior:
- IMU acquisition and Fusion AHRS update at `400 Hz`
- u-blox GPS UBX parser on `Serial1`
- BMP280 baro support on I2C
- binary mirror link on `Serial3`
- runtime mirror stream-rate control from the ESP side
- fusion settings command/readback over the mirror link
- CRSF telemetry output on `Serial2`
- USB console commands for diagnostics and calibration

Current defaults:
- fusion gain: `0.06`
- accel rejection: `20 deg`
- magnetic rejection: `60 deg`
- recovery trigger period: `1200 samples` (`3.0 s @ 400 Hz`)
- mirror/source stream rate default: `50 Hz`
- mirror/log rate default: `50 Hz`

## Folder Structure

```text
Teensy/
  platformio.ini
  lib/
    Fusion/
    bmi/
    crsf/
  src/
    config.h
    gps_ubx.cpp/.h
    imu_fusion.cpp/.h
    MagCal.cpp/.h
    main.cpp
    mirror.cpp/.h
    state.h
    telemetry_crsf.cpp/.h
```

## Serial And I2C Mapping

Defined in `src/config.h`.

- USB console:
  - `Serial @ 115200`

- Mirror link to `ESP_AIR`:
  - `MIRROR_SERIAL = Serial3`
  - `MIRROR_BAUD = 921600`
  - Teensy pins: `TX=14`, `RX=15`

- CRSF telemetry:
  - `CRSF_SERIAL = Serial2`
  - `CRSF_BAUD = 420000`
  - Teensy pins: `TX=8`, `RX=7`

- GPS:
  - `GPS_SERIAL = Serial1`
  - `GPS_BAUD = 115200`
  - Teensy pins: `TX=1`, `RX=0`

- I2C:
  - `Wire`
  - Teensy pins: `SDA=18`, `SCL=19`

## Runtime Model

Main runtime responsibilities:
- IMU/fusion update at `400 Hz`
- GPS polling/parser in the main loop
- baro polling/filtering in the main loop
- mirror RX command handling
- mirror TX at runtime-configurable stream rate
- CRSF telemetry scheduling
- 2 Hz USB summary output

Implemented mirror command types:
- `CMD_SET_FUSION_SETTINGS`
- `CMD_GET_FUSION_SETTINGS`
- `CMD_SET_STREAM_RATE`

Implemented mirror outbound data:
- full state frame
- fusion settings frame
- command ACK/NACK frame

Mirror TX is non-blocking:
- if the serial TX path cannot accept a frame, that frame is dropped
- `mirror_drop_count` increments

## State Model

`src/state.h` contains the live avionics state sent to the ESP side and reused by CRSF:
- attitude: roll, pitch, yaw
- GPS: `iTOW`, `fixType`, `numSV`, `lat`, `lon`, `hMSL`, `gSpeed`, `headMot`, `hAcc`, `sAcc`
- baro: temperature, pressure, altitude, VSI
- counters: `gps_parse_errors`, `mirror_tx_ok`, `mirror_drop_count`
- timestamps: `last_gps_ms`, `last_imu_ms`, `last_baro_ms`

## Fusion AHRS

Fusion implementation is in `src/imu_fusion.cpp`.

Current design:
- Fusion library update at `400 Hz`
- calibrated accel/gyro/mag path
- runtime configurable Fusion settings

Public interface in `src/imu_fusion.h`:
- `update400Hz(State&)`
- `getFusionSettings(...)`
- `setFusionSettings(...)`
- IMU config and calibration helpers

## CRSF Telemetry

CRSF telemetry output is implemented in `src/telemetry_crsf.cpp`.

Current output includes:
- attitude
- GPS
- baro altitude
- vario

This path is independent of the ESP web UI path.

## USB Console

`src/main.cpp` provides the bench console.

Examples of current commands:
- `help`
- `stats`
- `getfusion`
- `setfusion <gain> <accelRej> <magRej> <recovery>`
- `espcomtest`
- `teensyloopback`
- `showcrsfin`

Exact command behavior lives in `src/main.cpp`.

## Build And Upload

From this folder:

```powershell
pio run
pio run -t upload
pio device monitor -b 115200
```

Current `platformio.ini` uses `teensy-gui` for upload, so the Teensy Loader may open during flashing.

## Bench Notes

Observed in the current three-unit bench setup:
- the Teensy mirror link recovers cleanly across AIR and GND reflashes/resets
- fusion setting changes from the GND web UI are now applied and read back correctly
- repeated system reflashes and resets have not shown bad behavior

For higher-level architecture notes, see:
- [TELEMETRY_PIPELINE_SPEC.md](TELEMETRY_PIPELINE_SPEC.md)
- [TELEMETRY_PIPELINE_IMPLEMENTATION_PLAN.md](TELEMETRY_PIPELINE_IMPLEMENTATION_PLAN.md)
