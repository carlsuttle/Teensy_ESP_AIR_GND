#pragma once

#include <Arduino.h>
#include "config_store.h"
#include "types_shared.h"

namespace uart_telem {

struct RxStats {
  uint32_t rx_bytes;
  uint32_t frames_ok;
  uint32_t crc_err;
  uint32_t cobs_err;
  uint32_t len_err;
  uint32_t unknown_msg;
  uint32_t drop;
  uint32_t last_rx_ms;
};

struct Snapshot {
  bool has_state;
  telem::TelemetryFullStateV1 state;
  uint32_t seq;
  uint32_t t_us;
  bool has_fusion_settings;
  telem::FusionSettingsV1 fusion_settings;
  uint32_t fusion_rx_seq;
  RxStats stats;
  bool has_ack;
  uint32_t ack_rx_seq;
  uint16_t ack_command;
  bool ack_ok;
  uint32_t ack_code;
};

struct PendingState {
  telem::TelemetryFullStateV1 state;
  uint32_t seq;
  uint32_t t_us;
};

struct LoopbackResult {
  bool pass;
  uint32_t sent;
  uint32_t received;
  uint32_t mismatches;
  uint32_t elapsed_ms;
  uint8_t first_mismatch_index;
  uint8_t expected;
  uint8_t actual;
};

void begin(const AppConfig& cfg);
void reconfigure(const AppConfig& cfg);
void poll();
Snapshot snapshot();
bool popPendingState(PendingState& out);
LoopbackResult runLoopbackTest(uint32_t timeout_ms = 120U);
bool sendSetFusionSettings(const telem::CmdSetFusionSettingsV1& cmd);
bool sendGetFusionSettings();
bool sendSetStreamRate(const telem::CmdSetStreamRateV1& cmd);
bool probeRxPin(uint8_t rx_pin, uint32_t baud, uint32_t dwell_ms, uint32_t& out_bytes);

}  // namespace uart_telem
