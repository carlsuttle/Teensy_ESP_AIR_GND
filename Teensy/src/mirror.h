#pragma once

#include <Arduino.h>
#include "state.h"

namespace mirror {

struct RxDebugStats {
  uint32_t rxBytes;
  uint32_t framesOk;
  uint32_t cobsErr;
  uint32_t lenErr;
  uint32_t crcErr;
  uint32_t unknownMsg;
  uint32_t cmdSetFusion;
  uint32_t cmdGetFusion;
  uint32_t cmdSetStreamRate;
  uint32_t ackSent;
  uint32_t nackSent;
  uint16_t lastMsgType;
};

void begin();
void pollRx();
bool sendFastState(const State& s, uint32_t seq, uint32_t t_us);
uint16_t crc16Ccitt(const uint8_t* data, uint16_t len);
RxDebugStats getRxDebugStats();
uint16_t streamRateHz();
uint16_t logRateHz();
uint32_t streamPeriodUs();

}  // namespace mirror
