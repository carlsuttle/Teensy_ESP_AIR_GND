#pragma once

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

namespace sd_worker {

static constexpr uint16_t kInvalidFileHandle = 0xFFFFU;

void begin();

bool fsBegin(uint8_t cs_pin, SPIClass* spi, uint32_t hz);
void fsEnd();
uint8_t fsCardType();
uint64_t fsCardSize();
uint64_t fsTotalBytes();
uint64_t fsUsedBytes();

bool fsOpen(const char* path, const char* mode, uint16_t& out_handle);
bool fsExists(const char* path);
bool fsMkdir(const char* path);
bool fsRemove(const char* path);
bool fsRename(const char* from, const char* to);

bool fileValid(uint16_t handle);
bool fileIsDirectory(uint16_t handle);
bool fileName(uint16_t handle, char* out_name, size_t out_len);
size_t fileSize(uint16_t handle);
size_t filePosition(uint16_t handle);
bool fileLastWrite(uint16_t handle, time_t& out_time);
bool fileSeek(uint16_t handle, size_t pos);
size_t fileRead(uint16_t handle, uint8_t* dst, size_t len);
size_t fileWrite(uint16_t handle, const uint8_t* src, size_t len);
void fileFlush(uint16_t handle);
void fileClose(uint16_t handle);
bool fileOpenNext(uint16_t handle, uint16_t& out_handle);

}  // namespace sd_worker
