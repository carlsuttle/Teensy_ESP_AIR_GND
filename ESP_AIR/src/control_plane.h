#pragma once

#include <Arduino.h>

#include "log_store.h"
#include "replay_bridge.h"
#include "sd_file_api.h"
#include "time_service.h"
#include "types_shared.h"

namespace control_plane {

enum class SourceInterface : uint8_t {
  Unknown = 0U,
  SerialConsole = 1U,
  Radio = 2U,
  WebForwarded = 3U,
};

enum class RequestCategory : uint8_t {
  None = 0U,
  Recording = 1U,
  Replay = 2U,
  FileSd = 3U,
  Fusion = 4U,
  Status = 5U,
};

enum class SystemMode : uint8_t {
  Idle = 0U,
  Recording = 1U,
  Replay = 2U,
  RecordingBusy = 3U,
  ReplayBusy = 4U,
};

enum class ActivityState : uint8_t {
  Idle = 0U,
  Active = 1U,
  Busy = 2U,
};

enum class FileListState : uint8_t {
  Idle = 0U,
  Busy = 1U,
  Valid = 2U,
};

enum class DispositionStatus : uint8_t {
  Accepted = 0U,
  Busy = 1U,
  Rejected = 2U,
};

enum class CompletionStatus : uint8_t {
  None = 0U,
  CompletedOk = 1U,
  CompletedError = 2U,
};

enum class ControlCode : uint32_t {
  Ok = 0U,
  InvalidArgument = 1U,
  SdNotReady = 2U,
  BusyRecording = 3U,
  BusyReplay = 4U,
  NotSupported = 5U,
  InvalidHandle = 6U,
  NoFiles = 7U,
  IoError = 8U,
  InternalError = 9U,
  FileNotFound = 10U,
  AlreadyExists = 11U,
  BusyRequest = 12U,
  BackendFailed = 13U,
};

enum class RequestAction : uint8_t {
  None = 0U,
  RecordStart,
  RecordStop,
  RecordStatus,
  ReplayStartLatest,
  ReplayStartFile,
  ReplayStop,
  ReplayPause,
  ReplaySeekRelative,
  ReplayStatus,
  FileListPage,
  FileListJson,
  StorageStatus,
  MountMedia,
  EjectMedia,
  DeleteFile,
  RenameFile,
  ExportCsv,
  SetRecordPrefix,
  FusionSet,
  FusionGet,
};

struct Request {
  uint32_t request_id = 0U;
  SourceInterface source = SourceInterface::Unknown;
  RequestCategory category = RequestCategory::None;
  RequestAction action = RequestAction::None;
  uint16_t command_id = 0U;
  uint16_t offset = 0U;
  uint16_t limit = 0U;
  uint32_t session_id = 0U;
  bool has_explicit_session_id = false;
  int32_t delta_records = 0;
  telem::CmdSetFusionSettingsV1 fusion = {};
  log_store::FileSortKey sort_key = log_store::FileSortKey::date;
  log_store::FileSortDirection sort_dir = log_store::FileSortDirection::descending;
  Stream* io_stream = nullptr;
  char name[telem::kLogFileNameBytes] = {};
  char aux_name[telem::kLogFileNameBytes] = {};
  char prefix[telem::kRecordPrefixBytes] = {};
};

struct Result {
  uint32_t request_id = 0U;
  DispositionStatus disposition = DispositionStatus::Rejected;
  CompletionStatus completion = CompletionStatus::None;
  bool accepted = false;
  bool completed = false;
  bool ok = false;
  uint32_t disposition_code = (uint32_t)ControlCode::InvalidArgument;
  uint32_t completion_code = (uint32_t)ControlCode::InvalidArgument;
  uint32_t payload_ref = 0U;
  uint32_t code = (uint32_t)ControlCode::InvalidArgument;
  bool has_storage_status = false;
  bool has_log_status = false;
  bool has_replay_status = false;
  bool has_file_list_page = false;
  bool has_files_json = false;
  bool has_fusion_settings = false;
  telem::StorageStatusPayloadV1 storage_status = {};
  telem::LogStatusPayloadV1 log_status = {};
  telem::ReplayStatusPayloadV1 replay_status = {};
  telem::FusionSettingsV1 fusion_settings = {};
  String files_json;
};

struct StateSnapshot {
  SystemMode system_mode = SystemMode::Idle;
  bool request_pending = false;
  uint32_t pending_request_id = 0U;
  SourceInterface pending_source = SourceInterface::Unknown;
  RequestCategory pending_category = RequestCategory::None;
  RequestAction pending_action = RequestAction::None;
  uint32_t last_request_id = 0U;
  uint32_t last_completed_request_id = 0U;
  uint32_t last_result_code = 0U;
  uint16_t last_command_id = 0U;
  SourceInterface last_source = SourceInterface::Unknown;
  RequestCategory last_category = RequestCategory::None;
  RequestAction last_action = RequestAction::None;
  bool last_ok = false;
  bool recording_requested = false;
  uint32_t recording_session_id = 0U;
  uint16_t recording_last_command = 0U;
  uint32_t recording_last_change_ms = 0U;
  ActivityState recording_state = ActivityState::Idle;
  ActivityState replay_state = ActivityState::Idle;
  FileListState file_list_state = FileListState::Idle;
  bool replay_paused = false;
  bool replay_file_open = false;
  uint16_t replay_last_command = 0U;
  uint32_t file_list_generation = 0U;
  bool file_list_valid = false;
  bool sd_ready = false;
  bool sd_mounted = false;
  bool sd_media_present = false;
  uint8_t time_state = (uint8_t)telem::TimeStateCode::UNSET;
  uint8_t time_source = (uint8_t)telem::TimeSourceCode::NONE;
  uint8_t time_flags = 0U;
  bool has_fusion_settings = false;
  telem::FusionSettingsV1 fusion_settings = {};
  char selected_file[telem::kLogFileNameBytes] = {};
};

struct ResultSnapshot {
  uint32_t request_id = 0U;
  DispositionStatus disposition = DispositionStatus::Rejected;
  CompletionStatus completion = CompletionStatus::None;
  bool accepted = false;
  bool completed = false;
  bool ok = false;
  uint32_t disposition_code = (uint32_t)ControlCode::InvalidArgument;
  uint32_t completion_code = (uint32_t)ControlCode::InvalidArgument;
  uint32_t code = (uint32_t)ControlCode::InvalidArgument;
};

inline const char* dispositionText(DispositionStatus status) {
  switch (status) {
    case DispositionStatus::Accepted: return "accepted";
    case DispositionStatus::Busy: return "busy";
    case DispositionStatus::Rejected: return "rejected";
    default: return "rejected";
  }
}

inline const char* completionText(CompletionStatus status) {
  switch (status) {
    case CompletionStatus::None: return "none";
    case CompletionStatus::CompletedOk: return "completed_ok";
    case CompletionStatus::CompletedError: return "completed_error";
    default: return "none";
  }
}

inline const char* controlCodeText(uint32_t code) {
  switch ((ControlCode)code) {
    case ControlCode::Ok: return "ok";
    case ControlCode::InvalidArgument: return "invalid_argument";
    case ControlCode::SdNotReady: return "sd_not_ready";
    case ControlCode::BusyRecording: return "busy_recording";
    case ControlCode::BusyReplay: return "busy_replay";
    case ControlCode::NotSupported: return "not_supported";
    case ControlCode::InvalidHandle: return "invalid_handle";
    case ControlCode::NoFiles: return "no_files";
    case ControlCode::IoError: return "io_error";
    case ControlCode::InternalError: return "internal_error";
    case ControlCode::FileNotFound: return "file_not_found";
    case ControlCode::AlreadyExists: return "already_exists";
    case ControlCode::BusyRequest: return "busy_request";
    case ControlCode::BackendFailed: return "backend_failed";
    default: return "unknown";
  }
}

void begin();
bool submit(const Request& request, Result& out_result);
StateSnapshot stateSnapshot(uint32_t now_ms);
ResultSnapshot lastResultSnapshot();
telem::LogStatusPayloadV1 currentLogStatusPayload(uint32_t now_ms);
const sd_file_api::FileListPage& lastFileListPage();
void noteRecorderFeatureEnabled(bool enabled, uint32_t now_ms);

}  // namespace control_plane
