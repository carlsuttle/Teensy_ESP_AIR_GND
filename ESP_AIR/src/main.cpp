#include <Arduino.h>
#include <WiFi.h>
#include <ctype.h>
#include <esp_wifi.h>
#include <string.h>

#include "config_store.h"
#include "log_store.h"
#include "sd_api.h"
#include "sd_backend.h"
#include "sd_capture_test.h"
#include "teensy_api.h"
#include "teensy_link.h"
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
uint32_t g_last_capture_rate_tx_ms = 0;
uint16_t g_last_capture_rate_hz = 0;
bool g_quiet_serial = false;
bool g_radio_ready = false;
bool g_link_ready = false;
bool g_link_wait_printed = false;
bool g_teensy_ready = false;
bool g_teensy_wait_printed = false;
bool g_gpio_pulse_active = false;
uint8_t g_gpio_pulse_pin = 0U;
uint32_t g_gpio_pulse_until_ms = 0U;
bool g_sd_capture_was_active = false;
bool g_sd_soak_active = false;
uint16_t g_sd_soak_rate_hz = 0U;
uint32_t g_sd_soak_seq = 0U;
uint32_t g_sd_soak_period_us = 0U;
uint32_t g_sd_soak_next_us = 0U;
bool g_baseline_active = false;
bool g_baseline_completed = false;
uint32_t g_baseline_duration_ms = 0U;
uint32_t g_baseline_started_ms = 0U;
uint32_t g_baseline_stopped_ms = 0U;
uint32_t g_console_log_session_id = 0U;
struct SdReadWriteSoakRun {
  bool active = false;
  bool ok = false;
  uint32_t started_ms = 0U;
  uint32_t duration_ms = 0U;
  uint32_t bytes_read = 0U;
  uint32_t bytes_written = 0U;
  uint32_t wraps = 0U;
  uint32_t read_failures = 0U;
  uint32_t write_failures = 0U;
  uint32_t max_read_us = 0U;
  uint32_t max_write_us = 0U;
  uint32_t iterations = 0U;
  String source_name;
  String dest_name;
  sd_api::File source_file;
  sd_api::File dest_file;
} g_sd_rw_soak = {};
struct SdReadWriteLogSoakRun {
  bool active = false;
  bool stop_requested = false;
  bool ok = false;
  uint32_t started_ms = 0U;
  uint32_t duration_ms = 0U;
  uint32_t bytes_read = 0U;
  uint32_t records_read = 0U;
  uint32_t wraps = 0U;
  uint32_t read_failures = 0U;
  uint32_t unsupported_records = 0U;
  uint32_t session_id = 0U;
  String source_name;
  sd_api::File source_file;
} g_sd_rw_log_soak = {};
struct ReplayCaptureRun {
  bool active = false;
  bool stop_requested = false;
  uint32_t session_id = 0U;
  uint32_t started_ms = 0U;
} g_replay_capture = {};
struct ReplayCompareRun {
  bool active = false;
  uint8_t base_factor = 1U;
  uint8_t completed_runs = 0U;
  uint32_t last_completion_ms = 0U;
  uint32_t sessions[2] = {};
  bool ok[2] = {};
  uint32_t written[2] = {};
  uint32_t dropped[2] = {};
  uint16_t capture_rate_hz[2] = {};
  String source_name;
  String outputs[2];
} g_replay_compare = {};
enum class BenchSweepPhase : uint8_t {
  Idle = 0,
  WaitApply,
  Logging,
  WaitLogStop,
};
struct BenchSweepRun {
  bool active = false;
  uint32_t duration_ms = 0U;
  uint8_t rate_index = 0U;
  uint16_t current_rate_hz = 0U;
  uint32_t session_id = 0U;
  uint32_t phase_started_ms = 0U;
  BenchSweepPhase phase = BenchSweepPhase::Idle;
} g_bench_sweep = {};
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
constexpr bool kEnableRadioWatchdogRestart = false;
constexpr uint16_t kBenchCaptureRatesHz[] = {50U, 100U, 200U, 400U, 800U, 1600U};
constexpr uint16_t kReplaySourceProfilesHz[] = {25U, 50U, 100U, 200U, 400U, 800U, 1600U};
constexpr uint32_t kBenchApplySettleMs = 1000U;
constexpr uint32_t kReplayCompareSettleMs = 1000U;
constexpr size_t kSdRwSoakChunkBytes = 65536U;

#pragma pack(push, 1)
struct SoakBinaryLogRecordV2 {
  uint32_t magic;
  uint16_t version;
  uint16_t record_size;
  uint16_t record_kind;
  uint16_t reserved;
  uint32_t seq;
  uint32_t t_us;
  uint8_t payload[telem::kReplayRecordBytes];
};
#pragma pack(pop)

constexpr uint32_t kSoakLogMagic = 0x4C4F4731UL;
constexpr uint16_t kSoakLogVersion = 2U;

struct CaptureBenchBaseline {
  radio_link::Stats radio = {};
  teensy_link::RxStats teensy = {};
  uint32_t started_ms = 0U;
};

CaptureBenchBaseline g_sd_capture_baseline = {};

void beginWifiStation();
void restartWifiStation();
void beginSdCaptureBenchmark(uint32_t duration_ms);
void printSdCaptureImpactReport();
bool beginSdSoakBenchmark(uint32_t duration_ms, uint16_t rate_hz);
void stopSdSoakBenchmark();
void serviceSdSoakBenchmark();
void beginBaselineBenchmark(uint32_t duration_ms);
void stopBaselineBenchmark();
void printBaselineImpactReport();
bool beginSdReadWriteSoak(uint32_t duration_ms, const String* source_override = nullptr);
void serviceSdReadWriteSoak();
void stopSdReadWriteSoak(bool ok, const char* reason = nullptr);
bool beginSdReadWriteLogSoak(uint32_t duration_ms, const String* source_override = nullptr);
void serviceSdReadWriteLogSoak();
void stopSdReadWriteLogSoak(bool ok, const char* reason = nullptr);
bool handleTeensyApiConsoleCommand(const char* line);
void printAirLogStatus();
bool beginReplayCapture(const String* source_override = nullptr);
void serviceReplayCapture();
bool beginReplayCompare(uint8_t base_factor, const String* source_override = nullptr);
void serviceReplayCompare();
bool isStandaloneBench();
bool sendConfiguredCaptureRateNow();
void ensureConfiguredCaptureRate();
bool setCaptureRateHz(uint16_t hz, bool persist_config);
bool saveCaptureRateDefaults();
bool beginBenchSweep(uint32_t duration_ms);
void serviceBenchSweep();
void printSdApiStatus(const char* tag = "SDSTATE");
bool waitForLogStoreIdle(uint32_t timeout_ms);
bool writeSdApiTestLog();
uint16_t replayCaptureRateForFactor(uint8_t average_factor);
bool getActiveFusionSettings(telem::CmdSetFusionSettingsV1& cmd);
bool applyReplayRunControls(uint8_t average_factor, uint16_t* applied_capture_hz = nullptr);

bool serialNoiseEnabled() {
  return !g_quiet_serial;
}

String shortLogName(const String& path) {
  return path.startsWith("/logs/") ? path.substring(6) : path;
}

String nextSdRwSoakName() {
  return String("/logs/rwsoak_") + String(millis()) + ".bin";
}

bool readExactFile(sd_api::File& file, uint8_t* dst, size_t wanted) {
  size_t total = 0U;
  while (total < wanted) {
    const size_t got = file.read(dst + total, wanted - total);
    if (got == 0U) break;
    total += got;
  }
  return total == wanted;
}

uint32_t nextConsoleLogSessionId() {
  g_console_log_session_id++;
  if (g_console_log_session_id == 0U) g_console_log_session_id = 1U;
  return g_console_log_session_id;
}

bool isStandaloneBench() {
  return config_store::get().standalone_bench != 0U;
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
  if (isStandaloneBench()) return;
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
  if (!teensy_link::sendSetStreamRate(cmd)) return;

  g_last_stream_rate_ui_hz = target_stream_hz;
  g_last_stream_rate_log_hz = target_log_hz;
  g_last_stream_rate_tx_ms = now;
  g_wait_stream_rate_ack = true;
}

void ensureConfiguredCaptureRate() {
  if (!isStandaloneBench()) return;
  if (log_store::active()) return;
  const AppConfig& cfg = config_store::get();
  const uint16_t target_hz = cfg.source_rate_hz;
  const bool targetChanged = target_hz != g_last_capture_rate_hz;
  const uint32_t now = millis();
  if (!targetChanged && (uint32_t)(now - g_last_capture_rate_tx_ms) < 1000U) return;

  telem::CmdSetCaptureSettingsV1 cmd = {};
  cmd.source_rate_hz = target_hz;
  if (!teensy_link::sendSetCaptureSettings(cmd)) return;

  g_last_capture_rate_hz = target_hz;
  g_last_capture_rate_tx_ms = now;
}

bool sendConfiguredStreamRateNow() {
  if (isStandaloneBench()) return sendConfiguredCaptureRateNow();
  const AppConfig& cfg = config_store::get();
  telem::CmdSetStreamRateV1 cmd = {};
  cmd.ws_rate_hz = cfg.source_rate_hz;
  cmd.log_rate_hz = cfg.log_rate_hz;
  const bool ok = teensy_link::sendSetStreamRate(cmd);
  if (ok) {
    g_last_stream_rate_ui_hz = cmd.ws_rate_hz;
    g_last_stream_rate_log_hz = cmd.log_rate_hz;
    g_last_stream_rate_tx_ms = millis();
    g_wait_stream_rate_ack = true;
  }
  return ok;
}

bool sendConfiguredCaptureRateNow() {
  const AppConfig& cfg = config_store::get();
  telem::CmdSetCaptureSettingsV1 cmd = {};
  cmd.source_rate_hz = cfg.source_rate_hz;
  const bool ok = teensy_link::sendSetCaptureSettings(cmd);
  if (ok) {
    g_last_capture_rate_hz = cmd.source_rate_hz;
    g_last_capture_rate_tx_ms = millis();
  }
  return ok;
}

bool setCaptureRateHz(uint16_t hz, bool persist_config) {
  AppConfig cfg = config_store::get();
  cfg.source_rate_hz = hz;
  cfg.log_rate_hz = hz;
  if (persist_config) {
    config_store::update(cfg);
  }
  bool ok = teensy_link::sendSetCaptureSettings(telem::CmdSetCaptureSettingsV1{hz, 0U});
  if (ok) {
    g_last_capture_rate_hz = hz;
    g_last_capture_rate_tx_ms = millis();
  }
  if (ok && !isStandaloneBench()) {
    ok = sendConfiguredStreamRateNow();
  }
  return ok;
}

bool saveCaptureRateDefaults() {
  teensy_api::CommandAckResult result = {};
  return teensy_api::saveCaptureSettings(result);
}

uint16_t replayCaptureRateForFactor(uint8_t average_factor) {
  const uint8_t divisor = average_factor ? average_factor : 1U;
  // Replay ladder tests are intended to exercise the Teensy's tested source-rate
  // profiles, not whatever live AIR config happened to be active before replay.
  const uint32_t target_hz = 1600U / (uint32_t)divisor;
  uint16_t selected_hz = kReplaySourceProfilesHz[0];
  for (uint16_t hz : kReplaySourceProfilesHz) {
    if ((uint32_t)hz > target_hz) break;
    selected_hz = hz;
  }
  return selected_hz;
}

bool getActiveFusionSettings(telem::CmdSetFusionSettingsV1& cmd) {
  const auto snap = teensy_link::snapshot();
  if (snap.has_fusion_settings) {
    cmd.gain = snap.fusion_settings.gain;
    cmd.accelerationRejection = snap.fusion_settings.accelerationRejection;
    cmd.magneticRejection = snap.fusion_settings.magneticRejection;
    cmd.recoveryTriggerPeriod = snap.fusion_settings.recoveryTriggerPeriod;
    cmd.reserved = 0U;
    return true;
  }
  if (snap.has_state) {
    cmd.gain = snap.state.fusion_gain;
    cmd.accelerationRejection = snap.state.fusion_accel_rej;
    cmd.magneticRejection = snap.state.fusion_mag_rej;
    cmd.recoveryTriggerPeriod = snap.state.fusion_recovery_period;
    cmd.reserved = 0U;
    return true;
  }
  cmd = {};
  return false;
}

bool applyReplayRunControls(uint8_t average_factor, uint16_t* applied_capture_hz) {
  const uint16_t capture_hz = replayCaptureRateForFactor(average_factor);
  const bool cap_ok = teensy_link::sendSetCaptureSettings(telem::CmdSetCaptureSettingsV1{capture_hz, 0U});
  if (cap_ok) {
    g_last_capture_rate_hz = capture_hz;
    g_last_capture_rate_tx_ms = millis();
  }

  telem::CmdSetFusionSettingsV1 fusion = {};
  bool fusion_ok = true;
  if (getActiveFusionSettings(fusion)) {
    fusion_ok = teensy_link::sendSetFusionSettings(fusion);
  }

  if (applied_capture_hz) *applied_capture_hz = capture_hz;
  return cap_ok && fusion_ok;
}

void printConsoleHelp() {
  Serial.println("AIR COMMANDS:");
  Serial.println("  help / h      - show command list");
  Serial.println("  getfusion     - send CMD_GET_FUSION_SETTINGS to Teensy");
  Serial.println("  kickteensy    - resend current stream-rate command to Teensy");
  Serial.println("  resendrate    - same as kickteensy");
  Serial.println("  sdprobe       - probe SD API/backend and print current state");
  Serial.println("  sdmount       - mount the SD card backend");
  Serial.println("  sdeject       - eject the SD card backend when idle");
  Serial.println("  sdstate       - print SD mount and backend state");
  Serial.println("  sdwrite       - write one small managed test log through the SD API");
  Serial.println("  sdrename a b  - rename one managed file through the SD API");
  Serial.println("  sddelete <f>  - delete one managed file through the SD API");
  Serial.println("  base1m        - run 60-second radio/Teensy-link baseline with no SD capture");
  Serial.println("  basestop      - stop active baseline run");
  Serial.println("  basestat      - print baseline status");
  Serial.println("  logstart      - start real AIR SD logging session");
  Serial.println("  logstartid <n> - start real AIR SD logging with an explicit session id");
  Serial.println("  logstop       - stop real AIR SD logging session");
  Serial.println("  logstat       - print real AIR SD logging status");
  Serial.println("  latestlog     - print latest .tlog on SD");
  Serial.println("  largestlog    - print largest .tlog on SD");
  Serial.println("  latestlogsession <n> - print latest .tlog for a given session id");
  Serial.println("  logfiles [name|size|date] [asc|desc] - print sorted managed file list");
  Serial.println("  logprefix [prefix] - show or set the auto-record file prefix");
  Serial.println("  csvfile <name.tlog> - convert a single binary log to CSV");
  Serial.println("  verifylog     - copy latest .tlog to *_copy.tlog and verify byte-exact match");
  Serial.println("  expandlogs    - expand every .tlog on SD into a sibling .csv file");
  Serial.println("  comparelogs a b - compare two .tlog files, ignoring fusion outputs");
  Serial.println("  comparetimed a b [skip] - compare two .tlog files by nearest timestamp");
  Serial.println("  logkinds <name> - count state/control/input record kinds in a .tlog");
  Serial.println("  logfusion <name> - print fusion settings seen in a replay/log file");
  Serial.println("  logflags <name> - print fusion flag/error counts seen in a replay/log file");
  Serial.println("  sdrwsoak <seconds> [file] - mixed sequential SD read/write soak with both files held open");
  Serial.println("  sdrwsoakstat - print current mixed read/write soak state");
  Serial.println("  sdrwsoakstop - stop active mixed read/write soak");
  Serial.println("  sdrwlogsoak <seconds> [file] - mixed SD read with production log_store write path");
  Serial.println("  sdrwlogsoakstat - print current mixed SD read/log write soak state");
  Serial.println("  sdrwlogsoakstop - stop active mixed SD read/log write soak");
  Serial.println("  carrysig [count] - send synthetic replay records and verify exact carry-through on AIR");
  Serial.println("  carrysigcsv [ms] [window] - burst synthetic replay records and print CSV rows by returned seq");
  Serial.println("  tapi ...      - stable Teensy API proof surface (help/status/getfusion/setcap/setstream/setfusion/carry/carrycsv/replaybench/selftest)");
  Serial.println("  replaycapture - replay latest .tlog into Teensy while logging returned state");
  Serial.println("  replaycapfile <name> - replay a specific .tlog into Teensy while logging returned state");
  Serial.println("  replayfile <name> - replay a specific .tlog into Teensy without rerecording");
  Serial.println("  replaylargest - replay the largest .tlog into Teensy without rerecording");
  Serial.println("  replaycapstat - print replay-capture progress");
  Serial.println("  replayavg <n> - select one replay sample out of each N-source-record window");
  Serial.println("  replayavgstat - print current replay averaging factor");
  Serial.println("  replaycmp <n> - replay latest .tlog twice, with N then N+1 averaging");
  Serial.println("  replaycmpfile <n> <name> - same compare run for a specific .tlog");
  Serial.println("  setcap <hz>   - set Teensy live capture/source rate over SPI/DMA");
  Serial.println("  savecap       - save current Teensy capture settings to EEPROM");
  Serial.println("  bench on/off/status - control standalone SPI/DMA bench mode");
  Serial.println("  benchauto [ms] - log all supported capture rates automatically");
  Serial.println("  setpin <gpio> - drive a GPIO high for 5 seconds, then return it low");
  Serial.println("  tx1           - send current state once to GND");
  Serial.println("  linkclear     - clear AIR radio-link state and stop ESP-NOW");
  Serial.println("  linkopen      - reopen AIR radio-link only");
  Serial.println("  wifidrop      - clear discovered peer state");
  Serial.println("  wifioffon     - power-cycle Wi-Fi only");
  Serial.println("  relink        - restart AIR radio-link");
  Serial.println("  resetnet      - restart AIR Wi-Fi/ESP-NOW side");
  Serial.println("  setfusion g a m r - send CMD_SET_FUSION_SETTINGS");
  Serial.println("  quiet on/off/status - stop or resume unsolicited serial chatter");
  Serial.println("  stats         - start 1Hz STAT stream");
  Serial.println("  x             - stop active stream/mode");
}

void printStats(const teensy_link::Snapshot& snap) {
  const auto link = radio_link::stats();
  const auto cap = sd_capture_test::stats();
  const auto spi = spi_bridge::stats();
  Serial.printf(
      "STAT unit=AIR seq=%lu t_us=%lu has=%u ack=%u cmd=%u ack_ok=%u code=%lu "
      "rx_bytes=%lu ok=%lu crc=%lu cobs=%lu len=%lu unk=%lu drop=%lu poll_ms=%lu poll_gap_max=%lu poll_runs=%lu state_drain=%lu raw_drain=%lu "
      "link_tx=%lu link_rx=%lu link_drop=%lu link_state_tx=%lu link_unified_tx=%lu src_seen=%lu src_seen_seq=%lu src_seen_t_us=%lu src_seen_rx_ms=%lu "
      "pub_try=%lu pub_ok=%lu pub_skip_ns=%lu pub_skip_np=%lu pub_skip_rate=%lu pub_skip_old=%lu link_last_seq=%lu link_last_t_us=%lu link_last_tx_ms=%lu pub_try_ms=%lu pub_age_ms=%lu "
      "spi_txn=%lu spi_fail=%lu spi_state=%lu spi_last_ms=%lu spi_replay=%lu spi_crc=%lu spi_type=%lu spi_rxof=%lu spi_txof=%lu spi_hdr=%08lX/%u/%u/%u sdcap=%u sdcap_drop=%lu sdcap_qmax=%lu\n",
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
      (unsigned long)snap.stats.last_poll_ms,
      (unsigned long)snap.stats.max_poll_gap_ms,
      (unsigned long)snap.stats.poll_runs,
      (unsigned long)snap.stats.state_records_drained,
      (unsigned long)snap.stats.raw_records_drained,
      (unsigned long)link.tx_packets,
      (unsigned long)link.rx_packets,
      (unsigned long)link.tx_drop,
      (unsigned long)link.tx_state_packets,
      (unsigned long)link.tx_unified_packets,
      (unsigned long)link.source_snapshots_seen,
      (unsigned long)link.latest_source_seq_seen,
      (unsigned long)link.latest_source_t_us_seen,
      (unsigned long)link.latest_source_rx_ms_seen,
      (unsigned long)link.publish_attempts,
      (unsigned long)link.publish_ok,
      (unsigned long)link.publish_skip_no_state,
      (unsigned long)link.publish_skip_no_peer,
      (unsigned long)link.publish_skip_rate,
      (unsigned long)link.publish_skip_not_new,
      (unsigned long)link.last_source_seq,
      (unsigned long)link.last_source_t_us,
      (unsigned long)link.last_tx_ms,
      (unsigned long)link.last_publish_attempt_ms,
      (unsigned long)link.last_publish_age_ms,
      (unsigned long)spi.transactions_completed,
      (unsigned long)spi.transaction_failures,
      (unsigned long)spi.state_records_received,
      (unsigned long)spi.last_state_rx_ms,
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
  stopSdSoakBenchmark();
  stopBaselineBenchmark();
  const auto snap = teensy_link::snapshot();
  g_sd_capture_baseline.radio = radio_link::stats();
  g_sd_capture_baseline.teensy = snap.stats;
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

bool handleTeensyApiConsoleCommand(const char* line) {
  if (strncmp(line, "tapi", 4) != 0) return false;

  const char* args = line + 4;
  while (*args == ' ') args++;

  if (*args == '\0' || strcmp(args, "help") == 0) {
    Serial.println("TAPI commands:");
    Serial.println("  tapi status");
    Serial.println("  tapi getfusion");
    Serial.println("  tapi setcap <hz>");
    Serial.println("  tapi setstream <ws_hz> [log_hz]");
    Serial.println("  tapi setfusion <gain> <accelRej> <magRej> <recovery>");
    Serial.println("  tapi carry [count]");
    Serial.println("  tapi carrycsv [ms] [window]");
    Serial.println("  tapi replaybench [ms] [batch_hz] [records_per_batch]");
    Serial.println("  tapi selftest [hz] [count]");
    return true;
  }

  if (strcmp(args, "status") == 0) {
    teensy_api::printStatus(Serial);
    return true;
  }

  if (strcmp(args, "getfusion") == 0) {
    teensy_api::CommandAckResult result = {};
    const bool ok = teensy_api::getFusionSettings(result);
    Serial.printf("TAPI GETFUSION tx_ok=%u ack_seen=%u ack_ok=%u code=%lu seq=%lu t_us=%lu gain=%.3f accRej=%.2f magRej=%.2f rec=%u\r\n",
                  result.tx_ok ? 1U : 0U,
                  result.ack_seen ? 1U : 0U,
                  result.ack_ok ? 1U : 0U,
                  (unsigned long)result.ack_code,
                  (unsigned long)result.snapshot.seq,
                  (unsigned long)result.snapshot.t_us,
                  result.snapshot.has_state ? (double)result.snapshot.state.fusion_gain : 0.0,
                  result.snapshot.has_state ? (double)result.snapshot.state.fusion_accel_rej : 0.0,
                  result.snapshot.has_state ? (double)result.snapshot.state.fusion_mag_rej : 0.0,
                  result.snapshot.has_state ? (unsigned)result.snapshot.state.fusion_recovery_period : 0U);
    Serial.printf("TAPI GETFUSION RESULT ok=%u\r\n", (ok && result.ack_ok) ? 1U : 0U);
    return true;
  }

  if (strncmp(args, "setcap ", 7) == 0) {
    unsigned hz = 0U;
    if (sscanf(args + 7, "%u", &hz) == 1 && hz > 0U) {
      teensy_api::CommandAckResult result = {};
      const bool ok = teensy_api::setCaptureSettings((uint16_t)hz, result);
      Serial.printf("TAPI SETCAP tx_ok=%u ack_seen=%u ack_ok=%u code=%lu hz=%u\r\n",
                    result.tx_ok ? 1U : 0U,
                    result.ack_seen ? 1U : 0U,
                    result.ack_ok ? 1U : 0U,
                    (unsigned long)result.ack_code,
                    hz);
      Serial.printf("TAPI SETCAP RESULT ok=%u\r\n", (ok && result.ack_ok) ? 1U : 0U);
    } else {
      Serial.println("TAPI SETCAP usage: tapi setcap <hz>");
    }
    return true;
  }

  if (strncmp(args, "setstream ", 10) == 0) {
    unsigned ws_hz = 0U;
    unsigned log_hz = 0U;
    const int parsed = sscanf(args + 10, "%u %u", &ws_hz, &log_hz);
    if (parsed >= 1 && ws_hz > 0U) {
      if (parsed < 2 || log_hz == 0U) log_hz = ws_hz;
      teensy_api::CommandAckResult result = {};
      const bool ok = teensy_api::setStreamRate((uint16_t)ws_hz, (uint16_t)log_hz, result);
      Serial.printf("TAPI SETSTREAM tx_ok=%u ack_seen=%u ack_ok=%u code=%lu ws_hz=%u log_hz=%u\r\n",
                    result.tx_ok ? 1U : 0U,
                    result.ack_seen ? 1U : 0U,
                    result.ack_ok ? 1U : 0U,
                    (unsigned long)result.ack_code,
                    ws_hz,
                    log_hz);
      Serial.printf("TAPI SETSTREAM RESULT ok=%u\r\n", (ok && result.ack_ok) ? 1U : 0U);
    } else {
      Serial.println("TAPI SETSTREAM usage: tapi setstream <ws_hz> [log_hz]");
    }
    return true;
  }

  if (strncmp(args, "setfusion ", 10) == 0) {
    float g = 0.0f, a = 0.0f, m = 0.0f;
    unsigned r = 0U;
    if (sscanf(args + 10, "%f %f %f %u", &g, &a, &m, &r) == 4) {
      telem::CmdSetFusionSettingsV1 cmd = {};
      cmd.gain = g;
      cmd.accelerationRejection = a;
      cmd.magneticRejection = m;
      cmd.recoveryTriggerPeriod = (uint16_t)r;
      teensy_api::CommandAckResult result = {};
      const bool ok = teensy_api::setFusionSettings(cmd, result);
      Serial.printf("TAPI SETFUSION tx_ok=%u ack_seen=%u ack_ok=%u code=%lu gain=%.3f accRej=%.2f magRej=%.2f rec=%u\r\n",
                    result.tx_ok ? 1U : 0U,
                    result.ack_seen ? 1U : 0U,
                    result.ack_ok ? 1U : 0U,
                    (unsigned long)result.ack_code,
                    (double)cmd.gain,
                    (double)cmd.accelerationRejection,
                    (double)cmd.magneticRejection,
                    (unsigned)cmd.recoveryTriggerPeriod);
      Serial.printf("TAPI SETFUSION RESULT ok=%u\r\n", (ok && result.ack_ok) ? 1U : 0U);
    } else {
      Serial.println("TAPI SETFUSION usage: tapi setfusion <gain> <accelRej> <magRej> <recovery>");
    }
    return true;
  }

  if (strncmp(args, "carrycsv", 8) == 0) {
    uint32_t duration_ms = 2000U;
    unsigned window = 16U;
    if (args[8] == '\0') {
      teensy_api::runCarrySequenceCsvTest(Serial, duration_ms, (uint8_t)window);
    } else if (args[8] == ' ') {
      const int matched = sscanf(args + 9, "%lu %u", &duration_ms, &window);
      if (matched >= 1 && duration_ms > 0U) {
        if (window == 0U) window = 16U;
        if (window > 255U) window = 255U;
        teensy_api::runCarrySequenceCsvTest(Serial, duration_ms, (uint8_t)window);
      } else {
        Serial.println("TAPI CARRYCSV usage: tapi carrycsv [ms] [window]");
      }
    } else {
      Serial.println("TAPI CARRYCSV usage: tapi carrycsv [ms] [window]");
    }
    return true;
  }

  if (strncmp(args, "carry", 5) == 0) {
    unsigned count = 8U;
    (void)sscanf(args + 5, "%u", &count);
    if (count == 0U) count = 8U;
    if (count > 255U) count = 255U;
    (void)teensy_api::runCarrySignatureTest(Serial, (uint8_t)count);
    return true;
  }

  if (strncmp(args, "replaybench", 11) == 0) {
    uint32_t duration_ms = 5000U;
    unsigned batch_hz = 100U;
    unsigned records_per_batch = 16U;
    if (args[11] == '\0') {
      (void)teensy_api::runReplayBatchBenchmark(Serial, duration_ms, (uint16_t)batch_hz, (uint16_t)records_per_batch);
    } else if (args[11] == ' ') {
      const int matched = sscanf(args + 12, "%lu %u %u", &duration_ms, &batch_hz, &records_per_batch);
      if (matched >= 1 && duration_ms > 0U && batch_hz > 0U && records_per_batch > 0U) {
        if (batch_hz > 1000U) batch_hz = 1000U;
        if (records_per_batch > 255U) records_per_batch = 255U;
        (void)teensy_api::runReplayBatchBenchmark(Serial, duration_ms, (uint16_t)batch_hz, (uint16_t)records_per_batch);
      } else {
        Serial.println("TAPI REPLAYBENCH usage: tapi replaybench [ms] [batch_hz] [records_per_batch]");
      }
    } else {
      Serial.println("TAPI REPLAYBENCH usage: tapi replaybench [ms] [batch_hz] [records_per_batch]");
    }
    return true;
  }

  if (strncmp(args, "selftest", 8) == 0) {
    unsigned hz = 1600U;
    unsigned count = 8U;
    (void)sscanf(args + 8, "%u %u", &hz, &count);
    if (count == 0U) count = 8U;
    teensy_api::CommandAckResult fusion = {};
    teensy_api::CommandAckResult cap = {};
    const bool get_ok = teensy_api::getFusionSettings(fusion);
    const bool cap_ok = teensy_api::setCaptureSettings((uint16_t)hz, cap);
    const auto carry = teensy_api::runCarrySignatureTest(Serial, (uint8_t)count);
    Serial.printf("TAPI SELFTEST RESULT ok=%u getfusion=%u setcap=%u carry_fail=%lu carry_timeout=%lu hz=%u count=%u\r\n",
                  (get_ok && fusion.ack_ok && cap_ok && cap.ack_ok && carry.fail == 0U && carry.timeout == 0U) ? 1U : 0U,
                  (get_ok && fusion.ack_ok) ? 1U : 0U,
                  (cap_ok && cap.ack_ok) ? 1U : 0U,
                  (unsigned long)carry.fail,
                  (unsigned long)carry.timeout,
                  hz,
                  count);
    return true;
  }

  Serial.print("TAPI unknown cmd: ");
  Serial.println(args);
  return true;
}

bool beginSdSoakBenchmark(uint32_t duration_ms, uint16_t rate_hz) {
  if (rate_hz == 0U) rate_hz = 400U;
  if (rate_hz > 5000U) rate_hz = 5000U;
  beginSdCaptureBenchmark(duration_ms);
  const auto cap = sd_capture_test::stats();
  if (!cap.active) return false;
  g_sd_soak_active = true;
  g_sd_soak_rate_hz = rate_hz;
  g_sd_soak_seq = 0U;
  g_sd_soak_period_us = 1000000UL / rate_hz;
  if (g_sd_soak_period_us == 0U) g_sd_soak_period_us = 1U;
  g_sd_soak_next_us = micros();
  Serial.printf("SDSOAK START ok=1 duration_ms=%lu rate_hz=%u period_us=%lu\r\n",
                (unsigned long)duration_ms,
                (unsigned)g_sd_soak_rate_hz,
                (unsigned long)g_sd_soak_period_us);
  return true;
}

void stopSdSoakBenchmark() {
  g_sd_soak_active = false;
  g_sd_soak_rate_hz = 0U;
  g_sd_soak_period_us = 0U;
}

void serviceSdSoakBenchmark() {
  if (!g_sd_soak_active) return;
  const auto cap = sd_capture_test::stats();
  if (!cap.active) {
    stopSdSoakBenchmark();
    return;
  }

  telem::TelemetryFullStateV1 state = {};
  const uint32_t now_us = micros();
  uint8_t generated = 0U;
  while ((int32_t)(now_us - g_sd_soak_next_us) >= 0 && generated < 32U) {
    const uint32_t sample_t_us = g_sd_soak_next_us;
    state.last_imu_ms = sample_t_us / 1000U;
    state.flags = telem::kStateFlagFusionInitialising;
    sd_capture_test::enqueueState(g_sd_soak_seq++, sample_t_us, state);
    g_sd_soak_next_us += g_sd_soak_period_us;
    generated++;
  }
  if (generated == 32U && (int32_t)(now_us - g_sd_soak_next_us) >= 0) {
    g_sd_soak_next_us = now_us + g_sd_soak_period_us;
  }
}

void beginBaselineBenchmark(uint32_t duration_ms) {
  sd_capture_test::stop(false);
  stopSdSoakBenchmark();
  g_sd_capture_was_active = false;
  const auto snap = teensy_link::snapshot();
  g_sd_capture_baseline.radio = radio_link::stats();
  g_sd_capture_baseline.teensy = snap.stats;
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
  const auto snap = teensy_link::snapshot();
  const uint32_t elapsed_ms =
      (g_baseline_stopped_ms >= g_sd_capture_baseline.started_ms)
          ? (g_baseline_stopped_ms - g_sd_capture_baseline.started_ms)
          : 0U;
  Serial.printf("BASELINE RESULT elapsed_ms=%lu duration_ms=%lu\r\n",
                (unsigned long)elapsed_ms,
                (unsigned long)g_baseline_duration_ms);
  Serial.printf(
      "BASELINE IMPACT radio_tx=%lu radio_rx=%lu radio_drop=%lu teensy_ok=%lu teensy_crc=%lu teensy_cobs=%lu teensy_len=%lu teensy_unk=%lu teensy_drop=%lu\r\n",
      (unsigned long)(radio_now.tx_packets - g_sd_capture_baseline.radio.tx_packets),
      (unsigned long)(radio_now.rx_packets - g_sd_capture_baseline.radio.rx_packets),
      (unsigned long)(radio_now.tx_drop - g_sd_capture_baseline.radio.tx_drop),
      (unsigned long)(snap.stats.frames_ok - g_sd_capture_baseline.teensy.frames_ok),
      (unsigned long)(snap.stats.crc_err - g_sd_capture_baseline.teensy.crc_err),
      (unsigned long)(snap.stats.cobs_err - g_sd_capture_baseline.teensy.cobs_err),
      (unsigned long)(snap.stats.len_err - g_sd_capture_baseline.teensy.len_err),
      (unsigned long)(snap.stats.unknown_msg - g_sd_capture_baseline.teensy.unknown_msg),
      (unsigned long)(snap.stats.drop - g_sd_capture_baseline.teensy.drop));
}

void printSdCaptureImpactReport() {
  const auto cap = sd_capture_test::stats();
  const auto radio_now = radio_link::stats();
  const auto snap = teensy_link::snapshot();
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
      "SDCAP IMPACT radio_tx=%lu radio_rx=%lu radio_drop=%lu teensy_ok=%lu teensy_crc=%lu teensy_cobs=%lu teensy_len=%lu teensy_unk=%lu teensy_drop=%lu\r\n",
      (unsigned long)(radio_now.tx_packets - g_sd_capture_baseline.radio.tx_packets),
      (unsigned long)(radio_now.rx_packets - g_sd_capture_baseline.radio.rx_packets),
      (unsigned long)(radio_now.tx_drop - g_sd_capture_baseline.radio.tx_drop),
      (unsigned long)(snap.stats.frames_ok - g_sd_capture_baseline.teensy.frames_ok),
      (unsigned long)(snap.stats.crc_err - g_sd_capture_baseline.teensy.crc_err),
      (unsigned long)(snap.stats.cobs_err - g_sd_capture_baseline.teensy.cobs_err),
      (unsigned long)(snap.stats.len_err - g_sd_capture_baseline.teensy.len_err),
      (unsigned long)(snap.stats.unknown_msg - g_sd_capture_baseline.teensy.unknown_msg),
      (unsigned long)(snap.stats.drop - g_sd_capture_baseline.teensy.drop));
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

void printSdApiStatus(const char* tag) {
  log_store::probeBackend();
  sd_backend::Status backend = {};
  const bool ready = sd_backend::refreshStatus(backend);
  const auto recorder = log_store::recorderStatus();
  Serial.printf("%s ready=%u mounted=%u media=%u state=%u init_hz=%lu total=%lu used=%lu free=%lu prefix=%s preview=%s\r\n",
                tag ? tag : "SDSTATE",
                ready ? 1U : 0U,
                sd_backend::mounted() ? 1U : 0U,
                sd_backend::mediaPresent() ? 1U : 0U,
                (unsigned)sd_backend::mediaState(),
                (unsigned long)backend.init_hz,
                (unsigned long)backend.total_bytes,
                (unsigned long)backend.used_bytes,
                (unsigned long)recorder.free_bytes,
                log_store::recordPrefix().c_str(),
                shortLogName(log_store::previewLogName()).c_str());
}

bool waitForLogStoreIdle(uint32_t timeout_ms) {
  const uint32_t started_ms = millis();
  while (log_store::busy()) {
    log_store::poll();
    teensy_link::poll();
    replay_bridge::poll();
    delay(2);
    if ((uint32_t)(millis() - started_ms) >= timeout_ms) {
      return false;
    }
  }
  return true;
}

bool writeSdApiTestLog() {
  if (replay_bridge::active()) {
    Serial.println("SDWRITE ok=0 reason=replay_busy");
    return false;
  }

  sd_backend::Status backend = {};
  if (!sd_backend::mounted() && !sd_backend::mount(&backend)) {
    printSdApiStatus("SDWRITE");
    Serial.println("SDWRITE ok=0 reason=mount_failed");
    return false;
  }

  log_store::probeBackend();
  if (!waitForLogStoreIdle(1000U)) {
    Serial.println("SDWRITE ok=0 reason=logger_busy");
    return false;
  }

  const uint32_t session_id = nextConsoleLogSessionId();
  if (!isStandaloneBench()) radio_link::setRecorderEnabled(true);
  if (!log_store::startSession(session_id)) {
    Serial.printf("SDWRITE ok=0 reason=start_failed session=%lu\r\n", (unsigned long)session_id);
    printAirLogStatus();
    return false;
  }

  telem::TelemetryFullStateV1 state = {};
  const auto snap = teensy_link::snapshot();
  if (snap.has_state) {
    state = snap.state;
  } else {
    state.roll_deg = 1.0f;
    state.pitch_deg = 2.0f;
    state.yaw_deg = 3.0f;
    state.mag_heading_deg = 4.0f;
    state.last_imu_ms = millis();
  }
  const uint32_t seq = snap.has_state && snap.seq != 0U ? snap.seq : 1U;
  log_store::enqueueState(seq, micros(), state);
  log_store::stopSession();

  if (!waitForLogStoreIdle(4000U)) {
    Serial.printf("SDWRITE ok=0 reason=close_timeout session=%lu\r\n", (unsigned long)session_id);
    printAirLogStatus();
    return false;
  }

  String latest_name;
  const bool found_name = log_store::latestLogNameForSession(session_id, latest_name);
  const auto recorder = log_store::recorderStatus();
  const auto stats = log_store::stats();
  Serial.printf("SDWRITE ok=%u session=%lu file=%s bytes=%lu records=%lu dropped=%lu\r\n",
                found_name ? 1U : 0U,
                (unsigned long)session_id,
                found_name ? shortLogName(latest_name).c_str() : "(unknown)",
                (unsigned long)recorder.bytes_written,
                (unsigned long)stats.records_written,
                (unsigned long)stats.dropped);
  printSdApiStatus("SDWRITE");
  return found_name;
}

bool beginSdReadWriteSoak(uint32_t duration_ms, const String* source_override) {
  if (g_sd_rw_soak.active) {
    Serial.println("SDRWSOAK START ok=0 reason=already_active");
    return false;
  }
  if (log_store::busy()) {
    Serial.println("SDRWSOAK START ok=0 reason=logger_busy");
    return false;
  }
  if (g_replay_capture.active || replay_bridge::active()) {
    Serial.println("SDRWSOAK START ok=0 reason=replay_busy");
    return false;
  }
  if (!sd_backend::mounted()) {
    sd_backend::Status backend = {};
    if (!sd_backend::begin(&backend)) {
      Serial.println("SDRWSOAK START ok=0 reason=sd_mount_failed");
      return false;
    }
  }

  String source_name;
  if (source_override && !source_override->isEmpty()) {
    source_name = *source_override;
  } else if (!log_store::largestLogName(source_name)) {
    Serial.println("SDRWSOAK START ok=0 reason=no_source_log");
    return false;
  }
  if (!source_name.startsWith("/")) source_name = String("/logs/") + source_name;

  sd_api::File source_file = sd_api::open(source_name, sd_api::OpenMode::read);
  if (!source_file) {
    Serial.printf("SDRWSOAK START ok=0 reason=open_source_failed src=%s\r\n",
                  shortLogName(source_name).c_str());
    return false;
  }

  const String dest_name = nextSdRwSoakName();
  sd_api::File dest_file = sd_api::open(dest_name, sd_api::OpenMode::write);
  if (!dest_file) {
    source_file.close();
    Serial.printf("SDRWSOAK START ok=0 reason=open_dest_failed dst=%s\r\n",
                  shortLogName(dest_name).c_str());
    return false;
  }

  g_sd_rw_soak = {};
  g_sd_rw_soak.active = true;
  g_sd_rw_soak.ok = true;
  g_sd_rw_soak.started_ms = millis();
  g_sd_rw_soak.duration_ms = duration_ms;
  g_sd_rw_soak.source_name = source_name;
  g_sd_rw_soak.dest_name = dest_name;
  g_sd_rw_soak.source_file = source_file;
  g_sd_rw_soak.dest_file = dest_file;
  Serial.printf("SDRWSOAK START ok=1 duration_ms=%lu src=%s dst=%s chunk=%u\r\n",
                (unsigned long)duration_ms,
                shortLogName(source_name).c_str(),
                shortLogName(dest_name).c_str(),
                (unsigned)kSdRwSoakChunkBytes);
  return true;
}

void stopSdReadWriteSoak(bool ok, const char* reason) {
  if (!g_sd_rw_soak.active) return;
  if (g_sd_rw_soak.dest_file) {
    g_sd_rw_soak.dest_file.flush();
    g_sd_rw_soak.dest_file.close();
  }
  if (g_sd_rw_soak.source_file) {
    g_sd_rw_soak.source_file.close();
  }
  const uint32_t elapsed_ms = millis() - g_sd_rw_soak.started_ms;
  Serial.printf(
      "SDRWSOAK RESULT ok=%u elapsed_ms=%lu src=%s dst=%s bytes_read=%lu bytes_written=%lu wraps=%lu read_fail=%lu write_fail=%lu max_read_us=%lu max_write_us=%lu iterations=%lu",
      ok ? 1U : 0U,
      (unsigned long)elapsed_ms,
      shortLogName(g_sd_rw_soak.source_name).c_str(),
      shortLogName(g_sd_rw_soak.dest_name).c_str(),
      (unsigned long)g_sd_rw_soak.bytes_read,
      (unsigned long)g_sd_rw_soak.bytes_written,
      (unsigned long)g_sd_rw_soak.wraps,
      (unsigned long)g_sd_rw_soak.read_failures,
      (unsigned long)g_sd_rw_soak.write_failures,
      (unsigned long)g_sd_rw_soak.max_read_us,
      (unsigned long)g_sd_rw_soak.max_write_us,
      (unsigned long)g_sd_rw_soak.iterations);
  if (reason && reason[0] != '\0') {
    Serial.printf(" reason=%s", reason);
  }
  Serial.print("\r\n");
  g_sd_rw_soak = {};
}

void serviceSdReadWriteSoak() {
  if (!g_sd_rw_soak.active) return;

  static uint8_t* s_buffer = nullptr;
  if (!s_buffer) {
    s_buffer = (uint8_t*)malloc(kSdRwSoakChunkBytes);
    if (!s_buffer) {
      g_sd_rw_soak.ok = false;
      stopSdReadWriteSoak(false, "alloc_failed");
      return;
    }
  }

  if ((uint32_t)(millis() - g_sd_rw_soak.started_ms) >= g_sd_rw_soak.duration_ms) {
    stopSdReadWriteSoak(g_sd_rw_soak.ok, nullptr);
    return;
  }

  uint32_t total_read_us = 0U;
  size_t got = 0U;
  while (got < kSdRwSoakChunkBytes) {
    const uint32_t t0 = micros();
    const size_t chunk = g_sd_rw_soak.source_file.read(s_buffer + got, kSdRwSoakChunkBytes - got);
    total_read_us += micros() - t0;
    if (chunk == 0U) {
      if (!g_sd_rw_soak.source_file.seek(0U)) {
        g_sd_rw_soak.read_failures++;
        g_sd_rw_soak.ok = false;
        stopSdReadWriteSoak(false, "source_seek_failed");
        return;
      }
      g_sd_rw_soak.wraps++;
      continue;
    }
    got += chunk;
  }
  if (total_read_us > g_sd_rw_soak.max_read_us) g_sd_rw_soak.max_read_us = total_read_us;

  if (got == 0U) {
    g_sd_rw_soak.read_failures++;
    g_sd_rw_soak.ok = false;
    stopSdReadWriteSoak(false, "source_read_failed");
    return;
  }

  uint32_t t0 = micros();
  const size_t wrote = g_sd_rw_soak.dest_file.write(s_buffer, got);
  const uint32_t write_us = micros() - t0;
  if (write_us > g_sd_rw_soak.max_write_us) g_sd_rw_soak.max_write_us = write_us;
  if (wrote != got) {
    g_sd_rw_soak.write_failures++;
    g_sd_rw_soak.ok = false;
    stopSdReadWriteSoak(false, "dest_write_failed");
    return;
  }

  g_sd_rw_soak.bytes_read += (uint32_t)got;
  g_sd_rw_soak.bytes_written += (uint32_t)wrote;
  g_sd_rw_soak.iterations++;
}

bool beginSdReadWriteLogSoak(uint32_t duration_ms, const String* source_override) {
  if (g_sd_rw_log_soak.active) {
    Serial.println("SDRWLOGSOAK START ok=0 reason=already_active");
    return false;
  }
  if (g_sd_rw_soak.active) {
    Serial.println("SDRWLOGSOAK START ok=0 reason=raw_soak_active");
    return false;
  }
  if (log_store::busy()) {
    Serial.println("SDRWLOGSOAK START ok=0 reason=logger_busy");
    return false;
  }
  if (g_replay_capture.active || replay_bridge::active()) {
    Serial.println("SDRWLOGSOAK START ok=0 reason=replay_busy");
    return false;
  }
  if (!sd_backend::mounted()) {
    sd_backend::Status backend = {};
    if (!sd_backend::begin(&backend)) {
      Serial.println("SDRWLOGSOAK START ok=0 reason=sd_mount_failed");
      return false;
    }
  }

  String source_name;
  if (source_override && !source_override->isEmpty()) {
    source_name = *source_override;
  } else if (!log_store::largestLogName(source_name)) {
    Serial.println("SDRWLOGSOAK START ok=0 reason=no_source_log");
    return false;
  }
  if (!source_name.startsWith("/")) source_name = String("/logs/") + source_name;

  sd_api::File source_file = sd_api::open(source_name, sd_api::OpenMode::read);
  if (!source_file) {
    Serial.printf("SDRWLOGSOAK START ok=0 reason=open_source_failed src=%s\r\n",
                  shortLogName(source_name).c_str());
    return false;
  }

  const uint32_t session_id = nextConsoleLogSessionId();
  if (!isStandaloneBench()) radio_link::setRecorderEnabled(true);
  if (!log_store::startSession(session_id)) {
    source_file.close();
    Serial.printf("SDRWLOGSOAK START ok=0 reason=logstart_failed session=%lu\r\n",
                  (unsigned long)session_id);
    return false;
  }

  g_sd_rw_log_soak = {};
  g_sd_rw_log_soak.active = true;
  g_sd_rw_log_soak.ok = true;
  g_sd_rw_log_soak.started_ms = millis();
  g_sd_rw_log_soak.duration_ms = duration_ms;
  g_sd_rw_log_soak.session_id = session_id;
  g_sd_rw_log_soak.source_name = source_name;
  g_sd_rw_log_soak.source_file = source_file;
  Serial.printf("SDRWLOGSOAK START ok=1 duration_ms=%lu src=%s session=%lu\r\n",
                (unsigned long)duration_ms,
                shortLogName(source_name).c_str(),
                (unsigned long)session_id);
  return true;
}

void stopSdReadWriteLogSoak(bool ok, const char* reason) {
  if (!g_sd_rw_log_soak.active) return;
  if (!g_sd_rw_log_soak.stop_requested) {
    g_sd_rw_log_soak.stop_requested = true;
    g_sd_rw_log_soak.ok = g_sd_rw_log_soak.ok && ok;
    if (g_sd_rw_log_soak.source_file) {
      g_sd_rw_log_soak.source_file.close();
    }
    log_store::stopSession();
  }
  if (log_store::busy()) return;

  const auto stats = log_store::stats();
  const auto recorder = log_store::recorderStatus();
  String closed_name;
  (void)log_store::latestLogNameForSession(g_sd_rw_log_soak.session_id, closed_name);
  const uint32_t elapsed_ms = millis() - g_sd_rw_log_soak.started_ms;
  Serial.printf(
      "SDRWLOGSOAK RESULT ok=%u elapsed_ms=%lu src=%s dst=%s session=%lu bytes_read=%lu records_read=%lu wraps=%lu read_fail=%lu unsupported=%lu logged_bytes=%lu written=%lu dropped=%lu",
      (g_sd_rw_log_soak.ok && ok) ? 1U : 0U,
      (unsigned long)elapsed_ms,
      shortLogName(g_sd_rw_log_soak.source_name).c_str(),
      shortLogName(closed_name).c_str(),
      (unsigned long)g_sd_rw_log_soak.session_id,
      (unsigned long)g_sd_rw_log_soak.bytes_read,
      (unsigned long)g_sd_rw_log_soak.records_read,
      (unsigned long)g_sd_rw_log_soak.wraps,
      (unsigned long)g_sd_rw_log_soak.read_failures,
      (unsigned long)g_sd_rw_log_soak.unsupported_records,
      (unsigned long)recorder.bytes_written,
      (unsigned long)stats.records_written,
      (unsigned long)stats.dropped);
  if (reason && reason[0] != '\0') {
    Serial.printf(" reason=%s", reason);
  }
  Serial.print("\r\n");
  g_sd_rw_log_soak = {};
}

void serviceSdReadWriteLogSoak() {
  if (!g_sd_rw_log_soak.active) return;
  if (g_sd_rw_log_soak.stop_requested) {
    stopSdReadWriteLogSoak(g_sd_rw_log_soak.ok, nullptr);
    return;
  }
  if ((uint32_t)(millis() - g_sd_rw_log_soak.started_ms) >= g_sd_rw_log_soak.duration_ms) {
    stopSdReadWriteLogSoak(g_sd_rw_log_soak.ok, nullptr);
    return;
  }

  SoakBinaryLogRecordV2 record = {};
  if (!readExactFile(g_sd_rw_log_soak.source_file, (uint8_t*)&record, sizeof(record))) {
    if (!g_sd_rw_log_soak.source_file.seek(0U)) {
      g_sd_rw_log_soak.read_failures++;
      g_sd_rw_log_soak.ok = false;
      stopSdReadWriteLogSoak(false, "source_seek_failed");
      return;
    }
    g_sd_rw_log_soak.wraps++;
    if (!readExactFile(g_sd_rw_log_soak.source_file, (uint8_t*)&record, sizeof(record))) {
      g_sd_rw_log_soak.read_failures++;
      g_sd_rw_log_soak.ok = false;
      stopSdReadWriteLogSoak(false, "source_read_failed");
      return;
    }
  }

  if (record.magic != kSoakLogMagic || record.version != kSoakLogVersion ||
      record.record_size != sizeof(record)) {
    g_sd_rw_log_soak.read_failures++;
    g_sd_rw_log_soak.ok = false;
    stopSdReadWriteLogSoak(false, "bad_record");
    return;
  }

  switch ((telem::LogRecordKind)record.record_kind) {
    case telem::LogRecordKind::State160: {
      telem::TelemetryFullStateV1 state = {};
      memcpy(&state, record.payload, sizeof(state));
      log_store::enqueueState(record.seq, record.t_us, state);
      break;
    }
    case telem::LogRecordKind::ReplayInput160: {
      telem::ReplayInputRecord160 replay = {};
      memcpy(&replay, record.payload, sizeof(replay));
      log_store::enqueueReplayInput(record.seq, record.t_us, replay);
      break;
    }
    case telem::LogRecordKind::ReplayControl160: {
      telem::ReplayControlRecord160 replay = {};
      memcpy(&replay, record.payload, sizeof(replay));
      log_store::enqueueReplayControl(
          replay.payload.command_id,
          record.seq,
          record.t_us,
          replay.payload.payload,
          replay.payload.payload_len,
          replay.payload.apply_flags);
      break;
    }
    default:
      g_sd_rw_log_soak.unsupported_records++;
      break;
  }

  g_sd_rw_log_soak.bytes_read += (uint32_t)sizeof(record);
  g_sd_rw_log_soak.records_read++;
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
  uint16_t applied_capture_hz = 0U;
  if (!applyReplayRunControls(replay_bridge::averageFactor(), &applied_capture_hz)) {
    Serial.printf("AIRREPLAYCAP START ok=0 reason=control_apply_failed src=%s session=%lu avg_n=%u\r\n",
                  shortLogName(source_name).c_str(),
                  (unsigned long)session_id,
                  (unsigned)replay_bridge::averageFactor());
    return false;
  }

  log_store::setNextSessionMetadata(source_name,
                                    replay_bridge::averageFactor(),
                                    applied_capture_hz);
  if (!isStandaloneBench()) radio_link::setRecorderEnabled(true);
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
  Serial.printf("AIRREPLAYCAP START ok=1 src=%s dst=%s session=%lu records_total=%lu avg_n=%u cap_hz=%u\r\n",
                shortLogName(source_name).c_str(),
                shortLogName(capture_name).c_str(),
                (unsigned long)session_id,
                (unsigned long)replay.records_total,
                (unsigned)replay_bridge::averageFactor(),
                (unsigned)applied_capture_hz);
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
  Serial.printf("AIRREPLAY START ok=1 src=%s records_total=%lu avg_n=%u\r\n",
                shortLogName(source_name).c_str(),
                (unsigned long)replay.records_total,
                (unsigned)replay_bridge::averageFactor());
  return true;
}

void noteReplayCaptureComplete(const String& closed_name, uint32_t session_id, bool ok,
                               uint32_t written, uint32_t dropped) {
  if (!g_replay_compare.active) return;
  const uint8_t index = g_replay_compare.completed_runs;
  if (index >= 2U) return;
  g_replay_compare.sessions[index] = session_id;
  g_replay_compare.ok[index] = ok;
  g_replay_compare.written[index] = written;
  g_replay_compare.dropped[index] = dropped;
  g_replay_compare.capture_rate_hz[index] =
      replayCaptureRateForFactor((uint8_t)(g_replay_compare.base_factor + index));
  g_replay_compare.outputs[index] = closed_name;
  g_replay_compare.completed_runs = (uint8_t)(index + 1U);
  g_replay_compare.last_completion_ms = millis();
  Serial.printf("AIRREPLAYCMP PASS done avg_n=%u cap_hz=%u dst=%s written=%lu dropped=%lu ok=%u\r\n",
                (unsigned)(g_replay_compare.base_factor + index),
                (unsigned)g_replay_compare.capture_rate_hz[index],
                shortLogName(closed_name).c_str(),
                (unsigned long)written,
                (unsigned long)dropped,
                ok ? 1U : 0U);
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
  const bool ok = (replay.last_error == 0U);
  Serial.printf(
      "AIRREPLAYCAP RESULT ok=%u elapsed_ms=%lu dst=%s session=%lu sent=%lu total=%lu bytes=%lu written=%lu dropped=%lu avg_n=%u\r\n",
      ok ? 1U : 0U,
      (unsigned long)elapsed_ms,
      shortLogName(closed_name).c_str(),
      (unsigned long)g_replay_capture.session_id,
      (unsigned long)replay.records_sent,
      (unsigned long)replay.records_total,
      (unsigned long)recorder.bytes_written,
      (unsigned long)stats.records_written,
      (unsigned long)stats.dropped,
      (unsigned)replay_bridge::averageFactor());
  noteReplayCaptureComplete(closed_name, g_replay_capture.session_id, ok,
                            stats.records_written, stats.dropped);
  g_replay_capture = ReplayCaptureRun{};
}

bool beginReplayCompare(uint8_t base_factor, const String* source_override) {
  if (g_replay_compare.active) {
    Serial.println("AIRREPLAYCMP START ok=0 reason=already_active");
    return false;
  }
  if (g_replay_capture.active || replay_bridge::active() || log_store::busy()) {
    Serial.println("AIRREPLAYCMP START ok=0 reason=busy");
    return false;
  }

  String source_name;
  if (source_override && !source_override->isEmpty()) {
    source_name = *source_override;
  } else if (!log_store::latestLogName(source_name)) {
    Serial.println("AIRREPLAYCMP START ok=0 reason=no_source_log");
    return false;
  }

  g_replay_compare = {};
  g_replay_compare.active = true;
  g_replay_compare.source_name = source_name;
  replay_bridge::setAverageFactor((uint8_t)constrain((unsigned)base_factor, 1U, 31U));
  g_replay_compare.base_factor = replay_bridge::averageFactor();
  if (!beginReplayCapture(&g_replay_compare.source_name)) {
    g_replay_compare = {};
    Serial.println("AIRREPLAYCMP START ok=0 reason=replaycap_failed");
    return false;
  }

  Serial.printf("AIRREPLAYCMP START ok=1 src=%s n=%u n_next=%u\r\n",
                shortLogName(g_replay_compare.source_name).c_str(),
                (unsigned)g_replay_compare.base_factor,
                (unsigned)(g_replay_compare.base_factor + 1U));
  return true;
}

void serviceReplayCompare() {
  if (!g_replay_compare.active) return;
  if (g_replay_capture.active || replay_bridge::active() || log_store::busy()) return;

  if (g_replay_compare.completed_runs >= 2U) {
    Serial.printf("AIRREPLAYCMP RESULT ok=%u src=%s n=%u cap_n=%u cap_n1=%u file_n=%s file_n1=%s written_n=%lu written_n1=%lu dropped_n=%lu dropped_n1=%lu\r\n",
                  (g_replay_compare.ok[0] && g_replay_compare.ok[1]) ? 1U : 0U,
                  shortLogName(g_replay_compare.source_name).c_str(),
                  (unsigned)g_replay_compare.base_factor,
                  (unsigned)g_replay_compare.capture_rate_hz[0],
                  (unsigned)g_replay_compare.capture_rate_hz[1],
                  shortLogName(g_replay_compare.outputs[0]).c_str(),
                  shortLogName(g_replay_compare.outputs[1]).c_str(),
                  (unsigned long)g_replay_compare.written[0],
                  (unsigned long)g_replay_compare.written[1],
                  (unsigned long)g_replay_compare.dropped[0],
                  (unsigned long)g_replay_compare.dropped[1]);
    g_replay_compare = {};
    return;
  }

  if (g_replay_compare.completed_runs == 1U &&
      (uint32_t)(millis() - g_replay_compare.last_completion_ms) >= kReplayCompareSettleMs) {
    replay_bridge::setAverageFactor((uint8_t)(g_replay_compare.base_factor + 1U));
    if (!beginReplayCapture(&g_replay_compare.source_name)) {
      Serial.println("AIRREPLAYCMP RESULT ok=0 reason=second_pass_start_failed");
      g_replay_compare = {};
    }
  }
}

bool beginBenchSweep(uint32_t duration_ms) {
  if (g_bench_sweep.active) {
    Serial.println("AIRBENCH START ok=0 reason=already_active");
    return false;
  }
  if (!isStandaloneBench()) {
    Serial.println("AIRBENCH START ok=0 reason=standalone_required");
    return false;
  }
  if (g_replay_capture.active || replay_bridge::active() || log_store::busy()) {
    Serial.println("AIRBENCH START ok=0 reason=busy");
    return false;
  }
  g_bench_sweep = {};
  g_bench_sweep.active = true;
  g_bench_sweep.duration_ms = duration_ms;
  g_bench_sweep.rate_index = 0U;
  g_bench_sweep.current_rate_hz = kBenchCaptureRatesHz[0];
  g_bench_sweep.phase = BenchSweepPhase::WaitApply;
  g_bench_sweep.phase_started_ms = millis();
  if (!setCaptureRateHz(g_bench_sweep.current_rate_hz, true)) {
    g_bench_sweep = {};
    Serial.println("AIRBENCH START ok=0 reason=set_capture_failed");
    return false;
  }
  Serial.printf("AIRBENCH START ok=1 duration_ms=%lu rates=%u\r\n",
                (unsigned long)duration_ms,
                (unsigned)(sizeof(kBenchCaptureRatesHz) / sizeof(kBenchCaptureRatesHz[0])));
  return true;
}

void serviceBenchSweep() {
  if (!g_bench_sweep.active) return;
  const uint32_t now = millis();
  switch (g_bench_sweep.phase) {
    case BenchSweepPhase::WaitApply:
      if ((uint32_t)(now - g_bench_sweep.phase_started_ms) < kBenchApplySettleMs) return;
      g_bench_sweep.session_id = nextConsoleLogSessionId();
      if (!log_store::startSession(g_bench_sweep.session_id)) {
        Serial.printf("AIRBENCH RATE ok=0 rate=%u reason=logstart_failed\r\n",
                      (unsigned)g_bench_sweep.current_rate_hz);
        g_bench_sweep.active = false;
        g_bench_sweep.phase = BenchSweepPhase::Idle;
        return;
      }
      g_bench_sweep.phase = BenchSweepPhase::Logging;
      g_bench_sweep.phase_started_ms = now;
      Serial.printf("AIRBENCH RATE start rate=%u session=%lu\r\n",
                    (unsigned)g_bench_sweep.current_rate_hz,
                    (unsigned long)g_bench_sweep.session_id);
      return;

    case BenchSweepPhase::Logging:
      if ((uint32_t)(now - g_bench_sweep.phase_started_ms) < g_bench_sweep.duration_ms) return;
      log_store::stopSession();
      g_bench_sweep.phase = BenchSweepPhase::WaitLogStop;
      g_bench_sweep.phase_started_ms = now;
      return;

    case BenchSweepPhase::WaitLogStop:
      if (log_store::busy()) return;
      {
        String closed_name;
        (void)log_store::latestLogNameForSession(g_bench_sweep.session_id, closed_name);
        const auto stats = log_store::stats();
        Serial.printf("AIRBENCH RATE done rate=%u session=%lu file=%s records=%lu dropped=%lu\r\n",
                      (unsigned)g_bench_sweep.current_rate_hz,
                      (unsigned long)g_bench_sweep.session_id,
                      shortLogName(closed_name).c_str(),
                      (unsigned long)stats.records_written,
                      (unsigned long)stats.dropped);
      }
      g_bench_sweep.rate_index++;
      if (g_bench_sweep.rate_index >= (sizeof(kBenchCaptureRatesHz) / sizeof(kBenchCaptureRatesHz[0]))) {
        g_bench_sweep.active = false;
        g_bench_sweep.phase = BenchSweepPhase::Idle;
        Serial.println("AIRBENCH RESULT ok=1");
        return;
      }
      g_bench_sweep.current_rate_hz = kBenchCaptureRatesHz[g_bench_sweep.rate_index];
      g_bench_sweep.phase = BenchSweepPhase::WaitApply;
      g_bench_sweep.phase_started_ms = now;
      if (!setCaptureRateHz(g_bench_sweep.current_rate_hz, true)) {
        Serial.printf("AIRBENCH RATE ok=0 rate=%u reason=set_capture_failed\r\n",
                      (unsigned)g_bench_sweep.current_rate_hz);
        g_bench_sweep.active = false;
        g_bench_sweep.phase = BenchSweepPhase::Idle;
      }
      return;

    case BenchSweepPhase::Idle:
    default:
      g_bench_sweep.active = false;
      return;
  }
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

      if (handleTeensyApiConsoleCommand(g_console_line)) {
        continue;
      } else if (strcmp(g_console_line, "help") == 0 || strcmp(g_console_line, "h") == 0) {
        printConsoleHelp();
      } else if (strcmp(g_console_line, "sdprobe") == 0) {
        printSdApiStatus("SDPROBE");
      } else if (strcmp(g_console_line, "sdmount") == 0) {
        sd_backend::Status backend = {};
        const bool ok = sd_backend::mount(&backend);
        Serial.printf("SDMOUNT ok=%u mounted=%u card_type=%u init_hz=%lu total=%lu used=%lu\r\n",
                      ok ? 1U : 0U,
                      sd_backend::mounted() ? 1U : 0U,
                      (unsigned)backend.card_type,
                      (unsigned long)backend.init_hz,
                      (unsigned long)backend.total_bytes,
                      (unsigned long)backend.used_bytes);
      } else if (strcmp(g_console_line, "sdeject") == 0) {
        if (log_store::busy() || replay_bridge::active()) {
          Serial.println("SDEJECT ok=0 reason=busy");
        } else {
          const bool ok = sd_backend::eject();
          Serial.printf("SDEJECT ok=%u mounted=%u\r\n", ok ? 1U : 0U, sd_backend::mounted() ? 1U : 0U);
        }
      } else if (strcmp(g_console_line, "sdstate") == 0) {
        printSdApiStatus("SDSTATE");
      } else if (strcmp(g_console_line, "sdrwsoakstat") == 0) {
        Serial.printf("SDRWSOAK active=%u src=%s dst=%s bytes_read=%lu bytes_written=%lu wraps=%lu read_fail=%lu write_fail=%lu max_read_us=%lu max_write_us=%lu iterations=%lu\r\n",
                      g_sd_rw_soak.active ? 1U : 0U,
                      shortLogName(g_sd_rw_soak.source_name).c_str(),
                      shortLogName(g_sd_rw_soak.dest_name).c_str(),
                      (unsigned long)g_sd_rw_soak.bytes_read,
                      (unsigned long)g_sd_rw_soak.bytes_written,
                      (unsigned long)g_sd_rw_soak.wraps,
                      (unsigned long)g_sd_rw_soak.read_failures,
                      (unsigned long)g_sd_rw_soak.write_failures,
                      (unsigned long)g_sd_rw_soak.max_read_us,
                      (unsigned long)g_sd_rw_soak.max_write_us,
                      (unsigned long)g_sd_rw_soak.iterations);
      } else if (strcmp(g_console_line, "sdrwsoakstop") == 0) {
        if (g_sd_rw_soak.active) {
          stopSdReadWriteSoak(g_sd_rw_soak.ok, "stopped");
        } else {
          Serial.println("SDRWSOAK active=0");
        }
      } else if (strncmp(g_console_line, "sdrwsoak ", 9) == 0) {
        unsigned duration_s = 0U;
        char src_name[64] = {};
        const int n = sscanf(g_console_line + 9, "%u %63s", &duration_s, src_name);
        if (n >= 1 && duration_s > 0U) {
          String source_name;
          String* source_override = nullptr;
          if (n >= 2) {
            source_name = String(src_name);
            source_name.trim();
            source_override = &source_name;
          }
          (void)beginSdReadWriteSoak((uint32_t)duration_s * 1000U, source_override);
        } else {
          Serial.println("SDRWSOAK usage: sdrwsoak <seconds> [file]");
        }
      } else if (strcmp(g_console_line, "sdrwlogsoakstat") == 0) {
        Serial.printf("SDRWLOGSOAK active=%u stop=%u src=%s session=%lu bytes_read=%lu records_read=%lu wraps=%lu read_fail=%lu unsupported=%lu\r\n",
                      g_sd_rw_log_soak.active ? 1U : 0U,
                      g_sd_rw_log_soak.stop_requested ? 1U : 0U,
                      shortLogName(g_sd_rw_log_soak.source_name).c_str(),
                      (unsigned long)g_sd_rw_log_soak.session_id,
                      (unsigned long)g_sd_rw_log_soak.bytes_read,
                      (unsigned long)g_sd_rw_log_soak.records_read,
                      (unsigned long)g_sd_rw_log_soak.wraps,
                      (unsigned long)g_sd_rw_log_soak.read_failures,
                      (unsigned long)g_sd_rw_log_soak.unsupported_records);
      } else if (strcmp(g_console_line, "sdrwlogsoakstop") == 0) {
        if (g_sd_rw_log_soak.active) {
          stopSdReadWriteLogSoak(g_sd_rw_log_soak.ok, "stopped");
        } else {
          Serial.println("SDRWLOGSOAK active=0");
        }
      } else if (strncmp(g_console_line, "sdrwlogsoak ", 12) == 0) {
        unsigned duration_s = 0U;
        char src_name[64] = {};
        const int n = sscanf(g_console_line + 12, "%u %63s", &duration_s, src_name);
        if (n >= 1 && duration_s > 0U) {
          String source_name;
          String* source_override = nullptr;
          if (n >= 2) {
            source_name = String(src_name);
            source_name.trim();
            source_override = &source_name;
          }
          (void)beginSdReadWriteLogSoak((uint32_t)duration_s * 1000U, source_override);
        } else {
          Serial.println("SDRWLOGSOAK usage: sdrwlogsoak <seconds> [file]");
        }
      } else if (strncmp(g_console_line, "carrysigcsv", 11) == 0) {
        uint32_t duration_ms = 2000U;
        unsigned window = 16U;
        if (g_console_line[11] == '\0') {
          (void)teensy_api::runCarrySequenceCsvTest(Serial, duration_ms, (uint8_t)window);
        } else if (g_console_line[11] == ' ') {
          const int matched = sscanf(g_console_line + 12, "%lu %u", &duration_ms, &window);
          if (matched >= 1 && duration_ms > 0U) {
            if (window == 0U) window = 16U;
            if (window > 255U) window = 255U;
            (void)teensy_api::runCarrySequenceCsvTest(Serial, duration_ms, (uint8_t)window);
          } else {
            Serial.println("CARRYSIGCSV usage: carrysigcsv [ms] [window]");
          }
        } else {
          Serial.println("CARRYSIGCSV usage: carrysigcsv [ms] [window]");
        }
      } else if (strncmp(g_console_line, "carrysig", 8) == 0) {
        unsigned count = 8U;
        (void)sscanf(g_console_line + 8, "%u", &count);
        if (count == 0U) count = 8U;
        if (count > 255U) count = 255U;
        (void)teensy_api::runCarrySignatureTest(Serial, (uint8_t)count);
      } else if (strcmp(g_console_line, "sdwrite") == 0) {
        (void)writeSdApiTestLog();
      } else if (strncmp(g_console_line, "sdrename ", 9) == 0) {
        char src_name[48] = {};
        char dst_name[48] = {};
        if (sscanf(g_console_line + 9, "%47s %47s", src_name, dst_name) == 2) {
          const bool ok = log_store::renameFileByName(String(src_name), String(dst_name));
          Serial.printf("SDRENAME ok=%u from=%s to=%s\r\n", ok ? 1U : 0U, src_name, dst_name);
        } else {
          Serial.println("SDRENAME usage: sdrename <from> <to>");
        }
      } else if (strncmp(g_console_line, "sddelete ", 9) == 0) {
        char file_name[48] = {};
        if (sscanf(g_console_line + 9, "%47s", file_name) == 1) {
          const bool ok = log_store::deleteFileByName(String(file_name));
          Serial.printf("SDDELETE ok=%u file=%s\r\n", ok ? 1U : 0U, file_name);
        } else {
          Serial.println("SDDELETE usage: sddelete <file>");
        }
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
      } else if (strcmp(g_console_line, "logstart") == 0) {
        const uint32_t session_id = nextConsoleLogSessionId();
        log_store::setNextSessionMetadata(String(), 1U, config_store::get().source_rate_hz);
        if (!isStandaloneBench()) radio_link::setRecorderEnabled(true);
        const bool ok = log_store::startSession(session_id);
        Serial.printf("AIRLOG START ok=%u session=%lu\r\n",
                      ok ? 1U : 0U,
                      (unsigned long)session_id);
        printAirLogStatus();
      } else if (strncmp(g_console_line, "logstartid ", 11) == 0) {
        unsigned session_id = 0U;
        if (sscanf(g_console_line + 11, "%u", &session_id) == 1 && session_id != 0U) {
          g_console_log_session_id = (uint32_t)session_id;
          log_store::setNextSessionMetadata(String(), 1U, config_store::get().source_rate_hz);
          if (!isStandaloneBench()) radio_link::setRecorderEnabled(true);
          const bool ok = log_store::startSession((uint32_t)session_id);
          Serial.printf("AIRLOG START ok=%u session=%u\r\n", ok ? 1U : 0U, session_id);
          printAirLogStatus();
        } else {
          Serial.println("AIRLOG usage: logstartid <session>");
        }
      } else if (strcmp(g_console_line, "logstop") == 0) {
        log_store::stopSession();
        Serial.println("AIRLOG STOP requested=1");
        const bool stopped_ok = waitForLogStoreIdle(10000U);
        Serial.printf("AIRLOG STOP ok=%u\r\n", stopped_ok ? 1U : 0U);
        printAirLogStatus();
      } else if (strcmp(g_console_line, "logstat") == 0) {
        printAirLogStatus();
      } else if (strcmp(g_console_line, "latestlog") == 0) {
        String latest_name;
        const bool ok = log_store::latestLogName(latest_name);
        Serial.printf("AIRLOG latest_ok=%u file=%s\r\n",
                      ok ? 1U : 0U,
                      ok ? shortLogName(latest_name).c_str() : "(none)");
      } else if (strncmp(g_console_line, "logfiles", 8) == 0) {
        log_store::FileSortKey sort_key = log_store::FileSortKey::date;
        log_store::FileSortDirection sort_dir = log_store::FileSortDirection::descending;
        char key_buf[12] = {};
        char dir_buf[12] = {};
        if (sscanf(g_console_line + 8, "%11s %11s", key_buf, dir_buf) >= 1) {
          if (strcmp(key_buf, "name") == 0) sort_key = log_store::FileSortKey::name;
          else if (strcmp(key_buf, "size") == 0) sort_key = log_store::FileSortKey::size;
          else if (strcmp(key_buf, "date") == 0) sort_key = log_store::FileSortKey::date;
          if (strcmp(dir_buf, "asc") == 0) sort_dir = log_store::FileSortDirection::ascending;
          else if (strcmp(dir_buf, "desc") == 0) sort_dir = log_store::FileSortDirection::descending;
        }
        Serial.println(log_store::filesJson(sort_key, sort_dir));
      } else if (strncmp(g_console_line, "logprefix", 9) == 0) {
        char prefix_buf[24] = {};
        if (sscanf(g_console_line + 9, "%23s", prefix_buf) == 1) {
          AppConfig cfg = config_store::get();
          strlcpy(cfg.record_prefix, prefix_buf, sizeof(cfg.record_prefix));
          config_store::update(cfg);
          log_store::setConfig(config_store::get());
        }
        Serial.printf("AIRLOG prefix=%s preview=%s\r\n",
                      log_store::recordPrefix().c_str(),
                      shortLogName(log_store::previewLogName()).c_str());
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
      } else if (strncmp(g_console_line, "csvfile ", 8) == 0) {
        char file_name[48] = {};
        if (sscanf(g_console_line + 8, "%47s", file_name) == 1) {
          const bool ok = log_store::exportLogToCsvByName(String(file_name), &Serial);
          Serial.printf("AIRCSV FILE ok=%u file=%s\r\n", ok ? 1U : 0U, file_name);
        } else {
          Serial.println("AIRCSV usage: csvfile <name.tlog>");
        }
      } else if (strncmp(g_console_line, "comparelogs ", 12) == 0) {
        char src_name[48] = {};
        char dst_name[48] = {};
        if (sscanf(g_console_line + 12, "%47s %47s", src_name, dst_name) == 2) {
          const bool ok = log_store::compareLogs(Serial, String(src_name), String(dst_name));
          Serial.printf("AIRCOMPARE RESULT ok=%u\r\n", ok ? 1U : 0U);
        } else {
          Serial.println("AIRCOMPARE usage: comparelogs <src.tlog> <dst.tlog>");
        }
      } else if (strncmp(g_console_line, "comparetimed ", 13) == 0) {
        char src_name[48] = {};
        char dst_name[48] = {};
        unsigned warmup_skip = 4U;
        const int parsed = sscanf(g_console_line + 13, "%47s %47s %u", src_name, dst_name, &warmup_skip);
        if (parsed >= 2) {
          const bool ok = log_store::compareLogsTimed(Serial, String(src_name), String(dst_name), (uint16_t)warmup_skip);
          Serial.printf("AIRCOMPARET RESULT ok=%u\r\n", ok ? 1U : 0U);
        } else {
          Serial.println("AIRCOMPARET usage: comparetimed <src.tlog> <dst.tlog> [warmup_skip]");
        }
      } else if (strncmp(g_console_line, "logkinds ", 9) == 0) {
        String log_name = String(g_console_line + 9);
        log_name.trim();
        if (log_name.length() == 0U) {
          Serial.println("AIRLOGKINDS usage: logkinds <file.tlog>");
        } else {
          const bool ok = log_store::printRecordKindSummary(Serial, log_name);
          Serial.printf("AIRLOGKINDS RESULT ok=%u\r\n", ok ? 1U : 0U);
        }
      } else if (strncmp(g_console_line, "logfusion ", 10) == 0) {
        String log_name = String(g_console_line + 10);
        log_name.trim();
        if (log_name.length() == 0U) {
          Serial.println("AIRLOGFUSION usage: logfusion <file.tlog>");
        } else {
          const bool ok = log_store::printFusionSettingsSummary(Serial, log_name);
          Serial.printf("AIRLOGFUSION RESULT ok=%u\r\n", ok ? 1U : 0U);
        }
      } else if (strncmp(g_console_line, "logflags ", 9) == 0) {
        String log_name = String(g_console_line + 9);
        log_name.trim();
        if (log_name.length() == 0U) {
          Serial.println("AIRLOGFLAGS usage: logflags <file.tlog>");
        } else {
          const bool ok = log_store::printFusionFlagSummary(Serial, log_name);
          Serial.printf("AIRLOGFLAGS RESULT ok=%u\r\n", ok ? 1U : 0U);
        }
      } else if (strcmp(g_console_line, "replaycapture") == 0 || strcmp(g_console_line, "replaycap") == 0) {
        (void)beginReplayCapture(nullptr);
      } else if (strncmp(g_console_line, "replayavg ", 10) == 0) {
        unsigned factor = 0U;
        if (sscanf(g_console_line + 10, "%u", &factor) == 1) {
          replay_bridge::setAverageFactor((uint8_t)factor);
          Serial.printf("AIRREPLAYAVG n=%u\r\n", (unsigned)replay_bridge::averageFactor());
        } else {
          Serial.println("AIRREPLAYAVG usage: replayavg <n>");
        }
      } else if (strcmp(g_console_line, "replayavgstat") == 0) {
        Serial.printf("AIRREPLAYAVG n=%u\r\n", (unsigned)replay_bridge::averageFactor());
      } else if (strncmp(g_console_line, "replaycmpfile ", 14) == 0) {
        unsigned factor = 0U;
        char src_name[48] = {};
        if (sscanf(g_console_line + 14, "%u %47s", &factor, src_name) == 2) {
          String source_name = String(src_name);
          source_name.trim();
          (void)beginReplayCompare((uint8_t)factor, &source_name);
        } else {
          Serial.println("AIRREPLAYCMP usage: replaycmpfile <n> <file.tlog>");
        }
      } else if (strncmp(g_console_line, "replaycmp ", 10) == 0) {
        unsigned factor = 0U;
        if (sscanf(g_console_line + 10, "%u", &factor) == 1) {
          (void)beginReplayCompare((uint8_t)factor, nullptr);
        } else {
          Serial.println("AIRREPLAYCMP usage: replaycmp <n>");
        }
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
        Serial.printf("AIRREPLAYCAP active=%u stop_requested=%u session=%lu sent=%lu total=%lu last_error=%lu avg_n=%u cmp_active=%u cmp_done=%u\r\n",
                      g_replay_capture.active ? 1U : 0U,
                      g_replay_capture.stop_requested ? 1U : 0U,
                      (unsigned long)g_replay_capture.session_id,
                      (unsigned long)replay.records_sent,
                      (unsigned long)replay.records_total,
                      (unsigned long)replay.last_error,
                      (unsigned)replay_bridge::averageFactor(),
                      g_replay_compare.active ? 1U : 0U,
                      (unsigned)g_replay_compare.completed_runs);
      } else if (strncmp(g_console_line, "setpin ", 7) == 0) {
        unsigned pin = 0U;
        if (sscanf(g_console_line + 7, "%u", &pin) == 1 && pin <= 48U) {
          startGpioPulse((uint8_t)pin);
        } else {
          Serial.println("SETPIN usage: setpin <gpio>");
        }
      } else if (strcmp(g_console_line, "getfusion") == 0 || strcmp(g_console_line, "get fusion") == 0) {
        teensy_api::CommandAckResult result = {};
        (void)teensy_api::getFusionSettings(result);
        Serial.printf("GETFUSION tx_ok=%u ack_seen=%u ack_ok=%u code=%lu\r\n",
                      result.tx_ok ? 1U : 0U,
                      result.ack_seen ? 1U : 0U,
                      result.ack_ok ? 1U : 0U,
                      (unsigned long)result.ack_code);
      } else if (strncmp(g_console_line, "setcap ", 7) == 0) {
        unsigned hz = 0U;
        if (sscanf(g_console_line + 7, "%u", &hz) == 1) {
          const bool ok = setCaptureRateHz((uint16_t)hz, true);
          Serial.printf("SETCAP tx_ok=%u hz=%u\r\n", ok ? 1U : 0U, hz);
        } else {
          Serial.println("SETCAP usage: setcap <hz>");
        }
      } else if (strcmp(g_console_line, "savecap") == 0) {
        teensy_api::CommandAckResult result = {};
        (void)teensy_api::saveCaptureSettings(result);
        Serial.printf("SAVECAP tx_ok=%u ack_seen=%u ack_ok=%u code=%lu\r\n",
                      result.tx_ok ? 1U : 0U,
                      result.ack_seen ? 1U : 0U,
                      result.ack_ok ? 1U : 0U,
                      (unsigned long)result.ack_code);
      } else if (strcmp(g_console_line, "bench status") == 0 || strcmp(g_console_line, "benchstatus") == 0 ||
                 strcmp(g_console_line, "benchstat") == 0) {
        const AppConfig& cfg = config_store::get();
        Serial.printf("BENCH status standalone=%u active=%u rate=%u idx=%u duration_ms=%lu\r\n",
                      cfg.standalone_bench ? 1U : 0U,
                      g_bench_sweep.active ? 1U : 0U,
                      (unsigned)g_bench_sweep.current_rate_hz,
                      (unsigned)g_bench_sweep.rate_index,
                      (unsigned long)g_bench_sweep.duration_ms);
      } else if (strcmp(g_console_line, "bench on") == 0 || strcmp(g_console_line, "benchon") == 0) {
        AppConfig cfg = config_store::get();
        cfg.standalone_bench = 1U;
        config_store::update(cfg);
        radio_link::resetNetworkState();
        resetWifiStatusFlags();
        WiFi.mode(WIFI_OFF);
        radio_link::setVerbose(false);
        Serial.println("BENCH standalone=1 radio=0 wifi=off");
      } else if (strcmp(g_console_line, "bench off") == 0 || strcmp(g_console_line, "benchoff") == 0) {
        AppConfig cfg = config_store::get();
        cfg.standalone_bench = 0U;
        config_store::update(cfg);
        restartWifiStation();
        radio_link::reconfigure(config_store::get());
        radio_link::setVerbose(!g_quiet_serial);
        Serial.println("BENCH standalone=0 radio=1 wifi=restart");
      } else if (strncmp(g_console_line, "benchauto ", 10) == 0) {
        unsigned duration_ms = 0U;
        if (sscanf(g_console_line + 10, "%u", &duration_ms) == 1 && duration_ms >= 1000U) {
          (void)beginBenchSweep((uint32_t)duration_ms);
        } else {
          Serial.println("AIRBENCH usage: benchauto <duration_ms>");
        }
      } else if (strcmp(g_console_line, "benchauto") == 0) {
        (void)beginBenchSweep(15000U);
      } else if (strcmp(g_console_line, "kickteensy") == 0 || strcmp(g_console_line, "kickstream") == 0 ||
                 strcmp(g_console_line, "resendrate") == 0) {
        const AppConfig& cfg = config_store::get();
        const bool ok = sendConfiguredStreamRateNow();
        Serial.printf("KICKTEENSY tx_ok=%u ws_hz=%u log_hz=%u\n",
                      ok ? 1U : 0U,
                      (unsigned)cfg.source_rate_hz,
                      (unsigned)cfg.log_rate_hz);
      } else if (strcmp(g_console_line, "tx1") == 0 || strcmp(g_console_line, "sendstate") == 0) {
        if (isStandaloneBench()) {
          Serial.println("TX1 tx_ok=0 reason=bench_mode");
          continue;
        }
        const auto snap = teensy_link::snapshot();
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
        if (isStandaloneBench()) {
          Serial.println("LINKCLEAR skipped reason=bench_mode");
          continue;
        }
        radio_link::resetNetworkState();
        resetWifiStatusFlags();
        Serial.println("LINKCLEAR done state=cleared link=stopped");
      } else if (strcmp(g_console_line, "linkopen") == 0) {
        if (isStandaloneBench()) {
          Serial.println("LINKOPEN skipped reason=bench_mode");
          continue;
        }
        const AppConfig& cfg = config_store::get();
        radio_link::reconfigure(cfg);
        Serial.printf("LINKOPEN peer=%s channel=%u\n",
                      radio_link::peerMac().c_str(),
                      (unsigned)telem::kRadioChannel);
      } else if (strcmp(g_console_line, "wifidrop") == 0) {
        if (isStandaloneBench()) {
          Serial.println("WIFIDROP skipped reason=bench_mode");
          continue;
        }
        const AppConfig& cfg = config_store::get();
        radio_link::resetNetworkState();
        resetWifiStatusFlags();
        radio_link::reconfigure(cfg);
        Serial.println("WIFIDROP peer_state_cleared");
      } else if (strcmp(g_console_line, "wifioffon") == 0) {
        if (isStandaloneBench()) {
          Serial.println("WIFIOFFON skipped reason=bench_mode");
          continue;
        }
        restartWifiStation();
        Serial.println("WIFIOFFON radio_power_cycle");
      } else if (strcmp(g_console_line, "relink") == 0) {
        if (isStandaloneBench()) {
          Serial.println("RELINK skipped reason=bench_mode");
          continue;
        }
        const AppConfig& cfg = config_store::get();
        radio_link::resetNetworkState();
        radio_link::reconfigure(cfg);
        resetWifiStatusFlags();
        Serial.printf("RELINK peer=%s channel=%u\n",
                      radio_link::peerMac().c_str(),
                      (unsigned)telem::kRadioChannel);
      } else if (strcmp(g_console_line, "resetnet") == 0 || strcmp(g_console_line, "netreset") == 0) {
        if (isStandaloneBench()) {
          Serial.println("RESETNET skipped reason=bench_mode");
          continue;
        }
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
          teensy_api::CommandAckResult result = {};
          (void)teensy_api::setFusionSettings(cmd, result);
          Serial.printf("SETFUSION tx_ok=%u ack_seen=%u ack_ok=%u code=%lu gain=%.3f accRej=%.2f magRej=%.2f rec=%u\n",
                        result.tx_ok ? 1U : 0U,
                        result.ack_seen ? 1U : 0U,
                        result.ack_ok ? 1U : 0U,
                        (unsigned long)result.ack_code,
                        (double)cmd.gain,
                        (double)cmd.accelerationRejection,
                        (double)cmd.magneticRejection,
                        (unsigned)cmd.recoveryTriggerPeriod);
        } else {
          Serial.println("SETFUSION usage: setfusion <gain> <accelRej> <magRej> <recovery>");
        }
      } else if (strcmp(g_console_line, "quiet on") == 0) {
        g_quiet_serial = true;
        g_stats_streaming = false;
        radio_link::setVerbose(false);
        Serial.println("QUIET on");
      } else if (strcmp(g_console_line, "quiet off") == 0) {
        g_quiet_serial = false;
        radio_link::setVerbose(true);
        Serial.println("QUIET off");
      } else if (strcmp(g_console_line, "quiet status") == 0) {
        Serial.printf("QUIET enabled=%u airtx=%u stats=%u\r\n",
                      g_quiet_serial ? 1U : 0U,
                      radio_link::verbose() ? 1U : 0U,
                      g_stats_streaming ? 1U : 0U);
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
  if (isStandaloneBench()) return;
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
  if (isStandaloneBench()) return;
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
  if (isStandaloneBench()) return;
  if (!radio_link::radioReady()) {
    if (g_radio_ready) {
      if (serialNoiseEnabled()) Serial.println("AIR WAIT radio");
      g_radio_ready = false;
    }
    g_link_ready = false;
    g_link_wait_printed = false;
    return;
  }

  if (!g_radio_ready) {
    if (serialNoiseEnabled()) {
      Serial.printf("AIR READY radio channel=%u\n", (unsigned)telem::kRadioChannel);
    }
    g_radio_ready = true;
  }

  if (radio_link::hasPeer()) {
    if (!g_link_ready) {
      if (serialNoiseEnabled()) {
        Serial.printf("AIR READY gnd_link peer=%s\n", radio_link::peerMac().c_str());
      }
      g_link_ready = true;
      g_link_wait_printed = false;
    }
    return;
  }

  if (g_link_ready) {
    if (serialNoiseEnabled()) Serial.println("AIR WAIT gnd_link peer=discovery");
    g_link_ready = false;
    g_link_wait_printed = true;
    return;
  }

  if (!g_link_wait_printed && serialNoiseEnabled()) {
    Serial.println("AIR WAIT gnd_link peer=discovery");
    g_link_wait_printed = true;
  }
}

void updateTeensyReadiness(const teensy_link::Snapshot& snap) {
  const uint32_t now = millis();
  const bool fresh = snap.stats.last_rx_ms != 0U && (uint32_t)(now - snap.stats.last_rx_ms) <= 3000U;

  if (fresh) {
    if (!g_teensy_ready) {
      if (serialNoiseEnabled()) {
        Serial.printf("AIR READY teensy_link seq=%lu t_us=%lu\n",
                      (unsigned long)snap.seq,
                      (unsigned long)snap.t_us);
      }
      g_teensy_ready = true;
      g_teensy_wait_printed = false;
    }
    return;
  }

  if (g_teensy_ready) {
    if (serialNoiseEnabled()) {
      Serial.printf("AIR WAIT teensy_link telemetry timeout_ms=%lu\n",
                    (unsigned long)(snap.stats.last_rx_ms ? (now - snap.stats.last_rx_ms) : 0U));
    }
    teensy_link::resync();
    if (serialNoiseEnabled()) {
      Serial.printf("AIR WARN teensy_link_resync rx_bytes=%lu ok=%lu cobs=%lu len=%lu unk=%lu\n",
                    (unsigned long)snap.stats.rx_bytes,
                    (unsigned long)snap.stats.frames_ok,
                    (unsigned long)snap.stats.cobs_err,
                    (unsigned long)snap.stats.len_err,
                    (unsigned long)snap.stats.unknown_msg);
    }
    g_teensy_ready = false;
    g_wait_stream_rate_ack = true;
    g_last_stream_rate_tx_ms = 0;
    return;
  }

  if (!g_teensy_wait_printed && serialNoiseEnabled()) {
    Serial.println("AIR WAIT teensy_link telemetry");
    g_teensy_wait_printed = true;
  }
}

void maybeRecoverRadioLink(const teensy_link::Snapshot& snap) {
  if (isStandaloneBench()) return;
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

  if (serialNoiseEnabled()) {
    Serial.printf("AIR WARN radio_watchdog restart idle_ms=%lu tx=%lu rx=%lu peer=%s\n",
                  (unsigned long)(now - g_last_radio_progress_ms),
                  (unsigned long)link.tx_packets,
                  (unsigned long)link.rx_packets,
                  radio_link::peerMac().c_str());
  }
  g_last_radio_recovery_ms = now;
  if (kEnableRadioWatchdogRestart) {
    restartWifiStation();
  } else {
    if (serialNoiseEnabled()) {
      Serial.println("AIR INFO radio_watchdog auto_restart=disabled");
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("ESP_AIR boot");

  config_store::begin();
  const AppConfig& cfg = config_store::get();
  Serial.printf("TEENSY LINK cfg(legacy_uart_fields) port=%u rx=%u tx=%u baud=%lu\n",
                (unsigned)cfg.uart_port,
                (unsigned)cfg.uart_rx_pin,
                (unsigned)cfg.uart_tx_pin,
                (unsigned long)cfg.uart_baud);

  beginWifiStation();

  const bool air_file_logging_enabled = kEnableAirFileLogging;

  teensy_link::begin(cfg);
  if (!isStandaloneBench()) {
    radio_link::begin(cfg);
  }
  replay_bridge::begin();
  log_store::begin(cfg, air_file_logging_enabled);
  g_console_log_session_id = log_store::highestLogSessionId();
  if (!isStandaloneBench()) {
    radio_link::setRecorderEnabled(air_file_logging_enabled);
  }
  Serial.printf("AIR INFO recorder=%s\n", air_file_logging_enabled ? "on" : "off");
  Serial.printf("AIR INFO next_session=%lu\n", (unsigned long)(g_console_log_session_id + 1U));
  if (isStandaloneBench()) {
    Serial.println("AIR INFO mode=standalone_bench spi_dma=1 radio=0");
  }
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
  serviceSdSoakBenchmark();
  serviceSdReadWriteSoak();
  serviceSdReadWriteLogSoak();
  static bool s_idle_media_checks_enabled = true;
  const bool mixed_sd_io_active =
      g_replay_capture.active || replay_bridge::active() || g_sd_rw_soak.active || g_sd_rw_log_soak.active;
  const bool idle_media_checks_enabled = !mixed_sd_io_active;
  if (idle_media_checks_enabled != s_idle_media_checks_enabled) {
    log_store::setIdleMediaChecksEnabled(idle_media_checks_enabled);
    s_idle_media_checks_enabled = idle_media_checks_enabled;
  }
  log_store::poll();
  serviceReplayCapture();
  serviceReplayCompare();
  serviceBenchSweep();
  if (g_baseline_active &&
      (uint32_t)(millis() - g_baseline_started_ms) >= g_baseline_duration_ms) {
    stopBaselineBenchmark();
  }
  if (!isStandaloneBench() && radio_link::takeNetworkResetRequest()) {
    restartWifiStation();
  }
  if (!isStandaloneBench()) {
    radio_link::poll();
  }
  replay_bridge::poll();
  updateWifiReadiness();

  const auto snap = teensy_link::snapshot();
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

  if (isStandaloneBench()) {
    ensureConfiguredCaptureRate();
  } else {
    ensureConfiguredStreamRate();
    radio_link::publish(snap);
  }
  maybeRecoverRadioLink(snap);

  if (!g_baseline_active && g_baseline_completed) {
    printBaselineImpactReport();
    g_baseline_completed = false;
  }

  const uint32_t now = millis();
  if (!g_quiet_serial && g_stats_streaming && (uint32_t)(now - g_last_stat_ms) >= 1000U) {
    g_last_stat_ms = now;
    printStats(snap);
  }
}




