#pragma once

#include <Arduino.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>

namespace sd_backend {

struct Status {
  uint8_t cs_pin = 0;
  uint8_t sck_pin = 0;
  uint8_t miso_pin = 0;
  uint8_t mosi_pin = 0;
  uint32_t init_hz = 0;
  bool spi_configured = false;
  bool begin_ok = false;
  uint8_t card_type = CARD_NONE;
  uint64_t card_size_bytes = 0;
  uint64_t total_bytes = 0;
  uint64_t used_bytes = 0;
};

constexpr BaseType_t kSdWriterCore = 0;
constexpr UBaseType_t kSdWriterPriority = 1U;

uint8_t csPin();
uint8_t sckPin();
uint8_t misoPin();
uint8_t mosiPin();

void fillPinStatus(Status& status);
bool begin(Status* status = nullptr);
bool refreshStatus(Status& status);
bool mounted();
uint32_t mountedFrequencyHz();
void end();

}  // namespace sd_backend
