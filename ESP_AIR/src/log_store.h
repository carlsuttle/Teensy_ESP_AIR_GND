#pragma once

#include <Arduino.h>

#include "config_store.h"
#include "types_shared.h"

class AsyncWebServerRequest;

namespace log_store {

struct Stats {
  uint32_t queue_cur;
  uint32_t queue_max;
  uint32_t enqueued;
  uint32_t dropped;
  uint32_t records_written;
  uint32_t bytes_written;
  uint32_t flushes;
  uint32_t fs_open_last_ms;
  uint32_t fs_open_max_ms;
  uint32_t fs_write_last_ms;
  uint32_t fs_write_max_ms;
  uint32_t fs_close_last_ms;
  uint32_t fs_close_max_ms;
  uint32_t fs_delete_last_ms;
  uint32_t fs_delete_max_ms;
  uint32_t fs_download_last_ms;
  uint32_t fs_download_max_ms;
};

void begin(const AppConfig& cfg);
void setConfig(const AppConfig& cfg);
void enqueueState(uint32_t seq, uint32_t t_us, const telem::TelemetryFullStateV1& state);
Stats stats();
void resetStats();

String filesJson();
bool deleteFileByName(const String& name);
bool isSafeName(const String& name);
void sendDownload(AsyncWebServerRequest* req, const String& name);

}  // namespace log_store
