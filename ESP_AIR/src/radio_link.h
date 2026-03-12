#pragma once

#include <Arduino.h>

#include "config_store.h"
#include "types_shared.h"
#include "uart_telem.h"

namespace radio_link {

struct Stats {
  uint32_t tx_packets = 0;
  uint32_t tx_bytes = 0;
  uint32_t tx_drop = 0;
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
void publish(const uart_telem::Snapshot& snap);
bool publishState(const telem::TelemetryFullStateV1& state, uint32_t seq, uint32_t t_us);
bool publishStressState(const telem::TelemetryFullStateV1& state, uint32_t seq, uint32_t t_us);
Stats stats();
size_t txQueueFree();
bool stateOnlyMode();
bool hasPeer();
String peerMac();
bool radioReady();
void setRecorderEnabled(bool enabled);
bool takeNetworkResetRequest();
void resetNetworkState();

}  // namespace radio_link
