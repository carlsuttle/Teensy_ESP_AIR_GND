#include "replay_bridge.h"

#include <SD.h>

#include "sd_backend.h"
#include "uart_telem.h"

namespace replay_bridge {
namespace {

constexpr char kLogDir[] = "/logs";
constexpr uint32_t kReplayMinPeriodUs = 2500U;
constexpr uint32_t kReplayMaxPeriodUs = 100000U;
constexpr uint32_t kReplayErrorOpenFailed = 1U;
constexpr uint32_t kReplayErrorNoLogFound = 2U;
constexpr uint32_t kReplayErrorShortRead = 3U;
constexpr uint32_t kReplayErrorUnsupportedRecord = 4U;
constexpr uint32_t kReplayErrorSendFailed = 5U;

#pragma pack(push, 1)
struct BinaryLogRecordV2 {
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

static_assert(sizeof(BinaryLogRecordV2) == 180U, "BinaryLogRecordV2 must match logger record size");

File g_file;
Status g_status = {};
bool g_status_dirty = false;
bool g_have_pending = false;
BinaryLogRecordV2 g_pending = {};
uint32_t g_next_send_us = 0U;
uint32_t g_next_session_id = 1U;
uint32_t g_last_progress_report_ms = 0U;
uint32_t g_last_record_t_us = 0U;
struct OutputSourceStamp {
  uint32_t seq = 0U;
  uint32_t t_us = 0U;
};
constexpr uint8_t kOutputSourceDepth = 8U;
OutputSourceStamp g_output_source_queue[kOutputSourceDepth] = {};
uint8_t g_output_source_head = 0U;
uint8_t g_output_source_tail = 0U;
uint8_t g_output_source_count = 0U;

String displayFileName(const String& path) {
  if (path.startsWith("/logs/")) return path.substring(6);
  if (path.startsWith("/")) return path.substring(1);
  return path;
}

void setCurrentFileName(const String& path) {
  memset(g_status.current_file, 0, sizeof(g_status.current_file));
  const String display = displayFileName(path);
  strncpy(g_status.current_file, display.c_str(), sizeof(g_status.current_file) - 1U);
}

void markStatusChanged() {
  g_status.last_change_ms = millis();
  g_status_dirty = true;
}

void resetRuntime(bool preserve_session = true) {
  if (g_file) {
    g_file.close();
  }
  g_have_pending = false;
  g_pending = {};
  g_next_send_us = 0U;
  g_last_progress_report_ms = 0U;
  g_last_record_t_us = 0U;
  g_output_source_head = 0U;
  g_output_source_tail = 0U;
  g_output_source_count = 0U;
  const uint32_t session_id = preserve_session ? g_status.session_id : 0U;
  g_status = {};
  g_status.session_id = session_id;
  markStatusChanged();
}

bool seekToRecord(uint32_t target_index) {
  if (!g_file || g_status.records_total == 0U) return false;
  if (target_index >= g_status.records_total) {
    target_index = g_status.records_total - 1U;
  }
  const size_t record_offset = (size_t)target_index * sizeof(BinaryLogRecordV2);
  if (!g_file.seek(record_offset)) return false;
  g_have_pending = false;
  g_pending = {};
  g_status.records_sent = target_index;
  g_status.flags &= (uint8_t)~telem::kReplayStatusFlagAtEof;
  g_status.last_error = 0U;
  g_last_record_t_us = 0U;
  g_next_send_us = micros();
  markStatusChanged();
  return true;
}

bool openLatestLog(String& out_name) {
  if (!sd_backend::mounted()) {
    sd_backend::Status backend = {};
    if (!sd_backend::begin(&backend)) return false;
  }

  File dir = SD.open(kLogDir);
  if (!dir || !dir.isDirectory()) return false;

  String best_name;
  File best_file;
  for (;;) {
    File candidate = dir.openNextFile();
    if (!candidate) break;
    if (candidate.isDirectory()) {
      candidate.close();
      continue;
    }
    const String name = String(candidate.name());
    if (!name.endsWith(".tlog")) {
      candidate.close();
      continue;
    }
    if (!best_file || name > best_name) {
      if (best_file) best_file.close();
      best_name = name;
      best_file = candidate;
    } else {
      candidate.close();
    }
  }
  dir.close();

  if (!best_file) return false;
  if (g_file) g_file.close();
  g_file = best_file;
  out_name = best_name;
  return true;
}

bool openLogByName(const String& requested_name, String& out_name) {
  if (!sd_backend::mounted()) {
    sd_backend::Status backend = {};
    if (!sd_backend::begin(&backend)) return false;
  }

  String full_name = requested_name;
  if (!full_name.startsWith("/")) full_name = String(kLogDir) + "/" + full_name;
  File file = SD.open(full_name, FILE_READ);
  if (!file) return false;

  if (g_file) g_file.close();
  g_file = file;
  out_name = full_name;
  return true;
}

bool loadNextRecord() {
  if (!g_file) return false;
  BinaryLogRecordV2 record = {};
  const size_t got = g_file.read((uint8_t*)&record, sizeof(record));
  if (got == 0U) {
    g_status.flags |= telem::kReplayStatusFlagAtEof;
    g_status.flags &= (uint8_t)~telem::kReplayStatusFlagActive;
    g_status.flags &= (uint8_t)~telem::kReplayStatusFlagPaused;
    markStatusChanged();
    return false;
  }
  if (got != sizeof(record)) {
    g_status.last_error = kReplayErrorShortRead;
    g_status.flags &= (uint8_t)~telem::kReplayStatusFlagActive;
    markStatusChanged();
    stop();
    return false;
  }
  g_pending = record;
  g_have_pending = true;
  return true;
}

bool fillReplayInput(const BinaryLogRecordV2& record, telem::ReplayInputRecord160& replay) {
  telem::TelemetryFullStateV1 state = {};
  memcpy(&state, record.payload, sizeof(state));

  replay.hdr.magic = telem::kReplayMagic;
  replay.hdr.version = telem::kReplayVersion;
  replay.hdr.kind = (uint8_t)telem::ReplayRecordKind::Input;
  replay.hdr.flags = 0U;
  replay.hdr.seq = record.seq;
  replay.hdr.t_us = record.t_us;

  replay.payload.present_mask = state.raw_present_mask;
  replay.payload.source_flags = state.flags;
  replay.payload.imu_seq = record.seq;
  replay.payload.gps_seq = record.seq;
  replay.payload.baro_seq = record.seq;
  replay.payload.accel_milli_mps2[0] = (int32_t)lroundf(state.accel_x_mps2 * 1000.0f);
  replay.payload.accel_milli_mps2[1] = (int32_t)lroundf(state.accel_y_mps2 * 1000.0f);
  replay.payload.accel_milli_mps2[2] = (int32_t)lroundf(state.accel_z_mps2 * 1000.0f);
  replay.payload.gyro_milli_dps[0] = (int32_t)lroundf(state.gyro_x_dps * 1000.0f);
  replay.payload.gyro_milli_dps[1] = (int32_t)lroundf(state.gyro_y_dps * 1000.0f);
  replay.payload.gyro_milli_dps[2] = (int32_t)lroundf(state.gyro_z_dps * 1000.0f);
  replay.payload.mag_milli_uT[0] = (int32_t)lroundf(state.mag_x_uT * 1000.0f);
  replay.payload.mag_milli_uT[1] = (int32_t)lroundf(state.mag_y_uT * 1000.0f);
  replay.payload.mag_milli_uT[2] = (int32_t)lroundf(state.mag_z_uT * 1000.0f);
  replay.payload.iTOW_ms = state.iTOW_ms;
  replay.payload.fixType = state.fixType;
  replay.payload.numSV = state.numSV;
  replay.payload.gps_flags = state.flags;
  replay.payload.lat_1e7 = state.lat_1e7;
  replay.payload.lon_1e7 = state.lon_1e7;
  replay.payload.hMSL_mm = state.hMSL_mm;
  replay.payload.gSpeed_mms = state.gSpeed_mms;
  replay.payload.headMot_1e5deg = state.headMot_1e5deg;
  replay.payload.hAcc_mm = state.hAcc_mm;
  replay.payload.sAcc_mms = state.sAcc_mms;
  replay.payload.baro_temp_milli_c = (int32_t)lroundf(state.baro_temp_c * 1000.0f);
  replay.payload.baro_press_milli_hpa = (int32_t)lroundf(state.baro_press_hpa * 1000.0f);
  replay.payload.baro_alt_mm = (int32_t)lroundf(state.baro_alt_m * 1000.0f);
  replay.payload.baro_vsi_milli_mps = (int32_t)lroundf(state.baro_vsi_mps * 1000.0f);
  memcpy(replay.payload.reserved + 0, &state.last_gps_ms, sizeof(state.last_gps_ms));
  memcpy(replay.payload.reserved + 4, &state.last_imu_ms, sizeof(state.last_imu_ms));
  memcpy(replay.payload.reserved + 8, &state.last_baro_ms, sizeof(state.last_baro_ms));
  return true;
}

bool sendPendingRecord() {
  if (!g_have_pending) return true;

  switch ((telem::LogRecordKind)g_pending.record_kind) {
    case telem::LogRecordKind::State160: {
      telem::ReplayInputRecord160 replay = {};
      if (!fillReplayInput(g_pending, replay)) {
        g_status.last_error = kReplayErrorUnsupportedRecord;
        return false;
      }
      if (!uart_telem::sendReplayInputRecord(replay)) {
        g_status.last_error = kReplayErrorSendFailed;
        return false;
      }
      if (g_output_source_count >= kOutputSourceDepth) {
        g_output_source_tail = (uint8_t)((g_output_source_tail + 1U) % kOutputSourceDepth);
        g_output_source_count--;
      }
      g_output_source_queue[g_output_source_head].seq = g_pending.seq;
      g_output_source_queue[g_output_source_head].t_us = g_pending.t_us;
      g_output_source_head = (uint8_t)((g_output_source_head + 1U) % kOutputSourceDepth);
      g_output_source_count++;
      break;
    }
    case telem::LogRecordKind::ReplayControl160: {
      telem::ReplayControlRecord160 replay = {};
      memcpy(&replay, g_pending.payload, sizeof(replay));
      if (!uart_telem::sendReplayControlRecord(replay)) {
        g_status.last_error = kReplayErrorSendFailed;
        return false;
      }
      break;
    }
    default:
      g_status.last_error = kReplayErrorUnsupportedRecord;
      return false;
  }

  g_have_pending = false;
  g_status.records_sent++;
  uint32_t next_period_us = kReplayMinPeriodUs;
  if (g_last_record_t_us != 0U && g_pending.t_us > g_last_record_t_us) {
    const uint32_t delta_us = g_pending.t_us - g_last_record_t_us;
    next_period_us = constrain(delta_us, kReplayMinPeriodUs, kReplayMaxPeriodUs);
  }
  g_last_record_t_us = g_pending.t_us;
  g_next_send_us = micros() + next_period_us;
  const uint32_t now_ms = millis();
  if (g_last_progress_report_ms == 0U || (uint32_t)(now_ms - g_last_progress_report_ms) >= 250U) {
    g_last_progress_report_ms = now_ms;
    markStatusChanged();
  }
  return true;
}

}  // namespace

void begin() {
  resetRuntime(false);
}

void poll() {
  if ((g_status.flags & telem::kReplayStatusFlagActive) == 0U) return;

  const uint32_t now_us = micros();
  uint8_t burst_guard = 0U;
  while ((g_status.flags & telem::kReplayStatusFlagActive) != 0U && burst_guard < 8U) {
    if (g_next_send_us != 0U && (int32_t)(now_us - g_next_send_us) < 0) break;
    if (!g_have_pending && !loadNextRecord()) break;
    if (!sendPendingRecord()) {
      g_status.flags &= (uint8_t)~telem::kReplayStatusFlagActive;
      markStatusChanged();
      break;
    }
    burst_guard++;
  }
}

bool startLatest() {
  resetRuntime(false);
  String file_name;
  if (!openLatestLog(file_name)) {
    g_status.last_error = sd_backend::mounted() ? kReplayErrorNoLogFound : kReplayErrorOpenFailed;
    markStatusChanged();
    return false;
  }

  g_status.session_id = g_next_session_id++;
  g_status.records_total = (uint32_t)(g_file.size() / sizeof(BinaryLogRecordV2));
  g_status.records_sent = 0U;
  g_status.last_error = 0U;
  g_status.last_command = telem::CMD_REPLAY_START;
  g_status.flags = telem::kReplayStatusFlagActive | telem::kReplayStatusFlagFileOpen;
  setCurrentFileName(file_name);
  g_have_pending = false;
  g_next_send_us = micros();
  markStatusChanged();
  return true;
}

bool startFile(const String& file_name) {
  resetRuntime(false);
  String opened_name;
  if (!openLogByName(file_name, opened_name)) {
    g_status.last_error = sd_backend::mounted() ? kReplayErrorNoLogFound : kReplayErrorOpenFailed;
    markStatusChanged();
    return false;
  }

  g_status.session_id = g_next_session_id++;
  g_status.records_total = (uint32_t)(g_file.size() / sizeof(BinaryLogRecordV2));
  g_status.records_sent = 0U;
  g_status.last_error = 0U;
  g_status.last_command = telem::CMD_REPLAY_START;
  g_status.flags = telem::kReplayStatusFlagActive | telem::kReplayStatusFlagFileOpen;
  setCurrentFileName(opened_name);
  g_have_pending = false;
  g_next_send_us = micros();
  markStatusChanged();
  return true;
}

bool resume() {
  if (!g_file) return false;
  if ((g_status.flags & telem::kReplayStatusFlagAtEof) != 0U) {
    if (!seekToRecord(0U)) return false;
  }
  g_status.flags |= telem::kReplayStatusFlagFileOpen;
  g_status.flags |= telem::kReplayStatusFlagActive;
  g_status.flags &= (uint8_t)~telem::kReplayStatusFlagPaused;
  g_status.last_command = telem::CMD_REPLAY_START;
  g_status.last_error = 0U;
  if (g_next_send_us == 0U) g_next_send_us = micros();
  markStatusChanged();
  return true;
}

bool pause() {
  if ((g_status.flags & telem::kReplayStatusFlagActive) == 0U || !g_file) return false;
  g_status.flags &= (uint8_t)~telem::kReplayStatusFlagActive;
  g_status.flags |= telem::kReplayStatusFlagPaused;
  g_status.last_command = telem::CMD_REPLAY_PAUSE;
  markStatusChanged();
  return true;
}

bool seekRelative(int32_t delta_records) {
  if (!g_file || g_status.records_total == 0U) return false;
  const int32_t current_index = (int32_t)g_status.records_sent;
  const int32_t max_index = (int32_t)g_status.records_total - 1;
  const int32_t target_index = constrain(current_index + delta_records, 0, max_index);
  g_status.last_command = telem::CMD_REPLAY_SEEK_REL;
  return seekToRecord((uint32_t)target_index);
}

void stop() {
  if (g_file) {
    g_file.close();
  }
  g_have_pending = false;
  g_pending = {};
  g_status.flags &= (uint8_t)~telem::kReplayStatusFlagActive;
  g_status.flags &= (uint8_t)~telem::kReplayStatusFlagFileOpen;
  g_status.flags &= (uint8_t)~telem::kReplayStatusFlagPaused;
  if (g_status.last_command != telem::CMD_REPLAY_STOP) {
    g_status.last_command = telem::CMD_REPLAY_STOP;
  }
  memset(g_status.current_file, 0, sizeof(g_status.current_file));
  markStatusChanged();
}

Status status() {
  return g_status;
}

telem::ReplayStatusPayloadV1 currentPayload() {
  telem::ReplayStatusPayloadV1 payload = {};
  payload.flags = g_status.flags;
  payload.last_command = g_status.last_command;
  payload.session_id = g_status.session_id;
  payload.records_total = g_status.records_total;
  payload.records_sent = g_status.records_sent;
  payload.last_error = g_status.last_error;
  payload.last_change_ms = g_status.last_change_ms ? (uint32_t)(millis() - g_status.last_change_ms) : 0U;
  memcpy(payload.current_file, g_status.current_file, sizeof(payload.current_file));
  return payload;
}

bool takeStatusDirty() {
  const bool dirty = g_status_dirty;
  g_status_dirty = false;
  return dirty;
}

bool active() {
  return (g_status.flags & telem::kReplayStatusFlagActive) != 0U;
}

String currentFileName() {
  return String(g_status.current_file);
}

bool takeOutputSourceStamp(uint32_t& seq, uint32_t& t_us) {
  if (g_output_source_count == 0U) return false;
  seq = g_output_source_queue[g_output_source_tail].seq;
  t_us = g_output_source_queue[g_output_source_tail].t_us;
  g_output_source_tail = (uint8_t)((g_output_source_tail + 1U) % kOutputSourceDepth);
  g_output_source_count--;
  return true;
}

}  // namespace replay_bridge
