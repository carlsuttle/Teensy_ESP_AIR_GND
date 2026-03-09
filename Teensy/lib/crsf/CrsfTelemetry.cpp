#include "CrsfTelemetry.h"
#include <math.h>

namespace crsf {

static inline void put_u16be(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xFF); }
static inline void put_i16be(uint8_t* p, int16_t v)  { put_u16be(p, (uint16_t)v); }
static inline void put_i32be(uint8_t* p, int32_t v)  {
    p[0] = (uint8_t)((uint32_t)v >> 24);
    p[1] = (uint8_t)((uint32_t)v >> 16);
    p[2] = (uint8_t)((uint32_t)v >> 8);
    p[3] = (uint8_t)((uint32_t)v & 0xFF);
}

uint8_t crc8_dvb_s2(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0xD5) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

bool sendFrame(Stream& out, uint8_t type, const uint8_t* payload, size_t payload_len) {
    if (payload_len > 60) return false;

    const uint8_t len = (uint8_t)(payload_len + 2); // TYPE + CRC + payload
    uint8_t header[3] = { SYNC, len, type };
    const size_t total_len = sizeof(header) + payload_len + 1;

    // Non-blocking: only write when the TX buffer can accept the full frame.
    if (out.availableForWrite() < (int)total_len) {
        return false;
    }

    // CRC is over [TYPE][PAYLOAD...]
    uint8_t crc_buf[1 + 60];
    crc_buf[0] = type;
    if (payload_len && payload) memcpy(&crc_buf[1], payload, payload_len);
    const uint8_t crc = crc8_dvb_s2(crc_buf, 1 + payload_len);

    const size_t h = out.write(header, sizeof(header));
    if (h != sizeof(header)) return false;
    if (payload_len && payload) {
        const size_t p = out.write(payload, payload_len);
        if (p != payload_len) return false;
    }
    const size_t c = out.write(&crc, 1);
    return c == 1;
}

bool sendGps(Stream& out, double lat_deg, double lon_deg, float groundSpeed_mps,
             float heading_deg, float altitude_m, uint8_t sats) {

    // Convert to CRSF units:
    // - lat/lon: deg * 1e7
    // - groundspeed: km/h * 10 (0.1 km/h)
    // - heading: deg * 100
    // - altitude: meters + 1000 offset (uint16)
    const int32_t lat = (int32_t)lrint(lat_deg * 1e7);
    const int32_t lon = (int32_t)lrint(lon_deg * 1e7);

    float gspd_kmh = groundSpeed_mps * 3.6f;
    uint16_t gspd = (uint16_t)constrain((int)lrintf(gspd_kmh * 10.0f), 0, 65535);

    // Normalize heading into [0,360)
    while (heading_deg < 0) heading_deg += 360.0f;
    while (heading_deg >= 360.0f) heading_deg -= 360.0f;
    uint16_t hdg = (uint16_t)lrintf(heading_deg * 100.0f);

    int alt_i = (int)lrintf(altitude_m + 1000.0f);
    uint16_t alt = (uint16_t)constrain(alt_i, 0, 65535);

    uint8_t pl[4 + 4 + 2 + 2 + 2 + 1];
    put_i32be(&pl[0], lat);
    put_i32be(&pl[4], lon);
    put_u16be(&pl[8], gspd);
    put_u16be(&pl[10], hdg);
    put_u16be(&pl[12], alt);
    pl[14] = sats;

    return sendFrame(out, FRAMETYPE_GPS, pl, sizeof(pl));
}

bool sendVario(Stream& out, float verticalSpeed_mps) {
    // CRSF VARIO is int16 in **cm/s** (signed).
    // +1.23 m/s => +123 cm/s.
    int16_t v = (int16_t)constrain((int)lrintf(verticalSpeed_mps * 100.0f), -32768, 32767);
    uint8_t pl[2];
    put_i16be(pl, v);
    return sendFrame(out, FRAMETYPE_VARIO, pl, sizeof(pl));
}

bool sendBaroAltitude(Stream& out, float altitude_m) {
    // CRSF BARO ALTITUDE uses a 16-bit packed encoding:
    // - Below ~2276.7 m: decimeters with +1000 m offset
    // - Above that: meters with 0x8000 flag
    const int32_t altitude_dm = (int32_t)lrintf(altitude_m * 10.0f);
    constexpr int32_t ALT_MIN_DM = -10000;                  // -1000.0 m
    constexpr int32_t ALT_THRESH_DM = 0x8000 - ALT_MIN_DM;  // 22768 dm
    constexpr int32_t ALT_MAX_DM = (0x7ffe * 10) - 5;       // 32765.5 m in dm

    uint16_t enc = 0;
    if (altitude_dm < ALT_MIN_DM) {
        enc = 0;
    } else if (altitude_dm > ALT_MAX_DM) {
        enc = 0xFFFE;
    } else if (altitude_dm < ALT_THRESH_DM) {
        enc = (uint16_t)(altitude_dm - ALT_MIN_DM);
    } else {
        enc = (uint16_t)(((altitude_dm + 5) / 10) | 0x8000);
    }

    uint8_t pl[2];
    put_u16be(pl, enc);
    return sendFrame(out, FRAMETYPE_BARO_ALTITUDE, pl, sizeof(pl));
}

bool sendBattery(Stream& out, float voltage_V, float current_A, uint32_t used_mAh, uint8_t remaining_pct) {
    // CRSF battery payload uses deci-units for voltage/current.
    // 12.3 V -> 123, 4.2 A -> 42.
    uint32_t v_raw = (uint32_t)constrain((int)lrintf(voltage_V * 10.0f), 0, 65535);
    uint32_t c_raw = (uint32_t)constrain((int)lrintf(current_A * 10.0f), 0, 65535);

    uint8_t pl[2 + 2 + 3 + 1];
    put_u16be(&pl[0], (uint16_t)v_raw);
    put_u16be(&pl[2], (uint16_t)c_raw);

    // uint24 capacity used (mAh)
    used_mAh = used_mAh & 0xFFFFFF;
    pl[4] = (uint8_t)((used_mAh >> 16) & 0xFF);
    pl[5] = (uint8_t)((used_mAh >> 8) & 0xFF);
    pl[6] = (uint8_t)(used_mAh & 0xFF);

    pl[7] = remaining_pct;
    return sendFrame(out, FRAMETYPE_BATTERY_SENSOR, pl, sizeof(pl));
}

bool sendAttitude(Stream& out, float roll_deg, float pitch_deg, float yaw_deg) {
    // CRSF attitude uses radians * 10000 (int16), big-endian.
    const float deg2rad = 0.01745329251994329577f;
    float roll = roll_deg * deg2rad;
    float pitch = pitch_deg * deg2rad;
    float yaw = yaw_deg * deg2rad;

    int16_t p = (int16_t)constrain((int)lrintf(pitch * 10000.0f), -32768, 32767);
    int16_t r = (int16_t)constrain((int)lrintf(roll  * 10000.0f), -32768, 32767);
    int16_t y = (int16_t)constrain((int)lrintf(yaw   * 10000.0f), -32768, 32767);

    uint8_t pl[6];
    put_i16be(&pl[0], p);
    put_i16be(&pl[2], r);
    put_i16be(&pl[4], y);

    return sendFrame(out, FRAMETYPE_ATTITUDE, pl, sizeof(pl));
}

} // namespace crsf
