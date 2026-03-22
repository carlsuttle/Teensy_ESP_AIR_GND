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
    } else {
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
  refreshBackendStatus(true);
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
  refreshBackendStatus(true);
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
  refreshBackendStatus(true);
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
  refreshBackendStatus(false);
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

bool isSafeName(const String& name) {
  if (name.length() == 0 || name.length() > 96) return false;
  if (name.indexOf("..") >= 0 || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return false;
  if (!name.endsWith(".tlog") && !name.endsWith(".csv") && !name.endsWith(".ndjson")) return false;
  return true;
}

bool deleteFileByName(const String& name) {
  if (!sd_backend::mounted()) return false;
  if (!isSafeName(name)) return false;
  const String full = String(LOG_DIR) + "/" + name;
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

}  // namespace log_store
