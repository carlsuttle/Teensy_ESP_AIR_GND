#include "sd_worker.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

namespace sd_worker {
namespace {

constexpr uint32_t kWorkerStackBytes = 12288U;
constexpr UBaseType_t kWorkerPriority = 2U;
constexpr BaseType_t kWorkerCore = 1;
constexpr uint8_t kQueueDepth = 8U;
constexpr uint8_t kMaxOpenHandles = 24U;

enum class Op : uint8_t {
  FsBegin,
  FsEnd,
  FsCardType,
  FsCardSize,
  FsTotalBytes,
  FsUsedBytes,
  FsOpen,
  FsExists,
  FsMkdir,
  FsRemove,
  FsRename,
  FileValid,
  FileIsDirectory,
  FileName,
  FileSize,
  FilePosition,
  FileLastWrite,
  FileSeek,
  FileRead,
  FileWrite,
  FileFlush,
  FileClose,
  FileOpenNext,
};

struct Command {
  Op op = Op::FsCardType;
  TaskHandle_t reply_task = nullptr;
  bool ok = false;
  uint8_t u8 = 0U;
  uint16_t handle = kInvalidFileHandle;
  uint16_t out_handle = kInvalidFileHandle;
  uint32_t u32 = 0U;
  uint64_t u64 = 0U;
  size_t size_value = 0U;
  time_t time_value = (time_t)0;
  SPIClass* spi = nullptr;
  const char* path = nullptr;
  const char* path2 = nullptr;
  const char* mode = nullptr;
  uint8_t* buffer = nullptr;
  const uint8_t* const_buffer = nullptr;
  size_t buffer_len = 0U;
  char* name_buffer = nullptr;
  size_t name_buffer_len = 0U;
};

QueueHandle_t g_queue = nullptr;
TaskHandle_t g_task = nullptr;
::File g_files[kMaxOpenHandles];
bool g_file_used[kMaxOpenHandles] = {};

bool onWorkerTask() {
  return g_task != nullptr && xTaskGetCurrentTaskHandle() == g_task;
}

uint16_t allocHandle(const ::File& file) {
  if (!file) return kInvalidFileHandle;
  for (uint16_t i = 0U; i < kMaxOpenHandles; ++i) {
    if (g_file_used[i]) continue;
    g_files[i] = file;
    g_file_used[i] = true;
    return i;
  }
  ::File tmp = file;
  tmp.close();
  return kInvalidFileHandle;
}

bool validHandle(uint16_t handle) {
  return handle < kMaxOpenHandles && g_file_used[handle] && g_files[handle];
}

void closeHandle(uint16_t handle) {
  if (!validHandle(handle)) return;
  g_files[handle].close();
  g_files[handle] = ::File();
  g_file_used[handle] = false;
}

void process(Command& cmd) {
  cmd.ok = false;
  switch (cmd.op) {
    case Op::FsBegin:
      cmd.ok = cmd.spi && SD.begin(cmd.u8, *cmd.spi, cmd.u32);
      break;
    case Op::FsEnd:
      for (uint16_t i = 0U; i < kMaxOpenHandles; ++i) {
        closeHandle(i);
      }
      SD.end();
      cmd.ok = true;
      break;
    case Op::FsCardType:
      cmd.u8 = SD.cardType();
      cmd.ok = true;
      break;
    case Op::FsCardSize:
      cmd.u64 = SD.cardSize();
      cmd.ok = true;
      break;
    case Op::FsTotalBytes:
      cmd.u64 = SD.totalBytes();
      cmd.ok = true;
      break;
    case Op::FsUsedBytes:
      cmd.u64 = SD.usedBytes();
      cmd.ok = true;
      break;
    case Op::FsOpen: {
      if (!cmd.path || !cmd.mode) break;
      const ::File file = SD.open(cmd.path, cmd.mode);
      cmd.out_handle = allocHandle(file);
      cmd.ok = cmd.out_handle != kInvalidFileHandle;
      break;
    }
    case Op::FsExists:
      cmd.ok = cmd.path && SD.exists(cmd.path);
      break;
    case Op::FsMkdir:
      cmd.ok = cmd.path && SD.mkdir(cmd.path);
      break;
    case Op::FsRemove:
      cmd.ok = cmd.path && SD.remove(cmd.path);
      break;
    case Op::FsRename:
      cmd.ok = cmd.path && cmd.path2 && SD.rename(cmd.path, cmd.path2);
      break;
    case Op::FileValid:
      cmd.ok = validHandle(cmd.handle);
      break;
    case Op::FileIsDirectory:
      cmd.ok = validHandle(cmd.handle) && g_files[cmd.handle].isDirectory();
      break;
    case Op::FileName:
      if (validHandle(cmd.handle) && cmd.name_buffer && cmd.name_buffer_len != 0U) {
        const char* name = g_files[cmd.handle].name();
        if (!name) name = "";
        strlcpy(cmd.name_buffer, name, cmd.name_buffer_len);
        cmd.ok = true;
      }
      break;
    case Op::FileSize:
      if (validHandle(cmd.handle)) {
        cmd.size_value = (size_t)g_files[cmd.handle].size();
        cmd.ok = true;
      }
      break;
    case Op::FilePosition:
      if (validHandle(cmd.handle)) {
        cmd.size_value = (size_t)g_files[cmd.handle].position();
        cmd.ok = true;
      }
      break;
    case Op::FileLastWrite:
      if (validHandle(cmd.handle)) {
        cmd.time_value = g_files[cmd.handle].getLastWrite();
        cmd.ok = true;
      }
      break;
    case Op::FileSeek:
      cmd.ok = validHandle(cmd.handle) && g_files[cmd.handle].seek(cmd.size_value);
      break;
    case Op::FileRead:
      if (validHandle(cmd.handle) && cmd.buffer && cmd.buffer_len != 0U) {
        cmd.size_value = g_files[cmd.handle].read(cmd.buffer, cmd.buffer_len);
        cmd.ok = true;
      }
      break;
    case Op::FileWrite:
      if (validHandle(cmd.handle) && cmd.const_buffer && cmd.buffer_len != 0U) {
        cmd.size_value = g_files[cmd.handle].write(cmd.const_buffer, cmd.buffer_len);
        cmd.ok = true;
      }
      break;
    case Op::FileFlush:
      if (validHandle(cmd.handle)) {
        g_files[cmd.handle].flush();
        cmd.ok = true;
      }
      break;
    case Op::FileClose:
      closeHandle(cmd.handle);
      cmd.ok = true;
      break;
    case Op::FileOpenNext:
      if (validHandle(cmd.handle)) {
        const ::File next = g_files[cmd.handle].openNextFile();
        cmd.out_handle = allocHandle(next);
        cmd.ok = cmd.out_handle != kInvalidFileHandle;
      }
      break;
  }
}

bool submit(Command& cmd) {
  begin();
  if (!g_queue) return false;
  if (onWorkerTask()) {
    process(cmd);
    return cmd.ok;
  }
  cmd.reply_task = xTaskGetCurrentTaskHandle();
  Command* queued = &cmd;
  if (xQueueSend(g_queue, &queued, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  return cmd.ok;
}

void workerTask(void* param) {
  (void)param;
  for (;;) {
    Command* cmd = nullptr;
    if (xQueueReceive(g_queue, &cmd, portMAX_DELAY) != pdTRUE || !cmd) continue;
    process(*cmd);
    xTaskNotifyGive(cmd->reply_task);
  }
}

}  // namespace

void begin() {
  if (g_task) return;
  if (!g_queue) {
    g_queue = xQueueCreate(kQueueDepth, sizeof(Command*));
  }
  if (!g_queue) return;
  xTaskCreatePinnedToCore(workerTask, "sd_worker", kWorkerStackBytes, nullptr, kWorkerPriority, &g_task,
                          kWorkerCore);
}

bool fsBegin(uint8_t cs_pin, SPIClass* spi, uint32_t hz) {
  Command cmd = {};
  cmd.op = Op::FsBegin;
  cmd.u8 = cs_pin;
  cmd.u32 = hz;
  cmd.spi = spi;
  return submit(cmd);
}

void fsEnd() {
  Command cmd = {};
  cmd.op = Op::FsEnd;
  (void)submit(cmd);
}

uint8_t fsCardType() {
  Command cmd = {};
  cmd.op = Op::FsCardType;
  (void)submit(cmd);
  return cmd.u8;
}

uint64_t fsCardSize() {
  Command cmd = {};
  cmd.op = Op::FsCardSize;
  (void)submit(cmd);
  return cmd.u64;
}

uint64_t fsTotalBytes() {
  Command cmd = {};
  cmd.op = Op::FsTotalBytes;
  (void)submit(cmd);
  return cmd.u64;
}

uint64_t fsUsedBytes() {
  Command cmd = {};
  cmd.op = Op::FsUsedBytes;
  (void)submit(cmd);
  return cmd.u64;
}

bool fsOpen(const char* path, const char* mode, uint16_t& out_handle) {
  Command cmd = {};
  cmd.op = Op::FsOpen;
  cmd.path = path;
  cmd.mode = mode;
  const bool ok = submit(cmd);
  out_handle = cmd.out_handle;
  return ok;
}

bool fsExists(const char* path) {
  Command cmd = {};
  cmd.op = Op::FsExists;
  cmd.path = path;
  return submit(cmd);
}

bool fsMkdir(const char* path) {
  Command cmd = {};
  cmd.op = Op::FsMkdir;
  cmd.path = path;
  return submit(cmd);
}

bool fsRemove(const char* path) {
  Command cmd = {};
  cmd.op = Op::FsRemove;
  cmd.path = path;
  return submit(cmd);
}

bool fsRename(const char* from, const char* to) {
  Command cmd = {};
  cmd.op = Op::FsRename;
  cmd.path = from;
  cmd.path2 = to;
  return submit(cmd);
}

bool fileValid(uint16_t handle) {
  Command cmd = {};
  cmd.op = Op::FileValid;
  cmd.handle = handle;
  return submit(cmd);
}

bool fileIsDirectory(uint16_t handle) {
  Command cmd = {};
  cmd.op = Op::FileIsDirectory;
  cmd.handle = handle;
  return submit(cmd);
}

bool fileName(uint16_t handle, char* out_name, size_t out_len) {
  Command cmd = {};
  cmd.op = Op::FileName;
  cmd.handle = handle;
  cmd.name_buffer = out_name;
  cmd.name_buffer_len = out_len;
  return submit(cmd);
}

size_t fileSize(uint16_t handle) {
  Command cmd = {};
  cmd.op = Op::FileSize;
  cmd.handle = handle;
  (void)submit(cmd);
  return cmd.size_value;
}

size_t filePosition(uint16_t handle) {
  Command cmd = {};
  cmd.op = Op::FilePosition;
  cmd.handle = handle;
  (void)submit(cmd);
  return cmd.size_value;
}

bool fileLastWrite(uint16_t handle, time_t& out_time) {
  Command cmd = {};
  cmd.op = Op::FileLastWrite;
  cmd.handle = handle;
  const bool ok = submit(cmd);
  out_time = cmd.time_value;
  return ok;
}

bool fileSeek(uint16_t handle, size_t pos) {
  Command cmd = {};
  cmd.op = Op::FileSeek;
  cmd.handle = handle;
  cmd.size_value = pos;
  return submit(cmd);
}

size_t fileRead(uint16_t handle, uint8_t* dst, size_t len) {
  Command cmd = {};
  cmd.op = Op::FileRead;
  cmd.handle = handle;
  cmd.buffer = dst;
  cmd.buffer_len = len;
  (void)submit(cmd);
  return cmd.size_value;
}

size_t fileWrite(uint16_t handle, const uint8_t* src, size_t len) {
  Command cmd = {};
  cmd.op = Op::FileWrite;
  cmd.handle = handle;
  cmd.const_buffer = src;
  cmd.buffer_len = len;
  (void)submit(cmd);
  return cmd.size_value;
}

void fileFlush(uint16_t handle) {
  Command cmd = {};
  cmd.op = Op::FileFlush;
  cmd.handle = handle;
  (void)submit(cmd);
}

void fileClose(uint16_t handle) {
  Command cmd = {};
  cmd.op = Op::FileClose;
  cmd.handle = handle;
  (void)submit(cmd);
}

bool fileOpenNext(uint16_t handle, uint16_t& out_handle) {
  Command cmd = {};
  cmd.op = Op::FileOpenNext;
  cmd.handle = handle;
  const bool ok = submit(cmd);
  out_handle = cmd.out_handle;
  return ok;
}

}  // namespace sd_worker
