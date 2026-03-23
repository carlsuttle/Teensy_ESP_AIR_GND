# Integration Test Report - March 21, 2026

## Scope

This report summarizes the hardware bring-up, standalone bench validation, SPI/DMA plus SPI/SD integration work, fault isolation, and final verified operating state for the Teensy/ESP AIR/GND stack.

Tested system components:
- Teensy 4.0 avionics unit
- ESP_AIR on XIAO ESP32-S3
- ESP_GND on XIAO ESP32-S3
- SPI/DMA link between Teensy and AIR
- SD logging/capture on AIR
- ESP-NOW AIR to GND path
- IMU over I2C on Teensy
- GPS UBX binary parser over UART on Teensy

Date of testing:
- March 21, 2026

Serial ports used during validation:
- Teensy: `COM10`
- ESP_AIR: `COM7`
- ESP_GND: `COM9`

## Wiring Verified

### Teensy 4.0 to ESP_AIR SPI transport
- Teensy pin `13` -> ESP `GPIO5` (`SCK`)
- Teensy pin `12` -> ESP `GPIO1` (slave `SDI` from master `MOSI`)
- Teensy pin `11` -> ESP `GPIO6` (slave `SDO` to master `MISO`)
- Teensy pin `10` -> ESP `GPIO44` (`CS`)
- Teensy pin `2` -> ESP `GPIO4` (`READY`)
- Common ground required

### ESP_AIR to SD card
- ESP `GPIO7` -> SD `SCK`
- ESP `GPIO8` -> SD `MISO`
- ESP `GPIO9` -> SD `MOSI`
- ESP `GPIO2` -> SD `CS`
- `3V3` -> SD `VCC`
- `GND` -> SD `GND`

### Teensy sensor wiring confirmed
- I2C: `SDA=18`, `SCL=19`
- GPS UART: `RX1=0`, `TX1=1`
- GPS uses UBX binary data path, not NMEA text

## Standalone Bench Validation

### 1. SPI/DMA prototype benchmark
Project:
- `SPI_DMA_Transport_Prototype`

Result:
- Proven working at `40 MHz` full duplex
- Highest-speed stress test passed cleanly
- No replay misses, no duplicates, no CRC errors, no queue overflow during the validated prototype stress run

Interpretation:
- The physical Teensy-to-AIR transport wiring was good
- The SPI protocol and DMA design were fundamentally sound before full-stack integration

### 2. Standalone ESP_AIR SD test
Project:
- `Teensy_ESP_AIR_GND/ESP_AIR`

Result:
- SD probe and small write test succeeded on pins `7/8/9/2`
- Card was detected and writable

Interpretation:
- The AIR SD wiring and base SD software path were functional before combined integration load

### 3. Teensy IMU and GPS BIT
Project:
- `FAST_Teensy_Avionics`

Built-in test added and run on hardware.

Result:
- IMU on I2C passed
- GPS UBX binary parser path passed
- Valid GPS UART traffic and NAV-PVT frames were observed

Interpretation:
- Teensy sensor stack was healthy before integrated transport work

## Full-Stack Integration Work

Target stack:
- `Teensy_ESP_AIR_GND/Teensy`
- `Teensy_ESP_AIR_GND/ESP_AIR`
- `Teensy_ESP_AIR_GND/ESP_GND`

Integrated behavior now in place:
- Teensy publishes telemetry state over SPI/DMA instead of the old UART mirror path
- AIR receives Teensy state over SPI, forwards live data to GND, and can capture/log to SD
- AIR replay/control traffic is sent back to Teensy over the SPI reverse path
- GND remains on the radio/web side and receives live state from AIR

## Faults Found During Integration

### Fault 1. Teensy SPI clock pin was being stolen by LED logic
Symptom:
- AIR clocked SPI transactions but got no valid data
- Teensy armed one transaction and never completed it

Root cause:
- In the full Teensy application, CRSF LED activity used `LED_BUILTIN`
- On Teensy 4.0, `LED_BUILTIN` is pin `13`
- Pin `13` is also the SPI transport `SCK`
- That remuxed the clock pin away from the SPI slave path after bring-up

Fix applied:
- Disabled the LED path while SPI mirror/transport is enabled

Result after fix:
- Teensy transaction arm and completion counters matched
- AIR immediately began receiving valid SPI state frames

### Fault 2. AIR SD backend was stealing the SPI transport host
Symptom:
- SPI transport worked until SD capture started
- Then the Teensy side got stranded with an armed transaction and AIR stopped advancing correctly

Root cause:
- AIR SD backend used Arduino default `SPI`
- On this ESP32-S3 environment, Arduino default `SPI` maps to the same hardware host used by the custom SPI transport master
- Starting SD capture reconfigured the same controller the transport depended on

Fix applied:
- Moved AIR SD access onto a dedicated SPI controller using a separate `SPIClass(HSPI)` instance

Result after fix:
- SD activity no longer destabilized the transport host
- SPI transport remained alive while SD capture was active

### Fault 3. `spi_fail` was counting normal not-ready gaps as failures
Symptom:
- `spi_fail` climbed even while the web client showed valid live data

Root cause:
- AIR counted normal `READY` wait misses as transport failures
- These were scheduler/readiness timing gaps, not actual bus-transfer errors

Fix applied:
- AIR master loop now waits more realistically for `READY`
- Normal not-ready gaps are retried instead of being counted as SPI failures
- Only real transaction problems should increment `spi_fail`

Result after fix:
- `spi_fail=0` during normal live streaming

### Fault 4. AIR SD long capture unstable at 40 MHz
Symptom:
- SD probe and small write tests passed
- Long capture under sustained load failed at `40 MHz`
- Repeated SD status errors were observed during capture

Root cause:
- SD bus signal margin/stability issue under sustained write load at `40 MHz`
- This was not a transport bug

Fault-finding method:
- Temporary SD clock reduction only for diagnosis
- `20 MHz` test completed successfully for full 60-second capture
- `26 MHz` test also completed successfully for full 60-second capture
- This isolated the issue to SD bus speed margin rather than queueing or SPI/DMA transport logic

Result:
- `26 MHz` is the currently verified stable AIR SD operating point
- Current AIR SD init order uses `26 MHz` first, then `20 MHz` fallback

## Final Verified Results

### Live telemetry and web display
Result:
- Live data is arriving at the web client and displaying correctly

Interpretation:
- End-to-end Teensy -> AIR -> GND -> web path is operational

### SPI transport current state
Result:
- AIR receives live state frames continuously
- `spi_fail=0` in normal streaming after fix
- Teensy SPI transaction progression remained healthy during validation

Interpretation:
- The integrated SPI/DMA link is now operating correctly in the full stack

### AIR SD capture current state
Stable verified configuration:
- AIR SD bus at `26 MHz`
- Separate SPI controller from transport host

Validated capture result:
- Command: `sdcap1m`
- Duration: `60 s`
- Result: success
- `records=3000`
- `bytes=528000`
- `dropped=0`
- `queue_max=2`
- `err=(none)`
- `init_hz=26000000`

Interpretation:
- SPI transport and SPI/SD can coexist under live capture load in the integrated stack
- `26 MHz` is currently the highest verified stable AIR SD setting from this session

## Current Firmware State

Key fixes retained in the codebase:
- Teensy pin-13 LED conflict removed while SPI mirror is enabled
- AIR SD moved to separate SPI host
- AIR SPI fail accounting corrected
- AIR SD frequency set to tested stable `26 MHz` first, `20 MHz` fallback

Temporary debug instrumentation status:
- Temporary Teensy-side deep SPI debug summary was removed after verification
- Functional fixes remain in place

## Recommended Next Test Sequence

Next steps from the current known-good baseline:
1. Test live record using the current integrated system
2. Verify recorded file integrity and session behavior
3. Test replay path from recorded data
4. Validate replay control/command handling back to Teensy
5. If desired later, retest SD clock upward from `26 MHz` only as a controlled optimization task

## Replay Addendum

Follow-on validation after the original integrated bring-up completed the replay
path using the main `Teensy_ESP_AIR_GND` firmware pair.

### Key replay tooling now present on AIR

- `verifylog`
- `expandlogs`
- `comparelogs <src> <dst>`
- `replaycapture`
- `replaycapfile <name>`
- `latestlog`
- `latestlogsession <n>`
- `logstartid <n>`

### Replay bug found and fixed

A replay-specific structural bug was identified on the Teensy side:

- replay IMU samples were entering the same frame queue as live IMU samples
- replay mode was still using the normal frame-averaging path
- the averaged frame path dropped the `processed_body` marker
- replayed body-frame accel/mag samples could then be transformed a second time

Fixes retained:

- preserve `processed_body` through averaged frames
- in replay mode, consume one queued replay frame at a time instead of averaging
- preserve replay source `seq` / `t_us`
- snapshot non-fusion source fields per replay record

### Stationary replay validation

Source / output pair:

- `air_130_238447.tlog`
- `air_131_316437.tlog`

Result:

- `dst_prefix=6`
- `imu_mismatch=0`
- `mag_mismatch=0`
- `gps_mismatch=0`
- `baro_mismatch=0`
- `mask_mismatch=0`
- `roll_mean_abs=0.018657`
- `pitch_mean_abs=0.124635`
- `yaw_mean_abs=0.132151`
- `maghdg_mean_abs=0.019909`

Interpretation:

- replay is now correct for stationary data apart from the bounded startup
  alignment prefix

### Motion replay validation

Source / output pair:

- `air_140_478029.tlog`
- `air_141_554078.tlog`

Result:

- `dst_prefix=7`
- `imu_mismatch=0`
- `mag_mismatch=0`
- `gps_mismatch=0`
- `baro_mismatch=0`
- `mask_mismatch=0`
- `roll_mean_abs=0.834425`
- `pitch_mean_abs=0.722921`
- `yaw_mean_abs=0.999702`
- `maghdg_mean_abs=0.022302`

Replay-fusion diagnostics:

- replay accel input matched source closely:
  - `accel_input_mean_abs=(0.000254,0.000253,0.000249)`
- motion replay showed genuine filter behavior:
  - `accel_ignored=23`
  - `mag_ignored=331`

Interpretation:

- no remaining replay sign reversal was observed
- remaining motion differences are now attributable to fusion/rejection behavior
  rather than replay corruption

## Conclusion

The system is no longer in a bench-only state. The integrated stack is functioning with:
- live telemetry visible in the web client
- stable SPI/DMA transport
- `spi_fail=0` in normal live operation
- stable AIR SD capture at `26 MHz`
- validated coexistence of transport and SD logging/capture paths
- validated replay path suitable for offline fusion-algorithm development

The main remaining work is now feature validation of:
- live recording behavior through the GUI
- replay behavior through the GUI
rather than basic transport bring-up.

## Follow-on Notes (March 23, 2026)

Subsequent integrated work on March 23, 2026 produced these additional
practical findings:

- a new motion log was recorded through the live system and replayed
  successfully through the web workflow
- the replay/file-management UI on GND was substantially simplified and made
  more operationally useful
- AIR/GND log status now exposes a distinct `busy` state so the browser can
  defer file refresh until log open/close/finalization work is complete
- AIR logging now aborts cleanly if SD media disappears during an active
  session, instead of leaving the recorder path wedged
- GND remote file tracking was expanded to support larger AIR file libraries
  and AIR file-list transmission is now paced to avoid chunk loss during large
  refreshes

Important radio note:

- attempting to enable ESP-NOW LR mode on the GND SoftAP path caused the
  `Telemetry` Wi-Fi network to disappear for normal client devices
- the working system therefore remains on normal Wi-Fi protocol for the
  user-facing GND AP
- true bidirectional LR remains a future architecture task requiring a cleaner
  separation between the GND web AP and the AIR radio leg

Additional SD-removal behavior validated on March 23, 2026:

- removing the AIR SD card while idle now correctly reports `media missing`,
  greys the recorder control, and hides the replay library
- reinserting the card restores normal operation; recorder/library availability
  can be refreshed explicitly from the UI
- the remaining observed side effect is a brief `2-3 s` FPS/DQI dip at the
  instant of physical card removal, attributed to the final real SD health
  probe used to confirm media loss

Operational conclusion:

- this remaining transient is acceptable for now because telemetry fully
  recovers and the system no longer enters the earlier reconnect/wedge state
