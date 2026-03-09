# Telemetry Pipeline Implementation Plan

Status: draft

Date: 2026-03-07

Related spec:
- [TELEMETRY_PIPELINE_SPEC.md](c:/Users/dell/Platformio/esp32_crsf_telemetry/FAST_Teensy_Avionics/TELEMETRY_PIPELINE_SPEC.md)

## 1. Goal

Implement a stable telemetry architecture with these operating targets:

- `50-100 Hz` source telemetry
- `50-100 Hz` authoritative ESP32 logging
- `20 Hz` human UI
- reliable control independent of telemetry
- clean reconnect without manual refresh

## 2. Project Principle

Do not continue tuning the mixed single-path design.

Instead:

1. split browser control and telemetry paths
2. make browser telemetry latest-only
3. cap human rendering at `20 Hz`
4. isolate ESP32 logging from browser load
5. add minimal but decisive observability

## 3. Phases

## Phase 0: Freeze Baseline

Purpose:

- create a known rollback point before structural refactor

Tasks:

- commit current state of:
  - `esp32_air_unit_ws_bridge`
  - `FAST_Teensy_Avionics`
- record current observed behavior:
  - source rate
  - UI rate
  - logging rate
  - average control ping
  - stale telemetry behavior

Deliverable:

- baseline commit hash

Exit criteria:

- reproducible rollback point exists

## Phase 1: Split Browser Control and Telemetry

Purpose:

- stop control and ping from sharing telemetry backlog

ESP32 files:

- [ws_server.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/ws_server.cpp)
- [ws_server.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/ws_server.h)

Browser files:

- [app.js](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/data/app.js)
- [index.html](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/data/index.html)

Tasks:

- add dedicated control socket endpoint:
  - `/ws_ctrl`
- add dedicated telemetry socket endpoint:
  - `/ws_state`
- move these messages to control socket only:
  - ping and pong
  - config
  - `set_rate`
  - `set_fusion`
  - `get_fusion`
  - ACK and NACK
  - reconnect and health state
- move binary state stream to telemetry socket only

Notes:

- keep JSON on control socket
- keep binary state on telemetry socket

Exit criteria:

- control socket can stay healthy when telemetry socket is stressed
- ping is measured only on control socket

## Phase 2: Add Explicit Control Request Correlation

Purpose:

- make reliable browser control unambiguous end-to-end

ESP32 files:

- [ws_server.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/ws_server.cpp)
- [main.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/main.cpp)
- [uart_telem.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/uart_telem.cpp)
- [uart_telem.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/uart_telem.h)

Browser files:

- [app.js](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/data/app.js)

Tasks:

- add `req_id` to browser control messages
- return `req_id` on ACK and NACK
- map browser `req_id` to the Teensy command transaction until ACK/NACK resolves
- add timeout handling for commands that do not complete

Exit criteria:

- every browser control action has a deterministic ACK/NACK outcome
- control reliability no longer depends on reading streamed state to infer success

## Phase 3: Introduce Shared Latest-State Store

Purpose:

- define one clear handoff point between ingest, logging, and browser publish

ESP32 files:

- [uart_telem.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/uart_telem.cpp)
- [uart_telem.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/uart_telem.h)
- [types_shared.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/types_shared.h)

Tasks:

- define one shared latest-state object
- maintain monotonic `state_seq`
- include source timestamp and ESP32 receive time in the snapshot metadata
- ensure readers never see partially updated state

Recommended implementation:

- double buffer or short critical-section copy

Exit criteria:

- ingest updates are deterministic
- publish and logging can both snapshot state without long blocking

## Phase 4: Make Telemetry Socket Latest-Only

Purpose:

- browser does not need every frame
- old telemetry must not delay new telemetry

ESP32 files:

- [ws_server.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/ws_server.cpp)

Tasks:

- replace per-client telemetry FIFO behavior with latest-only behavior
- maintain at most one unsent telemetry snapshot worth of work per client
- if a new state arrives before the previous state is sent:
  - overwrite pending telemetry with newest snapshot intent
- do not queue multiple telemetry frames per client

Preferred implementation:

- periodic publish task at `ui_publish_rate_hz`
- on each tick:
  - copy current latest state
  - attempt one non-blocking binary send per client
  - skip busy clients

Exit criteria:

- telemetry socket does not build queue depth under steady-state operation
- stale drops are intentional and bounded
- control ping remains low under telemetry load

## Phase 5: Cap Browser Rendering at 20 Hz

Purpose:

- browser must consume high-rate telemetry without rendering at high rate

Browser files:

- [app.js](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/data/app.js)
- optional later split into:
  - [telemetry.js](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/data/telemetry.js)
  - UI-specific modules

Tasks:

- maintain latest in-memory telemetry snapshot from `/ws_state`
- render visible UI from that snapshot at fixed `20 Hz`
- do not perform DOM writes inside high-rate telemetry `onmessage`
- keep Link and health information driven from `/ws_ctrl`
- do not blank fields during short telemetry gaps
- show stale status while holding last valid values

Exit criteria:

- UI no longer flashes to `-` under normal operation
- browser main thread remains responsive at `50 Hz` source telemetry

## Phase 6: Clean Reconnect State Machines

Purpose:

- reconnect should be predictable and independent per plane

Browser files:

- [app.js](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/data/app.js)

ESP32 files:

- [ws_server.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/ws_server.cpp)

Tasks:

- separate control reconnect from telemetry reconnect
- on control reconnect:
  - fetch or send config
  - refresh control-side state
- on telemetry reconnect:
  - send latest snapshot immediately or on first publish tick
- represent UI status as:
  - `Disconnected`
  - `Connected`
  - `Connected / stale telemetry`

Do not:

- reconnect control socket just because telemetry is stale
- reconnect telemetry because DOM rendering is delayed

Exit criteria:

- AP interruption recovers without page refresh
- telemetry stale does not cause reconnect loops

## Phase 7: Redesign ESP32 Logging as an Independent Authoritative Subsystem

Purpose:

- browser load must not affect recording correctness

ESP32 files:

- [log_store.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/log_store.cpp)
- [log_store.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/log_store.h)
- [types_shared.h](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/types_shared.h)
- [main.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/main.cpp)

Tasks:

- define compact binary log record format
- replace CSV hot-path writing with binary hot-path writing
- keep a bounded queue or ring between ingest and logging worker
- record:
  - log sequence
  - source timestamp
  - state payload
  - record CRC if needed
- move CSV generation to export time, not capture time

Validation requirement:

- LittleFS may be retained only if it passes sustained `50 Hz` logging validation with telemetry and browser load present
- if LittleFS fails, replace the ESP32 storage backend rather than moving logging to the Teensy

Exit criteria:

- no log record loss at validated operating mode
- logging load does not disturb control ping materially

## Phase 8: Simplify and Finalize Metrics

Purpose:

- make failures obvious instead of ambiguous

ESP32 files:

- [ws_server.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/ws_server.cpp)
- [log_store.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/src/log_store.cpp)

Browser files:

- [app.js](c:/Users/dell/Platformio/esp32_crsf_telemetry/esp32_air_unit_ws_bridge/data/app.js)

Tasks:

- separate control metrics from telemetry metrics
- separate intentional telemetry drop from loss and error
- expose:
  - control ping
  - control reconnects
  - command timeout count
  - telemetry source age
  - telemetry publish age
  - telemetry stale-drop count
  - logging queue metrics
  - UART integrity counters

Exit criteria:

- one stale event can be classified quickly as:
  - source stalled
  - ESP32 publish stalled
  - browser consume or render stalled
  - logger overloaded

## Phase 9: Extend to 100 Hz

Purpose:

- support `100 Hz` source and logging cleanly

Teensy files:

- [mirror.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/FAST_Teensy_Avionics/src/mirror.cpp)
- [main.cpp](c:/Users/dell/Platformio/esp32_crsf_telemetry/FAST_Teensy_Avionics/src/main.cpp)

ESP32 files:

- rate-control and validation files as required

Tasks:

- validate `100 Hz` source telemetry generation on the Teensy
- validate `100 Hz` source ingest on the ESP32
- validate `100 Hz` ESP32 authoritative logging
- keep browser publish and render at `20 Hz`

Exit criteria:

- `100/100/20` is proven or bounded with clear documented limits

## 4. Recommended Order of Execution

Implement in this order:

1. Phase 1: split sockets
2. Phase 2: request correlation
3. Phase 3: latest-state store
4. Phase 4: latest-only telemetry
5. Phase 5: fixed-rate browser render
6. Phase 6: reconnect cleanup
7. Phase 8: simplified metrics
8. Phase 7: ESP32 authoritative logging redesign
9. Phase 9: `100 Hz` extension

Reason:

- control stability first
- telemetry freshness second
- logging hardening third
- high-rate extension last

## 5. Testing Matrix

After each relevant phase, test the following:

### Test A

- `source=50`
- `ui_publish=20`
- `ui_render=20`
- `log=0`

Expected:

- control ping under `10 ms` average
- no stale telemetry in normal operation

### Test B

- `source=50`
- `ui_publish=20`
- `ui_render=20`
- `log=50`

Expected:

- no UI flashing
- no log loss
- control ping remains low

### Test C

- AP or client interruption and recovery

Expected:

- clean reconnect
- no page refresh required

### Test D

- `source=100`
- `ui_publish=20`
- `ui_render=20`
- `log=100`

Expected:

- browser may drop telemetry snapshots
- control remains reliable
- authoritative ESP32 log remains complete

### Test E

- artificially slow browser client

Expected:

- slow client does not degrade control path
- slow client does not create telemetry backlog replay

## 6. Risks

Main risks:

- AsyncWebSocket library behavior may still make perfect latest-only semantics awkward
- browser code may still accidentally couple control and telemetry state
- LittleFS may remain inadequate for sustained authoritative logging

Mitigations:

- keep telemetry socket minimal
- keep control socket JSON and simple
- isolate logger queue and worker early
- move to a better ESP32-side storage backend if LittleFS proves inadequate

## 7. Rollback Points

Create a commit after each of:

1. socket split
2. latest-only telemetry
3. reconnect cleanup
4. logging redesign

This refactor should not be done as one giant commit.

## 8. Definition of Done

The work is complete when:

- control and telemetry are separate
- browser telemetry is latest-only
- UI renders at `20 Hz`
- control ping is back to low stable values
- reconnect works without manual refresh
- logging is accurate and independent of browser telemetry delivery
- authoritative logging remains on the ESP32
