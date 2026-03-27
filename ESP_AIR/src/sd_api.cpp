#include "sd_api.h"

namespace sd_api {

File::File(::File file) : file_(file) {}

File::operator bool() const {
  return (bool)file_;
}

bool File::isDirectory() {
  return file_ && file_.isDirectory();
}

const char* File::name() {
  return file_ ? file_.name() : "";
}

size_t File::size() {
  return file_ ? (size_t)file_.size() : 0U;
}

size_t File::position() {
  return file_ ? (size_t)file_.position() : 0U;
}

bool File::seek(size_t pos) {
  return file_ && file_.seek(pos);
}

size_t File::read(uint8_t* dst, size_t len) {
  if (!file_ || !dst || len == 0U) return 0U;
  return file_.read(dst, len);
}

size_t File::write(const uint8_t* src, size_t len) {
  if (!file_ || !src || len == 0U) return 0U;
  return file_.write(src, len);
}

size_t File::print(const char* text) {
  if (!file_ || !text) return 0U;
  return file_.print(text);
}

size_t File::print(const String& text) {
  if (!file_) return 0U;
  return file_.print(text);
}

void File::flush() {
  if (file_) file_.flush();
}

void File::close() {
  if (file_) file_.close();
}

File File::openNextFile() {
  if (!file_) return File();
  return File(file_.openNextFile());
}

bool begin(uint8_t cs_pin, SPIClass& spi, uint32_t hz) {
  return SD.begin(cs_pin, spi, hz);
}

void end() {
  SD.end();
}

uint8_t cardType() {
  return SD.cardType();
}

uint64_t cardSize() {
  return SD.cardSize();
}

uint64_t totalBytes() {
  return SD.totalBytes();
}

uint64_t usedBytes() {
  return SD.usedBytes();
}

File open(const char* path, OpenMode mode) {
  if (!path) return File();
  return File(SD.open(path, mode == OpenMode::write ? FILE_WRITE : FILE_READ));
}

File open(const String& path, OpenMode mode) {
  return open(path.c_str(), mode);
}

bool exists(const char* path) {
  return path && SD.exists(path);
}

bool exists(const String& path) {
  return exists(path.c_str());
}

bool mkdir(const char* path) {
  return path && SD.mkdir(path);
}

bool mkdir(const String& path) {
  return mkdir(path.c_str());
}

bool remove(const char* path) {
  return path && SD.remove(path);
}

bool remove(const String& path) {
  return remove(path.c_str());
}

bool rename(const char* from, const char* to) {
  return from && to && SD.rename(from, to);
}

bool rename(const String& from, const String& to) {
  return rename(from.c_str(), to.c_str());
}

}  // namespace sd_api
