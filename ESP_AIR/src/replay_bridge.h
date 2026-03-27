#pragma once

#include <Arduino.h>

#include "types_shared.h"

namespace replay_bridge {

struct Status {
  uint8_t flags = 0;
  uint16_t last_command = 0;
  uint32_t session_id = 0;
  uint32_t records_total = 0;
  uint32_t records_sent = 0;
  uint32_t last_error = 0;
  uint32_t last_change_ms = 0;
  char current_file[telem::kLogFileNameBytes] = {};
};

void begin();
void poll();
bool startLatest();
bool startFile(const String& file_name);
void setAverageFactor(uint8_t factor);
uint8_t averageFactor();
bool resume();
bool pause();
bool seekRelative(int32_t delta_records);
void stop();
Status status();
telem::ReplayStatusPayloadV1 currentPayload();
bool takeStatusDirty();
bool active();
String currentFileName();
bool takeOutputSourceStamp(uint32_t& seq, uint32_t& t_us);

}  // namespace replay_bridge
