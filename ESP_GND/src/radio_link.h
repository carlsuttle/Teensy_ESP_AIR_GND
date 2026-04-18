#pragma once

#include <Arduino.h>

#include "config_store.h"
#include "types_shared.h"

namespace radio_link {

struct Stats {
  uint32_t rx_packets = 0;
  uint32_t rx_bytes = 0;
  uint32_t frames_ok = 0;
  uint32_t state_packets = 0;
  uint32_t state_seq_gap = 0;
  uint32_t state_seq_rewind = 0;
  uint32_t crc_err = 0;
  uint32_t cobs_err = 0;
  uint32_t len_err = 0;
  uint32_t unknown_msg = 0;
  uint32_t drop = 0;
  uint32_t last_rx_ms = 0;
  uint32_t last_state_apply_ms = 0;
  uint32_t last_state_seq = 0;
  uint32_t last_state_source_t_us = 0;
};

struct Snapshot {
  bool has_state = false;
  bool has_ack = false;
  bool has_fusion_settings = false;
  bool has_link_meta = false;
  bool has_log_status = false;
  bool has_replay_status = false;
  bool has_live_status = false;
  uint32_t seq = 0;
  uint32_t boot_id = 0;
  uint32_t t_us = 0;
  uint32_t live_flags = 0;
  telem::TelemetryStateRecord state = {};
  telem::DownlinkStatusV1 live_status = {};
  telem::FusionSettingsV1 fusion_settings = {};
  telem::LinkMetaPayloadV1 link_meta = {};
  telem::LogStatusPayloadV1 log_status = {};
  telem::ReplayStatusPayloadV1 replay_status = {};
  uint32_t desired_control_gen = 0U;
  uint32_t applied_control_gen = 0U;
  uint32_t control_code = 0U;
  uint16_t ack_command = 0;
  bool ack_ok = false;
  uint32_t ack_code = 0;
  uint32_t ack_rx_seq = 0;
  uint32_t radio_rtt_ms = 0;
  uint32_t radio_rtt_avg_ms = 0;
  uint32_t last_radio_pong_ms = 0;
  uint32_t uplink_ping_sent = 0;
  uint32_t uplink_ping_ok = 0;
  uint32_t uplink_ping_timeout = 0;
  uint32_t uplink_ping_miss_streak = 0;
  uint32_t last_uplink_ack_ms = 0;
  Stats stats = {};
};

struct RemoteFilesStatus {
  uint32_t revision = 0;
  uint16_t total_files = 0;
  uint16_t stored_files = 0;
  uint16_t page_offset = 0;
  uint16_t page_limit = 0;
  uint32_t last_update_ms = 0;
  bool complete = false;
  bool refresh_inflight = false;
  bool truncated = false;
};

struct RemoteStorageStatus {
  bool known = false;
  uint32_t revision = 0;
  uint32_t last_update_ms = 0;
  uint8_t media_state = 0;
  bool mounted = false;
  bool backend_ready = false;
  bool media_present = false;
  bool busy = false;
  uint32_t init_hz = 0;
  uint32_t free_bytes = telem::kLogBytesUnknown;
  uint32_t total_bytes = 0;
  uint16_t file_count = 0;
  uint8_t time_state = (uint8_t)telem::TimeStateCode::UNSET;
  uint8_t time_source = (uint8_t)telem::TimeSourceCode::NONE;
  uint8_t time_flags = 0U;
  uint32_t system_time_utc_s = 0U;
  uint32_t time_last_set_age_ms = 0xFFFFFFFFUL;
  uint16_t time_sync_count = 0U;
  char record_prefix[telem::kRecordPrefixBytes] = {};
  char next_record_name[telem::kLogFileNameBytes] = {};
};

void begin(const AppConfig& cfg);
void setNetworkReady(const char* reason);
void reconfigure(const AppConfig& cfg);
void restart(const AppConfig& cfg);
void poll();
Snapshot snapshot();
void resetStats();
bool sendSetFusionSettings(const telem::CmdSetFusionSettingsV1& cmd);
uint32_t desiredControlGen();
bool sendGetFusionSettings();
bool sendSetStreamRate(const telem::CmdSetStreamRateV1& cmd);
bool sendSetRadioMode(const telem::CmdSetRadioModeV1& cmd);
bool sendResetNetwork();
bool sendLogStart();
bool sendLogStop();
bool sendGetLogStatus();
bool sendReplayStart();
bool sendReplayStartFile(const String& name);
bool sendReplayStop();
bool sendReplayPause();
bool sendReplaySeekRelative(int32_t delta_records);
bool sendGetReplayStatus();
bool sendGetLogFileList(uint16_t offset = 0U, uint16_t limit = 32U);
bool sendDeleteLogFile(const String& name);
bool sendRenameLogFile(const String& src_name, const String& dst_name);
bool sendGetStorageStatus();
bool sendMountMedia();
bool sendEjectMedia();
bool sendExportLogCsv(const String& name);
bool sendSetRecordPrefix(const String& prefix);
RemoteFilesStatus remoteFilesStatus();
String remoteFilesJson(bool refresh_requested);
RemoteStorageStatus remoteStorageStatus();
String remoteStorageJson(bool refresh_requested);
bool hasLearnedSender();
String targetSenderMac();
String lastSenderMac();

}  // namespace radio_link
