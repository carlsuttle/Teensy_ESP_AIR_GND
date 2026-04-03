#include "ws_server.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <WiFi.h>

#include "config_store.h"
#include "radio_link.h"
#include "types_shared.h"

namespace ws_server {
namespace {

AsyncWebServer g_server(80);
AsyncWebSocket g_ws("/ws");

constexpr uint16_t kSnapshotRateHz = 30U;
constexpr uint32_t kSnapshotPeriodMs = 1000U / kSnapshotRateHz;
constexpr uint32_t kAckTimeoutMs = 2000U;
constexpr uint32_t kFilesTimeoutMs = 15000U;
constexpr uint32_t kStoragePollMs = 2000U;

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
  AwaitFiles,
};

struct PendingCommand {
  PendingOp op = PendingOp::None;
  PendingPhase phase = PendingPhase::None;
  uint32_t req_id = 0U;
  uint32_t started_ms = 0U;
  uint32_t deadline_ms = 0U;
  uint32_t ack_baseline = 0U;
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

void sendJson(const JsonDocument& doc) {
  String text;
  serializeJson(doc, text);
  g_ws.textAll(text);
}

void sendAck(PendingOp op, uint32_t req_id, bool ok, uint32_t code, const char* detail = "") {
  JsonDocument doc;
  doc["type"] = "ack";
  doc["op"] = opText(op);
  doc["req_id"] = req_id;
  doc["ok"] = ok;
  doc["code"] = code;
  if (detail && detail[0] != '\0') doc["detail"] = detail;
  sendJson(doc);
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
  if (client) client->text(text);
}

void sendFiles(uint32_t req_id = 0U) {
  JsonDocument doc;
  const String payload = radio_link::remoteFilesJson(false);
  if (deserializeJson(doc, payload)) return;
  doc["type"] = "files";
  if (req_id != 0U) doc["req_id"] = req_id;
  sendJson(doc);
  g_last_files_revision_sent = radio_link::remoteFilesStatus().revision;
}

void sendStorage(uint32_t req_id = 0U) {
  JsonDocument doc;
  const String payload = radio_link::remoteStorageJson(false);
  if (deserializeJson(doc, payload)) return;
  doc["type"] = "storage";
  if (req_id != 0U) doc["req_id"] = req_id;
  sendJson(doc);
  g_last_storage_revision_sent = radio_link::remoteStorageStatus().revision;
}

void updateTxStats(const radio_link::Snapshot& snap, uint32_t now_ms) {
  const uint32_t freshness_ms =
      snap.stats.last_state_apply_ms != 0U ? snap.stats.last_state_apply_ms : snap.stats.last_rx_ms;
  g_last_state_seq_sent = snap.seq;
  g_last_source_t_us_sent = snap.t_us;
  g_last_radio_rx_ms_seen = freshness_ms;
  g_last_ui_tx_ms = now_ms;
  g_last_ui_tx_latency_ms =
      freshness_ms != 0U ? (uint32_t)(now_ms - freshness_ms) : 0U;
  if (g_last_ui_tx_latency_ms > g_max_ui_tx_latency_ms) {
    g_max_ui_tx_latency_ms = g_last_ui_tx_latency_ms;
  }
}

void maybeBroadcastSnapshot() {
  if (g_ws.count() == 0U) return;
  const uint32_t now_ms = millis();
  if (g_last_state_broadcast_ms != 0U &&
      (uint32_t)(now_ms - g_last_state_broadcast_ms) < kSnapshotPeriodMs) {
    return;
  }

  const radio_link::Snapshot snap = radio_link::snapshot();
  const telem::GpsCalendarTime gps_time = telem::gpsCalendarTime(snap.state);
  const bool gps_calendar_valid =
      gps_time.year != 0U && gps_time.month != 0U && gps_time.day != 0U;
  const uint32_t freshness_ms =
      snap.stats.last_state_apply_ms != 0U ? snap.stats.last_state_apply_ms : snap.stats.last_rx_ms;
  uint32_t replay_source_seq = 0U;
  uint32_t replay_source_t_us = 0U;
  telem::decodeReplaySourceStamp(snap.state, replay_source_seq, replay_source_t_us);

  JsonDocument doc;
  doc["type"] = "snapshot";
  doc["schema_id"] = telem::kActiveSchema.schema_id;
  doc["schema_version"] = telem::kActiveSchema.schema_version;
  doc["ws_seq"] = ++g_ws_state_seq;
  doc["seq"] = snap.seq;
  doc["source_t_us"] = snap.t_us;
  doc["replay_source_seq"] = replay_source_seq;
  doc["replay_source_t_us"] = replay_source_t_us;
  doc["fresh"] = freshness_ms != 0U && (uint32_t)(now_ms - freshness_ms) <= 3000U;
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
  doc["time_state"] = telem::timeStateText(snap.live_status.time_state);
  doc["time_source"] = telem::timeSourceText(snap.live_status.time_source);
  doc["gps_calendar_present"] =
      (snap.live_status.time_flags & telem::kTimeStatusFlagGpsCalendarPresent) != 0U;
  doc["gps_time_valid"] = (snap.live_status.time_flags & telem::kTimeStatusFlagGpsTimeValid) != 0U;
  doc["system_time_set"] = (snap.live_status.time_flags & telem::kTimeStatusFlagSystemTimeSet) != 0U;
  doc["system_time_utc_s"] = snap.live_status.system_time_utc_s;
  doc["time_last_set_age_ms"] = snap.live_status.time_last_set_age_ms;
  doc["time_sync_count"] = snap.live_status.time_sync_count;
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
  doc["has_state"] = snap.has_state;
  doc["roll_deg"] = snap.state.roll_deg;
  doc["pitch_deg"] = snap.state.pitch_deg;
  doc["yaw_deg"] = snap.state.yaw_deg;
  doc["mag_heading_deg"] = snap.state.mag_heading_deg;
  doc["lat_1e7"] = snap.state.lat_1e7;
  doc["lon_1e7"] = snap.state.lon_1e7;
  doc["hMSL_mm"] = snap.state.hMSL_mm;
  doc["gSpeed_mms"] = snap.state.gSpeed_mms;
  doc["headMot_1e5deg"] = snap.state.headMot_1e5deg;
  doc["gps_itow_ms"] = snap.state.iTOW_ms;
  doc["gps_fix_type"] = snap.state.fixType;
  doc["gps_num_sv"] = snap.state.numSV;
  doc["hAcc_mm"] = snap.state.hAcc_mm;
  doc["sAcc_mms"] = snap.state.sAcc_mms;
  doc["baro_temp_c"] = snap.state.baro_temp_c;
  doc["baro_press_hpa"] = snap.state.baro_press_hpa;
  doc["baro_alt_m"] = snap.state.baro_alt_m;
  doc["baro_vsi_mps"] = snap.state.baro_vsi_mps;
  doc["fusion_gain"] = snap.state.fusion_gain;
  doc["fusion_accel_rej"] = snap.state.fusion_accel_rej;
  doc["fusion_mag_rej"] = snap.state.fusion_mag_rej;
  doc["fusion_recovery_period"] = snap.state.fusion_recovery_period;
  doc["flags"] = snap.state.flags;
  doc["raw_present_mask"] = snap.state.raw_present_mask;
  doc["gps_calendar_valid"] = gps_calendar_valid;
  doc["gps_year"] = gps_time.year;
  doc["gps_month"] = gps_time.month;
  doc["gps_day"] = gps_time.day;
  doc["gps_hour"] = gps_time.hour;
  doc["gps_min"] = gps_time.minute;
  doc["gps_sec"] = gps_time.second;
  if (gps_calendar_valid) {
    JsonObject calendar = doc["gps_calendar"].to<JsonObject>();
    calendar["year"] = gps_time.year;
    calendar["month"] = gps_time.month;
    calendar["day"] = gps_time.day;
    calendar["hour"] = gps_time.hour;
    calendar["min"] = gps_time.minute;
    calendar["sec"] = gps_time.second;
  }

  updateTxStats(snap, now_ms);
  sendJson(doc);
  g_last_state_broadcast_ms = now_ms;
}

void clearPending() {
  g_pending = {};
}

bool enqueuePending(const PendingCommand& next) {
  if (g_pending.op != PendingOp::None) {
    Serial.printf("WSCTL enqueue_busy op=%s req=%lu pending=%s\r\n",
                  opText(next.op),
                  (unsigned long)next.req_id,
                  opText(g_pending.op));
    return false;
  }
  g_pending = next;
  g_pending.started_ms = millis();
  Serial.printf("WSCTL enqueue op=%s req=%lu\r\n",
                opText(next.op),
                (unsigned long)next.req_id);
  return true;
}

void finalizePending(bool ok, uint32_t code, const char* detail = "") {
  Serial.printf("WSCTL finalize op=%s req=%lu ok=%u code=%lu detail=%s\r\n",
                opText(g_pending.op),
                (unsigned long)g_pending.req_id,
                ok ? 1U : 0U,
                (unsigned long)code,
                detail ? detail : "");
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
        g_pending.phase = PendingPhase::AwaitAck;
        g_pending.deadline_ms = now_ms + kAckTimeoutMs;
        break;
      default:
        tx_ok = false;
        break;
    }

    if (!tx_ok) {
      finalizePending(false, 1U, "tx_failed");
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

  if (g_pending.phase == PendingPhase::AwaitFiles) {
    const radio_link::Snapshot snap = radio_link::snapshot();
    if (snap.has_ack &&
        snap.ack_command == telem::CMD_GET_LOG_FILE_LIST &&
        snap.ack_rx_seq != 0U &&
        snap.ack_rx_seq != g_pending.ack_baseline) {
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

void handleWsMessage(const char* text, size_t len) {
  Serial.printf("WSCTL rx len=%u text=%.*s\r\n",
                (unsigned)len,
                (int)len,
                text ? text : "");
  JsonDocument doc;
  if (deserializeJson(doc, text, len)) {
    Serial.println("WSCTL parse_error");
    return;
  }

  const char* type = doc["type"] | "";
  const uint32_t req_id = doc["req_id"] | 0U;

  if (strcmp(type, "control") == 0) {
    const char* category = doc["category"] | "";
    const char* action = doc["action"] | "";

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
    Serial.printf("WSCTL unsupported_control req=%lu category=%s action=%s\r\n",
                  (unsigned long)req_id,
                  category,
                  action);
    sendJson(err);
    return;
  }

  JsonDocument err;
  err["type"] = "ack";
  err["req_id"] = req_id;
  err["ok"] = false;
  err["code"] = 4U;
  err["detail"] = "unsupported";
  Serial.printf("WSCTL unsupported_message req=%lu type=%s\r\n",
                (unsigned long)req_id,
                type);
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
    Serial.printf("WSCTL connect client=%u total=%u\r\n",
                  client ? client->id() : 0U,
                  (unsigned)g_ws.count());
    sendHello(client);
    sendFiles();
    sendStorage();
    (void)radio_link::sendGetStorageStatus();
    (void)radio_link::sendGetReplayStatus();
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
  g_server.serveStatic("/", LittleFS, "/")
      .setDefaultFile("index.html")
      .setTryGzipFirst(false)
      .setCacheControl("no-cache, no-store, must-revalidate");
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
  g_last_files_revision_sent = 0U;
  g_last_storage_revision_sent = 0U;
  g_last_storage_poll_ms = 0U;
  clearPending();
  radio_link::resetStats();
}

}  // namespace ws_server
