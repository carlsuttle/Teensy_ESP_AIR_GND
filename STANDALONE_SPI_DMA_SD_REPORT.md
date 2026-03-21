# Standalone SPI/DMA and SPI/SD Performance Report

Date: 2026-03-21

## Scope

This report covers standalone bench-prototype performance only.

It does not claim full system-integrated performance inside the final avionics
stack.

Authoritative project-history reference:

- [AI_AVIONICS_PROJECT_RECORD.md](c:/Users/dell/Platformio/esp32_crsf_telemetry/Teensy_ESP_AIR_GND/AI_AVIONICS_PROJECT_RECORD.md)

## 1. SPI/DMA Inter-Processor Bridge

Prototype:

- ESP32-S3 SPI master
- Teensy 4.0 SPI slave
- `40 MHz` SPI clock
- DMA-backed transport on both sides
- fixed full-duplex framed transactions
- benchmark record size: `160 bytes`

Exact packed-record fit per transaction:

- `2048 bytes` -> `12` records max
- `4096 bytes` -> `25` records max
- `8192 bytes` -> `51` records max

Demonstrated clean full-duplex operating points:

| Transaction Size | Transaction Rate | Approx Records / Transaction / Direction | Approx Records / Second / Direction |
|---|---:|---:|---:|
| `2048` | `300 Hz` | `10` | `3000` |
| `4096` | `200 Hz` | `15` | `3000` |
| `8192` | `100 Hz` | `35` | `3500` |

Raw full-duplex transport result at `8192 bytes`:

- exact fit: `51` `160-byte` records per transaction per direction
- clean through `110 Hz`
- practical raw ceiling: about `5627 records/s` each way

Recommended safe operating point:

- `8192-byte` transactions
- `100 Hz`
- about `50 x 160-byte` records per transaction per direction
- about `5000 records/s` each way full duplex

Approximate useful payload at the recommended safe point:

- about `800,000 bytes/s` each way
- about `781 KiB/s` each way
- about `1.56 MiB/s` combined

Practical interpretation:

- the SPI/DMA bridge is proven as a credible bounded-latency bulk transport
- `100 Hz` transaction cadence keeps latency bounded to about `10 ms`
- this is a strong candidate for replay and bulk-transfer workloads

## 2. SPI / microSD Logging

Platform:

- ESP_AIR shared microSD backend
- SPI SD card interface
- standalone recorder-style benchmark

Benchmark target used in the standalone SD investigation:

- `250-byte` records
- `400 Hz`
- about `97.7 KB/s`

Observed standalone SD write behavior:

- block size: `10,000 bytes`
- average write time: about `7 ms`
- worst write time: about `9-10 ms`
- no slow flush events observed in final standalone runs

Implied instantaneous write bandwidth:

- about `1.4 MB/s`

Margin against original recorder target:

- about `10-14x`

Current SD backend wiring on `ESP_AIR`:

- `CS=2`
- `SCK=7`
- `MISO=8`
- `MOSI=9`

Current SD init behavior:

- try `40 MHz` first
- fall back to `26 MHz` if needed for mount reliability

Practical interpretation:

- standalone SD throughput is not the limiting factor
- remaining risk is integrated interaction under real load:
  - UART ingest
  - radio servicing
  - logging task scheduling
  - filesystem latency spikes

## Summary

Standalone bench results support these claims:

- SPI/DMA bridge:
  - safe recommended full-duplex rate: about `5000` `160-byte` records/s each
    way
  - with `8192-byte` transactions at `100 Hz`
- SPI/SD logging:
  - standalone SD write path comfortably exceeds the original AIR recorder
    target
  - measured standalone write capability is about `1.4 MB/s` instantaneous with
    `10 KB` blocks

Overall conclusion:

- SPI/DMA inter-processor bulk transfer is proven in standalone bench work
- SPI/SD logging throughput is proven in standalone bench work
- the remaining work is system integration and interaction validation, not basic
  feasibility
