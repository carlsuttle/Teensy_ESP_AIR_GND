#include "config_store.h"

#include <Preferences.h>
#include <string.h>

namespace config_store {
namespace {

Preferences g_prefs;
AppConfig g_cfg;
constexpr char kCfgKey[] = "cfg";
constexpr char kNextSessionKey[] = "next_sess";
constexpr char kLastSessionKey[] = "last_sess";
constexpr char kLastNameKey[] = "last_name";

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

struct LegacyAppConfigV3 {
  char ap_ssid[33];
  char ap_pass[65];
  uint8_t reserved_transport[20];
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
  c.uart_port = 1;
  c.uart_rx_pin = 3;
  c.uart_tx_pin = 4;
  c.uart_baud = 921600;
  c.source_rate_hz = 50;
  c.ui_rate_hz = 30;
  c.log_rate_hz = 50;
  c.log_mode = 1;
  c.radio_state_only = 0;
  c.radio_lr_mode = 1;
  c.standalone_bench = 1;
  c.max_log_bytes = 4UL * 1024UL * 1024UL;
  strncpy(c.record_prefix, "air", sizeof(c.record_prefix) - 1);
}

void sanitizeRecordPrefix(char* prefix, size_t len) {
  if (!prefix || len == 0U) return;
  bool have_visible = false;
  for (size_t i = 0; i + 1U < len && prefix[i] != '\0'; ++i) {
    const char c = prefix[i];
    const bool ok = (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') ||
                    (c == '-');
    prefix[i] = ok ? c : '-';
    if (prefix[i] != '-') have_visible = true;
  }
  prefix[len - 1U] = '\0';
  if (!have_visible || prefix[0] == '\0') {
    strncpy(prefix, "air", len - 1U);
    prefix[len - 1U] = '\0';
  }
}

void sanitize(AppConfig& c) {
  c.source_rate_hz = clampv<uint16_t>(c.source_rate_hz, 50U, 1600U);
  c.ui_rate_hz = 30U;
  c.log_rate_hz = c.source_rate_hz;
  if (c.max_log_bytes < 512UL * 1024UL) c.max_log_bytes = 512UL * 1024UL;
  if (c.uart_baud < 115200UL) c.uart_baud = 115200UL;
  if (c.ap_ssid[0] == '\0') strncpy(c.ap_ssid, "Telemetry", sizeof(c.ap_ssid) - 1);
  if (c.ap_pass[0] == '\0') strncpy(c.ap_pass, "telemetry", sizeof(c.ap_pass) - 1);
  c.ap_ssid[sizeof(c.ap_ssid) - 1] = '\0';
  c.ap_pass[sizeof(c.ap_pass) - 1] = '\0';
  sanitizeRecordPrefix(c.record_prefix, sizeof(c.record_prefix));
  c.log_mode = 1;
  c.radio_state_only = c.radio_state_only ? 1U : 0U;
  c.radio_lr_mode = c.radio_lr_mode ? 1U : 0U;
  c.standalone_bench = c.standalone_bench ? 1U : 0U;
}

void saveInternal() {
  g_prefs.putBytes(kCfgKey, &g_cfg, sizeof(g_cfg));
}

}  // namespace

void begin() {
  setDefaults(g_cfg);
  g_prefs.begin("air_cfg", false);
  if (g_prefs.getBytesLength(kCfgKey) == sizeof(g_cfg)) {
    g_prefs.getBytes(kCfgKey, &g_cfg, sizeof(g_cfg));
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
  } else if (g_prefs.getBytesLength(kCfgKey) == sizeof(LegacyAppConfigV3)) {
    LegacyAppConfigV3 legacy = {};
    g_prefs.getBytes(kCfgKey, &legacy, sizeof(legacy));
    strlcpy(g_cfg.ap_ssid, legacy.ap_ssid, sizeof(g_cfg.ap_ssid));
    strlcpy(g_cfg.ap_pass, legacy.ap_pass, sizeof(g_cfg.ap_pass));
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
  } else if (g_prefs.getBytesLength(kCfgKey) == sizeof(LegacyAppConfigV2)) {
    LegacyAppConfigV2 legacy = {};
    g_prefs.getBytes(kCfgKey, &legacy, sizeof(legacy));
    strlcpy(g_cfg.ap_ssid, legacy.ap_ssid, sizeof(g_cfg.ap_ssid));
    strlcpy(g_cfg.ap_pass, legacy.ap_pass, sizeof(g_cfg.ap_pass));
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
  } else if (g_prefs.getBytesLength(kCfgKey) == sizeof(LegacyAppConfigV1)) {
    LegacyAppConfigV1 legacy = {};
    g_prefs.getBytes(kCfgKey, &legacy, sizeof(legacy));
    strlcpy(g_cfg.ap_ssid, legacy.ap_ssid, sizeof(g_cfg.ap_ssid));
    strlcpy(g_cfg.ap_pass, legacy.ap_pass, sizeof(g_cfg.ap_pass));
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
  if (g_cfg.log_mode != 1U) {
    g_cfg.log_mode = 1U;
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

uint32_t nextLogSessionId() {
  uint32_t next = g_prefs.getULong(kNextSessionKey, 1U);
  if (next == 0U) next = 1U;
  const uint32_t current = next;
  uint32_t following = current + 1U;
  if (following == 0U) following = 1U;
  g_prefs.putULong(kNextSessionKey, following);
  return current;
}

void noteLogSessionUsed(uint32_t session_id) {
  if (session_id == 0U) return;
  uint32_t next = session_id + 1U;
  if (next == 0U) next = 1U;
  const uint32_t stored = g_prefs.getULong(kNextSessionKey, 1U);
  if (stored < next) g_prefs.putULong(kNextSessionKey, next);
}

bool lastClosedLogName(String& out_name) {
  out_name = g_prefs.getString(kLastNameKey, "");
  return !out_name.isEmpty();
}

bool lastClosedLogNameForSession(uint32_t session_id, String& out_name) {
  out_name = "";
  if (session_id == 0U) return false;
  const uint32_t stored_session = g_prefs.getULong(kLastSessionKey, 0U);
  if (stored_session != session_id) return false;
  out_name = g_prefs.getString(kLastNameKey, "");
  return !out_name.isEmpty();
}

void noteClosedLog(uint32_t session_id, const String& name) {
  if (session_id == 0U || name.isEmpty()) return;
  g_prefs.putULong(kLastSessionKey, session_id);
  g_prefs.putString(kLastNameKey, name);
}

}  // namespace config_store
