#include "mirror.h"
#include "config.h"
#include "imu_fusion.h"

namespace mirror {

namespace {
constexpr uint32_t kMagic = 0x54454C4DUL;  // "TELM"
constexpr uint16_t kVersion = 1U;
constexpr uint16_t kMsgTypeFullState = 1U;
constexpr uint16_t kMsgTypeFusionSettings = 4U;
constexpr uint16_t kMsgTypeSetFusion = 100U;
constexpr uint16_t kMsgTypeGetFusion = 101U;
constexpr uint16_t kMsgTypeSetStreamRate = 102U;
constexpr uint16_t kMsgTypeAck = 200U;
constexpr uint16_t kMsgTypeNack = 201U;
constexpr uint16_t kStateFlagGpsFix3d = 1U << 0;
constexpr uint16_t kStateFlagFusionInitialising = 1U << 1;
constexpr uint16_t kStateFlagFusionAngularRecovery = 1U << 2;
constexpr uint16_t kStateFlagFusionAccelerationRecovery = 1U << 3;
constexpr uint16_t kStateFlagFusionMagneticRecovery = 1U << 4;
constexpr uint16_t kStateFlagFusionAccelerationError = 1U << 5;
constexpr uint16_t kStateFlagFusionAccelerometerIgnored = 1U << 6;
constexpr uint16_t kStateFlagFusionMagneticError = 1U << 7;
constexpr uint16_t kStateFlagFusionMagnetometerIgnored = 1U << 8;
constexpr size_t kPacketMax = 512U;
constexpr size_t kRxMax = 768U;
constexpr uint16_t kDefaultStreamRateHz = 50U;
constexpr uint16_t kDefaultLogRateHz = 50U;
constexpr uint16_t kMinStreamRateHz = 1U;
constexpr uint16_t kMaxStreamRateHz = 400U;

#pragma pack(push, 1)
struct FrameHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t msg_type;
  uint16_t payload_len;
  uint16_t reserved;
  uint32_t seq;
  uint32_t t_us;
};

struct TelemetryFullStateV1 {
  float roll_deg;
  float pitch_deg;
  float yaw_deg;
  float mag_heading_deg;

  uint32_t iTOW_ms;
  uint8_t fixType;
  uint8_t numSV;
  int32_t lat_1e7;
  int32_t lon_1e7;
  int32_t hMSL_mm;
  int32_t gSpeed_mms;
  int32_t headMot_1e5deg;
  uint32_t hAcc_mm;
  uint32_t sAcc_mms;

  uint32_t gps_parse_errors;
  uint32_t mirror_tx_ok;
  uint32_t mirror_drop_count;

  uint32_t last_gps_ms;
  uint32_t last_imu_ms;
  uint32_t last_baro_ms;

  float baro_temp_c;
  float baro_press_hpa;
  float baro_alt_m;
  float baro_vsi_mps;

  float fusion_gain;
  float fusion_accel_rej;
  float fusion_mag_rej;
  uint16_t fusion_recovery_period;
  uint16_t flags;
};

struct CmdSetFusionSettingsV1 {
  float gain;
  float accelerationRejection;
  float magneticRejection;
  uint16_t recoveryTriggerPeriod;
  uint16_t reserved;
};

struct FusionSettingsV1 {
  float gain;
  float accelerationRejection;
  float magneticRejection;
  uint16_t recoveryTriggerPeriod;
  uint16_t reserved;
};

struct CmdSetStreamRateV1 {
  uint16_t ws_rate_hz;
  uint16_t log_rate_hz;
};

struct AckPayloadV1 {
  uint16_t command;
  uint16_t ok;
  uint32_t code;
};
#pragma pack(pop)

uint8_t g_rxRaw[kRxMax];
size_t g_rxLen = 0;
uint32_t g_txSeq = 1U;
RxDebugStats g_dbg = {};
uint16_t g_streamRateHz = kDefaultStreamRateHz;
uint16_t g_logRateHz = kDefaultLogRateHz;

uint16_t clampRateHz(uint16_t rateHz) {
  if (rateHz < kMinStreamRateHz) return kMinStreamRateHz;
  if (rateHz > kMaxStreamRateHz) return kMaxStreamRateHz;
  return rateHz;
}

uint32_t crc32Calc(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1U));
    }
  }
  return ~crc;
}

size_t cobsEncode(const uint8_t* in, size_t len, uint8_t* out, size_t outMax) {
  size_t readIndex = 0;
  size_t writeIndex = 1;
  size_t codeIndex = 0;
  uint8_t code = 1;

  while (readIndex < len) {
    if (in[readIndex] == 0) {
      if (codeIndex >= outMax) return 0;
      out[codeIndex] = code;
      code = 1;
      codeIndex = writeIndex++;
      if (writeIndex > outMax) return 0;
      readIndex++;
    } else {
      if (writeIndex >= outMax) return 0;
      out[writeIndex++] = in[readIndex++];
      code++;
      if (code == 0xFF) {
        if (codeIndex >= outMax) return 0;
        out[codeIndex] = code;
        code = 1;
        codeIndex = writeIndex++;
        if (writeIndex > outMax) return 0;
      }
    }
  }
  if (codeIndex >= outMax) return 0;
  out[codeIndex] = code;
  return writeIndex;
}

size_t cobsDecode(const uint8_t* in, size_t len, uint8_t* out, size_t outMax) {
  size_t ip = 0;
  size_t op = 0;
  while (ip < len) {
    const uint8_t code = in[ip++];
    if (code == 0U) return 0U;
    for (uint8_t i = 1U; i < code; ++i) {
      if (ip >= len || op >= outMax) return 0U;
      out[op++] = in[ip++];
    }
    if (code != 0xFFU && ip < len) {
      if (op >= outMax) return 0U;
      out[op++] = 0U;
    }
  }
  return op;
}

bool writeFrame(uint16_t msgType, const void* payload, uint16_t payloadLen) {
  uint8_t packet[kPacketMax];
  const size_t headerLen = sizeof(FrameHeader);
  const size_t plainLen = headerLen + payloadLen + sizeof(uint32_t);
  if (plainLen > sizeof(packet)) return false;

  FrameHeader hdr{};
  hdr.magic = kMagic;
  hdr.version = kVersion;
  hdr.msg_type = msgType;
  hdr.payload_len = payloadLen;
  hdr.reserved = 0U;
  hdr.seq = g_txSeq++;
  hdr.t_us = micros();

  memcpy(packet, &hdr, headerLen);
  if (payloadLen > 0U && payload) {
    memcpy(packet + headerLen, payload, payloadLen);
  }
  const uint32_t crc = crc32Calc(packet, headerLen + payloadLen);
  memcpy(packet + headerLen + payloadLen, &crc, sizeof(crc));

  uint8_t encoded[kPacketMax + 8U];
  const size_t encLen = cobsEncode(packet, plainLen, encoded, sizeof(encoded) - 1U);
  if (encLen == 0U) return false;
  encoded[encLen] = 0U;
  const size_t txLen = encLen + 1U;
  return MIRROR_SERIAL.write(encoded, txLen) == txLen;
}

void sendAckFor(uint16_t command, bool ok, uint32_t code) {
  AckPayloadV1 ack{};
  ack.command = command;
  ack.ok = ok ? 1U : 0U;
  ack.code = code;
  if (ok) g_dbg.ackSent++;
  else g_dbg.nackSent++;
  (void)writeFrame(ok ? kMsgTypeAck : kMsgTypeNack, &ack, (uint16_t)sizeof(ack));
}

bool sendFusionSettingsFrame() {
  FusionSettingsV1 payload{};
  imu_fusion::getFusionSettings(payload.gain, payload.accelerationRejection, payload.magneticRejection,
                                payload.recoveryTriggerPeriod);
  payload.reserved = 0U;
  return writeFrame(kMsgTypeFusionSettings, &payload, (uint16_t)sizeof(payload));
}

void handlePacket(const uint8_t* pkt, size_t len) {
  const size_t headerLen = sizeof(FrameHeader);
  if (len < headerLen + sizeof(uint32_t)) {
    g_dbg.lenErr++;
    return;
  }

  FrameHeader hdr{};
  memcpy(&hdr, pkt, headerLen);
  if (hdr.magic != kMagic || hdr.version != kVersion) {
    g_dbg.unknownMsg++;
    return;
  }
  g_dbg.lastMsgType = hdr.msg_type;
  if (headerLen + hdr.payload_len + sizeof(uint32_t) != len) {
    g_dbg.lenErr++;
    return;
  }

  uint32_t crcRx = 0U;
  memcpy(&crcRx, pkt + headerLen + hdr.payload_len, sizeof(uint32_t));
  const uint32_t crcCalc = crc32Calc(pkt, headerLen + hdr.payload_len);
  if (crcRx != crcCalc) {
    g_dbg.crcErr++;
    return;
  }
  g_dbg.framesOk++;

  const uint8_t* pl = pkt + headerLen;
  if (hdr.msg_type == kMsgTypeSetFusion) {
    g_dbg.cmdSetFusion++;
    if (hdr.payload_len != sizeof(CmdSetFusionSettingsV1)) {
      sendAckFor(kMsgTypeSetFusion, false, 1U);
      return;
    }
    CmdSetFusionSettingsV1 cmd{};
    memcpy(&cmd, pl, sizeof(cmd));
    const bool ok = imu_fusion::setFusionSettings(
        cmd.gain, cmd.accelerationRejection, cmd.magneticRejection, cmd.recoveryTriggerPeriod);
    sendAckFor(kMsgTypeSetFusion, ok, ok ? 0U : 2U);
    if (ok) {
      (void)sendFusionSettingsFrame();
    }
    return;
  }

  if (hdr.msg_type == kMsgTypeGetFusion) {
    g_dbg.cmdGetFusion++;
    sendAckFor(kMsgTypeGetFusion, true, 0U);
    (void)sendFusionSettingsFrame();
    return;
  }
  if (hdr.msg_type == kMsgTypeSetStreamRate) {
    g_dbg.cmdSetStreamRate++;
    if (hdr.payload_len != sizeof(CmdSetStreamRateV1)) {
      sendAckFor(kMsgTypeSetStreamRate, false, 1U);
      return;
    }
    CmdSetStreamRateV1 cmd{};
    memcpy(&cmd, pl, sizeof(cmd));
    g_streamRateHz = clampRateHz(cmd.ws_rate_hz);
    g_logRateHz = clampRateHz(cmd.log_rate_hz);
    sendAckFor(kMsgTypeSetStreamRate, true, 0U);
    return;
  }
  g_dbg.unknownMsg++;
}
}  // namespace

void begin() {
#if ENABLE_MIRROR
  MIRROR_SERIAL.begin(MIRROR_BAUD);
#endif
  g_streamRateHz = kDefaultStreamRateHz;
  g_logRateHz = kDefaultLogRateHz;
}

uint16_t crc16Ccitt(const uint8_t* data, uint16_t len) {
  uint16_t crc = 0xFFFF;
  for (uint16_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; ++b) {
      if (crc & 0x8000) {
        crc = (uint16_t)((crc << 1) ^ 0x1021);
      } else {
        crc <<= 1;
      }
    }
  }
  return crc;
}

bool sendFastState(const State& s, uint32_t seq, uint32_t t_us) {
#if !ENABLE_MIRROR
  (void)s;
  (void)seq;
  (void)t_us;
  return false;
#else
  TelemetryFullStateV1 payload{};
  payload.roll_deg = s.roll;
  payload.pitch_deg = s.pitch;
  payload.yaw_deg = s.yaw;
  payload.mag_heading_deg = s.mag_heading;
  payload.iTOW_ms = s.iTOW;
  payload.fixType = s.fixType;
  payload.numSV = s.numSV;
  payload.lat_1e7 = s.lat;
  payload.lon_1e7 = s.lon;
  payload.hMSL_mm = s.hMSL;
  payload.gSpeed_mms = s.gSpeed;
  payload.headMot_1e5deg = s.headMot;
  payload.hAcc_mm = s.hAcc;
  payload.sAcc_mms = s.sAcc;
  payload.gps_parse_errors = s.gps_parse_errors;
  payload.mirror_tx_ok = s.mirror_tx_ok;
  payload.mirror_drop_count = s.mirror_drop_count;
  payload.last_gps_ms = s.last_gps_ms;
  payload.last_imu_ms = s.last_imu_ms;
  payload.last_baro_ms = s.last_baro_ms;
  payload.baro_temp_c = s.baro_temp_c;
  payload.baro_press_hpa = s.baro_press_hpa;
  payload.baro_alt_m = s.baro_alt_m;
  payload.baro_vsi_mps = s.baro_vsi_mps;
  imu_fusion::getFusionSettings(payload.fusion_gain, payload.fusion_accel_rej, payload.fusion_mag_rej,
                                payload.fusion_recovery_period);
  bool fusionInitialising = false;
  bool fusionAngularRecovery = false;
  bool fusionAccelerationRecovery = false;
  bool fusionMagneticRecovery = false;
  bool fusionAccelerationError = false;
  bool fusionAccelerometerIgnored = false;
  bool fusionMagneticError = false;
  bool fusionMagnetometerIgnored = false;
  imu_fusion::getFusionFlags(
      fusionInitialising, fusionAngularRecovery, fusionAccelerationRecovery, fusionMagneticRecovery);
  imu_fusion::getFusionHealthFlags(
      fusionAccelerationError, fusionAccelerometerIgnored, fusionMagneticError, fusionMagnetometerIgnored);
  payload.flags = 0U;
  if (s.fixType >= 3U) payload.flags |= kStateFlagGpsFix3d;
  if (fusionInitialising) payload.flags |= kStateFlagFusionInitialising;
  if (fusionAngularRecovery) payload.flags |= kStateFlagFusionAngularRecovery;
  if (fusionAccelerationRecovery) payload.flags |= kStateFlagFusionAccelerationRecovery;
  if (fusionMagneticRecovery) payload.flags |= kStateFlagFusionMagneticRecovery;
  if (fusionAccelerationError) payload.flags |= kStateFlagFusionAccelerationError;
  if (fusionAccelerometerIgnored) payload.flags |= kStateFlagFusionAccelerometerIgnored;
  if (fusionMagneticError) payload.flags |= kStateFlagFusionMagneticError;
  if (fusionMagnetometerIgnored) payload.flags |= kStateFlagFusionMagnetometerIgnored;

  FrameHeader hdr{};
  hdr.magic = kMagic;
  hdr.version = kVersion;
  hdr.msg_type = kMsgTypeFullState;
  hdr.payload_len = (uint16_t)sizeof(payload);
  hdr.reserved = 0;
  hdr.seq = seq;
  hdr.t_us = t_us;

  uint8_t packet[kPacketMax];
  const size_t headerLen = sizeof(hdr);
  const size_t payloadLen = sizeof(payload);
  const size_t plainLen = headerLen + payloadLen + sizeof(uint32_t);
  if (plainLen > sizeof(packet)) {
    return false;
  }
  memcpy(packet, &hdr, headerLen);
  memcpy(packet + headerLen, &payload, payloadLen);
  const uint32_t crc = crc32Calc(packet, headerLen + payloadLen);
  memcpy(packet + headerLen + payloadLen, &crc, sizeof(crc));

  uint8_t encoded[kPacketMax + 8U];
  const size_t encLen = cobsEncode(packet, plainLen, encoded, sizeof(encoded) - 1U);
  if (encLen == 0U) return false;
  encoded[encLen] = 0U;
  const size_t txLen = encLen + 1U;
  const size_t written = MIRROR_SERIAL.write(encoded, txLen);
  return written == txLen;
#endif
}

void pollRx() {
#if !ENABLE_MIRROR
  return;
#else
  uint8_t decoded[kRxMax];
  while (MIRROR_SERIAL.available() > 0) {
    const int rv = MIRROR_SERIAL.read();
    if (rv < 0) break;
    const uint8_t b = (uint8_t)rv;
    g_dbg.rxBytes++;

    if (b == 0U) {
      if (g_rxLen == 0U) continue;
      const size_t decLen = cobsDecode(g_rxRaw, g_rxLen, decoded, sizeof(decoded));
      if (decLen > 0U) {
        handlePacket(decoded, decLen);
      } else {
        g_dbg.cobsErr++;
      }
      g_rxLen = 0U;
      continue;
    }

    if (g_rxLen >= sizeof(g_rxRaw)) {
      g_rxLen = 0U;
      continue;
    }
    g_rxRaw[g_rxLen++] = b;
  }
#endif
}

RxDebugStats getRxDebugStats() {
  return g_dbg;
}

uint16_t streamRateHz() {
  return g_streamRateHz;
}

uint16_t logRateHz() {
  return g_logRateHz;
}

uint32_t streamPeriodUs() {
  const uint32_t rateHz = (uint32_t)(g_streamRateHz ? g_streamRateHz : kDefaultStreamRateHz);
  return 1000000UL / rateHz;
}

}  // namespace mirror
