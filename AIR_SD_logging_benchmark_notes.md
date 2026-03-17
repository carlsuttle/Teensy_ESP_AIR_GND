
# AIR Unit SD Logging Investigation
Project: FAST Telemetry – ESP32-S3 AIR Recorder
Date: 2026-03-17

## Objective
Evaluate whether an ESP32‑S3 using SPI SD storage can reliably log telemetry records for the AIR unit without becoming the limiting factor for system throughput.

Target recording contract:

- Record size: 250 bytes
- Record rate: 400 Hz
- Sustained data rate: ~97.7 KB/s
- Hardware: Seeed XIAO ESP32‑S3
- Interface: SPI SD card
- Typical block write size tested: 10,000 bytes

The logging subsystem must **never limit data capture**. If reduction of data rate is required, it must originate in the flight computer (Teensy), not the recorder.

---

# Benchmark Methodology

## Synthetic Producer / Consumer Test

A synthetic benchmark was created to emulate the expected recorder behavior:

Producer:
- Generates 250‑byte records
- Runs at 400 Hz (2.5 ms period)
- Packs records into blocks

Writer:
- Writes full blocks to the SD card
- Block size used: 10,000 bytes (40 records)

Test buffer configurations:

| Blocks | Total RAM |
|------|------|
| 5 | 50 KB |
| 10 | 100 KB |

Producer/consumer queues were used to simulate asynchronous recording.

---

# Measured SD Performance

Typical write statistics with a Mindstar 64GB SD card:

| Metric | Value |
|------|------|
| Block size | 10,000 bytes |
| Average write time | ~7 ms |
| Worst write time | ~9–10 ms |
| Slow flush threshold | >12 ms |
| Slow flush count | 0 observed in final runs |

Estimated instantaneous write bandwidth:

10 KB / 7 ms ≈ **1.4 MB/s**

Required system bandwidth:

≈ **0.098 MB/s**

Therefore the SD path has **~14× bandwidth margin** relative to the required logging rate.

---

# Observations from the Synthetic Benchmark

Several issues were discovered with the benchmark harness itself:

1. FreeRTOS scheduling artifacts caused the producer to starve the writer task in some runs.
2. Queue state sometimes prevented blocks from being reacquired correctly.
3. Status reporting occurred before the writer drained its queue.

These issues distorted statistics such as:

- dropped record counts
- free block events
- queue depth measurements

However, **SD write timing itself remained reliable** and consistently in the 7–10 ms range.

---

# Key Technical Conclusions

## 1. SPI SD bandwidth is sufficient

We are going to try 26Nhz for the SPI bus frequency to provide some headroom for noise and real world headroom.

The measured block latency demonstrates that SPI SD logging can comfortably support the target data rate.

Required rate:
~100 KB/s

Observed instantaneous capacity:
>1 MB/s

Margin:
≈ **10–14×**

---

## 2. Block buffering remains necessary

Even though throughput is sufficient, buffering protects against:

- SD internal erase cycles
- filesystem latency spikes
- interrupt or scheduling delays

Recommended minimum recorder buffer:

- Block size: 10 KB
- Block count: 2–4
- Total RAM: 20–40 KB

---

## 3. Synthetic benchmarks have diminishing value

The standalone benchmark does **not represent the real AIR workload** because it excludes:

- UART ingestion from the flight computer
- radio telemetry servicing
- packet framing
- interrupt load
- system housekeeping

Therefore further refinement of the synthetic benchmark is unlikely to improve confidence in the real system.

---

# Recommended AIR Logging Architecture

Simplified recorder design:

1. Receive telemetry records via UART.
2. Append records to a RAM block buffer.
3. When block full:
   - write block to SD
   - reset buffer

Example parameters:

| Parameter | Value |
|------|------|
| Record size | 250 B |
| Record rate | 400 Hz |
| Records per block | 40 |
| Block size | 10,000 B |
| Block write period | ~100 ms |

Measured write latency (~7 ms) is well within the available 100 ms window.

---

# Required Runtime Instrumentation

When integrated into the AIR code, the following counters should be maintained:

- UART records received
- records written to SD
- records dropped
- maximum SD write time
- buffer high‑water mark
- no‑free‑buffer events
- radio service backlog

These metrics will allow system validation under real telemetry and radio load.

---

# Validation Plan

### Stage 1
AIR unit with UART ingest + SD logging only.

Measure:

- write latency
- buffer usage
- dropped records

### Stage 2
AIR unit with UART ingest + SD logging + radio traffic.

Measure:

- buffer pressure
- radio service delays
- SD latency under RF load

### Stage 3
Worst‑case system load with all subsystems active.

---

# Final Assessment

Based on measured SD latency and throughput:

**SPI SD logging on the ESP32‑S3 is capable of supporting the AIR recorder requirements with comfortable performance margin.**

The remaining engineering risk lies in **system interaction (UART + radio + logging)** rather than SD throughput itself.

Future validation should therefore occur **inside the full AIR application**, not through further synthetic storage benchmarks.

Note:  Pins that worked
// ---------- SPI pins ----------
int8_t sck  = D8;
int8_t miso = D10;
int8_t mosi = D9;
int8_t cs   = D7;

---

End of document.
