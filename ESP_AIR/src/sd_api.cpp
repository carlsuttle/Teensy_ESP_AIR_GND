#include "sd_api.h"

#include <new>

#include "sd_worker.h"

namespace sd_api {

struct File::SharedHandle {
  uint16_t handle = sd_worker::kInvalidFileHandle;
  uint16_t refs = 1U;
};

File::File(uint16_t handle) {
  if (handle == sd_worker::kInvalidFileHandle) return;
  shared_ = new (std::nothrow) SharedHandle{};
  if (!shared_) {
    sd_worker::fileClose(handle);
    return;
  }
  shared_->handle = handle;
}

File::File(const File& other) {
  retain(other);
}

File::File(File&& other) noexcept {
  shared_ = other.shared_;
  other.shared_ = nullptr;
  name_cache_[0] = '\0';
}

File::~File() {
  release();
}

File& File::operator=(const File& other) {
  if (this == &other) return *this;
  release();
  retain(other);
  return *this;
}

File& File::operator=(File&& other) noexcept {
  if (this == &other) return *this;
  release();
  shared_ = other.shared_;
  other.shared_ = nullptr;
  name_cache_[0] = '\0';
  return *this;
}

void File::retain(const File& other) {
  shared_ = other.shared_;
  if (shared_) shared_->refs++;
  name_cache_[0] = '\0';
}

void File::release() {
  if (!shared_) return;
  if (shared_->refs > 1U) {
    shared_->refs--;
    shared_ = nullptr;
    return;
  }
  sd_worker::fileClose(shared_->handle);
  delete shared_;
  shared_ = nullptr;
}

bool File::validHandle() const {
  return shared_ && shared_->handle != sd_worker::kInvalidFileHandle;
}

uint16_t File::handleValue() const {
  return validHandle() ? shared_->handle : sd_worker::kInvalidFileHandle;
}

File::operator bool() const {
  return validHandle() && sd_worker::fileValid(handleValue());
}

bool File::isDirectory() {
  return validHandle() && sd_worker::fileIsDirectory(handleValue());
}

const char* File::name() {
  name_cache_[0] = '\0';
  if (!validHandle()) return name_cache_;
  (void)sd_worker::fileName(handleValue(), name_cache_, sizeof(name_cache_));
  return name_cache_;
}

size_t File::size() {
  return validHandle() ? sd_worker::fileSize(handleValue()) : 0U;
}

size_t File::position() {
  return validHandle() ? sd_worker::filePosition(handleValue()) : 0U;
}

time_t File::getLastWrite() {
  time_t out = (time_t)0;
  if (!validHandle()) return out;
  (void)sd_worker::fileLastWrite(handleValue(), out);
  return out;
}

bool File::seek(size_t pos) {
  return validHandle() && sd_worker::fileSeek(handleValue(), pos);
}

size_t File::read(uint8_t* dst, size_t len) {
  if (!validHandle() || !dst || len == 0U) return 0U;
  return sd_worker::fileRead(handleValue(), dst, len);
}

size_t File::write(const uint8_t* src, size_t len) {
  if (!validHandle() || !src || len == 0U) return 0U;
  return sd_worker::fileWrite(handleValue(), src, len);
}

size_t File::print(const char* text) {
  if (!text) return 0U;
  return write(reinterpret_cast<const uint8_t*>(text), strlen(text));
}

size_t File::print(const String& text) {
  return print(text.c_str());
}

void File::flush() {
  if (validHandle()) sd_worker::fileFlush(handleValue());
}

void File::close() {
  release();
}

File File::openNextFile() {
  if (!validHandle()) return File();
  uint16_t next = sd_worker::kInvalidFileHandle;
  if (!sd_worker::fileOpenNext(handleValue(), next)) return File();
  return File(next);
}

bool begin(uint8_t cs_pin, SPIClass& spi, uint32_t hz) {
  return sd_worker::fsBegin(cs_pin, &spi, hz);
}

void end() {
  sd_worker::fsEnd();
}

uint8_t cardType() {
  return sd_worker::fsCardType();
}

uint64_t cardSize() {
  return sd_worker::fsCardSize();
}

uint64_t totalBytes() {
  return sd_worker::fsTotalBytes();
}

uint64_t usedBytes() {
  return sd_worker::fsUsedBytes();
}

File open(const char* path, OpenMode mode) {
  if (!path) return File();
  uint16_t handle = sd_worker::kInvalidFileHandle;
  const char* open_mode = mode == OpenMode::write ? FILE_WRITE : FILE_READ;
  if (!sd_worker::fsOpen(path, open_mode, handle)) return File();
  return File(handle);
}

File open(const String& path, OpenMode mode) {
  return open(path.c_str(), mode);
}

bool exists(const char* path) {
  return path && sd_worker::fsExists(path);
}

bool exists(const String& path) {
  return exists(path.c_str());
}

bool mkdir(const char* path) {
  return path && sd_worker::fsMkdir(path);
}

bool mkdir(const String& path) {
  return mkdir(path.c_str());
}

bool remove(const char* path) {
  return path && sd_worker::fsRemove(path);
}

bool remove(const String& path) {
  return remove(path.c_str());
}

bool rename(const char* from, const char* to) {
  return from && to && sd_worker::fsRename(from, to);
}

bool rename(const String& from, const String& to) {
  return rename(from.c_str(), to.c_str());
}

}  // namespace sd_api
