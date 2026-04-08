#pragma once

#include <Arduino.h>

namespace serial_control {

bool handleLine(const char* line, Stream& io);

}  // namespace serial_control
