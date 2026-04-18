#pragma once

#include <Arduino.h>

struct AppConfig {
  char ap_ssid[33];
  char ap_pass[65];
  uint16_t reserved_transport_port;  // Preserves stored Preferences layout after removing obsolete transport fields.
  uint16_t source_rate_hz;
  uint16_t ui_rate_hz;   // Fixed browser update rate for normal mode.
  uint16_t log_rate_hz;  // Mirrors source_rate_hz so capture and logging stay aligned.
  uint8_t log_mode;      // Always on.
  uint8_t radio_state_only;  // Reserved legacy config byte; canonical live streaming ignores this field.
  uint8_t radio_lr_mode;     // 0=normal protocol, 1=LR protocol enabled for ESP-NOW testing.
  uint8_t reserved_flags0;
  uint8_t sta_enable;
  uint16_t sta_connect_timeout_ms;
  uint32_t max_log_bytes;
  char sta_ssid[33];
  char sta_pass[65];
};

namespace config_store {

void begin();
const AppConfig& get();
void update(const AppConfig& cfg);
void factoryReset();

}  // namespace config_store
