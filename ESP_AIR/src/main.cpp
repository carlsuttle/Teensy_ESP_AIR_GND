#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <ctype.h>
#include <esp_wifi.h>
#include <string.h>

#include "config_store.h"
#include "log_store.h"
#include "sd_capture_test.h"
#include "sd_card_test.h"
#include "uart_telem.h"
#include "radio_link.h"
#include "spi_bridge.h"
#include "replay_bridge.h"

namespace {

uint32_t g_last_stat_ms = 0;
char g_console_line[96];
uint8_t g_console_idx = 0;
bool g_stats_streaming = false;
bool g_wait_getfusion_ack = false;
bool g_wait_stream_rate_ack = false;
uint32_t g_last_printed_ack_seq = 0;
uint32_t g_last_stream_rate_tx_ms = 0;
uint16_t g_last_stream_rate_ui_hz = 0;
uint16_t g_last_stream_rate_log_hz = 0;
bool g_radio_ready = false;
bool g_link_ready = false;
bool g_link_wait_printed = false;
bool g_teensy_ready = false;
bool g_teensy_wait_printed = false;
bool g_gpio_pulse_active = false;
uint8_t g_gpio_pulse_pin = 0U;
uint32_t g_gpio_pulse_until_ms = 0U;
bool g_sd_capture_was_active = false;
bool g_baseline_active = false;
bool g_baseline_completed = false;
uint32_t g_baseline_duration_ms = 0U;
uint32_t g_baseline_started_ms = 0U;
uint32_t g_baseline_stopped_ms = 0U;
uint32_t g_console_log_session_id = 0U;
struct ReplayCaptureRun {
  bool active = false;
  bool stop_requested = false;
  uint32_t session_id = 0U;
  uint32_t started_ms = 0U;
} g_replay_capture = {};
uint32_t g_last_radio_tx_packets = 0U;
uint32_t g_last_radio_rx_packets = 0U;
uint32_t g_last_source_seq_seen = 0U;
uint32_t g_last_source_progress_ms = 0U;
uint32_t g_last_radio_progress_ms = 0U;
uint32_t g_last_radio_recovery_ms = 0U;
constexpr bool kEnableAirFileLogging = true;
constexpr size_t kStateTxReserveSlots = 4U;
constexpr uint32_t kRadioProgressTimeoutMs = 6000U;
constexpr uint32_t kRadioRecoveryCooldownMs = 10000U;

struct CaptureBenchBaseline {
  radio_link::Stats radio = {};
  uart_telem::RxStats uart = {};
  uint32_t started_ms = 0U;
};

CaptureBenchBaseline g_sd_capture_baseline = {};

void beginWifiStation();
void restartWifiStation();
void beginSdCaptureBenchmark(uint32_t duration_ms);
void printSdCaptureImpactReport();
void beginBaselineBenchmark(uint32_t duration_ms);
void stopBaselineBenchmark();
void printBaselineImpactReport();
void printAirLogStatus();
bool beginReplayCapture(const String* source_override = nullptr);
void serviceReplayCapture();

String shortLogName(const String& path) {
  return path.startsWith("/logs/") ? path.substring(6) : path;
}

uint32_t nextConsoleLogSessionId() {
  g_console_log_session_id++;
  if (g_console_log_session_id == 0U) g_console_log_session_id = 1U;
  return g_console_log_session_id;
}

void stopGpioPulse() {
  if (!g_gpio_pulse_active) return;
  digitalWrite(g_gpio_pulse_pin, LOW);
  pinMode(g_gpio_pulse_pin, INPUT);
  Serial.printf("SETPIN done gpio=%u state=0\r\n", (unsigned)g_gpio_pulse_pin);
  g_gpio_pulse_active = false;
}

void startGpioPulse(uint8_t pin) {
  stopGpioPulse();
  g_gpio_pulse_pin = pin;
  pinMode(g_gpio_pulse_pin, OUTPUT);
  digitalWrite(g_gpio_pulse_pin, HIGH);
  g_gpio_pulse_until_ms = millis() + 5000U;
  g_gpio_pulse_active = true;
  Serial.printf("SETPIN gpio=%u state=1 duration_ms=5000\r\n", (unsigned)g_gpio_pulse_pin);
}

void serviceGpioPulse() {
  if (!g_gpio_pulse_active) return;
  if ((int32_t)(millis() - g_gpio_pulse_until_ms) >= 0) {
    stopGpioPulse();
  }
}

void resetRadioWatchdog() {
  g_last_radio_tx_packets = 0U;
  g_last_radio_rx_packets = 0U;
  g_last_source_seq_seen = 0U;
  g_last_source_progress_ms = millis();
  g_last_radio_progress_ms = millis();
}

void resetWifiStatusFlags() {
  g_radio_ready = false;
  g_link_ready = false;
  g_link_wait_printed = false;
}

void ensureConfiguredStreamRate() {
  const AppConfig& cfg = config_store::get();
  const uint16_t target_stream_hz = cfg.source_rate_hz;
  const uint16_t target_log_hz = cfg.log_rate_hz;
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
  cmd.log_rate_hz = cfg.log_rate_hz;
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
  Serial.println("  sdprobe       - probe microSD card over SPI and print init/mount status");
  Serial.println("  sdwrite       - run tiny binary create/write/delete test on microSD");
  Serial.println("  base1m        - run 60-second radio/UART baseline with no SD capture");
  Serial.println("  basestop      - stop active baseline run");
  Serial.println("  basestat      - print baseline status");
  Serial.println("  sdcap1m       - capture every Teensy state to one SD binary file for 60 seconds");
  Serial.println("  sdcapstop     - stop active SD capture benchmark");
  Serial.println("  sdcapstat     - print SD capture benchmark status");
  Serial.println("  logstart      - start real AIR SD logging session");
  Serial.println("  logstartid <n> - start real AIR SD logging with an explicit session id");
  Serial.println("  logstop       - stop real AIR SD logging session");
  Serial.println("  logstat       - print real AIR SD logging status");
  Serial.println("  latestlog     - print latest .tlog on SD");
  Serial.println("  largestlog    - print largest .tlog on SD");
  Serial.println("  latestlogsession <n> - print latest .tlog for a given session id");
  Serial.println("  verifylog     - copy latest .tlog to *_copy.tlog and verify byte-exact match");
  Serial.println("  expandlogs    - expand every .tlog on SD into a sibling .csv file");
  Serial.println("  comparelogs a b - compare two .tlog files, ignoring fusion outputs");
  Serial.println("  replaycapture - replay latest .tlog into Teensy while logging returned state");
  Serial.println("  replaycapfile <name> - replay a specific .tlog into Teensy while logging returned state");
  Serial.println("  replayfile <name> - replay a specific .tlog into Teensy without rerecording");
  Serial.println("  replaylargest - replay the largest .tlog into Teensy without rerecording");
  Serial.println("  replaycapstat - print replay-capture progress");
  Serial.println("  setpin <gpio> - drive a GPIO high for 5 seconds, then return it low");
  Serial.println("  tx1           - send current state once to GND");
  Serial.println("  linkclear     - clear AIR radio-link state and stop ESP-NOW");
  Serial.println("  linkopen      - reopen AIR radio-link only");
  Serial.println("  wifidrop      - clear discovered peer state");
  Serial.println("  wifioffon     - power-cycle Wi-Fi only");
  Serial.println("  relink        - restart AIR radio-link");
  Serial.println("  resetnet      - restart AIR Wi-Fi/ESP-NOW side");
  Serial.println("  setfusion g a m r - send CMD_SET_FUSION_SETTINGS");
  Serial.println("  stats         - start 1Hz STAT stream");
  Serial.println("  x             - stop active stream/mode");
}

void printStats(const uart_telem::Snapshot& snap) {
  const auto link = radio_link::stats();
  const auto cap = sd_capture_test::stats();
  const auto spi = spi_bridge::stats();
  Serial.printf(
      "STAT unit=AIR seq=%lu t_us=%lu has=%u ack=%u cmd=%u ack_ok=%u code=%lu "
      "rx_bytes=%lu ok=%lu crc=%lu cobs=%lu len=%lu unk=%lu drop=%lu "
      "link_tx=%lu link_rx=%lu link_drop=%lu spi_txn=%lu spi_fail=%lu spi_state=%lu spi_replay=%lu spi_crc=%lu spi_type=%lu spi_rxof=%lu spi_txof=%lu spi_hdr=%08lX/%u/%u/%u sdcap=%u sdcap_drop=%lu sdcap_qmax=%lu\n",
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
      (unsigned long)link.tx_packets,
      (unsigned long)link.rx_packets,
      (unsigned long)link.tx_drop,
      (unsigned long)spi.transactions_completed,
      (unsigned long)spi.transaction_failures,
      (unsigned long)spi.state_records_received,
      (unsigned long)spi.replay_records_sent,
      (unsigned long)spi.rx_crc_errors,
      (unsigned long)spi.rx_type_errors,
      (unsigned long)spi.rx_overflows,
      (unsigned long)spi.tx_overflows,
      (unsigned long)spi.last_magic,
      (unsigned)spi.last_version,
      (unsigned)spi.last_type,
      (unsigned)spi.last_len,
      cap.active ? 1U : 0U,
      (unsigned long)cap.dropped,
      (unsigned long)cap.queue_max);
}

void beginSdCaptureBenchmark(uint32_t duration_ms) {
  stopBaselineBenchmark();
  const auto snap = uart_telem::snapshot();
  g_sd_capture_baseline.radio = radio_link::stats();
  g_sd_capture_baseline.uart = snap.stats;
  g_sd_capture_baseline.started_ms = millis();
  sd_capture_test::clearCompleted();
  const bool ok = sd_capture_test::start(duration_ms);
  const auto cap = sd_capture_test::stats();
  Serial.printf("SDCAP START ok=%u duration_ms=%lu init_hz=%lu file=%s\r\n",
                ok ? 1U : 0U,
                (unsigned long)duration_ms,
                (unsigned long)cap.init_hz,
                cap.file_name[0] ? cap.file_name : "(none)");
  if (!ok) {
    sd_capture_test::printReport(Serial, cap);
  }
}

void beginBaselineBenchmark(uint32_t duration_ms) {
  sd_capture_test::stop(false);
  g_sd_capture_was_active = false;
  const auto snap = uart_telem::snapshot();
  g_sd_capture_baseline.radio = radio_link::stats();
  g_sd_capture_baseline.uart = snap.stats;
  g_sd_capture_baseline.started_ms = millis();
  g_baseline_active = true;
  g_baseline_completed = false;
  g_baseline_duration_ms = duration_ms;
  g_baseline_started_ms = g_sd_capture_baseline.started_ms;
  g_baseline_stopped_ms = 0U;
  Serial.printf("BASELINE START duration_ms=%lu\r\n", (unsigned long)duration_ms);
}

void stopBaselineBenchmark() {
  if (!g_baseline_active) return;
  g_baseline_active = false;
  g_baseline_completed = true;
  g_baseline_stopped_ms = millis();
}

void printBaselineImpactReport() {
  const auto radio_now = radio_link::stats();
  const auto snap = uart_telem::snapshot();
  const uint32_t elapsed_ms =
      (g_baseline_stopped_ms >= g_sd_capture_baseline.started_ms)
          ? (g_baseline_stopped_ms - g_sd_capture_baseline.started_ms)
          : 0U;
  Serial.printf("BASELINE RESULT elapsed_ms=%lu duration_ms=%lu\r\n",
                (unsigned long)elapsed_ms,
                (unsigned long)g_baseline_duration_ms);
  Serial.printf(
      "BASELINE IMPACT radio_tx=%lu radio_rx=%lu radio_drop=%lu uart_ok=%lu uart_crc=%lu uart_cobs=%lu uart_len=%lu uart_unk=%lu uart_drop=%lu\r\n",
      (unsigned long)(radio_now.tx_packets - g_sd_capture_baseline.radio.tx_packets),
      (unsigned long)(radio_now.rx_packets - g_sd_capture_baseline.radio.rx_packets),
      (unsigned long)(radio_now.tx_drop - g_sd_capture_baseline.radio.tx_drop),
      (unsigned long)(snap.stats.frames_ok - g_sd_capture_baseline.uart.frames_ok),
      (unsigned long)(snap.stats.crc_err - g_sd_capture_baseline.uart.crc_err),
      (unsigned long)(snap.stats.cobs_err - g_sd_capture_baseline.uart.cobs_err),
      (unsigned long)(snap.stats.len_err - g_sd_capture_baseline.uart.len_err),
      (unsigned long)(snap.stats.unknown_msg - g_sd_capture_baseline.uart.unknown_msg),
      (unsigned long)(snap.stats.drop - g_sd_capture_baseline.uart.drop));
}

void printSdCaptureImpactReport() {
  const auto cap = sd_capture_test::stats();
  const auto radio_now = radio_link::stats();
  const auto snap = uart_telem::snapshot();
  const uint32_t elapsed_ms =
      (cap.stopped_ms >= g_sd_capture_baseline.started_ms) ? (cap.stopped_ms - g_sd_capture_baseline.started_ms) : 0U;

  Serial.printf(
      "SDCAP RESULT elapsed_ms=%lu timed_out=%u file=%s records=%lu bytes=%lu qmax=%lu cap_drop=%lu\r\n",
      (unsigned long)elapsed_ms,
      cap.timed_out ? 1U : 0U,
      cap.file_name[0] ? cap.file_name : "(none)",
      (unsigned long)cap.records_written,
      (unsigned long)cap.bytes_written,
      (unsigned long)cap.queue_max,
      (unsigned long)cap.dropped);
  Serial.printf(
      "SDCAP IMPACT radio_tx=%lu radio_rx=%lu radio_drop=%lu uart_ok=%lu uart_crc=%lu uart_cobs=%lu uart_len=%lu uart_unk=%lu uart_drop=%lu\r\n",
      (unsigned long)(radio_now.tx_packets - g_sd_capture_baseline.radio.tx_packets),
      (unsigned long)(radio_now.rx_packets - g_sd_capture_baseline.radio.rx_packets),
      (unsigned long)(radio_now.tx_drop - g_sd_capture_baseline.radio.tx_drop),
      (unsigned long)(snap.stats.frames_ok - g_sd_capture_baseline.uart.frames_ok),
      (unsigned long)(snap.stats.crc_err - g_sd_capture_baseline.uart.crc_err),
      (unsigned long)(snap.stats.cobs_err - g_sd_capture_baseline.uart.cobs_err),
      (unsigned long)(snap.stats.len_err - g_sd_capture_baseline.uart.len_err),
      (unsigned long)(snap.stats.unknown_msg - g_sd_capture_baseline.uart.unknown_msg),
      (unsigned long)(snap.stats.drop - g_sd_capture_baseline.uart.drop));
  sd_capture_test::printReport(Serial, cap);
}

void printAirLogStatus() {
  const auto recorder = log_store::recorderStatus();
  const auto stats = log_store::stats();
  Serial.printf("AIRLOG enabled=%u active=%u backend_ready=%u media_present=%u session=%lu init_hz=%lu bytes=%lu free=%lu\r\n",
                recorder.feature_enabled ? 1U : 0U,
                recorder.active ? 1U : 0U,
                recorder.backend_ready ? 1U : 0U,
                recorder.media_present ? 1U : 0U,
                (unsigned long)recorder.session_id,
                (unsigned long)recorder.init_hz,
                (unsigned long)recorder.bytes_written,
                (unsigned long)recorder.free_bytes);
  Serial.printf("AIRLOG queue_cur=%lu queue_max=%lu enqueued=%lu dropped=%lu written=%lu blocks_written=%lu blocks_dropped=%lu no_free=%lu min_free=%lu\r\n",
                (unsigned long)stats.queue_cur,
                (unsigned long)stats.queue_max,
                (unsigned long)stats.enqueued,
                (unsigned long)stats.dropped,
                (unsigned long)stats.records_written,
                (unsigned long)stats.blocks_written,
                (unsigned long)stats.blocks_dropped,
                (unsigned long)stats.no_free_block_events,
                (unsigned long)stats.min_free_blocks_seen);
  Serial.printf("AIRLOG fs_open_last_ms=%lu fs_open_max_ms=%lu fs_write_last_ms=%lu fs_write_max_ms=%lu fs_close_last_ms=%lu fs_close_max_ms=%lu max_write_bytes=%lu\r\n",
                (unsigned long)stats.fs_open_last_ms,
                (unsigned long)stats.fs_open_max_ms,
                (unsigned long)stats.fs_write_last_ms,
                (unsigned long)stats.fs_write_max_ms,
                (unsigned long)stats.fs_close_last_ms,
                (unsigned long)stats.fs_close_max_ms,
                (unsigned long)stats.max_write_bytes);
}

bool beginReplayCapture(const String* source_override) {
  if (g_replay_capture.active) {
    Serial.println("AIRREPLAYCAP START ok=0 reason=already_active");
    return false;
  }
  if (log_store::busy()) {
    Serial.println("AIRREPLAYCAP START ok=0 reason=logger_busy");
    return false;
  }
  if (replay_bridge::active()) {
    Serial.println("AIRREPLAYCAP START ok=0 reason=replay_busy");
    return false;
  }

  String source_name;
  if (source_override && !source_override->isEmpty()) {
    source_name = *source_override;
  } else if (!log_store::latestLogName(source_name)) {
    Serial.println("AIRREPLAYCAP START ok=0 reason=no_source_log");
    return false;
  }

  const uint32_t session_id = nextConsoleLogSessionId();
  radio_link::setRecorderEnabled(true);
  if (!log_store::startSession(session_id)) {
    Serial.printf("AIRREPLAYCAP START ok=0 reason=logstart_failed session=%lu\r\n",
                  (unsigned long)session_id);
    return false;
  }

  const String capture_name = log_store::currentFileName();
  if (!replay_bridge::startFile(source_name)) {
    log_store::stopSession();
    Serial.printf("AIRREPLAYCAP START ok=0 reason=replay_start_failed src=%s session=%lu\r\n",
                  shortLogName(source_name).c_str(),
                  (unsigned long)session_id);
    return false;
  }

  g_replay_capture.active = true;
  g_replay_capture.stop_requested = false;
  g_replay_capture.session_id = session_id;
  g_replay_capture.started_ms = millis();

  const auto replay = replay_bridge::status();
  Serial.printf("AIRREPLAYCAP START ok=1 src=%s dst=%s session=%lu records_total=%lu\r\n",
                shortLogName(source_name).c_str(),
                shortLogName(capture_name).c_str(),
                (unsigned long)session_id,
                (unsigned long)replay.records_total);
  return true;
}

bool beginReplayDirect(const String& source_name) {
  if (g_replay_capture.active) {
    Serial.println("AIRREPLAY START ok=0 reason=replay_capture_active");
    return false;
  }
  if (replay_bridge::active()) {
    Serial.println("AIRREPLAY START ok=0 reason=already_active");
    return false;
  }
  if (source_name.isEmpty()) {
    Serial.println("AIRREPLAY START ok=0 reason=no_source_log");
    return false;
  }
  if (!replay_bridge::startFile(source_name)) {
    Serial.printf("AIRREPLAY START ok=0 reason=replay_start_failed src=%s\r\n",
                  shortLogName(source_name).c_str());
    return false;
  }
  const auto replay = replay_bridge::status();
  Serial.printf("AIRREPLAY START ok=1 src=%s records_total=%lu\r\n",
                shortLogName(source_name).c_str(),
                (unsigned long)replay.records_total);
  return true;
}

void serviceReplayCapture() {
  if (!g_replay_capture.active) return;

  const auto replay = replay_bridge::status();
  if (!g_replay_capture.stop_requested && !replay_bridge::active()) {
    g_replay_capture.stop_requested = true;
    log_store::stopSession();
    Serial.printf("AIRREPLAYCAP STOP requested=1 sent=%lu total=%lu last_error=%lu\r\n",
                  (unsigned long)replay.records_sent,
                  (unsigned long)replay.records_total,
                  (unsigned long)replay.last_error);
    return;
  }

  if (!g_replay_capture.stop_requested || log_store::busy()) return;

  const auto recorder = log_store::recorderStatus();
  const auto stats = log_store::stats();
  const uint32_t elapsed_ms = millis() - g_replay_capture.started_ms;
  String closed_name;
  (void)log_store::latestLogNameForSession(g_replay_capture.session_id, closed_name);
  Serial.printf(
      "AIRREPLAYCAP RESULT ok=%u elapsed_ms=%lu dst=%s session=%lu sent=%lu total=%lu bytes=%lu written=%lu dropped=%lu\r\n",
      (replay.last_error == 0U) ? 1U : 0U,
      (unsigned long)elapsed_ms,
      shortLogName(closed_name).c_str(),
      (unsigned long)g_replay_capture.session_id,
      (unsigned long)replay.records_sent,
      (unsigned long)replay.records_total,
      (unsigned long)recorder.bytes_written,
      (unsigned long)stats.records_written,
      (unsigned long)stats.dropped);
  g_replay_capture = ReplayCaptureRun{};
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
      } else if (strcmp(g_console_line, "sdprobe") == 0 || strcmp(g_console_line, "sdcard") == 0) {
        sd_card_test::Status status = {};
        (void)sd_card_test::probe(status);
        sd_card_test::printProbeReport(Serial, status);
      } else if (strcmp(g_console_line, "sdwrite") == 0 || strcmp(g_console_line, "sdtest") == 0) {
        sd_card_test::Status status = {};
        (void)sd_card_test::writeProbe(status);
        sd_card_test::printProbeReport(Serial, status);
      } else if (strcmp(g_console_line, "base1m") == 0) {
        beginBaselineBenchmark(60000U);
      } else if (strcmp(g_console_line, "basestop") == 0) {
        stopBaselineBenchmark();
        printBaselineImpactReport();
        g_baseline_completed = false;
      } else if (strcmp(g_console_line, "basestat") == 0) {
        Serial.printf("BASELINE active=%u completed=%u duration_ms=%lu started_ms=%lu stopped_ms=%lu\r\n",
                      g_baseline_active ? 1U : 0U,
                      g_baseline_completed ? 1U : 0U,
                      (unsigned long)g_baseline_duration_ms,
                      (unsigned long)g_baseline_started_ms,
                      (unsigned long)g_baseline_stopped_ms);
      } else if (strcmp(g_console_line, "sdcap1m") == 0) {
        beginSdCaptureBenchmark(60000U);
      } else if (strcmp(g_console_line, "sdcapstop") == 0) {
        sd_capture_test::stop(false);
        printSdCaptureImpactReport();
        sd_capture_test::clearCompleted();
      } else if (strcmp(g_console_line, "sdcapstat") == 0) {
        const auto cap = sd_capture_test::stats();
        sd_capture_test::printReport(Serial, cap);
      } else if (strcmp(g_console_line, "logstart") == 0) {
        const uint32_t session_id = nextConsoleLogSessionId();
        radio_link::setRecorderEnabled(true);
        const bool ok = log_store::startSession(session_id);
        Serial.printf("AIRLOG START ok=%u session=%lu\r\n",
                      ok ? 1U : 0U,
                      (unsigned long)session_id);
        printAirLogStatus();
      } else if (strncmp(g_console_line, "logstartid ", 11) == 0) {
        unsigned session_id = 0U;
        if (sscanf(g_console_line + 11, "%u", &session_id) == 1 && session_id != 0U) {
          g_console_log_session_id = (uint32_t)session_id;
          radio_link::setRecorderEnabled(true);
          const bool ok = log_store::startSession((uint32_t)session_id);
          Serial.printf("AIRLOG START ok=%u session=%u\r\n", ok ? 1U : 0U, session_id);
          printAirLogStatus();
        } else {
          Serial.println("AIRLOG usage: logstartid <session>");
        }
      } else if (strcmp(g_console_line, "logstop") == 0) {
        log_store::stopSession();
        Serial.println("AIRLOG STOP requested=1");
        printAirLogStatus();
      } else if (strcmp(g_console_line, "logstat") == 0) {
        printAirLogStatus();
      } else if (strcmp(g_console_line, "latestlog") == 0) {
        String latest_name;
        const bool ok = log_store::latestLogName(latest_name);
        Serial.printf("AIRLOG latest_ok=%u file=%s\r\n",
                      ok ? 1U : 0U,
                      ok ? shortLogName(latest_name).c_str() : "(none)");
      } else if (strcmp(g_console_line, "largestlog") == 0) {
        String largest_name;
        const bool ok = log_store::largestLogName(largest_name);
        Serial.printf("AIRLOG largest_ok=%u file=%s\r\n",
                      ok ? 1U : 0U,
                      ok ? shortLogName(largest_name).c_str() : "(none)");
      } else if (strncmp(g_console_line, "latestlogsession ", 17) == 0) {
        unsigned session_id = 0U;
        if (sscanf(g_console_line + 17, "%u", &session_id) == 1 && session_id != 0U) {
          String latest_name;
          const bool ok = log_store::latestLogNameForSession((uint32_t)session_id, latest_name);
          Serial.printf("AIRLOG latest_session_ok=%u session=%u file=%s\r\n",
                        ok ? 1U : 0U,
                        session_id,
                        ok ? shortLogName(latest_name).c_str() : "(none)");
        } else {
          Serial.println("AIRLOG usage: latestlogsession <session>");
        }
      } else if (strcmp(g_console_line, "verifylog") == 0 || strcmp(g_console_line, "copylog") == 0) {
        const bool ok = log_store::copyLatestLogAndVerify(Serial);
        Serial.printf("AIRVERIFY RESULT ok=%u\r\n", ok ? 1U : 0U);
      } else if (strcmp(g_console_line, "expandlogs") == 0 || strcmp(g_console_line, "expandcsv") == 0) {
        const bool ok = log_store::exportAllLogsToCsv(Serial);
        Serial.printf("AIRCSV RESULT ok=%u\r\n", ok ? 1U : 0U);
      } else if (strncmp(g_console_line, "comparelogs ", 12) == 0) {
        char src_name[48] = {};
        char dst_name[48] = {};
        if (sscanf(g_console_line + 12, "%47s %47s", src_name, dst_name) == 2) {
          const bool ok = log_store::compareLogs(Serial, String(src_name), String(dst_name));
          Serial.printf("AIRCOMPARE RESULT ok=%u\r\n", ok ? 1U : 0U);
        } else {
          Serial.println("AIRCOMPARE usage: comparelogs <src.tlog> <dst.tlog>");
        }
      } else if (strcmp(g_console_line, "replaycapture") == 0 || strcmp(g_console_line, "replaycap") == 0) {
        (void)beginReplayCapture(nullptr);
      } else if (strncmp(g_console_line, "replaycapfile ", 14) == 0) {
        String source_name = String(g_console_line + 14);
        source_name.trim();
        if (source_name.length() == 0) {
          Serial.println("AIRREPLAYCAP usage: replaycapfile <file.tlog>");
        } else {
          (void)beginReplayCapture(&source_name);
        }
      } else if (strncmp(g_console_line, "replayfile ", 11) == 0) {
        String source_name = String(g_console_line + 11);
        source_name.trim();
        if (source_name.length() == 0) {
          Serial.println("AIRREPLAY usage: replayfile <file.tlog>");
        } else {
          (void)beginReplayDirect(source_name);
        }
      } else if (strcmp(g_console_line, "replaylargest") == 0) {
        String largest_name;
        if (!log_store::largestLogName(largest_name)) {
          Serial.println("AIRREPLAY START ok=0 reason=no_source_log");
        } else {
          (void)beginReplayDirect(largest_name);
        }
      } else if (strcmp(g_console_line, "replaycapstat") == 0) {
        const auto replay = replay_bridge::status();
        Serial.printf("AIRREPLAYCAP active=%u stop_requested=%u session=%lu sent=%lu total=%lu last_error=%lu\r\n",
                      g_replay_capture.active ? 1U : 0U,
                      g_replay_capture.stop_requested ? 1U : 0U,
                      (unsigned long)g_replay_capture.session_id,
                      (unsigned long)replay.records_sent,
                      (unsigned long)replay.records_total,
                      (unsigned long)replay.last_error);
      } else if (strncmp(g_console_line, "setpin ", 7) == 0) {
        unsigned pin = 0U;
        if (sscanf(g_console_line + 7, "%u", &pin) == 1 && pin <= 48U) {
          startGpioPulse((uint8_t)pin);
        } else {
          Serial.println("SETPIN usage: setpin <gpio>");
        }
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
                      (unsigned)cfg.log_rate_hz);
      } else if (strcmp(g_console_line, "tx1") == 0 || strcmp(g_console_line, "sendstate") == 0) {
        const auto snap = uart_telem::snapshot();
        if (!snap.has_state) {
          Serial.println("TX1 tx_ok=0 reason=no_state");
        } else {
          const bool ok = radio_link::publishStressState(snap.state, snap.seq, snap.t_us);
          Serial.printf("TX1 tx_ok=%u seq=%lu t_us=%lu\n",
                        ok ? 1U : 0U,
                        (unsigned long)snap.seq,
                        (unsigned long)snap.t_us);
        }
      } else if (strcmp(g_console_line, "linkclear") == 0) {
        radio_link::resetNetworkState();
        resetWifiStatusFlags();
        Serial.println("LINKCLEAR done state=cleared link=stopped");
      } else if (strcmp(g_console_line, "linkopen") == 0) {
        const AppConfig& cfg = config_store::get();
        radio_link::reconfigure(cfg);
        Serial.printf("LINKOPEN peer=%s channel=%u\n",
                      radio_link::peerMac().c_str(),
                      (unsigned)telem::kRadioChannel);
      } else if (strcmp(g_console_line, "wifidrop") == 0) {
        const AppConfig& cfg = config_store::get();
        radio_link::resetNetworkState();
        resetWifiStatusFlags();
        radio_link::reconfigure(cfg);
        Serial.println("WIFIDROP peer_state_cleared");
      } else if (strcmp(g_console_line, "wifioffon") == 0) {
        restartWifiStation();
        Serial.println("WIFIOFFON radio_power_cycle");
      } else if (strcmp(g_console_line, "relink") == 0) {
        const AppConfig& cfg = config_store::get();
        radio_link::resetNetworkState();
        radio_link::reconfigure(cfg);
        resetWifiStatusFlags();
        Serial.printf("RELINK peer=%s channel=%u\n",
                      radio_link::peerMac().c_str(),
                      (unsigned)telem::kRadioChannel);
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

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  (void)esp_wifi_set_ps(WIFI_PS_NONE);
  (void)esp_wifi_set_max_tx_power(78);
  (void)esp_wifi_set_channel(telem::kRadioChannel, WIFI_SECOND_CHAN_NONE);

  Serial.printf("AIR RADIO channel=%u ap=%s lr=%u\n",
                (unsigned)telem::kRadioChannel,
                cfg.ap_ssid,
                (unsigned)(cfg.radio_lr_mode != 0U));
}

void restartWifiStation() {
  const AppConfig& cfg = config_store::get();
  Serial.println("AIR CMD reset_network");
  radio_link::resetNetworkState();
  resetWifiStatusFlags();
  WiFi.mode(WIFI_OFF);
  delay(50);
  beginWifiStation();
  g_wait_stream_rate_ack = true;
  g_last_stream_rate_tx_ms = 0;
  g_last_stream_rate_ui_hz = 0;
  g_last_stream_rate_log_hz = 0;
  resetRadioWatchdog();
  radio_link::reconfigure(cfg);
}

void updateWifiReadiness() {
  if (!radio_link::radioReady()) {
    if (g_radio_ready) {
      Serial.println("AIR WAIT radio");
      g_radio_ready = false;
    }
    g_link_ready = false;
    g_link_wait_printed = false;
    return;
  }

  if (!g_radio_ready) {
    Serial.printf("AIR READY radio channel=%u\n", (unsigned)telem::kRadioChannel);
    g_radio_ready = true;
  }

  if (radio_link::hasPeer()) {
    if (!g_link_ready) {
      Serial.printf("AIR READY gnd_link peer=%s\n", radio_link::peerMac().c_str());
      g_link_ready = true;
      g_link_wait_printed = false;
    }
    return;
  }

  if (g_link_ready) {
    Serial.println("AIR WAIT gnd_link peer=discovery");
    g_link_ready = false;
    g_link_wait_printed = true;
    return;
  }

  if (!g_link_wait_printed) {
    Serial.println("AIR WAIT gnd_link peer=discovery");
    g_link_wait_printed = true;
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
  if (!radio_link::stateOnlyMode()) {
    uart_telem::clearPendingStates();
    return;
  }
  if (!radio_link::hasPeer()) return;
  uart_telem::PendingState pending = {};
  while (radio_link::txQueueFree() > kStateTxReserveSlots && uart_telem::popPendingState(pending)) {
    if (!radio_link::publishState(pending.state, pending.seq, pending.t_us)) {
      break;
    }
  }
}

void maybeRecoverRadioLink(const uart_telem::Snapshot& snap) {
  const auto link = radio_link::stats();
  const uint32_t now = millis();
  if (link.tx_packets != g_last_radio_tx_packets || link.rx_packets != g_last_radio_rx_packets) {
    g_last_radio_tx_packets = link.tx_packets;
    g_last_radio_rx_packets = link.rx_packets;
    g_last_radio_progress_ms = now;
  }
  if (snap.has_state && snap.seq != g_last_source_seq_seen) {
    g_last_source_seq_seen = snap.seq;
    g_last_source_progress_ms = now;
  }

  const bool source_active =
      g_last_source_progress_ms != 0U && (uint32_t)(now - g_last_source_progress_ms) <= 2000U;
  if (!source_active) return;
  if ((uint32_t)(now - g_last_radio_progress_ms) < kRadioProgressTimeoutMs) return;
  if ((uint32_t)(now - g_last_radio_recovery_ms) < kRadioRecoveryCooldownMs) return;

  Serial.printf("AIR WARN radio_watchdog restart idle_ms=%lu tx=%lu rx=%lu peer=%s\n",
                (unsigned long)(now - g_last_radio_progress_ms),
                (unsigned long)link.tx_packets,
                (unsigned long)link.rx_packets,
                radio_link::peerMac().c_str());
  g_last_radio_recovery_ms = now;
  restartWifiStation();
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

  const bool air_file_logging_enabled = kEnableAirFileLogging;

  uart_telem::begin(cfg);
  radio_link::begin(cfg);
  replay_bridge::begin();
  sd_capture_test::begin();
  log_store::begin(cfg, air_file_logging_enabled);
  radio_link::setRecorderEnabled(air_file_logging_enabled);
  Serial.printf("AIR INFO recorder=%s\n", air_file_logging_enabled ? "on" : "off");
  g_last_stream_rate_ui_hz = 0;
  g_last_stream_rate_log_hz = 0;
  g_last_stream_rate_tx_ms = 0;
  g_wait_stream_rate_ack = true;
  resetRadioWatchdog();
  printConsoleHelp();
}

void loop() {
  handleConsoleCommands();
  serviceGpioPulse();
  sd_capture_test::poll();
  log_store::poll();
  serviceReplayCapture();
  if (g_baseline_active &&
      (uint32_t)(millis() - g_baseline_started_ms) >= g_baseline_duration_ms) {
    stopBaselineBenchmark();
  }
  if (radio_link::takeNetworkResetRequest()) {
    restartWifiStation();
  }
  uart_telem::poll();
  radio_link::poll();
  replay_bridge::poll();
  updateWifiReadiness();

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
  radio_link::publish(snap);
  maybeRecoverRadioLink(snap);

  const auto cap = sd_capture_test::stats();
  if (g_sd_capture_was_active && !cap.active && cap.completed) {
    printSdCaptureImpactReport();
    sd_capture_test::clearCompleted();
  }
  g_sd_capture_was_active = cap.active;
  if (!g_baseline_active && g_baseline_completed) {
    printBaselineImpactReport();
    g_baseline_completed = false;
  }

  const uint32_t now = millis();
  if (g_stats_streaming && (uint32_t)(now - g_last_stat_ms) >= 1000U) {
    g_last_stat_ms = now;
    printStats(snap);
  }
}




