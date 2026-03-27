#pragma once

#include <Arduino.h>

namespace ws_server {

void begin();
void loop();
uint32_t clientCount();
void resetCounters();

struct Stats {
  uint32_t clients = 0;
  uint32_t ws_state_seq = 0;
  uint32_t last_state_seq_sent = 0;
  uint32_t last_source_t_us_sent = 0;
  uint32_t last_radio_rx_ms_seen = 0;
  uint32_t last_ui_tx_ms = 0;
  uint32_t last_ui_tx_latency_ms = 0;
  uint32_t max_ui_tx_latency_ms = 0;
};

Stats stats();

}  // namespace ws_server
