# GND to AIR Control Protocol for Release 0.03

This document defines the ESP-NOW control protocol used when `ESP_GND` forwards control requests to `ESP_AIR`.

Scope:

- AIR remains the single execution authority for control behavior
- GND is a thin request packager, result correlator, and browser-facing forwarder
- request/result semantics match the AIR serial control protocol
- AIR backend logic stays in the AIR control plane

Non-goals:

- no serial protocol redesign
- no schema-v2 redesign
- no Teensy fast-path redesign
- no GND-owned mode gating

## Ownership

Authoritative backend:

- `ESP_AIR/src/control_plane.h`
- `ESP_AIR/src/control_plane.cpp`

Transport adapters:

- AIR radio adapter in `ESP_AIR/src/radio_link.cpp`
- GND radio adapter in `ESP_GND/src/radio_link.cpp`

Rule:

- GND packages control requests
- AIR validates state, mode, and arguments
- AIR dispatches to SD, recording, replay, and fusion backends
- GND surfaces AIR immediate/final results
- GND does not execute recording/replay/fusion/file business logic locally

## Shared Semantic Model

This protocol must mirror the AIR serial control semantics documented in:

- `docs/AIR_SERIAL_CONTROL_PROTOCOL_0.03.md`

The shared two-phase model is:

1. immediate disposition
2. final completion for accepted requests only

Immediate disposition values:

- `accepted`
- `busy`
- `rejected`

Final completion values:

- `completed_ok`
- `completed_error`

Rule:

- `busy` or `rejected` ends the request immediately
- only `accepted` requests produce a later final result

## Control Request Envelope

Logical request fields are:

- `req_id`
- `category`
- `action`
- `args`

Optional transport provenance:

- `source`
- `radio_seq`
- `t_us`

Field meanings:

- `req_id`
  - GND-chosen correlation id
  - unique among currently in-flight requests on GND
- `category`
  - AIR control category such as `state`, `recording`, `replay`, `file`, or `fusion`
- `action`
  - AIR control action within that category
- `args`
  - bounded action-specific fields only

## Immediate Result Envelope

Logical AIR immediate result fields are:

- `req_id`
- `status`
- `code`

Required `status` values:

- `accepted`
- `busy`
- `rejected`

Rules:

- AIR must echo the same `req_id`
- AIR must provide an explicit `code`
- GND must surface immediate disposition without inventing local reasons

## Final Result Envelope

Logical AIR final result fields are:

- `req_id`
- `status`
- `code`
- optional `payload_ref`

Required `status` values:

- `completed_ok`
- `completed_error`

`payload_ref` meaning:

- a lightweight indication of which AIR-owned payload accompanies the result
- examples:
  - `state`
  - `recording`
  - `replay`
  - `storage`
  - `file_list`
  - `fusion`

Payload rule:

- payload content is AIR-authored status/data
- GND may cache and reshape it
- GND must not synthesize equivalent payloads locally

## Retry and Correlation Over ESP-NOW

ESP-NOW is not a guaranteed-delivery RPC transport, so correlation rules must be explicit.

### GND responsibilities

- assign a `req_id`
- keep the request pending until AIR immediate or final evidence arrives, or local timeout expires
- correlate all received AIR control responses by `req_id`
- avoid sending duplicate retries while a matching request is still locally pending unless the higher layer explicitly requests retry

### AIR responsibilities

- treat `req_id` as opaque transport correlation
- echo `req_id` in every immediate/final response
- keep AIR control-plane acceptance/completion semantics unchanged

### Timeout/loss rule

If GND does not receive:

- an immediate result in time
- or the final result for an accepted request in time

then GND may mark the browser-visible request as transport-unknown or timed-out.

Important:

- GND timeout does not mean AIR rejected the request
- GND timeout does not grant permission to infer backend state
- recovery should prefer an AIR status/get request over local guesswork

## Phase Order For Implementation

To keep GND thin and the rollout testable, implement categories in this order only.

### Phase 2A: status/get only

Implement first:

- `state/get`
- `recording/status`
- `fusion/get`

### Phase 2B: one mutating category

Implement next:

- `recording/start`
- `recording/stop`

### Phase 2C: replay control

Implement after recording is stable:

- `replay/start_latest`
- `replay/start_file`
- `replay/stop`
- `replay/status`

### Phase 2D: file operations

Implement last:

- `storage_status`
- `file_list_page`
- later file mutations only after the above are proven

## AIR-Owned Mode And Backend Policy

Mode gating belongs to AIR.

AIR alone decides:

- whether replay may start during recording
- whether recording may start during replay
- whether file operations are inhibited
- whether fusion access is temporarily unavailable

GND responsibilities are only:

- encode request
- send request
- surface AIR result

## Packet Mapping In This Phase

The current repo already has command-specific radio packet types in `types_shared.h`.

This protocol document defines the ownership and semantics that those packets must converge to:

- GND-originated request
  - one AIR control-plane request with `req_id`, `category`, `action`, and bounded args
- AIR-originated immediate result
  - `accepted | busy | rejected` plus `code`
- AIR-originated final result
  - `completed_ok | completed_error` plus `code` and optional payload

Implementation note:

- transport may continue to use bounded binary payload structs in this phase
- but the semantic contract must remain the AIR shared request/result model, not a collection of GND-owned command behaviors

## GND Must Not Own

GND must not:

- block or allow replay locally
- block or allow recording locally
- enforce SD inhibition rules
- mutate fusion state locally
- infer completion from missing packets alone
- turn browser convenience requests into local backend behavior

## Browser-Facing Result Policy

GND may reduce AIR results for browser display, but must preserve:

- `req_id`
- immediate vs final phase distinction
- `status`
- `code`

If browser-facing JSON differs in shape from the ESP-NOW packet, that is a shaping concern only.

The AIR/GND control semantics must remain unchanged.
