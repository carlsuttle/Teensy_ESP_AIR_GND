# Control Plane Inventory: 0.03 RC1

Date:
- 2026-04-02

Scope:
- AIR-side control behavior only
- categories:
  - recording start / stop
  - replay start / stop / pause / seek
  - SD / file operations
  - fusion parameter changes

Purpose:
- identify direct control-action paths
- classify whether they already use the SD API
- classify whether they use the shared AIR control plane
- identify remaining bypasses and the correct keep / migrate / remove recommendation

---

## 1. Current Adapter Inventory

### A. AIR radio adapter

File:
- `ESP_AIR/src/radio_link.cpp`

Main function:
- `handleCommand(...)`

Current state:
- recording commands: shared control plane
- replay commands: shared control plane
- file / SD commands: shared control plane
- fusion get/set: shared control plane

Classification:

| Path | Uses SD API | Uses shared backend | Bypasses control plane | Recommendation |
|---|---:|---:|---:|---|
| radio recording commands | n/a | yes | no | keep |
| radio replay commands | n/a | yes | no | keep |
| radio file / SD commands | yes | yes | no | keep |
| radio fusion commands | n/a | yes | no | keep |

Conclusion:

- the AIR radio adapter is now thin for the targeted control categories

### B. AIR serial / console adapter

File:
- `ESP_AIR/src/main.cpp`
- `ESP_AIR/src/serial_control.cpp`

Main function:
- `handleConsoleCommands()`

Current state:

Migrated to shared control plane:

- `sdmount`
- `sdeject`
- `sdstate`
- `sdrename`
- `sddelete`
- `logstart`
- `logstartid`
- `logstop`
- `logfiles`
- `logprefix`
- `csvfile`
- `replayfile`
- `replaylargest`
- `getfusion`
- `setfusion`
- `tapi getfusion`
- `tapi setfusion`

Machine-readable serial adapter:

- one-line JSON request -> AIR control-plane request
- one-line JSON immediate result
- one-line JSON final result for accepted requests
- supported categories:
  - `state`
  - `recording`
  - `replay`
  - `file`
  - `fusion`

Current validation status:

- this machine-readable serial adapter is now a valid RC2 runtime proof surface for functional control and mode gating
- SD/file actions on this path no longer execute FATFS/VFS work on `loopTask`; they execute on the dedicated AIR `sd_worker` task

Classification:

| Path | Uses SD API | Uses shared backend | Bypasses control plane | Recommendation |
|---|---:|---:|---:|---|
| serial recording commands above | yes where relevant | yes | no | keep |
| serial replay start-by-file commands | n/a | yes | no | keep |
| serial file / SD commands above | yes | yes | no | keep |
| serial fusion get/set commands | n/a | yes | no | keep |
| serial JSON control adapter | yes where relevant | yes | no | keep |

Conclusion:

- the normal AIR console adapter is now thin for the targeted user-facing control categories

### C. Browser / WebSocket path

Relevant architecture:

- browser talks to `ESP_GND`
- `ESP_GND` forwards controls over AIR/GND transport
- AIR radio command handler is the AIR-facing control adapter

Classification:

| Path | Uses SD API | Uses shared backend | Bypasses control plane | Recommendation |
|---|---:|---:|---:|---|
| browser -> GND -> AIR file / record / replay / fusion controls | yes where relevant on AIR | yes | no on AIR | keep |

Conclusion:

- on AIR, browser-originated control is centralized because it lands in the radio adapter, which now uses the control plane

---

## 2. Remaining Direct / Bypass Paths

These are the current remaining paths that still directly perform targeted actions without going through the AIR control plane.

### A. Replay-capture / replay-compare proof helpers

File:
- `ESP_AIR/src/main.cpp`

Functions:

- `beginReplayCapture(...)`
- `beginReplayCompare(...)`
- related replay-capture completion helpers

Direct actions:

- direct `log_store::startSession(...)`
- direct `replay_bridge::startFile(...)`
- direct `log_store::stopSession()`

Classification:

| Path | Uses SD API | Uses shared backend | Bypasses control plane | Recommendation |
|---|---:|---:|---:|---|
| replay capture helper flow | no | no | yes | keep for now, migrate later only with care |
| replay compare helper flow | no | no | yes | keep for now, migrate later only with care |

Reason:

- these are internal proof orchestration helpers, not the normal runtime control adapter path
- changing them aggressively risks disturbing the validated replay/logging proof workflow

### B. Benchmark / soak / validation helpers

File:
- `ESP_AIR/src/main.cpp`

Examples:

- `writeSdApiTestLog()`
- `runTimeValidation(...)`
- SD soak helpers
- bench sweep helper flows

Direct actions:

- direct `log_store::startSession(...)`
- direct `log_store::stopSession()`
- occasional direct replay or fusion helper use

Classification:

| Path | Uses SD API | Uses shared backend | Bypasses control plane | Recommendation |
|---|---:|---:|---:|---|
| SD/time validation helper flows | mixed | no | yes | keep for now |
| bench / soak proof flows | mixed | no | yes | keep for now |

Reason:

- these are proof/test internals rather than interface adapters
- they are still direct and should remain clearly documented as such

### C. Low-level Teensy helper transport

File:
- `ESP_AIR/src/teensy_api.cpp`

Direct action:

- `setFusionSettings(...)` -> `teensy_link::sendSetFusionSettings(...)`

Classification:

| Path | Uses SD API | Uses shared backend | Bypasses control plane | Recommendation |
|---|---:|---:|---:|---|
| low-level teensy API fusion helper | no | no | yes | keep as low-level helper for now |

Reason:

- this is not the normal AIR runtime adapter path
- it is an internal helper surface used by proof tools

---

## 3. Summary By File / Function

Files and functions that still bypass centralized control for the targeted categories:

- `ESP_AIR/src/main.cpp`
  - `handleTeensyApiConsoleCommand(...)`
  - `beginReplayCapture(...)`
  - `beginReplayCompare(...)`
  - benchmark / soak / validation helper flows
- `ESP_AIR/src/teensy_api.cpp`
  - low-level fusion helper transport

Files now using centralized control for the targeted categories:

- `ESP_AIR/src/radio_link.cpp`
  - `handleCommand(...)`
- `ESP_AIR/src/main.cpp`
  - normal user-facing `handleConsoleCommands()` paths for record / replay start-by-file / file / SD / fusion
- `ESP_AIR/src/serial_control.cpp`
  - machine-readable serial request/result adapter for state / recording / replay / storage / file-list / fusion

---

## 4. Recommendation

### Keep

- AIR radio adapter through the shared control plane
- normal AIR console adapter through the shared control plane
- machine-readable AIR serial adapter through the shared control plane
- GND/browser forwarding into AIR radio control

### Keep Direct For Now

- replay proof orchestration helpers
- SD/time self-test helpers
- soak / bench helpers
- low-level `teensy_api` transport helper internals

Reason:

- these are not the normal runtime control surface
- they are still valuable proof tools
- migrating them carelessly would risk collateral changes to validated test flows

### Remove

- no broad removal recommended in this pass
- remove only duplicated runtime adapter logic once the shared control path is confirmed stable

---

## 5. Bottom Line

For the targeted runtime control categories:

- AIR radio adapter: centralized
- AIR normal console adapter: centralized
- AIR machine-readable serial adapter: centralized
- browser/GND path on AIR side: centralized

Remaining bypasses are now mostly:

- proof helpers
- benchmark helpers
- low-level helper internals

That means the main runtime control behavior is now converging on one AIR-owned control plane, while proof/test helper flows remain deliberately direct until a later focused cleanup pass.

2026-04-08 update:

- the targeted runtime control surface is now not just converging but proven in live use for RC2
- remaining direct paths are intentionally outside the RC2 runtime proof surface and should stay documented as such
- standalone replaybench remains a valid proof tool only when it skips radio poll/publish activity in `standalone_bench`
