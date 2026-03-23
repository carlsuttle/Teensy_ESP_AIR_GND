#include "log_store.h"

#include <Arduino.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

#include "sd_backend.h"

namespace log_store {
namespace {

constexpr char LOG_DIR[] = "/logs";
constexpr char kBinaryExt[] = ".tlog";
constexpr size_t kBlockBytes = 10000U;
constexpr size_t kBlockCount = 4U;
constexpr uint32_t kMaxReportableFreeBytes = telem::kLogBytesUnknown - 1U;
constexpr uint32_t kLogMagic = 0x4C4F4731UL;  // "LOG1"
constexpr uint16_t kLogVersion = 2U;
constexpr uint8_t kWriteRetryCount = 8U;
constexpr uint32_t kWriteRetryDelayMs = 2U;

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

struct Block {
  uint8_t data[kBlockBytes];
  size_t used_bytes = 0;
  uint32_t record_count = 0;
};

struct LockGuard {
  SemaphoreHandle_t handle = nullptr;
  bool locked = false;

  explicit LockGuard(SemaphoreHandle_t h) : handle(h) {
    if (handle) locked = xSemaphoreTake(handle, portMAX_DELAY) == pdTRUE;
  }

  ~LockGuard() {
    if (locked && handle) xSemaphoreGive(handle);
  }
};

AppConfig g_cfg = {};
File g_file;
String g_current_name;
TaskHandle_t g_writer_task = nullptr;
SemaphoreHandle_t g_state_mutex = nullptr;
QueueHandle_t g_free_block_queue = nullptr;
QueueHandle_t g_full_block_queue = nullptr;
Stats g_stats = {};
RecorderStatus g_recorder = {};
Block g_blocks[kBlockCount] = {};
int g_active_block_index = -1;
bool g_close_pending = false;
uint32_t g_last_seq = 0U;
uint32_t g_last_status_refresh_ms = 0U;
portMUX_TYPE g_stats_mux = portMUX_INITIALIZER_UNLOCKED;

void recordDuration(uint32_t elapsed_ms, uint32_t& last_ms, uint32_t& max_ms) {
  last_ms = elapsed_ms;
  if (elapsed_ms > max_ms) max_ms = elapsed_ms;
}

void updateQueueCur() {
  const uint32_t q = g_full_block_queue ? (uint32_t)uxQueueMessagesWaiting(g_full_block_queue) : 0U;
  portENTER_CRITICAL(&g_stats_mux);
  g_stats.queue_cur = q;
  if (q > g_stats.queue_max) g_stats.queue_max = q;
  portEXIT_CRITICAL(&g_stats_mux);
}

void noteFreeDepth(UBaseType_t free_depth) {
  portENTER_CRITICAL(&g_stats_mux);
  if (g_stats.min_free_blocks_seen == 0U || free_depth < g_stats.min_free_blocks_seen) {
    g_stats.min_free_blocks_seen = (uint32_t)free_depth;
  }
  portEXIT_CRITICAL(&g_stats_mux);
}

void refreshBackendStatus(bool force = false) {
  const uint32_t now = millis();
  if (!force && (uint32_t)(now - g_last_status_refresh_ms) < 1000U) return;
  g_last_status_refresh_ms = now;

  sd_backend::Status backend = {};
  bool ready = false;
  if (g_recorder.feature_enabled) {
    if (sd_backend::mounted()) {
      ready = sd_backend::refreshStatus(backend);
    } else if (force || g_recorder.active) {
      ready = sd_backend::begin(&backend);
    }
  }

  g_recorder.backend_ready = ready;
  g_recorder.media_present = ready && backend.card_type != CARD_NONE;
  g_recorder.init_hz = backend.init_hz;
  const uint64_t total_bytes = backend.total_bytes ? backend.total_bytes : backend.card_size_bytes;
  if (ready && total_bytes >= backend.used_bytes) {
    const uint64_t free_bytes = total_bytes - backend.used_bytes;
    g_recorder.free_bytes =
        (free_bytes > (uint64_t)kMaxReportableFreeBytes) ? kMaxReportableFreeBytes : (uint32_t)free_bytes;
  } else {
    g_recorder.free_bytes = telem::kLogBytesUnknown;
  }
}

String makeLogName(uint32_t session_id) {
  return String(LOG_DIR) + "/air_" + String(session_id ? session_id : 1U) + "_" + String(millis()) + kBinaryExt;
}

String normalizeLogPath(const String& path) {
  if (path.startsWith("/")) return path;
  return String(LOG_DIR) + "/" + path;
}

String makeCsvName(const String& log_path) {
  if (!log_path.endsWith(kBinaryExt)) return log_path + ".csv";
  return log_path.substring(0, log_path.length() - (int)strlen(kBinaryExt)) + ".csv";
}

String makeSiblingName(const String& log_path, const char* suffix, const char* ext = kBinaryExt) {
  String full = normalizeLogPath(log_path);
  String stem = full;
  if (stem.endsWith(kBinaryExt)) {
    stem.remove(stem.length() - (int)strlen(kBinaryExt));
  }
  stem += suffix;
  stem += ext;
  return stem;
}

void appendCsvSep(String& line) {
  if (!line.isEmpty()) line += ',';
}

void appendCsvText(String& line, const char* value) {
  appendCsvSep(line);
  if (value) line += value;
}

void appendCsvText(String& line, const String& value) {
  appendCsvSep(line);
  line += value;
}

void appendCsvU32(String& line, uint32_t value) {
  appendCsvSep(line);
  line += String(value);
}

void appendCsvI32(String& line, int32_t value) {
  appendCsvSep(line);
  line += String(value);
}

void appendCsvU16(String& line, uint16_t value) {
  appendCsvSep(line);
  line += String((uint32_t)value);
}

void appendCsvFloat(String& line, float value, uint8_t precision = 6U) {
  appendCsvSep(line);
  line += String(value, (unsigned int)precision);
}

void appendCsvBlank(String& line) {
  appendCsvSep(line);
}

String hexString(const uint8_t* data, size_t len) {
  static const char kHex[] = "0123456789ABCDEF";
  String out;
  out.reserve(len * 2U);
  for (size_t i = 0; i < len; ++i) {
    out += kHex[(data[i] >> 4) & 0x0F];
    out += kHex[data[i] & 0x0F];
  }
  return out;
}

struct FusionReplayDiagDecoded {
  float accel_body_x_mps2 = 0.0f;
  float accel_body_y_mps2 = 0.0f;
  float accel_body_z_mps2 = 0.0f;
  float mag_fusion_x = 0.0f;
  float mag_fusion_y = 0.0f;
  float mag_fusion_z = 0.0f;
  float accel_error_deg = 0.0f;
  float mag_error_deg = 0.0f;
};

FusionReplayDiagDecoded decodeFusionReplayDiag(const telem::TelemetryFullStateV1& state) {
  FusionReplayDiagDecoded out = {};
  int16_t packed[7] = {};
  memcpy(packed, state.reserved1, sizeof(packed));
  out.accel_body_x_mps2 = (float)packed[0] * 0.001f;
  out.accel_body_y_mps2 = (float)packed[1] * 0.001f;
  out.accel_body_z_mps2 = (float)packed[2] * 0.001f;
  out.mag_fusion_x = (float)packed[3] * 0.01f;
  out.mag_fusion_y = (float)packed[4] * 0.01f;
  out.mag_fusion_z = (float)packed[5] * 0.01f;
  out.accel_error_deg = (float)(uint16_t)packed[6] * 0.01f;
  out.mag_error_deg = (float)state.reserved0 * 0.01f;
  return out;
}

const char* recordKindName(uint16_t kind) {
  switch ((telem::LogRecordKind)kind) {
    case telem::LogRecordKind::State160: return "state160";
    case telem::LogRecordKind::ReplayControl160: return "replay_control160";
    default: return "unknown";
  }
}

bool writeCsvHeader(File& csv) {
  static constexpr char kHeader[] =
      "record_kind_name,record_kind,seq,t_us,"
      "roll_deg,pitch_deg,yaw_deg,mag_heading_deg,"
      "iTOW_ms,fixType,numSV,lat_1e7,lon_1e7,hMSL_mm,gSpeed_mms,headMot_1e5deg,hAcc_mm,sAcc_mms,"
      "gps_parse_errors,mirror_tx_ok,mirror_drop_count,last_gps_ms,last_imu_ms,last_baro_ms,"
      "baro_temp_c,baro_press_hpa,baro_alt_m,baro_vsi_mps,"
      "fusion_gain,fusion_accel_rej,fusion_mag_rej,fusion_recovery_period,flags,"
      "accel_x_mps2,accel_y_mps2,accel_z_mps2,gyro_x_dps,gyro_y_dps,gyro_z_dps,"
      "mag_x_uT,mag_y_uT,mag_z_uT,raw_present_mask,"
      "fusion_diag_mag_error_deg,fusion_diag_accel_error_deg,"
      "fusion_diag_accel_body_x_mps2,fusion_diag_accel_body_y_mps2,fusion_diag_accel_body_z_mps2,"
      "fusion_diag_mag_fusion_x,fusion_diag_mag_fusion_y,fusion_diag_mag_fusion_z,"
      "reserved0,reserved1_hex,"
      "command_id,payload_len,command_seq,received_t_us,apply_flags,control_payload_hex\r\n";
  return csv.print(kHeader) == (sizeof(kHeader) - 1U);
}

bool writeStateCsvRow(File& csv, const BinaryLogRecordV2& record) {
  telem::TelemetryFullStateV1 state = {};
  memcpy(&state, record.payload, sizeof(state));
  const FusionReplayDiagDecoded diag = decodeFusionReplayDiag(state);

  String line;
  line.reserve(768);
  appendCsvText(line, recordKindName(record.record_kind));
  appendCsvU16(line, record.record_kind);
  appendCsvU32(line, record.seq);
  appendCsvU32(line, record.t_us);
  appendCsvFloat(line, state.roll_deg);
  appendCsvFloat(line, state.pitch_deg);
  appendCsvFloat(line, state.yaw_deg);
  appendCsvFloat(line, state.mag_heading_deg);
  appendCsvU32(line, state.iTOW_ms);
  appendCsvU32(line, state.fixType);
  appendCsvU32(line, state.numSV);
  appendCsvI32(line, state.lat_1e7);
  appendCsvI32(line, state.lon_1e7);
  appendCsvI32(line, state.hMSL_mm);
  appendCsvI32(line, state.gSpeed_mms);
  appendCsvI32(line, state.headMot_1e5deg);
  appendCsvU32(line, state.hAcc_mm);
  appendCsvU32(line, state.sAcc_mms);
  appendCsvU32(line, state.gps_parse_errors);
  appendCsvU32(line, state.mirror_tx_ok);
  appendCsvU32(line, state.mirror_drop_count);
  appendCsvU32(line, state.last_gps_ms);
  appendCsvU32(line, state.last_imu_ms);
  appendCsvU32(line, state.last_baro_ms);
  appendCsvFloat(line, state.baro_temp_c);
  appendCsvFloat(line, state.baro_press_hpa);
  appendCsvFloat(line, state.baro_alt_m);
  appendCsvFloat(line, state.baro_vsi_mps);
  appendCsvFloat(line, state.fusion_gain);
  appendCsvFloat(line, state.fusion_accel_rej);
  appendCsvFloat(line, state.fusion_mag_rej);
  appendCsvU16(line, state.fusion_recovery_period);
  appendCsvU16(line, state.flags);
  appendCsvFloat(line, state.accel_x_mps2);
  appendCsvFloat(line, state.accel_y_mps2);
  appendCsvFloat(line, state.accel_z_mps2);
  appendCsvFloat(line, state.gyro_x_dps);
  appendCsvFloat(line, state.gyro_y_dps);
  appendCsvFloat(line, state.gyro_z_dps);
  appendCsvFloat(line, state.mag_x_uT);
  appendCsvFloat(line, state.mag_y_uT);
  appendCsvFloat(line, state.mag_z_uT);
  appendCsvU16(line, state.raw_present_mask);
  appendCsvFloat(line, diag.mag_error_deg);
  appendCsvFloat(line, diag.accel_error_deg);
  appendCsvFloat(line, diag.accel_body_x_mps2);
  appendCsvFloat(line, diag.accel_body_y_mps2);
  appendCsvFloat(line, diag.accel_body_z_mps2);
  appendCsvFloat(line, diag.mag_fusion_x);
  appendCsvFloat(line, diag.mag_fusion_y);
  appendCsvFloat(line, diag.mag_fusion_z);
  appendCsvU16(line, state.reserved0);
  appendCsvText(line, hexString(state.reserved1, sizeof(state.reserved1)));
  for (uint8_t i = 0U; i < 6U; ++i) appendCsvBlank(line);
  line += "\r\n";
  return csv.print(line) == line.length();
}

bool writeReplayControlCsvRow(File& csv, const BinaryLogRecordV2& record) {
  telem::ReplayControlRecord160 control = {};
  memcpy(&control, record.payload, sizeof(control));

  String line;
  line.reserve(768);
  appendCsvText(line, recordKindName(record.record_kind));
  appendCsvU16(line, record.record_kind);
  appendCsvU32(line, record.seq);
  appendCsvU32(line, record.t_us);
  for (uint8_t i = 0U; i < 41U; ++i) appendCsvBlank(line);
  appendCsvU16(line, control.payload.command_id);
  appendCsvU16(line, control.payload.payload_len);
  appendCsvU32(line, control.payload.command_seq);
  appendCsvU32(line, control.payload.received_t_us);
  appendCsvU32(line, control.payload.apply_flags);
  appendCsvText(line, hexString(control.payload.payload, sizeof(control.payload.payload)));
  line += "\r\n";
  return csv.print(line) == line.length();
}

bool writeUnknownCsvRow(File& csv, const BinaryLogRecordV2& record) {
  String line;
  line.reserve(768);
  appendCsvText(line, recordKindName(record.record_kind));
  appendCsvU16(line, record.record_kind);
  appendCsvU32(line, record.seq);
  appendCsvU32(line, record.t_us);
  for (uint8_t i = 0U; i < 47U; ++i) appendCsvBlank(line);
  appendCsvText(line, hexString(record.payload, sizeof(record.payload)));
  line += "\r\n";
  return csv.print(line) == line.length();
}

size_t readExact(File& src, uint8_t* dst, size_t wanted) {
  size_t total = 0U;
  while (total < wanted) {
    const size_t got = src.read(dst + total, wanted - total);
    if (got == 0U) break;
    total += got;
  }
  return total;
}

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8U; ++bit) {
      crc = (crc & 1U) ? (crc >> 1U) ^ 0xEDB88320UL : (crc >> 1U);
    }
  }
  return ~crc;
}

bool nextLogName(File& dir, String& out_name) {
  out_name = "";
  File f = dir.openNextFile();
  while (f) {
    const bool is_dir = f.isDirectory();
    const String name = String(f.name());
    const uint32_t file_size = (uint32_t)f.size();
    f.close();
    if (!is_dir && name.endsWith(kBinaryExt) && file_size >= sizeof(BinaryLogRecordV2)) {
      out_name = normalizeLogPath(name);
      return true;
    }
    f = dir.openNextFile();
  }
  return false;
}

bool computeLogDigest(File& file, uint32_t& size_bytes, uint32_t& records, uint32_t& crc32) {
  size_bytes = 0U;
  records = 0U;
  crc32 = 0U;

  BinaryLogRecordV2 record = {};
  for (;;) {
    const size_t got = readExact(file, (uint8_t*)&record, sizeof(record));
    if (got == 0U) return true;
    if (got != sizeof(record)) return false;
    if (record.magic != kLogMagic || record.version != kLogVersion || record.record_size != sizeof(record)) return false;
    crc32 = crc32Update(crc32, (const uint8_t*)&record, sizeof(record));
    size_bytes += sizeof(record);
    records++;
  }
}

bool copyFileRaw(File& src, File& dst) {
  uint8_t buffer[512];
  for (;;) {
    const size_t got = src.read(buffer, sizeof(buffer));
    if (got == 0U) return true;
    size_t written_total = 0U;
    while (written_total < got) {
      const size_t written = dst.write(buffer + written_total, got - written_total);
      if (written == 0U) return false;
      written_total += written;
    }
  }
}

bool copyFileAndDigest(File& src, File& dst,
                       uint32_t& src_size, uint32_t& src_records, uint32_t& src_crc32,
                       uint32_t& dst_size, uint32_t& dst_records, uint32_t& dst_crc32) {
  src_size = 0U;
  src_records = 0U;
  src_crc32 = 0U;
  dst_size = 0U;
  dst_records = 0U;
  dst_crc32 = 0U;

  BinaryLogRecordV2 record = {};
  for (;;) {
    const size_t got = readExact(src, (uint8_t*)&record, sizeof(record));
    if (got == 0U) return true;
    if (got != sizeof(record)) return false;
    if (record.magic != kLogMagic || record.version != kLogVersion || record.record_size != sizeof(record)) return false;

    const size_t written = dst.write((const uint8_t*)&record, sizeof(record));
    if (written != sizeof(record)) return false;

    src_crc32 = crc32Update(src_crc32, (const uint8_t*)&record, sizeof(record));
    dst_crc32 = crc32Update(dst_crc32, (const uint8_t*)&record, sizeof(record));
    src_size += sizeof(record);
    dst_size += sizeof(record);
    src_records++;
    dst_records++;
  }
}

bool exportSingleLogToCsv(File& src, File& csv, uint32_t& rows, uint32_t& unsupported_rows) {
  if (!writeCsvHeader(csv)) return false;

  BinaryLogRecordV2 record = {};
  for (;;) {
    const size_t got = readExact(src, (uint8_t*)&record, sizeof(record));
    if (got == 0U) return true;
    if (got != sizeof(record)) return false;
    if (record.magic != kLogMagic || record.version != kLogVersion || record.record_size != sizeof(record)) return false;

    bool ok = false;
    switch ((telem::LogRecordKind)record.record_kind) {
      case telem::LogRecordKind::State160:
        ok = writeStateCsvRow(csv, record);
        break;
      case telem::LogRecordKind::ReplayControl160:
        ok = writeReplayControlCsvRow(csv, record);
        break;
      default:
        unsupported_rows++;
        ok = writeUnknownCsvRow(csv, record);
        break;
    }
    if (!ok) return false;
    rows++;
  }
}

enum class ReadStateResult : uint8_t {
  Ok,
  Eof,
  Error,
};

ReadStateResult readNextStateRecord(File& file, BinaryLogRecordV2& record, telem::TelemetryFullStateV1& state,
                                    uint32_t& state_count) {
  for (;;) {
    const size_t got = readExact(file, (uint8_t*)&record, sizeof(record));
    if (got == 0U) return ReadStateResult::Eof;
    if (got != sizeof(record)) return ReadStateResult::Error;
    if (record.magic != kLogMagic || record.version != kLogVersion || record.record_size != sizeof(record)) {
      return ReadStateResult::Error;
    }
    if ((telem::LogRecordKind)record.record_kind != telem::LogRecordKind::State160) continue;
    memcpy(&state, record.payload, sizeof(state));
    state_count++;
    return ReadStateResult::Ok;
  }
}

bool floatNear(float a, float b, float tol) {
  return fabsf(a - b) <= tol;
}

float wrappedAngleDiffDeg(float a, float b) {
  float diff = a - b;
  while (diff > 180.0f) diff -= 360.0f;
  while (diff < -180.0f) diff += 360.0f;
  return diff;
}

bool imuMatches(const telem::TelemetryFullStateV1& a, const telem::TelemetryFullStateV1& b) {
  return floatNear(a.accel_x_mps2, b.accel_x_mps2, 0.0025f) &&
         floatNear(a.accel_y_mps2, b.accel_y_mps2, 0.0025f) &&
         floatNear(a.accel_z_mps2, b.accel_z_mps2, 0.0025f) &&
         floatNear(a.gyro_x_dps, b.gyro_x_dps, 0.0025f) &&
         floatNear(a.gyro_y_dps, b.gyro_y_dps, 0.0025f) &&
         floatNear(a.gyro_z_dps, b.gyro_z_dps, 0.0025f);
}

bool magMatches(const telem::TelemetryFullStateV1& a, const telem::TelemetryFullStateV1& b) {
  return floatNear(a.mag_x_uT, b.mag_x_uT, 0.0025f) &&
         floatNear(a.mag_y_uT, b.mag_y_uT, 0.0025f) &&
         floatNear(a.mag_z_uT, b.mag_z_uT, 0.0025f);
}

bool gpsMatches(const telem::TelemetryFullStateV1& a, const telem::TelemetryFullStateV1& b) {
  return a.iTOW_ms == b.iTOW_ms &&
         a.fixType == b.fixType &&
         a.numSV == b.numSV &&
         a.lat_1e7 == b.lat_1e7 &&
         a.lon_1e7 == b.lon_1e7 &&
         a.hMSL_mm == b.hMSL_mm &&
         a.gSpeed_mms == b.gSpeed_mms &&
         a.headMot_1e5deg == b.headMot_1e5deg &&
         a.hAcc_mm == b.hAcc_mm &&
         a.sAcc_mms == b.sAcc_mms;
}

bool baroMatches(const telem::TelemetryFullStateV1& a, const telem::TelemetryFullStateV1& b) {
  return floatNear(a.baro_temp_c, b.baro_temp_c, 0.0025f) &&
         floatNear(a.baro_press_hpa, b.baro_press_hpa, 0.005f) &&
         floatNear(a.baro_alt_m, b.baro_alt_m, 0.0025f) &&
         floatNear(a.baro_vsi_mps, b.baro_vsi_mps, 0.0025f);
}

bool stateCoreMatches(const telem::TelemetryFullStateV1& a, const telem::TelemetryFullStateV1& b) {
  return a.raw_present_mask == b.raw_present_mask &&
         imuMatches(a, b) &&
         magMatches(a, b) &&
         gpsMatches(a, b) &&
         baroMatches(a, b);
}

void resetBlockQueues() {
  g_active_block_index = -1;
  g_close_pending = false;
  if (g_free_block_queue) xQueueReset(g_free_block_queue);
  if (g_full_block_queue) xQueueReset(g_full_block_queue);
  for (int i = 0; i < (int)kBlockCount; ++i) {
    g_blocks[i].used_bytes = 0U;
    g_blocks[i].record_count = 0U;
    if (g_free_block_queue) {
      (void)xQueueSend(g_free_block_queue, &i, 0);
    }
  }
  g_stats.min_free_blocks_seen = kBlockCount;
  updateQueueCur();
}

bool openCurrentLog() {
  if (g_file) return true;
  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present) return false;

  const uint32_t t0 = millis();
  if (!SD.exists(LOG_DIR)) (void)SD.mkdir(LOG_DIR);
  g_current_name = makeLogName(g_recorder.session_id);
  g_file = SD.open(g_current_name, FILE_WRITE);
  portENTER_CRITICAL(&g_stats_mux);
  recordDuration(millis() - t0, g_stats.fs_open_last_ms, g_stats.fs_open_max_ms);
  portEXIT_CRITICAL(&g_stats_mux);
  refreshBackendStatus(false);
  return (bool)g_file;
}

void closeCurrentLog() {
  if (!g_file) return;
  const uint32_t t0 = millis();
  g_file.flush();
  g_file.close();
  portENTER_CRITICAL(&g_stats_mux);
  recordDuration(millis() - t0, g_stats.fs_close_last_ms, g_stats.fs_close_max_ms);
  portEXIT_CRITICAL(&g_stats_mux);
  g_current_name = "";
  refreshBackendStatus(false);
}

void abandonCurrentLog() {
  if (g_file) {
    g_file.close();
  }
  g_current_name = "";
}

void abortSessionNoMedia() {
  g_recorder.active = false;
  g_close_pending = false;
  g_active_block_index = -1;
  abandonCurrentLog();
  resetBlockQueues();
}

bool acquireFreeBlock(int& block_index) {
  if (!g_free_block_queue) return false;
  if (xQueueReceive(g_free_block_queue, &block_index, 0) != pdTRUE) {
    portENTER_CRITICAL(&g_stats_mux);
    g_stats.no_free_block_events++;
    portEXIT_CRITICAL(&g_stats_mux);
    return false;
  }
  g_blocks[block_index].used_bytes = 0U;
  g_blocks[block_index].record_count = 0U;
  noteFreeDepth(uxQueueMessagesWaiting(g_free_block_queue));
  return true;
}

void releaseBlock(int block_index) {
  if (block_index < 0 || block_index >= (int)kBlockCount) return;
  g_blocks[block_index].used_bytes = 0U;
  g_blocks[block_index].record_count = 0U;
  if (g_free_block_queue) {
    (void)xQueueSend(g_free_block_queue, &block_index, 0);
    noteFreeDepth(uxQueueMessagesWaiting(g_free_block_queue));
  }
}

bool queueBlockForWrite(int block_index) {
  if (block_index < 0 || block_index >= (int)kBlockCount || !g_full_block_queue) return false;
  if (xQueueSend(g_full_block_queue, &block_index, 0) != pdTRUE) {
    portENTER_CRITICAL(&g_stats_mux);
    g_stats.blocks_dropped++;
    g_stats.dropped += g_blocks[block_index].record_count;
    portEXIT_CRITICAL(&g_stats_mux);
    releaseBlock(block_index);
    return false;
  }
  updateQueueCur();
  if (g_writer_task) xTaskNotifyGive(g_writer_task);
  return true;
}

bool enqueueRecord(const BinaryLogRecordV2& record) {
  LockGuard lock(g_state_mutex);
  if (!g_recorder.active) return false;

  if (g_active_block_index < 0) {
    if (!acquireFreeBlock(g_active_block_index)) {
      portENTER_CRITICAL(&g_stats_mux);
      g_stats.dropped++;
      portEXIT_CRITICAL(&g_stats_mux);
      return false;
    }
  }

  const size_t record_size = sizeof(record);
  if ((g_blocks[g_active_block_index].used_bytes + record_size) > sizeof(g_blocks[g_active_block_index].data)) {
    const int full_block_index = g_active_block_index;
    g_active_block_index = -1;
    if (!queueBlockForWrite(full_block_index)) {
      portENTER_CRITICAL(&g_stats_mux);
      g_stats.dropped++;
      portEXIT_CRITICAL(&g_stats_mux);
      return false;
    }
    if (!acquireFreeBlock(g_active_block_index)) {
      portENTER_CRITICAL(&g_stats_mux);
      g_stats.dropped++;
      portEXIT_CRITICAL(&g_stats_mux);
      return false;
    }
  }

  Block& block = g_blocks[g_active_block_index];
  memcpy(block.data + block.used_bytes, &record, sizeof(record));
  block.used_bytes += sizeof(record);
  block.record_count++;
  portENTER_CRITICAL(&g_stats_mux);
  g_stats.enqueued++;
  portEXIT_CRITICAL(&g_stats_mux);
  return true;
}

bool writeBlock(Block& block) {
  if (!block.used_bytes) return true;
  if (!openCurrentLog()) return false;

  const uint32_t t0 = millis();
  size_t total_written = 0U;
  while (total_written < block.used_bytes) {
    size_t written = 0U;
    for (uint8_t attempt = 0U; attempt < kWriteRetryCount; ++attempt) {
      written = g_file.write(block.data + total_written, block.used_bytes - total_written);
      if (written > 0U) break;
      delay(kWriteRetryDelayMs);
    }
    if (written == 0U) {
      portENTER_CRITICAL(&g_stats_mux);
      recordDuration(millis() - t0, g_stats.fs_write_last_ms, g_stats.fs_write_max_ms);
      portEXIT_CRITICAL(&g_stats_mux);
      return false;
    }
    total_written += written;
  }

  const uint32_t elapsed_ms = millis() - t0;
  portENTER_CRITICAL(&g_stats_mux);
  g_stats.bytes_written += (uint32_t)block.used_bytes;
  g_stats.records_written += block.record_count;
  g_stats.blocks_written++;
  g_stats.flushes++;
  recordDuration(elapsed_ms, g_stats.fs_write_last_ms, g_stats.fs_write_max_ms);
  if (block.used_bytes > g_stats.max_write_bytes) g_stats.max_write_bytes = (uint32_t)block.used_bytes;
  portEXIT_CRITICAL(&g_stats_mux);

  g_recorder.bytes_written = g_stats.bytes_written;
  block.used_bytes = 0U;
  block.record_count = 0U;
  return true;
}

void writerTask(void* param) {
  (void)param;
  for (;;) {
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    int block_index = -1;
    while (g_full_block_queue && xQueueReceive(g_full_block_queue, &block_index, 0) == pdTRUE) {
      if (block_index < 0 || block_index >= (int)kBlockCount) {
        g_recorder.active = false;
        g_close_pending = true;
        continue;
      }
      if (!writeBlock(g_blocks[block_index])) {
        g_recorder.active = false;
        g_close_pending = true;
      }
      releaseBlock(block_index);
      updateQueueCur();
    }

    if (g_close_pending && (!g_full_block_queue || uxQueueMessagesWaiting(g_full_block_queue) == 0U)) {
      closeCurrentLog();
      g_close_pending = false;
    }
  }
}

}  // namespace

void begin(const AppConfig& cfg, bool enabled) {
  g_cfg = cfg;
  g_stats = {};
  g_recorder = {};
  g_recorder.feature_enabled = enabled;
  g_last_seq = 0U;
  g_last_status_refresh_ms = 0U;

  if (!g_state_mutex) g_state_mutex = xSemaphoreCreateMutex();
  if (!g_free_block_queue) g_free_block_queue = xQueueCreate(kBlockCount, sizeof(int));
  if (!g_full_block_queue) g_full_block_queue = xQueueCreate(kBlockCount, sizeof(int));
  resetBlockQueues();
  refreshBackendStatus(true);

  if (!g_writer_task) {
    xTaskCreatePinnedToCore(writerTask, "log_writer", 6144, nullptr,
                            sd_backend::kSdWriterPriority, &g_writer_task, sd_backend::kSdWriterCore);
  }
}

void setConfig(const AppConfig& cfg) {
  g_cfg = cfg;
}

void setEnabled(bool enabled) {
  if (!enabled) stopSession();
  g_recorder.feature_enabled = enabled;
  refreshBackendStatus(true);
}

bool startSession(uint32_t session_id) {
  LockGuard lock(g_state_mutex);
  if (!g_recorder.feature_enabled) {
    refreshBackendStatus(true);
    return false;
  }
  if (g_recorder.active) return true;

  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present) return false;

  resetStats();
  resetBlockQueues();
  closeCurrentLog();
  g_recorder.session_id = session_id;
  g_recorder.bytes_written = 0U;
  g_last_seq = 0U;
  if (!openCurrentLog()) {
    refreshBackendStatus(true);
    return false;
  }
  g_recorder.active = true;
  return true;
}

void stopSession() {
  LockGuard lock(g_state_mutex);
  if (!g_recorder.active && !g_file) return;

  int final_block = -1;
  if (g_active_block_index >= 0) {
    if (g_blocks[g_active_block_index].used_bytes > 0U) {
      final_block = g_active_block_index;
    } else {
      releaseBlock(g_active_block_index);
    }
    g_active_block_index = -1;
  }

  g_recorder.active = false;
  g_close_pending = true;
  if (final_block >= 0) {
    if (!queueBlockForWrite(final_block)) {
      if (g_writer_task) xTaskNotifyGive(g_writer_task);
    }
  } else {
    if (g_writer_task) xTaskNotifyGive(g_writer_task);
  }
}

void poll() {
  LockGuard lock(g_state_mutex);
  refreshBackendStatus(false);
  if ((!g_recorder.backend_ready || !g_recorder.media_present) &&
      (g_recorder.active || g_close_pending || (bool)g_file)) {
    abortSessionNoMedia();
  }
  g_recorder.bytes_written = g_stats.bytes_written;
  if (!g_recorder.active && g_close_pending && g_writer_task) {
    xTaskNotifyGive(g_writer_task);
  }
}

void enqueueState(uint32_t seq, uint32_t t_us, const telem::TelemetryFullStateV1& state) {
  if (!g_recorder.feature_enabled || !g_recorder.active) return;
  if (seq == g_last_seq) return;
  g_last_seq = seq;

  BinaryLogRecordV2 record = {};
  record.magic = kLogMagic;
  record.version = kLogVersion;
  record.record_size = (uint16_t)sizeof(record);
  record.record_kind = (uint16_t)telem::LogRecordKind::State160;
  record.seq = seq;
  record.t_us = t_us;
  memcpy(record.payload, &state, sizeof(state));
  (void)enqueueRecord(record);
}

void enqueueReplayControl(uint16_t command_id, uint32_t seq, uint32_t t_us,
                          const void* payload, uint16_t payload_len, uint32_t apply_flags) {
  if (!g_recorder.feature_enabled || !g_recorder.active) return;

  telem::ReplayControlRecord160 replay = {};
  replay.hdr.magic = telem::kReplayMagic;
  replay.hdr.version = telem::kReplayVersion;
  replay.hdr.kind = (uint8_t)telem::ReplayRecordKind::Control;
  replay.hdr.flags = 0U;
  replay.hdr.seq = seq;
  replay.hdr.t_us = t_us;
  replay.payload.command_id = command_id;
  replay.payload.payload_len =
      (payload_len > sizeof(replay.payload.payload)) ? (uint16_t)sizeof(replay.payload.payload) : payload_len;
  replay.payload.command_seq = seq;
  replay.payload.received_t_us = t_us;
  replay.payload.apply_flags = apply_flags;
  if (replay.payload.payload_len > 0U && payload) {
    memcpy(replay.payload.payload, payload, replay.payload.payload_len);
  }

  BinaryLogRecordV2 record = {};
  record.magic = kLogMagic;
  record.version = kLogVersion;
  record.record_size = (uint16_t)sizeof(record);
  record.record_kind = (uint16_t)telem::LogRecordKind::ReplayControl160;
  record.seq = seq;
  record.t_us = t_us;
  memcpy(record.payload, &replay, sizeof(replay));
  (void)enqueueRecord(record);
}

bool active() {
  return g_recorder.active;
}

void probeBackend() {
  LockGuard lock(g_state_mutex);
  refreshBackendStatus(true);
}

RecorderStatus recorderStatus() {
  RecorderStatus status = g_recorder;
  status.bytes_written = g_stats.bytes_written;
  return status;
}

Stats stats() {
  Stats copy = {};
  portENTER_CRITICAL(&g_stats_mux);
  copy = g_stats;
  portEXIT_CRITICAL(&g_stats_mux);
  return copy;
}

void resetStats() {
  portENTER_CRITICAL(&g_stats_mux);
  g_stats = {};
  g_stats.min_free_blocks_seen = kBlockCount;
  portEXIT_CRITICAL(&g_stats_mux);
}

String filesJson() {
  if (!sd_backend::mounted()) return "[]";
  String out = "[";
  bool first = true;
  File dir = SD.open(LOG_DIR);
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f) {
      if (!f.isDirectory()) {
        String p = f.name();
        if (p.startsWith("/logs/")) p = p.substring(6);
        if (!first) out += ',';
        first = false;
        out += "{\"name\":\"";
        out += p;
        out += "\",\"size\":";
        out += String((uint32_t)f.size());
        out += "}";
      }
      f = dir.openNextFile();
    }
  }
  out += "]";
  return out;
}

bool listFiles(telem::LogFileInfoV1* out_files,
               uint16_t max_files,
               uint16_t offset,
               uint16_t& total_files,
               uint16_t& returned_files) {
  total_files = 0U;
  returned_files = 0U;

  LockGuard lock(g_state_mutex);
  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present || !sd_backend::mounted()) {
    return false;
  }

  File dir = SD.open(LOG_DIR);
  if (!dir || !dir.isDirectory()) return false;

  String candidate_name;
  while (nextLogName(dir, candidate_name)) {
    const String short_name = candidate_name.startsWith("/logs/") ? candidate_name.substring(6) : candidate_name;
    File file = SD.open(candidate_name, FILE_READ);
    const uint32_t size_bytes = file ? (uint32_t)file.size() : 0U;
    if (file) file.close();

    if (total_files >= offset && returned_files < max_files && out_files) {
      telem::LogFileInfoV1& entry = out_files[returned_files];
      memset(&entry, 0, sizeof(entry));
      entry.size_bytes = size_bytes;
      strncpy(entry.name, short_name.c_str(), sizeof(entry.name) - 1U);
      returned_files++;
    }
    total_files++;
  }
  dir.close();
  return true;
}

bool latestLogNameLocked(String& out_name) {
  out_name = "";
  if (!sd_backend::mounted()) return false;
  File dir = SD.open(LOG_DIR);
  if (!dir || !dir.isDirectory()) return false;

  String best_name;
  String candidate_name;
  while (nextLogName(dir, candidate_name)) {
    if (candidate_name > best_name) best_name = candidate_name;
  }
  dir.close();
  out_name = best_name;
  return !out_name.isEmpty();
}

bool largestLogNameLocked(String& out_name) {
  out_name = "";
  if (!sd_backend::mounted()) return false;
  File dir = SD.open(LOG_DIR);
  if (!dir || !dir.isDirectory()) return false;

  uint32_t best_size = 0U;
  String best_name;
  File f = dir.openNextFile();
  while (f) {
    const bool is_dir = f.isDirectory();
    const String name = String(f.name());
    const uint32_t file_size = (uint32_t)f.size();
    f.close();
    if (!is_dir && name.endsWith(kBinaryExt) && file_size >= sizeof(BinaryLogRecordV2)) {
      const String normalized = normalizeLogPath(name);
      if (file_size > best_size || (file_size == best_size && normalized > best_name)) {
        best_size = file_size;
        best_name = normalized;
      }
    }
    f = dir.openNextFile();
  }
  dir.close();
  out_name = best_name;
  return !out_name.isEmpty();
}

bool latestLogNameForSessionLocked(uint32_t session_id, String& out_name) {
  out_name = "";
  if (!sd_backend::mounted()) return false;
  File dir = SD.open(LOG_DIR);
  if (!dir || !dir.isDirectory()) return false;

  const String prefix = String(LOG_DIR) + "/air_" + String(session_id) + "_";
  String best_name;
  String candidate_name;
  while (nextLogName(dir, candidate_name)) {
    if (candidate_name.startsWith(prefix) && candidate_name > best_name) {
      best_name = candidate_name;
    }
  }
  dir.close();
  out_name = best_name;
  return !out_name.isEmpty();
}

bool isSafeName(const String& name) {
  if (name.length() == 0 || name.length() > 96) return false;
  if (name.indexOf("..") >= 0 || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return false;
  if (!name.endsWith(".tlog") && !name.endsWith(".csv") && !name.endsWith(".ndjson")) return false;
  return true;
}

bool deleteFileByName(const String& name) {
  LockGuard lock(g_state_mutex);
  if (g_recorder.active || g_close_pending) return false;
  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present) return false;
  if (!sd_backend::mounted()) return false;
  if (!isSafeName(name)) return false;
  const String full = String(LOG_DIR) + "/" + name;
  if (!SD.exists(full)) return false;
  if (g_current_name == full) {
    closeCurrentLog();
  }
  const uint32_t t0 = millis();
  const bool ok = SD.remove(full);
  portENTER_CRITICAL(&g_stats_mux);
  recordDuration(millis() - t0, g_stats.fs_delete_last_ms, g_stats.fs_delete_max_ms);
  portEXIT_CRITICAL(&g_stats_mux);
  refreshBackendStatus(true);
  return ok;
}

bool renameFileByName(const String& src_name, const String& dst_name) {
  LockGuard lock(g_state_mutex);
  if (g_recorder.active || g_close_pending) return false;
  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present) return false;
  if (!sd_backend::mounted()) return false;
  if (!isSafeName(src_name) || !isSafeName(dst_name)) return false;
  if (src_name == dst_name) return true;

  const String src_full = String(LOG_DIR) + "/" + src_name;
  const String dst_full = String(LOG_DIR) + "/" + dst_name;
  if (src_full == dst_full) return true;
  if (!SD.exists(src_full) || SD.exists(dst_full)) return false;

  if (g_current_name == src_full) {
    closeCurrentLog();
  }
  bool ok = SD.rename(src_full, dst_full);
  if (!ok) {
    File src = SD.open(src_full, FILE_READ);
    if (src) {
      if (SD.exists(dst_full)) (void)SD.remove(dst_full);
      File dst = SD.open(dst_full, FILE_WRITE);
      if (dst) {
        ok = copyFileRaw(src, dst);
        dst.flush();
        dst.close();
        if (ok) {
          ok = SD.remove(src_full);
        } else if (SD.exists(dst_full)) {
          (void)SD.remove(dst_full);
        }
      }
      src.close();
    }
  }
  refreshBackendStatus(true);
  return ok;
}

bool exportAllLogsToCsv(Stream& out) {
  LockGuard lock(g_state_mutex);
  if (g_recorder.active || g_close_pending || g_file) {
    out.println("AIRCSV export_ok=0 reason=logger_busy");
    return false;
  }

  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present || !sd_backend::mounted()) {
    out.println("AIRCSV export_ok=0 reason=backend_not_ready");
    return false;
  }

  File dir = SD.open(LOG_DIR);
  if (!dir || !dir.isDirectory()) {
    out.println("AIRCSV export_ok=0 reason=no_logs_dir");
    return false;
  }

  uint32_t converted = 0U;
  uint32_t failed = 0U;
  uint32_t skipped = 0U;
  uint32_t total_rows = 0U;
  uint32_t unsupported_rows = 0U;

  File src = dir.openNextFile();
  while (src) {
    if (src.isDirectory()) {
      src.close();
      src = dir.openNextFile();
      continue;
    }

    const String src_path = normalizeLogPath(String(src.name()));
    if (!src_path.endsWith(kBinaryExt)) {
      skipped++;
      src.close();
      src = dir.openNextFile();
      continue;
    }

    const String csv_path = makeCsvName(src_path);
    if (SD.exists(csv_path)) (void)SD.remove(csv_path);
    File csv = SD.open(csv_path, FILE_WRITE);
    if (!csv) {
      out.printf("AIRCSV file=%s export_ok=0 reason=open_csv_failed\r\n", src_path.c_str());
      failed++;
      src.close();
      src = dir.openNextFile();
      continue;
    }

    uint32_t file_rows = 0U;
    uint32_t file_unsupported_rows = 0U;
    const bool ok = exportSingleLogToCsv(src, csv, file_rows, file_unsupported_rows);
    csv.flush();
    csv.close();
    src.close();

    if (!ok) {
      out.printf("AIRCSV file=%s export_ok=0 reason=convert_failed\r\n", src_path.c_str());
      failed++;
      if (SD.exists(csv_path)) (void)SD.remove(csv_path);
    } else {
      const String csv_name = csv_path.startsWith("/logs/") ? csv_path.substring(6) : csv_path;
      out.printf("AIRCSV file=%s csv=%s export_ok=1 rows=%lu unsupported=%lu\r\n",
                 src_path.c_str(),
                 csv_name.c_str(),
                 (unsigned long)file_rows,
                 (unsigned long)file_unsupported_rows);
      converted++;
      total_rows += file_rows;
      unsupported_rows += file_unsupported_rows;
    }

    src = dir.openNextFile();
  }

  dir.close();

  out.printf("AIRCSV DONE converted=%lu failed=%lu skipped=%lu rows=%lu unsupported=%lu\r\n",
             (unsigned long)converted,
             (unsigned long)failed,
             (unsigned long)skipped,
             (unsigned long)total_rows,
             (unsigned long)unsupported_rows);
  return failed == 0U;
}

bool busy() {
  LockGuard lock(g_state_mutex);
  return g_recorder.active || g_close_pending || (bool)g_file;
}

String currentFileName() {
  LockGuard lock(g_state_mutex);
  return g_current_name;
}

bool latestLogName(String& out_name) {
  LockGuard lock(g_state_mutex);
  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present || !sd_backend::mounted()) {
    out_name = "";
    return false;
  }
  return latestLogNameLocked(out_name);
}

bool largestLogName(String& out_name) {
  LockGuard lock(g_state_mutex);
  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present || !sd_backend::mounted()) {
    out_name = "";
    return false;
  }
  return largestLogNameLocked(out_name);
}

bool latestLogNameForSession(uint32_t session_id, String& out_name) {
  LockGuard lock(g_state_mutex);
  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present || !sd_backend::mounted()) {
    out_name = "";
    return false;
  }
  return latestLogNameForSessionLocked(session_id, out_name);
}

bool copyLatestLogAndVerify(Stream& out) {
  LockGuard lock(g_state_mutex);
  if (g_recorder.active || g_close_pending || g_file) {
    out.println("AIRVERIFY ok=0 reason=logger_busy");
    return false;
  }

  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present || !sd_backend::mounted()) {
    out.println("AIRVERIFY ok=0 reason=backend_not_ready");
    return false;
  }

  String src_path;
  if (!latestLogNameLocked(src_path)) {
    out.println("AIRVERIFY ok=0 reason=no_log_found");
    return false;
  }

  const String dst_path = makeSiblingName(src_path, "_copy");
  if (SD.exists(dst_path)) (void)SD.remove(dst_path);

  File src = SD.open(src_path, FILE_READ);
  if (!src) {
    out.printf("AIRVERIFY ok=0 file=%s reason=open_src_failed\r\n", src_path.c_str());
    return false;
  }

  File dst = SD.open(dst_path, FILE_WRITE);
  if (!dst) {
    src.close();
    out.printf("AIRVERIFY ok=0 file=%s reason=open_dst_failed\r\n", dst_path.c_str());
    return false;
  }

  uint32_t src_size = 0U;
  uint32_t src_records = 0U;
  uint32_t src_crc32 = 0U;
  uint32_t dst_size = 0U;
  uint32_t dst_records = 0U;
  uint32_t dst_crc32 = 0U;
  const bool ok = copyFileAndDigest(src, dst, src_size, src_records, src_crc32, dst_size, dst_records, dst_crc32);
  dst.flush();
  dst.close();
  src.close();

  if (!ok) {
    if (SD.exists(dst_path)) (void)SD.remove(dst_path);
    out.printf("AIRVERIFY ok=0 file=%s reason=copy_failed\r\n", src_path.c_str());
    return false;
  }

  const bool match = (src_size == dst_size) && (src_records == dst_records) && (src_crc32 == dst_crc32);
  const String src_name = src_path.startsWith("/logs/") ? src_path.substring(6) : src_path;
  const String dst_name = dst_path.startsWith("/logs/") ? dst_path.substring(6) : dst_path;
  out.printf(
      "AIRVERIFY src=%s copy=%s size=%lu records=%lu crc_src=%08lX crc_copy=%08lX match=%u\r\n",
      src_name.c_str(),
      dst_name.c_str(),
      (unsigned long)src_size,
      (unsigned long)src_records,
      (unsigned long)src_crc32,
      (unsigned long)dst_crc32,
      match ? 1U : 0U);
  return match;
}

bool compareLogs(Stream& out, const String& src_name, const String& dst_name) {
  LockGuard lock(g_state_mutex);
  if (g_recorder.active || g_close_pending || g_file) {
    out.println("AIRCOMPARE ok=0 reason=logger_busy");
    return false;
  }

  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present || !sd_backend::mounted()) {
    out.println("AIRCOMPARE ok=0 reason=backend_not_ready");
    return false;
  }

  const String src_path = normalizeLogPath(src_name);
  const String dst_path = normalizeLogPath(dst_name);
  File src = SD.open(src_path, FILE_READ);
  File dst = SD.open(dst_path, FILE_READ);
  if (!src || !dst) {
    if (src) src.close();
    if (dst) dst.close();
    out.printf("AIRCOMPARE ok=0 reason=open_failed src=%s dst=%s\r\n", src_path.c_str(), dst_path.c_str());
    return false;
  }

  BinaryLogRecordV2 src_record = {};
  BinaryLogRecordV2 dst_record = {};
  telem::TelemetryFullStateV1 src_state = {};
  telem::TelemetryFullStateV1 dst_state = {};
  uint32_t src_states = 0U;
  uint32_t dst_states = 0U;

  const ReadStateResult src_first = readNextStateRecord(src, src_record, src_state, src_states);
  if (src_first != ReadStateResult::Ok) {
    src.close();
    dst.close();
    out.printf("AIRCOMPARE ok=0 reason=no_source_state src=%s\r\n", src_path.c_str());
    return false;
  }

  uint32_t dst_prefix_skip = 0U;
  bool aligned = false;
  for (;;) {
    const ReadStateResult dst_read = readNextStateRecord(dst, dst_record, dst_state, dst_states);
    if (dst_read != ReadStateResult::Ok) break;
    if (stateCoreMatches(src_state, dst_state)) {
      aligned = true;
      break;
    }
    dst_prefix_skip++;
  }

  if (!aligned) {
    src.close();
    dst.close();
    out.printf("AIRCOMPARE ok=0 reason=no_alignment src=%s dst=%s\r\n", src_path.c_str(), dst_path.c_str());
    return false;
  }

  uint32_t compared = 0U;
  uint32_t seq_exact = 0U;
  uint32_t seq_mismatch = 0U;
  uint32_t ts_exact = 0U;
  uint32_t ts_mismatch = 0U;
  uint32_t max_abs_delta_us = 0U;
  uint32_t imu_mismatch = 0U;
  uint32_t mag_mismatch = 0U;
  uint32_t gps_mismatch = 0U;
  uint32_t baro_mismatch = 0U;
  uint32_t mask_mismatch = 0U;
  uint32_t first_ts_index = 0U;
  uint32_t first_ts_src = 0U;
  uint32_t first_ts_dst = 0U;
  uint32_t first_raw_index = 0U;
  uint32_t first_raw_src = 0U;
  uint32_t first_raw_dst = 0U;
  double roll_abs_sum = 0.0;
  double pitch_abs_sum = 0.0;
  double yaw_abs_sum = 0.0;
  double mag_heading_abs_sum = 0.0;
  double accel_error_sum = 0.0;
  double mag_error_sum = 0.0;
  double accel_input_x_abs_sum = 0.0;
  double accel_input_y_abs_sum = 0.0;
  double accel_input_z_abs_sum = 0.0;
  float roll_abs_max = 0.0f;
  float pitch_abs_max = 0.0f;
  float yaw_abs_max = 0.0f;
  float mag_heading_abs_max = 0.0f;
  float accel_error_max = 0.0f;
  float mag_error_max = 0.0f;
  float accel_input_x_abs_max = 0.0f;
  float accel_input_y_abs_max = 0.0f;
  float accel_input_z_abs_max = 0.0f;
  uint32_t accel_ignored_count = 0U;
  uint32_t mag_ignored_count = 0U;
  uint32_t accel_error_flag_count = 0U;
  uint32_t mag_error_flag_count = 0U;

  bool have_pair = true;
  while (have_pair) {
    compared++;

    if (src_record.seq == dst_record.seq) {
      seq_exact++;
    } else {
      seq_mismatch++;
    }

    const uint32_t delta = (src_record.t_us > dst_record.t_us) ? (src_record.t_us - dst_record.t_us)
                                                               : (dst_record.t_us - src_record.t_us);
    if (delta > max_abs_delta_us) max_abs_delta_us = delta;
    if (src_record.t_us == dst_record.t_us) {
      ts_exact++;
    } else {
      ts_mismatch++;
      if (first_ts_index == 0U) {
        first_ts_index = compared;
        first_ts_src = src_record.t_us;
        first_ts_dst = dst_record.t_us;
      }
    }

    const bool imu_ok = imuMatches(src_state, dst_state);
    const bool mag_ok = magMatches(src_state, dst_state);
    const bool gps_ok = gpsMatches(src_state, dst_state);
    const bool baro_ok = baroMatches(src_state, dst_state);
    const bool mask_ok = src_state.raw_present_mask == dst_state.raw_present_mask;
    if (!imu_ok) imu_mismatch++;
    if (!mag_ok) mag_mismatch++;
    if (!gps_ok) gps_mismatch++;
    if (!baro_ok) baro_mismatch++;
    if (!mask_ok) mask_mismatch++;
    if (first_raw_index == 0U && (!imu_ok || !mag_ok || !gps_ok || !baro_ok || !mask_ok)) {
      first_raw_index = compared;
      first_raw_src = src_record.t_us;
      first_raw_dst = dst_record.t_us;
    }

    const float roll_diff = fabsf(src_state.roll_deg - dst_state.roll_deg);
    const float pitch_diff = fabsf(src_state.pitch_deg - dst_state.pitch_deg);
    const float yaw_diff = fabsf(wrappedAngleDiffDeg(src_state.yaw_deg, dst_state.yaw_deg));
    const float mag_heading_diff = fabsf(wrappedAngleDiffDeg(src_state.mag_heading_deg, dst_state.mag_heading_deg));
    const FusionReplayDiagDecoded diag = decodeFusionReplayDiag(dst_state);
    const float accel_input_x_diff = fabsf(src_state.accel_x_mps2 - diag.accel_body_x_mps2);
    const float accel_input_y_diff = fabsf(src_state.accel_y_mps2 - diag.accel_body_y_mps2);
    const float accel_input_z_diff = fabsf(src_state.accel_z_mps2 - diag.accel_body_z_mps2);
    roll_abs_sum += roll_diff;
    pitch_abs_sum += pitch_diff;
    yaw_abs_sum += yaw_diff;
    mag_heading_abs_sum += mag_heading_diff;
    accel_error_sum += diag.accel_error_deg;
    mag_error_sum += diag.mag_error_deg;
    accel_input_x_abs_sum += accel_input_x_diff;
    accel_input_y_abs_sum += accel_input_y_diff;
    accel_input_z_abs_sum += accel_input_z_diff;
    if (roll_diff > roll_abs_max) roll_abs_max = roll_diff;
    if (pitch_diff > pitch_abs_max) pitch_abs_max = pitch_diff;
    if (yaw_diff > yaw_abs_max) yaw_abs_max = yaw_diff;
    if (mag_heading_diff > mag_heading_abs_max) mag_heading_abs_max = mag_heading_diff;
    if (diag.accel_error_deg > accel_error_max) accel_error_max = diag.accel_error_deg;
    if (diag.mag_error_deg > mag_error_max) mag_error_max = diag.mag_error_deg;
    if (accel_input_x_diff > accel_input_x_abs_max) accel_input_x_abs_max = accel_input_x_diff;
    if (accel_input_y_diff > accel_input_y_abs_max) accel_input_y_abs_max = accel_input_y_diff;
    if (accel_input_z_diff > accel_input_z_abs_max) accel_input_z_abs_max = accel_input_z_diff;
    if ((dst_state.flags & telem::kStateFlagFusionAccelerometerIgnored) != 0U) accel_ignored_count++;
    if ((dst_state.flags & telem::kStateFlagFusionMagnetometerIgnored) != 0U) mag_ignored_count++;
    if ((dst_state.flags & telem::kStateFlagFusionAccelerationError) != 0U) accel_error_flag_count++;
    if ((dst_state.flags & telem::kStateFlagFusionMagneticError) != 0U) mag_error_flag_count++;

    const ReadStateResult src_next = readNextStateRecord(src, src_record, src_state, src_states);
    const ReadStateResult dst_next = readNextStateRecord(dst, dst_record, dst_state, dst_states);
    if (src_next != ReadStateResult::Ok || dst_next != ReadStateResult::Ok) {
      break;
    }
  }

  while (readNextStateRecord(src, src_record, src_state, src_states) == ReadStateResult::Ok) {}
  while (readNextStateRecord(dst, dst_record, dst_state, dst_states) == ReadStateResult::Ok) {}

  src.close();
  dst.close();

  const uint32_t src_remaining = (src_states > compared) ? (src_states - compared) : 0U;
  const uint32_t dst_tail_extra = (dst_states > (dst_prefix_skip + compared)) ? (dst_states - dst_prefix_skip - compared) : 0U;
  const bool ok = (ts_mismatch == 0U) && (imu_mismatch == 0U) && (mag_mismatch == 0U) &&
                  (gps_mismatch == 0U) && (baro_mismatch == 0U) && (mask_mismatch == 0U);

  const String src_short = src_path.startsWith("/logs/") ? src_path.substring(6) : src_path;
  const String dst_short = dst_path.startsWith("/logs/") ? dst_path.substring(6) : dst_path;
  out.printf(
      "AIRCOMPARE src=%s dst=%s compared=%lu src_states=%lu dst_states=%lu dst_prefix=%lu dst_tail=%lu seq_exact=%lu seq_mismatch=%lu ts_exact=%lu ts_mismatch=%lu ts_max_delta_us=%lu imu_mismatch=%lu mag_mismatch=%lu gps_mismatch=%lu baro_mismatch=%lu mask_mismatch=%lu ok=%u\r\n",
      src_short.c_str(),
      dst_short.c_str(),
      (unsigned long)compared,
      (unsigned long)src_states,
      (unsigned long)dst_states,
      (unsigned long)dst_prefix_skip,
      (unsigned long)dst_tail_extra,
      (unsigned long)seq_exact,
      (unsigned long)seq_mismatch,
      (unsigned long)ts_exact,
      (unsigned long)ts_mismatch,
      (unsigned long)max_abs_delta_us,
      (unsigned long)imu_mismatch,
      (unsigned long)mag_mismatch,
      (unsigned long)gps_mismatch,
      (unsigned long)baro_mismatch,
      (unsigned long)mask_mismatch,
      ok ? 1U : 0U);
  if (compared != 0U) {
    out.printf(
        "AIRCOMPARE fusion roll_mean_abs=%.6f roll_max_abs=%.6f pitch_mean_abs=%.6f pitch_max_abs=%.6f yaw_mean_abs=%.6f yaw_max_abs=%.6f maghdg_mean_abs=%.6f maghdg_max_abs=%.6f\r\n",
        roll_abs_sum / (double)compared,
        (double)roll_abs_max,
        pitch_abs_sum / (double)compared,
        (double)pitch_abs_max,
        yaw_abs_sum / (double)compared,
        (double)yaw_abs_max,
        mag_heading_abs_sum / (double)compared,
        (double)mag_heading_abs_max);
    out.printf(
        "AIRCOMPARE fusion_diag accel_err_mean=%.6f accel_err_max=%.6f mag_err_mean=%.6f mag_err_max=%.6f accel_ignored=%lu accel_error_flag=%lu mag_ignored=%lu mag_error_flag=%lu accel_input_mean_abs=(%.6f,%.6f,%.6f) accel_input_max_abs=(%.6f,%.6f,%.6f)\r\n",
        accel_error_sum / (double)compared,
        (double)accel_error_max,
        mag_error_sum / (double)compared,
        (double)mag_error_max,
        (unsigned long)accel_ignored_count,
        (unsigned long)accel_error_flag_count,
        (unsigned long)mag_ignored_count,
        (unsigned long)mag_error_flag_count,
        accel_input_x_abs_sum / (double)compared,
        accel_input_y_abs_sum / (double)compared,
        accel_input_z_abs_sum / (double)compared,
        (double)accel_input_x_abs_max,
        (double)accel_input_y_abs_max,
        (double)accel_input_z_abs_max);
  }
  if (first_ts_index != 0U) {
    out.printf("AIRCOMPARE first_ts_mismatch index=%lu src_t_us=%lu dst_t_us=%lu\r\n",
               (unsigned long)first_ts_index,
               (unsigned long)first_ts_src,
               (unsigned long)first_ts_dst);
  }
  if (first_raw_index != 0U) {
    out.printf("AIRCOMPARE first_raw_mismatch index=%lu src_t_us=%lu dst_t_us=%lu\r\n",
               (unsigned long)first_raw_index,
               (unsigned long)first_raw_src,
               (unsigned long)first_raw_dst);
  }
  if (src_remaining != 0U) {
    out.printf("AIRCOMPARE src_remaining=%lu\r\n", (unsigned long)src_remaining);
  }
  return ok;
}

}  // namespace log_store
