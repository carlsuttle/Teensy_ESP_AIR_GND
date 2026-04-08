#include "serial_control.h"

#include <stdlib.h>
#include <string.h>

#include "control_plane.h"

namespace serial_control {
namespace {

control_plane::Request g_request_scratch = {};
control_plane::Result g_result_scratch = {};
control_plane::StateSnapshot g_state_scratch = {};

const char* sourceText(control_plane::SourceInterface source) {
  switch (source) {
    case control_plane::SourceInterface::SerialConsole: return "serial";
    case control_plane::SourceInterface::Radio: return "radio";
    case control_plane::SourceInterface::WebForwarded: return "web_forwarded";
    case control_plane::SourceInterface::Unknown:
    default:
      return "unknown";
  }
}

const char* categoryText(control_plane::RequestCategory category) {
  switch (category) {
    case control_plane::RequestCategory::Recording: return "recording";
    case control_plane::RequestCategory::Replay: return "replay";
    case control_plane::RequestCategory::FileSd: return "file";
    case control_plane::RequestCategory::Fusion: return "fusion";
    case control_plane::RequestCategory::Status: return "state";
    case control_plane::RequestCategory::None:
    default:
      return "none";
  }
}

const char* actionText(control_plane::RequestAction action) {
  switch (action) {
    case control_plane::RequestAction::StateGet: return "get";
    case control_plane::RequestAction::RecordStart: return "start";
    case control_plane::RequestAction::RecordStop: return "stop";
    case control_plane::RequestAction::RecordStatus: return "status";
    case control_plane::RequestAction::ReplayStartLatest: return "start_latest";
    case control_plane::RequestAction::ReplayStartFile: return "start_file";
    case control_plane::RequestAction::ReplayStop: return "stop";
    case control_plane::RequestAction::ReplayPause: return "pause";
    case control_plane::RequestAction::ReplaySeekRelative: return "seek_relative";
    case control_plane::RequestAction::ReplayStatus: return "status";
    case control_plane::RequestAction::FileListPage: return "list_page";
    case control_plane::RequestAction::StorageStatus: return "storage_status";
    case control_plane::RequestAction::MountMedia: return "mount_media";
    case control_plane::RequestAction::EjectMedia: return "eject_media";
    case control_plane::RequestAction::DeleteFile: return "delete";
    case control_plane::RequestAction::RenameFile: return "rename";
    case control_plane::RequestAction::ExportCsv: return "export_csv";
    case control_plane::RequestAction::SetRecordPrefix: return "set_record_prefix";
    case control_plane::RequestAction::FusionSet: return "set";
    case control_plane::RequestAction::FusionGet: return "get";
    case control_plane::RequestAction::FileListJson:
      return "list_json";
    case control_plane::RequestAction::None:
    default:
      return "none";
  }
}

const char* modeText(control_plane::SystemMode mode) {
  switch (mode) {
    case control_plane::SystemMode::Idle: return "idle";
    case control_plane::SystemMode::Recording: return "recording";
    case control_plane::SystemMode::Replay: return "replay";
    case control_plane::SystemMode::RecordingBusy: return "recording_busy";
    case control_plane::SystemMode::ReplayBusy: return "replay_busy";
    default: return "idle";
  }
}

const char* activityText(control_plane::ActivityState state) {
  switch (state) {
    case control_plane::ActivityState::Idle: return "idle";
    case control_plane::ActivityState::Active: return "active";
    case control_plane::ActivityState::Busy: return "busy";
    default: return "idle";
  }
}

const char* fileListStateText(control_plane::FileListState state) {
  switch (state) {
    case control_plane::FileListState::Idle: return "idle";
    case control_plane::FileListState::Busy: return "busy";
    case control_plane::FileListState::Valid: return "valid";
    default: return "idle";
  }
}

String trimLine(const char* line) {
  String out = line ? String(line) : String();
  out.trim();
  return out;
}

int findKeyValueStart(const String& json, const char* key) {
  const String pattern = String("\"") + key + "\"";
  const int key_pos = json.indexOf(pattern);
  if (key_pos < 0) return -1;
  int colon = json.indexOf(':', key_pos + pattern.length());
  if (colon < 0) return -1;
  colon++;
  while (colon < (int)json.length()) {
    const char c = json[(unsigned)colon];
    if (c != ' ' && c != '\t') break;
    colon++;
  }
  return colon;
}

bool parseJsonString(const String& json, const char* key, String& out) {
  const int start = findKeyValueStart(json, key);
  if (start < 0 || start >= (int)json.length() || json[(unsigned)start] != '"') return false;
  out = "";
  bool escape = false;
  for (int i = start + 1; i < (int)json.length(); ++i) {
    const char c = json[(unsigned)i];
    if (escape) {
      switch (c) {
        case '"':
        case '\\':
        case '/':
          out += c;
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        default:
          out += c;
          break;
      }
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') return true;
    out += c;
  }
  return false;
}

bool parseJsonNumberToken(const String& json, const char* key, String& out) {
  const int start = findKeyValueStart(json, key);
  if (start < 0 || start >= (int)json.length()) return false;
  int end = start;
  while (end < (int)json.length()) {
    const char c = json[(unsigned)end];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
      end++;
      continue;
    }
    break;
  }
  if (end == start) return false;
  out = json.substring(start, end);
  out.trim();
  return out.length() > 0;
}

bool parseJsonUInt32(const String& json, const char* key, uint32_t& out) {
  String token;
  if (!parseJsonNumberToken(json, key, token)) return false;
  char* end_ptr = nullptr;
  const unsigned long value = strtoul(token.c_str(), &end_ptr, 10);
  if (end_ptr == token.c_str() || *end_ptr != '\0') return false;
  out = (uint32_t)value;
  return true;
}

bool parseJsonUInt16(const String& json, const char* key, uint16_t& out) {
  uint32_t value = 0U;
  if (!parseJsonUInt32(json, key, value) || value > 0xFFFFU) return false;
  out = (uint16_t)value;
  return true;
}

bool parseJsonInt32(const String& json, const char* key, int32_t& out) {
  String token;
  if (!parseJsonNumberToken(json, key, token)) return false;
  char* end_ptr = nullptr;
  const long value = strtol(token.c_str(), &end_ptr, 10);
  if (end_ptr == token.c_str() || *end_ptr != '\0') return false;
  out = (int32_t)value;
  return true;
}

bool parseJsonFloat(const String& json, const char* key, float& out) {
  String token;
  if (!parseJsonNumberToken(json, key, token)) return false;
  char* end_ptr = nullptr;
  out = strtof(token.c_str(), &end_ptr);
  return !(end_ptr == token.c_str() || *end_ptr != '\0');
}

void writeJsonEscaped(Stream& io, const String& value) {
  io.print('"');
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    switch (c) {
      case '\\':
      case '"':
        io.print('\\');
        io.print(c);
        break;
      case '\n':
        io.print("\\n");
        break;
      case '\r':
        io.print("\\r");
        break;
      case '\t':
        io.print("\\t");
        break;
      default:
        io.print(c);
        break;
    }
  }
  io.print('"');
}

void writeFusionSettings(Stream& io, const telem::FusionSettingsV1& fusion) {
  io.print("\"fusion\":{");
  io.print("\"gain\":");
  io.print((double)fusion.gain, 6);
  io.print(",\"accelerationRejection\":");
  io.print((double)fusion.accelerationRejection, 6);
  io.print(",\"magneticRejection\":");
  io.print((double)fusion.magneticRejection, 6);
  io.print(",\"recoveryTriggerPeriod\":");
  io.print((unsigned)fusion.recoveryTriggerPeriod);
  io.print("}");
}

void writeStateJson(Stream& io, const control_plane::StateSnapshot& state) {
  io.print("\"state\":{");
  io.print("\"mode\":");
  writeJsonEscaped(io, modeText(state.system_mode));
  io.print(",\"request_pending\":");
  io.print(state.request_pending ? "true" : "false");
  io.print(",\"pending_request_id\":");
  io.print((unsigned long)state.pending_request_id);
  io.print(",\"pending_source\":");
  writeJsonEscaped(io, sourceText(state.pending_source));
  io.print(",\"pending_category\":");
  writeJsonEscaped(io, categoryText(state.pending_category));
  io.print(",\"pending_action\":");
  writeJsonEscaped(io, actionText(state.pending_action));
  io.print(",\"recording_active\":");
  io.print(state.recording_state == control_plane::ActivityState::Active ? "true" : "false");
  io.print(",\"recording_busy\":");
  io.print(state.recording_state == control_plane::ActivityState::Busy ? "true" : "false");
  io.print(",\"recording_state\":");
  writeJsonEscaped(io, activityText(state.recording_state));
  io.print(",\"recording_session_id\":");
  io.print((unsigned long)state.recording_session_id);
  io.print(",\"recording_last_change_ms\":");
  io.print((unsigned long)state.recording_last_change_ms);
  io.print(",\"replay_active\":");
  io.print(state.replay_state == control_plane::ActivityState::Active ? "true" : "false");
  io.print(",\"replay_busy\":");
  io.print(state.replay_state == control_plane::ActivityState::Busy ? "true" : "false");
  io.print(",\"replay_state\":");
  writeJsonEscaped(io, activityText(state.replay_state));
  io.print(",\"replay_paused\":");
  io.print(state.replay_paused ? "true" : "false");
  io.print(",\"replay_file_open\":");
  io.print(state.replay_file_open ? "true" : "false");
  io.print(",\"selected_file\":");
  writeJsonEscaped(io, state.selected_file);
  io.print(",\"sd_ready\":");
  io.print(state.sd_ready ? "true" : "false");
  io.print(",\"sd_mounted\":");
  io.print(state.sd_mounted ? "true" : "false");
  io.print(",\"sd_media_present\":");
  io.print(state.sd_media_present ? "true" : "false");
  io.print(",\"file_list_state\":");
  writeJsonEscaped(io, fileListStateText(state.file_list_state));
  io.print(",\"file_list_generation\":");
  io.print((unsigned long)state.file_list_generation);
  io.print(",\"file_list_valid\":");
  io.print(state.file_list_valid ? "true" : "false");
  io.print(",\"time_state\":");
  writeJsonEscaped(io, telem::timeStateText(state.time_state));
  io.print(",\"time_source\":");
  writeJsonEscaped(io, telem::timeSourceText(state.time_source));
  io.print(",\"time_flags\":");
  io.print((unsigned)state.time_flags);
  io.print(",\"last_request_id\":");
  io.print((unsigned long)state.last_request_id);
  io.print(",\"last_completed_request_id\":");
  io.print((unsigned long)state.last_completed_request_id);
  io.print(",\"last_result_code\":");
  writeJsonEscaped(io, control_plane::controlCodeText(state.last_result_code));
  io.print(",\"last_result_code_id\":");
  io.print((unsigned long)state.last_result_code);
  io.print(",\"last_source\":");
  writeJsonEscaped(io, sourceText(state.last_source));
  io.print(",\"last_category\":");
  writeJsonEscaped(io, categoryText(state.last_category));
  io.print(",\"last_action\":");
  writeJsonEscaped(io, actionText(state.last_action));
  io.print(",\"has_fusion_settings\":");
  io.print(state.has_fusion_settings ? "true" : "false");
  if (state.has_fusion_settings) {
    io.print(",");
    writeFusionSettings(io, state.fusion_settings);
  }
  io.print("}");
}

void writeStoragePayload(Stream& io, const telem::StorageStatusPayloadV1& storage) {
  io.print("\"storage\":{");
  io.print("\"backend_ready\":");
  io.print((storage.flags & telem::kStorageStatusFlagBackendReady) ? "true" : "false");
  io.print(",\"mounted\":");
  io.print((storage.flags & telem::kStorageStatusFlagMounted) ? "true" : "false");
  io.print(",\"media_present\":");
  io.print((storage.flags & telem::kStorageStatusFlagMediaPresent) ? "true" : "false");
  io.print(",\"busy\":");
  io.print((storage.flags & telem::kStorageStatusFlagBusy) ? "true" : "false");
  io.print(",\"total_bytes\":");
  io.print((unsigned long)storage.total_bytes);
  io.print(",\"free_bytes\":");
  io.print((unsigned long)storage.free_bytes);
  io.print(",\"file_count\":");
  io.print((unsigned long)storage.file_count);
  io.print(",\"init_hz\":");
  io.print((unsigned long)storage.init_hz);
  io.print(",\"time_state\":");
  writeJsonEscaped(io, telem::timeStateText(storage.time_state));
  io.print(",\"time_source\":");
  writeJsonEscaped(io, telem::timeSourceText(storage.time_source));
  io.print(",\"time_flags\":");
  io.print((unsigned)storage.time_flags);
  io.print("}");
}

void writeLogPayload(Stream& io, const telem::LogStatusPayloadV1& log_status) {
  io.print("\"recording\":{");
  io.print("\"active\":");
  io.print((log_status.flags & telem::kLogStatusFlagActive) ? "true" : "false");
  io.print(",\"requested\":");
  io.print((log_status.flags & telem::kLogStatusFlagRequested) ? "true" : "false");
  io.print(",\"backend_ready\":");
  io.print((log_status.flags & telem::kLogStatusFlagBackendReady) ? "true" : "false");
  io.print(",\"media_present\":");
  io.print((log_status.flags & telem::kLogStatusFlagMediaPresent) ? "true" : "false");
  io.print(",\"busy\":");
  io.print((log_status.flags & telem::kLogStatusFlagBusy) ? "true" : "false");
  io.print(",\"session_id\":");
  io.print((unsigned long)log_status.session_id);
  io.print(",\"bytes_written\":");
  io.print((unsigned long)log_status.bytes_written);
  io.print(",\"free_bytes\":");
  io.print((unsigned long)log_status.free_bytes);
  io.print(",\"last_change_ms\":");
  io.print((unsigned long)log_status.last_change_ms);
  io.print("}");
}

void writeReplayPayload(Stream& io, const telem::ReplayStatusPayloadV1& replay) {
  io.print("\"replay\":{");
  io.print("\"active\":");
  io.print((replay.flags & telem::kReplayStatusFlagActive) ? "true" : "false");
  io.print(",\"paused\":");
  io.print((replay.flags & telem::kReplayStatusFlagPaused) ? "true" : "false");
  io.print(",\"file_open\":");
  io.print((replay.flags & telem::kReplayStatusFlagFileOpen) ? "true" : "false");
  io.print(",\"at_eof\":");
  io.print((replay.flags & telem::kReplayStatusFlagAtEof) ? "true" : "false");
  io.print(",\"teensy_seen\":");
  io.print((replay.flags & telem::kReplayStatusFlagTeensyReplaySeen) ? "true" : "false");
  io.print(",\"session_id\":");
  io.print((unsigned long)replay.session_id);
  io.print(",\"records_sent\":");
  io.print((unsigned long)replay.records_sent);
  io.print(",\"records_total\":");
  io.print((unsigned long)replay.records_total);
  io.print(",\"last_error\":");
  io.print((unsigned long)replay.last_error);
  io.print(",\"last_command\":");
  io.print((unsigned)replay.last_command);
  io.print(",\"current_file\":");
  writeJsonEscaped(io, replay.current_file);
  io.print("}");
}

void writeFileListPayload(Stream& io, const sd_file_api::FileListPage& page) {
  io.print("\"file_list\":{");
  io.print("\"generation\":");
  io.print((unsigned long)page.handle.generation);
  io.print(",\"offset\":");
  io.print((unsigned)page.offset);
  io.print(",\"limit\":");
  io.print((unsigned)page.limit);
  io.print(",\"total_files\":");
  io.print((unsigned)page.total_files);
  io.print(",\"returned_files\":");
  io.print((unsigned)page.returned_files);
  io.print(",\"has_more\":");
  io.print(page.has_more ? "true" : "false");
  io.print(",\"entries\":[");
  for (uint16_t i = 0; i < page.returned_files; ++i) {
    if (i != 0U) io.print(",");
    io.print("{\"name\":");
    writeJsonEscaped(io, page.entries[i].name);
    io.print(",\"size_bytes\":");
    io.print((unsigned long)page.entries[i].size_bytes);
    io.print(",\"mtime_utc_s\":");
    io.print((unsigned long)page.entries[i].mtime_utc_s);
    io.print("}");
  }
  io.print("]}");
}

void writeResultEnvelopeStart(Stream& io,
                              const control_plane::Request& request,
                              uint32_t req_id,
                              const char* phase,
                              const char* status,
                              const char* code_text,
                              uint32_t code) {
  io.print("{\"type\":\"control_result\"");
  io.print(",\"phase\":");
  writeJsonEscaped(io, phase);
  io.print(",\"req_id\":");
  io.print((unsigned long)req_id);
  io.print(",\"category\":");
  writeJsonEscaped(io, categoryText(request.category));
  io.print(",\"action\":");
  writeJsonEscaped(io, actionText(request.action));
  io.print(",\"status\":");
  writeJsonEscaped(io, status);
  io.print(",\"code\":");
  writeJsonEscaped(io, code_text);
  io.print(",\"code_id\":");
  io.print((unsigned long)code);
}

void emitImmediate(Stream& io, const control_plane::Request& request, const control_plane::Result& result) {
  writeResultEnvelopeStart(io,
                           request,
                           result.request_id,
                           "immediate",
                           control_plane::dispositionText(result.disposition),
                           control_plane::controlCodeText(result.disposition_code),
                           result.disposition_code);
  io.println("}");
}

void emitFinal(Stream& io, const control_plane::Request& request, const control_plane::Result& result) {
  const telem::StorageStatusPayloadV1* storage_hint = result.has_storage_status ? &result.storage_status : nullptr;
  control_plane::fillStateSnapshot(millis(), g_state_scratch, storage_hint);
  const control_plane::StateSnapshot& state = g_state_scratch;
  writeResultEnvelopeStart(io,
                           request,
                           result.request_id,
                           "final",
                           control_plane::completionText(result.completion),
                           control_plane::controlCodeText(result.completion_code),
                           result.completion_code);
  io.print(",\"ok\":");
  io.print(result.ok ? "true" : "false");
  io.print(",");
  writeStateJson(io, state);
  if (result.has_storage_status) {
    io.print(",");
    writeStoragePayload(io, result.storage_status);
  }
  if (result.has_log_status) {
    io.print(",");
    writeLogPayload(io, result.log_status);
  }
  if (result.has_replay_status) {
    io.print(",");
    writeReplayPayload(io, result.replay_status);
  }
  if (result.has_file_list_page) {
    io.print(",");
    writeFileListPayload(io, control_plane::lastFileListPage());
  }
  if (result.has_fusion_settings) {
    io.print(",\"has_fusion_settings\":true,");
    writeFusionSettings(io, result.fusion_settings);
  } else {
    io.print(",\"has_fusion_settings\":");
    io.print(state.has_fusion_settings ? "true" : "false");
  }
  io.println("}");
}

void emitRejectedParse(Stream& io, uint32_t req_id, const char* detail) {
  io.print("{\"type\":\"control_result\",\"phase\":\"immediate\",\"req_id\":");
  io.print((unsigned long)req_id);
  io.print(",\"category\":\"unknown\",\"action\":\"unknown\",\"status\":\"rejected\",\"code\":\"invalid_argument\",\"code_id\":");
  io.print((unsigned long)control_plane::ControlCode::InvalidArgument);
  if (detail && detail[0] != '\0') {
    io.print(",\"detail\":");
    writeJsonEscaped(io, detail);
  }
  io.println("}");
}

bool buildRequest(const String& json, control_plane::Request& request, uint32_t& req_id, String& error) {
  request = {};
  req_id = 0U;
  if (!parseJsonUInt32(json, "req_id", req_id) || req_id == 0U) {
    error = "missing_or_invalid_req_id";
    return false;
  }
  request.request_id = req_id;
  request.source = control_plane::SourceInterface::SerialConsole;

  String category;
  String action;
  if (!parseJsonString(json, "category", category) || !parseJsonString(json, "action", action)) {
    error = "missing_category_or_action";
    return false;
  }

  if (category == "state") {
    request.category = control_plane::RequestCategory::Status;
    if (action == "get") {
      request.action = control_plane::RequestAction::StateGet;
      return true;
    }
  } else if (category == "recording") {
    request.category = control_plane::RequestCategory::Recording;
    if (action == "start") {
      request.action = control_plane::RequestAction::RecordStart;
      if (parseJsonUInt32(json, "session_id", request.session_id) && request.session_id != 0U) {
        request.has_explicit_session_id = true;
      }
      return true;
    }
    if (action == "stop") {
      request.action = control_plane::RequestAction::RecordStop;
      return true;
    }
    if (action == "status") {
      request.action = control_plane::RequestAction::RecordStatus;
      return true;
    }
  } else if (category == "replay") {
    request.category = control_plane::RequestCategory::Replay;
    if (action == "start" || action == "start_latest") {
      String name;
      if (parseJsonString(json, "name", name) && name.length() > 0U) {
        request.action = control_plane::RequestAction::ReplayStartFile;
        strlcpy(request.name, name.c_str(), sizeof(request.name));
      } else {
        request.action = control_plane::RequestAction::ReplayStartLatest;
      }
      return true;
    }
    if (action == "start_file") {
      String name;
      if (!parseJsonString(json, "name", name) || name.length() == 0U) {
        error = "missing_name";
        return false;
      }
      request.action = control_plane::RequestAction::ReplayStartFile;
      strlcpy(request.name, name.c_str(), sizeof(request.name));
      return true;
    }
    if (action == "stop") {
      request.action = control_plane::RequestAction::ReplayStop;
      return true;
    }
    if (action == "status") {
      request.action = control_plane::RequestAction::ReplayStatus;
      return true;
    }
  } else if (category == "fusion") {
    request.category = control_plane::RequestCategory::Fusion;
    if (action == "get") {
      request.action = control_plane::RequestAction::FusionGet;
      return true;
    }
    if (action == "set") {
      request.action = control_plane::RequestAction::FusionSet;
      if (!parseJsonFloat(json, "gain", request.fusion.gain) ||
          !parseJsonFloat(json, "accelerationRejection", request.fusion.accelerationRejection) ||
          !parseJsonFloat(json, "magneticRejection", request.fusion.magneticRejection) ||
          !parseJsonUInt16(json, "recoveryTriggerPeriod", request.fusion.recoveryTriggerPeriod)) {
        error = "missing_fusion_fields";
        return false;
      }
      request.fusion.reserved = 0U;
      return true;
    }
  } else if (category == "file") {
    request.category = control_plane::RequestCategory::FileSd;
    if (action == "storage_status" || action == "status") {
      request.action = control_plane::RequestAction::StorageStatus;
      return true;
    }
    if (action == "list_page" || action == "list") {
      request.action = control_plane::RequestAction::FileListPage;
      (void)parseJsonUInt16(json, "offset", request.offset);
      (void)parseJsonUInt16(json, "limit", request.limit);
      return true;
    }
  }

  error = "unsupported_category_or_action";
  return false;
}

}  // namespace

bool handleLine(const char* line, Stream& io) {
  const String trimmed = trimLine(line);
  if (trimmed.length() == 0U || trimmed[0] != '{') return false;

  uint32_t req_id = 0U;
  String error;
  if (!buildRequest(trimmed, g_request_scratch, req_id, error)) {
    emitRejectedParse(io, req_id, error.c_str());
    return true;
  }

  g_result_scratch = {};
  (void)control_plane::submit(g_request_scratch, g_result_scratch);
  emitImmediate(io, g_request_scratch, g_result_scratch);
  if (g_result_scratch.disposition == control_plane::DispositionStatus::Accepted) {
    emitFinal(io, g_request_scratch, g_result_scratch);
  }
  return true;
}

}  // namespace serial_control
