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

2. Release `0.03` schema architecture refactor:
   - modify the Teensy <-> `ESP_AIR` transfer path so schema changes are easy to make and review.
   - remove hard-coded schema dependencies from transport, parsing, logging, replay, and test-facing code.
   - support multiple schemas in the codebase, but allow only one active schema selection per build.
   - define clean schema-selection boundaries so future schema swaps do not require broad code edits.

3. Hard-coded dependency audit:
   - identify every direct size/offset/type dependency tied to the current transfer schema.
   - replace those dependencies with schema-owned definitions or version-selected interfaces.
   - prove there are no remaining unintended hard-coded schema references before introducing the next format.

4. New schema creation for release `0.03`:
   - invent a new transfer schema only after the dependency audit/refactor is complete.
   - make the new schema a strict superset of the current schema so baseline fields still exist.
   - keep work on this branch only: `teensy-source-rate-refactor`.

5. Replay fidelity validation:
   - run 1:1 / 1:2 / 1:4 replay rerecord comparisons.
   - verify carry-through fields (GPS/baro/IMU/meta/time stamps) against expected behavior.

6. Integrated AIR+Teensy+SD workflow:
   - run start/stop recording cycles with control changes during replay.
   - verify produced files and metadata lineage rules.

7. Release `0.03` regression gate:
   - re-run all validation scripts used to prove release `0.02`.
   - require `0.03` to meet the same pass/fail standards and operating envelopes as `0.02`.
   - treat any regression against the `0.02` bar as a release blocker until resolved.

8. Performance operating envelopes:
   - formalize recommended limits for 100Hz and 50Hz modes.
   - document safe defaults and fail/timeout boundaries.

9. Optional future work:
   - keep DMA transport enhancements as future optimization only if measured need appears.

---

## 5. Release 0.03 Goals

- keep all release `0.03` work on branch `teensy-source-rate-refactor`
- refactor Teensy-to-`ESP_AIR` schema handling so changes are centralized and easy to update
- eliminate hard-coded dependencies on one transfer schema across build, transport, logging, replay, and validation surfaces
- support multiple schemas in source control while enforcing one selected schema per build
- create a new schema that is a superset of the current baseline schema
- rerun the full release `0.02` validation suite and require equivalent proof quality before accepting release `0.03`
- preserve the release `0.02` baseline as the comparison standard throughout the `0.03` cycle

---

## 6. Release 0.03 Implementation Plan

Phase 1: Baseline lock and dependency inventory

- freeze current `0.02` proof behavior as the comparison target before refactor work begins
- inventory all transfer-schema dependencies in:
  - `ESP_AIR/src/types_shared.h`
  - `ESP_AIR/src/spi_bridge.cpp`
  - `Teensy/src/spi_bridge.cpp`
  - `ESP_AIR/src/replay_bridge.cpp`
  - `ESP_AIR/src/log_store.cpp`
  - `ESP_AIR/src/teensy_link.cpp`
  - `Teensy/src/mirror.cpp`
  - any scripts or docs that assume fixed `160`-byte transfer records
- classify each dependency as one of:
  - schema-owned type/size constant
  - transport framing dependency
  - log/replay decoding dependency
  - UI/test/documentation dependency

Phase 2: Centralize schema selection

- create one schema-selection layer that is the only place a build chooses the active transfer schema
- define a schema descriptor for the active build with fields such as:
  - schema id / version
  - state record bytes
  - replay input record bytes
  - replay control record bytes
  - log metadata schema id
  - log record kind mapping
- move size constants like `160` and schema-specific type aliases out of transport and replay code and behind that selection layer
- require both `ESP_AIR` and `Teensy` builds to include the same selected schema definition

Phase 3: Remove hard-coded transport coupling

- replace fixed `kRecordBytes = 160U` assumptions in both SPI bridge implementations with selected-schema values
- ensure ring buffers, payload batching, CRC coverage, and record-pop/push APIs use schema-owned sizes
- keep the transport generic: it should move opaque schema records without knowing field layout
- preserve the existing single-schema-per-build rule even after multi-schema support is added to source

Phase 4: Remove hard-coded decode and logging coupling

- replace direct `ReplayInputRecord160`, `ReplayControlRecord160`, `State160`, and `Metadata160` dependencies where possible with schema-selected aliases or decoder helpers
- isolate schema-specific decode/encode logic for:
  - replay input generation
  - replay control application
  - binary log metadata emission
  - state extraction for comparison/export
- keep file format validation explicit by checking schema id/version at read time

Phase 5: Introduce release `0.03` schema

- define a new transfer schema only after Phases 1 through 4 are complete
- make the new schema a strict superset of the current schema so prior fields remain valid and aligned by design
- assign new schema identifiers and record-kind names instead of overloading the old `160`-specific names
- update logging/replay metadata so recorded files identify which schema produced them

Phase 6: Validation and release gate

- run the full `0.02` proof suite after each bounded schema milestone:
  - `run_teensy_api_exerciser.ps1`
  - `run_teensy_api_mode_sweep.ps1`
  - `run_teensy_api_replay_benchmark.ps1`
- compare `0.03` against the `0.02` standard for:
  - pass/fail totals
  - validated replay throughput envelope
  - preferred operating point stability
  - replay fidelity and log lineage behavior
- treat any regression as a blocker until the root cause is understood and fixed

Definition of done for release `0.03`:

- there are no unintended hard-coded references tying the transfer path to one schema layout
- one build flag or one schema-selection header chooses the active schema for both `ESP_AIR` and `Teensy`
- at least two schemas exist in source, with the new one being a superset of the old one
- log/replay artifacts declare which schema they use
- the release `0.02` validation bar is re-proven on this branch with release `0.03`

---

## 7. Operator Commands (Quick Start)

Flash:

- AIR:
  - `platformio run -e seeed_xiao_esp32s3 -t upload --upload-port COM7` (from `ESP_AIR`)
- Teensy:
  - `platformio run -e teensy40 -t upload --upload-port COM10` (from `Teensy`)

Validation scripts:

- `powershell -ExecutionPolicy Bypass -File scripts/run_teensy_api_exerciser.ps1 -ComPort COM7`
- `powershell -ExecutionPolicy Bypass -File scripts/run_teensy_api_mode_sweep.ps1 -ComPort COM7`
- `powershell -ExecutionPolicy Bypass -File scripts/run_teensy_api_replay_benchmark.ps1 -ComPort COM7`
