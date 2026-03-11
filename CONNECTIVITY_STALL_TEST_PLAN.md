# Connectivity And Startup Stall Test Plan

## Goal

Prove that:

- startup order does not matter
- temporary connectivity loss does not create a lockout
- the system recovers without manual intervention
- telemetry, UI, and logging all resume after recovery

This plan is focused on:

- `ESP_GND` AP startup and browser access
- `ESP_AIR` STA reconnect and UDP resume
- `Teensy` telemetry resume
- end-to-end sequence continuity and stall detection

## Preconditions

- `ESP_GND` is the AP at `192.168.4.1`
- `ESP_AIR` is the STA at `192.168.4.2`
- GND DHCP lease pool starts at `192.168.4.50`
- iPad/phone/browser clients get `.50+` addresses
- the current baseline config is known
  - example: `source_rate_hz=50`
  - example: `ui_rate_hz=20`
  - example: `log_rate_hz=50`
- all three serial ports are known

## Pass Criteria

- `ESP_GND` reaches `GND READY ap` without retries or lockout
- `ESP_AIR` reaches `AIR READY wifi` once the AP exists
- `ESP_AIR` reaches `AIR READY teensy_link` once Teensy telemetry exists
- `ESP_GND` reaches `GND READY air_link` once AIR packets exist
- after any interruption, recovery happens without manual reset
- no unit stays in a `WAIT` state past the configured deadline
- no telemetry stall exceeds the configured stall threshold
- the browser remains usable after reconnect
- logging continues or resumes without panic/reset

## Startup Order Matrix

Run each scenario from full power-off.

1. `GND -> AIR -> Teensy`
2. `GND -> Teensy -> AIR`
3. `AIR -> GND -> Teensy`
4. `AIR -> Teensy -> GND`
5. `Teensy -> GND -> AIR`
6. `Teensy -> AIR -> GND`
7. All three powered within a few seconds

For each run, verify:

- GND AP becomes visible and browser can join
- AIR joins the AP automatically
- Teensy telemetry appears on AIR
- GND receives packets
- UI loads and stays connected

## Connectivity Interruption Matrix

Run these after a stable baseline is established.

1. Reset `ESP_AIR`
2. Reset `ESP_GND`
3. Reset `Teensy`
4. Power-cycle `ESP_AIR`
5. Power-cycle `ESP_GND`
6. Power-cycle `Teensy`
7. Join and leave the AP from the iPad repeatedly
8. Leave the iPad connected during AIR logging
9. Turn logging on and off during active telemetry

For each interruption, verify:

- no permanent lockout
- all `READY` states return automatically
- `seq` resumes increasing on all active units
- `udp_rx` resumes on GND
- UI reconnects without manual board reset

## Suggested Deadlines

These are operational targets for the soak.

- GND AP ready: `<= 15 s` after GND boot
- AIR Wi-Fi ready: `<= 30 s` after both AIR boot and GND AP ready
- AIR Teensy link ready: `<= 30 s` after both AIR Wi-Fi ready and Teensy stats active
- GND AIR link ready: `<= 30 s` after both GND AP ready and AIR Wi-Fi ready
- live telemetry stall threshold: `4 s`

Adjust only if real hardware measurements justify it.

## Automated Monitor

Use:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\monitor_stalls.ps1 -AirPort COM6 -GndPort COM7 -TeensyPort COM10
```

The script:

- opens the selected serial ports
- sends `stats` to the ESPs and Teensy
- logs raw serial output per unit
- writes `events.csv`
- detects:
  - boot events
  - `READY` / `WAIT` state changes
  - Wi-Fi disconnect / reconnect transitions
  - startup deadline violations
  - sequence stalls
  - sequence recovery after a stall
- writes `summary.json` on exit

Output directory:

```text
soak_logs\YYYYMMDD-HHMMSS\
```

Contents:

- `AIR.log`
- `GND.log`
- `TEENSY.log`
- `events.csv`
- `summary.json`

## Manual Artifacts To Save

- GND diag CSV
- GND websocket event CSV
- browser client event CSV
- AIR log files created during the run
- any screenshots of stuck Wi-Fi join pages

## Recommended Soak Sequence

1. Run startup order matrix at `50/20/50`
2. Run interruption matrix at `50/20/50`
3. Run 30 to 60 minute soak with logging on
4. Confirm AIR log files can be retrieved
5. Increase source/log rate only after the baseline passes
