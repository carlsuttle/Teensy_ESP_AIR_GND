# ChatGPT Handoff: Release 0.03 RC1

Date:
- 2026-04-02

Repo:
- `C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND`

Branch:
- `teensy-source-rate-refactor`

Status:
- working `0.03 RC1` engineering state
- not a final release snapshot
- current repo worktree is dirty and includes important uncommitted work

---

## 1. What This Branch Is

This branch is the `0.03` architecture/refactor line.

Primary intent:

- preserve the proven `0.02` record/replay behavior
- remove fixed-schema coupling
- centralize schema ownership
- keep logging, replay, and deterministic test behavior intact
- add the minimum backend structure needed for schema evolution and browser/GND integration

This is not primarily a UI branch.
This is not a Teensy fast-path redesign branch.

---

## 2. Proven Current State

The following should be treated as already available and already proven at the current engineering level.

### A. Teensy DMA / fast path

- the Teensy DMA-backed capture / transfer path exists
- it remains the validated fast path
- it must not be casually redesigned

### B. Schema-defined transport

- schema-defined bidirectional AIR <-> GND transport exists
- bidirectional command / acknowledgement behavior is in place
- schema v2 is active in the current `0.03 RC1` work

### C. Record / replay / live proof envelope

The branch has a proven operating point around:

- `50Hz x 48`
- about `2400 rps`

This operating point is the comparison standard for changes.

### D. Stage 2 browser telemetry route

The single-client Stage 2 browser telemetry path over `ESP-NOW` has been measured and is acceptable at:

- `10 Hz`
- `30 Hz`

Measured conclusion:

- one browser client at either tested rate did not materially worsen the validated Teensy/AIR replay envelope
- the browser telemetry route is not currently the primary performance bottleneck

Important scope note:

- this statement applies to the tested single-client browser telemetry path
- it does not imply all browser control and SD workflows are fully optimized

### E. SD/file backend

An AIR-owned SD API now exists and is the authoritative backend for:

- file list refresh and paging
- storage status
- mount / eject
- delete / rename
- CSV export
- mode-based inhibition decisions
- explicit SD result codes

### F. GPS-derived UTC time service

An AIR-owned UTC wall-clock service now exists for:

- filesystem timestamps
- browser-visible time-valid state
- holdover behavior after GPS loss

Design intent:

- use GPS as the authoritative UTC source when trusted
- do not block startup, recording, or file operations on immediate GPS lock

---

## 3. Authoritative Backend Contracts Already In Repo

These documents exist and should be treated as the current branch-local contracts.

### Field / browser / instrumentation contract

- [STAGE2_FIELD_CONTRACT_0.03.md](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/docs/STAGE2_FIELD_CONTRACT_0.03.md)

Defines:

- authoritative engineering schema fields
- AIR -> GND live instrumentation subset
- browser snapshot fields
- browser control message scope

### SD API contract

- [SD_API_0.03.md](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/docs/SD_API_0.03.md)

Defines:

- AIR-owned SD/file backend ownership
- supported operations
- explicit SD result codes
- mode inhibition rules
- snapshot/paging model for file lists

### Time service contract

- [TIME_SERVICE_0.03.md](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/docs/TIME_SERVICE_0.03.md)

Defines:

- UTC wall-clock state model
- GPS trust rules
- unset / tentative / gps-valid / holdover behavior
- filesystem timestamp policy

### AIR control-plane design

- [AIR_CONTROL_PLANE_0.03.md](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/docs/AIR_CONTROL_PLANE_0.03.md)

Defines:

- AIR-owned control register / control manager direction
- thin-adapter model
- state / request / result register concepts
- migration target for serial, radio, and future interfaces

---

## 4. Current Architecture Summary

### Teensy

Owns:

- deterministic sensor processing
- fusion and aircraft-state computation
- DMA-backed fast capture / source path

Should not own:

- AIR filesystem time
- AIR SD/file logic
- browser logic

### ESP_AIR

Owns:

- transport fan-out
- logging
- replay bridge
- SD card and file operations
- filesystem wall clock
- increasingly, the authoritative control plane

### ESP_GND

Owns:

- browser-facing distribution
- websocket/UI shaping
- reduced browser snapshot generation
- forwarding browser controls toward AIR

Should not own:

- SD business logic
- file mode rules
- replay business logic
- fusion business logic

---

## 5. Current AIR Control-Path Situation

Direction of travel:

- AIR should own one authoritative control plane
- interfaces should become thin adapters

Current partial state:

- the refactor toward a shared AIR control plane has started
- AIR radio and some AIR console command paths have been moved toward a shared backend dispatch layer
- not every direct path is migrated yet

Important meaning for ChatGPT:

- do not add new control behavior by bypassing AIR and writing interface-local logic
- do not put SD/replay/fusion mode rules into GND/browser code
- new control work should extend the AIR-owned control plane, not fragment it

---

## 6. Runtime Priorities

When there is a tradeoff, preserve this order:

1. ingestion integrity
2. logging integrity
3. replay correctness
4. deterministic control/test behavior
5. radio / telemetry fan-out
6. browser / UI convenience

This matters.

Browser convenience is never allowed to break:

- capture
- logging
- replay
- the validated operating point

---

## 7. What Is Still Rough / Incomplete

These areas should be treated as still under active engineering cleanup.

### A. Worktree state

- the repo is currently dirty
- important functionality exists in uncommitted files
- there is not one clean committed hash that represents the entire current `0.03 RC1` working state

### B. AIR control-plane migration

Still incomplete:

- some console replay helpers
- some direct fusion/helper paths
- some diagnostic/proof helpers
- stream-rate / radio-mode control path

### C. Browser SD/control UX

- functional pieces exist
- status clarity has improved
- but this is still an engineering GUI, not a polished production UI

### D. File-list performance under load

- bounded paging exists
- correctness is much better than earlier blocking refresh behavior
- but SD/listing traffic can still interfere with live telemetry freshness during transfer

---

## 8. Things ChatGPT Must Preserve

If ChatGPT is asked to continue work on this repo, it should preserve these assumptions.

### Preserve

- schema v2 meaning
- record/replay determinism
- logging priority
- Teensy fast path
- AIR ownership of SD and filesystem time
- GND as browser-facing distribution point
- one active schema per build

### Do not casually change

- timestamps
- replay ordering
- replay pacing
- batch semantics
- command/result semantics that existing scripts depend on
- browser transport rate defaults unless explicitly measuring them

### Prefer

- minimal coherent backend changes
- explicit contracts
- AIR-owned mode gating
- adapter thinning
- documentation updates alongside backend ownership changes

---

## 9. Good Next Kinds Of Work

These are aligned with the current branch state:

- continue consolidating AIR control paths behind one control plane
- remove remaining direct-control bypasses incrementally
- improve SD/file transfer cooperativeness without redesigning transport
- tighten state/result reporting consistency across serial, radio, and browser paths
- keep documenting proven branch-local contracts

---

## 10. Bad Next Kinds Of Work

These should be avoided unless explicitly requested:

- broad rollback of the dirty tree
- speculative UI redesign
- moving SD logic to Teensy
- live schema switching
- redesigning replay timing
- altering the Teensy fast path “for neatness”
- adding interface-local business logic in GND/browser

---

## 11. Short Executive Summary

`0.03 RC1` already has:

- a proven Teensy DMA fast path
- schema-defined bidirectional AIR/GND transport
- validated single-client Stage 2 browser telemetry at `10 Hz` and `30 Hz`
- an AIR-owned SD API
- an AIR-owned GPS-derived UTC time service
- documented field, SD, time, and control-plane contracts

The repo is not yet in a final release-clean state.
The main remaining engineering direction is backend consolidation and cleanup, especially around the AIR-owned control plane and the nonessential-control paths layered over the already-proven record/replay/logging core.
