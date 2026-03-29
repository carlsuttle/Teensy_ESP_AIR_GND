# Milestone Manifest: Release 0.02

Date:
- 2026-03-28

Purpose:
- freeze the validated `ESP_AIR` to Teensy API library state
- capture the AIR-side SD/replay hardening that remains on top of the API-library work
- provide one manifest a future AI interaction can use to rebuild and reproduce the current Teensy/AIR bench state

Repo:
- `C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND`

Branch:
- `teensy-source-rate-refactor`

Release commits:
- `bfb8658` - `Add Teensy API library and proof benchmarks`
- release `0.02` commit - see current `HEAD`

Bench ports:
- `ESP_AIR`: `COM7`
- `Teensy`: `COM10`
- `ESP_GND`: optional / not required for the Teensy API proof because AIR scripts force `bench on`

Relevant files:
- [ESP_AIR/src/teensy_api.h](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\teensy_api.h)
- [ESP_AIR/src/teensy_api.cpp](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\teensy_api.cpp)
- [ESP_AIR/src/main.cpp](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\main.cpp)
- [ESP_AIR/src/log_store.h](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\log_store.h)
- [ESP_AIR/src/log_store.cpp](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\log_store.cpp)
- [ESP_AIR/src/replay_bridge.cpp](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\replay_bridge.cpp)
- [ESP_AIR/src/sd_backend.cpp](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\sd_backend.cpp)
- [ESP_AIR_TEENSY_API_LIBRARY_AND_TEST_RESULTS_2026-03-28.md](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR_TEENSY_API_LIBRARY_AND_TEST_RESULTS_2026-03-28.md)
- [AI_AVIONICS_PROJECT_RECORD.md](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\AI_AVIONICS_PROJECT_RECORD.md)
- [scripts/run_teensy_api_exerciser.ps1](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_exerciser.ps1)
- [scripts/run_teensy_api_mode_sweep.ps1](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_mode_sweep.ps1)
- [scripts/run_teensy_api_replay_benchmark.ps1](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_replay_benchmark.ps1)

State captured in release `0.02`:
- `ESP_AIR` operational logs use:
  - one `Metadata160` record at file creation
  - then `State160` records only
- AIR logger block pool enlarged from `4` to `64`
- idle SD media checks can be disabled during active logging/replay
- replay source reads use exact full-record reads
- replay skips metadata records cleanly
- AIR now tries `40 MHz` SD mount first
- Teensy API proof surface is documented and scriptable through `tapi ...`
- baseline-lock protocol added to project record as required AI behavior:
  - run API gate scripts before transport/schema edits
  - stop on first regression and revert to baseline
  - require before/after proof logs for accepted fixes

How to reproduce this state:

1. Build and flash `ESP_AIR` to `COM7`
2. Build and flash `Teensy` to `COM10`
3. Open AIR serial on `COM7`
4. Open Teensy serial on `COM10`

Recommended proof scripts:

Simple exerciser:
```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_exerciser.ps1 -ComPort COM7
```

Expected final line:
- `FINAL SUMMARY passed=9 failed=0 port=COM7`

Mode sweep:
```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_mode_sweep.ps1 -ComPort COM7
```

Expected final line:
- `FINAL SUMMARY passed=10 failed=0 port=COM7`

Replay/API benchmark:
```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_replay_benchmark.ps1 -AirComPort COM7 -TeensyComPort COM10
```

Expected validated replay maxima:
- `100 Hz x 24` -> about `2399.3 records/s`
- `50 Hz x 48` -> about `2399.3 records/s`

Preferred operating point:
- `50 Hz x 48`
- same validated throughput as `100 Hz x 24`
- lower batch cadence and better queue headroom

Expected AIR bench behavior:
- scripts force `bench on`
- then `quiet on`
- expected response:
  - `BENCH standalone=1 radio=0 wifi=off`

Notes:
- unrelated historical manifests and generated CSV benchmark artifacts may remain untracked in the repo root; they are not part of release `0.02`
- `ESP_GND` is not required for the Teensy API proof because AIR bench mode disables radio/web activity
