#include "ws_server.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <math.h>

#include "config_store.h"
#include "radio_link.h"
#include "types_shared.h"

namespace ws_server {
namespace {

AsyncWebServer g_server(80);
AsyncWebSocket g_ws("/ws");

constexpr uint16_t kSnapshotRateHz = 30U;
constexpr uint32_t kSnapshotPeriodMs = 1000U / kSnapshotRateHz;
constexpr uint32_t kSnapshotFreshMs = 300U;
constexpr uint32_t kAckTimeoutMs = 2000U;
constexpr uint32_t kFilesTimeoutMs = 15000U;
constexpr uint32_t kStoragePollMs = 2000U;
constexpr bool kAutoWsLogsEnabled = false;
constexpr uint32_t kSyntheticSourcePeriodMs = 33U;
constexpr int32_t kSyntheticBaseLat1e7 = 372282000;
constexpr int32_t kSyntheticBaseLon1e7 = -1218882700;

enum class PendingOp : uint8_t {
  None = 0,
  StartRecord,
  StopRecord,
  RefreshFiles,
  DeleteFile,
  ExportCsv,
  SetFusion,
};

enum class PendingPhase : uint8_t {
  None = 0,
  AwaitAck,
  AwaitApply,
  AwaitFiles,
};

struct PendingCommand {
  PendingOp op = PendingOp::None;
  PendingPhase phase = PendingPhase::None;
  uint32_t req_id = 0U;
  uint32_t started_ms = 0U;
  uint32_t deadline_ms = 0U;
  uint32_t ack_baseline = 0U;
  uint32_t expected_control_gen = 0U;
  uint32_t files_revision_baseline = 0U;
  uint16_t file_offset = 0U;
  uint16_t file_limit = 32U;
  telem::CmdSetFusionSettingsV1 fusion = {};
  char name[telem::kLogFileNameBytes] = {};
};

PendingCommand g_pending = {};
uint32_t g_ws_state_seq = 0U;
uint32_t g_last_state_broadcast_ms = 0U;
uint32_t g_last_state_seq_sent = 0U;
uint32_t g_last_source_t_us_sent = 0U;
uint32_t g_last_radio_rx_ms_seen = 0U;
uint32_t g_last_ui_tx_ms = 0U;
uint32_t g_last_ui_tx_latency_ms = 0U;
uint32_t g_max_ui_tx_latency_ms = 0U;
uint32_t g_last_files_revision_sent = 0U;
uint32_t g_last_storage_revision_sent = 0U;
uint32_t g_last_storage_poll_ms = 0U;
uint32_t g_http_root_gets = 0U;
uint32_t g_http_asset_gets = 0U;
uint32_t g_http_not_found = 0U;
uint32_t g_ws_connects = 0U;
uint32_t g_ws_disconnects = 0U;
uint32_t g_ws_snapshots_sent = 0U;
uint32_t g_ws_live_updates_sent = 0U;
uint32_t g_ws_send_failures = 0U;
uint32_t g_ws_control_rx = 0U;
uint32_t g_ws_control_tx_air = 0U;
uint32_t g_ws_control_ack = 0U;
uint32_t g_ws_control_tx = 0U;
uint32_t g_last_ws_connect_ms = 0U;
uint32_t g_last_ws_send_ms = 0U;
uint32_t g_last_ws_live_log_ms = 0U;
uint32_t g_last_ws_backpressure_log_ms = 0U;
uint32_t g_ws_sum_count = 0U;
uint32_t g_ws_sum_last_seq = 0U;
uint32_t g_ws_sum_last_age_ms = 0U;
bool g_synthetic_live_mode = false;
uint32_t g_synthetic_live_seq = 0U;

float triangleWaveDegrees(uint32_t now_ms, uint32_t period_ms, float amplitude, uint32_t phase_ms = 0U) {
  if (period_ms == 0U) return 0.0f;
  const uint32_t phase = (now_ms + phase_ms) % period_ms;
  const float norm = (float)phase / (float)period_ms;
  const float tri = norm < 0.5f ? (-1.0f + 4.0f * norm) : (3.0f - 4.0f * norm);
  return tri * amplitude;
}

int32_t triangleWaveE7(uint32_t now_ms, uint32_t period_ms, int32_t amplitude_e7, uint32_t phase_ms = 0U) {
  return (int32_t)lroundf(triangleWaveDegrees(now_ms, period_ms, (float)amplitude_e7, phase_ms));
}

void fillSyntheticState(uint32_t now_ms, telem::TelemetryStateRecord& state, uint32_t& seq, uint32_t& t_us) {
  seq = ++g_synthetic_live_seq;
  t_us = now_ms * 1000U;
  state = {};
  const float roll_deg = triangleWaveDegrees(now_ms, 4000U, 45.0f);
  const float pitch_deg = triangleWaveDegrees(now_ms, 7000U, 10.0f, 1200U);
  const float yaw_deg = (float)((now_ms % 12000U) * (360.0 / 12000.0));
  const float altitude_m = 120.0f + triangleWaveDegrees(now_ms, 9000U, 25.0f, 800U);
  const float vsi_mps = triangleWaveDegrees(now_ms, 3000U, 4.0f, 300U);

  state.roll_deg = roll_deg;
  state.pitch_deg = pitch_deg;
  state.yaw_deg = yaw_deg;
  state.mag_heading_deg = yaw_deg;
  state.iTOW_ms = now_ms;
  state.fixType = 3U;
  state.numSV = 12U;
  state.lat_1e7 = kSyntheticBaseLat1e7 + triangleWaveE7(now_ms, 8000U, 2200, 0U);
  state.lon_1e7 = kSyntheticBaseLon1e7 + triangleWaveE7(now_ms, 10000U, 3200, 1500U);
  state.hMSL_mm = (int32_t)lroundf(altitude_m * 1000.0f);
  state.gSpeed_mms = 18000;
  state.headMot_1e5deg = (int32_t)lroundf(yaw_deg * 100000.0f);
  state.hAcc_mm = 1200U;
  state.sAcc_mms = 600U;
  state.last_gps_ms = now_ms;
  state.last_imu_ms = now_ms;
  state.last_baro_ms = now_ms;
  state.baro_alt_m = altitude_m;
  state.baro_vsi_mps = vsi_mps;
  state.fusion_gain = 0.50f;
  state.fusion_accel_rej = 10.0f;
  state.fusion_mag_rej = 10.0f;
  state.fusion_recovery_period = 400U;
  state.flags = telem::kStateFlagGpsFix3d;
  state.raw_present_mask = (uint16_t)(telem::kSensorPresentImu | telem::kSensorPresentGps | telem::kSensorPresentBaro);
#if TELEM_ACTIVE_SCHEMA_ID == TELEM_SCHEMA_ID_RELEASE_0_03_CANDIDATE
  state.gps_year = 2026U;
  state.gps_month = 4U;
  state.gps_day = 18U;
  state.gps_hour = 12U;
  state.gps_min = (uint8_t)((now_ms / 60000U) % 60U);
  state.gps_sec = (uint8_t)((now_ms / 1000U) % 60U);
#endif
}

const char* sendStatusText(AsyncWebSocket::SendStatus status) {
  switch (status) {
    case AsyncWebSocket::ENQUEUED: return "enqueued";
    case AsyncWebSocket::PARTIALLY_ENQUEUED: return "partial";
    case AsyncWebSocket::DISCARDED:
    default:
      return "discarded";
  }
}

void logHttpRequest(const char* kind, AsyncWebServerRequest* request, const char* path) {
  if (!kAutoWsLogsEnabled) return;
  const String ip = request ? request->client()->remoteIP().toString() : String("-");
  Serial.printf("WEB %s path=%s from=%s\n",
                kind ? kind : "http_get",
                path ? path : "-",
                ip.c_str());
}

void serveLoggedFile(AsyncWebServerRequest* request, const char* path, const char* content_type, bool root_request) {
  if (root_request) {
    g_http_root_gets++;
    logHttpRequest("http_get", request, path);
  } else {
    g_http_asset_gets++;
    logHttpRequest("http_asset", request, path);
  }
  AsyncWebServerResponse* response = request->beginResponse(LittleFS, path, content_type);
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  request->send(response);
}

const char* opText(PendingOp op) {
  switch (op) {
    case PendingOp::StartRecord: return "start_record";
    case PendingOp::StopRecord: return "stop_record";
    case PendingOp::RefreshFiles: return "files_refresh";
    case PendingOp::DeleteFile: return "file_delete";
    case PendingOp::ExportCsv: return "file_export_csv";
    case PendingOp::SetFusion: return "fusion_set";
    default: return "unknown";
  }
}

bool isSdFileOp(PendingOp op) {
  return op == PendingOp::RefreshFiles || op == PendingOp::DeleteFile || op == PendingOp::ExportCsv;
}

const char* rejectDetailText(PendingOp op, uint32_t code) {
  if (isSdFileOp(op)) return telem::sdApiStatusText(code);
  return "rejected";
}

uint16_t expectedAckCommand(PendingOp op) {
  switch (op) {
    case PendingOp::StartRecord: return telem::CMD_LOG_START;
    case PendingOp::StopRecord: return telem::CMD_LOG_STOP;
    case PendingOp::DeleteFile: return telem::CMD_DELETE_LOG_FILE;
    case PendingOp::ExportCsv: return telem::CMD_EXPORT_LOG_CSV;
    case PendingOp::SetFusion: return telem::CMD_SET_FUSION_SETTINGS;
    default: return 0U;
  }
}

AsyncWebSocket::SendStatus sendJson(const JsonDocument& doc) {
  String text;
  serializeJson(doc, text);
  const AsyncWebSocket::SendStatus status = g_ws.textAll(text);
  g_last_ws_send_ms = millis();
  if (status != AsyncWebSocket::ENQUEUED) {
    g_ws_send_failures++;
  }
  return status;
}

void sendAck(PendingOp op, uint32_t req_id, bool ok, uint32_t code, const char* detail = "") {
  JsonDocument doc;
  doc["type"] = "ack";
  doc["op"] = opText(op);
  doc["req_id"] = req_id;
  doc["ok"] = ok;
  doc["code"] = code;
  if (detail && detail[0] != '\0') doc["detail"] = detail;
  const AsyncWebSocket::SendStatus status = sendJson(doc);
  g_ws_control_tx++;
  if (kAutoWsLogsEnabled) {
    Serial.printf("CTRL tx_to_ws op=%s req_id=%lu ok=%u code=%lu send=%s\n",
                  opText(op),
                  (unsigned long)req_id,
                  ok ? 1U : 0U,
                  (unsigned long)code,
                  sendStatusText(status));
  }
}

void sendHello(AsyncWebSocketClient* client) {
  JsonDocument doc;
  doc["type"] = "hello";
  doc["snapshot_hz"] = kSnapshotRateHz;
  JsonArray controls = doc["controls"].to<JsonArray>();
  JsonObject recording = controls.add<JsonObject>();
  recording["category"] = "recording";
  recording["actions"] = "start,stop";
  JsonObject file = controls.add<JsonObject>();
  file["category"] = "file";
  file["actions"] = "refresh,delete,export_csv";
  JsonObject fusion = controls.add<JsonObject>();
  fusion["category"] = "fusion";
  fusion["actions"] = "set";
  String text;
  serializeJson(doc, text);
  if (client) {
    const bool ok = client->text(text);
    g_last_ws_send_ms = millis();
    if (!ok) g_ws_send_failures++;
    if (kAutoWsLogsEnabled) {
      Serial.printf("WEB ws_hello client=%u ip=%s ok=%u bytes=%u\n",
                    client->id(),
                    client->remoteIP().toString().c_str(),
                    ok ? 1U : 0U,
                    (unsigned)text.length());
    }
  }
}

void sendFiles(uint32_t req_id = 0U) {
  JsonDocument doc;
  const String payload = radio_link::remoteFilesJson(false);
  if (deserializeJson(doc, payload)) return;
  doc["type"] = "files";
  if (req_id != 0U) doc["req_id"] = req_id;
  const AsyncWebSocket::SendStatus status = sendJson(doc);
  if (kAutoWsLogsEnabled) {
    Serial.printf("WEB ws_files req_id=%lu send=%s\n",
                  (unsigned long)req_id,
                  sendStatusText(status));
  }
  g_last_files_revision_sent = radio_link::remoteFilesStatus().revision;
}

void sendStorage(uint32_t req_id = 0U) {
  JsonDocument doc;
  const String payload = radio_link::remoteStorageJson(false);
  if (deserializeJson(doc, payload)) return;
  doc["type"] = "storage";
  if (req_id != 0U) doc["req_id"] = req_id;
  const AsyncWebSocket::SendStatus status = sendJson(doc);
  if (kAutoWsLogsEnabled) {
    Serial.printf("WEB ws_storage req_id=%lu send=%s\n",
                  (unsigned long)req_id,
                  sendStatusText(status));
  }
  g_last_storage_revision_sent = radio_link::remoteStorageStatus().revision;
}

void updateTxStats(const radio_link::Snapshot& snap, uint32_t now_ms) {
  const bool use_synth = g_synthetic_live_mode;
  const uint32_t freshness_ms =
      use_synth ? now_ms : (snap.stats.last_state_apply_ms != 0U ? snap.stats.last_state_apply_ms : snap.stats.last_rx_ms);
  g_last_state_seq_sent = use_synth ? g_synthetic_live_seq : snap.seq;
  g_last_source_t_us_sent = use_synth ? (now_ms * 1000U) : snap.t_us;
  g_last_radio_rx_ms_seen = freshness_ms;
  g_last_ui_tx_ms = now_ms;
  g_last_ui_tx_latency_ms =
      freshness_ms != 0U ? (uint32_t)(now_ms - freshness_ms) : 0U;
  if (g_last_ui_tx_latency_ms > g_max_ui_tx_latency_ms) {
    g_max_ui_tx_latency_ms = g_last_ui_tx_latency_ms;
  }
}

void fillSnapshotDoc(JsonDocument& doc, const radio_link::Snapshot& snap, uint32_t now_ms) {
  telem::TelemetryStateRecord state = snap.state;
  uint32_t seq = snap.seq;
  uint32_t source_t_us = snap.t_us;
  uint32_t freshness_ms =
      snap.stats.last_state_apply_ms != 0U ? snap.stats.last_state_apply_ms : snap.stats.last_rx_ms;
  if (g_synthetic_live_mode) {
    fillSyntheticState(now_ms, state, seq, source_t_us);
    freshness_ms = now_ms;
  }

  const telem::GpsCalendarTime gps_time = telem::gpsCalendarTime(state);
  const bool gps_calendar_valid =
      gps_time.year != 0U && gps_time.month != 0U && gps_time.day != 0U;
  uint32_t replay_source_seq = 0U;
  uint32_t replay_source_t_us = 0U;
  telem::decodeReplaySourceStamp(state, replay_source_seq, replay_source_t_us);

  doc["type"] = "snapshot";
  doc["schema_id"] = telem::kActiveSchema.schema_id;
  doc["schema_version"] = telem::kActiveSchema.schema_version;
  doc["ws_seq"] = ++g_ws_state_seq;
  doc["seq"] = seq;
  doc["state_seq"] = seq;
  doc["source_t_us"] = source_t_us;
  doc["replay_source_seq"] = replay_source_seq;
  doc["replay_source_t_us"] = replay_source_t_us;
  doc["fresh"] = freshness_ms != 0U && (uint32_t)(now_ms - freshness_ms) <= kSnapshotFreshMs;
  doc["age_ms"] = freshness_ms != 0U ? (uint32_t)(now_ms - freshness_ms) : 0xFFFFFFFFUL;
  doc["radio_rtt_ms"] = snap.radio_rtt_ms;
  doc["drop"] = snap.stats.drop;
  doc["len_err"] = snap.stats.len_err;
  doc["unknown_msg"] = snap.stats.unknown_msg;
  doc["state_gap"] = snap.stats.state_seq_gap;
  doc["state_rewind"] = snap.stats.state_seq_rewind;
  doc["recording_active"] = (snap.log_status.flags & telem::kLogStatusFlagActive) != 0U;
  doc["recording_busy"] = (snap.log_status.flags & telem::kLogStatusFlagBusy) != 0U;
  doc["recording_session_id"] = snap.log_status.session_id;
  doc["recording_bytes_written"] = snap.log_status.bytes_written;
  doc["replay_active"] = (snap.replay_status.flags & telem::kReplayStatusFlagActive) != 0U;
  doc["replay_paused"] = (snap.replay_status.flags & telem::kReplayStatusFlagPaused) != 0U;
  doc["replay_file_open"] = (snap.replay_status.flags & telem::kReplayStatusFlagFileOpen) != 0U;
  doc["replay_at_eof"] = (snap.replay_status.flags & telem::kReplayStatusFlagAtEof) != 0U;
  doc["replay_teensy_seen"] = (snap.replay_status.flags & telem::kReplayStatusFlagTeensyReplaySeen) != 0U;
  doc["replay_session_id"] = snap.replay_status.session_id;
  doc["replay_records_sent"] = snap.replay_status.records_sent;
  doc["replay_records_total"] = snap.replay_status.records_total;
  doc["replay_last_error"] = snap.replay_status.last_error;
  doc["replay_last_command"] = snap.replay_status.last_command;
  doc["replay_current_file"] = snap.replay_status.current_file;
  doc["roll_deg"] = state.roll_deg;
  doc["pitch_deg"] = state.pitch_deg;
  doc["yaw_deg"] = state.yaw_deg;
  doc["mag_heading_deg"] = state.mag_heading_deg;
  doc["lat_1e7"] = state.lat_1e7;
  doc["lon_1e7"] = state.lon_1e7;
  doc["hMSL_mm"] = state.hMSL_mm;
  doc["gSpeed_mms"] = state.gSpeed_mms;
  doc["headMot_1e5deg"] = state.headMot_1e5deg;
  doc["gps_itow_ms"] = state.iTOW_ms;
  doc["gps_fix_type"] = state.fixType;
  doc["gps_num_sv"] = state.numSV;
  doc["hAcc_mm"] = state.hAcc_mm;
  doc["sAcc_mms"] = state.sAcc_mms;
  doc["baro_temp_c"] = state.baro_temp_c;
  doc["baro_press_hpa"] = state.baro_press_hpa;
  doc["baro_alt_m"] = state.baro_alt_m;
  doc["baro_vsi_mps"] = state.baro_vsi_mps;
  doc["fusion_gain"] = state.fusion_gain;
  doc["fusion_accel_rej"] = state.fusion_accel_rej;
  doc["fusion_mag_rej"] = state.fusion_mag_rej;
  doc["fusion_recovery_period"] = state.fusion_recovery_period;
  doc["desired_control_gen"] = snap.desired_control_gen;
  doc["applied_control_gen"] = snap.applied_control_gen;
  doc["control_code"] = snap.control_code;
  doc["flags"] = state.flags;
  doc["raw_present_mask"] = state.raw_present_mask;
  doc["gps_calendar_valid"] = gps_calendar_valid;
  doc["gps_year"] = gps_time.year;
  doc["gps_month"] = gps_time.month;
  doc["gps_day"] = gps_time.day;
  doc["gps_hour"] = gps_time.hour;
  doc["gps_min"] = gps_time.minute;
  doc["gps_sec"] = gps_time.second;
}

void logWsTrace(const JsonDocument& doc) {
  Serial.printf("WSTRACE ws_seq=%lu state_seq=%lu roll=%.2f pitch=%.2f yaw=%.2f lat=%ld lon=%ld\r\n",
                (unsigned long)(doc["ws_seq"] | 0U),
                (unsigned long)(doc["state_seq"] | 0U),
                (double)(doc["roll_deg"] | 0.0),
                (double)(doc["pitch_deg"] | 0.0),
                (double)(doc["yaw_deg"] | 0.0),
                (long)(doc["lat_1e7"] | 0L),
                (long)(doc["lon_1e7"] | 0L));
}

void maybeBroadcastSnapshot() {
  if (g_ws.count() == 0U) return;
  const uint32_t now_ms = millis();
  if (g_last_state_broadcast_ms != 0U &&
      (uint32_t)(now_ms - g_last_state_broadcast_ms) < kSnapshotPeriodMs) {
    return;
  }
  if (!g_ws.availableForWriteAll()) {
    if (g_last_ws_backpressure_log_ms == 0U ||
        (uint32_t)(now_ms - g_last_ws_backpressure_log_ms) >= 1000U) {
      if (kAutoWsLogsEnabled) {
        Serial.printf("WEB ws_live backpressure clients=%u last_seq=%lu\n",
                      (unsigned)g_ws.count(),
                      (unsigned long)g_last_state_seq_sent);
      }
      g_last_ws_backpressure_log_ms = now_ms;
    }
  }

  const radio_link::Snapshot snap = radio_link::snapshot();
  if (!g_synthetic_live_mode && !snap.has_state) {
    return;
  }
  JsonDocument doc;
  fillSnapshotDoc(doc, snap, now_ms);
  logWsTrace(doc);

  updateTxStats(snap, now_ms);
  (void)sendJson(doc);
  g_ws_snapshots_sent++;
  g_ws_live_updates_sent++;
  g_ws_sum_count++;
  g_ws_sum_last_seq = doc["seq"] | 0U;
  g_ws_sum_last_age_ms = doc["age_ms"] | 0U;
  if (g_last_ws_live_log_ms == 0U || (uint32_t)(now_ms - g_last_ws_live_log_ms) >= 1000U) {
    g_ws_sum_count = 0U;
    g_last_ws_live_log_ms = now_ms;
  }
  g_last_state_broadcast_ms = now_ms;
}

void clearPending() {
  g_pending = {};
}

bool enqueuePending(const PendingCommand& next) {
  if (g_pending.op != PendingOp::None) {
    if (kAutoWsLogsEnabled) {
      Serial.printf("CTRL enqueue_busy op=%s req_id=%lu pending=%s\n",
                    opText(next.op),
                    (unsigned long)next.req_id,
                    opText(g_pending.op));
    }
    return false;
  }
  g_pending = next;
  g_pending.started_ms = millis();
  if (kAutoWsLogsEnabled) {
    Serial.printf("CTRL enqueue op=%s req_id=%lu\n",
                  opText(next.op),
                  (unsigned long)next.req_id);
  }
  return true;
}

void finalizePending(bool ok, uint32_t code, const char* detail = "") {
  if (kAutoWsLogsEnabled) {
    Serial.printf("CTRL finalize op=%s req_id=%lu ok=%u code=%lu detail=%s\n",
                  opText(g_pending.op),
                  (unsigned long)g_pending.req_id,
                  ok ? 1U : 0U,
                  (unsigned long)code,
                  detail ? detail : "");
  }
  sendAck(g_pending.op, g_pending.req_id, ok, code, detail);
  clearPending();
}

void processPending() {
  if (g_pending.op == PendingOp::None) return;

  const uint32_t now_ms = millis();
  if (g_pending.phase == PendingPhase::None) {
    const radio_link::Snapshot snap = radio_link::snapshot();
    g_pending.ack_baseline = snap.ack_rx_seq;
    g_pending.files_revision_baseline = radio_link::remoteFilesStatus().revision;

    bool tx_ok = false;
    switch (g_pending.op) {
      case PendingOp::StartRecord:
        tx_ok = radio_link::sendLogStart();
        (void)radio_link::sendGetStorageStatus();
        g_pending.phase = PendingPhase::AwaitAck;
        g_pending.deadline_ms = now_ms + kAckTimeoutMs;
        break;
      case PendingOp::StopRecord:
        tx_ok = radio_link::sendLogStop();
        (void)radio_link::sendGetStorageStatus();
        g_pending.phase = PendingPhase::AwaitAck;
        g_pending.deadline_ms = now_ms + kAckTimeoutMs;
        break;
      case PendingOp::RefreshFiles:
        tx_ok = radio_link::sendGetLogFileList(g_pending.file_offset, g_pending.file_limit);
        g_pending.phase = PendingPhase::AwaitFiles;
        g_pending.deadline_ms = now_ms + kFilesTimeoutMs;
        break;
      case PendingOp::DeleteFile:
        tx_ok = radio_link::sendDeleteLogFile(String(g_pending.name));
        (void)radio_link::sendGetStorageStatus();
        g_pending.phase = PendingPhase::AwaitAck;
        g_pending.deadline_ms = now_ms + kAckTimeoutMs;
        break;
      case PendingOp::ExportCsv:
        tx_ok = radio_link::sendExportLogCsv(String(g_pending.name));
        (void)radio_link::sendGetStorageStatus();
        g_pending.phase = PendingPhase::AwaitAck;
        g_pending.deadline_ms = now_ms + kAckTimeoutMs;
        break;
      case PendingOp::SetFusion:
        tx_ok = radio_link::sendSetFusionSettings(g_pending.fusion);
        g_pending.expected_control_gen = radio_link::desiredControlGen();
        g_pending.phase = PendingPhase::AwaitApply;
        g_pending.deadline_ms = now_ms + kAckTimeoutMs;
        break;
      default:
        tx_ok = false;
        break;
    }

    if (!tx_ok) {
      finalizePending(false, 1U, "tx_failed");
    } else {
      g_ws_control_tx_air++;
      if (kAutoWsLogsEnabled) {
        Serial.printf("CTRL tx_to_air op=%s req_id=%lu ok=1\n",
                      opText(g_pending.op),
                      (unsigned long)g_pending.req_id);
      }
    }
    return;
  }

  if (g_pending.phase == PendingPhase::AwaitAck) {
    const radio_link::Snapshot snap = radio_link::snapshot();
    const uint16_t expected = expectedAckCommand(g_pending.op);
    if (snap.has_ack &&
        snap.ack_command == expected &&
        snap.ack_rx_seq != 0U &&
        snap.ack_rx_seq != g_pending.ack_baseline) {
      g_ws_control_ack++;
      if (kAutoWsLogsEnabled) {
        Serial.printf("CTRL ack_from_air op=%s req_id=%lu ok=%u code=%lu\n",
                      opText(g_pending.op),
                      (unsigned long)g_pending.req_id,
                      snap.ack_ok ? 1U : 0U,
                      (unsigned long)snap.ack_code);
      }
      if (g_pending.op == PendingOp::SetFusion && snap.ack_ok) {
        (void)radio_link::sendGetFusionSettings();
      }
      if (g_pending.op == PendingOp::DeleteFile && snap.ack_ok) {
        g_pending.phase = PendingPhase::AwaitFiles;
        g_pending.files_revision_baseline = radio_link::remoteFilesStatus().revision;
        g_pending.deadline_ms = now_ms + kFilesTimeoutMs;
        if (!radio_link::sendGetLogFileList()) {
          finalizePending(false, 2U, "refresh_failed");
        }
        return;
      }
      if (g_pending.op == PendingOp::StartRecord || g_pending.op == PendingOp::StopRecord ||
          g_pending.op == PendingOp::ExportCsv) {
        (void)radio_link::sendGetStorageStatus();
        sendStorage(g_pending.req_id);
      }
      finalizePending(snap.ack_ok,
                      snap.ack_code,
                      snap.ack_ok ? "applied" : rejectDetailText(g_pending.op, snap.ack_code));
      return;
    }
    if ((int32_t)(now_ms - g_pending.deadline_ms) >= 0) {
      finalizePending(false, 3U, "timeout");
    }
    return;
  }

  if (g_pending.phase == PendingPhase::AwaitApply) {
    const radio_link::Snapshot snap = radio_link::snapshot();
    if (snap.applied_control_gen != 0U && snap.applied_control_gen >= g_pending.expected_control_gen) {
      g_ws_control_ack++;
      if (kAutoWsLogsEnabled) {
        Serial.printf("CTRL apply_from_air op=%s req_id=%lu gen=%lu code=%lu\n",
                      opText(g_pending.op),
                      (unsigned long)g_pending.req_id,
                      (unsigned long)snap.applied_control_gen,
                      (unsigned long)snap.control_code);
      }
      finalizePending(snap.control_code == 0U,
                      snap.control_code,
                      snap.control_code == 0U ? "applied" : rejectDetailText(g_pending.op, snap.control_code));
      return;
    }
    if ((int32_t)(now_ms - g_pending.deadline_ms) >= 0) {
      finalizePending(false, 3U, "timeout");
    }
    return;
  }

  if (g_pending.phase == PendingPhase::AwaitFiles) {
    const radio_link::Snapshot snap = radio_link::snapshot();
    if (snap.has_ack &&
        snap.ack_command == telem::CMD_GET_LOG_FILE_LIST &&
        snap.ack_rx_seq != 0U &&
        snap.ack_rx_seq != g_pending.ack_baseline) {
      g_ws_control_ack++;
      if (kAutoWsLogsEnabled) {
        Serial.printf("CTRL ack_from_air op=%s req_id=%lu ok=%u code=%lu\n",
                      opText(g_pending.op),
                      (unsigned long)g_pending.req_id,
                      snap.ack_ok ? 1U : 0U,
                      (unsigned long)snap.ack_code);
      }
      g_pending.ack_baseline = snap.ack_rx_seq;
      if (!snap.ack_ok) {
        finalizePending(false, snap.ack_code, rejectDetailText(g_pending.op, snap.ack_code));
        return;
      }
    }
    const radio_link::RemoteFilesStatus files = radio_link::remoteFilesStatus();
    if (files.revision != g_pending.files_revision_baseline) {
      g_pending.files_revision_baseline = files.revision;
      g_pending.deadline_ms = now_ms + kFilesTimeoutMs;
      sendFiles(g_pending.req_id);
    }
    const bool refreshed =
        files.complete && !files.refresh_inflight;
    if (refreshed) {
      finalizePending(true, 0U, "files_ready");
      return;
    }
    if ((int32_t)(now_ms - g_pending.deadline_ms) >= 0) {
      sendFiles(g_pending.req_id);
      finalizePending(false, 3U, "files_timeout");
    }
  }
}

void sendSnapshot(AsyncWebSocketClient* client) {
  if (!client) return;
  const uint32_t now_ms = millis();
  const radio_link::Snapshot snap = radio_link::snapshot();
  JsonDocument doc;
  fillSnapshotDoc(doc, snap, now_ms);
  logWsTrace(doc);
  String text;
  serializeJson(doc, text);
  const bool ok = client->text(text);
  g_last_ws_send_ms = now_ms;
  if (!ok) g_ws_send_failures++;
  updateTxStats(snap, now_ms);
  g_ws_snapshots_sent++;
  g_ws_live_updates_sent++;
  if (kAutoWsLogsEnabled) {
    Serial.printf("WEB ws_snapshot client=%u ok=%u bytes=%u seq=%lu\n",
                  client->id(),
                  ok ? 1U : 0U,
                  (unsigned)text.length(),
                  (unsigned long)snap.seq);
  }
}

void handleWsMessage(const char* text, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, text, len)) {
    if (kAutoWsLogsEnabled) Serial.println("WEB ws_parse_error");
    return;
  }

  const char* type = doc["type"] | "";
  const uint32_t req_id = doc["req_id"] | 0U;

  if (strcmp(type, "control") == 0) {
    g_ws_control_rx++;
    const char* category = doc["category"] | "";
    const char* action = doc["action"] | "";
    if (kAutoWsLogsEnabled) {
      Serial.printf("CTRL rx_from_ws req_id=%lu category=%s action=%s\n",
                    (unsigned long)req_id,
                    category,
                    action);
    }

    if (strcmp(category, "recording") == 0) {
      PendingCommand next = {};
      if (strcmp(action, "start") == 0) {
        next.op = PendingOp::StartRecord;
      } else if (strcmp(action, "stop") == 0) {
        next.op = PendingOp::StopRecord;
      } else {
        sendAck(PendingOp::None, req_id, false, 4U, "unsupported_action");
        return;
      }
      next.req_id = req_id;
      if (!enqueuePending(next)) sendAck(next.op, req_id, false, 9U, "busy");
      return;
    }

    if (strcmp(category, "file") == 0) {
      PendingCommand next = {};
      if (strcmp(action, "refresh") == 0) {
        next.op = PendingOp::RefreshFiles;
        next.file_offset = doc["offset"] | 0U;
        next.file_limit = doc["limit"] | 32U;
      } else if (strcmp(action, "delete") == 0) {
        const char* name = doc["name"] | "";
        if (!name[0]) {
          sendAck(PendingOp::DeleteFile, req_id, false, 2U, "missing_name");
          return;
        }
        next.op = PendingOp::DeleteFile;
        strncpy(next.name, name, sizeof(next.name) - 1U);
      } else if (strcmp(action, "export_csv") == 0) {
        const char* name = doc["name"] | "";
        if (!name[0]) {
          sendAck(PendingOp::ExportCsv, req_id, false, 2U, "missing_name");
          return;
        }
        next.op = PendingOp::ExportCsv;
        strncpy(next.name, name, sizeof(next.name) - 1U);
      } else {
        sendAck(PendingOp::None, req_id, false, 4U, "unsupported_action");
        return;
      }
      next.req_id = req_id;
      if (!enqueuePending(next)) sendAck(next.op, req_id, false, 9U, "busy");
      return;
    }

    if (strcmp(category, "fusion") == 0 && strcmp(action, "set") == 0) {
      if (doc["gain"].isNull() || doc["accelerationRejection"].isNull() ||
          doc["magneticRejection"].isNull() || doc["recoveryTriggerPeriod"].isNull()) {
        sendAck(PendingOp::SetFusion, req_id, false, 2U, "missing_fields");
        return;
      }
      PendingCommand next = {};
      next.op = PendingOp::SetFusion;
      next.req_id = req_id;
      next.fusion.gain = doc["gain"].as<float>();
      next.fusion.accelerationRejection = doc["accelerationRejection"].as<float>();
      next.fusion.magneticRejection = doc["magneticRejection"].as<float>();
      next.fusion.recoveryTriggerPeriod = doc["recoveryTriggerPeriod"].as<uint16_t>();
      if (!enqueuePending(next)) sendAck(PendingOp::SetFusion, req_id, false, 9U, "busy");
      return;
    }

    JsonDocument err;
    err["type"] = "ack";
    err["req_id"] = req_id;
    err["ok"] = false;
    err["code"] = 4U;
    err["detail"] = "unsupported_control";
    if (kAutoWsLogsEnabled) {
      Serial.printf("CTRL unsupported_control req_id=%lu category=%s action=%s\n",
                    (unsigned long)req_id,
                    category,
                    action);
    }
    sendJson(err);
    return;
  }

  JsonDocument err;
  err["type"] = "ack";
  err["req_id"] = req_id;
  err["ok"] = false;
  err["code"] = 4U;
  err["detail"] = "unsupported";
  if (kAutoWsLogsEnabled) {
    Serial.printf("WEB ws_unsupported_message req_id=%lu type=%s\n",
                  (unsigned long)req_id,
                  type);
  }
  sendJson(err);
}

void onWsEvent(AsyncWebSocket* server,
               AsyncWebSocketClient* client,
               AwsEventType type,
               void* arg,
               uint8_t* data,
               size_t len) {
  (void)server;
  if (type == WS_EVT_CONNECT) {
    g_ws_connects++;
    g_last_ws_connect_ms = millis();
    if (kAutoWsLogsEnabled) {
      Serial.printf("WEB ws_open client=%u ip=%s total=%u\n",
                    client ? client->id() : 0U,
                    client ? client->remoteIP().toString().c_str() : "-",
                    (unsigned)g_ws.count());
    }
    sendHello(client);
    sendSnapshot(client);
    sendFiles();
    sendStorage();
    (void)radio_link::sendGetStorageStatus();
    (void)radio_link::sendGetReplayStatus();
    return;
  }
  if (type == WS_EVT_DISCONNECT) {
    g_ws_disconnects++;
    if (kAutoWsLogsEnabled) {
      Serial.printf("WEB ws_close client=%u total=%u\n",
                    client ? client->id() : 0U,
                    (unsigned)g_ws.count());
    }
    return;
  }
  if (type == WS_EVT_ERROR) {
    g_ws_send_failures++;
    if (kAutoWsLogsEnabled) {
      Serial.printf("WEB ws_error client=%u total=%u\n",
                    client ? client->id() : 0U,
                    (unsigned)g_ws.count());
    }
    return;
  }
  if (type != WS_EVT_DATA || !arg || !data || len == 0U) return;

  const AwsFrameInfo* info = reinterpret_cast<const AwsFrameInfo*>(arg);
  if (!info->final || info->index != 0U || info->opcode != WS_TEXT) return;
  handleWsMessage(reinterpret_cast<const char*>(data), len);
}

}  // namespace

void begin() {
  g_ws.onEvent(onWsEvent);
  g_server.addHandler(&g_ws);
  g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveLoggedFile(request, "/index.html", "text/html", true);
  });
  g_server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveLoggedFile(request, "/style.css", "text/css", false);
  });
  g_server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveLoggedFile(request, "/app.js", "application/javascript", false);
  });
  g_server.serveStatic("/", LittleFS, "/")
      .setDefaultFile("index.html")
      .setTryGzipFirst(false)
      .setCacheControl("no-cache, no-store, must-revalidate");
  g_server.onNotFound([](AsyncWebServerRequest* request) {
    g_http_not_found++;
    logHttpRequest("http_404", request, request ? request->url().c_str() : "-");
    request->send(404, "text/plain", "Not found");
  });
  g_server.begin();
}

void loop() {
  g_ws.cleanupClients();
  processPending();
  if (g_ws.count() != 0U) {
    const radio_link::RemoteFilesStatus files = radio_link::remoteFilesStatus();
    if (files.revision != g_last_files_revision_sent) {
      sendFiles();
    }
    const radio_link::RemoteStorageStatus storage = radio_link::remoteStorageStatus();
    if (storage.revision != g_last_storage_revision_sent) {
      sendStorage();
    }
    const uint32_t now_ms = millis();
    if (g_last_storage_poll_ms == 0U ||
        (uint32_t)(now_ms - g_last_storage_poll_ms) >= kStoragePollMs) {
      (void)radio_link::sendGetStorageStatus();
      g_last_storage_poll_ms = now_ms;
    }
  }
  maybeBroadcastSnapshot();
}

uint32_t clientCount() { return g_ws.count(); }

void setSyntheticLiveMode(bool enabled) {
  g_synthetic_live_mode = enabled;
  if (enabled) {
    g_synthetic_live_seq = 0U;
  }
}

bool syntheticLiveMode() { return g_synthetic_live_mode; }

void printSyntheticLiveStatus(Stream& out) {
  out.printf("SYNTHLIVE enabled=%u seq=%lu period_ms=%lu base_lat=%ld base_lon=%ld\r\n",
             g_synthetic_live_mode ? 1U : 0U,
             (unsigned long)g_synthetic_live_seq,
             (unsigned long)kSyntheticSourcePeriodMs,
             (long)kSyntheticBaseLat1e7,
             (long)kSyntheticBaseLon1e7);
}

Stats stats() {
  Stats out = {};
  out.clients = clientCount();
  out.ws_state_seq = g_ws_state_seq;
  out.last_state_seq_sent = g_last_state_seq_sent;
  out.last_source_t_us_sent = g_last_source_t_us_sent;
  out.last_radio_rx_ms_seen = g_last_radio_rx_ms_seen;
  out.last_ui_tx_ms = g_last_ui_tx_ms;
  out.last_ui_tx_latency_ms = g_last_ui_tx_latency_ms;
  out.max_ui_tx_latency_ms = g_max_ui_tx_latency_ms;
  out.http_root_gets = g_http_root_gets;
  out.http_asset_gets = g_http_asset_gets;
  out.http_not_found = g_http_not_found;
  out.ws_connects = g_ws_connects;
  out.ws_disconnects = g_ws_disconnects;
  out.ws_snapshots_sent = g_ws_snapshots_sent;
  out.ws_live_updates_sent = g_ws_live_updates_sent;
  out.ws_send_failures = g_ws_send_failures;
  out.ws_control_rx = g_ws_control_rx;
  out.ws_control_tx_air = g_ws_control_tx_air;
  out.ws_control_ack = g_ws_control_ack;
  out.ws_control_tx = g_ws_control_tx;
  out.last_ws_connect_ms = g_last_ws_connect_ms;
  out.last_ws_send_ms = g_last_ws_send_ms;
  return out;
}

void resetCounters() {
  g_ws_state_seq = 0U;
  g_last_state_broadcast_ms = 0U;
  g_last_state_seq_sent = 0U;
  g_last_source_t_us_sent = 0U;
  g_last_radio_rx_ms_seen = 0U;
  g_last_ui_tx_ms = 0U;
  g_last_ui_tx_latency_ms = 0U;
  g_max_ui_tx_latency_ms = 0U;
  g_http_root_gets = 0U;
  g_http_asset_gets = 0U;
  g_http_not_found = 0U;
  g_ws_connects = 0U;
  g_ws_disconnects = 0U;
  g_ws_snapshots_sent = 0U;
  g_ws_live_updates_sent = 0U;
  g_ws_send_failures = 0U;
  g_ws_control_rx = 0U;
  g_ws_control_tx_air = 0U;
  g_ws_control_ack = 0U;
  g_ws_control_tx = 0U;
  g_last_ws_connect_ms = 0U;
  g_last_ws_send_ms = 0U;
  g_last_ws_live_log_ms = 0U;
  g_last_ws_backpressure_log_ms = 0U;
  g_ws_sum_count = 0U;
  g_ws_sum_last_seq = 0U;
  g_ws_sum_last_age_ms = 0U;
  g_synthetic_live_seq = 0U;
  g_last_files_revision_sent = 0U;
  g_last_storage_revision_sent = 0U;
  g_last_storage_poll_ms = 0U;
  clearPending();
  radio_link::resetStats();
}

}  // namespace ws_server
