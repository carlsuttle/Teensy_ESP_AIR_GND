#pragma once

#include <Arduino.h>

namespace spi_bridge {

struct Stats {
  uint32_t transactions_completed = 0U;
  uint32_t transaction_failures = 0U;
  uint32_t state_records_received = 0U;
  uint32_t replay_records_sent = 0U;
  uint32_t rx_crc_errors = 0U;
  uint32_t rx_type_errors = 0U;
  uint32_t rx_overflows = 0U;
  uint32_t tx_overflows = 0U;
  uint32_t last_magic = 0U;
  uint16_t last_version = 0U;
  uint16_t last_type = 0U;
  uint16_t last_len = 0U;
};

void begin();
void poll();
bool popStateRecord(uint8_t* record_out, size_t len);
bool queueReplayRecord(const uint8_t* record_bytes, size_t len);
Stats stats();

}  // namespace spi_bridge
