#pragma once

#include <Arduino.h>

namespace ws_server {

void begin();
void loop();
uint32_t clientCount();
void resetCounters();

}  // namespace ws_server
