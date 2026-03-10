#include "config_store.h"

#include <Preferences.h>
#include <string.h>

namespace config_store {
namespace {

Preferences g_prefs;
AppConfig g_cfg;

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
  c.udp_listen_port = 9000;
  c.source_rate_hz = 50;
  c.ui_rate_hz = 20;
  c.log_rate_hz = 50;
  c.log_mode = 0;
  c.max_log_bytes = 4UL * 1024UL * 1024UL;
}

void sanitize(AppConfig& c) {
  c.udp_listen_port = clampv<uint16_t>(c.udp_listen_port, 1U, 65535U);
  c.source_rate_hz = clampv<uint8_t>(c.source_rate_hz, 1, 50);
  c.ui_rate_hz = clampv<uint8_t>(c.ui_rate_hz, 1, 20);
  c.log_rate_hz = clampv<uint8_t>(c.log_rate_hz, 1, 50);
  c.log_mode = c.log_mode ? 1 : 0;
  if (c.max_log_bytes < 512UL * 1024UL) c.max_log_bytes = 512UL * 1024UL;
  if (c.ap_ssid[0] == '\0') strncpy(c.ap_ssid, "Telemetry", sizeof(c.ap_ssid) - 1);
  if (c.ap_pass[0] == '\0') strncpy(c.ap_pass, "telemetry", sizeof(c.ap_pass) - 1);
  c.ap_ssid[sizeof(c.ap_ssid) - 1] = '\0';
  c.ap_pass[sizeof(c.ap_pass) - 1] = '\0';
}

void saveInternal() {
  g_prefs.putBytes("cfg", &g_cfg, sizeof(g_cfg));
}

}  // namespace

void begin() {
  setDefaults(g_cfg);
  g_prefs.begin("gnd_cfg", false);
  if (g_prefs.getBytesLength("cfg") == sizeof(g_cfg)) {
    g_prefs.getBytes("cfg", &g_cfg, sizeof(g_cfg));
  } else {
    saveInternal();
  }
  const AppConfig beforeSanitize = g_cfg;
  if (g_cfg.source_rate_hz != 50U) g_cfg.source_rate_hz = 50U;
  if (g_cfg.log_rate_hz != 50U) g_cfg.log_rate_hz = 50U;
  sanitize(g_cfg);
  if (memcmp(&beforeSanitize, &g_cfg, sizeof(g_cfg)) != 0) saveInternal();
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
