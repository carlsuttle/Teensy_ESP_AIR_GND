#pragma once

#include <Arduino.h>

#include "log_store.h"
#include "types_shared.h"

namespace sd_file_api {

enum class Operation : uint8_t {
  List = 0,
  RefreshList,
  FileInfo,
  Delete,
  Rename,
  ExportCsv,
  StorageStatus,
  MountMedia,
  EjectMedia,
};

struct FileListHandle {
  uint32_t generation = 0U;

  bool valid() const { return generation != 0U; }
};

struct FileListPage {
  FileListHandle handle = {};
  uint16_t offset = 0U;
  uint16_t limit = 0U;
  uint16_t total_files = 0U;
  uint16_t returned_files = 0U;
  bool has_more = false;
  telem::LogFileInfoV1 entries[32] = {};
};

struct FileInfo {
  telem::LogFileInfoV1 entry = {};
};

void begin();
void invalidateFileList();
FileListHandle currentFileListHandle();

telem::SdApiStatusCode refreshFileList(FileListHandle& out_handle,
                                       log_store::FileSortKey sort_key = log_store::FileSortKey::date,
                                       log_store::FileSortDirection sort_dir =
                                           log_store::FileSortDirection::descending);
telem::SdApiStatusCode getFileListPage(FileListHandle handle,
                                       uint16_t offset,
                                       uint16_t limit,
                                       FileListPage& out_page);
telem::SdApiStatusCode getOrRefreshFileListPage(uint16_t offset,
                                                uint16_t limit,
                                                FileListPage& out_page,
                                                bool force_refresh,
                                                log_store::FileSortKey sort_key = log_store::FileSortKey::date,
                                                log_store::FileSortDirection sort_dir =
                                                    log_store::FileSortDirection::descending);
telem::SdApiStatusCode getFileInfo(const String& name, FileInfo& out_info);
telem::SdApiStatusCode deleteFile(const String& name);
telem::SdApiStatusCode renameFile(const String& src_name, const String& dst_name);
telem::SdApiStatusCode exportLogCsv(const String& name, Stream* out = nullptr);
telem::SdApiStatusCode getStorageStatus(telem::StorageStatusPayloadV1& out_payload);
telem::SdApiStatusCode mountMedia(telem::StorageStatusPayloadV1& out_payload);
telem::SdApiStatusCode ejectMedia(telem::StorageStatusPayloadV1& out_payload);
String filesJson(log_store::FileSortKey sort_key = log_store::FileSortKey::date,
                 log_store::FileSortDirection sort_dir = log_store::FileSortDirection::descending);
const char* operationText(Operation op);

}  // namespace sd_file_api
