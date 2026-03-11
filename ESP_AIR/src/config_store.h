#pragma once

#include <Arduino.h>

struct AppConfig {
  char ap_ssid[33];
  char ap_pass[65];
  char gnd_ip[16];
  uint16_t udp_local_port;
  uint16_t udp_gnd_port;
  uint8_t uart_port;
  uint8_t uart_rx_pin;
  uint8_t uart_tx_pin;
  uint32_t uart_baud;
  uint8_t source_rate_hz;
  uint8_t ui_rate_hz;
  uint8_t log_rate_hz;  // Mirrors source_rate_hz for protocol compatibility.
  uint8_t log_mode;     // Always on.
  uint32_t max_log_bytes;
};

namespace config_store {

void begin();
const AppConfig& get();
void update(const AppConfig& cfg);
void factoryReset();

}  // namespace config_store
