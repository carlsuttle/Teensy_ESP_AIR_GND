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
- mount attempts `26 MHz` first
- fallback mount at `20 MHz` if `26 MHz` does not mount

Standalone SD logging benchmark state:

- SD logging was standalone benchmarked successfully and has now also been validated in the integrated AIR capture path at `26 MHz` for a full `60 s` run with live transport active
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

- frame conventions still need a controlled synthetic magnetometer test for final sign / earth-body closure
- acceleration rejection behavior during aggressive motion is now observable in replay, but final tuning is still open
- SD logging backend has now been validated under integrated transport load at a stable `26 MHz` SD clock
- SPI/DMA transport has now been integrated into the main avionics firmware path and validated with live web display plus AIR SD capture
- replay source -> Teensy -> rerecorded file integrity has now been validated for both stationary and motion sessions

---

## 12. Current Development State

Known working or mostly working:

- Teensy IMU fusion loop runs at about 400 Hz
- GPS NAV-PVT parsing is implemented
- shared telemetry struct and control messages exist
- AIR <-> GND ESP-NOW link exists
- GND web UI and WebSocket streaming exist
- PFD / HSI generation by AI was highly effective for structure and data flow
- integrated Teensy <-> AIR SPI/DMA transport is now running in the main stack and feeding the live AIR -> GND -> web path

Current areas under active investigation:

- frame correctness for heading and tilt compensation
- acceleration rejection during roll / pitch tests and later tuning work
- live record and replay workflow validation in the integrated stack over the web controls
- display refinement and pilot-facing presentation quality

Current logging state:

- filesystem logger exists on AIR using LittleFS
- AIR SD capture is now reliable at `26 MHz` using a dedicated SPI host
- integrated `sdcap1m` has completed successfully with `3000` records, `528000` bytes, and `0` dropped records

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

---

## 19. Integrated Transport And SD Validation (2026-03-21)

A dedicated integrated test session was completed on March 21, 2026. Full detail is recorded in:

- `INTEGRATION_TEST_REPORT_2026-03-21.md`

Verified hardware during this session:

- Teensy 4.0 on `COM10`
- `ESP_AIR` on `COM7`
- `ESP_GND` on `COM9`
- Teensy IMU over I2C
- Teensy GPS using UBX binary parsing over UART
- Teensy <-> AIR SPI/DMA transport
- AIR SD logging/capture path
- AIR <-> GND radio/web path

Integrated issues found and resolved:

- Teensy pin `13` LED activity conflicted with SPI `SCK` in the full firmware and had to be disabled while SPI mirror/transport is enabled
- AIR SD originally reused the same ESP SPI host as the transport and had to be moved onto a separate SPI controller
- AIR `spi_fail` originally counted normal `READY` wait gaps as failures and was corrected so that normal streaming now runs with `spi_fail=0`
- AIR SD sustained capture at `40 MHz` was not stable even though simple probe/write tests succeeded

Integrated results verified during this session:

- live telemetry reached the GND web client and displayed correctly
- integrated SPI/DMA transport stayed up in the full stack
- AIR `spi_fail=0` in normal live streaming after timing/accounting fix
- `sdcap1m` completed successfully for `60 s` at `26 MHz`
- successful integrated capture result:
  - `records=3000`
  - `bytes=528000`
  - `dropped=0`
  - `queue_max=2`
  - `err=(none)`

Current integrated operating point after this validation:

- AIR SD should currently be treated as stable at `26 MHz`
- AIR SD fallback is `20 MHz`
- `40 MHz` should be treated as standalone-capable but not yet integrated-capture-stable

Practical conclusion:

- the work has moved from prototype-only transport validation into a functioning integrated system
- the next major verification steps are live record workflow and replay workflow validation through the web GUI rather than basic transport bring-up

---

## 16. Replay Validation Addendum

This section records the direct replay-integrity work completed after the initial
integrated SPI/DMA + SD bring-up.

### Objective

Verify that recorded source sensor/state data can be:

- written to AIR SD
- replayed from AIR back into the Teensy
- rerecorded on AIR
- compared against the original source log

with all non-fusion fields preserved, while allowing the fusion outputs to be
recomputed.

### AIR replay/log tools added

The AIR console now includes tools to support this workflow:

- `verifylog`
- `expandlogs`
- `comparelogs <src> <dst>`
- `replaycapture`
- `replaycapfile <name>`
- `latestlog`
- `latestlogsession <n>`
- `logstartid <n>`

These support deterministic source selection, byte-exact SD verification, and
direct source-vs-rerecorded comparison.

### Root cause found in replay path

A structural replay bug was found in the Teensy replay IMU path:

- replay IMU samples were being queued into the same frame path as live IMU
- the live frame-averaging logic did not preserve `processed_body`
- replayed body-frame samples could therefore be transformed a second time
- replay mode was also averaging queued replay samples instead of consuming them
  one-by-one

That bug produced very large replay fusion errors, including apparent roll sign
reversal and large `mag_heading` mismatch, even when the copied raw fields were
correct.

The fix was:

- preserve `processed_body` through averaged frames
- bypass averaging in replay mode and consume one queued replay sample at a time
- preserve replay source `seq` / `t_us`
- use per-record source shadows for the non-fusion output fields

### Stationary replay result after fix

Validated pair:

- source: `air_130_238447.tlog`
- replay output: `air_131_316437.tlog`

Result:

- bounded startup prefix only:
  - `dst_prefix=6`
  - `seq_mismatch=7`
  - `ts_mismatch=7`
- copied raw fields all matched:
  - `imu_mismatch=0`
  - `mag_mismatch=0`
  - `gps_mismatch=0`
  - `baro_mismatch=0`
  - `mask_mismatch=0`
- replay fusion input matched source very closely:
  - `accel_input_mean_abs=(0.000247,0.000250,0.000251)`
- fusion outputs were very close:
  - `roll_mean_abs=0.018657`
  - `pitch_mean_abs=0.124635`
  - `yaw_mean_abs=0.132151`
  - `maghdg_mean_abs=0.019909`

Interpretation:

- the replay path is now correct for stationary data apart from the known
  startup alignment prefix

### Motion replay result after fix

Validated pair:

- source: `air_140_478029.tlog`
- replay output: `air_141_554078.tlog`

Result:

- bounded startup prefix only:
  - `dst_prefix=7`
  - `seq_mismatch=6`
  - `ts_mismatch=6`
- copied raw fields all matched:
  - `imu_mismatch=0`
  - `mag_mismatch=0`
  - `gps_mismatch=0`
  - `baro_mismatch=0`
  - `mask_mismatch=0`
- replay fusion input again matched source very closely:
  - `accel_input_mean_abs=(0.000254,0.000253,0.000249)`
- fusion outputs stayed sane under motion:
  - `roll_mean_abs=0.834425`
  - `pitch_mean_abs=0.722921`
  - `yaw_mean_abs=0.999702`
  - `maghdg_mean_abs=0.022302`
- motion replay showed real filter behavior rather than replay corruption:
  - `accel_ignored=23`
  - `mag_ignored=331`

Interpretation:

- no remaining replay sign reversal was observed
- the remaining differences are now normal fusion / rejection behavior under
  dynamic motion, not transport or replay corruption

### Practical conclusion

The replay loop is now credible for algorithm development:

- record a real source session
- replay the exact source file into the Teensy
- rerecord the returned state
- compare source vs rerecorded logs
- study fusion-output changes after code or tuning changes

This moves replay validation out of the "open transport risk" category and into
the "usable tuning workflow" category.

---

## 17. Web Workflow / Recorder / Radio Follow-on Notes (March 23, 2026)

This section records the integrated web-control and radio-link work completed
after replay integrity had already been validated.

### Results verified in the live stack

- replay can now be started from GND and exercised through the web GUI using
  actual AIR log files
- a newly recorded motion session was replayed successfully and the web
  instruments responded correctly
- the default tab is now `PFD`
- the old separate `GPS` and `Baro` tabs were merged into a single
  `Position` tab with barometric data rendered below GPS data
- the top header recorder control was simplified into a single start/stop
  recorder button with `1 Hz` flash while active
- the replay library now returns the full file list again after scaling the
  GND-side file cache and chunk tracking and pacing AIR file-list chunks

### Techniques that worked

- the recorder UI is now driven from explicit AIR log-status flags rather than
  inferred link metadata
- a dedicated `busy` log-status flag was added so the browser can distinguish
  "actively recording" from "opening/closing/finalizing a file"
- replay-file refresh is now deferred while AIR is actively recording or
  finalizing a log, then flushed when the logger becomes idle
- AIR now aborts the active log session cleanly if SD media/backend disappear,
  rather than wedging the whole recorder state
- the browser DQI path now suppresses false penalties during deliberate control
  transitions such as replay and record start/stop
- GND remote file tracking was expanded from the earlier small cache to a
  chunked, scalable `256`-file model, and AIR file-list transmission is now
  lightly paced between chunks

### Difficulties found

- replay/control activity initially made the browser look broken even when the
  backend transport path was healthy; page reloads helped expose that this was a
  client-state/DQI issue rather than a Teensy/AIR telemetry failure
- file operations were difficult to reason about because browser, GND, AIR, and
  SD behavior were all asynchronous; rename/delete correctness and file-list
  freshness had to be separated as different problems
- start/stop recording could temporarily depress FPS/DQI because intentional
  control-side discontinuities were being scored as telemetry quality failures
- SD removal during recording originally left the recorder in a bad state and
  could stall the UI until the recorder/session-abort logic was hardened

### ESP-NOW LR finding

An important radio compatibility finding was made during this session:

- enabling `WIFI_PROTOCOL_LR` on GND's SoftAP made the `Telemetry` Wi-Fi
  network disappear to normal client devices
- the web server remains on GND and must continue to present a normal Wi-Fi AP
  to the iPhone/browser
- LR is therefore not currently active end-to-end on the working AIR <-> GND
  radio path

Current practical status:

- GND SoftAP is back on normal `11b/g/n` so the web client can connect
- AIR can still request LR on its STA side
- true bidirectional LR between AIR and GND remains a follow-on task and will
  require a cleaner GND AP/STA split so the user-facing AP is not the same path
  carrying the AIR radio leg

### Practical conclusion

The integrated system is now substantially easier to operate from the browser:

- live recording is controlled from a single top-bar recorder button
- replay management is concentrated in the `Logs` tab
- file-list completeness is restored
- recorder/media failures are handled more explicitly

The main remaining radio architecture task is no longer basic link bring-up. It
is the clean separation required to keep:

- normal Wi-Fi from GND to the phone/browser
- LR-only radio behavior between AIR and GND

### SD removal follow-up

Additional live validation on March 23, 2026 refined the AIR SD-removal
behavior further:

- AIR now detects missing media while idle without permanently wedging the
  recorder state
- when the SD card is removed, the top recorder control greys out and the
  replay library becomes unavailable as intended
- live telemetry now recovers correctly when the card is reinserted, with
  library/recorder availability restored by explicit user refresh
- a small, temporary FPS/DQI dip still occurs for roughly `2-3 s` at the
  moment of physical card removal because AIR still performs one real SD health
  check (`cardType` / root open) before declaring media missing

Practical decision:

- this remaining dip is considered acceptable for now because it is brief,
  bounded, understandable, and far less risky than reintroducing aggressive
  background remount/reprobe behavior that previously destabilized the live
  telemetry path
