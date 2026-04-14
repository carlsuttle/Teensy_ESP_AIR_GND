#include "sd_file_api.h"

#include <string.h>

#include "config_store.h"
#include "replay_bridge.h"
#include "sd_backend.h"
#include "time_service.h"

namespace sd_file_api {
namespace {

constexpr uint16_t kFilesJsonMaxEntries = 20U;

telem::LogFileInfoV1* g_snapshot_files = nullptr;
uint16_t g_snapshot_total_files = 0U;
uint32_t g_snapshot_generation = 0U;
bool g_snapshot_valid = false;
log_store::FileSortKey g_snapshot_sort_key = log_store::FileSortKey::date;
log_store::FileSortDirection g_snapshot_sort_dir = log_store::FileSortDirection::descending;

bool replayFileOpen() {
  return (replay_bridge::status().flags & telem::kReplayStatusFlagFileOpen) != 0U;
}

telem::SdApiStatusCode checkModeAllowed(Operation op) {
  if (op == Operation::StorageStatus || op == Operation::MountMedia) {
    return telem::SdApiStatusCode::OK;
  }

  const log_store::RecorderStatus recorder = log_store::recorderStatus();
  if (recorder.active || log_store::busy()) {
    return telem::SdApiStatusCode::BUSY_RECORDING;
  }
  if (replayFileOpen()) {
    return telem::SdApiStatusCode::BUSY_REPLAY;
  }
  return telem::SdApiStatusCode::OK;
}

telem::SdApiStatusCode ensureSdReady() {
  log_store::probeBackend();
  const log_store::RecorderStatus recorder = log_store::recorderStatus();
  if (!recorder.backend_ready || !recorder.media_present || !sd_backend::mounted()) {
    return telem::SdApiStatusCode::SD_NOT_READY;
  }
  return telem::SdApiStatusCode::OK;
}

void releaseSnapshot() {
  if (g_snapshot_files) {
    log_store::freeFilesSnapshot(g_snapshot_files);
    g_snapshot_files = nullptr;
  }
  g_snapshot_total_files = 0U;
  g_snapshot_valid = false;
}

void fillStoragePayload(telem::StorageStatusPayloadV1& out_payload) {
  memset(&out_payload, 0, sizeof(out_payload));
  const log_store::RecorderStatus recorder = log_store::recorderStatus();
  sd_backend::Status backend = {};
  const bool mounted = sd_backend::mounted();
  const bool backend_ok = mounted ? sd_backend::refreshStatus(backend) : false;

  out_payload.media_state = static_cast<uint8_t>(sd_backend::mediaState());
  out_payload.init_hz = backend_ok ? backend.init_hz : sd_backend::mountedFrequencyHz();
  out_payload.free_bytes = recorder.free_bytes;
  if (backend_ok) {
    const uint64_t total_bytes = backend.total_bytes ? backend.total_bytes : backend.card_size_bytes;
    out_payload.total_bytes = (total_bytes > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)total_bytes;
  }

  uint16_t total_files = 0U;
  // Storage status only needs a count, so avoid the sorted listing path here.
  if (log_store::enumerateManagedLogFiles(nullptr, nullptr, total_files)) {
    out_payload.file_count = total_files;
  }

  uint8_t flags = 0U;
  if (mounted) flags |= telem::kStorageStatusFlagMounted;
  if (recorder.backend_ready) flags |= telem::kStorageStatusFlagBackendReady;
  if (recorder.media_present) flags |= telem::kStorageStatusFlagMediaPresent;
  if (log_store::busy()) flags |= telem::kStorageStatusFlagBusy;
  out_payload.flags = flags;

  const String prefix = log_store::recordPrefix();
  const String next_name = log_store::previewLogName();
  strlcpy(out_payload.record_prefix, prefix.c_str(), sizeof(out_payload.record_prefix));
  strlcpy(out_payload.next_record_name, next_name.c_str(), sizeof(out_payload.next_record_name));
  time_service::fillStorageStatus(out_payload, millis());
}

telem::SdApiStatusCode buildSnapshot(FileListHandle& out_handle,
                                     log_store::FileSortKey sort_key,
                                     log_store::FileSortDirection sort_dir) {
  const telem::SdApiStatusCode mode_code = checkModeAllowed(Operation::RefreshList);
  if (mode_code != telem::SdApiStatusCode::OK) return mode_code;
  const telem::SdApiStatusCode ready_code = ensureSdReady();
  if (ready_code != telem::SdApiStatusCode::OK) return ready_code;

  telem::LogFileInfoV1* files = nullptr;
  uint16_t total_files = 0U;
  if (!log_store::listFilesSnapshot(files, total_files, sort_key, sort_dir)) {
    return telem::SdApiStatusCode::IO_ERROR;
  }

  releaseSnapshot();
  g_snapshot_files = files;
  g_snapshot_total_files = total_files;
  g_snapshot_sort_key = sort_key;
  g_snapshot_sort_dir = sort_dir;
  ++g_snapshot_generation;
  if (g_snapshot_generation == 0U) ++g_snapshot_generation;
  g_snapshot_valid = true;
  out_handle.generation = g_snapshot_generation;
  return total_files == 0U ? telem::SdApiStatusCode::NO_FILES : telem::SdApiStatusCode::OK;
}

telem::SdApiStatusCode ensureSnapshotForPage(uint16_t offset,
                                             uint16_t limit,
                                             FileListPage& out_page,
                                             bool force_refresh,
                                             log_store::FileSortKey sort_key = log_store::FileSortKey::date,
                                             log_store::FileSortDirection sort_dir = log_store::FileSortDirection::descending) {
  FileListHandle handle = currentFileListHandle();
  if (force_refresh || !handle.valid()) {
    const telem::SdApiStatusCode refresh_code = refreshFileList(handle, sort_key, sort_dir);
    if (refresh_code != telem::SdApiStatusCode::OK && refresh_code != telem::SdApiStatusCode::NO_FILES) {
      return refresh_code;
    }
  }
  return getFileListPage(handle, offset, limit, out_page);
}

}  // namespace

void begin() {
  invalidateFileList();
}

void invalidateFileList() {
  releaseSnapshot();
  ++g_snapshot_generation;
  if (g_snapshot_generation == 0U) ++g_snapshot_generation;
}

FileListHandle currentFileListHandle() {
  FileListHandle handle = {};
  if (g_snapshot_valid) {
    handle.generation = g_snapshot_generation;
  }
  return handle;
}

telem::SdApiStatusCode refreshFileList(FileListHandle& out_handle,
                                       log_store::FileSortKey sort_key,
                                       log_store::FileSortDirection sort_dir) {
  return buildSnapshot(out_handle, sort_key, sort_dir);
}

telem::SdApiStatusCode getFileListPage(FileListHandle handle,
                                       uint16_t offset,
                                       uint16_t limit,
                                       FileListPage& out_page) {
  memset(&out_page, 0, sizeof(out_page));
  out_page.limit = constrain(limit, (uint16_t)1U, (uint16_t)32U);
  out_page.offset = offset;

  const telem::SdApiStatusCode mode_code = checkModeAllowed(Operation::List);
  if (mode_code != telem::SdApiStatusCode::OK) return mode_code;
  const telem::SdApiStatusCode ready_code = ensureSdReady();
  if (ready_code != telem::SdApiStatusCode::OK) return ready_code;

  if (!g_snapshot_valid || !handle.valid() || handle.generation != g_snapshot_generation ||
      (g_snapshot_total_files != 0U && !g_snapshot_files)) {
    return telem::SdApiStatusCode::INVALID_HANDLE;
  }

  out_page.handle = handle;
  out_page.total_files = g_snapshot_total_files;
  if (g_snapshot_total_files == 0U) {
    return telem::SdApiStatusCode::NO_FILES;
  }
  if (offset >= g_snapshot_total_files) {
    return telem::SdApiStatusCode::INVALID_ARGUMENT;
  }

  const uint16_t remaining = (uint16_t)(g_snapshot_total_files - offset);
  out_page.returned_files = (remaining < out_page.limit) ? remaining : out_page.limit;
  out_page.has_more = (offset + out_page.returned_files) < g_snapshot_total_files;
  for (uint16_t i = 0U; i < out_page.returned_files; ++i) {
    out_page.entries[i] = g_snapshot_files[offset + i];
  }
  return telem::SdApiStatusCode::OK;
}

telem::SdApiStatusCode getOrRefreshFileListPage(uint16_t offset,
                                                uint16_t limit,
                                                FileListPage& out_page,
                                                bool force_refresh,
                                                log_store::FileSortKey sort_key,
                                                log_store::FileSortDirection sort_dir) {
  telem::SdApiStatusCode code =
      ensureSnapshotForPage(offset, limit, out_page, force_refresh || offset == 0U, sort_key, sort_dir);
  if (code == telem::SdApiStatusCode::INVALID_HANDLE && offset != 0U) {
    code = ensureSnapshotForPage(offset, limit, out_page, true, sort_key, sort_dir);
  }
  return code;
}

telem::SdApiStatusCode getFileInfo(const String& name, FileInfo& out_info) {
  memset(&out_info, 0, sizeof(out_info));
  if (!log_store::isSafeName(name)) return telem::SdApiStatusCode::INVALID_ARGUMENT;

  FileListPage page = {};
  telem::SdApiStatusCode code = getOrRefreshFileListPage(
      0U, 32U, page, false, log_store::FileSortKey::date, log_store::FileSortDirection::descending);
  if (code != telem::SdApiStatusCode::OK && code != telem::SdApiStatusCode::NO_FILES) return code;
  if (code == telem::SdApiStatusCode::NO_FILES) return telem::SdApiStatusCode::NO_FILES;

  for (uint16_t offset = 0U; offset < page.total_files; offset = (uint16_t)(offset + page.returned_files)) {
    code = getFileListPage(currentFileListHandle(), offset, 32U, page);
    if (code != telem::SdApiStatusCode::OK) return code;
    for (uint16_t i = 0U; i < page.returned_files; ++i) {
      if (String(page.entries[i].name) == name) {
        out_info.entry = page.entries[i];
        return telem::SdApiStatusCode::OK;
      }
    }
    if (!page.has_more) break;
  }
  return telem::SdApiStatusCode::NOT_FOUND;
}

telem::SdApiStatusCode deleteFile(const String& name) {
  if (!log_store::isSafeName(name)) return telem::SdApiStatusCode::INVALID_ARGUMENT;
  const telem::SdApiStatusCode mode_code = checkModeAllowed(Operation::Delete);
  if (mode_code != telem::SdApiStatusCode::OK) return mode_code;
  const telem::SdApiStatusCode ready_code = ensureSdReady();
  if (ready_code != telem::SdApiStatusCode::OK) return ready_code;
  if (!log_store::deleteFileByName(name)) return telem::SdApiStatusCode::IO_ERROR;
  invalidateFileList();
  return telem::SdApiStatusCode::OK;
}

telem::SdApiStatusCode renameFile(const String& src_name, const String& dst_name) {
  if (!log_store::isSafeName(src_name) || !log_store::isSafeName(dst_name)) {
    return telem::SdApiStatusCode::INVALID_ARGUMENT;
  }
  const telem::SdApiStatusCode mode_code = checkModeAllowed(Operation::Rename);
  if (mode_code != telem::SdApiStatusCode::OK) return mode_code;
  const telem::SdApiStatusCode ready_code = ensureSdReady();
  if (ready_code != telem::SdApiStatusCode::OK) return ready_code;
  if (!log_store::renameFileByName(src_name, dst_name)) return telem::SdApiStatusCode::IO_ERROR;
  invalidateFileList();
  return telem::SdApiStatusCode::OK;
}

telem::SdApiStatusCode exportLogCsv(const String& name, Stream* out) {
  if (!log_store::isSafeName(name) || !name.endsWith(".tlog")) {
    return telem::SdApiStatusCode::INVALID_ARGUMENT;
  }
  const telem::SdApiStatusCode mode_code = checkModeAllowed(Operation::ExportCsv);
  if (mode_code != telem::SdApiStatusCode::OK) return mode_code;
  const telem::SdApiStatusCode ready_code = ensureSdReady();
  if (ready_code != telem::SdApiStatusCode::OK) return ready_code;
  if (!log_store::exportLogToCsvByName(name, out)) return telem::SdApiStatusCode::IO_ERROR;
  invalidateFileList();
  return telem::SdApiStatusCode::OK;
}

telem::SdApiStatusCode getStorageStatus(telem::StorageStatusPayloadV1& out_payload) {
  log_store::probeBackend();
  fillStoragePayload(out_payload);
  return sd_backend::mounted() ? telem::SdApiStatusCode::OK : telem::SdApiStatusCode::SD_NOT_READY;
}

telem::SdApiStatusCode mountMedia(telem::StorageStatusPayloadV1& out_payload) {
  sd_backend::Status status = {};
  if (!sd_backend::mount(&status)) {
    log_store::probeBackend();
    fillStoragePayload(out_payload);
    return telem::SdApiStatusCode::IO_ERROR;
  }
  log_store::probeBackend();
  invalidateFileList();
  fillStoragePayload(out_payload);
  return telem::SdApiStatusCode::OK;
}

telem::SdApiStatusCode ejectMedia(telem::StorageStatusPayloadV1& out_payload) {
  const telem::SdApiStatusCode mode_code = checkModeAllowed(Operation::EjectMedia);
  if (mode_code != telem::SdApiStatusCode::OK) {
    fillStoragePayload(out_payload);
    return mode_code;
  }
  if (!sd_backend::eject()) {
    log_store::probeBackend();
    fillStoragePayload(out_payload);
    return telem::SdApiStatusCode::IO_ERROR;
  }
  log_store::probeBackend();
  invalidateFileList();
  fillStoragePayload(out_payload);
  return telem::SdApiStatusCode::OK;
}

String filesJson(log_store::FileSortKey sort_key, log_store::FileSortDirection sort_dir) {
  FileListHandle handle = {};
  const telem::SdApiStatusCode refresh_code = refreshFileList(handle, sort_key, sort_dir);
  if (refresh_code != telem::SdApiStatusCode::OK && refresh_code != telem::SdApiStatusCode::NO_FILES) {
    return "{\"total_files\":0,\"returned_files\":0,\"truncated\":false,\"files\":[]}";
  }

  const uint16_t total_files = g_snapshot_total_files;
  const uint16_t target_count = (total_files < kFilesJsonMaxEntries) ? total_files : kFilesJsonMaxEntries;
  const bool truncated = total_files > target_count;

  String out = "{\"total_files\":";
  out += String(total_files);
  out += ",\"returned_files\":";
  out += String(target_count);
  out += ",\"truncated\":";
  out += truncated ? "true" : "false";
  out += ",\"files\":[";
  bool first = true;
  if (handle.valid() && g_snapshot_valid && g_snapshot_files != nullptr) {
    for (uint16_t i = 0U; i < target_count; ++i) {
      const telem::LogFileInfoV1& entry = g_snapshot_files[i];
      if (!first) out += ',';
      first = false;
      out += "{\"name\":\"";
      out += entry.name;
      out += "\",\"size_bytes\":";
      out += String(entry.size_bytes);
      out += ",\"mtime_utc_s\":";
      out += String(entry.mtime_utc_s);
      out += "}";
    }
  }
  out += "]}";
  return out;
}

const char* operationText(Operation op) {
  switch (op) {
    case Operation::List: return "list";
    case Operation::RefreshList: return "refresh_list";
    case Operation::FileInfo: return "file_info";
    case Operation::Delete: return "delete";
    case Operation::Rename: return "rename";
    case Operation::ExportCsv: return "export_csv";
    case Operation::StorageStatus: return "storage_status";
    case Operation::MountMedia: return "mount_media";
    case Operation::EjectMedia: return "eject_media";
    default: return "unknown";
  }
}

}  // namespace sd_file_api
