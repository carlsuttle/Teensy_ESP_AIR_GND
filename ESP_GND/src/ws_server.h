#pragma once

#include <Arduino.h>

namespace ws_server {

void begin();
void loop();
uint32_t clientCount();
void resetCounters();
void setSyntheticLiveMode(bool enabled);
bool syntheticLiveMode();
void printSyntheticLiveStatus(Stream& out);

struct Stats {
  uint32_t clients = 0;
  uint32_t ws_state_seq = 0;
  uint32_t last_state_seq_sent = 0;
  uint32_t last_source_t_us_sent = 0;
  uint32_t last_radio_rx_ms_seen = 0;
  uint32_t last_ui_tx_ms = 0;
  uint32_t last_ui_tx_latency_ms = 0;
  uint32_t max_ui_tx_latency_ms = 0;
  uint32_t http_root_gets = 0;
  uint32_t http_asset_gets = 0;
  uint32_t http_not_found = 0;
  uint32_t ws_connects = 0;
  uint32_t ws_disconnects = 0;
  uint32_t ws_snapshots_sent = 0;
  uint32_t ws_live_updates_sent = 0;
  uint32_t ws_send_failures = 0;
  uint32_t ws_control_rx = 0;
  uint32_t ws_control_tx_air = 0;
  uint32_t ws_control_ack = 0;
  uint32_t ws_control_tx = 0;
  uint32_t last_ws_connect_ms = 0;
  uint32_t last_ws_send_ms = 0;
};

Stats stats();

}  // namespace ws_server
