#include "log_store.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

#include "sd_api.h"
#include "sd_backend.h"

namespace log_store {
using File = sd_api::File;
namespace {

constexpr char LOG_DIR[] = "/logs";
constexpr char kBinaryExt[] = ".tlog";
constexpr char kSessionMetaPath[] = "/logs/session.meta";
constexpr size_t kBlockBytes = 10000U;
constexpr size_t kBlockCount = 64U;
constexpr size_t kBenchRingDepth = 512U;
constexpr uint32_t kMaxReportableFreeBytes = telem::kLogBytesUnknown - 1U;
constexpr uint32_t kLogMagic = 0x4C4F4731UL;  // "LOG1"
constexpr uint16_t kLogVersion = 2U;
constexpr uint8_t kWriteRetryCount = 8U;
constexpr uint32_t kWriteRetryDelayMs = 2U;
constexpr uint32_t kBenchFlushIntervalMs = 250U;
constexpr uint32_t kMountedStatusRefreshMs = 1000U;
constexpr uint32_t kMissingMediaProbeIntervalMs = 5000U;
constexpr uint32_t kIdleMediaCheckMs = 1000U;

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

#pragma pack(push, 1)
struct SessionMetaRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t next_session_id;
  uint32_t last_closed_session_id;
  char last_closed_name[96];
};
#pragma pack(pop)

constexpr uint32_t kSessionMetaMagic = 0x4D455441UL;  // "META"
constexpr uint16_t kSessionMetaVersion = 1U;

bool parseLogNameParts(const String& path, uint32_t& session_id, uint32_t& stamp) {
  session_id = 0U;
  stamp = 0U;

  String name = path;
  if (name.startsWith("/logs/")) name = name.substring(6);
  const int ext_pos = (int)name.length() - (int)strlen(kBinaryExt);
  if (!name.endsWith(kBinaryExt)) return false;
  const int last_sep = name.lastIndexOf('_', ext_pos - 1);
  if (last_sep <= 0) return false;
  const int second_last_sep = name.lastIndexOf('_', last_sep - 1);
  if (second_last_sep <= 0) return false;

  const String prefix_part = name.substring(0, second_last_sep);
  const String session_part = name.substring(second_last_sep + 1, last_sep);
  const String stamp_part = name.substring(last_sep + 1, ext_pos);
  if (prefix_part.length() == 0 || session_part.length() == 0 || stamp_part.length() == 0) return false;

  session_id = (uint32_t)strtoul(session_part.c_str(), nullptr, 10);
  stamp = (uint32_t)strtoul(stamp_part.c_str(), nullptr, 10);
  return session_id != 0U && stamp != 0U;
}

AppConfig g_cfg = {};
File g_file;
String g_current_name;
String g_last_closed_name;
uint32_t g_last_closed_session_id = 0U;
TaskHandle_t g_writer_task = nullptr;
SemaphoreHandle_t g_state_mutex = nullptr;
QueueHandle_t g_free_block_queue = nullptr;
QueueHandle_t g_full_block_queue = nullptr;
Stats g_stats = {};
RecorderStatus g_recorder = {};
Block* g_blocks = nullptr;
BinaryLogRecordV2* g_bench_ring = nullptr;
uint8_t* g_bench_write_buffer = nullptr;
int g_active_block_index = -1;
bool g_close_pending = false;
uint32_t g_last_seq = 0U;
uint32_t g_last_replay_input_seq = 0U;
size_t g_bench_write_used = 0U;
uint32_t g_bench_write_records = 0U;
uint32_t g_bench_last_write_ms = 0U;
uint32_t g_last_status_refresh_ms = 0U;
uint32_t g_last_probe_attempt_ms = 0U;
uint32_t g_last_idle_media_check_ms = 0U;
bool g_idle_media_checks_enabled = true;
uint16_t g_bench_ring_head = 0U;
uint16_t g_bench_ring_tail = 0U;
struct PendingLogMetadata {
  bool valid = false;
  uint16_t replay_average_factor = 1U;
  uint16_t applied_capture_rate_hz = 0U;
  uint16_t override_mask = 0U;
  uint16_t flags = 0U;
  String source_name;
};
PendingLogMetadata g_next_log_metadata = {};
portMUX_TYPE g_stats_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_bench_ring_mux = portMUX_INITIALIZER_UNLOCKED;

constexpr uint32_t kLogMetadataSchemaVersion = 1U;

void defaultSessionMeta(SessionMetaRecord& meta) {
  memset(&meta, 0, sizeof(meta));
  meta.magic = kSessionMetaMagic;
  meta.version = kSessionMetaVersion;
  meta.next_session_id = 1U;
}

bool loadSessionMeta(SessionMetaRecord& meta) {
  defaultSessionMeta(meta);
  if (!sd_backend::mounted()) return false;
  File f = sd_api::open(kSessionMetaPath);
  if (!f) return false;
  const bool ok = (f.read((uint8_t*)&meta, sizeof(meta)) == (int)sizeof(meta));
  f.close();
  if (!ok || meta.magic != kSessionMetaMagic || meta.version != kSessionMetaVersion) {
    defaultSessionMeta(meta);
    return false;
  }
  if (meta.next_session_id == 0U) meta.next_session_id = 1U;
  meta.last_closed_name[sizeof(meta.last_closed_name) - 1U] = '\0';
  return true;
}

bool saveSessionMeta(const SessionMetaRecord& meta) {
  if (!sd_backend::mounted()) return false;
  if (!sd_api::exists(LOG_DIR)) (void)sd_api::mkdir(LOG_DIR);
  File f = sd_api::open(kSessionMetaPath, sd_api::OpenMode::write);
  if (!f) return false;
  f.seek(0);
  const size_t written = f.write((const uint8_t*)&meta, sizeof(meta));
  f.flush();
  f.close();
  return written == sizeof(meta);
}

void updateSessionMetaClosedLog(uint32_t session_id, const String& closed_name) {
  SessionMetaRecord meta = {};
  (void)loadSessionMeta(meta);
  if (session_id != 0U) {
    meta.last_closed_session_id = session_id;
    const uint32_t next_id = session_id + 1U;
    if (next_id > meta.next_session_id) meta.next_session_id = next_id;
  }
  memset(meta.last_closed_name, 0, sizeof(meta.last_closed_name));
  const String short_name = closed_name.startsWith("/logs/") ? closed_name.substring(6) : closed_name;
  short_name.substring(0, sizeof(meta.last_closed_name) - 1U).toCharArray(meta.last_closed_name, sizeof(meta.last_closed_name));
  (void)saveSessionMeta(meta);
}

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
  const bool logger_busy = g_recorder.active || g_close_pending || (bool)g_file;
  const bool sdcap_busy = false;
  if ((logger_busy || sdcap_busy) && !force) return;
  const uint32_t refresh_interval_ms = sd_backend::mounted() ? kMountedStatusRefreshMs : kMissingMediaProbeIntervalMs;
  if (!force && (uint32_t)(now - g_last_status_refresh_ms) < refresh_interval_ms) return;
  g_last_status_refresh_ms = now;

  sd_backend::Status backend = {};
  bool ready = false;
  if (g_recorder.feature_enabled) {
    if (sd_backend::mounted()) {
      ready = sd_backend::refreshStatus(backend);
    } else if (force || logger_busy) {
      g_last_probe_attempt_ms = now;
      ready = sd_backend::begin(&backend);
    }
  }
  if (ready) g_last_probe_attempt_ms = 0U;

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

String activeRecordPrefix() {
  const AppConfig& cfg = config_store::get();
  return (cfg.record_prefix[0] != '\0') ? String(cfg.record_prefix) : String("air");
}

String makeLogName(uint32_t session_id) {
  return String(LOG_DIR) + "/" + activeRecordPrefix() + "_" + String(session_id ? session_id : 1U) + "_" +
         String(millis()) + kBinaryExt;
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

bool writeCurrentFileBytes(const uint8_t* data, size_t len, uint32_t& elapsed_ms);

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
    case telem::LogRecordKind::ReplayInput160: return "replay_input160";
    case telem::LogRecordKind::Metadata160: return "metadata160";
    default: return "unknown";
  }
}

bool useSimpleBenchWriter() {
  return config_store::get().standalone_bench != 0U && g_bench_ring && g_bench_write_buffer;
}

uint32_t benchRingCountUnsafe() {
  return (uint16_t)(g_bench_ring_head - g_bench_ring_tail);
}

void resetBenchRuntime() {
  portENTER_CRITICAL(&g_bench_ring_mux);
  g_bench_ring_head = 0U;
  g_bench_ring_tail = 0U;
  portEXIT_CRITICAL(&g_bench_ring_mux);
  g_bench_write_used = 0U;
  g_bench_write_records = 0U;
  g_bench_last_write_ms = 0U;
}

bool benchRingPush(const BinaryLogRecordV2& record, uint32_t& q_now) {
  bool ok = false;
  portENTER_CRITICAL(&g_bench_ring_mux);
  const uint32_t q = benchRingCountUnsafe();
  if (q < kBenchRingDepth) {
    g_bench_ring[g_bench_ring_head % kBenchRingDepth] = record;
    g_bench_ring_head = (uint16_t)(g_bench_ring_head + 1U);
    q_now = benchRingCountUnsafe();
    ok = true;
  } else {
    q_now = q;
  }
  portEXIT_CRITICAL(&g_bench_ring_mux);
  return ok;
}

bool benchRingPop(BinaryLogRecordV2& record, uint32_t& q_now) {
  bool ok = false;
  portENTER_CRITICAL(&g_bench_ring_mux);
  if (g_bench_ring_tail != g_bench_ring_head) {
    record = g_bench_ring[g_bench_ring_tail % kBenchRingDepth];
    g_bench_ring_tail = (uint16_t)(g_bench_ring_tail + 1U);
    q_now = benchRingCountUnsafe();
    ok = true;
  } else {
    q_now = 0U;
  }
  portEXIT_CRITICAL(&g_bench_ring_mux);
  return ok;
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
      case telem::LogRecordKind::Metadata160:
        ok = writeUnknownCsvRow(csv, record);
        break;
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

struct CoreComparableState {
  telem::TelemetryFullStateV1 state = {};
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
  if (!g_blocks) return;
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
  if (!sd_api::exists(LOG_DIR)) (void)sd_api::mkdir(LOG_DIR);
  g_current_name = makeLogName(g_recorder.session_id);
  g_file = sd_api::open(g_current_name, sd_api::OpenMode::write);
  portENTER_CRITICAL(&g_stats_mux);
  recordDuration(millis() - t0, g_stats.fs_open_last_ms, g_stats.fs_open_max_ms);
  portEXIT_CRITICAL(&g_stats_mux);
  if (!g_file) {
    g_next_log_metadata = {};
    return false;
  }

  telem::LogMetadataPayloadV1 metadata = {};
  metadata.schema_version = kLogMetadataSchemaVersion;
  metadata.file_session_id = g_recorder.session_id;
  metadata.replay_average_factor = g_next_log_metadata.valid ? g_next_log_metadata.replay_average_factor : 1U;
  metadata.applied_capture_rate_hz = g_next_log_metadata.valid ? g_next_log_metadata.applied_capture_rate_hz
                                                               : config_store::get().source_rate_hz;
  metadata.override_mask = g_next_log_metadata.valid ? g_next_log_metadata.override_mask : 0U;
  metadata.flags = g_next_log_metadata.valid ? g_next_log_metadata.flags : 0U;

  if (g_next_log_metadata.valid) {
    uint32_t parent_session_id = 0U;
    uint32_t parent_stamp = 0U;
    const String normalized_source = normalizeLogPath(g_next_log_metadata.source_name);
    if (parseLogNameParts(normalized_source, parent_session_id, parent_stamp)) {
      metadata.parent_session_id = parent_session_id;
    }
    const String short_source_name = g_next_log_metadata.source_name.startsWith("/logs/")
                                         ? g_next_log_metadata.source_name.substring(6)
                                         : g_next_log_metadata.source_name;
    short_source_name.substring(0, sizeof(metadata.source_name) - 1U)
        .toCharArray(metadata.source_name, sizeof(metadata.source_name));
  }

  const String short_current_name = g_current_name.startsWith("/logs/") ? g_current_name.substring(6) : g_current_name;
  short_current_name.substring(0, sizeof(metadata.file_name) - 1U)
      .toCharArray(metadata.file_name, sizeof(metadata.file_name));

  BinaryLogRecordV2 record = {};
  record.magic = kLogMagic;
  record.version = kLogVersion;
  record.record_size = (uint16_t)sizeof(record);
  record.record_kind = (uint16_t)telem::LogRecordKind::Metadata160;
  memcpy(record.payload, &metadata, sizeof(metadata));

  uint32_t elapsed_ms = 0U;
  const bool wrote_metadata = writeCurrentFileBytes((const uint8_t*)&record, sizeof(record), elapsed_ms);
  portENTER_CRITICAL(&g_stats_mux);
  recordDuration(elapsed_ms, g_stats.fs_write_last_ms, g_stats.fs_write_max_ms);
  if (wrote_metadata) {
    g_stats.records_written++;
    g_stats.bytes_written += sizeof(record);
    if (sizeof(record) > g_stats.max_write_bytes) g_stats.max_write_bytes = sizeof(record);
  }
  portEXIT_CRITICAL(&g_stats_mux);
  if (wrote_metadata) g_recorder.bytes_written = g_stats.bytes_written;
  g_next_log_metadata = {};
  if (!wrote_metadata) {
    g_file.close();
    g_file = File();
    g_current_name = "";
    return false;
  }
  return true;
}

bool recoverCurrentLogAfterWriteFailure() {
  if (g_current_name.isEmpty()) return false;

  if (g_file) {
    g_file.close();
  }

  sd_backend::end();
  delay(10);

  sd_backend::Status backend = {};
  const bool ready = sd_backend::begin(&backend);
  g_recorder.backend_ready = ready;
  g_recorder.media_present = ready && backend.card_type != CARD_NONE;
  g_recorder.init_hz = ready ? backend.init_hz : 0U;
  if (!ready || !g_recorder.media_present) return false;

  g_file = sd_api::open(g_current_name, sd_api::OpenMode::write);
  return (bool)g_file;
}

bool writeCurrentFileBytes(const uint8_t* data, size_t len, uint32_t& elapsed_ms) {
  elapsed_ms = 0U;
  if (!data || len == 0U) return true;
  if (!openCurrentLog()) return false;

  const uint32_t t0 = millis();
  size_t total_written = 0U;
  while (total_written < len) {
    size_t written = 0U;
    for (uint8_t attempt = 0U; attempt < kWriteRetryCount; ++attempt) {
      written = g_file.write(data + total_written, len - total_written);
      if (written > 0U) break;
      delay(kWriteRetryDelayMs);
    }
    if (written == 0U) {
      if (!recoverCurrentLogAfterWriteFailure()) {
        elapsed_ms = millis() - t0;
        return false;
      }
      continue;
    }
    total_written += written;
  }

  elapsed_ms = millis() - t0;
  return true;
}

bool flushBenchBufferLocked() {
  if (!g_bench_write_used) return true;

  uint32_t elapsed_ms = 0U;
  if (!writeCurrentFileBytes(g_bench_write_buffer, g_bench_write_used, elapsed_ms)) {
    portENTER_CRITICAL(&g_stats_mux);
    recordDuration(elapsed_ms, g_stats.fs_write_last_ms, g_stats.fs_write_max_ms);
    portEXIT_CRITICAL(&g_stats_mux);
    return false;
  }

  portENTER_CRITICAL(&g_stats_mux);
  g_stats.bytes_written += (uint32_t)g_bench_write_used;
  g_stats.records_written += g_bench_write_records;
  g_stats.blocks_written++;
  g_stats.flushes++;
  recordDuration(elapsed_ms, g_stats.fs_write_last_ms, g_stats.fs_write_max_ms);
  if (g_bench_write_used > g_stats.max_write_bytes) g_stats.max_write_bytes = (uint32_t)g_bench_write_used;
  portEXIT_CRITICAL(&g_stats_mux);

  g_recorder.bytes_written = g_stats.bytes_written;
  g_bench_write_used = 0U;
  g_bench_write_records = 0U;
  return true;
}

bool appendBenchRecordLocked(const BinaryLogRecordV2& record) {
  const size_t record_size = sizeof(record);
  if ((g_bench_write_used + record_size) > kBlockBytes) {
    if (!flushBenchBufferLocked()) return false;
  }
  memcpy(g_bench_write_buffer + g_bench_write_used, &record, sizeof(record));
  g_bench_write_used += sizeof(record);
  g_bench_write_records++;
  return true;
}

void closeCurrentLog() {
  if (!g_file) return;
  const uint32_t t0 = millis();
  const String closed_name = g_current_name;
  const uint32_t closed_session_id = g_recorder.session_id;
  g_file.flush();
  g_file.close();
  portENTER_CRITICAL(&g_stats_mux);
  recordDuration(millis() - t0, g_stats.fs_close_last_ms, g_stats.fs_close_max_ms);
  portEXIT_CRITICAL(&g_stats_mux);
  g_current_name = "";
  g_last_closed_name = closed_name;
  g_last_closed_session_id = closed_session_id;
  updateSessionMetaClosedLog(closed_session_id, closed_name);
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
  resetBenchRuntime();
  abandonCurrentLog();
  resetBlockQueues();
  g_recorder.backend_ready = false;
  g_recorder.media_present = false;
  g_recorder.free_bytes = telem::kLogBytesUnknown;
  g_recorder.init_hz = 0U;
}

bool acquireFreeBlock(int& block_index) {
  if (!g_blocks) return false;
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
  if (!g_blocks) return;
  if (block_index < 0 || block_index >= (int)kBlockCount) return;
  g_blocks[block_index].used_bytes = 0U;
  g_blocks[block_index].record_count = 0U;
  if (g_free_block_queue) {
    (void)xQueueSend(g_free_block_queue, &block_index, 0);
    noteFreeDepth(uxQueueMessagesWaiting(g_free_block_queue));
  }
}

bool queueBlockForWrite(int block_index) {
  if (!g_blocks) return false;
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
  if (!g_blocks) return false;

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
  uint32_t elapsed_ms = 0U;
  if (!writeCurrentFileBytes(block.data, block.used_bytes, elapsed_ms)) {
    portENTER_CRITICAL(&g_stats_mux);
    recordDuration(elapsed_ms, g_stats.fs_write_last_ms, g_stats.fs_write_max_ms);
    portEXIT_CRITICAL(&g_stats_mux);
    return false;
  }

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
    if (useSimpleBenchWriter()) {
      BinaryLogRecordV2 record = {};
      uint32_t q_now = 0U;
      bool have = benchRingPop(record, q_now);
      while (have) {
        {
          LockGuard lock(g_state_mutex);
          if (g_recorder.active || g_bench_write_used != 0U) {
            if (!appendBenchRecordLocked(record)) {
              g_recorder.active = false;
              g_close_pending = true;
            }
          }
        }
        have = benchRingPop(record, q_now);
      }

      portENTER_CRITICAL(&g_stats_mux);
      g_stats.queue_cur = q_now;
      if (q_now > g_stats.queue_max) g_stats.queue_max = q_now;
      portEXIT_CRITICAL(&g_stats_mux);

      const uint32_t now = millis();
      LockGuard lock(g_state_mutex);
      if (g_bench_write_used != 0U && (uint32_t)(now - g_bench_last_write_ms) >= kBenchFlushIntervalMs) {
        if (!flushBenchBufferLocked()) {
          g_recorder.active = false;
          g_close_pending = true;
        }
        g_bench_last_write_ms = now;
      }
      if (!g_recorder.active && g_close_pending && g_bench_write_used != 0U) {
        if (!flushBenchBufferLocked()) {
          g_close_pending = false;
          abandonCurrentLog();
        }
        g_bench_last_write_ms = now;
      }
    }

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

    if (g_close_pending &&
        (!g_full_block_queue || uxQueueMessagesWaiting(g_full_block_queue) == 0U) &&
        g_bench_write_used == 0U) {
      closeCurrentLog();
      g_close_pending = false;
    }
  }
}

bool preferReplayInputCoreRecords(File& file) {
  const uint32_t start_pos = (uint32_t)file.position();
  BinaryLogRecordV2 record = {};
  uint32_t state_count = 0U;
  uint32_t replay_input_count = 0U;
  for (;;) {
    const size_t got = readExact(file, (uint8_t*)&record, sizeof(record));
    if (got == 0U) break;
    if (got != sizeof(record)) break;
    if (record.magic != kLogMagic || record.version != kLogVersion || record.record_size != sizeof(record)) break;
    switch ((telem::LogRecordKind)record.record_kind) {
      case telem::LogRecordKind::State160:
        state_count++;
        break;
      case telem::LogRecordKind::ReplayInput160:
        replay_input_count++;
        break;
      default:
        break;
    }
  }
  file.seek(start_pos);
  if (replay_input_count == 0U) return false;
  if (state_count == 0U) return true;
  return replay_input_count > state_count;
}

void fillCoreComparableStateFromReplayInput(CoreComparableState& out, const telem::ReplayInputRecord160& replay) {
  memset(&out.state, 0, sizeof(out.state));
  const telem::ReplayInputPayloadV1& p = replay.payload;
  out.state.iTOW_ms = p.iTOW_ms;
  out.state.fixType = p.fixType;
  out.state.numSV = p.numSV;
  out.state.lat_1e7 = p.lat_1e7;
  out.state.lon_1e7 = p.lon_1e7;
  out.state.hMSL_mm = p.hMSL_mm;
  out.state.gSpeed_mms = p.gSpeed_mms;
  out.state.headMot_1e5deg = p.headMot_1e5deg;
  out.state.hAcc_mm = p.hAcc_mm;
  out.state.sAcc_mms = p.sAcc_mms;
  out.state.baro_temp_c = (float)p.baro_temp_milli_c * 0.001f;
  out.state.baro_press_hpa = (float)p.baro_press_milli_hpa * 0.001f;
  out.state.baro_alt_m = (float)p.baro_alt_mm * 0.001f;
  out.state.baro_vsi_mps = (float)p.baro_vsi_milli_mps * 0.001f;
  out.state.raw_present_mask = (uint16_t)p.present_mask;
}

ReadStateResult readNextCoreComparableRecord(File& file, bool prefer_replay_input,
                                             BinaryLogRecordV2& record, CoreComparableState& core,
                                             uint32_t& core_count) {
  for (;;) {
    const size_t got = readExact(file, (uint8_t*)&record, sizeof(record));
    if (got == 0U) return ReadStateResult::Eof;
    if (got != sizeof(record)) return ReadStateResult::Error;
    if (record.magic != kLogMagic || record.version != kLogVersion || record.record_size != sizeof(record)) {
      return ReadStateResult::Error;
    }

    const telem::LogRecordKind kind = (telem::LogRecordKind)record.record_kind;
    if (prefer_replay_input) {
      if (kind != telem::LogRecordKind::ReplayInput160) continue;
      telem::ReplayInputRecord160 replay = {};
      memcpy(&replay, record.payload, sizeof(replay));
      fillCoreComparableStateFromReplayInput(core, replay);
      core_count++;
      return ReadStateResult::Ok;
    }

    if (kind != telem::LogRecordKind::State160) continue;
    memcpy(&core.state, record.payload, sizeof(core.state));
    core_count++;
    return ReadStateResult::Ok;
  }
}

}  // namespace

void begin(const AppConfig& cfg, bool enabled) {
  g_cfg = cfg;
  g_stats = {};
  g_recorder = {};
  g_recorder.feature_enabled = enabled;
  g_last_seq = 0U;
  g_last_replay_input_seq = 0U;
  g_last_status_refresh_ms = 0U;
  g_last_probe_attempt_ms = 0U;
  g_last_idle_media_check_ms = 0U;

  if (!g_state_mutex) g_state_mutex = xSemaphoreCreateMutex();
  if (!g_free_block_queue) g_free_block_queue = xQueueCreate(kBlockCount, sizeof(int));
  if (!g_full_block_queue) g_full_block_queue = xQueueCreate(kBlockCount, sizeof(int));
  if (!g_blocks) {
    g_blocks = (Block*)heap_caps_malloc(sizeof(Block) * kBlockCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_blocks) {
      g_blocks = (Block*)heap_caps_malloc(sizeof(Block) * kBlockCount, MALLOC_CAP_8BIT);
    }
    if (g_blocks) {
      memset(g_blocks, 0, sizeof(Block) * kBlockCount);
    }
  }
  if (!g_bench_ring) {
    g_bench_ring = (BinaryLogRecordV2*)heap_caps_malloc(sizeof(BinaryLogRecordV2) * kBenchRingDepth,
                                                        MALLOC_CAP_8BIT);
  }
  if (!g_bench_write_buffer) {
    g_bench_write_buffer = (uint8_t*)heap_caps_malloc(kBlockBytes, MALLOC_CAP_8BIT);
  }
  resetBlockQueues();
  resetBenchRuntime();
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

void setIdleMediaChecksEnabled(bool enabled) {
  LockGuard lock(g_state_mutex);
  g_idle_media_checks_enabled = enabled;
  if (enabled) {
    g_last_idle_media_check_ms = 0U;
  }
}

void setNextSessionMetadata(const String& source_name, uint16_t replay_average_factor,
                            uint16_t applied_capture_rate_hz, uint16_t override_mask,
                            uint16_t flags) {
  LockGuard lock(g_state_mutex);
  g_next_log_metadata.valid = true;
  g_next_log_metadata.source_name = source_name;
  g_next_log_metadata.replay_average_factor = replay_average_factor ? replay_average_factor : 1U;
  g_next_log_metadata.applied_capture_rate_hz = applied_capture_rate_hz;
  g_next_log_metadata.override_mask = override_mask;
  g_next_log_metadata.flags = flags;
}

void clearNextSessionMetadata() {
  LockGuard lock(g_state_mutex);
  g_next_log_metadata = {};
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
  resetBenchRuntime();
  closeCurrentLog();
  g_recorder.session_id = session_id;
  g_recorder.bytes_written = 0U;
  g_last_seq = 0U;
  g_last_replay_input_seq = 0U;
  if (!openCurrentLog()) {
    g_next_log_metadata = {};
    refreshBackendStatus(true);
    return false;
  }
  g_recorder.active = true;
  return true;
}

void stopSession() {
  LockGuard lock(g_state_mutex);
  if (!g_recorder.active && !g_file) return;

  if (useSimpleBenchWriter()) {
    g_recorder.active = false;
    g_close_pending = true;
    if (g_writer_task) xTaskNotifyGive(g_writer_task);
    return;
  }

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
  const uint32_t now = millis();
  const bool logger_busy = g_recorder.active || g_close_pending || (bool)g_file;
  const bool sdcap_busy = false;
  if (logger_busy) {
    if (!g_recorder.backend_ready || !g_recorder.media_present) {
      abortSessionNoMedia();
    }
  } else if (sdcap_busy) {
    return;
  } else if (!sd_backend::mounted()) {
    g_recorder.backend_ready = false;
    g_recorder.media_present = false;
    g_recorder.free_bytes = telem::kLogBytesUnknown;
    g_recorder.init_hz = 0U;
  } else if (g_idle_media_checks_enabled &&
             sd_backend::mounted() &&
             (uint32_t)(now - g_last_idle_media_check_ms) >= kIdleMediaCheckMs) {
    g_last_idle_media_check_ms = now;
    if (!sd_backend::mediaPresent()) {
      g_recorder.backend_ready = false;
      g_recorder.media_present = false;
      g_recorder.free_bytes = telem::kLogBytesUnknown;
      g_recorder.init_hz = 0U;
    } else {
      g_recorder.backend_ready = true;
      g_recorder.media_present = true;
      if (g_recorder.init_hz == 0U) {
        g_recorder.init_hz = sd_backend::mountedFrequencyHz();
      }
    }
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

void enqueueReplayInput(uint32_t seq, uint32_t t_us, const telem::ReplayInputRecord160& replay) {
  (void)seq;
  (void)t_us;
  (void)replay;
}

void enqueueReplayControl(uint16_t command_id, uint32_t seq, uint32_t t_us,
                          const void* payload, uint16_t payload_len, uint32_t apply_flags) {
  (void)command_id;
  (void)seq;
  (void)t_us;
  (void)payload;
  (void)payload_len;
  (void)apply_flags;
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

struct ListedFileEntry {
  String full_name;
  String short_name;
  String extension;
  uint32_t size_bytes = 0U;
  uint32_t session_id = 0U;
  uint32_t stamp = 0U;
  bool parsed = false;
};

bool shouldIncludeListedFile(const String& short_name, bool logs_only) {
  if (short_name.length() == 0U) return false;
  if (logs_only) return short_name.endsWith(kBinaryExt);
  return short_name.endsWith(".tlog") || short_name.endsWith(".csv") || short_name.endsWith(".ndjson");
}

void sortListedFiles(ListedFileEntry* entries, size_t count, FileSortKey sort_key, FileSortDirection sort_dir) {
  if (!entries || count < 2U) return;
  for (size_t i = 0; i + 1U < count; ++i) {
    for (size_t j = i + 1U; j < count; ++j) {
      bool take_j = false;
      switch (sort_key) {
        case FileSortKey::size:
          take_j = (sort_dir == FileSortDirection::ascending) ? (entries[j].size_bytes < entries[i].size_bytes)
                                                              : (entries[j].size_bytes > entries[i].size_bytes);
          if (entries[j].size_bytes == entries[i].size_bytes) {
            take_j = entries[j].short_name < entries[i].short_name;
          }
          break;
        case FileSortKey::date:
          take_j = (sort_dir == FileSortDirection::ascending) ? (entries[j].stamp < entries[i].stamp)
                                                              : (entries[j].stamp > entries[i].stamp);
          if (entries[j].stamp == entries[i].stamp) {
            take_j = entries[j].short_name < entries[i].short_name;
          }
          break;
        case FileSortKey::name:
        default:
          take_j = (sort_dir == FileSortDirection::ascending) ? (entries[j].short_name < entries[i].short_name)
                                                              : (entries[j].short_name > entries[i].short_name);
          break;
      }
      if (take_j) {
        const ListedFileEntry tmp = entries[i];
        entries[i] = entries[j];
        entries[j] = tmp;
      }
    }
  }
}

bool collectListedFiles(ListedFileEntry*& out_entries, size_t& out_count, bool logs_only,
                        FileSortKey sort_key, FileSortDirection sort_dir) {
  out_entries = nullptr;
  out_count = 0U;

  File dir = sd_api::open(LOG_DIR);
  if (!dir || !dir.isDirectory()) return false;

  size_t count = 0U;
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String short_name = String(f.name());
      if (short_name.startsWith("/logs/")) short_name = short_name.substring(6);
      if (shouldIncludeListedFile(short_name, logs_only)) {
        count++;
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();

  if (count == 0U) return true;
  ListedFileEntry* entries = new ListedFileEntry[count];
  if (!entries) return false;

  dir = sd_api::open(LOG_DIR);
  if (!dir || !dir.isDirectory()) {
    delete[] entries;
    return false;
  }

  size_t index = 0U;
  f = dir.openNextFile();
  while (f && index < count) {
    if (!f.isDirectory()) {
      String short_name = String(f.name());
      if (short_name.startsWith("/logs/")) short_name = short_name.substring(6);
      if (shouldIncludeListedFile(short_name, logs_only)) {
        ListedFileEntry& entry = entries[index++];
        entry.short_name = short_name;
        entry.full_name = normalizeLogPath(short_name);
        entry.size_bytes = (uint32_t)f.size();
        const int dot = short_name.lastIndexOf('.');
        entry.extension = (dot >= 0) ? short_name.substring(dot + 1) : String("");
        entry.parsed = parseLogNameParts(short_name, entry.session_id, entry.stamp);
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();

  out_entries = entries;
  out_count = index;
  sortListedFiles(out_entries, out_count, sort_key, sort_dir);
  return true;
}

String filesJson(FileSortKey sort_key, FileSortDirection sort_dir) {
  if (!sd_backend::mounted()) return "[]";
  ListedFileEntry* entries = nullptr;
  size_t count = 0U;
  if (!collectListedFiles(entries, count, false, sort_key, sort_dir)) return "[]";

  String out = "[";
  for (size_t i = 0; i < count; ++i) {
    if (i != 0U) out += ',';
    out += "{\"name\":\"";
    out += entries[i].short_name;
    out += "\",\"size\":";
    out += String(entries[i].size_bytes);
    out += ",\"session\":";
    out += String(entries[i].session_id);
    out += ",\"date\":";
    out += String(entries[i].stamp);
    out += ",\"ext\":\"";
    out += entries[i].extension;
    out += "\"}";
  }
  out += "]";
  delete[] entries;
  return out;
}

bool listFiles(telem::LogFileInfoV1* out_files,
               uint16_t max_files,
               uint16_t offset,
               uint16_t& total_files,
               uint16_t& returned_files,
               FileSortKey sort_key,
               FileSortDirection sort_dir) {
  total_files = 0U;
  returned_files = 0U;

  LockGuard lock(g_state_mutex);
  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present || !sd_backend::mounted()) {
    return false;
  }

  ListedFileEntry* entries = nullptr;
  size_t count = 0U;
  if (!collectListedFiles(entries, count, true, sort_key, sort_dir)) return false;

  total_files = (count > 0xFFFFU) ? 0xFFFFU : (uint16_t)count;
  for (size_t i = offset; i < count && returned_files < max_files; ++i) {
    telem::LogFileInfoV1& entry = out_files[returned_files];
    memset(&entry, 0, sizeof(entry));
    entry.size_bytes = entries[i].size_bytes;
    strncpy(entry.name, entries[i].short_name.c_str(), sizeof(entry.name) - 1U);
    returned_files++;
  }
  delete[] entries;
  return true;
}

bool latestLogNameLocked(String& out_name) {
  out_name = "";
  if (g_last_closed_name.isEmpty()) {
    SessionMetaRecord meta = {};
    if (loadSessionMeta(meta) && meta.last_closed_name[0] != '\0') {
      g_last_closed_name = normalizeLogPath(String(meta.last_closed_name));
      g_last_closed_session_id = meta.last_closed_session_id;
    }
  }
  if (!g_last_closed_name.isEmpty()) {
    out_name = g_last_closed_name;
    return true;
  }
  if (!sd_backend::mounted()) return false;
  File dir = sd_api::open(LOG_DIR);
  if (!dir || !dir.isDirectory()) return false;

  String best_name;
  uint32_t best_session = 0U;
  uint32_t best_stamp = 0U;
  bool have_best = false;
  String candidate_name;
  while (nextLogName(dir, candidate_name)) {
    uint32_t candidate_session = 0U;
    uint32_t candidate_stamp = 0U;
    const bool parsed = parseLogNameParts(candidate_name, candidate_session, candidate_stamp);
    if (!have_best) {
      best_name = candidate_name;
      best_session = candidate_session;
      best_stamp = candidate_stamp;
      have_best = true;
      continue;
    }
    if (parsed) {
      if (candidate_session > best_session ||
          (candidate_session == best_session && candidate_stamp > best_stamp)) {
        best_name = candidate_name;
        best_session = candidate_session;
        best_stamp = candidate_stamp;
      }
    } else if (best_name.isEmpty() || candidate_name > best_name) {
      best_name = candidate_name;
    }
  }
  dir.close();
  out_name = best_name;
  return !out_name.isEmpty();
}

bool largestLogNameLocked(String& out_name) {
  out_name = "";
  if (!sd_backend::mounted()) return false;
  File dir = sd_api::open(LOG_DIR);
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
  if (g_last_closed_session_id == 0U) {
    SessionMetaRecord meta = {};
    if (loadSessionMeta(meta) && meta.last_closed_session_id == session_id && meta.last_closed_name[0] != '\0') {
      g_last_closed_session_id = meta.last_closed_session_id;
      g_last_closed_name = normalizeLogPath(String(meta.last_closed_name));
    }
  }
  if (g_last_closed_session_id == session_id && !g_last_closed_name.isEmpty()) {
    out_name = g_last_closed_name;
    return true;
  }
  if (!sd_backend::mounted()) return false;
  File dir = sd_api::open(LOG_DIR);
  if (!dir || !dir.isDirectory()) return false;

  String best_name;
  uint32_t best_stamp = 0U;
  bool have_best = false;
  String candidate_name;
  while (nextLogName(dir, candidate_name)) {
    uint32_t candidate_session = 0U;
    uint32_t candidate_stamp = 0U;
    if (parseLogNameParts(candidate_name, candidate_session, candidate_stamp) &&
        candidate_session == session_id) {
      if (!have_best || candidate_stamp > best_stamp) {
        best_name = candidate_name;
        best_stamp = candidate_stamp;
        have_best = true;
      }
    } else if (!have_best && (best_name.isEmpty() || candidate_name > best_name)) {
      best_name = candidate_name;
    }
  }
  dir.close();
  out_name = best_name;
  return !out_name.isEmpty();
}

uint32_t highestLogSessionIdLocked() {
  SessionMetaRecord meta = {};
  if (loadSessionMeta(meta) && meta.next_session_id > 0U) {
    return meta.next_session_id - 1U;
  }
  if (!sd_backend::mounted()) return 0U;
  File dir = sd_api::open(LOG_DIR);
  if (!dir || !dir.isDirectory()) return 0U;

  uint32_t best_session = 0U;
  String candidate_name;
  while (nextLogName(dir, candidate_name)) {
    uint32_t candidate_session = 0U;
    uint32_t candidate_stamp = 0U;
    if (parseLogNameParts(candidate_name, candidate_session, candidate_stamp) &&
        candidate_session > best_session) {
      best_session = candidate_session;
    }
  }
  dir.close();
  return best_session;
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
  if (!sd_api::exists(full)) return false;
  if (g_current_name == full) {
    closeCurrentLog();
  }
  const uint32_t t0 = millis();
  const bool ok = sd_api::remove(full);
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
  if (!sd_api::exists(src_full) || sd_api::exists(dst_full)) return false;

  if (g_current_name == src_full) {
    closeCurrentLog();
  }
  bool ok = sd_api::rename(src_full, dst_full);
  if (!ok) {
    File src = sd_api::open(src_full);
    if (src) {
      if (sd_api::exists(dst_full)) (void)sd_api::remove(dst_full);
      File dst = sd_api::open(dst_full, sd_api::OpenMode::write);
      if (dst) {
        ok = copyFileRaw(src, dst);
        dst.flush();
        dst.close();
        if (ok) {
          ok = sd_api::remove(src_full);
        } else if (sd_api::exists(dst_full)) {
          (void)sd_api::remove(dst_full);
        }
      }
      src.close();
    }
  }
  refreshBackendStatus(true);
  return ok;
}

bool exportLogToCsvByName(const String& name, Stream* out) {
  LockGuard lock(g_state_mutex);
  if (g_recorder.active || g_close_pending) {
    if (out) out->println("AIRCSV export_ok=0 reason=logger_busy");
    return false;
  }
  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present || !sd_backend::mounted()) {
    if (out) out->println("AIRCSV export_ok=0 reason=backend_not_ready");
    return false;
  }
  if (!isSafeName(name) || !name.endsWith(kBinaryExt)) {
    if (out) out->printf("AIRCSV file=%s export_ok=0 reason=invalid_name\r\n", name.c_str());
    return false;
  }

  const String src_path = normalizeLogPath(name);
  File src = sd_api::open(src_path);
  if (!src) {
    if (out) out->printf("AIRCSV file=%s export_ok=0 reason=open_src_failed\r\n", src_path.c_str());
    return false;
  }

  const String csv_path = makeCsvName(src_path);
  if (sd_api::exists(csv_path)) (void)sd_api::remove(csv_path);
  File csv = sd_api::open(csv_path, sd_api::OpenMode::write);
  if (!csv) {
    src.close();
    if (out) out->printf("AIRCSV file=%s export_ok=0 reason=open_csv_failed\r\n", src_path.c_str());
    return false;
  }

  uint32_t rows = 0U;
  uint32_t unsupported_rows = 0U;
  const bool ok = exportSingleLogToCsv(src, csv, rows, unsupported_rows);
  csv.flush();
  csv.close();
  src.close();

  if (!ok) {
    if (sd_api::exists(csv_path)) (void)sd_api::remove(csv_path);
    if (out) out->printf("AIRCSV file=%s export_ok=0 reason=convert_failed\r\n", src_path.c_str());
    return false;
  }

  if (out) {
    const String csv_name = csv_path.startsWith("/logs/") ? csv_path.substring(6) : csv_path;
    out->printf("AIRCSV file=%s csv=%s export_ok=1 rows=%lu unsupported=%lu\r\n",
                src_path.c_str(),
                csv_name.c_str(),
                (unsigned long)rows,
                (unsigned long)unsupported_rows);
  }
  return true;
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

  File dir = sd_api::open(LOG_DIR);
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
    if (sd_api::exists(csv_path)) (void)sd_api::remove(csv_path);
    File csv = sd_api::open(csv_path, sd_api::OpenMode::write);
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
      if (sd_api::exists(csv_path)) (void)sd_api::remove(csv_path);
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

String previewLogName(uint32_t session_id) {
  LockGuard lock(g_state_mutex);
  const uint32_t resolved_session =
      session_id ? session_id : ((g_last_closed_session_id != 0U) ? (g_last_closed_session_id + 1U) : 1U);
  return makeLogName(resolved_session);
}

String recordPrefix() {
  LockGuard lock(g_state_mutex);
  return activeRecordPrefix();
}

bool latestLogName(String& out_name) {
  LockGuard lock(g_state_mutex);
  if (!sd_backend::mounted()) refreshBackendStatus(true);
  if (!sd_backend::mounted()) {
    out_name = "";
    return false;
  }
  return latestLogNameLocked(out_name);
}

bool largestLogName(String& out_name) {
  LockGuard lock(g_state_mutex);
  if (!sd_backend::mounted()) refreshBackendStatus(true);
  if (!sd_backend::mounted()) {
    out_name = "";
    return false;
  }
  return largestLogNameLocked(out_name);
}

bool latestLogNameForSession(uint32_t session_id, String& out_name) {
  LockGuard lock(g_state_mutex);
  if (!sd_backend::mounted()) refreshBackendStatus(true);
  if (!sd_backend::mounted()) {
    out_name = "";
    return false;
  }
  return latestLogNameForSessionLocked(session_id, out_name);
}

uint32_t highestLogSessionId() {
  LockGuard lock(g_state_mutex);
  if (!sd_backend::mounted()) refreshBackendStatus(true);
  if (!sd_backend::mounted()) {
    return 0U;
  }
  return highestLogSessionIdLocked();
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
  if (sd_api::exists(dst_path)) (void)sd_api::remove(dst_path);

  File src = sd_api::open(src_path);
  if (!src) {
    out.printf("AIRVERIFY ok=0 file=%s reason=open_src_failed\r\n", src_path.c_str());
    return false;
  }

  File dst = sd_api::open(dst_path, sd_api::OpenMode::write);
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
    if (sd_api::exists(dst_path)) (void)sd_api::remove(dst_path);
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
  File src = sd_api::open(src_path);
  File dst = sd_api::open(dst_path);
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

bool compareLogsTimed(Stream& out, const String& src_name, const String& dst_name, uint16_t warmup_skip) {
  LockGuard lock(g_state_mutex);
  if (g_recorder.active || g_close_pending || g_file) {
    out.println("AIRCOMPARET ok=0 reason=logger_busy");
    return false;
  }

  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present || !sd_backend::mounted()) {
    out.println("AIRCOMPARET ok=0 reason=backend_not_ready");
    return false;
  }

  const String src_path = normalizeLogPath(src_name);
  const String dst_path = normalizeLogPath(dst_name);
  File src = sd_api::open(src_path);
  File src_core = sd_api::open(src_path);
  File dst = sd_api::open(dst_path);
  if (!src || !src_core || !dst) {
    if (src) src.close();
    if (src_core) src_core.close();
    if (dst) dst.close();
    out.printf("AIRCOMPARET ok=0 reason=open_failed src=%s dst=%s\r\n", src_path.c_str(), dst_path.c_str());
    return false;
  }

  const bool prefer_replay_input_core = preferReplayInputCoreRecords(src_core);
  src_core.seek(0);

  BinaryLogRecordV2 src_prev_record = {};
  BinaryLogRecordV2 src_curr_record = {};
  BinaryLogRecordV2 src_core_prev_record = {};
  BinaryLogRecordV2 src_core_curr_record = {};
  BinaryLogRecordV2 dst_record = {};
  telem::TelemetryFullStateV1 src_prev_state = {};
  telem::TelemetryFullStateV1 src_curr_state = {};
  telem::TelemetryFullStateV1 dst_state = {};
  CoreComparableState src_core_prev = {};
  CoreComparableState src_core_curr = {};
  uint32_t src_states = 0U;
  uint32_t src_core_records = 0U;
  uint32_t dst_states = 0U;

  if (readNextStateRecord(src, src_prev_record, src_prev_state, src_states) != ReadStateResult::Ok) {
    src.close();
    src_core.close();
    dst.close();
    out.printf("AIRCOMPARET ok=0 reason=no_source_state src=%s\r\n", src_path.c_str());
    return false;
  }

  bool have_src_curr = (readNextStateRecord(src, src_curr_record, src_curr_state, src_states) == ReadStateResult::Ok);
  if (readNextCoreComparableRecord(src_core, prefer_replay_input_core, src_core_prev_record, src_core_prev,
                                   src_core_records) != ReadStateResult::Ok) {
    src.close();
    src_core.close();
    dst.close();
    out.printf("AIRCOMPARET ok=0 reason=no_source_core src=%s\r\n", src_path.c_str());
    return false;
  }
  bool have_src_core_curr =
      (readNextCoreComparableRecord(src_core, prefer_replay_input_core, src_core_curr_record, src_core_curr,
                                    src_core_records) == ReadStateResult::Ok);

  for (uint16_t skipped = 0U; skipped < warmup_skip; ++skipped) {
    if (readNextStateRecord(dst, dst_record, dst_state, dst_states) != ReadStateResult::Ok) {
      src.close();
      src_core.close();
      dst.close();
      out.printf("AIRCOMPARET ok=0 reason=no_dest_state_after_skip src=%s dst=%s warmup_skip=%u\r\n",
                 src_path.c_str(), dst_path.c_str(), (unsigned)warmup_skip);
      return false;
    }
  }

  uint32_t compared = 0U;
  uint32_t gps_mismatch = 0U;
  uint32_t baro_mismatch = 0U;
  uint32_t mask_mismatch = 0U;
  bool have_first_gps_mismatch = false;
  bool have_first_baro_mismatch = false;
  BinaryLogRecordV2 first_gps_src_record = {};
  BinaryLogRecordV2 first_gps_dst_record = {};
  BinaryLogRecordV2 first_baro_src_record = {};
  BinaryLogRecordV2 first_baro_dst_record = {};
  telem::TelemetryFullStateV1 first_gps_src_state = {};
  telem::TelemetryFullStateV1 first_gps_dst_state = {};
  telem::TelemetryFullStateV1 first_baro_src_state = {};
  telem::TelemetryFullStateV1 first_baro_dst_state = {};
  uint32_t max_abs_delta_us = 0U;
  double roll_abs_sum = 0.0;
  double pitch_abs_sum = 0.0;
  double yaw_abs_sum = 0.0;
  double mag_heading_abs_sum = 0.0;
  double accel_abs_sum[3] = {};
  double gyro_abs_sum[3] = {};
  float roll_abs_max = 0.0f;
  float pitch_abs_max = 0.0f;
  float yaw_abs_max = 0.0f;
  float mag_heading_abs_max = 0.0f;
  float accel_abs_max[3] = {};
  float gyro_abs_max[3] = {};

  while (readNextStateRecord(dst, dst_record, dst_state, dst_states) == ReadStateResult::Ok) {
    while (have_src_curr && src_curr_record.t_us < dst_record.t_us) {
      src_prev_record = src_curr_record;
      src_prev_state = src_curr_state;
      have_src_curr = (readNextStateRecord(src, src_curr_record, src_curr_state, src_states) == ReadStateResult::Ok);
    }
    while (have_src_core_curr && src_core_curr_record.t_us < dst_record.t_us) {
      src_core_prev_record = src_core_curr_record;
      src_core_prev = src_core_curr;
      have_src_core_curr =
          (readNextCoreComparableRecord(src_core, prefer_replay_input_core, src_core_curr_record, src_core_curr,
                                        src_core_records) == ReadStateResult::Ok);
    }

    const telem::TelemetryFullStateV1* match_state = &src_prev_state;
    const BinaryLogRecordV2* match_record = &src_prev_record;
    if (have_src_curr) {
      const uint32_t prev_delta = (src_prev_record.t_us > dst_record.t_us) ? (src_prev_record.t_us - dst_record.t_us)
                                                                           : (dst_record.t_us - src_prev_record.t_us);
      const uint32_t curr_delta = (src_curr_record.t_us > dst_record.t_us) ? (src_curr_record.t_us - dst_record.t_us)
                                                                           : (dst_record.t_us - src_curr_record.t_us);
      if (curr_delta < prev_delta) {
        match_state = &src_curr_state;
        match_record = &src_curr_record;
      }
    }
    const telem::TelemetryFullStateV1* match_core_state = &src_core_prev.state;
    const BinaryLogRecordV2* match_core_record = &src_core_prev_record;
    if (have_src_core_curr) {
      const uint32_t prev_delta =
          (src_core_prev_record.t_us > dst_record.t_us) ? (src_core_prev_record.t_us - dst_record.t_us)
                                                        : (dst_record.t_us - src_core_prev_record.t_us);
      const uint32_t curr_delta =
          (src_core_curr_record.t_us > dst_record.t_us) ? (src_core_curr_record.t_us - dst_record.t_us)
                                                        : (dst_record.t_us - src_core_curr_record.t_us);
      if (curr_delta < prev_delta) {
        match_core_state = &src_core_curr.state;
        match_core_record = &src_core_curr_record;
      }
    }

    compared++;
    const uint32_t delta_us = (match_core_record->t_us > dst_record.t_us) ? (match_core_record->t_us - dst_record.t_us)
                                                                           : (dst_record.t_us - match_core_record->t_us);
    if (delta_us > max_abs_delta_us) max_abs_delta_us = delta_us;

    const float roll_diff = fabsf(match_state->roll_deg - dst_state.roll_deg);
    const float pitch_diff = fabsf(match_state->pitch_deg - dst_state.pitch_deg);
    const float yaw_diff = fabsf(wrappedAngleDiffDeg(match_state->yaw_deg, dst_state.yaw_deg));
    const float mag_heading_diff = fabsf(wrappedAngleDiffDeg(match_state->mag_heading_deg, dst_state.mag_heading_deg));
    const float accel_diff[3] = {
        fabsf(match_state->accel_x_mps2 - dst_state.accel_x_mps2),
        fabsf(match_state->accel_y_mps2 - dst_state.accel_y_mps2),
        fabsf(match_state->accel_z_mps2 - dst_state.accel_z_mps2),
    };
    const float gyro_diff[3] = {
        fabsf(match_state->gyro_x_dps - dst_state.gyro_x_dps),
        fabsf(match_state->gyro_y_dps - dst_state.gyro_y_dps),
        fabsf(match_state->gyro_z_dps - dst_state.gyro_z_dps),
    };

    roll_abs_sum += roll_diff;
    pitch_abs_sum += pitch_diff;
    yaw_abs_sum += yaw_diff;
    mag_heading_abs_sum += mag_heading_diff;
    if (roll_diff > roll_abs_max) roll_abs_max = roll_diff;
    if (pitch_diff > pitch_abs_max) pitch_abs_max = pitch_diff;
    if (yaw_diff > yaw_abs_max) yaw_abs_max = yaw_diff;
    if (mag_heading_diff > mag_heading_abs_max) mag_heading_abs_max = mag_heading_diff;

    for (uint8_t i = 0U; i < 3U; ++i) {
      accel_abs_sum[i] += accel_diff[i];
      gyro_abs_sum[i] += gyro_diff[i];
      if (accel_diff[i] > accel_abs_max[i]) accel_abs_max[i] = accel_diff[i];
      if (gyro_diff[i] > gyro_abs_max[i]) gyro_abs_max[i] = gyro_diff[i];
    }

    if (!gpsMatches(*match_core_state, dst_state)) {
      gps_mismatch++;
      if (!have_first_gps_mismatch) {
        have_first_gps_mismatch = true;
        first_gps_src_record = *match_core_record;
        first_gps_dst_record = dst_record;
        first_gps_src_state = *match_core_state;
        first_gps_dst_state = dst_state;
      }
    }
    if (!baroMatches(*match_core_state, dst_state)) {
      baro_mismatch++;
      if (!have_first_baro_mismatch) {
        have_first_baro_mismatch = true;
        first_baro_src_record = *match_core_record;
        first_baro_dst_record = dst_record;
        first_baro_src_state = *match_core_state;
        first_baro_dst_state = dst_state;
      }
    }
    if (match_core_state->raw_present_mask != dst_state.raw_present_mask) mask_mismatch++;
  }

  while (readNextStateRecord(src, src_prev_record, src_prev_state, src_states) == ReadStateResult::Ok) {}

  src.close();
  src_core.close();
  dst.close();

  if (compared == 0U) {
    out.printf("AIRCOMPARET ok=0 reason=no_compared_states src=%s dst=%s\r\n", src_path.c_str(), dst_path.c_str());
    return false;
  }

  const String src_short = src_path.startsWith("/logs/") ? src_path.substring(6) : src_path;
  const String dst_short = dst_path.startsWith("/logs/") ? dst_path.substring(6) : dst_path;
  out.printf(
      "AIRCOMPARET src=%s dst=%s compared=%lu warmup_skip=%u src_states=%lu src_core=%lu src_core_kind=%s dst_states=%lu ts_max_delta_us=%lu gps_mismatch=%lu baro_mismatch=%lu mask_mismatch=%lu\r\n",
      src_short.c_str(),
      dst_short.c_str(),
      (unsigned long)compared,
      (unsigned)warmup_skip,
      (unsigned long)src_states,
      (unsigned long)src_core_records,
      prefer_replay_input_core ? "replay_input160" : "state160",
      (unsigned long)dst_states,
      (unsigned long)max_abs_delta_us,
      (unsigned long)gps_mismatch,
      (unsigned long)baro_mismatch,
      (unsigned long)mask_mismatch);
  out.printf(
      "AIRCOMPARET fusion roll_mean_abs=%.6f roll_max_abs=%.6f pitch_mean_abs=%.6f pitch_max_abs=%.6f yaw_mean_abs=%.6f yaw_max_abs=%.6f maghdg_mean_abs=%.6f maghdg_max_abs=%.6f\r\n",
      roll_abs_sum / (double)compared,
      (double)roll_abs_max,
      pitch_abs_sum / (double)compared,
      (double)pitch_abs_max,
      yaw_abs_sum / (double)compared,
      (double)yaw_abs_max,
      mag_heading_abs_sum / (double)compared,
      (double)mag_heading_abs_max);
  out.printf(
      "AIRCOMPARET imu accel_mean_abs=(%.6f,%.6f,%.6f) accel_max_abs=(%.6f,%.6f,%.6f) gyro_mean_abs=(%.6f,%.6f,%.6f) gyro_max_abs=(%.6f,%.6f,%.6f)\r\n",
      accel_abs_sum[0] / (double)compared,
      accel_abs_sum[1] / (double)compared,
      accel_abs_sum[2] / (double)compared,
      (double)accel_abs_max[0],
      (double)accel_abs_max[1],
      (double)accel_abs_max[2],
      gyro_abs_sum[0] / (double)compared,
      gyro_abs_sum[1] / (double)compared,
      gyro_abs_sum[2] / (double)compared,
      (double)gyro_abs_max[0],
      (double)gyro_abs_max[1],
      (double)gyro_abs_max[2]);
  if (have_first_gps_mismatch) {
    out.printf(
        "AIRCOMPARET first_gps_mismatch src_t_us=%lu dst_t_us=%lu src_iTOW=%lu dst_iTOW=%lu src_fix=%u dst_fix=%u src_numSV=%u dst_numSV=%u src_lat=%ld dst_lat=%ld src_lon=%ld dst_lon=%ld src_hMSL=%ld dst_hMSL=%ld src_gSpeed=%ld dst_gSpeed=%ld src_headMot=%ld dst_headMot=%ld src_hAcc=%lu dst_hAcc=%lu src_sAcc=%lu dst_sAcc=%lu\r\n",
        (unsigned long)first_gps_src_record.t_us,
        (unsigned long)first_gps_dst_record.t_us,
        (unsigned long)first_gps_src_state.iTOW_ms,
        (unsigned long)first_gps_dst_state.iTOW_ms,
        (unsigned)first_gps_src_state.fixType,
        (unsigned)first_gps_dst_state.fixType,
        (unsigned)first_gps_src_state.numSV,
        (unsigned)first_gps_dst_state.numSV,
        (long)first_gps_src_state.lat_1e7,
        (long)first_gps_dst_state.lat_1e7,
        (long)first_gps_src_state.lon_1e7,
        (long)first_gps_dst_state.lon_1e7,
        (long)first_gps_src_state.hMSL_mm,
        (long)first_gps_dst_state.hMSL_mm,
        (long)first_gps_src_state.gSpeed_mms,
        (long)first_gps_dst_state.gSpeed_mms,
        (long)first_gps_src_state.headMot_1e5deg,
        (long)first_gps_dst_state.headMot_1e5deg,
        (unsigned long)first_gps_src_state.hAcc_mm,
        (unsigned long)first_gps_dst_state.hAcc_mm,
        (unsigned long)first_gps_src_state.sAcc_mms,
        (unsigned long)first_gps_dst_state.sAcc_mms);
  }
  if (have_first_baro_mismatch) {
    out.printf(
        "AIRCOMPARET first_baro_mismatch src_t_us=%lu dst_t_us=%lu src_temp=%.6f dst_temp=%.6f src_press=%.6f dst_press=%.6f src_alt=%.6f dst_alt=%.6f src_vsi=%.6f dst_vsi=%.6f src_mask=%u dst_mask=%u\r\n",
        (unsigned long)first_baro_src_record.t_us,
        (unsigned long)first_baro_dst_record.t_us,
        (double)first_baro_src_state.baro_temp_c,
        (double)first_baro_dst_state.baro_temp_c,
        (double)first_baro_src_state.baro_press_hpa,
        (double)first_baro_dst_state.baro_press_hpa,
        (double)first_baro_src_state.baro_alt_m,
        (double)first_baro_dst_state.baro_alt_m,
        (double)first_baro_src_state.baro_vsi_mps,
        (double)first_baro_dst_state.baro_vsi_mps,
        (unsigned)first_baro_src_state.raw_present_mask,
        (unsigned)first_baro_dst_state.raw_present_mask);
  }
  return true;
}

bool printRecordKindSummary(Stream& out, const String& log_name) {
  LockGuard lock(g_state_mutex);
  if (g_recorder.active || g_close_pending || g_file) {
    out.println("AIRLOGKINDS ok=0 reason=logger_busy");
    return false;
  }

  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present || !sd_backend::mounted()) {
    out.println("AIRLOGKINDS ok=0 reason=backend_not_ready");
    return false;
  }

  const String path = normalizeLogPath(log_name);
  File file = sd_api::open(path);
  if (!file) {
    out.printf("AIRLOGKINDS ok=0 reason=open_failed file=%s\r\n", path.c_str());
    return false;
  }

  BinaryLogRecordV2 record = {};
  uint32_t total = 0U;
  uint32_t state_count = 0U;
  uint32_t metadata_count = 0U;
  uint32_t replay_control_count = 0U;
  uint32_t replay_input_count = 0U;
  uint32_t unknown_count = 0U;

  for (;;) {
    const size_t got = readExact(file, (uint8_t*)&record, sizeof(record));
    if (got == 0U) break;
    if (got != sizeof(record) ||
        record.magic != kLogMagic ||
        record.version != kLogVersion ||
        record.record_size != sizeof(record)) {
      file.close();
      out.printf("AIRLOGKINDS ok=0 reason=bad_record file=%s total=%lu\r\n",
                 path.c_str(),
                 (unsigned long)total);
      return false;
    }

    total++;
    switch ((telem::LogRecordKind)record.record_kind) {
      case telem::LogRecordKind::State160:
        state_count++;
        break;
      case telem::LogRecordKind::Metadata160:
        metadata_count++;
        break;
      case telem::LogRecordKind::ReplayControl160:
        replay_control_count++;
        break;
      case telem::LogRecordKind::ReplayInput160:
        replay_input_count++;
        break;
      default:
        unknown_count++;
        break;
    }
  }

  file.close();
  out.printf(
      "AIRLOGKINDS ok=1 file=%s total=%lu state160=%lu metadata160=%lu replay_control160=%lu replay_input160=%lu unknown=%lu\r\n",
      path.c_str(),
      (unsigned long)total,
      (unsigned long)state_count,
      (unsigned long)metadata_count,
      (unsigned long)replay_control_count,
      (unsigned long)replay_input_count,
      (unsigned long)unknown_count);
  return true;
}

bool printFusionSettingsSummary(Stream& out, const String& log_name) {
  LockGuard lock(g_state_mutex);
  if (g_recorder.active || g_close_pending || g_file) {
    out.println("AIRLOGFUSION ok=0 reason=logger_busy");
    return false;
  }

  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present || !sd_backend::mounted()) {
    out.println("AIRLOGFUSION ok=0 reason=backend_not_ready");
    return false;
  }

  const String path = normalizeLogPath(log_name);
  File file = sd_api::open(path);
  if (!file) {
    out.printf("AIRLOGFUSION ok=0 reason=open_failed file=%s\r\n", path.c_str());
    return false;
  }

  BinaryLogRecordV2 record = {};
  telem::TelemetryFullStateV1 state = {};
  uint32_t state_count = 0U;
  if (readNextStateRecord(file, record, state, state_count) != ReadStateResult::Ok) {
    file.close();
    out.printf("AIRLOGFUSION ok=0 reason=no_state file=%s\r\n", path.c_str());
    return false;
  }

  const float first_gain = state.fusion_gain;
  const float first_acc = state.fusion_accel_rej;
  const float first_mag = state.fusion_mag_rej;
  const uint16_t first_rec = state.fusion_recovery_period;
  float last_gain = first_gain;
  float last_acc = first_acc;
  float last_mag = first_mag;
  uint16_t last_rec = first_rec;
  uint32_t changed = 0U;
  uint32_t match = 1U;

  while (readNextStateRecord(file, record, state, state_count) == ReadStateResult::Ok) {
    last_gain = state.fusion_gain;
    last_acc = state.fusion_accel_rej;
    last_mag = state.fusion_mag_rej;
    last_rec = state.fusion_recovery_period;
    const bool same =
        floatNear(state.fusion_gain, first_gain, 0.0005f) &&
        floatNear(state.fusion_accel_rej, first_acc, 0.005f) &&
        floatNear(state.fusion_mag_rej, first_mag, 0.005f) &&
        state.fusion_recovery_period == first_rec;
    if (same) {
      match++;
    } else {
      changed++;
    }
  }

  file.close();
  const String short_name = path.startsWith("/logs/") ? path.substring(6) : path;
  out.printf(
      "AIRLOGFUSION file=%s states=%lu first=(%.3f,%.2f,%.2f,%u) last=(%.3f,%.2f,%.2f,%u) match_first=%lu changed=%lu ok=1\r\n",
      short_name.c_str(),
      (unsigned long)state_count,
      (double)first_gain,
      (double)first_acc,
      (double)first_mag,
      (unsigned)first_rec,
      (double)last_gain,
      (double)last_acc,
      (double)last_mag,
      (unsigned)last_rec,
      (unsigned long)match,
      (unsigned long)changed);
  return true;
}

bool printFusionFlagSummary(Stream& out, const String& log_name) {
  LockGuard lock(g_state_mutex);
  if (g_recorder.active || g_close_pending || g_file) {
    out.println("AIRLOGFLAGS ok=0 reason=logger_busy");
    return false;
  }

  refreshBackendStatus(true);
  if (!g_recorder.backend_ready || !g_recorder.media_present || !sd_backend::mounted()) {
    out.println("AIRLOGFLAGS ok=0 reason=backend_not_ready");
    return false;
  }

  const String path = normalizeLogPath(log_name);
  File file = sd_api::open(path);
  if (!file) {
    out.printf("AIRLOGFLAGS ok=0 reason=open_failed file=%s\r\n", path.c_str());
    return false;
  }

  BinaryLogRecordV2 record = {};
  telem::TelemetryFullStateV1 state = {};
  uint32_t state_count = 0U;
  uint32_t accel_ignored = 0U;
  uint32_t mag_ignored = 0U;
  uint32_t accel_error_flag = 0U;
  uint32_t mag_error_flag = 0U;
  uint32_t mag_recovery = 0U;
  uint32_t accel_recovery = 0U;
  double accel_error_sum = 0.0;
  double mag_error_sum = 0.0;
  float accel_error_max = 0.0f;
  float mag_error_max = 0.0f;

  while (readNextStateRecord(file, record, state, state_count) == ReadStateResult::Ok) {
    const FusionReplayDiagDecoded diag = decodeFusionReplayDiag(state);
    accel_error_sum += diag.accel_error_deg;
    mag_error_sum += diag.mag_error_deg;
    if (diag.accel_error_deg > accel_error_max) accel_error_max = diag.accel_error_deg;
    if (diag.mag_error_deg > mag_error_max) mag_error_max = diag.mag_error_deg;
    if ((state.flags & telem::kStateFlagFusionAccelerometerIgnored) != 0U) accel_ignored++;
    if ((state.flags & telem::kStateFlagFusionMagnetometerIgnored) != 0U) mag_ignored++;
    if ((state.flags & telem::kStateFlagFusionAccelerationError) != 0U) accel_error_flag++;
    if ((state.flags & telem::kStateFlagFusionMagneticError) != 0U) mag_error_flag++;
    if ((state.flags & telem::kStateFlagFusionAccelerationRecovery) != 0U) accel_recovery++;
    if ((state.flags & telem::kStateFlagFusionMagneticRecovery) != 0U) mag_recovery++;
  }

  file.close();
  if (state_count == 0U) {
    out.printf("AIRLOGFLAGS ok=0 reason=no_state file=%s\r\n", path.c_str());
    return false;
  }

  const String short_name = path.startsWith("/logs/") ? path.substring(6) : path;
  out.printf(
      "AIRLOGFLAGS file=%s states=%lu accel_ignored=%lu mag_ignored=%lu accel_error_flag=%lu mag_error_flag=%lu accel_recovery=%lu mag_recovery=%lu accel_error_mean=%.6f accel_error_max=%.6f mag_error_mean=%.6f mag_error_max=%.6f ok=1\r\n",
      short_name.c_str(),
      (unsigned long)state_count,
      (unsigned long)accel_ignored,
      (unsigned long)mag_ignored,
      (unsigned long)accel_error_flag,
      (unsigned long)mag_error_flag,
      (unsigned long)accel_recovery,
      (unsigned long)mag_recovery,
      accel_error_sum / (double)state_count,
      (double)accel_error_max,
      mag_error_sum / (double)state_count,
      (double)mag_error_max);
  return true;
}

}  // namespace log_store
