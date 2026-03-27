#include "sd_capture_test.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

#include "sd_api.h"
#include "sd_backend.h"

namespace sd_capture_test {
namespace {
using File = sd_api::File;

constexpr char kBinaryExt[] = ".tlog";
constexpr size_t kQueueDepth = 512U;
constexpr size_t kWriteBufferBytes = 10000U;
constexpr uint32_t kFlushIntervalMs = 250U;
constexpr uint8_t kWriteRetryCount = 8U;
constexpr uint32_t kWriteRetryDelayMs = 2U;
constexpr uint32_t kLogMagic = 0x53444731UL;  // "SDG1"
constexpr uint16_t kLogVersion = 1U;

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

Stats g_stats = {};
File g_file;
SemaphoreHandle_t g_mutex = nullptr;
TaskHandle_t g_writer_task = nullptr;
BinaryLogRecordV1 g_ring[kQueueDepth] = {};
uint8_t g_write_buffer[kWriteBufferBytes] = {};
size_t g_write_used = 0U;
uint32_t g_write_buffer_records = 0U;
uint32_t g_last_write_ms = 0U;
uint16_t g_ring_head = 0U;
uint16_t g_ring_tail = 0U;
portMUX_TYPE g_ring_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t ringCountUnsafe() {
  return (uint16_t)(g_ring_head - g_ring_tail);
}

bool ringPush(const BinaryLogRecordV1& rec, uint32_t& q_now) {
  bool ok = false;
  portENTER_CRITICAL(&g_ring_mux);
  const uint32_t q = ringCountUnsafe();
  if (q < kQueueDepth) {
    g_ring[g_ring_head % kQueueDepth] = rec;
    g_ring_head = (uint16_t)(g_ring_head + 1U);
    q_now = ringCountUnsafe();
    ok = true;
  }
  portEXIT_CRITICAL(&g_ring_mux);
  return ok;
}

bool ringPop(BinaryLogRecordV1& rec, uint32_t& q_now) {
  bool ok = false;
  portENTER_CRITICAL(&g_ring_mux);
  if (g_ring_tail != g_ring_head) {
    rec = g_ring[g_ring_tail % kQueueDepth];
    g_ring_tail = (uint16_t)(g_ring_tail + 1U);
    q_now = ringCountUnsafe();
    ok = true;
  } else {
    q_now = 0U;
  }
  portEXIT_CRITICAL(&g_ring_mux);
  return ok;
}

struct LockGuard {
  bool locked = false;
  LockGuard() {
    if (g_mutex) locked = xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE;
  }
  ~LockGuard() {
    if (locked) xSemaphoreGive(g_mutex);
  }
};

void recordDuration(uint32_t elapsed_ms, uint32_t& last_ms, uint32_t& max_ms) {
  last_ms = elapsed_ms;
  if (elapsed_ms > max_ms) max_ms = elapsed_ms;
}

void fillPinStats() {
  g_stats.cs_pin = sd_backend::csPin();
  g_stats.sck_pin = sd_backend::sckPin();
  g_stats.miso_pin = sd_backend::misoPin();
  g_stats.mosi_pin = sd_backend::mosiPin();
}

void setError(const char* text) {
  strlcpy(g_stats.last_error, text ? text : "", sizeof(g_stats.last_error));
}

bool initSd() {
  g_stats.backend_ready = false;
  g_stats.init_hz = 0U;
  setError("");
  sd_backend::Status backend = {};
  if (!sd_backend::begin(&backend)) {
    setError("sd begin failed");
    return false;
  }
  g_stats.init_hz = backend.init_hz;
  g_stats.backend_ready = backend.begin_ok;
  return true;
}

void resetQueue() {
  portENTER_CRITICAL(&g_ring_mux);
  g_ring_head = 0U;
  g_ring_tail = 0U;
  portEXIT_CRITICAL(&g_ring_mux);
  g_write_used = 0U;
  g_write_buffer_records = 0U;
}

bool ensureFileOpen() {
  if (g_file) return true;
  const uint32_t t0 = millis();
  char file_name[48];
  snprintf(file_name, sizeof(file_name), "/cap_%lu%s",
           (unsigned long)g_stats.started_ms, kBinaryExt);
  strlcpy(g_stats.file_name, file_name, sizeof(g_stats.file_name));
  g_file = sd_api::open(file_name, sd_api::OpenMode::write);
  recordDuration(millis() - t0, g_stats.fs_open_last_ms, g_stats.fs_open_max_ms);
  if (!g_file) {
    setError("file open failed");
  }
  return (bool)g_file;
}

bool writeAllLocked(const uint8_t* data, size_t len, uint32_t& elapsed_ms) {
  elapsed_ms = 0U;
  if (!ensureFileOpen()) return false;
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
      elapsed_ms = millis() - t0;
      return false;
    }
    total_written += written;
  }
  elapsed_ms = millis() - t0;
  return true;
}

bool flushBufferLocked() {
  if (!g_write_used) return true;
  uint32_t elapsed_ms = 0U;
  if (!writeAllLocked(g_write_buffer, g_write_used, elapsed_ms)) {
    recordDuration(elapsed_ms, g_stats.fs_write_last_ms, g_stats.fs_write_max_ms);
    setError("file write failed");
    return false;
  }
  recordDuration(elapsed_ms, g_stats.fs_write_last_ms, g_stats.fs_write_max_ms);
  g_stats.bytes_written += (uint32_t)g_write_used;
  g_stats.records_written += g_write_buffer_records;
  g_write_used = 0U;
  g_write_buffer_records = 0U;
  g_stats.flushes++;
  setError("");
  return true;
}

bool appendRecordLocked(const BinaryLogRecordV1& rec) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&rec);
  const size_t len = sizeof(rec);
  if ((g_write_used + len) > sizeof(g_write_buffer)) {
    if (!flushBufferLocked()) return false;
  }
  memcpy(g_write_buffer + g_write_used, bytes, len);
  g_write_used += len;
  g_write_buffer_records++;
  return true;
}

void closeFileLocked() {
  (void)flushBufferLocked();
  if (g_file) {
    const uint32_t t0 = millis();
    g_file.flush();
    g_file.close();
    recordDuration(millis() - t0, g_stats.fs_close_last_ms, g_stats.fs_close_max_ms);
    if (g_write_used == 0U) {
      setError("");
    }
  }
}

void writerTask(void* param) {
  (void)param;
  BinaryLogRecordV1 record = {};
  for (;;) {
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    uint32_t q_now = 0U;
    bool have = ringPop(record, q_now);
    while (have) {
      {
        LockGuard lock;
        if (g_stats.active || g_write_used != 0U) {
          if (!appendRecordLocked(record)) {
            g_stats.active = false;
            g_stats.completed = true;
            g_stats.stopped_ms = millis();
          }
        }
      }
      have = ringPop(record, q_now);
    }
    g_stats.queue_cur = q_now;

    const uint32_t now = millis();
    LockGuard lock;
    if (g_write_used && (uint32_t)(now - g_last_write_ms) >= kFlushIntervalMs) {
      (void)flushBufferLocked();
      g_last_write_ms = now;
    }
    if (!g_stats.active && g_write_used) {
      (void)flushBufferLocked();
      g_last_write_ms = now;
    }
  }
}

}  // namespace

void begin() {
  fillPinStats();
  if (!g_mutex) g_mutex = xSemaphoreCreateMutex();
  if (!g_writer_task) {
    xTaskCreatePinnedToCore(writerTask, "sd_cap_writer", 4096, nullptr,
                            sd_backend::kSdWriterPriority, &g_writer_task, sd_backend::kSdWriterCore);
  }
}

bool start(uint32_t duration_ms) {
  if (duration_ms == 0U) duration_ms = 60000U;
  stop(false);
  g_stats = {};
  fillPinStats();
  g_stats.duration_ms = duration_ms;
  g_stats.started_ms = millis();
  g_stats.last_start_ok = false;
  g_stats.completed = false;
  g_stats.timed_out = false;
  resetQueue();

  LockGuard lock;
  if (!initSd()) {
    g_stats.stopped_ms = millis();
    return false;
  }
  if (!ensureFileOpen()) {
    g_stats.stopped_ms = millis();
    return false;
  }
  g_stats.active = true;
  g_stats.last_start_ok = true;
  setError("");
  g_last_write_ms = millis();
  return true;
}

void stop(bool timed_out) {
  const bool was_active = g_stats.active;
  g_stats.active = false;
  if (!was_active && !g_file) return;
  {
    LockGuard lock;
    closeFileLocked();
  }
  g_stats.stopped_ms = millis();
  g_stats.completed = true;
  g_stats.timed_out = timed_out;
  g_stats.queue_cur = 0U;
}

void poll() {
  if (!g_stats.active) return;
  if ((uint32_t)(millis() - g_stats.started_ms) >= g_stats.duration_ms) {
    stop(true);
  }
}

void enqueueState(uint32_t seq, uint32_t t_us, const telem::TelemetryFullStateV1& state) {
  if (!g_stats.active) return;

  BinaryLogRecordV1 rec = {};
  rec.magic = kLogMagic;
  rec.version = kLogVersion;
  rec.record_size = (uint16_t)sizeof(rec);
  rec.seq = seq;
  rec.t_us = t_us;
  rec.state = state;

  uint32_t q_now = 0U;
  if (ringPush(rec, q_now)) {
    g_stats.enqueued++;
    g_stats.queue_cur = q_now;
    if (q_now > g_stats.queue_max) g_stats.queue_max = q_now;
    if (g_writer_task) xTaskNotifyGive(g_writer_task);
  } else {
    g_stats.dropped++;
  }
}

Stats stats() {
  return g_stats;
}

void clearCompleted() {
  g_stats.completed = false;
}

void printReport(Stream& out, const Stats& stats) {
  out.printf("SDCAP active=%u backend_ready=%u start_ok=%u timed_out=%u duration_ms=%lu init_hz=%lu\r\n",
             stats.active ? 1U : 0U,
             stats.backend_ready ? 1U : 0U,
             stats.last_start_ok ? 1U : 0U,
             stats.timed_out ? 1U : 0U,
             (unsigned long)stats.duration_ms,
             (unsigned long)stats.init_hz);
  out.printf("SDCAP pins cs=%u sck=%u miso=%u mosi=%u file=%s err=%s\r\n",
             (unsigned)stats.cs_pin,
             (unsigned)stats.sck_pin,
             (unsigned)stats.miso_pin,
             (unsigned)stats.mosi_pin,
             stats.file_name[0] ? stats.file_name : "(none)",
             stats.last_error[0] ? stats.last_error : "(none)");
  out.printf("SDCAP queue_cur=%lu queue_max=%lu enqueued=%lu dropped=%lu written=%lu bytes=%lu flushes=%lu\r\n",
             (unsigned long)stats.queue_cur,
             (unsigned long)stats.queue_max,
             (unsigned long)stats.enqueued,
             (unsigned long)stats.dropped,
             (unsigned long)stats.records_written,
             (unsigned long)stats.bytes_written,
             (unsigned long)stats.flushes);
  out.printf("SDCAP fs_open_last_ms=%lu fs_open_max_ms=%lu fs_write_last_ms=%lu fs_write_max_ms=%lu fs_close_last_ms=%lu fs_close_max_ms=%lu\r\n",
             (unsigned long)stats.fs_open_last_ms,
             (unsigned long)stats.fs_open_max_ms,
             (unsigned long)stats.fs_write_last_ms,
             (unsigned long)stats.fs_write_max_ms,
             (unsigned long)stats.fs_close_last_ms,
             (unsigned long)stats.fs_close_max_ms);
}

}  // namespace sd_capture_test
