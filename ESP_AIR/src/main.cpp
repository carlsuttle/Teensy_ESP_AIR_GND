#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ctype.h>
#include <esp_wifi.h>
#include <string.h>

#include "config_store.h"
#include "log_store.h"
#include "uart_telem.h"
#include "udp_link.h"

namespace {

uint32_t g_last_stat_ms = 0;
char g_console_line[96];
uint8_t g_console_idx = 0;
bool g_stats_streaming = false;
bool g_wait_getfusion_ack = false;
bool g_wait_stream_rate_ack = false;
uint32_t g_last_printed_ack_seq = 0;
uint32_t g_last_stream_rate_tx_ms = 0;
uint8_t g_last_stream_rate_ui_hz = 0;
uint8_t g_last_stream_rate_log_hz = 0;
wl_status_t g_last_wifi_status = (wl_status_t)255;
bool g_wifi_ready = false;
bool g_teensy_ready = false;
bool g_teensy_wait_printed = false;
bool g_last_wifi_link_ok = false;
bool g_wifi_drop_pending = false;
bool g_wifi_drop_once_used = false;
uint32_t g_wifi_drop_due_ms = 0;
bool g_wifi_offon_pending = false;
bool g_wifi_offon_once_used = false;
uint32_t g_wifi_offon_due_ms = 0;
constexpr uint32_t kWifiDropDelayMs = 3000U;
constexpr uint32_t kWifiOffOnDelayMs = 9000U;
constexpr bool kEnableAirFileLogging = false;

void beginWifiStation();
void restartWifiStation();

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD:
      return "NO_SHIELD";
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO_SSID";
    case WL_SCAN_COMPLETED:
      return "SCAN_DONE";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

bool wifiLinkHealthy() {
  if (WiFi.status() != WL_CONNECTED) return false;
  wifi_ap_record_t ap_info = {};
  return esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
}

bool wifiConnected() { return wifiLinkHealthy(); }

void resetWifiStatusFlags() {
  g_wifi_ready = false;
  g_last_wifi_link_ok = false;
  g_last_wifi_status = (wl_status_t)255;
  g_wifi_drop_pending = false;
  g_wifi_drop_once_used = false;
  g_wifi_drop_due_ms = 0;
  g_wifi_offon_pending = false;
  g_wifi_offon_once_used = false;
  g_wifi_offon_due_ms = 0;
}

void ensureConfiguredStreamRate() {
  const AppConfig& cfg = config_store::get();
  const uint8_t target_stream_hz = cfg.source_rate_hz;
  const uint8_t target_log_hz = cfg.source_rate_hz;
  const bool targetChanged =
      (target_stream_hz != g_last_stream_rate_ui_hz) || (target_log_hz != g_last_stream_rate_log_hz);
  const uint32_t now = millis();
  if (!targetChanged && !g_wait_stream_rate_ack) return;
  if (!targetChanged && (uint32_t)(now - g_last_stream_rate_tx_ms) < 1000U) return;

  telem::CmdSetStreamRateV1 cmd = {};
  cmd.ws_rate_hz = target_stream_hz;
  cmd.log_rate_hz = target_log_hz;
  if (!uart_telem::sendSetStreamRate(cmd)) return;

  g_last_stream_rate_ui_hz = target_stream_hz;
  g_last_stream_rate_log_hz = target_log_hz;
  g_last_stream_rate_tx_ms = now;
  g_wait_stream_rate_ack = true;
}

bool sendConfiguredStreamRateNow() {
  const AppConfig& cfg = config_store::get();
  telem::CmdSetStreamRateV1 cmd = {};
  cmd.ws_rate_hz = cfg.source_rate_hz;
  cmd.log_rate_hz = cfg.source_rate_hz;
  const bool ok = uart_telem::sendSetStreamRate(cmd);
  if (ok) {
    g_last_stream_rate_ui_hz = cmd.ws_rate_hz;
    g_last_stream_rate_log_hz = cmd.log_rate_hz;
    g_last_stream_rate_tx_ms = millis();
    g_wait_stream_rate_ack = true;
  }
  return ok;
}

void printConsoleHelp() {
  Serial.println("AIR COMMANDS:");
  Serial.println("  help / h      - show command list");
  Serial.println("  getfusion     - send CMD_GET_FUSION_SETTINGS to Teensy");
  Serial.println("  kickteensy    - resend current stream-rate command to Teensy");
  Serial.println("  resendrate    - same as kickteensy");
  Serial.println("  tx1           - send current state once to GND");
  Serial.println("  udpclear      - clear AIR UDP state and stop UDP socket");
  Serial.println("  udpreopen     - reopen AIR UDP socket only");
  Serial.println("  wifidrop      - disconnect/reconnect Wi-Fi without radio off");
  Serial.println("  wifioffon     - power-cycle Wi-Fi only");
  Serial.println("  reudp         - restart AIR UDP socket");
  Serial.println("  resetnet      - restart AIR Wi-Fi/UDP network side");
  Serial.println("  setfusion g a m r - send CMD_SET_FUSION_SETTINGS");
  Serial.println("  stats         - start 1Hz STAT stream");
  Serial.println("  x             - stop active stream/mode");
}

void printStats(const uart_telem::Snapshot& snap) {
  const auto udp = udp_link::stats();
  Serial.printf(
      "STAT unit=AIR seq=%lu t_us=%lu has=%u ack=%u cmd=%u ack_ok=%u code=%lu "
      "rx_bytes=%lu ok=%lu crc=%lu cobs=%lu len=%lu unk=%lu drop=%lu "
      "udp_tx=%lu udp_rx=%lu udp_drop=%lu\n",
      (unsigned long)snap.seq,
      (unsigned long)snap.t_us,
      snap.has_state ? 1U : 0U,
      snap.has_ack ? 1U : 0U,
      (unsigned)snap.ack_command,
      snap.ack_ok ? 1U : 0U,
      (unsigned long)snap.ack_code,
      (unsigned long)snap.stats.rx_bytes,
      (unsigned long)snap.stats.frames_ok,
      (unsigned long)snap.stats.crc_err,
      (unsigned long)snap.stats.cobs_err,
      (unsigned long)snap.stats.len_err,
      (unsigned long)snap.stats.unknown_msg,
      (unsigned long)snap.stats.drop,
      (unsigned long)udp.tx_packets,
      (unsigned long)udp.rx_packets,
      (unsigned long)udp.tx_drop);
}

void handleConsoleCommands() {
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (g_console_idx == 0) continue;
      g_console_line[g_console_idx] = '\0';
      g_console_idx = 0;

      for (size_t i = 0; g_console_line[i] != '\0'; ++i) {
        g_console_line[i] = (char)tolower((unsigned char)g_console_line[i]);
      }

      if (strcmp(g_console_line, "help") == 0 || strcmp(g_console_line, "h") == 0) {
        printConsoleHelp();
      } else if (strcmp(g_console_line, "getfusion") == 0 || strcmp(g_console_line, "get fusion") == 0) {
        const bool ok = uart_telem::sendGetFusionSettings();
        Serial.printf("GETFUSION tx_ok=%u\n", ok ? 1U : 0U);
        g_wait_getfusion_ack = ok;
      } else if (strcmp(g_console_line, "kickteensy") == 0 || strcmp(g_console_line, "kickstream") == 0 ||
                 strcmp(g_console_line, "resendrate") == 0) {
        const AppConfig& cfg = config_store::get();
        const bool ok = sendConfiguredStreamRateNow();
        Serial.printf("KICKTEENSY tx_ok=%u ws_hz=%u log_hz=%u\n",
                      ok ? 1U : 0U,
                      (unsigned)cfg.source_rate_hz,
                      (unsigned)cfg.source_rate_hz);
      } else if (strcmp(g_console_line, "tx1") == 0 || strcmp(g_console_line, "sendstate") == 0) {
        const auto snap = uart_telem::snapshot();
        if (!snap.has_state) {
          Serial.println("TX1 tx_ok=0 reason=no_state");
        } else {
          const bool ok = udp_link::publishStressState(snap.state, snap.seq, snap.t_us);
          Serial.printf("TX1 tx_ok=%u seq=%lu t_us=%lu\n",
                        ok ? 1U : 0U,
                        (unsigned long)snap.seq,
                        (unsigned long)snap.t_us);
        }
      } else if (strcmp(g_console_line, "udpclear") == 0) {
        udp_link::resetNetworkState();
        Serial.println("UDPCLEAR done state=cleared socket=stopped");
      } else if (strcmp(g_console_line, "udpreopen") == 0) {
        const AppConfig& cfg = config_store::get();
        udp_link::reconfigure(cfg);
        Serial.printf("UDPREOPEN local=%u peer=%s:%u\n",
                      (unsigned)cfg.udp_local_port,
                      cfg.gnd_ip,
                      (unsigned)cfg.udp_gnd_port);
      } else if (strcmp(g_console_line, "wifidrop") == 0) {
        resetWifiStatusFlags();
        WiFi.disconnect(false, false);
        delay(10);
        WiFi.reconnect();
        Serial.println("WIFIDROP disconnect+reconnect");
      } else if (strcmp(g_console_line, "wifioffon") == 0) {
        resetWifiStatusFlags();
        WiFi.disconnect(false, false);
        WiFi.mode(WIFI_OFF);
        delay(50);
        beginWifiStation();
        Serial.println("WIFIOFFON radio_power_cycle");
      } else if (strcmp(g_console_line, "reudp") == 0 || strcmp(g_console_line, "restartudp") == 0) {
        const AppConfig& cfg = config_store::get();
        udp_link::reconfigure(cfg);
        Serial.printf("REUDP local=%u peer=%s:%u\n",
                      (unsigned)cfg.udp_local_port,
                      cfg.gnd_ip,
                      (unsigned)cfg.udp_gnd_port);
      } else if (strcmp(g_console_line, "resetnet") == 0 || strcmp(g_console_line, "netreset") == 0) {
        restartWifiStation();
      } else if (strncmp(g_console_line, "setfusion ", 10) == 0) {
        float g = 0.0f, a = 0.0f, m = 0.0f;
        unsigned r = 0U;
        const int n = sscanf(g_console_line + 10, "%f %f %f %u", &g, &a, &m, &r);
        if (n == 4) {
          telem::CmdSetFusionSettingsV1 cmd = {};
          cmd.gain = g;
          cmd.accelerationRejection = a;
          cmd.magneticRejection = m;
          cmd.recoveryTriggerPeriod = (uint16_t)r;
          const bool ok = uart_telem::sendSetFusionSettings(cmd);
          const bool ok_get = ok ? uart_telem::sendGetFusionSettings() : false;
          if (ok_get) g_wait_getfusion_ack = true;
          Serial.printf("SETFUSION tx_ok=%u gain=%.3f accRej=%.2f magRej=%.2f rec=%u\n",
                        ok ? 1U : 0U,
                        (double)cmd.gain,
                        (double)cmd.accelerationRejection,
                        (double)cmd.magneticRejection,
                        (unsigned)cmd.recoveryTriggerPeriod);
        } else {
          Serial.println("SETFUSION usage: setfusion <gain> <accelRej> <magRej> <recovery>");
        }
      } else if (strcmp(g_console_line, "stats") == 0) {
        g_stats_streaming = true;
        g_last_stat_ms = 0;
        Serial.println("STATS START");
      } else if (strcmp(g_console_line, "x") == 0) {
        g_stats_streaming = false;
        Serial.println("STATS STOP");
      } else {
        Serial.print("unknown cmd: ");
        Serial.println(g_console_line);
      }
    } else if ((size_t)g_console_idx + 1U < sizeof(g_console_line)) {
      g_console_line[g_console_idx++] = c;
    }
  }
}

void beginWifiStation() {
  const AppConfig& cfg = config_store::get();
  const IPAddress local_ip(192, 168, 4, 2);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  (void)esp_wifi_set_ps(WIFI_PS_NONE);
  (void)esp_wifi_set_max_tx_power(78);
  if (!WiFi.config(local_ip, gateway, subnet)) {
    Serial.println("AIR WARN wifi static IP config failed");
  }

  Serial.printf("UDP local=%u ground=%s:%u\n",
                (unsigned)cfg.udp_local_port,
                cfg.gnd_ip,
                (unsigned)cfg.udp_gnd_port);
  Serial.printf("Joining Wi-Fi ssid=%s\n", cfg.ap_ssid);
  WiFi.begin(cfg.ap_ssid, cfg.ap_pass);
}

void restartWifiStation() {
  const AppConfig& cfg = config_store::get();
  Serial.println("AIR CMD reset_network");
  udp_link::resetNetworkState();
  resetWifiStatusFlags();
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
  delay(50);
  beginWifiStation();
  g_wait_stream_rate_ack = true;
  g_last_stream_rate_tx_ms = 0;
  g_last_stream_rate_ui_hz = 0;
  g_last_stream_rate_log_hz = 0;
  udp_link::reconfigure(cfg);
}

void updateWifiReadiness() {
  const AppConfig& cfg = config_store::get();
  const uint32_t now = millis();
  const wl_status_t status = WiFi.status();
  const bool link_ok = wifiLinkHealthy();
  const bool was_link_ok = g_last_wifi_link_ok;
  if (status != g_last_wifi_status) {
    g_last_wifi_status = status;
    Serial.printf("Wi-Fi status=%s(%d)\n", wifiStatusName(status), (int)status);
  }
  if (link_ok != g_last_wifi_link_ok) {
    g_last_wifi_link_ok = link_ok;
    if (!link_ok && status == WL_CONNECTED) {
      Serial.println("AIR WARN wifi_link_unhealthy ap_info=missing");
    }
  }

  if (link_ok) {
    g_wifi_drop_pending = false;
    g_wifi_drop_once_used = false;
    g_wifi_drop_due_ms = 0;
    g_wifi_offon_pending = false;
    g_wifi_offon_once_used = false;
    g_wifi_offon_due_ms = 0;
    if (!g_wifi_ready) {
      udp_link::reconfigure(cfg);
      Serial.printf("AIR READY wifi ip=%s gateway=%s gnd=%s:%u\n",
                    WiFi.localIP().toString().c_str(),
                    WiFi.gatewayIP().toString().c_str(),
                    cfg.gnd_ip,
                    (unsigned)cfg.udp_gnd_port);
      g_wifi_ready = true;
    }
    return;
  }

  if (g_wifi_ready) {
    Serial.printf("AIR WAIT gnd_ap status=%s(%d) ssid=%s\n",
                  wifiStatusName(status),
                  (int)status,
                  cfg.ap_ssid);
    g_wifi_ready = false;
  }

  if (was_link_ok && !g_wifi_drop_pending && !g_wifi_drop_once_used && !g_wifi_offon_pending &&
      !g_wifi_offon_once_used) {
    g_wifi_drop_pending = true;
    g_wifi_drop_due_ms = now + kWifiDropDelayMs;
    g_wifi_offon_pending = true;
    g_wifi_offon_due_ms = now + kWifiOffOnDelayMs;
    Serial.printf("AIR INFO wifi_recovery_scheduled drop_ms=%lu offon_ms=%lu\n",
                  (unsigned long)kWifiDropDelayMs,
                  (unsigned long)kWifiOffOnDelayMs);
  }

  if (g_wifi_drop_pending && !g_wifi_drop_once_used &&
      (int32_t)(now - g_wifi_drop_due_ms) >= 0) {
    WiFi.disconnect(false, false);
    delay(10);
    WiFi.reconnect();
    g_wifi_drop_pending = false;
    g_wifi_drop_once_used = true;
    Serial.println("AIR INFO wifi_disconnect_reconnect_delayed");
  }

  if (g_wifi_offon_pending && !g_wifi_offon_once_used &&
      (int32_t)(now - g_wifi_offon_due_ms) >= 0) {
    g_wifi_ready = false;
    g_last_wifi_link_ok = false;
    g_last_wifi_status = (wl_status_t)255;
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    delay(50);
    beginWifiStation();
    g_wifi_drop_pending = false;
    g_wifi_drop_once_used = true;
    g_wifi_offon_pending = false;
    g_wifi_offon_once_used = true;
    Serial.println("AIR INFO wifi_power_cycle_delayed");
  }
}

void updateTeensyReadiness(const uart_telem::Snapshot& snap) {
  const uint32_t now = millis();
  const bool fresh = snap.stats.last_rx_ms != 0U && (uint32_t)(now - snap.stats.last_rx_ms) <= 3000U;

  if (fresh) {
    if (!g_teensy_ready) {
      Serial.printf("AIR READY teensy_link seq=%lu t_us=%lu\n",
                    (unsigned long)snap.seq,
                    (unsigned long)snap.t_us);
      g_teensy_ready = true;
      g_teensy_wait_printed = false;
    }
    return;
  }

  if (g_teensy_ready) {
    Serial.printf("AIR WAIT teensy telemetry timeout_ms=%lu\n",
                  (unsigned long)(snap.stats.last_rx_ms ? (now - snap.stats.last_rx_ms) : 0U));
    uart_telem::resync();
    Serial.printf("AIR WARN teensy_uart_resync rx_bytes=%lu ok=%lu cobs=%lu len=%lu unk=%lu\n",
                  (unsigned long)snap.stats.rx_bytes,
                  (unsigned long)snap.stats.frames_ok,
                  (unsigned long)snap.stats.cobs_err,
                  (unsigned long)snap.stats.len_err,
                  (unsigned long)snap.stats.unknown_msg);
    g_teensy_ready = false;
    g_wait_stream_rate_ack = true;
    g_last_stream_rate_tx_ms = 0;
    return;
  }

  if (!g_teensy_wait_printed) {
    Serial.println("AIR WAIT teensy telemetry");
    g_teensy_wait_printed = true;
  }
}

void publishPendingTelemetry() {
  if (!wifiConnected()) return;

  uart_telem::PendingState pending = {};
  while (uart_telem::popPendingState(pending)) {
    (void)udp_link::publishState(pending.state, pending.seq, pending.t_us);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("ESP_AIR boot");

  config_store::begin();
  const AppConfig& cfg = config_store::get();
  Serial.printf("UART port=%u rx=%u tx=%u baud=%lu\n",
                (unsigned)cfg.uart_port,
                (unsigned)cfg.uart_rx_pin,
                (unsigned)cfg.uart_tx_pin,
                (unsigned long)cfg.uart_baud);

  beginWifiStation();

  const bool fs_ready = LittleFS.begin(true);
  if (!fs_ready) {
    Serial.println("LittleFS mount failed");
  }

  uart_telem::begin(cfg);
  udp_link::begin(cfg);
  log_store::begin(cfg, fs_ready && kEnableAirFileLogging);
  if (!kEnableAirFileLogging) {
    Serial.println("AIR INFO file_logging_disabled");
  }
  g_last_stream_rate_ui_hz = 0;
  g_last_stream_rate_log_hz = 0;
  g_last_stream_rate_tx_ms = 0;
  g_wait_stream_rate_ack = true;
  printConsoleHelp();
}

void loop() {
  handleConsoleCommands();
  if (udp_link::takeNetworkResetRequest()) {
    restartWifiStation();
  }
  updateWifiReadiness();
  uart_telem::poll();
  udp_link::poll();

  const auto snap = uart_telem::snapshot();
  updateTeensyReadiness(snap);

  if (g_wait_getfusion_ack && snap.has_ack && snap.ack_command == 101U &&
      snap.ack_rx_seq != g_last_printed_ack_seq) {
    g_last_printed_ack_seq = snap.ack_rx_seq;
    g_wait_getfusion_ack = false;
    if (snap.has_state) {
      Serial.printf("GETFUSION ACK ok=%u code=%lu gain=%.3f accRej=%.2f magRej=%.2f rec=%u\n",
                    snap.ack_ok ? 1U : 0U,
                    (unsigned long)snap.ack_code,
                    (double)snap.state.fusion_gain,
                    (double)snap.state.fusion_accel_rej,
                    (double)snap.state.fusion_mag_rej,
                    (unsigned)snap.state.fusion_recovery_period);
    } else {
      Serial.printf("GETFUSION ACK ok=%u code=%lu (no state yet)\n",
                    snap.ack_ok ? 1U : 0U,
                    (unsigned long)snap.ack_code);
    }
  }

  if (snap.has_ack && snap.ack_command == 102U && snap.ack_rx_seq != g_last_printed_ack_seq) {
    g_last_printed_ack_seq = snap.ack_rx_seq;
    g_wait_stream_rate_ack = !snap.ack_ok;
  }

  ensureConfiguredStreamRate();
  publishPendingTelemetry();
  if (wifiConnected()) {
    udp_link::publish(snap);
  }

  const uint32_t now = millis();
  if (g_stats_streaming && (uint32_t)(now - g_last_stat_ms) >= 1000U) {
    g_last_stat_ms = now;
    printStats(snap);
  }
}
