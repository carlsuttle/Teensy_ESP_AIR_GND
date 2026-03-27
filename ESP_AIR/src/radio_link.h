#pragma once

#include <Arduino.h>

#include "config_store.h"
#include "types_shared.h"
#include "teensy_link.h"

namespace radio_link {

struct Stats {
  uint32_t tx_packets = 0;
  uint32_t tx_bytes = 0;
  uint32_t tx_drop = 0;
  uint32_t tx_state_packets = 0;
  uint32_t tx_unified_packets = 0;
  uint32_t source_snapshots_seen = 0;
  uint32_t latest_source_seq_seen = 0;
  uint32_t latest_source_t_us_seen = 0;
  uint32_t latest_source_rx_ms_seen = 0;
  uint32_t latest_source_seen_ms = 0;
  uint32_t publish_attempts = 0;
  uint32_t publish_ok = 0;
  uint32_t publish_skip_no_state = 0;
  uint32_t publish_skip_no_peer = 0;
  uint32_t publish_skip_rate = 0;
  uint32_t publish_skip_not_new = 0;
  uint32_t last_source_seq = 0;
  uint32_t last_source_t_us = 0;
  uint32_t last_tx_ms = 0;
  uint32_t last_publish_attempt_ms = 0;
  uint32_t last_publish_age_ms = 0;
  uint32_t rx_packets = 0;
  uint32_t rx_bytes = 0;
  uint32_t rx_bad_len = 0;
  uint32_t rx_bad_magic = 0;
  uint32_t rx_unknown = 0;
  uint32_t last_rx_ms = 0;
};

void begin(const AppConfig& cfg);
void reconfigure(const AppConfig& cfg);
void poll();
void noteSourceSnapshot(uint32_t seq, uint32_t t_us, uint32_t last_rx_ms);
void publish(const teensy_link::Snapshot& snap);
bool publishState(const telem::TelemetryFullStateV1& state, uint32_t seq, uint32_t t_us);
bool publishStressState(const telem::TelemetryFullStateV1& state, uint32_t seq, uint32_t t_us);
Stats stats();
size_t txQueueFree();
bool stateOnlyMode();
bool longRangeMode();
bool hasPeer();
String peerMac();
bool radioReady();
void setVerbose(bool enabled);
bool verbose();
void setRecorderEnabled(bool enabled);
bool takeNetworkResetRequest();
void resetNetworkState();

}  // namespace radio_link
