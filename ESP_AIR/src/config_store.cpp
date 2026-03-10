#include "config_store.h"

#include <Preferences.h>
#include <string.h>

namespace config_store {
namespace {

Preferences g_prefs;
AppConfig g_cfg;

struct LegacyAppConfigV1 {
  char ap_ssid[33];
  char ap_pass[65];
  uint8_t uart_port;
  uint8_t uart_rx_pin;
  uint8_t uart_tx_pin;
  uint32_t uart_baud;
  uint8_t ui_rate_hz;
  uint8_t log_rate_hz;
  uint8_t log_mode;
  uint32_t max_log_bytes;
};

struct LegacyAppConfigV2 {
  char ap_ssid[33];
  char ap_pass[65];
  uint8_t uart_port;
  uint8_t uart_rx_pin;
  uint8_t uart_tx_pin;
  uint32_t uart_baud;
  uint8_t source_rate_hz;
  uint8_t ui_rate_hz;
  uint8_t log_rate_hz;
  uint8_t log_mode;
  uint32_t max_log_bytes;
};

template <typename T>
T clampv(T v, T lo, T hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void setDefaults(AppConfig& c) {
  memset(&c, 0, sizeof(c));
  strncpy(c.ap_ssid, "Telemetry", sizeof(c.ap_ssid) - 1);
  strncpy(c.ap_pass, "telemetry", sizeof(c.ap_pass) - 1);
  strncpy(c.gnd_ip, "192.168.4.1", sizeof(c.gnd_ip) - 1);
  c.udp_local_port = 9000;
  c.udp_gnd_port = 9000;
  c.uart_port = 1;
  c.uart_rx_pin = 3;
  c.uart_tx_pin = 4;
  c.uart_baud = 921600;
  c.source_rate_hz = 50;
  c.ui_rate_hz = 20;
  c.log_rate_hz = 50;
  c.log_mode = 0;
  c.max_log_bytes = 4UL * 1024UL * 1024UL;
}

void sanitize(AppConfig& c) {
  if (c.gnd_ip[0] == '\0') strncpy(c.gnd_ip, "192.168.4.1", sizeof(c.gnd_ip) - 1);
  c.gnd_ip[sizeof(c.gnd_ip) - 1] = '\0';
  c.udp_local_port = clampv<uint16_t>(c.udp_local_port, 1U, 65535U);
  c.udp_gnd_port = clampv<uint16_t>(c.udp_gnd_port, 1U, 65535U);
  c.source_rate_hz = clampv<uint8_t>(c.source_rate_hz, 1, 50);
  c.ui_rate_hz = clampv<uint8_t>(c.ui_rate_hz, 5, 20);
  c.log_rate_hz = clampv<uint8_t>(c.log_rate_hz, 1, 50);
  if (c.max_log_bytes < 512UL * 1024UL) c.max_log_bytes = 512UL * 1024UL;
  if (c.uart_baud < 115200UL) c.uart_baud = 115200UL;
  if (c.ap_ssid[0] == '\0') strncpy(c.ap_ssid, "Telemetry", sizeof(c.ap_ssid) - 1);
  if (c.ap_pass[0] == '\0') strncpy(c.ap_pass, "telemetry", sizeof(c.ap_pass) - 1);
  c.ap_ssid[sizeof(c.ap_ssid) - 1] = '\0';
  c.ap_pass[sizeof(c.ap_pass) - 1] = '\0';
  c.log_mode = c.log_mode ? 1 : 0;
}

void saveInternal() {
  g_prefs.putBytes("cfg", &g_cfg, sizeof(g_cfg));
}

}  // namespace

void begin() {
  setDefaults(g_cfg);
  g_prefs.begin("air_cfg", false);
  if (g_prefs.getBytesLength("cfg") == sizeof(g_cfg)) {
    g_prefs.getBytes("cfg", &g_cfg, sizeof(g_cfg));
    // One-time UART migration: move bridge to UART1, RX=3, TX=4
    // Handles known legacy mappings (RX=5/TX=6, RX=4/TX=5, RX=D2/TX=D3).
    if ((g_cfg.uart_rx_pin == 5 && g_cfg.uart_tx_pin == 6) ||
        (g_cfg.uart_rx_pin == 4 && g_cfg.uart_tx_pin == 5) ||
        (g_cfg.uart_rx_pin == D2 && g_cfg.uart_tx_pin == D3)) {
      g_cfg.uart_port = 1;
      g_cfg.uart_rx_pin = 3;
      g_cfg.uart_tx_pin = 4;
        g_cfg.uart_baud = 921600;
        saveInternal();
    }
  } else if (g_prefs.getBytesLength("cfg") == sizeof(LegacyAppConfigV2)) {
    LegacyAppConfigV2 legacy = {};
    g_prefs.getBytes("cfg", &legacy, sizeof(legacy));
    strlcpy(g_cfg.ap_ssid, legacy.ap_ssid, sizeof(g_cfg.ap_ssid));
    strlcpy(g_cfg.ap_pass, legacy.ap_pass, sizeof(g_cfg.ap_pass));
    strlcpy(g_cfg.gnd_ip, "192.168.4.1", sizeof(g_cfg.gnd_ip));
    g_cfg.udp_local_port = 9000U;
    g_cfg.udp_gnd_port = 9000U;
    g_cfg.uart_port = legacy.uart_port;
    g_cfg.uart_rx_pin = legacy.uart_rx_pin;
    g_cfg.uart_tx_pin = legacy.uart_tx_pin;
    g_cfg.uart_baud = legacy.uart_baud;
    g_cfg.source_rate_hz = legacy.source_rate_hz;
    g_cfg.ui_rate_hz = legacy.ui_rate_hz;
    g_cfg.log_rate_hz = legacy.log_rate_hz;
    g_cfg.log_mode = legacy.log_mode;
    g_cfg.max_log_bytes = legacy.max_log_bytes;
    saveInternal();
  } else if (g_prefs.getBytesLength("cfg") == sizeof(LegacyAppConfigV1)) {
    LegacyAppConfigV1 legacy = {};
    g_prefs.getBytes("cfg", &legacy, sizeof(legacy));
    strlcpy(g_cfg.ap_ssid, legacy.ap_ssid, sizeof(g_cfg.ap_ssid));
    strlcpy(g_cfg.ap_pass, legacy.ap_pass, sizeof(g_cfg.ap_pass));
    strlcpy(g_cfg.gnd_ip, "192.168.4.1", sizeof(g_cfg.gnd_ip));
    g_cfg.udp_local_port = 9000U;
    g_cfg.udp_gnd_port = 9000U;
    g_cfg.uart_port = legacy.uart_port;
    g_cfg.uart_rx_pin = legacy.uart_rx_pin;
    g_cfg.uart_tx_pin = legacy.uart_tx_pin;
    g_cfg.uart_baud = legacy.uart_baud;
    g_cfg.source_rate_hz = legacy.ui_rate_hz ? legacy.ui_rate_hz : 50U;
    g_cfg.ui_rate_hz = legacy.ui_rate_hz;
    g_cfg.log_rate_hz = legacy.log_rate_hz;
    g_cfg.log_mode = legacy.log_mode;
    g_cfg.max_log_bytes = legacy.max_log_bytes;
    saveInternal();
  } else {
    saveInternal();
  }
  // Force known-good Teensy bridge UART mapping for XIAO ESP32S3 wiring:
  // RX=D2(GPIO3), TX=D3(GPIO4), UART1 @ 921600.
  bool changed = false;
  if (g_cfg.uart_port != 1) {
    g_cfg.uart_port = 1;
    changed = true;
  }
  if (g_cfg.uart_rx_pin != 3) {
    g_cfg.uart_rx_pin = 3;
    changed = true;
  }
  if (g_cfg.uart_tx_pin != 4) {
    g_cfg.uart_tx_pin = 4;
    changed = true;
  }
  if (g_cfg.uart_baud != 921600UL) {
    g_cfg.uart_baud = 921600UL;
    changed = true;
  }
  if (strncmp(g_cfg.gnd_ip, "192.168.4.1", sizeof(g_cfg.gnd_ip)) != 0) {
    strlcpy(g_cfg.gnd_ip, "192.168.4.1", sizeof(g_cfg.gnd_ip));
    changed = true;
  }
  if (g_cfg.source_rate_hz != 50U) {
    g_cfg.source_rate_hz = 50U;
    changed = true;
  }
  if (g_cfg.log_rate_hz != 50U) {
    g_cfg.log_rate_hz = 50U;
    changed = true;
  }
  const AppConfig beforeSanitize = g_cfg;
  sanitize(g_cfg);
  if (changed || memcmp(&beforeSanitize, &g_cfg, sizeof(g_cfg)) != 0) saveInternal();
}

const AppConfig& get() { return g_cfg; }

void update(const AppConfig& cfg) {
  g_cfg = cfg;
  sanitize(g_cfg);
  saveInternal();
}

void factoryReset() {
  setDefaults(g_cfg);
  sanitize(g_cfg);
  saveInternal();
}

}  // namespace config_store
