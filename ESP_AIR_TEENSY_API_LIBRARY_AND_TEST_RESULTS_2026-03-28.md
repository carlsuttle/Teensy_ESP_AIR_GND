# ESP_AIR Teensy API Library And Test Results

Date:
- 2026-03-28

Branch baseline:
- `teensy-source-rate-refactor`
- base commit `5823924`

Important note:
- This validation was performed on a dirty working tree based on `5823924`.
- The new Teensy API library and exerciser were added on top of that working tree.

## Goal

Create a clean `ESP_AIR` library/API surface for the Teensy DMA/SPI interface so that:
- control commands are sent through a stable wrapper
- carry-through data integrity can be proven repeatedly
- sequence and timestamp alignment can be exercised directly
- PowerShell automation can capture repeatable pass/fail results similar to the SD API work

## Library

New library files:
- [teensy_api.h](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\teensy_api.h)
- [teensy_api.cpp](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\ESP_AIR\src\teensy_api.cpp)

Wrapped API surface:
- `waitForAck(...)`
- `getFusionSettings(...)`
- `setCaptureSettings(...)`
- `setStreamRate(...)`
- `setFusionSettings(...)`
- `printStatus(...)`
- `runCarrySignatureTest(...)`
- `runCarrySequenceCsvTest(...)`
- `runReplayBatchBenchmark(...)`

Console surface in `ESP_AIR`:
- `tapi help`
- `tapi status`
- `tapi getfusion`
- `tapi setcap <hz>`
- `tapi setstream <ws_hz> [log_hz]`
- `tapi setfusion <gain> <accelRej> <magRej> <recovery>`
- `tapi carry [count]`
- `tapi carrycsv [duration_ms] [window]`
- `tapi replaybench [duration_ms] [batch_hz] [records_per_batch]`
- `tapi selftest [hz] [count]`

## Methods And Command Decoding

This API is intentionally small, but the commands map onto real transport behavior.

Important shared record size:
- `State160` payload size: `160 bytes`
- replay/source proof records also use the same `160-byte` transport envelope

Important current default:
- Teensy standby serial mode now defaults to `quiet on`
- this keeps the Teensy USB console quiet unless explicitly turned back on, so throughput tests are not polluted by idle serial chatter

What each command is doing:

- `tapi status`
  - reads the current AIR-side snapshot of the Teensy link
  - confirms the link is alive, sequence is advancing, and the last ack is valid

- `tapi getfusion`
  - sends a control request to the Teensy
  - waits for an acknowledged fusion-settings response
  - proves command/ack behavior through the transport

- `tapi setcap <hz>`
  - sends `CMD_SET_CAPTURE_SETTINGS`
  - in this validation, `1600` means the requested Teensy capture/source rate is `1600 Hz`

- `tapi setstream <ws_hz> [log_hz]`
  - sends stream/log rate control to the Teensy-side mirror path
  - in this validation, `1600 1600` means both requested stream rates were set to `1600 Hz`

- `tapi setfusion <gain> <accelRej> <magRej> <recovery>`
  - sends explicit fusion parameter updates and checks ack

- `tapi carry [count]`
  - injects `count` synthetic replay-style source records from `ESP_AIR`
  - each source record contains known signatures for:
    - GPS
    - baro
    - accel/gyro/mag
    - `seq`
    - `t_us`
    - metadata and masks
  - waits for the Teensy to return a new `State160` for each source record
  - validates that carry-through fields are unchanged and the returned state matches the injected signatures

- `tapi carrycsv [duration_ms] [window]`
  - runs the same carry-through mechanism for a timed burst
  - `duration_ms=2000` means a `2-second` burst
  - `window=16` means up to `16` synthetic source records may be in flight before the checker waits for returned states
  - prints CSV rows with:
    - source `seq`
    - expected `t_us`
    - returned `t_us`
    - expected random signature
    - returned random signature
  - this is the most useful transport-sequence integrity proof command

- `tapi selftest [hz] [count]`
  - combines:
    - `getfusion`
    - `setcap`
    - carry-through checks
  - in this validation, `selftest 1600 8` means:
    - request `1600 Hz`
    - validate `8` carry-through records

- `tapi replaybench [duration_ms] [batch_hz] [records_per_batch]`
  - sends synthetic replay records from `ESP_AIR` to the Teensy in fixed-size batches
  - validates the returned replay-output state stream by checking:
    - returned replay-state marker
    - `t_us`
    - deterministic `headMot` signature
  - `batch_hz` is the batch cadence
  - `records_per_batch` is the number of `160-byte` replay records sent in each batch
  - nominal record rate is:
    - `batch_hz * records_per_batch`
  - example:
    - `tapi replaybench 60000 100 24`
    - means `24` records every `10 ms`
    - nominally `2400 records/s`
    - nominal one-way payload rate:
      - `2400 * 160 = 384000 B/s`
      - about `375 KiB/s`
  - example:
    - `tapi replaybench 60000 50 48`
    - means `48` records every `20 ms`
    - nominally `2400 records/s`
    - same nominal payload rate, but half the batch frequency and double the batch size

What `carrycsv 2000 16` means in real terms:
- duration: `2 s`
- outstanding window: `16` records
- record size: `160 bytes`
- measured completed sequence-validated records: `1136`
- measured source/return proof rate:
  - `1136 / 2 s = 568 records/s`
- equivalent one-way payload rate for the validated stream:
  - `568 * 160 = 90,880 B/s`
  - about `88.8 KiB/s`
- equivalent round-trip validated payload movement:
  - about `177.5 KiB/s`

This is a proof workload, not a top-end transport benchmark:
- it includes per-record validation
- it prints/logs CSV text
- it is intentionally conservative and deterministic

## Replay Benchmark Method

The replay benchmark is the current top-end bidirectional API proof.

Bench precondition:
- the AIR-side proof and replay scripts now force:
  - `bench on`
  - then `quiet on`
- this disables radio/web activity so the benchmark reflects only the Teensy interface path

Command used by the PowerShell script:

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_replay_benchmark.ps1 -AirComPort COM7 -TeensyComPort COM10
```

The script does:
1. open AIR and Teensy serial consoles
2. drain startup output
3. send `bench on` to AIR so radio/web activity is disabled during the benchmark
4. send `quiet on` to AIR and Teensy
5. for each test case:
   - `resetloopperf` on Teensy
   - `tapi replaybench ...` on AIR
   - `showsource` on Teensy
6. parse both sides and classify pass/fail
7. run a `60 s` soak at the best short-run point for each batch rate

Expected AIR bench response:
- `BENCH standalone=1 radio=0 wifi=off`

Pass criteria:
- AIR side:
  - `ok=1`
  - `fail=0`
  - `timeout=0`
- Teensy side:
  - `outq_max < 64`
  - `replay_rx_free_min > 0`
  - `state_tx_free_min > 0`
  - `rx_overflows=0`
  - `crc_err=0`
  - `type_err=0`

Teensy indicators used to judge headroom:
- `outq_max`
  - replay-output queue fill against a capacity of `64`
- `replay_rx_occ_max`
  - replay receive ring fill against a capacity of `511`
- `state_tx_occ_max`
  - state transmit ring fill against a capacity of `511`
- `state_tx_free_min`
  - minimum free space remaining in the state transmit ring

These are the right indicators for maximum replay performance because they show whether the link is approaching queue/ring exhaustion while still returning fully validated records.

## Replay Benchmark Results

### Short Sweep

`100 Hz` sweep:
- `100Hz x 8`
  - `799.5 validated records/s`
  - `outq_max=16/64`
  - `replay_rx_occ_max=16/511`
  - `state_tx_occ_max=57/511`
- `100Hz x 16`
  - `1593.6 validated records/s`
  - `outq_max=32/64`
  - `replay_rx_occ_max=32/511`
  - `state_tx_occ_max=275/511`
- `100Hz x 24`
  - `2386.2 validated records/s`
  - `outq_max=48/64`
  - `replay_rx_occ_max=48/511`
  - `state_tx_occ_max=234/511`
- `100Hz x 32`
  - failed
  - `state_tx_occ_max=511/511`
  - `timeout=996`
- `100Hz x 40`
  - failed
  - `state_tx_occ_max=511/511`
  - `timeout=827`
- `100Hz x 48`
  - failed
  - `state_tx_occ_max=511/511`
  - `timeout=854`

`50 Hz` sweep:
- `50Hz x 16`
  - `799.5 validated records/s`
  - `outq_max=16/64`
  - `replay_rx_occ_max=16/511`
  - `state_tx_occ_max=50/511`
- `50Hz x 24`
  - `1198.3 validated records/s`
  - `outq_max=24/64`
  - `replay_rx_occ_max=24/511`
  - `state_tx_occ_max=354/511`
- `50Hz x 32`
  - `1598.7 validated records/s`
  - `outq_max=32/64`
  - `replay_rx_occ_max=32/511`
  - `state_tx_occ_max=350/511`
- `50Hz x 40`
  - `1994.8 validated records/s`
  - `outq_max=40/64`
  - `replay_rx_occ_max=40/511`
  - `state_tx_occ_max=337/511`
- `50Hz x 48`
  - `2394.7 validated records/s`
  - `outq_max=48/64`
  - `replay_rx_occ_max=48/511`
  - `state_tx_occ_max=364/511`

### Soak-Proven Maxima

`100 Hz` soak-proven maximum:
- command shape:
  - `24 records/batch @ 100 Hz`
- nominal rate:
  - `24 * 100 = 2400 records/s`
- nominal payload rate:
  - `2400 * 160 = 384000 B/s`
  - about `375 KiB/s` each direction
- `60 s` result:
  - `validated_rps=2399.3`
  - `sent=144000`
  - `recv=144000`
  - `pass=144000`
  - `fail=0`
  - `timeout=0`
- Teensy indicators:
  - `outq_max=48/64`
  - `replay_rx_occ_max=48/511`
  - `state_tx_occ_max=253/511`
  - `state_tx_free_min=258`

`50 Hz` soak-proven maximum:
- command shape:
  - `48 records/batch @ 50 Hz`
- nominal rate:
  - `48 * 50 = 2400 records/s`
- nominal payload rate:
  - `2400 * 160 = 384000 B/s`
  - about `375 KiB/s` each direction
- `60 s` result:
  - `validated_rps=2399.3`
  - `sent=144000`
  - `recv=144000`
  - `pass=144000`
  - `fail=0`
  - `timeout=0`
- Teensy indicators:
  - `outq_max=48/64`
  - `replay_rx_occ_max=48/511`
  - `state_tx_occ_max=80/511`
  - `state_tx_free_min=431`

Interpretation:
- both `100 Hz x 24` and `50 Hz x 48` reach essentially the same validated bidirectional replay performance:
  - about `2400 records/s`
- `50 Hz x 48` achieves that rate with half the batch frequency
- this lowers transaction cadence while keeping throughput nearly identical
- for integrated systems with added web/socket load, `50 Hz x 48` may therefore be the better operating point when `20 ms` transport latency is acceptable

## Mode Sweep

Additional script:
- [run_teensy_api_mode_sweep.ps1](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_mode_sweep.ps1)

Run command:

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_mode_sweep.ps1 -ComPort COM7
```

This organizes the API proof into reader-friendly phases:
- `Quiet`
- `Live`
- `Control`
- `ReplayCarry`
- `ReplaySequence`
- `SelfTest`

Observed phase summary:
- `Quiet`: `2/2` pass
- `Live`: `1/1` pass
- `Control`: `4/4` pass
- `ReplayCarry`: `1/1` pass
- `ReplaySequence`: `1/1` pass
- `SelfTest`: `1/1` pass

Observed final summary:
- `FINAL SUMMARY passed=10 failed=0 port=COM7`

## Exerciser

Script:
- [run_teensy_api_exerciser.ps1](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_exerciser.ps1)

Run command:

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_exerciser.ps1 -ComPort COM7
```

The exerciser:
- opens the AIR USB console without toggling DTR/RTS
- drains startup serial
- forces `bench on`
- sends the stable `tapi` proof sequence
- logs all console output to a timestamped `.log`
- prints per-step pass/fail state
- prints a final summary

## Verified Bench Setup

Ports used:
- `ESP_AIR`: `COM7`
- Teensy: existing linked target on the AIR transport path

Validation sequence:
1. `quiet on`
2. `tapi status`
3. `tapi getfusion`
4. `tapi setcap 1600`
5. `tapi setstream 1600 1600`
6. `tapi carry 8`
7. `tapi carrycsv 2000 16`
8. `tapi selftest 1600 8`

## Results

Final summary:
- `passed=9`
- `failed=0`

Verified control-path behavior:
- `tapi getfusion` returned valid fusion settings and ack
- `tapi setcap 1600` succeeded with ack
- `tapi setstream 1600 1600` succeeded with ack

Verified direct carry-through behavior:
- `tapi carry 8` returned:
  - `sent=8`
  - `recv=8`
  - `pass=8`
  - `fail=0`
  - `timeout=0`

Verified sequence/timestamp integrity:
- `tapi carrycsv 2000 16` returned:
  - `sent=1136`
  - `recv=1136`
  - `pass=1136`
  - `fail=0`
  - `timeout=0`
- For the sampled replay sequence rows:
  - `expected_t_us == actual_t_us`
  - `expected_headmot == actual_headmot`

Derived burst metric from the same run:
- duration: `2 s`
- validated sequence-matched records: `1136`
- validated source/return record rate: about `568 records/s`
- validated one-way payload rate: about `88.8 KiB/s`
- validated round-trip payload movement: about `177.5 KiB/s`

Verified replay benchmark behavior:
- new PowerShell runner:
  - [run_teensy_api_replay_benchmark.ps1](C:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND\scripts\run_teensy_api_replay_benchmark.ps1)
- soak-proven replay maxima:
  - `100 Hz x 24 records = ~2400 records/s`
  - `50 Hz x 48 records = ~2400 records/s`
- both passed `60 s` with:
  - `fail=0`
  - `timeout=0`
  - no queue/ring exhaustion

Verified combined self-test:
- `tapi selftest 1600 8` returned:
  - `ok=1`
  - `getfusion=1`
  - `setcap=1`
  - `carry_fail=0`
  - `carry_timeout=0`

## What This Proves

The new `teensy_api` layer proves:
- control commands from `ESP_AIR` to Teensy are clean and acknowledged
- replay-style carry-through of GPS, baro, IMU, metadata, and masks is clean
- replay source `seq/t_us` integrity is preserved through the transport path
- the proof surface can be exercised from PowerShell without relying on scattered legacy bench commands
- the API can be exercised by phase so that live state, control, replay carry-through, and combined self-test are separable in bench output

## Current Operational Caveat

Before `quiet on` lands, `ESP_AIR` may print normal `AIRTX ...` traffic. This is expected and does not invalidate the proof run.

## Recommended Use

Use `tapi` plus the exerciser:
- before replay-path work
- after transport changes
- after Teensy control-path changes
- before integrating the new API into higher-level AIR replay or web-control flows

Use the mode sweep when a reader needs:
- a phase-by-phase proof transcript
- a cleaner PowerShell view of which part of the transport is under test
- a quick sanity check after flashing `ESP_AIR`
