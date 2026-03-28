# Milestone Manifest: ESP_AIR Teensy API Proof

Date:
- 2026-03-28

Purpose:
- freeze the validated `ESP_AIR`-to-Teensy API proof surface
- capture the exact bench command sequence and observed result

Repo:
- `C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND`

Branch:
- `teensy-source-rate-refactor`

Base commit:
- `5823924`

Worktree state:
- dirty

Relevant files:
- [ESP_AIR/src/teensy_api.h](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\teensy_api.h)
- [ESP_AIR/src/teensy_api.cpp](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\teensy_api.cpp)
- [ESP_AIR/src/main.cpp](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\main.cpp)
- [scripts/run_teensy_api_exerciser.ps1](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_exerciser.ps1)

Bench ports:
- `ESP_AIR`: `COM7`

Validation sequence:
1. `bench on`
2. `quiet on`
3. `tapi status`
4. `tapi getfusion`
5. `tapi setcap 1600`
6. `tapi setstream 1600 1600`
7. `tapi carry 8`
8. `tapi carrycsv 2000 16`
9. `tapi selftest 1600 8`

Expected final line:
- `FINAL SUMMARY passed=10 failed=0 port=COM7`

Observed result:
- pass

Notes:
- startup `AIRTX ...` chatter may appear before `bench on`/`quiet on`
- the exerciser keeps DTR/RTS low so the port open does not force the ESP32-S3 into ROM boot
- replay benchmark maxima documented in the paired API results report:
  - `100 Hz x 24` -> about `2399.3 records/s`
  - `50 Hz x 48` -> about `2399.3 records/s`
