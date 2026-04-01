# AGENTS.md

## Project
FAST telemetry / AI-assisted avionics prototyping lab

This repository contains an embedded avionics prototype split across Teensy, ESP_AIR, and ESP_GND. The system is used for rapid prototyping, telemetry transport, deterministic logging/replay, and AI-assisted analysis of avionics architectures and sensor-fusion behavior.

Codex must preserve the architectural intent and system invariants below. Do not optimize locally in ways that violate system-level behavior.

---

## Primary engineering objective

Current branch objective:

- remove fixed schema-size coupling
- decouple schema definition from transport, logging, replay, and test harnesses
- preserve the proven release 0.02 behavior while preparing for schema evolution

This is an architecture refactor first, not a feature sprint.

Do not introduce unrelated features unless explicitly requested.

---

## System architecture

### Layers

1. Truth / source layer
- Real sensors or simulator-injected sensor truth
- Includes IMU, GPS, baro, and other source signals

2. Embedded processing layer
- Primarily Teensy
- Deterministic signal processing and state derivation
- Sensor fusion and aircraft-state computation live here unless explicitly documented otherwise

3. Transport and logging layer
- Primarily ESP_AIR
- Receives structured data from Teensy
- Responsible for bounded transport, telemetry fan-out, and logging

4. Analysis / display / replay layer
- ESP_GND, PC tools, browser tools, replay tools, and radios/display consumers

---

## Non-negotiable system invariants

### Logging and capture
1. Logging is highest priority.
2. UART/SPI ingestion required for logging must not lose records under the validated operating point.
3. SD logging and record capture are more important than browser telemetry or convenience streams.
4. Lossy telemetry paths are acceptable only where already intended by design.

### Replay and determinism
5. Replay is a first-class engineering function, not a debug convenience.
6. For identical recorded inputs and equivalent settings, replay must preserve timing semantics and produce equivalent downstream behavior.
7. Do not casually change timestamps, record ordering, batching semantics, sequence numbering, or replay pacing logic.

### Schema and transport separation
8. Transport code must not depend on hard-coded payload sizes or struct layouts except through explicit shared schema definitions.
9. Do not scatter literal record sizes such as 160 bytes through the codebase.
10. Schema version, record kind, and payload size must be defined centrally and consumed from a shared contract.
11. Schema evolution must be additive and explicit where possible.
12. Logging, replay, decode, and transport must not infer schema from incidental assumptions.

### Control and safety of refactors
13. Preserve existing validated behavior before extending it.
14. Prefer minimal, testable, high-confidence changes.
15. If a refactor touches data layout, update all dependent encode/decode/log/replay/test surfaces in the same change.
16. Do not silently “clean up” behavior that may be load-bearing.
17. If behavior is ambiguous, preserve it and leave a note rather than replacing it with a guess.

---

## Priority order for runtime behavior

Use this ordering when making tradeoffs:

1. data ingestion integrity
2. logging integrity
3. replay correctness
4. deterministic control/test interfaces
5. radio / telemetry fan-out
6. browser / UI convenience behavior

Browser streaming, websocket updates, and nonessential telemetry are never allowed to break capture or replay integrity.

---

## Refactor rules for schema work

When working on schema/version/refactor tasks:

1. Identify every dependency on:
- fixed record size
- fixed offsets
- direct struct memcpy assumptions
- implicit decode assumptions
- hard-coded log record lengths
- replay framing assumptions

2. Centralize:
- schema id/version
- record type ids
- payload sizes
- encode/decode helpers
- compatibility mapping if needed

3. Ensure:
- transport framing uses explicit metadata, not guessed struct size
- logging records enough metadata to identify schema/version
- replay selects the correct decode path explicitly
- tests cover both nominal behavior and schema mismatch behavior where practical

4. Do not merge partial schema changes that compile but leave logging, replay, or test tools inconsistent.

---

## Expected Codex workflow

For nontrivial changes, Codex should:

1. inspect the relevant architecture and data-contract files first
2. identify affected modules before editing
3. make coherent cross-file changes
4. update or add focused tests/scripts where useful
5. run the relevant checks or scripts
6. summarize:
- what changed
- what assumptions were preserved
- what remains unproven

If a requested change would violate the invariants above, say so explicitly and propose a safe alternative.

---

## Change scope guidance

Good tasks:
- remove hard-coded schema sizes
- centralize record metadata
- align replay/logging/decode paths
- tighten tests around transport/logging/replay
- improve diagnostics that expose queue depth, drops, timing, pacing, and schema id

Avoid unless requested:
- UI redesign
- unrelated renames
- speculative optimization
- changes to control semantics
- changing timing behavior “for neatness”

---

## Terminology

Use these meanings consistently:

- schema: the formal definition of record layout and meaning
- transport: the mechanism that moves records/messages between components
- logging: durable capture of records for later analysis/replay
- replay: regeneration of recorded traffic/data with controlled timing semantics
- validated operating point: the throughput/rate/batch condition already proven by tests on this branch

---

## Output expectations

When finishing a task, report:

1. files changed
2. invariants intentionally preserved
3. tests/scripts run
4. known risks or unproven areas
5. any remaining hard-coded assumptions not removed yet
