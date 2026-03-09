#include "telemetry_crsf.h"

#include <math.h>

#include "config.h"
#include "CrsfTelemetry.h"

namespace {

constexpr uint8_t CRSF_ADDR_FC = 0xC8;
constexpr uint32_t RATE_ATTITUDE_HZ = 20;
constexpr uint32_t RATE_VARIO_HZ = 10;
constexpr uint32_t RATE_GPS_HZ = 10;

uint32_t g_sent_att = 0;
uint32_t g_sent_vario = 0;
uint32_t g_sent_baro = 0;
uint32_t g_sent_gps = 0;

uint32_t g_rx_bytes = 0;
uint32_t g_rx_frames = 0;
uint32_t g_rx_rc_frames = 0;
uint8_t g_rx_last_type = 0;
uint32_t g_rx_last_frame_ms = 0;
bool g_led_on = false;
uint32_t g_led_pulse_ms = 0;
uint32_t g_led_gap_ms = 0;
constexpr uint16_t CRSF_LED_FLASH_MS = 40;
constexpr uint16_t CRSF_LED_MIN_GAP_MS = 200;

struct RxParser {
  enum StateId : uint8_t { WAIT_ADDR, WAIT_LEN, READ_BODY };
  StateId state = WAIT_ADDR;
  uint8_t len = 0;
  uint8_t body[64];
  uint8_t idx = 0;

  bool feed(uint8_t b, uint8_t& outType) {
    switch (state) {
      case WAIT_ADDR:
        if (b != CRSF_ADDR_FC) return false;
        state = WAIT_LEN;
        return false;

      case WAIT_LEN:
        len = b;
        if (len < 2 || len > sizeof(body)) {
          state = WAIT_ADDR;
          return false;
        }
        idx = 0;
        state = READ_BODY;
        return false;

      case READ_BODY:
        body[idx++] = b;
        if (idx >= len) {
          const uint8_t type = body[0];
          const uint8_t crc_rx = body[len - 1];
          const uint8_t crc_calc = crsf::crc8_dvb_s2(body, (size_t)(len - 1));
          state = WAIT_ADDR;
          if (crc_calc == crc_rx) {
            outType = type;
            return true;
          }
        }
        return false;
    }
    state = WAIT_ADDR;
    return false;
  }
};

RxParser g_rx;
uint32_t g_last_att_ms = 0;
uint32_t g_last_var_ms = 0;
uint32_t g_last_gps_ms = 0;

}  // namespace

void telemetry_setup() {
  CRSF_SERIAL.begin(CRSF_BAUD);
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
#endif
}

void telemetry_loop(const State& s) {
  bool got_rc_frame = false;
  bool got_crsf_frame = false;

  while (CRSF_SERIAL.available() > 0) {
    const uint8_t b = (uint8_t)CRSF_SERIAL.read();
    g_rx_bytes++;
    uint8_t type = 0;
    if (g_rx.feed(b, type)) {
      got_crsf_frame = true;
      g_rx_frames++;
      g_rx_last_type = type;
      g_rx_last_frame_ms = millis();
      if (type == 0x16) {
        g_rx_rc_frames++;
        got_rc_frame = true;
      }
    }
  }

#ifdef LED_BUILTIN
  const uint32_t now_ms = millis();
  if (got_crsf_frame && ((uint32_t)(now_ms - g_led_gap_ms) >= CRSF_LED_MIN_GAP_MS)) {
    digitalWrite(LED_BUILTIN, HIGH);
    g_led_on = true;
    g_led_pulse_ms = now_ms;
    g_led_gap_ms = now_ms;
  }
  if (g_led_on && ((uint32_t)(now_ms - g_led_pulse_ms) >= CRSF_LED_FLASH_MS)) {
    digitalWrite(LED_BUILTIN, LOW);
    g_led_on = false;
  }
#endif

  if (!got_rc_frame) return;

  const uint32_t att_period_ms = 1000U / RATE_ATTITUDE_HZ;
  const uint32_t var_period_ms = 1000U / RATE_VARIO_HZ;
  const uint32_t gps_period_ms = 1000U / RATE_GPS_HZ;

  const bool due_att = ((uint32_t)(now_ms - g_last_att_ms) >= att_period_ms);
  const bool due_var = ((uint32_t)(now_ms - g_last_var_ms) >= var_period_ms);
  const bool due_gps = ((uint32_t)(now_ms - g_last_gps_ms) >= gps_period_ms);

  float yaw_deg = s.yaw;
  while (yaw_deg > 180.0f) yaw_deg -= 360.0f;
  while (yaw_deg < -180.0f) yaw_deg += 360.0f;

  if (due_att) {
    if (crsf::sendAttitude(CRSF_SERIAL, s.roll, s.pitch, yaw_deg)) g_sent_att++;
    g_last_att_ms = now_ms;
  }

  if (due_var) {
    if (!isnan(s.baro_alt_m)) {
      if (crsf::sendBaroAltitude(CRSF_SERIAL, s.baro_alt_m)) g_sent_baro++;
    }
    if (!isnan(s.baro_vsi_mps)) {
      if (crsf::sendVario(CRSF_SERIAL, s.baro_vsi_mps)) g_sent_vario++;
    }
    g_last_var_ms = now_ms;
  }

  if (due_gps) {
    const bool gps_valid = (s.fixType >= 3) && (s.last_gps_ms != 0) && ((uint32_t)(now_ms - s.last_gps_ms) <= 3000U);
    const double lat = gps_valid ? ((double)s.lat * 1e-7) : 0.0;
    const double lon = gps_valid ? ((double)s.lon * 1e-7) : 0.0;
    const float spd = gps_valid ? ((float)s.gSpeed / 1000.0f) : 0.0f;
    float hdg = gps_valid ? ((float)s.headMot / 100000.0f) : 0.0f;
    while (hdg < 0.0f) hdg += 360.0f;
    while (hdg >= 360.0f) hdg -= 360.0f;
    const float alt = gps_valid ? ((float)s.hMSL / 1000.0f) : 0.0f;
    const uint8_t sats = gps_valid ? s.numSV : 0;

    if (crsf::sendGps(CRSF_SERIAL, lat, lon, spd, hdg, alt, sats)) g_sent_gps++;
    g_last_gps_ms = now_ms;
  }
}

void telemetry_getCrsfRxStats(CrsfRxStats& out) {
  out.rxBytes = g_rx_bytes;
  out.rxFrames = g_rx_frames;
  out.rxRcFrames = g_rx_rc_frames;
  out.lastType = g_rx_last_type;
  out.lastFrameMs = g_rx_last_frame_ms;
}
