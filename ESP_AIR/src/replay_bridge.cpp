#include "replay_bridge.h"

#include <SD.h>

#include "sd_backend.h"
#include "uart_telem.h"

namespace replay_bridge {
namespace {

constexpr char kLogDir[] = "/logs";
constexpr uint32_t kReplayRateHz = 400U;
constexpr uint32_t kReplayPeriodUs = 1000000UL / kReplayRateHz;
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
  const uint32_t session_id = preserve_session ? g_status.session_id : 0U;
  g_status = {};
  g_status.session_id = session_id;
  markStatusChanged();
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

bool loadNextRecord() {
  if (!g_file) return false;
  BinaryLogRecordV2 record = {};
  const size_t got = g_file.read((uint8_t*)&record, sizeof(record));
  if (got == 0U) {
    g_status.flags |= telem::kReplayStatusFlagAtEof;
    g_status.flags &= (uint8_t)~telem::kReplayStatusFlagActive;
    markStatusChanged();
    stop();
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
    if (g_next_send_us == 0U) {
      g_next_send_us = now_us + kReplayPeriodUs;
    } else {
      g_next_send_us += kReplayPeriodUs;
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
  g_have_pending = false;
  g_next_send_us = micros();
  markStatusChanged();
  return true;
}

void stop() {
  if (g_file) {
    g_file.close();
  }
  g_have_pending = false;
  g_status.flags &= (uint8_t)~telem::kReplayStatusFlagActive;
  g_status.flags &= (uint8_t)~telem::kReplayStatusFlagFileOpen;
  if (g_status.last_command != telem::CMD_REPLAY_STOP) {
    g_status.last_command = telem::CMD_REPLAY_STOP;
  }
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
  return payload;
}

bool takeStatusDirty() {
  const bool dirty = g_status_dirty;
  g_status_dirty = false;
  return dirty;
}

}  // namespace replay_bridge
