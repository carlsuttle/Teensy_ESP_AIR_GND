#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>

#include "config_store.h"
#include "types_shared.h"

namespace udp_telem {

struct Stats {
  uint32_t rx_packets = 0;
  uint32_t rx_bytes = 0;
  uint32_t frames_ok = 0;
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
  uint32_t seq = 0;
  uint32_t t_us = 0;
  telem::TelemetryFullStateV1 state = {};
  telem::FusionSettingsV1 fusion_settings = {};
  uint16_t ack_command = 0;
  bool ack_ok = false;
  uint32_t ack_code = 0;
  uint32_t ack_rx_seq = 0;
  Stats stats = {};
};

void begin(const AppConfig& cfg);
void reconfigure(const AppConfig& cfg);
void restart(const AppConfig& cfg);
void poll();
Snapshot snapshot();
bool sendSetFusionSettings(const telem::CmdSetFusionSettingsV1& cmd);
bool sendGetFusionSettings();
bool sendSetStreamRate(const telem::CmdSetStreamRateV1& cmd);
bool sendResetNetwork();
bool hasLearnedSender();
String targetSenderMac();
String lastSenderMac();
IPAddress targetSenderIp();
uint16_t targetSenderPort();
IPAddress lastSenderIp();
uint16_t lastSenderPort();

}  // namespace udp_telem
