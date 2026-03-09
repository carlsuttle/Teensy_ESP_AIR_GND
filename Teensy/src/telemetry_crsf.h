#pragma once

#include <Arduino.h>
#include "state.h"

struct CrsfRxStats {
  uint32_t rxBytes;
  uint32_t rxFrames;
  uint32_t rxRcFrames;
  uint8_t lastType;
  uint32_t lastFrameMs;
};

void telemetry_setup();
void telemetry_loop(const State& s);
void telemetry_getCrsfRxStats(CrsfRxStats& out);
