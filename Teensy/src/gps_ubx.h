#pragma once

#include <Arduino.h>
#include "state.h"

namespace gps_ubx {

void begin(HardwareSerial& serial, uint32_t baud);
void poll(State& s);
bool hasRecentFix(const State& s, uint32_t maxAgeMs = 3000);

}  // namespace gps_ubx

