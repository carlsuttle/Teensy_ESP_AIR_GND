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
};

void begin();
void poll();
bool startLatest();
void stop();
Status status();
telem::ReplayStatusPayloadV1 currentPayload();
bool takeStatusDirty();

}  // namespace replay_bridge
