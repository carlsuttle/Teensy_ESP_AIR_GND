# Milestone Manifest - Release 0.01

This manifest freezes the interim integrated software state that was validated
on March 27, 2026 and is being committed as `release 0.01`.

Important: this is an interim engineering checkpoint. It proves the current
replay/rerecord workflow shape and the current measured outcomes, including the
remaining GPS/baro carry-through defect.

---

## 1. Milestone Identity

- Milestone name: `release-0.01`
- Date: `2026-03-27`
- Operator: `Codex + Carl`
- Purpose:
  - preserve the current integrated replay/rerecord state
  - freeze the exact boards, ports, source file, output files, and compare
    results
  - create a clean restart point before further fixes
- Acceptance criteria:
  - `ESP_AIR`, `ESP_GND`, and `Teensy` build from the current worktree
  - replay/rerecord produces `1:2`, `1:4`, and `1:8` output files on AIR SD
  - all three compare runs return `AIRCOMPARET RESULT ok=1`
  - known remaining defect is documented explicitly

---

## 2. Exact Software State

### Repo identity

- Repo path:
  `c:\Users\dell\Platformio\esp32_crsf_telemetry\Teensy_ESP_AIR_GND`
- Active branch: `teensy-source-rate-refactor`
- Base commit:
  - `88cb9aaddf7236dcab78d6fc8558b3d227641dd3`
  - `2026-03-23 16:48:05 -0700`
  - `Harden AIR SD handling and recorder UI state`

### Built state note

- The committed state for `release 0.01` is a dirty-worktree integration on top
  of the base commit above.
- This milestone intentionally captures the integrated checkpoint as tested,
  rather than claiming equivalence to the base commit.

---

## 3. Hardware And Ports

- Teensy board: `Teensy 4.0`
- ESP_AIR board: `Seeed XIAO ESP32-S3`
- ESP_GND board: `Seeed XIAO ESP32-S3`
- Teensy port: `COM10`
- ESP_AIR port: `COM7`
- ESP_GND port: `COM9`
- SD card location: `ESP_AIR`

---

## 4. Flash And Runtime Actions

- Teensy flashed from current worktree: `yes`
- ESP_AIR flashed from current worktree: `yes`
- ESP_GND flashed from current worktree: `yes`
- ESP_GND filesystem uploaded: `yes`
- AIR and Teensy quiet-mode controls verified: `yes`

Quiet-mode commands verified in this milestone:

- AIR:
  - `quiet on`
  - `quiet off`
  - `quiet status`
- Teensy:
  - `quiet on`
  - `quiet off`
  - `quiet status`

---

## 5. Test Assets

### Source log

- Source replay file:
  - `air_776433475_1034452.tlog`
  - `records_total=168326`

### Output logs created during this milestone

- `1:2` output:
  - `air_776433534_4247.tlog`
- `1:4` output:
  - `air_776433535_874699.tlog`
- `1:8` output:
  - `air_776433536_1045725.tlog`

---

## 6. Key Functional Changes Present In This Release

- `ESP_AIR` replay batching moved to `100 Hz` / `10 ms` transaction cadence.
- Old replay underfeed clamp removed.
- Replay source-rate mapping aligned to the Teensy supported profile ladder:
  - `1:2 -> 800 Hz`
  - `1:4 -> 400 Hz`
  - `1:8 -> 200 Hz`
- Replay source selection corrected to prefer `State160` when it dominates the
  source file.
- `logkinds` diagnostic added for replay output inspection.
- Quiet mode added to `ESP_AIR` and `Teensy` to suppress unsolicited serial
  chatter during timing tests.
- Teensy loop and replay instrumentation added for replay-path timing and queue
  analysis.

---

## 7. Replay Throughput And Output Results

### Direct replay throughput spot checks

- `1:1` direct replay:
  - about `1472 records/s` over a `20 s` window
- `1:2` direct replay:
  - about `801 records/s` over a `20 s` window
- `1:4` direct replay:
  - about `402 records/s` over a `20 s` window
- `1:8` direct replay:
  - about `207 records/s` over a `20 s` window

### Rerecord file creation results

- `1:2`
  - `AIRREPLAYCAP RESULT ok=1`
  - `elapsed_ms=111575`
  - `sent=84163`
  - `written=132869`
  - `dropped=0`
- `1:4`
  - `AIRREPLAYCAP RESULT ok=1`
  - `elapsed_ms=108838`
  - `sent=42082`
  - `written=66258`
  - `dropped=0`
- `1:8`
  - `AIRREPLAYCAP RESULT ok=1`
  - `elapsed_ms=106997`
  - `sent=21041`
  - `written=33505`
  - `dropped=0`

---

## 8. Compare Results Against Original

### `1:2` compare

- Command:
  - `comparetimed air_776433475_1034452.tlog air_776433534_4247.tlog 4`
- Result:
  - `AIRCOMPARET RESULT ok=1`
  - `compared=86992`
  - `gps_mismatch=86992`
  - `baro_mismatch=86992`
  - `mask_mismatch=3109`
  - fusion mean abs:
    - roll `0.287943`
    - pitch `1.052444`
    - yaw `1.102930`
    - mag heading `0.343245`

### `1:4` compare

- Command:
  - `comparetimed air_776433475_1034452.tlog air_776433535_874699.tlog 4`
- Result:
  - `AIRCOMPARET RESULT ok=1`
  - `compared=44690`
  - `gps_mismatch=44690`
  - `baro_mismatch=44690`
  - `mask_mismatch=2746`
  - fusion mean abs:
    - roll `0.374506`
    - pitch `1.032035`
    - yaw `1.681531`
    - mag heading `0.585062`

### `1:8` compare

- Command:
  - `comparetimed air_776433475_1034452.tlog air_776433536_1045725.tlog 4`
- Result:
  - `AIRCOMPARET RESULT ok=1`
  - `compared=23524`
  - `gps_mismatch=23524`
  - `baro_mismatch=23524`
  - `mask_mismatch=2551`
  - fusion mean abs:
    - roll `0.571103`
    - pitch `1.194107`
    - yaw `2.783211`
    - mag heading `1.024918`

---

## 9. What This Release Proves

- `ESP_AIR` can open the large source log and run replay/rerecord through the
  Teensy at the `1:2`, `1:4`, and `1:8` settings.
- The returned files are written to AIR SD with `dropped=0`.
- `comparetimed` runs successfully against all three output files.
- Subsampled output volume scales in the expected direction with stronger
  decimation.

---

## 10. Known Remaining Defect

- The carried non-attitude fields are still wrong in the rerecorded files.
- For all three compare runs:
  - `gps_mismatch == compared`
  - `baro_mismatch == compared`
- So `release 0.01` is a valid replay/rerecord milestone, but not a final
  correctness milestone for replay field carry-through.

---

## 11. Promotion Decision

- Promote as interim checkpoint: `yes`
- Promote as final known-good release: `no`
- Follow-up work:
  - fix replay GPS/baro carry-through into returned `State160` records
  - re-run `1:2`, `1:4`, `1:8`
  - produce a later milestone when non-attitude fields compare cleanly
