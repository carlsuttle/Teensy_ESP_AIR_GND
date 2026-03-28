#pragma once

#include <Arduino.h>

namespace spi_bridge {

struct Stats {
  uint32_t tx_records = 0U;
  uint32_t tx_overflows = 0U;
  uint32_t rx_records = 0U;
  uint32_t rx_overflows = 0U;
  uint32_t rx_crc_errors = 0U;
  uint32_t rx_type_errors = 0U;
  uint32_t state_tx_max_occupancy = 0U;
  uint32_t raw_tx_max_occupancy = 0U;
  uint32_t replay_rx_max_occupancy = 0U;
  uint32_t state_tx_free_min = 0U;
  uint32_t raw_tx_free_min = 0U;
  uint32_t replay_rx_free_min = 0U;
  bool ready_high = false;
};

void begin();
void poll();
bool pushStateRecord(const uint8_t* record_bytes, size_t len);
bool pushRawRecord(const uint8_t* record_bytes, size_t len);
bool popReplayRecord(uint8_t* record_out, size_t len);
uint16_t replayFreeSlots();
Stats stats();
void resetStats();

}  // namespace spi_bridge

