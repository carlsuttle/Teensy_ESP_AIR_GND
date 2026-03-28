#pragma once

#include <Arduino.h>

#include "teensy_link.h"
#include "types_shared.h"

namespace teensy_api {

struct CommandAckResult {
  bool tx_ok = false;
  bool ack_seen = false;
  bool ack_ok = false;
  uint16_t command = 0U;
  uint32_t ack_code = 0U;
  teensy_link::Snapshot snapshot = {};
};

struct CarrySummary {
  uint32_t sent = 0U;
  uint32_t received = 0U;
  uint32_t pass = 0U;
  uint32_t fail = 0U;
  uint32_t timeout = 0U;
};

struct ReplayBatchBenchmarkSummary {
  uint32_t duration_ms = 0U;
  uint16_t batch_hz = 0U;
  uint16_t records_per_batch = 0U;
  uint32_t sent = 0U;
  uint32_t received = 0U;
  uint32_t pass = 0U;
  uint32_t fail = 0U;
  uint32_t timeout = 0U;
  uint32_t elapsed_ms = 0U;
  float validated_rps = 0.0f;
};

bool waitForAck(uint16_t command, uint32_t ack_seq_before, uint32_t timeout_ms, CommandAckResult& out);
bool getFusionSettings(CommandAckResult& out, uint32_t timeout_ms = 1000U);
bool setCaptureSettings(uint16_t hz, CommandAckResult& out, uint32_t timeout_ms = 1000U);
bool saveCaptureSettings(CommandAckResult& out, uint32_t timeout_ms = 1000U);
bool setStreamRate(uint16_t ws_rate_hz, uint16_t log_rate_hz,
                   CommandAckResult& out, uint32_t timeout_ms = 1000U);
bool setFusionSettings(const telem::CmdSetFusionSettingsV1& cmd,
                       CommandAckResult& out, uint32_t timeout_ms = 1000U);

void printStatus(Stream& out);
CarrySummary runCarrySignatureTest(Stream& out, uint8_t count);
CarrySummary runCarrySequenceCsvTest(Stream& out, uint32_t duration_ms, uint8_t window_limit);
ReplayBatchBenchmarkSummary runReplayBatchBenchmark(Stream& out, uint32_t duration_ms,
                                                   uint16_t batch_hz, uint16_t records_per_batch);

}  // namespace teensy_api
