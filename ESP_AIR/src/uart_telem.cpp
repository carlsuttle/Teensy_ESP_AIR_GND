#include "uart_telem.h"

#include <string.h>

#include "log_store.h"

namespace uart_telem {
namespace {

HardwareSerial* g_ser = nullptr;
HardwareSerial g_serial1(1);
HardwareSerial g_serial2(2);
uint32_t g_tx_seq = 1;

static constexpr size_t RAW_MAX = 768;
static constexpr size_t DECODED_MAX = 768;
uint8_t g_raw[RAW_MAX];
size_t g_raw_len = 0;
static constexpr char kEspProbePrefix[] = "ESPTEST?";
uint8_t g_probe_match = 0;
bool g_probe_capture = false;
char g_probe_tail[48] = {};
uint8_t g_probe_tail_len = 0;

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
telem::TelemetryFullStateV1 g_state = {};
bool g_has_state = false;
uint32_t g_seq = 0;
uint32_t g_t_us = 0;
telem::FusionSettingsV1 g_fusion_settings = {};
bool g_has_fusion_settings = false;
uint32_t g_fusion_rx_seq = 0;
RxStats g_stats = {};
bool g_has_ack = false;
uint32_t g_ack_rx_seq = 0;
uint16_t g_ack_command = 0;
bool g_ack_ok = false;
uint32_t g_ack_code = 0;

uint32_t crc32_calc(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1));
    }
  }
  return ~crc;
}

size_t cobsDecode(const uint8_t* in, size_t len, uint8_t* out, size_t out_max) {
  size_t ip = 0;
  size_t op = 0;
  while (ip < len) {
    const uint8_t code = in[ip++];
    if (code == 0) return 0;
    for (uint8_t i = 1; i < code; ++i) {
      if (ip >= len || op >= out_max) return 0;
      out[op++] = in[ip++];
    }
    if (code != 0xFF && ip < len) {
      if (op >= out_max) return 0;
      out[op++] = 0;
    }
  }
  return op;
}

size_t cobsEncode(const uint8_t* in, size_t len, uint8_t* out, size_t out_max) {
  size_t read_index = 0;
  size_t write_index = 1;
  size_t code_index = 0;
  uint8_t code = 1;

  while (read_index < len) {
    if (in[read_index] == 0) {
      if (code_index >= out_max) return 0;
      out[code_index] = code;
      code = 1;
      code_index = write_index++;
      if (write_index > out_max) return 0;
      read_index++;
    } else {
      if (write_index >= out_max) return 0;
      out[write_index++] = in[read_index++];
      code++;
      if (code == 0xFF) {
        if (code_index >= out_max) return 0;
        out[code_index] = code;
        code = 1;
        code_index = write_index++;
        if (write_index > out_max) return 0;
      }
    }
  }
  if (code_index >= out_max) return 0;
  out[code_index] = code;
  return write_index;
}

bool writeFramed(uint16_t msg_type, const void* payload, uint16_t payload_len) {
  if (!g_ser) return false;
  uint8_t packet[DECODED_MAX];
  const size_t header_len = sizeof(telem::FrameHeader);
  const size_t need = header_len + payload_len + sizeof(uint32_t);
  if (need > sizeof(packet)) return false;

  telem::FrameHeader h = {};
  h.magic = telem::kMagic;
  h.version = telem::kVersion;
  h.msg_type = msg_type;
  h.payload_len = payload_len;
  h.seq = g_tx_seq++;
  h.t_us = micros();

  memcpy(packet, &h, header_len);
  if (payload_len && payload) memcpy(packet + header_len, payload, payload_len);
  const uint32_t crc = crc32_calc(packet, header_len + payload_len);
  memcpy(packet + header_len + payload_len, &crc, sizeof(crc));

  uint8_t encoded[DECODED_MAX + 8];
  const size_t enc_len = cobsEncode(packet, need, encoded, sizeof(encoded) - 1);
  if (enc_len == 0) return false;
  encoded[enc_len] = 0;

  if ((size_t)g_ser->availableForWrite() < enc_len + 1) return false;
  return g_ser->write(encoded, enc_len + 1) == enc_len + 1;
}

void resetProbeCapture() {
  g_probe_capture = false;
  g_probe_tail_len = 0;
  g_probe_tail[0] = '\0';
}

void sendProbeAck(const char* tail) {
  if (!g_ser) return;
  uint32_t seq = 0;
  bool hasSeq = false;
  if (tail) {
    const char* p = strstr(tail, "seq=");
    if (p) {
      p += 4;
      char* end = nullptr;
      const unsigned long v = strtoul(p, &end, 10);
      if (end != p) {
        seq = (uint32_t)v;
        hasSeq = true;
      }
    }
  }
  if (hasSeq) {
    g_ser->printf("ESPTEST_ACK seq=%lu\r\n", (unsigned long)seq);
  } else {
    g_ser->print("ESPTEST_ACK\r\n");
  }
}

void probeSniffByte(uint8_t b) {
  const size_t prefix_len = sizeof(kEspProbePrefix) - 1U;

  if (g_probe_capture) {
    if (b == '\r' || b == '\n') {
      g_probe_tail[g_probe_tail_len] = '\0';
      sendProbeAck(g_probe_tail);
      resetProbeCapture();
      g_probe_match = 0;
      return;
    }
    if (b < 32U || b > 126U) {
      resetProbeCapture();
      g_probe_match = 0;
      return;
    }
    if (g_probe_tail_len + 1U < sizeof(g_probe_tail)) {
      g_probe_tail[g_probe_tail_len++] = (char)b;
      g_probe_tail[g_probe_tail_len] = '\0';
    } else {
      resetProbeCapture();
      g_probe_match = 0;
    }
    return;
  }

  if (b == (uint8_t)kEspProbePrefix[g_probe_match]) {
    g_probe_match++;
    if (g_probe_match >= prefix_len) {
      g_probe_capture = true;
      g_probe_tail_len = 0;
      g_probe_tail[0] = '\0';
    }
  } else {
    g_probe_match = (b == (uint8_t)kEspProbePrefix[0]) ? 1U : 0U;
  }
}

void handlePacket(const uint8_t* pkt, size_t len) {
  const size_t header_len = sizeof(telem::FrameHeader);
  if (len < header_len + sizeof(uint32_t)) {
    g_stats.len_err++;
    return;
  }

  telem::FrameHeader h;
  memcpy(&h, pkt, header_len);
  if (h.magic != telem::kMagic || h.version != telem::kVersion) {
    g_stats.unknown_msg++;
    return;
  }
  if (header_len + h.payload_len + sizeof(uint32_t) != len) {
    g_stats.len_err++;
    return;
  }

  uint32_t crc_rx = 0;
  memcpy(&crc_rx, pkt + header_len + h.payload_len, sizeof(uint32_t));
  const uint32_t crc_calc = crc32_calc(pkt, header_len + h.payload_len);
  if (crc_rx != crc_calc) {
    g_stats.crc_err++;
    return;
  }

  const uint8_t* pl = pkt + header_len;
  if (h.msg_type == telem::TELEM_FULL_STATE) {
    if (h.payload_len < 16U) {
      g_stats.len_err++;
      return;
    }
    telem::TelemetryFullStateV1 tmp = {};
    const size_t copy_len =
        (h.payload_len < sizeof(tmp)) ? (size_t)h.payload_len : sizeof(tmp);
    memcpy(&tmp, pl, copy_len);
    portENTER_CRITICAL(&g_mux);
    g_state = tmp;
    g_has_state = true;
    g_seq = h.seq;
    g_t_us = h.t_us;
    g_stats.frames_ok++;
    g_stats.last_rx_ms = millis();
    portEXIT_CRITICAL(&g_mux);
    log_store::enqueueState(h.seq, h.t_us, tmp);
    return;
  }

  if (h.msg_type == telem::TELEM_FUSION_SETTINGS) {
    if (h.payload_len != sizeof(telem::FusionSettingsV1)) {
      g_stats.len_err++;
      return;
    }
    telem::FusionSettingsV1 tmp = {};
    memcpy(&tmp, pl, sizeof(tmp));
    portENTER_CRITICAL(&g_mux);
    g_fusion_settings = tmp;
    g_has_fusion_settings = true;
    g_fusion_rx_seq = h.seq;
    if (g_has_state) {
      g_state.fusion_gain = tmp.gain;
      g_state.fusion_accel_rej = tmp.accelerationRejection;
      g_state.fusion_mag_rej = tmp.magneticRejection;
      g_state.fusion_recovery_period = tmp.recoveryTriggerPeriod;
    }
    g_stats.frames_ok++;
    g_stats.last_rx_ms = millis();
    portEXIT_CRITICAL(&g_mux);
    return;
  }

  if (h.msg_type == telem::ACK || h.msg_type == telem::NACK || h.msg_type == telem::TELEM_EVENT ||
      h.msg_type == telem::TELEM_META) {
    if ((h.msg_type == telem::ACK || h.msg_type == telem::NACK) &&
        h.payload_len >= sizeof(telem::AckPayloadV1)) {
      telem::AckPayloadV1 ack = {};
      memcpy(&ack, pl, sizeof(telem::AckPayloadV1));
      portENTER_CRITICAL(&g_mux);
      g_has_ack = true;
      g_ack_rx_seq = h.seq;
      g_ack_command = ack.command;
      g_ack_ok = (h.msg_type == telem::ACK) && (ack.ok != 0U);
      g_ack_code = ack.code;
      portEXIT_CRITICAL(&g_mux);
    }
    g_stats.frames_ok++;
    g_stats.last_rx_ms = millis();
    return;
  }

  g_stats.unknown_msg++;
}

}  // namespace

void begin(const AppConfig& cfg) {
  g_ser = (cfg.uart_port == 1) ? &g_serial1 : &g_serial2;
  g_ser->begin(cfg.uart_baud, SERIAL_8N1, cfg.uart_rx_pin, cfg.uart_tx_pin);
  g_raw_len = 0;
  g_probe_match = 0;
  resetProbeCapture();
  g_stats = {};
}

void reconfigure(const AppConfig& cfg) {
  begin(cfg);
}

void poll() {
  if (!g_ser) return;
  uint8_t decoded[DECODED_MAX];

  while (g_ser->available() > 0) {
    const int rv = g_ser->read();
    if (rv < 0) break;
    const uint8_t b = (uint8_t)rv;
    g_stats.rx_bytes++;
    probeSniffByte(b);

    if (b == 0) {
      if (g_raw_len == 0) continue;
      const size_t dec_len = cobsDecode(g_raw, g_raw_len, decoded, sizeof(decoded));
      if (dec_len == 0) {
        g_stats.cobs_err++;
      } else {
        handlePacket(decoded, dec_len);
      }
      g_raw_len = 0;
      continue;
    }

    if (g_raw_len >= sizeof(g_raw)) {
      g_stats.drop++;
      g_raw_len = 0;
      continue;
    }
    g_raw[g_raw_len++] = b;
  }
}

Snapshot snapshot() {
  Snapshot s = {};
  portENTER_CRITICAL(&g_mux);
  s.has_state = g_has_state;
  s.state = g_state;
  s.seq = g_seq;
  s.t_us = g_t_us;
  s.has_fusion_settings = g_has_fusion_settings;
  s.fusion_settings = g_fusion_settings;
  s.fusion_rx_seq = g_fusion_rx_seq;
  s.stats = g_stats;
  s.has_ack = g_has_ack;
  s.ack_rx_seq = g_ack_rx_seq;
  s.ack_command = g_ack_command;
  s.ack_ok = g_ack_ok;
  s.ack_code = g_ack_code;
  portEXIT_CRITICAL(&g_mux);
  return s;
}

LoopbackResult runLoopbackTest(uint32_t timeout_ms) {
  LoopbackResult r = {};
  if (!g_ser) return r;
  if (timeout_ms == 0U) timeout_ms = 120U;

  constexpr size_t kLen = 32;
  uint8_t tx[kLen];
  uint8_t rx[kLen];
  memset(rx, 0, sizeof(rx));
  for (size_t i = 0; i < kLen; ++i) {
    tx[i] = (uint8_t)((0xA5U + (uint8_t)(i * 17U)) ^ 0x5CU);
  }

  while (g_ser->available() > 0) {
    (void)g_ser->read();
  }
  g_ser->flush();

  r.sent = (uint32_t)g_ser->write(tx, kLen);
  g_ser->flush();
  const uint32_t t0 = millis();
  size_t got = 0;

  while (got < kLen && (uint32_t)(millis() - t0) < timeout_ms) {
    while (g_ser->available() > 0 && got < kLen) {
      const int c = g_ser->read();
      if (c >= 0) rx[got++] = (uint8_t)c;
    }
  }

  r.received = (uint32_t)got;
  r.elapsed_ms = (uint32_t)(millis() - t0);
  r.first_mismatch_index = 0xFFU;
  for (size_t i = 0; i < got && i < kLen; ++i) {
    if (rx[i] != tx[i]) {
      if (r.first_mismatch_index == 0xFFU) {
        r.first_mismatch_index = (uint8_t)i;
        r.expected = tx[i];
        r.actual = rx[i];
      }
      r.mismatches++;
    }
  }

  r.pass = (r.sent == kLen) && (r.received == kLen) && (r.mismatches == 0U);
  return r;
}

bool sendSetFusionSettings(const telem::CmdSetFusionSettingsV1& cmd) {
  return writeFramed(telem::CMD_SET_FUSION_SETTINGS, &cmd, sizeof(cmd));
}

bool sendGetFusionSettings() { return writeFramed(telem::CMD_GET_FUSION_SETTINGS, nullptr, 0); }

bool sendSetStreamRate(const telem::CmdSetStreamRateV1& cmd) {
  return writeFramed(telem::CMD_SET_STREAM_RATE, &cmd, sizeof(cmd));
}

bool probeRxPin(uint8_t rx_pin, uint32_t baud, uint32_t dwell_ms, uint32_t& out_bytes) {
  out_bytes = 0;
  if (dwell_ms == 0U) dwell_ms = 300U;

  // Preserve runtime telemetry state/counters while probing.
  Snapshot saved = snapshot();

  if (g_ser) {
    g_ser->flush();
    g_ser->end();
  }

  // Probe on UART1 with RX-only mapping (TX disabled).
  g_serial1.begin(baud, SERIAL_8N1, rx_pin, -1);
  delay(10);
  while (g_serial1.available() > 0) (void)g_serial1.read();

  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < dwell_ms) {
    while (g_serial1.available() > 0) {
      (void)g_serial1.read();
      out_bytes++;
    }
    delay(1);
  }
  g_serial1.end();

  // Restore configured telemetry UART.
  begin(config_store::get());
  portENTER_CRITICAL(&g_mux);
  g_has_state = saved.has_state;
  g_state = saved.state;
  g_seq = saved.seq;
  g_t_us = saved.t_us;
  g_stats = saved.stats;
  portEXIT_CRITICAL(&g_mux);
  return true;
}

}  // namespace uart_telem
