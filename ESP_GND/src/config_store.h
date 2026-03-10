#pragma once

#include <Arduino.h>

struct AppConfig {
  char ap_ssid[33];
  char ap_pass[65];
  uint16_t udp_listen_port;
  uint8_t source_rate_hz;
  uint8_t ui_rate_hz;
  uint8_t log_rate_hz;
  uint8_t log_mode;  // Reserved for parity with the existing client UI.
  uint32_t max_log_bytes;
};

namespace config_store {

void begin();
const AppConfig& get();
void update(const AppConfig& cfg);
void factoryReset();

}  // namespace config_store
