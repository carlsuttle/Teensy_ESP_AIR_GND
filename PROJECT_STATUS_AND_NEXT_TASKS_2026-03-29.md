# Project Status And Next Tasks (2026-03-29)

Date:
- 2026-03-29

Repo:
- `C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND`

Branch:
- `teensy-source-rate-refactor`

Current head:
- `dc8d9d6`

---

## 1. Current Project Status

- Release baseline is locked at `0.02` (`16b9307`) with subsequent documentation/artifact commits on top.
- AIR (`COM7`) and Teensy (`COM10`) were reflashed and validated from this branch.
- Core Teensy API validation scripts are passing again when run from this baseline.
- Baseline-lock protocol has been added to project governance documentation and manifest.

Key confirmation runs:

- `run_teensy_api_exerciser.ps1`:
  - pass (`passed=9 failed=0`)
- `run_teensy_api_mode_sweep.ps1`:
  - pass (`passed=10 failed=0`)
- `run_teensy_api_replay_benchmark.ps1`:
  - mixed by operating point:
    - stable pass at `100Hz x 24` and `50Hz x 48`
    - failures/timeouts at higher 100Hz batch sizes (`x32` and above)
  - 60s soak passes at:
    - `100Hz x 24`
    - `50Hz x 48`

Current proven replay throughput envelopes:

- `100Hz x 24`: about `2399 rps`
- `50Hz x 48`: about `2400 rps`

Preferred operating point:

- `50Hz x 48` (same throughput with lower batch cadence)

---

## 2. Documents Updated In This Cycle

- [AI_AVIONICS_PROJECT_RECORD.md](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/AI_AVIONICS_PROJECT_RECORD.md)
  - includes mandatory Baseline-Lock Protocol section.
- [MILESTONE_MANIFEST_RELEASE_0.02.md](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/MILESTONE_MANIFEST_RELEASE_0.02.md)
  - includes baseline-lock enforcement note and updated release context.
- replay benchmark CSV artifacts committed for traceability:
  - `scripts/teensy_api_replay_benchmark_20260328_222339.csv`
  - `scripts/teensy_api_replay_benchmark_20260328_223054.csv`

---

## 3. Open Technical Constraints

- At 100Hz, increasing replay batch records above `24` causes queue pressure/timeouts in current configuration.
- This is a capacity/operating-point limit, not a confirmed schema regression.
- No confirmed product bug was proven specific to the attempted 196-byte schema expansion in this cycle.

---

## 4. Next Tasks (Prioritized)

1. Keep baseline stable:
   - continue using release `0.02` behavior and Baseline-Lock Protocol for all changes.

2. Complete schema expansion work safely:
   - move to 196-byte state record only under gated proof runs.
   - re-run full API gate suite after each bounded schema change.

3. Replay fidelity validation:
   - run 1:1 / 1:2 / 1:4 replay rerecord comparisons.
   - verify carry-through fields (GPS/baro/IMU/meta/time stamps) against expected behavior.

4. Integrated AIR+Teensy+SD workflow:
   - run start/stop recording cycles with control changes during replay.
   - verify produced files and metadata lineage rules.

5. Performance operating envelopes:
   - formalize recommended limits for 100Hz and 50Hz modes.
   - document safe defaults and fail/timeout boundaries.

6. Optional future work:
   - keep DMA transport enhancements as future optimization only if measured need appears.

---

## 5. Operator Commands (Quick Start)

Flash:

- AIR:
  - `platformio run -e seeed_xiao_esp32s3 -t upload --upload-port COM7` (from `ESP_AIR`)
- Teensy:
  - `platformio run -e teensy40 -t upload --upload-port COM10` (from `Teensy`)

Validation scripts:

- `powershell -ExecutionPolicy Bypass -File scripts/run_teensy_api_exerciser.ps1 -ComPort COM7`
- `powershell -ExecutionPolicy Bypass -File scripts/run_teensy_api_mode_sweep.ps1 -ComPort COM7`
- `powershell -ExecutionPolicy Bypass -File scripts/run_teensy_api_replay_benchmark.ps1 -ComPort COM7`

