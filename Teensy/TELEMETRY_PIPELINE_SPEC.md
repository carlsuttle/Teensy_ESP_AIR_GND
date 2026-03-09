# Telemetry Pipeline Specification

Status: implementation spec

Date: 2026-03-07

Canonical location:
- `FAST_Teensy_Avionics`

Applies to:
- `FAST_Teensy_Avionics`
- `esp32_air_unit_ws_bridge`
- browser client served by the ESP32

## 1. Objective

Implement a telemetry architecture that separates:

1. reliable browser control
2. lossy latest-only browser telemetry
3. independent authoritative logging on the ESP32

The design must support high-rate source telemetry from the Teensy while keeping the browser UI responsive and preventing browser transport behavior from degrading logging or command reliability.

## 2. Core Design Decision

Do not treat the browser as the delivery endpoint for every telemetry sample.

Instead:

- the Teensy is the source of truth for live state generation
- the ESP32 logger is the source of truth for capture continuity
- the browser is only a viewer of the newest usable state

This means:

- control path: reliable, ordered, ACK/NACK
- telemetry path: lossy, latest-only
- logging path: buffered, independent, authoritative

## 3. Why Logging Stays on the ESP32

Authoritative logging shall remain on the ESP32.

Reason:

- Teensy processing budget must remain focused on flight-side tasks
- the Teensy is expected to handle additional work including:
  - CRSF input/output
  - six PWM servo outputs
  - dynamic-pressure sensing over I2C for airspeed
  - battery voltage/current measurement
  - engine power calculation
  - adding these values to CRSF, telemetry, logging, and browser output

Therefore:

- the Teensy should generate clean telemetry and accept reliable commands
- the ESP32 should own browser serving and authoritative logging

## 4. Required Functional Outcomes

The implementation shall support:

- Teensy source telemetry generation at `50-100 Hz`
- authoritative ESP32 logging at `50-100 Hz`
- browser-facing live telemetry updates at `20 Hz` default
- explicit command/ACK/NACK between browser and Teensy
- reconnect without page refresh
- browser telemetry delivery that may skip stale intermediate frames
- browser UI that does not flash values to `-` during normal operation

Target performance:

- average control RTT under `10 ms` in normal operation
- `p95` control RTT under `20 ms` in normal operation
- stable browser session for at least `10 minutes`
- no command loss
- no authoritative log loss in validated operating mode

## 5. Non-Goals

The system does not need:

- delivery of every telemetry frame to the browser
- browser rendering at `50-100 Hz`
- raw UDP inside the browser
- CSV as the hot-path log write format

## 6. Required Top-Level Architecture

### 6.1 Teensy to ESP32 ingress

Keep the current UART binary framed transport.

Required properties:

- framed packets
- CRC protected
- deterministic parser
- command ACK/NACK support
- telemetry packet timestamp or monotonic source sequence
- non-blocking decode on ESP32

This is the primary internal transport into the ESP32.

### 6.2 Browser control plane

Create a dedicated WebSocket endpoint:

- `/ws_ctrl`

Purpose:

- configuration
- AHRS/fusion settings
- start/stop logging
- stream-rate changes
- diagnostics requests
- ping/health

Required properties:

- ordered
- reliable
- explicit request id
- explicit ACK/NACK response
- independent from telemetry path

Recommended payload format:

- JSON text frames

### 6.3 Browser telemetry plane

Create a dedicated WebSocket endpoint:

- `/ws_state`

Purpose:

- GPS state
- attitude state
- baro state
- instrumentation state

Low-rate bridge and diagnostic counters shall stay on `/ws_ctrl` unless a specific UI need proves they belong on `/ws_state`.

Required properties:

- binary frame format
- latest-only semantics
- stale intermediate frames may be discarded
- independent from control path
- no deep queue per client

### 6.4 Logging plane

Purpose:

- authoritative capture for later analysis
- not dependent on browser connection health
- not dependent on browser rendering rate
- not blocked by WebSocket serving

Authoritative location:

- ESP32

Hot-path storage format:

- binary records

Export format:

- CSV generated later during download or conversion

## 7. Explicit Rate Model

Expose and treat these as different rates:

- `source_rate_hz`
  - rate at which Teensy emits state updates
  - target: `50-100`

- `log_rate_hz`
  - rate at which the authoritative ESP32 logger records samples
  - target: `50-100`

- `ui_publish_rate_hz`
  - max rate at which ESP32 publishes snapshots to browser clients
  - default: `20`

- `ui_render_rate_hz`
  - browser DOM or canvas update rate
  - default: `20`

Recommended defaults:

- `source_rate_hz = 50`
- `log_rate_hz = 50`
- `ui_publish_rate_hz = 20`
- `ui_render_rate_hz = 20`

Important rule:

The ESP32 must not publish one browser frame for every ingress telemetry frame unless explicitly configured to do so.

## 8. Data Flow Model

The intended pipeline is:

1. Teensy emits binary telemetry packet over UART
2. ESP32 decodes it immediately
3. ESP32 updates one shared latest-state structure
4. ESP32 enqueues one binary log record to the logging worker if logging is enabled
5. ESP32 publishes snapshots to browser clients at bounded UI publish rate
6. browser stores the newest received snapshot in memory
7. browser renders at fixed rate from the newest snapshot

This means browser rendering is decoupled from UART arrival timing.

## 9. Latest-Only Telemetry Semantics

This is the most important behavior in the spec.

Required implementation semantics:

- maintain one shared decoded latest-state object on ESP32
- maintain a monotonic `state_seq`
- each client connection may have:
  - send-in-progress flag
  - last sent `state_seq`
  - one pending latest snapshot marker or equivalent accounting
- do not maintain a FIFO of telemetry state frames per client
- if a new source state arrives before an older unsent state is published, only the newest state matters
- if a client is temporarily slow, it receives a newer snapshot later rather than a backlog

Operational definition:

- there may be at most one unsent telemetry snapshot worth of work per client above the WebSocket library's internal queueing

Preferred implementation pattern:

- periodic publish task at `ui_publish_rate_hz`
- on each tick, serialize the current latest state once per client send opportunity
- if the WebSocket layer reports send still busy for that client, skip that publish tick for that client
- next publish attempt uses the newest state, not queued stale states

Forbidden behavior:

- unbounded telemetry queue per client
- per-packet browser publish on every ingress sample
- telemetry backlog replay after temporary congestion

## 10. Reconnect and Status Model

Control and telemetry sockets must reconnect independently.

### 10.1 Control reconnect

- auto reconnect with backoff
- on connect, browser requests current config and state summary
- control health is based only on `/ws_ctrl`

### 10.2 Telemetry reconnect

- auto reconnect independently
- on connect, ESP32 should send the latest snapshot immediately or on first publish tick
- reconnecting telemetry must not require page refresh
- telemetry reconnect failure must not force control reconnect

### 10.3 UI connection states

The UI shall expose at least:

- `Disconnected`
- `Connected`
- `Connected / stale telemetry`

Suggested meaning:

- `Disconnected`
  - control socket disconnected, or no valid session established

- `Connected`
  - control socket up and latest telemetry age below stale threshold

- `Connected / stale telemetry`
  - control socket up but latest telemetry age exceeds threshold

Suggested stale threshold:

- stale if `telemetry_state_age_ms > max(250 ms, 3 * ui_publish_period_ms)`

This threshold may be tuned later.

## 11. Required Sequence Numbers and Timestamps

### 11.1 Telemetry snapshot

Every telemetry snapshot shall include:

- `state_seq` - monotonic sequence assigned at source ingest
- `source_time_us` or `source_time_ms` - source timestamp from Teensy if available
- `esp_rx_time_ms` - receive timestamp on ESP32
- version and payload size fields

### 11.2 Control requests

Every control request shall include:

- `req_id`

Every control response shall include:

- `req_id`
- `ok`
- `error_code`
- optional `message`

The ESP32 shall map browser `req_id` to the corresponding Teensy command sequence or command transaction state so ACK/NACK correlation remains unambiguous end-to-end.

### 11.3 Log records

Each authoritative log record should include:

- `log_seq`
- source timestamp
- record type or version

This makes continuity verification possible.

## 12. Binary Snapshot Format for `/ws_state`

Keep the browser state socket binary.

Recommended packet layout:

### 12.1 Header

```c
struct WsStateHeaderV1 {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t payload_size;
    uint32_t flags;
    uint32_t state_seq;
    uint32_t source_time_ms;
    uint32_t esp_rx_time_ms;
};
```

### 12.2 Payload

Phase 1 requirement:

- keep payload close to the currently proven telemetry struct
- include only fast-changing state needed by the live UI
- keep low-rate diagnostic counters out of this payload

Suggested contents:

- GPS state
- attitude state
- fusion state
- baro state
- validity and status flags

Optional later expansion:

- full instrumentation payload
- extra rates and derived values
- compact validity groups

Notes:

- exact fields may be adjusted to match current project structs
- preserve alignment intentionally
- include validity flags so the UI can distinguish invalid from stale from absent

## 13. JSON Control Message Format for `/ws_ctrl`

Recommended browser to ESP32 command:

```json
{
  "type": "cmd",
  "req_id": 42,
  "cmd": "set_fusion",
  "args": {
    "gain": 0.10,
    "accelerationRejection": 3.0,
    "magneticRejection": 90.0,
    "recoveryTriggerPeriod": 200
  }
}
```

Recommended ACK response:

```json
{
  "type": "ack",
  "req_id": 42,
  "ok": true
}
```

Recommended NACK response:

```json
{
  "type": "ack",
  "req_id": 42,
  "ok": false,
  "error_code": "BAD_ARG",
  "message": "gain out of range"
}
```

Recommended control ping:

```json
{
  "type": "ping",
  "req_id": 99,
  "t0_ms": 1234567
}
```

Response:

```json
{
  "type": "pong",
  "req_id": 99,
  "t0_ms": 1234567
}
```

Important rule:

Control RTT must be measured only over `/ws_ctrl`.

Do not infer telemetry health from control ping.

## 14. Logging Architecture Requirements

### 14.1 Logging independence

Logging shall not execute on the WebSocket hot path.

Required model:

- ingress or decode updates latest state
- log worker receives compact binary records via bounded queue or ring
- storage writes occur in dedicated worker context
- temporary storage latency may consume log buffer headroom
- storage latency may not block control handling
- storage latency may not block telemetry publish servicing

### 14.2 Storage constraints

LittleFS may remain the initial on-device storage target, but it shall not be assumed adequate without validation.

Validation must include:

- sustained run duration
- queue occupancy tracking
- worst-case write latency
- Wi-Fi and browser load present
- clean shutdown behavior where relevant

If LittleFS cannot satisfy the required validated mode on the ESP32, the solution shall move to a better ESP32-side storage backend, not back to the Teensy.

### 14.3 Record format

Recommended hot-path log record:

```c
struct LogRecordV1 {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint32_t log_seq;
    uint32_t source_time_ms;
    TelemetryStatePayloadV1 state;
    uint32_t crc32;
};
```

CSV export should happen later, not on the write hot path.

## 15. Metrics and Observability

Expose metrics separately by subsystem.

### 15.1 Control metrics

- `ctrl_ping_ms_last`
- `ctrl_ping_ms_avg`
- `ctrl_reconnects`
- `ctrl_ack_timeout_count`
- `ctrl_cmd_sent_total`
- `ctrl_cmd_failed_total`

### 15.2 Telemetry metrics

- `source_rx_fps`
- `source_seq_last`
- `ui_pub_fps`
- `telemetry_state_age_ms`
- `telemetry_pub_age_ms`
- `telemetry_publish_skipped_busy`
- `telemetry_publish_sent_total`
- `telemetry_client_reconnects`
- `telemetry_drop_stale_count`

Important note:

Do not count intentional stale snapshot replacement as generic packet loss.

### 15.3 Logging metrics

- `log_queue_cur`
- `log_queue_max`
- `log_records_enqueued_total`
- `log_records_written_total`
- `log_records_dropped_total`
- `log_bytes_written_total`
- `log_write_max_latency_ms`

### 15.4 UART and ingress integrity metrics

- `uart_crc_err`
- `uart_cobs_err`
- `uart_len_err`
- `uart_drop`
- `uart_frames_ok`

## 16. Suggested ESP32 Task Structure

### 16.1 UART ingest task

Responsibilities:

- read UART bytes
- frame decode
- CRC verify
- parse telemetry packets
- parse command responses from Teensy
- update shared latest-state structure
- increment `state_seq`
- enqueue log record if logging enabled

Priority intent:

- highest data-path priority

### 16.2 Control socket handler

Responsibilities:

- serve `/ws_ctrl`
- parse JSON commands
- send commands to Teensy if required
- await or synthesize ACK/NACK
- track control ping and request ids

Priority intent:

- high
- must remain responsive under telemetry load

### 16.3 Telemetry publish task

Responsibilities:

- tick at `ui_publish_rate_hz`
- read current latest state
- serialize binary snapshot
- attempt non-blocking send to each connected client
- skip clients that are still busy
- never queue deep stale telemetry

Priority intent:

- medium

### 16.4 Logging worker

Responsibilities:

- drain log queue
- write binary records to storage
- update queue and latency metrics
- handle file open, close, and rollover if implemented

Priority intent:

- medium to lower than ingest and control, but isolated

### 16.5 File export and HTTP download path

Responsibilities:

- later CSV conversion
- file download
- non-hot-path maintenance

Priority intent:

- lowest

## 17. Shared State Rules

Use one shared latest-state object and guard it correctly.

Acceptable approaches:

- small mutex around copy-in and copy-out
- double buffer with sequence tagging
- critical section only around pointer or sequence swap

Requirements:

- readers must never observe partially updated state
- telemetry publish must not block ingest for long
- browser publish path must copy or snapshot quickly, then send outside critical section

Recommended pattern:

1. ingest builds decoded state into working buffer
2. brief lock or atomic swap into shared latest state
3. publish task copies latest state locally
4. publish task sends local copy outside lock

## 18. Browser Client Requirements

### 18.1 Control client

- maintain `/ws_ctrl`
- send commands with `req_id`
- match ACK/NACK by `req_id`
- run ping on this channel only
- reconnect with backoff

### 18.2 Telemetry client

- maintain `/ws_state`
- parse binary header and payload
- verify version and sizes
- keep only newest received snapshot in memory
- do not render directly from the receive callback
- reconnect independently

### 18.3 Render loop

- fixed timer at `20 Hz`
- render from latest in-memory snapshot
- if no fresh data, hold last valid display and indicate stale status
- do not blank to `-` during short telemetry gaps unless data is explicitly invalid

This is important for instrument behavior. Brief delivery gaps should not create visual flicker.

## 19. Required Browser UI Behavior

The UI shall:

- show separate control and telemetry health
- show telemetry age
- show source receive FPS
- show UI publish FPS if available
- show control ping measured from `/ws_ctrl`
- show stale telemetry state clearly
- not claim healthy telemetry only because control ping works
- avoid flicker when data is briefly delayed

## 20. Acceptance Tests

### 20.1 Baseline test at 50/50/20

Configuration:

- `source_rate_hz = 50`
- `log_rate_hz = 50`
- `ui_publish_rate_hz = 20`
- `ui_render_rate_hz = 20`

Pass criteria:

- average control ping under `10 ms`
- `p95` control ping under `20 ms`
- no visible UI flashing to `-`
- telemetry age remains bounded during steady operation
- no command loss
- no authoritative log loss
- reconnect after AP interruption without page refresh

### 20.2 Stress test at 100/100/20

Configuration:

- `source_rate_hz = 100`
- `log_rate_hz = 100`
- `ui_publish_rate_hz = 20`

Pass criteria:

- control remains reliable
- authoritative log remains complete
- browser telemetry may skip density but remains fresh and usable
- reconnect still works cleanly
- telemetry publish skips stale states rather than building backlog

### 20.3 Slow-client test

Introduce an artificially slow browser client.

Pass criteria:

- slow client does not degrade control path
- slow client does not degrade other clients
- slow client does not force telemetry backlog growth
- latest-only semantics remain in effect

### 20.4 Storage-latency test

Inject storage pauses.

Pass criteria:

- control remains responsive
- telemetry serving remains responsive
- logging queue absorbs temporary latency within configured budget
- metrics expose queue growth and any drops

## 21. Concrete Constraints

The implementation must preserve the following:

- existing Teensy binary transport unless specific parser changes are requested
- explicit ACK/NACK semantics for commands
- binary telemetry state payload for `/ws_state`
- browser compatibility with Safari on iPhone and iPad and standard desktop browsers
- no design that requires browser UDP support
- no design that blocks ingest on browser send completion

The implementation must avoid:

- one WebSocket for both control and telemetry
- telemetry ping on the same path as telemetry state frames
- per-client deep telemetry queues
- CSV generation on the hot-path logger
- UI rendering on every incoming packet
- display blanking on short telemetry stalls

## 22. Later Enhancements

Not required for first implementation, but compatible with this design:

- multiple browser clients
- binary delta packets
- compressed state packets
- selective subscriptions by page or tab
- WebTransport in future if platform support becomes attractive
- improved ESP32-side storage backend
- replay mode using recorded binary logs

## 23. Final Summary

Implement the browser interface as two separate channels:

- `/ws_ctrl` for reliable commands, ACK/NACK, and ping
- `/ws_state` for binary latest-only state snapshots

Implement logging as a third independent ESP32 subsystem.

The browser is a fresh-state viewer, not the transport-of-record for every sample.

The success condition is:

- reliable control
- fresh stable UI
- complete authoritative logs on the ESP32
- clean reconnect
- no backlog-driven telemetry collapse
