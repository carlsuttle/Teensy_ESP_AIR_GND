#pragma once

#include <Arduino.h>

#include "types_shared.h"

namespace time_service {

struct StatusSnapshot {
  uint8_t time_state = (uint8_t)telem::TimeStateCode::UNSET;
  uint8_t time_source = (uint8_t)telem::TimeSourceCode::NONE;
  uint8_t time_flags = 0U;
  uint8_t reserved0 = 0U;
  uint32_t system_time_utc_s = 0U;
  uint32_t time_last_set_ms = 0U;
  uint32_t time_last_set_age_ms = 0xFFFFFFFFUL;
  uint32_t time_sync_count = 0U;
  telem::GpsCalendarTime last_trusted_gps = {};
};

void begin();
void setVerbose(bool enabled);
void ingestState(const telem::TelemetryStateRecord& state, bool has_state, uint32_t now_ms);
void ingestGpsSample(uint8_t fix_type,
                     const telem::GpsCalendarTime& gps_time,
                     uint16_t state_flags,
                     uint32_t now_ms);
StatusSnapshot snapshot(uint32_t now_ms);

bool systemTimeUtcSeconds(uint32_t& out_utc_s);
void fillDownlinkStatus(telem::DownlinkStatusV1& status, uint32_t now_ms);
void fillStorageStatus(telem::StorageStatusPayloadV1& status, uint32_t now_ms);

bool runSelfTest(Stream& out);

}  // namespace time_service
