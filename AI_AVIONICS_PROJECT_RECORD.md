# AI-Assisted Experimental Avionics Laboratory

Version: 0.3
Author: Carl Suttle
Date: 2026
Updated: 2026-03-15

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

## 7. Flight State Data Model

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

## 8. Logging Architecture

Current authoritative implementation:

- [ESP_AIR/src/log_store.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/log_store.cpp)
- [ESP_AIR/src/log_store.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/ESP_AIR/src/log_store.h)

Current backend:

- `LittleFS`
- binary `.tlog` session files in `/logs`

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

Current operational rule:

- logging must not block flight processing or UART ingest
- if logging falls behind, bounded record loss is preferable to blocking

Important limitation:

- current production logging is still `LittleFS`-based
- SD logging is under development and should be treated as a separate backend
  effort, not a small patch to the existing logger

---

## 9. Coordinate Frames

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

## 10. Validation Approach

### Bench validation

- static sensor checks
- accel / gyro calibration verification
- magnetometer calibration verification
- UART transport checks
- logging throughput checks

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

---

## 11. Current Development State

Known working or mostly working:

- Teensy IMU fusion loop runs at about 400 Hz
- GPS NAV-PVT parsing is implemented
- shared telemetry struct and control messages exist
- AIR <-> GND ESP-NOW link exists
- GND web UI and WebSocket streaming exist
- PFD / HSI generation by AI was highly effective for structure and data flow

Current areas under active investigation:

- frame correctness for heading and tilt compensation
- acceleration rejection during roll / pitch tests
- SD card backend integration on ESP_AIR
- display refinement and pilot-facing presentation quality

Current logging state:

- filesystem logger exists on AIR using LittleFS
- SD interface testing has begun but is not yet a reliable subsystem

---

## 12. Known Risks And Questions

Known risks:

- magnetometer frame ambiguity
- heading instability during roll / pitch
- acceleration rejection during maneuvers
- SD logging performance under load
- display logic being mistaken for sensor truth

Open questions:

- best roll estimation during coordinated turns
- GPS support for inertial validation
- improved energy awareness displays
- final earth/body/magnetic frame convention
- best offline or on-ground binary-to-text conversion workflow

---

## 13. Display Development Lessons

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

## 14. Engineering Philosophy Of This Project

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

## 15. AI Session Briefing

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

Constraints:

- flight processing is primary
- logging must be non-blocking
- networking is secondary
- system must remain debuggable
- frame assumptions must be justified, not guessed

Current caution:

- coordinate-frame and heading behavior are still under investigation
- do not treat current heading math as closed unless validated again

---

## 16. Repository Structure

Current working tree structure:

- `Teensy/` - flight computer firmware
- `ESP_AIR/` - airborne transport / logging firmware
- `ESP_GND/` - ground node firmware and web assets

When resuming work:

- start from the subsystem owning the behavior
- use `types_shared.h` as the data contract anchor
- do not modify only one side of a transport pair

---

## 17. Record Maintenance

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
