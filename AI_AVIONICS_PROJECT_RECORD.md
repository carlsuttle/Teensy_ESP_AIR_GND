# AI-Assisted Experimental Avionics Laboratory

Version: 0.3
Author: Carl Suttle
Date: 2026
Updated: 2026-03-21

This document is the architectural briefing and continuity record for an
AI-assisted experimental avionics development project. It is intended to let a
new AI session continue useful work immediately instead of reconstructing the
system from scattered code and chat history.

---

## 1. Purpose

This project explores AI-assisted avionics development in a practical
experimental laboratory context.

Central question:

Can a technically minded pilot, working with AI tools, design and prototype
useful flight instrumentation systems that historically required larger
specialist engineering teams?

This is experimental research, not a certified avionics program.

Primary aims:

- Explore pilot-centric instrumentation
- Accelerate avionics prototyping
- Evaluate AI as an engineering collaborator
- Document a repeatable AI-assisted development method
- Build a transferable knowledge base for future AI sessions

Working hypothesis:

AI reduces implementation friction enough that domain expertise, test
discipline, and system judgement become the main drivers of useful innovation.

---

## 2. Role Of This Document

This document serves four purposes:

- Project memory
- Architecture reference
- AI session briefing
- Laboratory notebook summary

It is the canonical project-history document. Older standalone report notes
should be folded into this file rather than maintained in parallel.

It should be sufficient for a new AI session to understand:

- what processors and sensors exist
- what the live data contracts are
- where the source-of-truth files live
- what is working
- what is blocked
- what assumptions are still unproven

If a future AI session needs to inspect code immediately, this document should
point to the exact files to open first.

---

## 3. Project Philosophy

### Domain Knowledge Drives Design

Aviation knowledge defines the problems worth solving.

AI assists with:

- implementation
- debugging
- refactoring
- scaffolding
- documentation

AI does not determine avionics meaning or pilot needs.

### Rapid Prototyping Loop

1. Identify instrumentation problem
2. Form concept
3. Implement rapidly with AI assistance
4. Bench test
5. Simulate or flight test
6. Observe and document
7. Iterate

AI most strongly accelerates step 3.

### Demonstration Over Perfection

The first working demonstration is the key milestone.

A working prototype proves feasibility and enables refinement.

### Pilot-Centric Instrumentation

Focus on situational awareness rather than sensor novelty.

Questions:

- What information does a pilot actually need?
- When do conventional instruments mislead?
- How can energy state be displayed more clearly?

---

## 4. Engineering Method

AI-Assisted Domain Engineering

Traditional path:

Idea -> engineering organization -> implementation -> prototype

This project path:

Domain insight -> AI collaboration -> prototype

As implementation becomes easier, the difficult areas shift toward:

- system architecture
- validation
- human factors
- interpretation of physical behavior

---

## 5. Current System Architecture

### Main Processing Roles

- Teensy 4.0
- ESP_AIR (Seeed XIAO ESP32S3)
- ESP_GND (ESP32 ground display / AP node)

### Teensy 4.0

Responsibilities:

- sensor acquisition
- sensor fusion
- flight state estimation
- deterministic avionics processing
- command-line debug and calibration

Primary implementation files:

- [Teensy/src/main.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/Teensy/src/main.cpp)
- [Teensy/src/imu_fusion.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/Teensy/src/imu_fusion.cpp)
- [Teensy/src/gps_ubx.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/Teensy/src/gps_ubx.cpp)
- [Teensy/src/mirror.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/Teensy/src/mirror.cpp)
- [Teensy/src/config.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/Teensy/src/config.h)

### Sensors

- IMU: BMI270
- Magnetometer: BMM150
- GPS: u-blox M10 using NAV-PVT
- Barometer: BMP280

Sensor ownership:

- IMU + magnetometer + barometer are on Teensy
- GPS is on Teensy UART
- ESP_AIR and ESP_GND do not own flight sensors directly

### ESP_AIR

Responsibilities:

- receive telemetry from Teensy over UART
- maintain local telemetry snapshot
- radio transport to ground
- recorder / logging backend
- diagnostics and command relay

Primary implementation files:

- [ESP_AIR/src/main.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/main.cpp)
- [ESP_AIR/src/uart_telem.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/uart_telem.cpp)
- [ESP_AIR/src/radio_link.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/radio_link.cpp)
- [ESP_AIR/src/log_store.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/log_store.cpp)
- [ESP_AIR/src/types_shared.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/types_shared.h)

### ESP_GND

Responsibilities:

- receive telemetry from ESP_AIR over ESP-NOW
- host AP and web UI
- push state and control data over WebSocket
- user interaction for settings and logging control

Primary implementation files:

- [ESP_GND/src/main.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/src/main.cpp)
- [ESP_GND/src/radio_link.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/src/radio_link.cpp)
- [ESP_GND/src/ws_server.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/src/ws_server.cpp)
- [ESP_GND/src/types_shared.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/src/types_shared.h)
- [ESP_GND/data/app.js](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js)
- [ESP_GND/data/telemetry.js](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/telemetry.js)

### Data Path

Sensors -> Teensy -> UART mirror frames -> ESP_AIR -> ESP-NOW -> ESP_GND ->
WebSocket -> browser UI

### Approximate Update Rates

- IMU fusion loop: about 400 Hz
- GPS NAV-PVT: 10 Hz
- UART telemetry mirror: configurable, currently 50 Hz default
- AIR to GND radio unified downlink: 30 Hz normal mode
- Browser UI: 30 Hz fixed normal mode

---

## 6. Source-Of-Truth Interfaces

This section is the minimum starting point for any new development session.

### Shared telemetry schema

Authoritative file:

- [ESP_AIR/src/types_shared.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/types_shared.h)

The GND copy in:

- [ESP_GND/src/types_shared.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/src/types_shared.h)

must stay identical for transport compatibility.

Important constants:

- `kMagic = "TELM"`
- `kVersion = 1`
- `kWsStateMagic = "WSTE"`
- `kWsStateVersion = 2`
- `kRadioChannel = 6`
- `kEspNowMaxDataLen = 250`

Important message types:

- `TELEM_FULL_STATE`
- `TELEM_FUSION_SETTINGS`
- `TELEM_LOG_STATUS`
- `TELEM_CONTROL_STATUS`
- `TELEM_UNIFIED_DOWNLINK`
- `CMD_SET_FUSION_SETTINGS`
- `CMD_GET_FUSION_SETTINGS`
- `CMD_SET_STREAM_RATE`
- `CMD_SET_RADIO_MODE`

### UART link contract: Teensy -> ESP_AIR

AIR-side implementation:

- [ESP_AIR/src/uart_telem.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/uart_telem.cpp)
- [ESP_AIR/src/uart_telem.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/uart_telem.h)

AIR default config:

- UART port: `1`
- RX pin: `3`
- TX pin: `4`
- baud: `921600`

Defined in:

- [ESP_AIR/src/config_store.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/config_store.cpp)

Teensy mirror link:

- [Teensy/src/config.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/Teensy/src/config.h)

Teensy defaults:

- `MIRROR_SERIAL = Serial3`
- Teensy TX pin `14`
- Teensy RX pin `15`
- mirror baud `921600`

### AIR <-> GND radio contract

AIR side:

- [ESP_AIR/src/radio_link.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/radio_link.cpp)

GND side:

- [ESP_GND/src/radio_link.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/src/radio_link.cpp)

Current transport:

- ESP-NOW on Wi-Fi channel `6`
- normal unified downlink rate `30 Hz`
- GPS section rate `10 Hz`
- radio control rate `2 Hz`

### GND -> browser contract

Implementation:

- [ESP_GND/src/ws_server.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/src/ws_server.cpp)
- [ESP_GND/data/app.js](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js)
- [ESP_GND/data/telemetry.js](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/telemetry.js)

Current browser transport split:

- `/ws_state` for binary state stream
- `/ws_ctrl` for JSON control and status

---

## 7. Experimental SPI / DMA Transport Prototype

This section records a standalone transport prototype developed outside the main
avionics firmware tree. It is a transport experiment, not yet the production
AIR/GND integration path.

Prototype repository:

- [SPI_DMA_Transport_Prototype](c:/Users/dell/Platformio/esp32_crsf_telemetry/SPI_DMA_Transport_Prototype)

Prototype roles:

- ESP32-S3 is SPI master
- Teensy 4.0 is SPI slave
- one SPI bus carries fixed-size full-duplex framed transactions
- a Teensy `READY` line requests service from the ESP master

Important prototype source files:

- [esp32_xiao_s3/include/config.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/SPI_DMA_Transport_Prototype/esp32_xiao_s3/include/config.h)
- [esp32_xiao_s3/include/protocol.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/SPI_DMA_Transport_Prototype/esp32_xiao_s3/include/protocol.h)
- [esp32_xiao_s3/src/spi_master_link.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/SPI_DMA_Transport_Prototype/esp32_xiao_s3/src/spi_master_link.cpp)
- [esp32_xiao_s3/src/main.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/SPI_DMA_Transport_Prototype/esp32_xiao_s3/src/main.cpp)
- [teensy_t40/include/config.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/SPI_DMA_Transport_Prototype/teensy_t40/include/config.h)
- [teensy_t40/include/protocol.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/SPI_DMA_Transport_Prototype/teensy_t40/include/protocol.h)
- [teensy_t40/src/spi_link.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/SPI_DMA_Transport_Prototype/teensy_t40/src/spi_link.cpp)
- [teensy_t40/src/main.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/SPI_DMA_Transport_Prototype/teensy_t40/src/main.cpp)

Current validated prototype characteristics:

- SPI clock: `40 MHz`
- framed fixed-size full-duplex transactions
- benchmark record size: `160 bytes`
- DMA-backed transport on both sides in the current prototype
- exact packed-record fit depends on transaction size:
  - `2048 bytes` -> `12` records max
  - `4096 bytes` -> `25` records max
  - `8192 bytes` -> `51` records max

Demonstrated full-duplex operating points, `40 MHz`, `160-byte` records:

| Transaction Size | Txn Rate | Approx Records / Txn / Dir | Approx Records / s / Dir | Notes |
|---|---:|---:|---:|---|
| `2048` | `300 Hz` | `10` | `3000` | Clean, higher-cadence setting |
| `4096` | `200 Hz` | `15` | `3000` | Clean, same payload rate with lower churn |
| `8192` | `100 Hz` | `35` | `3500` | Clean, strong SD-replay candidate |

Demonstrated low-interference one-way operating points, `40 MHz`, `160-byte`
records:

| Transaction Size | Mode | Txn Rate | Approx Records / Txn | Approx Records / s | Notes |
|---|---|---:|---:|---:|---|
| `2048` | `1` or `2` | `200 Hz` | `10` | `2000` | Clean one-way in both directions |
| `4096` | `1` or `2` | `100 Hz` | `20` | `2000` | Clean one-way in both directions |

Raw transport benchmark result, `8192-byte` transactions, `160-byte` records,
full duplex, no live record generation in the timed path:

- exact fit: `51` records per transaction per direction
- clean through `110 Hz`
- saturates above that at about `6620` transactions in `60 s`
- practical raw full-duplex ceiling: about `5627 records/s` each way
- recommended safe raw full-duplex operating point: `5000 records/s` each way
  at `100 Hz`, about `50` records per transaction per direction

Approximate useful payload at the recommended safe raw full-duplex point:

- about `800,000 bytes/s` each way
- about `781 KiB/s` each way
- about `1.56 MiB/s` combined useful payload

Application notes:

- at `100 Hz`, transaction latency is bounded to about `10 ms`
- the recommended `8192 @ 100 Hz` setting supports about `50 x 160-byte`
  records each way per transaction
- this is favorable for SD replay and SD logging because it is already strongly
  batched
- buffering one to four transactions before writing gives chunk sizes of about
  `8 KiB` to `32 KiB`, which is substantially more SD-friendly than per-record
  writes
- for practical replay work, the `8192 @ 100 Hz` operating point is credible as
  a bulk path for replaying about `3.5 kHz` record rate through the Teensy back
  to the ESP while staying inside the `10 ms` latency target

Important problems that were overcome:

- symmetric reverse bulk transfer was unreliable in the original burst-oriented
  prototype
- one-record-per-transaction framing created unnecessary overhead and allowed
  Teensy TX backlog to build
- the Teensy slave receive path had a race where a completed transaction could
  be re-armed before the finished RX buffer was parsed
- that race caused reverse replay frames to be lost without CRC or type errors

Fixes that materially improved the prototype:

- replaced the old burst-oriented protocol with fixed transaction-based framing
- added batching so multiple records can be packed into one transaction
- added explicit run-start synchronization and cleaner benchmark control
- fixed the Teensy re-arm race so completed transactions are parsed before a new
  transaction can overwrite the RX buffer
- added raw one-way and raw full-duplex benchmark modes to separate transport
  limits from live synthetic record-generation limits
- confirmed that earlier apparent reverse-direction weakness was primarily a
  benchmark-source limitation, not a fundamental raw SPI asymmetry

Current interpretation:

- the SPI/DMA transport prototype is now a valid candidate for high-rate bulk
  transport
- it is especially attractive where bounded latency is acceptable and batching
  is desirable
- for integrated avionics use, `8192-byte` transactions at `100 Hz` are a
  strong background-bulk setting when `10 ms` latency is acceptable
- if lower transaction churn matters more than maximum rate, `4096 @ 200 Hz`
  and `8192 @ 100 Hz` are both attractive
- it is not yet integrated into the production avionics codebase

Integration guidance:

- do not replace the current UART mirror path in one step
- first integrate SPI transport as a separate experimental path while keeping
  the existing UART link available as fallback
- use SPI for bulk state / replay / logging-oriented transport, not as a casual
  patch into unrelated code
- any protocol change must be made on both prototype sides together
- if SPI and SD share controller time on the same MCU, assume that SD latency
  spikes still require a RAM buffer even when average throughput looks safe

---

## 8. Flight State Data Model

Canonical record:

- `telem::TelemetryFullStateV1`
- defined in [ESP_AIR/src/types_shared.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/types_shared.h)

Current fields:

- roll_deg
- pitch_deg
- yaw_deg
- mag_heading_deg
- GPS fix / time / position / speed / track / accuracy fields
- GPS parse error count
- mirror transmission counters
- age markers for GPS / IMU / baro
- baro temperature / pressure / altitude / vertical speed
- current fusion settings
- state flags

Key notes:

- units are encoded in field names where possible
- `headMot_1e5deg` is GPS motion heading in 1e-5 degrees
- `lat_1e7` and `lon_1e7` are geographic coordinates in 1e-7 degrees
- `flags` includes fusion recovery and GPS-fix state

Related reduced downlink records:

- `DownlinkFastStateV1`
- `DownlinkGpsStateV1`
- `UnifiedDownlinkBaseV1`

If a future AI changes the telemetry contract, all of these must be reviewed
together, not one file at a time.

---

## 9. Logging Architecture

Current authoritative implementation:

- [ESP_AIR/src/log_store.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/log_store.cpp)
- [ESP_AIR/src/log_store.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/log_store.h)
- [ESP_AIR/src/sd_backend.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/sd_backend.cpp)
- [ESP_AIR/src/sd_backend.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/sd_backend.h)
- [ESP_AIR/src/sd_capture_test.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/sd_capture_test.cpp)
- [ESP_AIR/README.md](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/README.md)
- [AIR_SD_logging_benchmark_notes.md](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/AIR_SD_logging_benchmark_notes.md)

Current backend:

- shared microSD backend on `ESP_AIR`
- binary `.tlog` session files in `/logs`
- logger, `sdprobe`, `sdwrite`, and `sdcap1m` all use the same SD backend

Current binary record:

- `BinaryLogRecordV1`
- magic `"LOG1"`
- version `1`
- fields: `seq`, `t_us`, `TelemetryFullStateV1 state`

Current buffering:

- ring depth: `256` records
- write buffer: `8192` bytes
- batch write timeout: `500 ms`
- writer task handles filesystem writes
- SD block writer configuration used in bench work:
  - `10000-byte` blocks
  - `4` blocks in RAM
  - writer task pinned to core `0`

Current SD backend wiring and init behavior on `ESP_AIR`:

- `CS=2`
- `SCK=7`
- `MISO=8`
- `MOSI=9`
- mount attempts `40 MHz` first
- fallback mount at `26 MHz` if `40 MHz` does not mount

Standalone SD logging benchmark state:

- SD logging has been standalone benchmarked successfully and should be treated
  as being at the same maturity level as the SPI/DMA transport prototype:
  proven in isolation, not yet fully validated under the final integrated load
- benchmark target in the AIR notes:
  - `250-byte` records
  - `400 Hz`
  - about `97.7 KB/s`
- observed SPI SD block-write behavior with a Mindstar `64 GB` card:
  - `10000-byte` block writes
  - average write time about `7 ms`
  - worst write time about `9-10 ms`
  - no slow flush events observed in the final standalone runs
- implied instantaneous write bandwidth:
  - about `1.4 MB/s`
- this gives about `10-14x` margin against the original `~100 KB/s` AIR
  recorder target
- key conclusion from the standalone SD work:
  - SD throughput itself is not the limiting factor
  - the remaining risk is system interaction under real AIR load:
    UART ingest, radio servicing, logging, and filesystem latency spikes

Current operational rule:

- logging must not block flight processing or UART ingest
- if logging falls behind, bounded record loss is preferable to blocking

Important limitation:

- the shared SD backend is working and benchmarked, but it is still an
  experimental subsystem until it is validated under real integrated AIR load
- file logging remains disabled by default at boot because quota / rotation /
  download workflow is not yet finished
- SD logging confidence should now come from integrated AIR validation rather
  than further synthetic storage-only bench refinement

---

## 10. Coordinate Frames

Reference frames used in the system:

- Body frame: aircraft axes
- Earth frame: Fusion / tilt compensation reference
- Magnetic frame: calibrated magnetometer vector

Current status:

- frame conventions are not yet fully settled
- this is an active technical risk, not a solved foundation

What is currently known:

- incorrect frame or sign assumptions can produce heading shifts during roll and
  pitch
- acceleration rejection behavior can be triggered by bad accel frame
  interpretation
- ad hoc sign flips are not acceptable unless justified by explicit frame math

Current source files for this work:

- [Teensy/src/imu_fusion.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/Teensy/src/imu_fusion.cpp)
- [Teensy/src/imu_fusion.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/Teensy/src/imu_fusion.h)

Useful current debug outputs:

- `showyawcmp`
- `gethdg`
- `showimudata`
- `showimuerror`
- synthetic / fixed-magnetometer tests are planned to isolate tilt compensation

Rule:

No new heading, fusion, or display interpretation work should assume the frame
problem is solved unless explicitly revalidated.

---

## 11. Validation Approach

### Bench validation

- static sensor checks
- accel / gyro calibration verification
- magnetometer calibration verification
- UART transport checks
- logging throughput checks
- startup-order and connectivity-recovery checks

### Connectivity and startup recovery

The project also has an explicit bench-validation concern around startup stalls
and reconnect lockout behavior.

Current connectivity validation intent:

- startup order should not matter
- temporary connectivity loss should not create permanent lockout
- telemetry, UI, and logging should resume without manual reset

Current practical acceptance targets:

- `ESP_GND` AP ready within about `15 s`
- `ESP_AIR` radio ready within about `10 s`
- AIR <-> GND link ready within about `30 s` after both are booted
- AIR <-> Teensy telemetry link ready within about `30 s`
- live telemetry stall threshold about `4 s`

Current recommended soak coverage:

- full startup-order matrix across `GND`, `AIR`, and `Teensy`
- reset and power-cycle interruption matrix for each unit
- browser join/leave cycling while telemetry is active
- logging on/off while telemetry is active

Useful support artifact:

- `scripts/monitor_stalls.ps1` can be used to capture multi-port serial logs,
  `events.csv`, and `summary.json` during startup/recovery testing

### Controlled motion tests

- slow roll and pitch checks
- yaw rotation checks
- synthetic magnetometer vector injection
- earth-frame versus body-frame magnetometer comparisons

### Flight tests

Performed only after sufficient bench confidence.

### Validation principle

A result must not be accepted only because it "looks right."

Whenever possible compare against:

- physical expectation
- explicit frame math
- independent GPS-derived reference
- known static orientation

### Current validation gaps

- frame conventions still need a controlled synthetic magnetometer test
- acceleration rejection behavior during roll and pitch is not fully explained
- SD logging backend has not yet been validated under real transport load
- SPI/DMA transport has been validated as a standalone prototype but not yet
  integrated into the main avionics firmware path

---

## 12. Current Development State

Known working or mostly working:

- Teensy IMU fusion loop runs at about 400 Hz
- GPS NAV-PVT parsing is implemented
- shared telemetry struct and control messages exist
- AIR <-> GND ESP-NOW link exists
- GND web UI and WebSocket streaming exist
- PFD / HSI generation by AI was highly effective for structure and data flow
- standalone Teensy <-> ESP SPI/DMA prototype now works as a clean framed
  bidirectional transport experiment

Current areas under active investigation:

- frame correctness for heading and tilt compensation
- acceleration rejection during roll / pitch tests
- SD card backend integration on ESP_AIR
- integration strategy for the standalone SPI/DMA transport prototype
- display refinement and pilot-facing presentation quality

Current logging state:

- filesystem logger exists on AIR using LittleFS
- SD interface testing has begun but is not yet a reliable subsystem
- SPI transport batching appears compatible with logger-friendly chunk sizes, but
  this has not yet been tested in the integrated AIR logger path

---

## 13. Known Risks And Questions

Known risks:

- magnetometer frame ambiguity
- heading instability during roll / pitch
- acceleration rejection during maneuvers
- SD logging performance under load
- SPI transport integration complexity versus keeping the current UART mirror
- display logic being mistaken for sensor truth

Open questions:

- best roll estimation during coordinated turns
- GPS support for inertial validation
- improved energy awareness displays
- final earth/body/magnetic frame convention
- best offline or on-ground binary-to-text conversion workflow
- whether SPI should replace UART for bulk transfer or coexist as an optional
  high-rate path

---

## 14. Display Development Lessons

The AI-generated PFD / HSI work was highly productive but exposed an important
workflow limitation.

Observed result:

- AI generated most of the display structure correctly
- data flow plumbing was especially helpful
- the last 10 percent required many visual edits, additions, and removals
- doing detailed visual refinement through chat was slow and inefficient

Implication:

- AI is effective at display scaffolding and data binding
- a graphical or direct-manipulation editor would likely accelerate visual
  refinement much more than additional text-only iteration

Desired future workflow:

- AI generates structure, state bindings, and initial layouts
- direct graphical editing adjusts geometry, styling, and composition
- AI then reconciles the edited visual design back into maintainable code

This should be treated as a tooling objective for future work, not just a
lesson learned.

---

## 15. Engineering Philosophy Of This Project

This is not primarily a product development effort.

It is an experimental avionics laboratory exploring:

- pilot-centric instrumentation
- AI-assisted engineering workflows
- rapid prototype cycles

The most valuable outcomes may be:

- working experimental instruments
- engineering insights
- improved engineering workflows

---

## 16. AI Session Briefing

Use the following as a starting brief for future AI sessions.

Project:

Experimental avionics laboratory using a Teensy flight computer and ESP32-based
telemetry / display nodes.

Architecture:

- Teensy 4.0: sensors, fusion, deterministic flight-state processing
- ESP_AIR: UART ingest, logging backend, ESP-NOW transport, diagnostics
- ESP_GND: AP, WebSocket server, browser display, ground interaction

Sensors:

- BMI270 IMU
- BMM150 magnetometer
- u-blox M10 GPS via NAV-PVT
- BMP280 barometer

Primary source-of-truth files:

- [Teensy/src/imu_fusion.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/Teensy/src/imu_fusion.cpp)
- [Teensy/src/main.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/Teensy/src/main.cpp)
- [ESP_AIR/src/types_shared.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/types_shared.h)
- [ESP_AIR/src/uart_telem.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/uart_telem.cpp)
- [ESP_AIR/src/radio_link.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/radio_link.cpp)
- [ESP_AIR/src/log_store.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/log_store.cpp)
- [ESP_GND/src/ws_server.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/src/ws_server.cpp)
- [ESP_GND/data/app.js](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_GND/data/app.js)

Goals:

- pilot-centric instrumentation
- new flight display concepts
- deterministic flight processing
- robust transport and logging
- evaluate a standalone SPI/DMA bulk transport path without breaking the current
  UART-based avionics stack

Constraints:

- flight processing is primary
- logging must be non-blocking
- networking is secondary
- system must remain debuggable
- frame assumptions must be justified, not guessed

Current caution:

- coordinate-frame and heading behavior are still under investigation
- do not treat current heading math as closed unless validated again
- SPI/DMA transport results are promising but are still prototype results until
  integrated and retested inside the avionics firmware

---

## 17. Repository Structure

Current working tree structure:

- `Teensy/` - flight computer firmware
- `ESP_AIR/` - airborne transport / logging firmware
- `ESP_GND/` - ground node firmware and web assets
- `../SPI_DMA_Transport_Prototype/` - standalone transport benchmark and
  protocol prototype

Reference-document intent:

- `AI_AVIONICS_PROJECT_RECORD.md` is the project memory / history file
- subsystem `README.md` files are live component references
- pipeline spec / implementation plan files are design references, not the
  canonical project history

When resuming work:

- start from the subsystem owning the behavior
- use `types_shared.h` as the data contract anchor
- do not modify only one side of a transport pair

---

## 18. Record Maintenance

Update this document when major changes occur in:

- architecture
- data contracts
- frame conventions
- validation approach
- logging backend
- lessons learned
- AI workflow assumptions

If a new AI session had to rediscover an important fact from code or chat, this
document is missing something and should be updated.
