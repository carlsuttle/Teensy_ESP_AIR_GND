#pragma once

#include <Arduino.h>

namespace sd_card_test {

struct Status {
  uint8_t cs_pin = 0;
  uint8_t sck_pin = 0;
  uint8_t miso_pin = 0;
  uint8_t mosi_pin = 0;
  uint32_t init_hz = 0;
  bool spi_configured = false;
  bool begin_ok = false;
  bool write_ok = false;
  uint8_t card_type = 0;
  uint64_t card_size_bytes = 0;
  uint64_t total_bytes = 0;
  uint64_t used_bytes = 0;
  uint32_t write_bytes = 0;
};

bool probe(Status& status);
bool writeProbe(Status& status);
void printProbeReport(Stream& out, const Status& status);

}  // namespace sd_card_test
