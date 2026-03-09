# FAST_Teensy_Avionics

Standalone avionics core for **Teensy 4.0** (`teensy40`) using Arduino framework.

This project is the flight-side processor in the current bench architecture:
- Teensy owns sensor acquisition and fusion
- Teensy emits binary telemetry to the ESP32 bridge
- Teensy also emits CRSF telemetry directly
- No Wi-Fi, web server, or browser code lives here

## Status

Current implemented behavior:
- IMU acquisition and Fusion AHRS update at **400 Hz**
- u-blox GPS UBX parser on `Serial1`
- BMP280 baro support on I2C
- Binary mirror link on `Serial3` to the ESP32 bridge
- Runtime mirror stream-rate control from the ESP32
- Fusion settings command/readback over the mirror link
- CRSF telemetry output on `Serial2`
- USB console commands for bench diagnostics and calibration support

Current defaults:
- fusion gain: `0.06`
- accel rejection: `20 deg`
- magnetic rejection: `60 deg`
- recovery trigger period: `1200 samples` (`3.0 s @ 400 Hz`)
- mirror/source stream rate default: `20 Hz` until overridden by ESP32 command

## Folder Structure

```text
FAST_Teensy_Avionics/
  platformio.ini
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

## Build Configuration

`platformio.ini`:
- board: `teensy40`
- monitor speed: `115200`

## Serial and I2C Mapping

Defined in `src/config.h`.

- USB console:
  - `Serial @ 115200`

- Mirror link to ESP32 bridge:
  - `MIRROR_SERIAL = Serial3`
  - `MIRROR_BAUD = 921600`
  - Teensy pins: `TX=14`, `RX=15`

- CRSF telemetry:
  - `CRSF_SERIAL = Serial2`
  - `CRSF_BAUD = 420000`
  - Teensy pins: `TX=8`, `RX=7`

- GPS link:
  - `GPS_SERIAL = Serial1`
  - `GPS_BAUD = 115200`
  - Teensy pins: `TX=1`, `RX=0`

- I2C bus:
  - `Wire`
  - Teensy pins: `SDA=18`, `SCL=19`

## Runtime Model

Main runtime responsibilities:
- IMU/fusion update at `400 Hz`
- GPS poll/parser in the main loop
- baro polling/filtering in the main loop
- mirror RX command handling
- mirror TX at runtime-configurable stream rate
- CRSF telemetry output scheduling
- 2 Hz USB summary output

The ESP32 can change the Teensy mirror stream rate using `CMD_SET_STREAM_RATE`.

## State Model

`src/state.h` contains the live avionics state sent to the ESP32 and reused by CRSF output:
- attitude: roll, pitch, yaw
- GPS: iTOW, fixType, numSV, lat, lon, hMSL, gSpeed, headMot, hAcc, sAcc
- baro: temperature, pressure, altitude, VSI
- counters: `gps_parse_errors`, `mirror_tx_ok`, `mirror_drop_count`
- timestamps: `last_gps_ms`, `last_imu_ms`, `last_baro_ms`

## Mirror Link

The Teensy mirror link is the UART bridge to the ESP32 web/logger unit.

Transport characteristics:
- UART on `Serial3 @ 921600`
- framed binary protocol
- command RX from ESP32
- ACK/NACK responses
- dedicated fusion-settings response frame

Implemented mirror command types include:
- `CMD_SET_FUSION_SETTINGS`
- `CMD_GET_FUSION_SETTINGS`
- `CMD_SET_STREAM_RATE`

Implemented outbound data includes:
- full state frame
- fusion settings frame
- command ACK/NACK frame

Mirror TX is non-blocking:
- if the serial TX side cannot accept a frame, it is dropped
- `mirror_drop_count` increments

## Fusion AHRS

Fusion implementation is in `src/imu_fusion.cpp`.

Current design:
- Fusion library update at `400 Hz`
- calibrated accel/gyro/mag path
- runtime configurable Fusion settings
- current defaults:
  - `gain = 0.06`
  - `accelerationRejection = 20 deg`
  - `magneticRejection = 60 deg`
  - `recoveryTriggerPeriod = 1200 samples`

Public control surface in `src/imu_fusion.h`:
- `update400Hz(State&)`
- `getFusionSettings(...)`
- `setFusionSettings(...)`

## CRSF Telemetry

CRSF telemetry output is in `src/telemetry_crsf.cpp`.

Current output includes:
- attitude
- GPS
- baro altitude
- vario

This path is independent of the ESP32 web UI path.

## USB Console

`src/main.cpp` provides a bench console on USB serial.

Examples of current commands:
- `help`
- `stats`
- `getfusion`
- `setfusion <gain> <accelRej> <magRej> <recovery>`
- `espcomtest`
- `teensyloopback`
- `showcrsfin`

Exact command behavior lives in `src/main.cpp`.

## Build / Upload

From this folder:

```powershell
pio run
pio run -t upload
pio device monitor -b 115200
```

## Bench Notes

- The Teensy default mirror stream rate is `20 Hz`.
- The ESP32 bridge normally overrides this after boot using `CMD_SET_STREAM_RATE`.
- If ESP32 and Teensy boot order differs, the system should self-correct once the command/ACK path is up.
- For current higher-level browser/logging architecture, see:
  - [TELEMETRY_PIPELINE_SPEC.md](c:/Users/dell/Platformio/esp32_crsf_telemetry/FAST_Teensy_Avionics/TELEMETRY_PIPELINE_SPEC.md)
  - [TELEMETRY_PIPELINE_IMPLEMENTATION_PLAN.md](c:/Users/dell/Platformio/esp32_crsf_telemetry/FAST_Teensy_Avionics/TELEMETRY_PIPELINE_IMPLEMENTATION_PLAN.md)
