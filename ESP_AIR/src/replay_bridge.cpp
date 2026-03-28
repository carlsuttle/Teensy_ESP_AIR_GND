#include "replay_bridge.h"

#include "sd_api.h"
#include "sd_backend.h"
#include "teensy_link.h"

namespace replay_bridge {
namespace {
using File = sd_api::File;

constexpr char kLogDir[] = "/logs";
constexpr uint32_t kLogMagic = 0x4C4F4731UL;  // "LOG1"
constexpr uint16_t kLogVersion = 2U;
// Replay should feed the Teensy in batched transactions at the documented
// 100 Hz cadence. When source metadata has usable timing, group records into
// 10 ms source-time windows. Otherwise derive a batch count from the logged
// source rate, falling back to the fastest replay profile we expect to use.
constexpr uint32_t kReplayBatchRateHz = 100U;
constexpr uint32_t kReplayBatchPeriodUs = 1000000UL / kReplayBatchRateHz;
constexpr uint32_t kReplayFallbackFastestHz = 3200U;
constexpr uint16_t kReplayFallbackRecordsPerBatch =
    (uint16_t)(kReplayFallbackFastestHz / kReplayBatchRateHz);
constexpr uint32_t kReplayErrorOpenFailed = 1U;
constexpr uint32_t kReplayErrorNoLogFound = 2U;
constexpr uint32_t kReplayErrorShortRead = 3U;
constexpr uint32_t kReplayErrorUnsupportedRecord = 4U;
constexpr uint32_t kReplayErrorSendFailed = 5U;
constexpr uint8_t kReplayAverageMax = 32U;

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

sd_api::File g_file;
Status g_status = {};
bool g_status_dirty = false;
bool g_have_pending = false;
BinaryLogRecordV2 g_pending = {};
bool g_prefer_replay_input = false;
uint8_t g_average_factor = 1U;
uint32_t g_next_send_us = 0U;
uint32_t g_next_session_id = 1U;
uint32_t g_last_progress_report_ms = 0U;
uint32_t g_last_record_t_us = 0U;
uint16_t g_metadata_source_rate_hz = 0U;
struct OutputSourceStamp {
  uint32_t seq = 0U;
  uint32_t t_us = 0U;
};
constexpr uint16_t kOutputSourceDepth = 512U;
OutputSourceStamp g_output_source_queue[kOutputSourceDepth] = {};
uint16_t g_output_source_head = 0U;
uint16_t g_output_source_tail = 0U;
uint16_t g_output_source_count = 0U;
portMUX_TYPE g_output_source_mux = portMUX_INITIALIZER_UNLOCKED;

struct StateWindow {
  uint8_t count = 0U;
  uint32_t seq = 0U;
  uint32_t t_us = 0U;
  telem::TelemetryFullStateV1 latest = {};
};

struct ReplayInputWindow {
  uint8_t count = 0U;
  uint32_t seq = 0U;
  uint32_t t_us = 0U;
  telem::ReplayInputRecord160 latest = {};
};

uint8_t clampAverageFactor(uint8_t factor) {
  if (factor == 0U) return 1U;
  if (factor > kReplayAverageMax) return kReplayAverageMax;
  return factor;
}

uint16_t replayBatchRecordLimit() {
  const uint8_t average_factor = clampAverageFactor(g_average_factor);
  uint32_t source_rate_hz = (g_metadata_source_rate_hz != 0U)
                                ? (uint32_t)g_metadata_source_rate_hz
                                : kReplayFallbackFastestHz;
  uint32_t output_rate_hz = source_rate_hz / average_factor;
  if (output_rate_hz == 0U) output_rate_hz = 1U;
  uint32_t records_per_batch =
      (output_rate_hz + (kReplayBatchRateHz / 2U)) / kReplayBatchRateHz;
  if (records_per_batch == 0U) records_per_batch = 1U;
  if (records_per_batch > 51U) records_per_batch = 51U;
  return (uint16_t)records_per_batch;
}

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
  g_metadata_source_rate_hz = 0U;
  portENTER_CRITICAL(&g_output_source_mux);
  g_output_source_head = 0U;
  g_output_source_tail = 0U;
  g_output_source_count = 0U;
  portEXIT_CRITICAL(&g_output_source_mux);
  g_prefer_replay_input = false;
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

  File dir = sd_api::open(kLogDir);
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
  File file = sd_api::open(full_name);
  if (!file) return false;

  if (g_file) g_file.close();
  g_file = file;
  out_name = full_name;
  return true;
}

bool loadNextRecord() {
  if (!g_file) return false;
  BinaryLogRecordV2 record = {};
  size_t total = 0U;
  while (total < sizeof(record)) {
    const size_t got = g_file.read(((uint8_t*)&record) + total, sizeof(record) - total);
    if (got == 0U) break;
    total += got;
  }
  if (total == 0U) {
    g_status.flags |= telem::kReplayStatusFlagAtEof;
    g_status.flags &= (uint8_t)~telem::kReplayStatusFlagActive;
    g_status.flags &= (uint8_t)~telem::kReplayStatusFlagPaused;
    markStatusChanged();
    return false;
  }
  if (total != sizeof(record)) {
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

bool preferReplayInputSource() {
  if (!g_file) return false;
  const uint32_t saved_pos = (uint32_t)g_file.position();
  if (!g_file.seek(0U)) return false;
  BinaryLogRecordV2 record = {};
  uint32_t state_count = 0U;
  uint32_t replay_input_count = 0U;
  while (g_file.read((uint8_t*)&record, sizeof(record)) == sizeof(record)) {
    if (record.magic != kLogMagic || record.version != kLogVersion || record.record_size != sizeof(record)) {
      break;
    }
    switch ((telem::LogRecordKind)record.record_kind) {
      case telem::LogRecordKind::State160:
        state_count++;
        break;
      case telem::LogRecordKind::Metadata160:
        break;
      case telem::LogRecordKind::ReplayInput160:
        replay_input_count++;
        break;
      default:
        break;
    }
  }
  g_file.seek(saved_pos);
  if (replay_input_count == 0U) return false;
  if (state_count == 0U) return true;
  return replay_input_count > state_count;
}

uint16_t findReplaySourceRateHz() {
  if (!g_file) return 0U;
  const uint32_t saved_pos = (uint32_t)g_file.position();
  if (!g_file.seek(0U)) return 0U;
  BinaryLogRecordV2 record = {};
  uint16_t found_hz = 0U;
  while (g_file.read((uint8_t*)&record, sizeof(record)) == sizeof(record)) {
    if (record.magic != kLogMagic || record.version != kLogVersion || record.record_size != sizeof(record)) {
      break;
    }
    if ((telem::LogRecordKind)record.record_kind != telem::LogRecordKind::ReplayControl160) continue;
    telem::ReplayControlRecord160 replay = {};
    memcpy(&replay, record.payload, sizeof(replay));
    if (replay.payload.command_id != telem::CMD_SET_CAPTURE_SETTINGS) continue;
    if (replay.payload.payload_len < sizeof(telem::CmdSetCaptureSettingsV1)) continue;
    telem::CmdSetCaptureSettingsV1 cmd = {};
    memcpy(&cmd, replay.payload.payload, sizeof(cmd));
    if (cmd.source_rate_hz != 0U) found_hz = cmd.source_rate_hz;
  }
  g_file.seek(saved_pos);
  return found_hz;
}

bool decodeStateRecord(const BinaryLogRecordV2& record, telem::TelemetryFullStateV1& state) {
  if ((telem::LogRecordKind)record.record_kind != telem::LogRecordKind::State160) return false;
  memcpy(&state, record.payload, sizeof(state));
  return true;
}

bool decodeReplayInputRecord(const BinaryLogRecordV2& record, telem::ReplayInputRecord160& replay) {
  if ((telem::LogRecordKind)record.record_kind != telem::LogRecordKind::ReplayInput160) return false;
  memcpy(&replay, record.payload, sizeof(replay));
  return true;
}

bool isSkippableRecordKind(telem::LogRecordKind kind) {
  return kind == telem::LogRecordKind::Metadata160;
}

void beginStateWindow(StateWindow& window, const BinaryLogRecordV2& record,
                      const telem::TelemetryFullStateV1& state) {
  window = {};
  window.count = 1U;
  window.seq = record.seq;
  window.t_us = record.t_us;
  window.latest = state;
}

void extendStateWindow(StateWindow& window, const BinaryLogRecordV2& record,
                       const telem::TelemetryFullStateV1& state) {
  window.count++;
  window.seq = record.seq;
  window.t_us = record.t_us;
  window.latest = state;
}

bool fillReplayInputFromState(const telem::TelemetryFullStateV1& state, uint32_t seq, uint32_t t_us,
                              telem::ReplayInputRecord160& replay) {
  memset(&replay, 0, sizeof(replay));

  replay.hdr.magic = telem::kReplayMagic;
  replay.hdr.version = telem::kReplayVersion;
  replay.hdr.kind = (uint8_t)telem::ReplayRecordKind::Input;
  replay.hdr.flags = 0U;
  replay.hdr.seq = seq;
  replay.hdr.t_us = t_us;

  replay.payload.present_mask = state.raw_present_mask;
  replay.payload.source_flags = state.flags;
  replay.payload.imu_seq = seq;
  replay.payload.gps_seq = seq;
  replay.payload.baro_seq = seq;
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

bool fillReplayInputFromWindow(const StateWindow& window, telem::ReplayInputRecord160& replay) {
  if (window.count == 0U) return false;
  return fillReplayInputFromState(window.latest, window.seq, window.t_us, replay);
}

void beginReplayInputWindow(ReplayInputWindow& window, const BinaryLogRecordV2& record,
                            const telem::ReplayInputRecord160& replay) {
  window = {};
  window.count = 1U;
  window.seq = record.seq;
  window.t_us = record.t_us;
  window.latest = replay;
}

void extendReplayInputWindow(ReplayInputWindow& window, const BinaryLogRecordV2& record,
                             const telem::ReplayInputRecord160& replay) {
  window.count++;
  window.seq = record.seq;
  window.t_us = record.t_us;
  window.latest = replay;
}

bool fillReplayInputFromWindow(const ReplayInputWindow& window, telem::ReplayInputRecord160& replay) {
  if (window.count == 0U) return false;
  replay = window.latest;
  replay.hdr.seq = window.seq;
  replay.hdr.t_us = window.t_us;
  replay.payload.imu_seq = window.seq;
  return true;
}

void finishSentRecord(uint32_t t_us) {
  g_have_pending = false;
  g_status.records_sent++;
  g_last_record_t_us = t_us;
  const uint32_t now_ms = millis();
  if (g_last_progress_report_ms == 0U || (uint32_t)(now_ms - g_last_progress_report_ms) >= 250U) {
    g_last_progress_report_ms = now_ms;
    markStatusChanged();
  }
}

bool sendPendingRecord() {
  if (!g_have_pending) return true;

  while (g_have_pending && isSkippableRecordKind((telem::LogRecordKind)g_pending.record_kind)) {
    g_have_pending = false;
    g_status.records_sent++;
    if (!loadNextRecord()) return true;
  }

  while (g_have_pending && g_prefer_replay_input &&
         (telem::LogRecordKind)g_pending.record_kind == telem::LogRecordKind::State160) {
    g_have_pending = false;
    g_status.records_sent++;
    if (!loadNextRecord()) return true;
  }

  switch ((telem::LogRecordKind)g_pending.record_kind) {
    case telem::LogRecordKind::ReplayInput160: {
      ReplayInputWindow window = {};
      telem::ReplayInputRecord160 replay = {};
      if (!decodeReplayInputRecord(g_pending, replay)) {
        g_status.last_error = kReplayErrorUnsupportedRecord;
        return false;
      }
      beginReplayInputWindow(window, g_pending, replay);
      g_have_pending = false;

      const uint8_t average_factor = clampAverageFactor(g_average_factor);
      while (window.count < average_factor) {
        if (!loadNextRecord()) break;
        if (isSkippableRecordKind((telem::LogRecordKind)g_pending.record_kind)) {
          g_have_pending = false;
          g_status.records_sent++;
          continue;
        }
        if ((telem::LogRecordKind)g_pending.record_kind == telem::LogRecordKind::State160 && g_prefer_replay_input) {
          g_have_pending = false;
          g_status.records_sent++;
          continue;
        }
        if ((telem::LogRecordKind)g_pending.record_kind != telem::LogRecordKind::ReplayInput160) {
          break;
        }
        if (!decodeReplayInputRecord(g_pending, replay)) {
          g_status.last_error = kReplayErrorUnsupportedRecord;
          return false;
        }
        extendReplayInputWindow(window, g_pending, replay);
        g_have_pending = false;
      }

      if (!fillReplayInputFromWindow(window, replay)) {
        g_status.last_error = kReplayErrorUnsupportedRecord;
        return false;
      }
      if (!teensy_link::sendReplayInputRecord(replay)) {
        g_status.last_error = kReplayErrorSendFailed;
        return false;
      }
      portENTER_CRITICAL(&g_output_source_mux);
      if (g_output_source_count >= kOutputSourceDepth) {
        g_output_source_tail = (uint16_t)((g_output_source_tail + 1U) % kOutputSourceDepth);
        g_output_source_count--;
      }
      g_output_source_queue[g_output_source_head].seq = window.seq;
      g_output_source_queue[g_output_source_head].t_us = window.t_us;
      g_output_source_head = (uint16_t)((g_output_source_head + 1U) % kOutputSourceDepth);
      g_output_source_count++;
      portEXIT_CRITICAL(&g_output_source_mux);
      finishSentRecord(window.t_us);
      break;
    }
    case telem::LogRecordKind::State160: {
      StateWindow window = {};
      telem::TelemetryFullStateV1 state = {};
      if (!decodeStateRecord(g_pending, state)) {
        g_status.last_error = kReplayErrorUnsupportedRecord;
        return false;
      }
      beginStateWindow(window, g_pending, state);
      g_have_pending = false;

      const uint8_t average_factor = clampAverageFactor(g_average_factor);
      while (window.count < average_factor) {
        if (!loadNextRecord()) break;
        if (isSkippableRecordKind((telem::LogRecordKind)g_pending.record_kind)) {
          g_have_pending = false;
          g_status.records_sent++;
          continue;
        }
        if ((telem::LogRecordKind)g_pending.record_kind != telem::LogRecordKind::State160) {
          break;
        }
        if (!decodeStateRecord(g_pending, state)) {
          g_status.last_error = kReplayErrorUnsupportedRecord;
          return false;
        }
        extendStateWindow(window, g_pending, state);
        g_have_pending = false;
      }

      telem::ReplayInputRecord160 replay = {};
      if (!fillReplayInputFromWindow(window, replay)) {
        g_status.last_error = kReplayErrorUnsupportedRecord;
        return false;
      }
      if (!teensy_link::sendReplayInputRecord(replay)) {
        g_status.last_error = kReplayErrorSendFailed;
        return false;
      }
      portENTER_CRITICAL(&g_output_source_mux);
      if (g_output_source_count >= kOutputSourceDepth) {
        g_output_source_tail = (uint16_t)((g_output_source_tail + 1U) % kOutputSourceDepth);
        g_output_source_count--;
      }
      g_output_source_queue[g_output_source_head].seq = window.seq;
      g_output_source_queue[g_output_source_head].t_us = window.t_us;
      g_output_source_head = (uint16_t)((g_output_source_head + 1U) % kOutputSourceDepth);
      g_output_source_count++;
      portEXIT_CRITICAL(&g_output_source_mux);
      finishSentRecord(window.t_us);
      break;
    }
    case telem::LogRecordKind::ReplayControl160: {
      telem::ReplayControlRecord160 replay = {};
      memcpy(&replay, g_pending.payload, sizeof(replay));
      if (!teensy_link::sendReplayControlRecord(replay)) {
        g_status.last_error = kReplayErrorSendFailed;
        return false;
      }
      finishSentRecord(g_pending.t_us);
      break;
    }
    default:
      g_status.last_error = kReplayErrorUnsupportedRecord;
      return false;
  }
  return true;
}

}  // namespace

void begin() {
  resetRuntime(false);
}

void setAverageFactor(uint8_t factor) {
  g_average_factor = clampAverageFactor(factor);
}

uint8_t averageFactor() {
  return clampAverageFactor(g_average_factor);
}

void poll() {
  if ((g_status.flags & telem::kReplayStatusFlagActive) == 0U) return;

  const uint32_t now_us = micros();
  if (g_next_send_us != 0U && (int32_t)(now_us - g_next_send_us) < 0) return;

  if (!g_have_pending && !loadNextRecord()) return;

  const uint16_t batch_limit = replayBatchRecordLimit();
  const bool use_timestamp_window = (g_have_pending && g_pending.t_us != 0U);
  const uint32_t batch_start_t_us = use_timestamp_window ? g_pending.t_us : 0U;
  const uint32_t batch_end_t_us = batch_start_t_us + kReplayBatchPeriodUs;

  uint16_t sent_in_batch = 0U;
  while ((g_status.flags & telem::kReplayStatusFlagActive) != 0U && sent_in_batch < batch_limit) {
    if (!g_have_pending && !loadNextRecord()) break;
    if (use_timestamp_window && sent_in_batch != 0U && g_pending.t_us != 0U &&
        (int32_t)(g_pending.t_us - batch_end_t_us) >= 0) {
      break;
    }
    if (!sendPendingRecord()) {
      g_status.flags &= (uint8_t)~telem::kReplayStatusFlagActive;
      markStatusChanged();
      break;
    }
    sent_in_batch++;
  }

  g_next_send_us = now_us + kReplayBatchPeriodUs;
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
  g_prefer_replay_input = preferReplayInputSource();
  g_metadata_source_rate_hz = findReplaySourceRateHz();
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
  g_prefer_replay_input = preferReplayInputSource();
  g_metadata_source_rate_hz = findReplaySourceRateHz();
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
  bool ok = false;
  portENTER_CRITICAL(&g_output_source_mux);
  if (g_output_source_count != 0U) {
    seq = g_output_source_queue[g_output_source_tail].seq;
    t_us = g_output_source_queue[g_output_source_tail].t_us;
    g_output_source_tail = (uint16_t)((g_output_source_tail + 1U) % kOutputSourceDepth);
    g_output_source_count--;
    ok = true;
  }
  portEXIT_CRITICAL(&g_output_source_mux);
  return ok;
}

}  // namespace replay_bridge
