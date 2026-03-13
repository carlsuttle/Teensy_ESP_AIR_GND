#pragma once

#include <Arduino.h>

#include "config_store.h"
#include "types_shared.h"

namespace radio_link {

struct Stats {
  uint32_t rx_packets = 0;
  uint32_t rx_bytes = 0;
  uint32_t frames_ok = 0;
  uint32_t state_packets = 0;
  uint32_t state_seq_gap = 0;
  uint32_t state_seq_rewind = 0;
  uint32_t crc_err = 0;
  uint32_t cobs_err = 0;
  uint32_t len_err = 0;
  uint32_t unknown_msg = 0;
  uint32_t drop = 0;
  uint32_t last_rx_ms = 0;
};

struct Snapshot {
  bool has_state = false;
  bool has_ack = false;
  bool has_fusion_settings = false;
  bool has_link_meta = false;
  bool has_log_status = false;
  uint32_t seq = 0;
  uint32_t t_us = 0;
  telem::TelemetryFullStateV1 state = {};
  telem::FusionSettingsV1 fusion_settings = {};
  telem::LinkMetaPayloadV1 link_meta = {};
  telem::LogStatusPayloadV1 log_status = {};
  uint16_t ack_command = 0;
  bool ack_ok = false;
  uint32_t ack_code = 0;
  uint32_t ack_rx_seq = 0;
  uint32_t radio_rtt_ms = 0;
  uint32_t radio_rtt_avg_ms = 0;
  uint32_t last_radio_pong_ms = 0;
  uint32_t uplink_ping_sent = 0;
  uint32_t uplink_ping_ok = 0;
  uint32_t uplink_ping_timeout = 0;
  uint32_t uplink_ping_miss_streak = 0;
  uint32_t last_uplink_ack_ms = 0;
  Stats stats = {};
};

void begin(const AppConfig& cfg);
void reconfigure(const AppConfig& cfg);
void restart(const AppConfig& cfg);
void poll();
Snapshot snapshot();
void resetStats();
bool sendSetFusionSettings(const telem::CmdSetFusionSettingsV1& cmd);
bool sendGetFusionSettings();
bool sendSetStreamRate(const telem::CmdSetStreamRateV1& cmd);
bool sendSetRadioMode(const telem::CmdSetRadioModeV1& cmd);
bool sendResetNetwork();
bool sendLogStart();
bool sendLogStop();
bool sendGetLogStatus();
bool hasLearnedSender();
String targetSenderMac();
String lastSenderMac();

}  // namespace radio_link
