#pragma once

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

namespace sd_api {

enum class OpenMode : uint8_t {
  read = 0,
  write,
};

class File {
 public:
  File() = default;
  explicit File(::File file);

  explicit operator bool() const;
  bool isDirectory();
  const char* name();
  size_t size();
  size_t position();
  bool seek(size_t pos);
  size_t read(uint8_t* dst, size_t len);
  size_t write(const uint8_t* src, size_t len);
  size_t print(const char* text);
  size_t print(const String& text);
  void flush();
  void close();
  File openNextFile();

 private:
  ::File file_;
};

bool begin(uint8_t cs_pin, SPIClass& spi, uint32_t hz);
void end();
uint8_t cardType();
uint64_t cardSize();
uint64_t totalBytes();
uint64_t usedBytes();

File open(const char* path, OpenMode mode = OpenMode::read);
File open(const String& path, OpenMode mode = OpenMode::read);
bool exists(const char* path);
bool exists(const String& path);
bool mkdir(const char* path);
bool mkdir(const String& path);
bool remove(const char* path);
bool remove(const String& path);
bool rename(const char* from, const char* to);
bool rename(const String& from, const String& to);

}  // namespace sd_api
