#pragma once

#include <Arduino.h>

struct AppConfig {
  char ap_ssid[33];
  char ap_pass[65];
  uint16_t reserved_transport_port;  // Preserves stored Preferences layout after removing obsolete transport fields.
  uint16_t source_rate_hz;
  uint16_t ui_rate_hz;
  uint16_t log_rate_hz;  // AIR->GND telemetry downlink rate in normal mixed mode.
  uint8_t log_mode;      // Always on.
  uint8_t radio_state_only;  // Reuses reserved config byte: 0=mixed protocol, 1=state-only stress mode.
  uint32_t max_log_bytes;
};

namespace config_store {

void begin();
const AppConfig& get();
void update(const AppConfig& cfg);
void factoryReset();

}  // namespace config_store
