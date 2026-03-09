#pragma once
#include <Arduino.h>

namespace crsf {

// CRSF framing
static constexpr uint8_t SYNC = 0xC8;

// Telemetry frame types (broadcast)
static constexpr uint8_t FRAMETYPE_GPS            = 0x02;
static constexpr uint8_t FRAMETYPE_VARIO          = 0x07;
static constexpr uint8_t FRAMETYPE_BATTERY_SENSOR = 0x08;
static constexpr uint8_t FRAMETYPE_BARO_ALTITUDE  = 0x09;
static constexpr uint8_t FRAMETYPE_ATTITUDE       = 0x1E;

// CRC8 DVB-S2 poly 0xD5 (used by CRSF)
uint8_t crc8_dvb_s2(const uint8_t* data, size_t len);

// Helper to build and send a CRSF frame on a Stream (HardwareSerial etc).
// Frame format: [SYNC][LEN][TYPE][PAYLOAD...][CRC]
// LEN = (payload_len + 2) = bytes from TYPE through CRC inclusive.
bool sendFrame(Stream& out, uint8_t type, const uint8_t* payload, size_t payload_len);

// Telemetry payload encoders (big-endian)
// GPS (0x02) payload: int32 lat (deg * 1e7), int32 lon (deg * 1e7),
// uint16 groundspeed (km/h * 10), uint16 heading (deg * 100),
// uint16 altitude (m + 1000), uint8 sats
bool sendGps(Stream& out, double lat_deg, double lon_deg, float groundSpeed_mps,
             float heading_deg, float altitude_m, uint8_t sats);

// VARIO (0x07) payload: int16 vertical speed (cm/s)
bool sendVario(Stream& out, float verticalSpeed_mps);

// BARO ALTITUDE (0x09) payload: int16 altitude in CRSF baro encoding.
bool sendBaroAltitude(Stream& out, float altitude_m);

// BATTERY (0x08) payload:
// uint16 voltage (V * 10), uint16 current (A * 10), uint24 capacity (mAh), uint8 remaining (%)
bool sendBattery(Stream& out, float voltage_V, float current_A, uint32_t used_mAh, uint8_t remaining_pct);

// ATTITUDE (0x1E) payload: int16 pitch, int16 roll, int16 yaw (rad * 10000)
bool sendAttitude(Stream& out, float roll_deg, float pitch_deg, float yaw_deg);

} // namespace crsf
