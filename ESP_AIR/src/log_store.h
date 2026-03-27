#pragma once

#include <Arduino.h>

#include "config_store.h"
#include "types_shared.h"

namespace log_store {

enum class FileSortKey : uint8_t {
  name = 0,
  size,
  date,
};

enum class FileSortDirection : uint8_t {
  ascending = 0,
  descending,
};

struct Stats {
  uint32_t queue_cur = 0;
  uint32_t queue_max = 0;
  uint32_t enqueued = 0;
  uint32_t dropped = 0;
  uint32_t records_written = 0;
  uint32_t bytes_written = 0;
  uint32_t flushes = 0;
  uint32_t fs_open_last_ms = 0;
  uint32_t fs_open_max_ms = 0;
  uint32_t fs_write_last_ms = 0;
  uint32_t fs_write_max_ms = 0;
  uint32_t fs_close_last_ms = 0;
  uint32_t fs_close_max_ms = 0;
  uint32_t fs_delete_last_ms = 0;
  uint32_t fs_delete_max_ms = 0;
  uint32_t fs_download_last_ms = 0;
  uint32_t fs_download_max_ms = 0;
  uint32_t no_free_block_events = 0;
  uint32_t blocks_written = 0;
  uint32_t blocks_dropped = 0;
  uint32_t min_free_blocks_seen = 0;
  uint32_t max_write_bytes = 0;
};

struct RecorderStatus {
  bool feature_enabled = false;
  bool backend_ready = false;
  bool media_present = false;
  bool active = false;
  uint32_t session_id = 0;
  uint32_t bytes_written = 0;
  uint32_t free_bytes = telem::kLogBytesUnknown;
  uint32_t init_hz = 0;
};

void begin(const AppConfig& cfg, bool enabled = true);
void setConfig(const AppConfig& cfg);
void setEnabled(bool enabled);
bool startSession(uint32_t session_id);
void stopSession();
void poll();
void enqueueState(uint32_t seq, uint32_t t_us, const telem::TelemetryFullStateV1& state);
void enqueueReplayInput(uint32_t seq, uint32_t t_us, const telem::ReplayInputRecord160& replay);
void enqueueReplayControl(uint16_t command_id, uint32_t seq, uint32_t t_us,
                          const void* payload, uint16_t payload_len, uint32_t apply_flags);
bool active();
void probeBackend();
RecorderStatus recorderStatus();
Stats stats();
void resetStats();

String filesJson(FileSortKey sort_key = FileSortKey::date,
                 FileSortDirection sort_dir = FileSortDirection::descending);
bool listFiles(telem::LogFileInfoV1* out_files, uint16_t max_files, uint16_t offset,
               uint16_t& total_files, uint16_t& returned_files,
               FileSortKey sort_key = FileSortKey::date,
               FileSortDirection sort_dir = FileSortDirection::descending);
bool deleteFileByName(const String& name);
bool renameFileByName(const String& src_name, const String& dst_name);
bool isSafeName(const String& name);
bool exportLogToCsvByName(const String& name, Stream* out = nullptr);
bool exportAllLogsToCsv(Stream& out);
bool busy();
String currentFileName();
String previewLogName(uint32_t session_id = 0U);
String recordPrefix();
bool latestLogName(String& out_name);
bool largestLogName(String& out_name);
bool latestLogNameForSession(uint32_t session_id, String& out_name);
uint32_t highestLogSessionId();
bool copyLatestLogAndVerify(Stream& out);
bool compareLogs(Stream& out, const String& src_name, const String& dst_name);
bool compareLogsTimed(Stream& out, const String& src_name, const String& dst_name, uint16_t warmup_skip = 4U);
bool printRecordKindSummary(Stream& out, const String& log_name);
bool printFusionSettingsSummary(Stream& out, const String& log_name);
bool printFusionFlagSummary(Stream& out, const String& log_name);

}  // namespace log_store
