#include <Arduino.h>
#include <ctype.h>
#include <LittleFS.h>
#include <string.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "config_store.h"
#include "log_store.h"
#include "uart_telem.h"
#include "ws_server.h"

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

void ensureConfiguredStreamRate() {
  const AppConfig& cfg = config_store::get();
  const bool targetChanged =
      (cfg.source_rate_hz != g_last_stream_rate_ui_hz) || (cfg.log_rate_hz != g_last_stream_rate_log_hz);
  const uint32_t now = millis();
  if (!targetChanged && !g_wait_stream_rate_ack) return;
  if (!targetChanged && (uint32_t)(now - g_last_stream_rate_tx_ms) < 1000U) return;

  telem::CmdSetStreamRateV1 cmd = {};
  cmd.ws_rate_hz = cfg.source_rate_hz;
  cmd.log_rate_hz = cfg.log_rate_hz;
  if (!uart_telem::sendSetStreamRate(cmd)) return;

  g_last_stream_rate_ui_hz = cfg.source_rate_hz;
  g_last_stream_rate_log_hz = cfg.log_rate_hz;
  g_last_stream_rate_tx_ms = now;
  g_wait_stream_rate_ack = true;
}

void printConsoleHelp() {
  Serial.println("AIR COMMANDS:");
  Serial.println("  help / h      - show command list");
  Serial.println("  esp32loopback - run active UART TX/RX loopback test");
  Serial.println("  rxscan        - scan candidate RX pins for UART bytes");
  Serial.println("  getfusion     - send CMD_GET_FUSION_SETTINGS to Teensy");
  Serial.println("  setfusion g a m r - send CMD_SET_FUSION_SETTINGS");
  Serial.println("  stats         - start 1Hz STAT stream");
  Serial.println("  x             - stop active stream/mode");
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
      } else if (strcmp(g_console_line, "esp32loopback") == 0) {
        Serial.println("ESP32LOOPBACK START timeout_ms=120");
        const auto r = uart_telem::runLoopbackTest(120U);
        if (r.pass) {
          Serial.printf("ESP32LOOPBACK PASS sent=%lu recv=%lu mismatches=%lu elapsed_ms=%lu\n",
                        (unsigned long)r.sent, (unsigned long)r.received,
                        (unsigned long)r.mismatches, (unsigned long)r.elapsed_ms);
        } else {
          Serial.printf("ESP32LOOPBACK FAIL sent=%lu recv=%lu mismatches=%lu elapsed_ms=%lu\n",
                        (unsigned long)r.sent, (unsigned long)r.received,
                        (unsigned long)r.mismatches, (unsigned long)r.elapsed_ms);
          if (r.first_mismatch_index != 0xFFU) {
            Serial.printf("ESP32LOOPBACK first_mismatch idx=%u exp=0x%02X got=0x%02X\n",
                          (unsigned)r.first_mismatch_index,
                          (unsigned)r.expected,
                          (unsigned)r.actual);
          }
        }
      } else if (strcmp(g_console_line, "rxscan") == 0) {
        static const uint8_t kCandidates[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 43, 44};
        Serial.println("RXSCAN START baud=921600 dwell_ms=300");
        uint8_t bestPin = 0xFFU;
        uint32_t bestBytes = 0;
        for (size_t i = 0; i < sizeof(kCandidates); ++i) {
          uint32_t bytes = 0;
          const uint8_t pin = kCandidates[i];
          if (!uart_telem::probeRxPin(pin, 921600U, 300U, bytes)) {
            Serial.printf("RXSCAN pin=%u probe_error\n", (unsigned)pin);
            continue;
          }
          Serial.printf("RXSCAN pin=%u bytes=%lu\n", (unsigned)pin, (unsigned long)bytes);
          if (bytes > bestBytes) {
            bestBytes = bytes;
            bestPin = pin;
          }
        }
        if (bestPin != 0xFFU && bestBytes > 0U) {
          Serial.printf("RXSCAN BEST pin=%u bytes=%lu\n", (unsigned)bestPin, (unsigned long)bestBytes);
        } else {
          Serial.println("RXSCAN RESULT no_activity");
        }
      } else if (strcmp(g_console_line, "getfusion") == 0 || strcmp(g_console_line, "get fusion") == 0) {
        const bool ok = uart_telem::sendGetFusionSettings();
        Serial.printf("GETFUSION tx_ok=%u\n", ok ? 1U : 0U);
        g_wait_getfusion_ack = ok;
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
                        ok ? 1U : 0U, (double)cmd.gain, (double)cmd.accelerationRejection,
                        (double)cmd.magneticRejection, (unsigned)cmd.recoveryTriggerPeriod);
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
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("ESP_AIR boot");

  config_store::begin();
  const AppConfig& cfg = config_store::get();
  Serial.printf("UART port=%u rx=%u tx=%u baud=%lu\n",
                (unsigned)cfg.uart_port,
                (unsigned)cfg.uart_rx_pin,
                (unsigned)cfg.uart_tx_pin,
                (unsigned long)cfg.uart_baud);

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(cfg.ap_ssid, cfg.ap_pass);
  (void)esp_wifi_set_ps(WIFI_PS_NONE);
  (void)esp_wifi_set_max_tx_power(78);
  Serial.printf("AP ssid=%s ip=%s\n", cfg.ap_ssid, WiFi.softAPIP().toString().c_str());

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  uart_telem::begin(cfg);
  g_last_stream_rate_ui_hz = 0;
  g_last_stream_rate_log_hz = 0;
  g_last_stream_rate_tx_ms = 0;
  g_wait_stream_rate_ack = true;
  log_store::begin(cfg);
  ws_server::begin();
  printConsoleHelp();
}

void loop() {
  handleConsoleCommands();
  uart_telem::poll();
  const auto snap = uart_telem::snapshot();
  ws_server::loop();

  if (g_wait_getfusion_ack && snap.has_ack && snap.ack_command == 101U &&
      snap.ack_rx_seq != g_last_printed_ack_seq) {
    g_last_printed_ack_seq = snap.ack_rx_seq;
    g_wait_getfusion_ack = false;
    if (snap.has_state) {
      Serial.printf("GETFUSION ACK ok=%u code=%lu gain=%.3f accRej=%.2f magRej=%.2f rec=%u\n",
                    snap.ack_ok ? 1U : 0U, (unsigned long)snap.ack_code,
                    (double)snap.state.fusion_gain, (double)snap.state.fusion_accel_rej,
                    (double)snap.state.fusion_mag_rej, (unsigned)snap.state.fusion_recovery_period);
    } else {
      Serial.printf("GETFUSION ACK ok=%u code=%lu (no state yet)\n",
                    snap.ack_ok ? 1U : 0U, (unsigned long)snap.ack_code);
    }
  }

  if (snap.has_ack && snap.ack_command == 102U && snap.ack_rx_seq != g_last_printed_ack_seq) {
    g_last_printed_ack_seq = snap.ack_rx_seq;
    g_wait_stream_rate_ack = !snap.ack_ok;
  }

  ensureConfiguredStreamRate();

  const uint32_t now = millis();
  if (g_stats_streaming && (uint32_t)(now - g_last_stat_ms) >= 1000U) {
    g_last_stat_ms = now;
    Serial.printf("STAT clients=%lu has=%u bytes=%lu ok=%lu crc=%lu cobs=%lu len=%lu drop=%lu ack=%u cmd=%u ack_ok=%u code=%lu aseq=%lu\n",
                  (unsigned long)ws_server::clientCount(), (unsigned)snap.has_state,
                  (unsigned long)snap.stats.rx_bytes, (unsigned long)snap.stats.frames_ok,
                  (unsigned long)snap.stats.crc_err, (unsigned long)snap.stats.cobs_err,
                  (unsigned long)snap.stats.len_err, (unsigned long)snap.stats.drop,
                  (unsigned)(snap.has_ack ? 1U : 0U), (unsigned)snap.ack_command,
                  (unsigned)(snap.ack_ok ? 1U : 0U), (unsigned long)snap.ack_code,
                  (unsigned long)snap.ack_rx_seq);
  }
}
