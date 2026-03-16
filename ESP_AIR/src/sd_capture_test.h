#pragma once

#include <Arduino.h>

#include "types_shared.h"

namespace sd_capture_test {

struct Stats {
  bool active = false;
  bool backend_ready = false;
  bool last_start_ok = false;
  bool completed = false;
  bool timed_out = false;
  uint8_t cs_pin = 0;
  uint8_t sck_pin = 0;
  uint8_t miso_pin = 0;
  uint8_t mosi_pin = 0;
  uint32_t init_hz = 0;
  uint32_t duration_ms = 0;
  uint32_t started_ms = 0;
  uint32_t stopped_ms = 0;
  uint32_t queue_cur = 0;
  uint32_t queue_max = 0;
  uint32_t enqueued = 0;
  uint32_t dropped = 0;
  uint32_t records_written = 0;
  uint32_t bytes_written = 0;
  uint32_t flushes = 0;
  uint32_t fs_open_last_ms = 0;
  uint32_t fs_open_max_ms = 0;
  uint32_t fs_write_last_ms = 0;
  uint32_t fs_write_max_ms = 0;
  uint32_t fs_close_last_ms = 0;
  uint32_t fs_close_max_ms = 0;
  char last_error[48] = {};
  char file_name[48] = {};
};

void begin();
bool start(uint32_t duration_ms);
void stop(bool timed_out = false);
void poll();
void enqueueState(uint32_t seq, uint32_t t_us, const telem::TelemetryFullStateV1& state);
Stats stats();
void clearCompleted();
void printReport(Stream& out, const Stats& stats);

}  // namespace sd_capture_test
