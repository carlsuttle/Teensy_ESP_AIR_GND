#include "log_store.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

namespace log_store {
namespace {

static constexpr char LOG_DIR[] = "/logs";
static constexpr char kBinaryExt[] = ".tlog";
static constexpr char kCsvHeader[] =
    "seq,t_us,roll_deg,pitch_deg,yaw_deg,"
    "iTOW_ms,fix,sats,lat_deg,lon_deg,hMSL_m,gSpeed_mps,headMot_deg,hAcc_m,sAcc_mps,"
    "gps_parse_errors,mirror_tx_ok,mirror_drop_count,last_gps_ms,last_imu_ms,last_baro_ms,"
    "baro_temp_c,baro_press_hpa,baro_alt_m,baro_vsi_mps,"
    "fusion_gain,fusion_accel_rej_deg,fusion_mag_rej_deg,fusion_recovery_period,flags\n";
static constexpr size_t kCsvLineMaxBytes = 512;
static constexpr size_t kQueueDepth = 256;
static constexpr size_t kWriteBufferBytes = 8192;
static constexpr uint32_t kBatchWriteMs = 500U;
static constexpr uint32_t kLogMagic = 0x4C4F4731UL;  // "LOG1"
static constexpr uint16_t kLogVersion = 1U;

#pragma pack(push, 1)
struct BinaryLogRecordV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t record_size;
  uint32_t seq;
  uint32_t t_us;
  telem::TelemetryFullStateV1 state;
};
#pragma pack(pop)

AppConfig g_cfg = {};
File g_file;
String g_current_name;
TaskHandle_t g_writer_task = nullptr;
SemaphoreHandle_t g_fs_mutex = nullptr;
Stats g_stats = {};
bool g_fs_ready = false;
uint32_t g_last_log_ms = 0;
uint32_t g_last_log_t_us = 0;
uint32_t g_last_seq = 0;
uint32_t g_last_write_ms = 0;
uint8_t g_write_buffer[kWriteBufferBytes];
size_t g_write_used = 0;
BinaryLogRecordV1 g_record_ring[kQueueDepth] = {};
volatile uint16_t g_ring_head = 0;
volatile uint16_t g_ring_tail = 0;
portMUX_TYPE g_log_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t ringCountUnsafe() {
  return (uint16_t)(g_ring_head - g_ring_tail);
}

bool ringPush(const BinaryLogRecordV1& rec, uint32_t& q_now) {
  bool ok = false;
  portENTER_CRITICAL(&g_log_mux);
  const uint32_t q = ringCountUnsafe();
  if (q < kQueueDepth) {
    g_record_ring[g_ring_head % kQueueDepth] = rec;
    g_ring_head = (uint16_t)(g_ring_head + 1U);
    q_now = ringCountUnsafe();
    ok = true;
  }
  portEXIT_CRITICAL(&g_log_mux);
  return ok;
}

bool ringPop(BinaryLogRecordV1& rec, uint32_t& q_now) {
  bool ok = false;
  portENTER_CRITICAL(&g_log_mux);
  if (g_ring_tail != g_ring_head) {
    rec = g_record_ring[g_ring_tail % kQueueDepth];
    g_ring_tail = (uint16_t)(g_ring_tail + 1U);
    q_now = ringCountUnsafe();
    ok = true;
  } else {
    q_now = 0;
  }
  portEXIT_CRITICAL(&g_log_mux);
  return ok;
}

struct FsLockGuard {
  bool locked = false;
  FsLockGuard() {
    if (g_fs_mutex) {
      locked = xSemaphoreTake(g_fs_mutex, portMAX_DELAY) == pdTRUE;
    }
  }
  ~FsLockGuard() {
    if (locked) {
      xSemaphoreGive(g_fs_mutex);
    }
  }
};

void recordDuration(uint32_t elapsedMs, uint32_t& lastMs, uint32_t& maxMs) {
  lastMs = elapsedMs;
  if (elapsedMs > maxMs) maxMs = elapsedMs;
}

String makeLogName() {
  const uint32_t t = millis();
  String name = String(LOG_DIR) + "/log_" + String(t) + kBinaryExt;
  return name;
}

void closeCurrentLog() {
  if (g_write_used && g_file) {
    const uint32_t t0 = millis();
    const size_t written = g_file.write(g_write_buffer, g_write_used);
    recordDuration(millis() - t0, g_stats.fs_write_last_ms, g_stats.fs_write_max_ms);
    g_stats.bytes_written += (uint32_t)written;
    g_write_used = (written == g_write_used) ? 0U : g_write_used;
  }
  if (g_file) {
    const uint32_t t0 = millis();
    g_file.close();
    recordDuration(millis() - t0, g_stats.fs_close_last_ms, g_stats.fs_close_max_ms);
  }
  g_current_name = "";
}

void ensureFileOpen() {
  if (g_file) return;
  const uint32_t t0 = millis();
  if (!LittleFS.exists(LOG_DIR)) LittleFS.mkdir(LOG_DIR);
  g_current_name = makeLogName();
  g_file = LittleFS.open(g_current_name, FILE_APPEND);
  recordDuration(millis() - t0, g_stats.fs_open_last_ms, g_stats.fs_open_max_ms);
}

bool flushBufferedWrites() {
  if (!g_write_used) return true;
  ensureFileOpen();
  if (!g_file) return false;
  const uint32_t t0 = millis();
  const size_t written = g_file.write(g_write_buffer, g_write_used);
  recordDuration(millis() - t0, g_stats.fs_write_last_ms, g_stats.fs_write_max_ms);
  if (written < g_write_used) {
    const size_t remain = g_write_used - written;
    memmove(g_write_buffer, g_write_buffer + written, remain);
    g_write_used = remain;
  } else {
    g_write_used = 0;
  }
  g_stats.bytes_written += (uint32_t)written;
  return written > 0;
}

bool appendBufferedBytes(const uint8_t* data, size_t len) {
  if (!data || !len) return true;
  if (len > sizeof(g_write_buffer)) {
    if (!flushBufferedWrites()) return false;
    ensureFileOpen();
    if (!g_file) return false;
    const uint32_t t0 = millis();
    const size_t written = g_file.write(data, len);
    recordDuration(millis() - t0, g_stats.fs_write_last_ms, g_stats.fs_write_max_ms);
    g_stats.bytes_written += (uint32_t)written;
    return written == len;
  }
  if ((g_write_used + len) > sizeof(g_write_buffer)) {
    if (!flushBufferedWrites()) return false;
  }
  memcpy(g_write_buffer + g_write_used, data, len);
  g_write_used += len;
  return true;
}

size_t formatCsvLine(const BinaryLogRecordV1& rec, char* out, size_t outSize) {
  if (!out || outSize == 0) return 0;
  const int n = snprintf(
      out, outSize,
      "%lu,%lu,%.3f,%.3f,%.3f,"
      "%lu,%u,%u,%.7f,%.7f,%.3f,%.3f,%.5f,%.3f,%.3f,"
      "%lu,%lu,%lu,%lu,%lu,%lu,"
      "%.3f,%.3f,%.3f,%.3f,"
      "%.3f,%.3f,%.3f,%u,%u\n",
      (unsigned long)rec.seq,
      (unsigned long)rec.t_us,
      (double)rec.state.roll_deg,
      (double)rec.state.pitch_deg,
      (double)rec.state.yaw_deg,
      (unsigned long)rec.state.iTOW_ms,
      (unsigned)rec.state.fixType,
      (unsigned)rec.state.numSV,
      (double)rec.state.lat_1e7 * 1e-7,
      (double)rec.state.lon_1e7 * 1e-7,
      (double)rec.state.hMSL_mm / 1000.0,
      (double)rec.state.gSpeed_mms / 1000.0,
      (double)rec.state.headMot_1e5deg / 100000.0,
      (double)rec.state.hAcc_mm / 1000.0,
      (double)rec.state.sAcc_mms / 1000.0,
      (unsigned long)rec.state.gps_parse_errors,
      (unsigned long)rec.state.mirror_tx_ok,
      (unsigned long)rec.state.mirror_drop_count,
      (unsigned long)rec.state.last_gps_ms,
      (unsigned long)rec.state.last_imu_ms,
      (unsigned long)rec.state.last_baro_ms,
      (double)rec.state.baro_temp_c,
      (double)rec.state.baro_press_hpa,
      (double)rec.state.baro_alt_m,
      (double)rec.state.baro_vsi_mps,
      (double)rec.state.fusion_gain,
      (double)rec.state.fusion_accel_rej,
      (double)rec.state.fusion_mag_rej,
      (unsigned)rec.state.fusion_recovery_period,
      (unsigned)rec.state.flags);
  if (n <= 0 || (size_t)n >= outSize) return 0;
  return (size_t)n;
}

void writerTask(void* param) {
  (void)param;
  BinaryLogRecordV1 record = {};
  for (;;) {
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    uint32_t q_now = 0;
    bool gotRecord = ringPop(record, q_now);
    while (gotRecord) {
      g_last_log_ms = millis();
      g_last_log_t_us = record.t_us;
      FsLockGuard lock;
      if (appendBufferedBytes(reinterpret_cast<const uint8_t*>(&record), sizeof(record))) {
        g_stats.records_written++;
      }
      gotRecord = ringPop(record, q_now);
    }
    g_stats.queue_cur = q_now;

    const uint32_t now = millis();
    {
      FsLockGuard lock;
      const bool batchDue = g_write_used && ((uint32_t)(now - g_last_write_ms) >= kBatchWriteMs);
      const bool bufferNearlyFull = g_write_used >= (sizeof(g_write_buffer) * 3U / 4U);
      if (batchDue || bufferNearlyFull) {
        (void)flushBufferedWrites();
        g_last_write_ms = now;
      }
    }
  }
}

}  // namespace

void begin(const AppConfig& cfg, bool fs_ready) {
  g_cfg = cfg;
  g_fs_ready = fs_ready;
  g_stats = {};
  g_last_log_ms = 0;
  g_last_log_t_us = 0;
  g_last_seq = 0;
  g_last_write_ms = 0;
  g_write_used = 0;
  if (!g_fs_mutex) {
    g_fs_mutex = xSemaphoreCreateMutex();
  }
  {
    FsLockGuard lock;
    if (!LittleFS.exists(LOG_DIR)) LittleFS.mkdir(LOG_DIR);
  }
  portENTER_CRITICAL(&g_log_mux);
  g_ring_head = 0;
  g_ring_tail = 0;
  portEXIT_CRITICAL(&g_log_mux);
  if (!g_fs_ready) {
    return;
  }
  if (!g_writer_task) {
    xTaskCreatePinnedToCore(writerTask, "log_writer", 4096, nullptr, 1, &g_writer_task, 1);
  }
}

void setConfig(const AppConfig& cfg) {
  g_cfg = cfg;
}

void enqueueState(uint32_t seq, uint32_t t_us, const telem::TelemetryFullStateV1& state) {
  if (!g_fs_ready) return;
  if (seq == g_last_seq) return;
  g_last_seq = seq;

  BinaryLogRecordV1 record = {};
  record.magic = kLogMagic;
  record.version = kLogVersion;
  record.record_size = (uint16_t)sizeof(record);
  record.seq = seq;
  record.t_us = t_us;
  record.state = state;

  uint32_t q_now = 0;
  if (ringPush(record, q_now)) {
    g_stats.enqueued++;
    g_stats.queue_cur = q_now;
    if (q_now > g_stats.queue_max) g_stats.queue_max = q_now;
    if (g_writer_task) {
      xTaskNotifyGive(g_writer_task);
    }
  } else {
    g_stats.dropped++;
  }
}

Stats stats() { return g_stats; }

void resetStats() {
  const uint32_t queueCur = g_stats.queue_cur;
  g_stats = {};
  g_stats.queue_cur = queueCur;
}

String filesJson() {
  if (!g_fs_ready) return "[]";
  String out = "[";
  bool first = true;
  FsLockGuard lock;
  File dir = LittleFS.open(LOG_DIR);
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

bool isSafeName(const String& name) {
  if (name.length() == 0 || name.length() > 96) return false;
  if (name.indexOf("..") >= 0 || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return false;
  if (!name.endsWith(".tlog") && !name.endsWith(".csv") && !name.endsWith(".ndjson")) return false;
  return true;
}

bool deleteFileByName(const String& name) {
  if (!g_fs_ready) return false;
  if (!isSafeName(name)) return false;
  const String full = String(LOG_DIR) + "/" + name;
  FsLockGuard lock;
  if (g_current_name == full) {
    closeCurrentLog();
  }
  const uint32_t t0 = millis();
  const bool ok = LittleFS.remove(full);
  recordDuration(millis() - t0, g_stats.fs_delete_last_ms, g_stats.fs_delete_max_ms);
  return ok;
}

}  // namespace log_store
