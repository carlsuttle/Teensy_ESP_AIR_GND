#include "gps_ubx.h"

namespace gps_ubx {
namespace {
constexpr uint8_t UBX_SYNC_1 = 0xB5;
constexpr uint8_t UBX_SYNC_2 = 0x62;
constexpr uint8_t UBX_CLASS_NAV = 0x01;
constexpr uint8_t UBX_ID_NAV_PVT = 0x07;
constexpr uint16_t UBX_NAV_PVT_LEN = 92;
constexpr uint16_t UBX_MAX_PAYLOAD = 512;

enum class ParseState : uint8_t {
  WAIT_SYNC1,
  WAIT_SYNC2,
  READ_CLASS,
  READ_ID,
  READ_LEN1,
  READ_LEN2,
  READ_PAYLOAD,
  READ_CK_A,
  READ_CK_B
};

HardwareSerial* g_serial = nullptr;
ParseState g_parse_state = ParseState::WAIT_SYNC1;
uint8_t g_msg_class = 0;
uint8_t g_msg_id = 0;
uint16_t g_msg_len = 0;
uint16_t g_payload_idx = 0;
uint8_t g_ck_a = 0;
uint8_t g_ck_b = 0;
uint8_t g_rx_ck_a = 0;
uint8_t g_payload[UBX_MAX_PAYLOAD];

uint32_t load_u32_le(const uint8_t* p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

int32_t load_i32_le(const uint8_t* p) {
  return (int32_t)load_u32_le(p);
}

void checksum_add(uint8_t b) {
  g_ck_a = (uint8_t)(g_ck_a + b);
  g_ck_b = (uint8_t)(g_ck_b + g_ck_a);
}

void parser_reset() {
  g_parse_state = ParseState::WAIT_SYNC1;
  g_msg_class = 0;
  g_msg_id = 0;
  g_msg_len = 0;
  g_payload_idx = 0;
  g_ck_a = 0;
  g_ck_b = 0;
  g_rx_ck_a = 0;
}

void decode_nav_pvt(State& s, const uint8_t* p, uint16_t len) {
  if (len != UBX_NAV_PVT_LEN) {
    s.gps_parse_errors++;
    return;
  }

  s.iTOW = load_u32_le(&p[0]);
  s.fixType = p[20];
  s.numSV = p[23];
  s.lon = load_i32_le(&p[24]);
  s.lat = load_i32_le(&p[28]);
  s.hMSL = load_i32_le(&p[36]);
  s.hAcc = load_u32_le(&p[40]);
  s.gSpeed = load_i32_le(&p[60]);
  s.headMot = load_i32_le(&p[64]);
  s.sAcc = load_u32_le(&p[68]);
  s.last_gps_ms = millis();
}

void feed_byte(State& s, uint8_t b) {
  switch (g_parse_state) {
    case ParseState::WAIT_SYNC1:
      if (b == UBX_SYNC_1) g_parse_state = ParseState::WAIT_SYNC2;
      break;

    case ParseState::WAIT_SYNC2:
      if (b == UBX_SYNC_2) {
        g_parse_state = ParseState::READ_CLASS;
        g_ck_a = 0;
        g_ck_b = 0;
      } else if (b != UBX_SYNC_1) {
        g_parse_state = ParseState::WAIT_SYNC1;
      }
      break;

    case ParseState::READ_CLASS:
      g_msg_class = b;
      checksum_add(b);
      g_parse_state = ParseState::READ_ID;
      break;

    case ParseState::READ_ID:
      g_msg_id = b;
      checksum_add(b);
      g_parse_state = ParseState::READ_LEN1;
      break;

    case ParseState::READ_LEN1:
      g_msg_len = b;
      checksum_add(b);
      g_parse_state = ParseState::READ_LEN2;
      break;

    case ParseState::READ_LEN2:
      g_msg_len |= (uint16_t)b << 8;
      checksum_add(b);
      if (g_msg_len > UBX_MAX_PAYLOAD) {
        s.gps_parse_errors++;
        parser_reset();
      } else {
        g_payload_idx = 0;
        g_parse_state = (g_msg_len == 0) ? ParseState::READ_CK_A : ParseState::READ_PAYLOAD;
      }
      break;

    case ParseState::READ_PAYLOAD:
      g_payload[g_payload_idx++] = b;
      checksum_add(b);
      if (g_payload_idx >= g_msg_len) g_parse_state = ParseState::READ_CK_A;
      break;

    case ParseState::READ_CK_A:
      g_rx_ck_a = b;
      g_parse_state = ParseState::READ_CK_B;
      break;

    case ParseState::READ_CK_B: {
      const uint8_t rx_ck_b = b;
      if (g_rx_ck_a == g_ck_a && rx_ck_b == g_ck_b) {
        if (g_msg_class == UBX_CLASS_NAV && g_msg_id == UBX_ID_NAV_PVT) {
          decode_nav_pvt(s, g_payload, g_msg_len);
        }
      } else {
        s.gps_parse_errors++;
      }
      parser_reset();
      break;
    }
  }
}
}  // namespace

void begin(HardwareSerial& serial, uint32_t baud) {
  g_serial = &serial;
  g_serial->begin(baud);
  parser_reset();
}

void poll(State& s) {
  if (!g_serial) return;
  while (g_serial->available() > 0) {
    feed_byte(s, (uint8_t)g_serial->read());
  }
}

bool hasRecentFix(const State& s, uint32_t maxAgeMs) {
  if (s.fixType < 3 || s.last_gps_ms == 0) return false;
  return (uint32_t)(millis() - s.last_gps_ms) <= maxAgeMs;
}

}  // namespace gps_ubx
