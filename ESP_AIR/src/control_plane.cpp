#include "control_plane.h"

#include "config_store.h"
#include "teensy_link.h"

namespace control_plane {
namespace {

struct StateRegisters {
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
  uint32_t recording_session_cursor = 0U;
  uint32_t recording_session_id = 0U;
  uint16_t recording_last_command = 0U;
  uint32_t recording_last_change_ms = 0U;
  ResultSnapshot last_result = {};
  bool file_list_busy = false;
};

StateRegisters g_state = {};
sd_file_api::FileListPage g_last_file_list_page = {};

uint32_t mapSdCode(telem::SdApiStatusCode code) { return (uint32_t)code; }

void recordDisposition(const Request& request, DispositionStatus disposition, uint32_t code) {
  g_state.last_request_id = request.request_id;
  g_state.last_result_code = code;
  g_state.last_command_id = request.command_id;
  g_state.last_source = request.source;
  g_state.last_category = request.category;
  g_state.last_action = request.action;
  g_state.last_result.request_id = request.request_id;
  g_state.last_result.disposition = disposition;
  g_state.last_result.completion = CompletionStatus::None;
  g_state.last_result.accepted = disposition == DispositionStatus::Accepted;
  g_state.last_result.completed = false;
  g_state.last_result.ok = false;
  g_state.last_result.disposition_code = code;
  g_state.last_result.completion_code = code;
  g_state.last_result.code = code;
}

void recordCompletion(const Request& request, CompletionStatus completion, uint32_t code) {
  const bool ok = completion == CompletionStatus::CompletedOk;
  g_state.last_completed_request_id = request.request_id;
  g_state.last_result_code = code;
  g_state.last_ok = ok;
  g_state.last_result.completion = completion;
  g_state.last_result.completed = completion != CompletionStatus::None;
  g_state.last_result.ok = ok;
  g_state.last_result.completion_code = code;
  g_state.last_result.code = code;
}

bool isFileAction(RequestAction action) {
  switch (action) {
    case RequestAction::FileListPage:
    case RequestAction::FileListJson:
    case RequestAction::MountMedia:
    case RequestAction::EjectMedia:
    case RequestAction::DeleteFile:
    case RequestAction::RenameFile:
    case RequestAction::ExportCsv:
    case RequestAction::SetRecordPrefix:
    case RequestAction::StorageStatus:
      return true;
    default:
      return false;
  }
}

bool requiresReplayIdle(RequestAction action) {
  return action == RequestAction::RecordStart;
}

bool requiresRecordingIdle(RequestAction action) {
  switch (action) {
    case RequestAction::ReplayStartLatest:
    case RequestAction::ReplayStartFile:
    case RequestAction::ReplayPause:
    case RequestAction::ReplaySeekRelative:
      return true;
    default:
      return false;
  }
}

bool nameRequired(RequestAction action) {
  return action == RequestAction::DeleteFile || action == RequestAction::ExportCsv ||
         action == RequestAction::ReplayStartFile;
}

uint32_t validateRequest(const Request& request) {
  if (request.action == RequestAction::None) return (uint32_t)ControlCode::InvalidArgument;
  if (nameRequired(request.action) && request.name[0] == '\0') return (uint32_t)ControlCode::InvalidArgument;
  if (request.action == RequestAction::RenameFile &&
      (request.name[0] == '\0' || request.aux_name[0] == '\0')) {
    return (uint32_t)ControlCode::InvalidArgument;
  }

  const bool recording_active = log_store::active() || log_store::busy();
  const replay_bridge::Status replay = replay_bridge::status();
  const bool replay_active = (replay.flags & telem::kReplayStatusFlagActive) != 0U ||
                             (replay.flags & telem::kReplayStatusFlagFileOpen) != 0U;

  if (isFileAction(request.action)) {
    if (request.action != RequestAction::StorageStatus && request.action != RequestAction::MountMedia) {
      if (recording_active) return (uint32_t)ControlCode::BusyRecording;
      if (replay_active) return (uint32_t)ControlCode::BusyReplay;
    }
  }
  if (requiresReplayIdle(request.action) && replay_active) return (uint32_t)ControlCode::BusyReplay;
  if (requiresRecordingIdle(request.action) && recording_active) return (uint32_t)ControlCode::BusyRecording;
  return (uint32_t)ControlCode::Ok;
}

uint32_t nextRecordingSessionId(const Request& request) {
  if (request.has_explicit_session_id && request.session_id != 0U) {
    g_state.recording_session_cursor = request.session_id;
    return request.session_id;
  }
  if (g_state.recording_session_cursor == 0U) {
    g_state.recording_session_cursor = log_store::highestLogSessionId();
  }
  g_state.recording_session_cursor++;
  if (g_state.recording_session_cursor == 0U) g_state.recording_session_cursor = 1U;
  return g_state.recording_session_cursor;
}

telem::ReplayStatusPayloadV1 currentReplayPayload() {
  return replay_bridge::currentPayload();
}

telem::StorageStatusPayloadV1 currentStoragePayload() {
  telem::StorageStatusPayloadV1 payload = {};
  (void)sd_file_api::getStorageStatus(payload);
  return payload;
}

}  // namespace

void begin() {
  g_state = {};
  g_last_file_list_page = {};
  g_state.recording_session_cursor = log_store::highestLogSessionId();
}

bool submit(const Request& request, Result& out_result) {
  bool clear_pending_on_exit = false;
  struct PendingGuard {
    bool& armed;
    StateRegisters& state;
    ~PendingGuard() {
      if (!armed) return;
      state.request_pending = false;
      state.pending_request_id = 0U;
      state.pending_source = SourceInterface::Unknown;
      state.pending_category = RequestCategory::None;
      state.pending_action = RequestAction::None;
      armed = false;
    }
  } pending_guard{clear_pending_on_exit, g_state};

  out_result = {};
  out_result.request_id = request.request_id;
  out_result.disposition = DispositionStatus::Rejected;
  out_result.completion = CompletionStatus::None;
  out_result.disposition_code = (uint32_t)ControlCode::InvalidArgument;
  out_result.completion_code = (uint32_t)ControlCode::InvalidArgument;
  out_result.code = out_result.disposition_code;

  if (g_state.request_pending) {
    out_result.disposition = DispositionStatus::Busy;
    out_result.disposition_code = (uint32_t)ControlCode::BusyRequest;
    out_result.code = out_result.disposition_code;
    recordDisposition(request, out_result.disposition, out_result.disposition_code);
    return false;
  }

  const uint32_t validate_code = validateRequest(request);
  if (validate_code != (uint32_t)ControlCode::Ok) {
    out_result.disposition = (validate_code == (uint32_t)ControlCode::BusyRecording ||
                              validate_code == (uint32_t)ControlCode::BusyReplay)
                                 ? DispositionStatus::Rejected
                                 : DispositionStatus::Rejected;
    out_result.disposition_code = validate_code;
    out_result.code = validate_code;
    recordDisposition(request, out_result.disposition, out_result.disposition_code);
    return false;
  }

  g_state.request_pending = true;
  g_state.pending_request_id = request.request_id;
  g_state.pending_source = request.source;
  g_state.pending_category = request.category;
  g_state.pending_action = request.action;
  clear_pending_on_exit = true;
  out_result.disposition = DispositionStatus::Accepted;
  out_result.accepted = true;
  out_result.disposition_code = (uint32_t)ControlCode::Ok;
  out_result.code = out_result.disposition_code;
  recordDisposition(request, out_result.disposition, out_result.disposition_code);

  switch (request.action) {
    case RequestAction::StateGet: {
      out_result.completed = true;
      out_result.completion = CompletionStatus::CompletedOk;
      out_result.ok = true;
      out_result.completion_code = (uint32_t)ControlCode::Ok;
      out_result.code = out_result.completion_code;
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::RecordStart: {
      const uint32_t session_id = nextRecordingSessionId(request);
      const bool ok = log_store::startSession(session_id);
      g_state.recording_requested = ok;
      g_state.recording_session_id = session_id;
      g_state.recording_last_command = request.command_id;
      g_state.recording_last_change_ms = millis();
      out_result.completed = true;
      out_result.completion = ok ? CompletionStatus::CompletedOk : CompletionStatus::CompletedError;
      out_result.ok = ok;
      out_result.completion_code = ok ? (uint32_t)ControlCode::Ok : (uint32_t)ControlCode::BackendFailed;
      out_result.code = out_result.completion_code;
      out_result.has_log_status = true;
      out_result.log_status = currentLogStatusPayload(millis());
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::RecordStop: {
      log_store::stopSession();
      g_state.recording_requested = false;
      g_state.recording_last_command = request.command_id;
      g_state.recording_last_change_ms = millis();
      out_result.completed = true;
      out_result.completion = CompletionStatus::CompletedOk;
      out_result.ok = true;
      out_result.completion_code = (uint32_t)ControlCode::Ok;
      out_result.code = out_result.completion_code;
      out_result.has_log_status = true;
      out_result.log_status = currentLogStatusPayload(millis());
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::RecordStatus: {
      g_state.recording_last_command = request.command_id;
      out_result.completed = true;
      out_result.completion = CompletionStatus::CompletedOk;
      out_result.ok = true;
      out_result.completion_code = (uint32_t)ControlCode::Ok;
      out_result.code = out_result.completion_code;
      out_result.has_log_status = true;
      out_result.log_status = currentLogStatusPayload(millis());
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::ReplayStartLatest: {
      const bool ok = ((replay_bridge::status().flags & telem::kReplayStatusFlagFileOpen) != 0U)
                          ? replay_bridge::resume()
                          : replay_bridge::startLatest();
      out_result.completed = true;
      out_result.completion = ok ? CompletionStatus::CompletedOk : CompletionStatus::CompletedError;
      out_result.ok = ok;
      out_result.completion_code = ok ? (uint32_t)ControlCode::Ok : (uint32_t)ControlCode::BackendFailed;
      out_result.code = out_result.completion_code;
      out_result.has_replay_status = true;
      out_result.replay_status = currentReplayPayload();
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::ReplayStartFile: {
      const bool ok = replay_bridge::startFile(String(request.name));
      out_result.completed = true;
      out_result.completion = ok ? CompletionStatus::CompletedOk : CompletionStatus::CompletedError;
      out_result.ok = ok;
      out_result.completion_code = ok ? (uint32_t)ControlCode::Ok : (uint32_t)ControlCode::BackendFailed;
      out_result.code = out_result.completion_code;
      out_result.has_replay_status = true;
      out_result.replay_status = currentReplayPayload();
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::ReplayStop: {
      replay_bridge::stop();
      out_result.completed = true;
      out_result.completion = CompletionStatus::CompletedOk;
      out_result.ok = true;
      out_result.completion_code = (uint32_t)ControlCode::Ok;
      out_result.code = out_result.completion_code;
      out_result.has_replay_status = true;
      out_result.replay_status = currentReplayPayload();
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::ReplayPause: {
      const bool ok = replay_bridge::pause();
      out_result.completed = true;
      out_result.completion = ok ? CompletionStatus::CompletedOk : CompletionStatus::CompletedError;
      out_result.ok = ok;
      out_result.completion_code = ok ? (uint32_t)ControlCode::Ok : (uint32_t)ControlCode::BackendFailed;
      out_result.code = out_result.completion_code;
      out_result.has_replay_status = true;
      out_result.replay_status = currentReplayPayload();
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::ReplaySeekRelative: {
      const bool ok = replay_bridge::seekRelative(request.delta_records);
      out_result.completed = true;
      out_result.completion = ok ? CompletionStatus::CompletedOk : CompletionStatus::CompletedError;
      out_result.ok = ok;
      out_result.completion_code = ok ? (uint32_t)ControlCode::Ok : (uint32_t)ControlCode::BackendFailed;
      out_result.code = out_result.completion_code;
      out_result.has_replay_status = true;
      out_result.replay_status = currentReplayPayload();
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::ReplayStatus: {
      out_result.completed = true;
      out_result.completion = CompletionStatus::CompletedOk;
      out_result.ok = true;
      out_result.completion_code = (uint32_t)ControlCode::Ok;
      out_result.code = out_result.completion_code;
      out_result.has_replay_status = true;
      out_result.replay_status = currentReplayPayload();
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::FileListPage: {
      const uint16_t limit = request.limit == 0U ? 32U : request.limit;
      g_state.file_list_busy = true;
      const telem::SdApiStatusCode code =
          sd_file_api::getOrRefreshFileListPage(request.offset, limit, g_last_file_list_page, request.offset == 0U);
      g_state.file_list_busy = false;
      out_result.completed = true;
      out_result.completion = (code == telem::SdApiStatusCode::OK || code == telem::SdApiStatusCode::NO_FILES)
                                  ? CompletionStatus::CompletedOk
                                  : CompletionStatus::CompletedError;
      out_result.ok = (code == telem::SdApiStatusCode::OK || code == telem::SdApiStatusCode::NO_FILES);
      out_result.completion_code = mapSdCode(code);
      out_result.code = out_result.completion_code;
      out_result.payload_ref = g_last_file_list_page.handle.generation;
      out_result.has_file_list_page = out_result.ok;
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::FileListJson: {
      out_result.completed = true;
      out_result.completion = CompletionStatus::CompletedOk;
      out_result.ok = true;
      out_result.completion_code = (uint32_t)ControlCode::Ok;
      out_result.code = out_result.completion_code;
      out_result.has_files_json = true;
      out_result.files_json = sd_file_api::filesJson(request.sort_key, request.sort_dir);
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::StorageStatus: {
      out_result.completed = true;
      out_result.completion = CompletionStatus::CompletedOk;
      out_result.ok = true;
      out_result.completion_code = (uint32_t)ControlCode::Ok;
      out_result.code = out_result.completion_code;
      out_result.has_storage_status = true;
      out_result.storage_status = currentStoragePayload();
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::MountMedia: {
      telem::StorageStatusPayloadV1 storage = {};
      const telem::SdApiStatusCode code = sd_file_api::mountMedia(storage);
      out_result.completed = true;
      out_result.completion = (code == telem::SdApiStatusCode::OK) ? CompletionStatus::CompletedOk
                                                                    : CompletionStatus::CompletedError;
      out_result.ok = (code == telem::SdApiStatusCode::OK);
      out_result.completion_code = mapSdCode(code);
      out_result.code = out_result.completion_code;
      out_result.has_storage_status = true;
      out_result.storage_status = storage;
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::EjectMedia: {
      telem::StorageStatusPayloadV1 storage = {};
      const telem::SdApiStatusCode code = sd_file_api::ejectMedia(storage);
      out_result.completed = true;
      out_result.completion = (code == telem::SdApiStatusCode::OK) ? CompletionStatus::CompletedOk
                                                                    : CompletionStatus::CompletedError;
      out_result.ok = (code == telem::SdApiStatusCode::OK);
      out_result.completion_code = mapSdCode(code);
      out_result.code = out_result.completion_code;
      out_result.has_storage_status = true;
      out_result.storage_status = storage;
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::DeleteFile: {
      const telem::SdApiStatusCode code = sd_file_api::deleteFile(String(request.name));
      out_result.completed = true;
      out_result.completion = (code == telem::SdApiStatusCode::OK) ? CompletionStatus::CompletedOk
                                                                    : CompletionStatus::CompletedError;
      out_result.ok = (code == telem::SdApiStatusCode::OK);
      out_result.completion_code = mapSdCode(code);
      out_result.code = out_result.completion_code;
      out_result.has_storage_status = true;
      out_result.storage_status = currentStoragePayload();
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::RenameFile: {
      const telem::SdApiStatusCode code = sd_file_api::renameFile(String(request.name), String(request.aux_name));
      out_result.completed = true;
      out_result.completion = (code == telem::SdApiStatusCode::OK) ? CompletionStatus::CompletedOk
                                                                    : CompletionStatus::CompletedError;
      out_result.ok = (code == telem::SdApiStatusCode::OK);
      out_result.completion_code = mapSdCode(code);
      out_result.code = out_result.completion_code;
      out_result.has_storage_status = true;
      out_result.storage_status = currentStoragePayload();
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::ExportCsv: {
      const telem::SdApiStatusCode code = sd_file_api::exportLogCsv(String(request.name), request.io_stream);
      out_result.completed = true;
      out_result.completion = (code == telem::SdApiStatusCode::OK) ? CompletionStatus::CompletedOk
                                                                    : CompletionStatus::CompletedError;
      out_result.ok = (code == telem::SdApiStatusCode::OK);
      out_result.completion_code = mapSdCode(code);
      out_result.code = out_result.completion_code;
      out_result.has_storage_status = true;
      out_result.storage_status = currentStoragePayload();
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::SetRecordPrefix: {
      AppConfig cfg = config_store::get();
      strlcpy(cfg.record_prefix, request.prefix, sizeof(cfg.record_prefix));
      config_store::update(cfg);
      log_store::setConfig(cfg);
      out_result.completed = true;
      out_result.completion = CompletionStatus::CompletedOk;
      out_result.ok = true;
      out_result.completion_code = (uint32_t)ControlCode::Ok;
      out_result.code = out_result.completion_code;
      out_result.has_storage_status = true;
      out_result.storage_status = currentStoragePayload();
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::FusionSet: {
      const bool ok = teensy_link::sendSetFusionSettings(request.fusion);
      if (ok && request.command_id != 0U &&
          (request.source == SourceInterface::Radio || request.source == SourceInterface::WebForwarded)) {
        log_store::enqueueReplayControl(request.command_id,
                                        request.request_id,
                                        micros(),
                                        &request.fusion,
                                        sizeof(request.fusion),
                                        telem::kReplayControlFlagSourceGui | telem::kReplayControlFlagSourceRadio);
      }
      out_result.completed = true;
      out_result.completion = ok ? CompletionStatus::CompletedOk : CompletionStatus::CompletedError;
      out_result.ok = ok;
      out_result.completion_code = ok ? (uint32_t)ControlCode::Ok : (uint32_t)ControlCode::BackendFailed;
      out_result.code = out_result.completion_code;
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::FusionGet: {
      const bool ok = teensy_link::sendGetFusionSettings();
      out_result.completed = true;
      out_result.completion = ok ? CompletionStatus::CompletedOk : CompletionStatus::CompletedError;
      out_result.ok = ok;
      out_result.completion_code = ok ? (uint32_t)ControlCode::Ok : (uint32_t)ControlCode::BackendFailed;
      out_result.code = out_result.completion_code;
      const teensy_link::Snapshot snap = teensy_link::snapshot();
      out_result.has_fusion_settings = snap.has_fusion_settings;
      if (snap.has_fusion_settings) out_result.fusion_settings = snap.fusion_settings;
      recordCompletion(request, out_result.completion, out_result.completion_code);
      return true;
    }
    case RequestAction::None:
    default:
      out_result.disposition = DispositionStatus::Rejected;
      out_result.ok = false;
      out_result.disposition_code = (uint32_t)ControlCode::InvalidArgument;
      out_result.code = out_result.disposition_code;
      recordDisposition(request, out_result.disposition, out_result.disposition_code);
      return false;
  }
}

void fillStateSnapshot(uint32_t now_ms,
                       StateSnapshot& out,
                       const telem::StorageStatusPayloadV1* storage_hint) {
  out = {};
  out.system_mode = SystemMode::Idle;
  out.request_pending = g_state.request_pending;
  out.pending_request_id = g_state.pending_request_id;
  out.pending_source = g_state.pending_source;
  out.pending_category = g_state.pending_category;
  out.pending_action = g_state.pending_action;
  out.last_request_id = g_state.last_request_id;
  out.last_completed_request_id = g_state.last_completed_request_id;
  out.last_result_code = g_state.last_result_code;
  out.last_command_id = g_state.last_command_id;
  out.last_source = g_state.last_source;
  out.last_category = g_state.last_category;
  out.last_action = g_state.last_action;
  out.last_ok = g_state.last_ok;
  out.recording_requested = g_state.recording_requested;
  out.recording_session_id = g_state.recording_session_id;
  out.recording_last_command = g_state.recording_last_command;
  out.recording_last_change_ms = g_state.recording_last_change_ms;

  const log_store::RecorderStatus recorder = log_store::recorderStatus();
  out.recording_state = recorder.active ? ActivityState::Active : (!recorder.active && log_store::busy())
                                                                   ? ActivityState::Busy
                                                                   : ActivityState::Idle;

  const replay_bridge::Status replay = replay_bridge::status();
  const bool replay_active = (replay.flags & telem::kReplayStatusFlagActive) != 0U;
  out.replay_state = replay_active ? ActivityState::Active : ActivityState::Idle;
  out.replay_paused = (replay.flags & telem::kReplayStatusFlagPaused) != 0U;
  out.replay_file_open = (replay.flags & telem::kReplayStatusFlagFileOpen) != 0U;
  out.replay_last_command = replay.last_command;
  strlcpy(out.selected_file, replay.current_file, sizeof(out.selected_file));

  out.file_list_generation = sd_file_api::currentFileListHandle().generation;
  out.file_list_valid = sd_file_api::currentFileListHandle().valid();
  out.file_list_state = g_state.file_list_busy ? FileListState::Busy
                                               : (out.file_list_valid ? FileListState::Valid : FileListState::Idle);

  telem::StorageStatusPayloadV1 storage = {};
  if (storage_hint) {
    storage = *storage_hint;
  } else {
    (void)sd_file_api::getStorageStatus(storage);
  }
  out.sd_ready = (storage.flags & telem::kStorageStatusFlagBackendReady) != 0U;
  out.sd_mounted = (storage.flags & telem::kStorageStatusFlagMounted) != 0U;
  out.sd_media_present = (storage.flags & telem::kStorageStatusFlagMediaPresent) != 0U;

  const time_service::StatusSnapshot time_snap = time_service::snapshot(now_ms);
  out.time_state = time_snap.time_state;
  out.time_source = time_snap.time_source;
  out.time_flags = time_snap.time_flags;
  const teensy_link::Snapshot snap = teensy_link::snapshot();
  out.has_fusion_settings = snap.has_fusion_settings;
  if (snap.has_fusion_settings) out.fusion_settings = snap.fusion_settings;
  if (out.recording_state == ActivityState::Active) out.system_mode = SystemMode::Recording;
  else if (out.replay_state == ActivityState::Active) out.system_mode = SystemMode::Replay;
  else if (out.recording_state == ActivityState::Busy) out.system_mode = SystemMode::RecordingBusy;
  else if (out.replay_state == ActivityState::Busy) out.system_mode = SystemMode::ReplayBusy;
}

StateSnapshot stateSnapshot(uint32_t now_ms) {
  StateSnapshot out = {};
  fillStateSnapshot(now_ms, out);
  return out;
}

ResultSnapshot lastResultSnapshot() { return g_state.last_result; }

telem::LogStatusPayloadV1 currentLogStatusPayload(uint32_t now_ms) {
  const log_store::RecorderStatus recorder = log_store::recorderStatus();
  telem::LogStatusPayloadV1 status = {};
  if (recorder.active) status.flags |= telem::kLogStatusFlagActive;
  if (g_state.recording_requested) status.flags |= telem::kLogStatusFlagRequested;
  if (recorder.backend_ready) status.flags |= telem::kLogStatusFlagBackendReady;
  if (recorder.media_present) status.flags |= telem::kLogStatusFlagMediaPresent;
  if (!recorder.active && log_store::busy()) status.flags |= telem::kLogStatusFlagBusy;
  status.last_command = g_state.recording_last_command;
  status.session_id = recorder.session_id ? recorder.session_id : g_state.recording_session_id;
  status.bytes_written = recorder.bytes_written;
  status.free_bytes = recorder.free_bytes;
  status.last_change_ms =
      g_state.recording_last_change_ms ? (uint32_t)(now_ms - g_state.recording_last_change_ms) : 0xFFFFFFFFUL;
  return status;
}

const sd_file_api::FileListPage& lastFileListPage() { return g_last_file_list_page; }

void noteRecorderFeatureEnabled(bool enabled, uint32_t now_ms) {
  if (enabled) return;
  g_state.recording_requested = false;
  g_state.recording_last_change_ms = now_ms;
}

}  // namespace control_plane
