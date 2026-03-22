#include "mirror.h"

#include <string.h>

#include "imu_fusion.h"
#include "spi_bridge.h"
#include "types_shared.h"

namespace mirror {
namespace {

constexpr uint16_t kDefaultStreamRateHz = 50U;
constexpr uint16_t kDefaultLogRateHz = 50U;
constexpr uint16_t kMinStreamRateHz = 1U;
constexpr uint16_t kMaxStreamRateHz = 400U;
constexpr size_t kRecordBytes = sizeof(telem::TelemetryFullStateV1);
constexpr uint32_t kReplayIdleTimeoutMs = 250U;
constexpr uint8_t kReplayOutputQueueDepth = 64U;

struct ReplayInputMetaV1 {
  uint32_t last_gps_ms;
  uint32_t last_imu_ms;
  uint32_t last_baro_ms;
};

RxDebugStats g_dbg = {};
uint16_t g_streamRateHz = kDefaultStreamRateHz;
uint16_t g_logRateHz = kDefaultLogRateHz;
bool g_replayActive = false;
uint32_t g_lastReplayRxMs = 0U;
ReplayOutputMeta g_replayOutputQueue[kReplayOutputQueueDepth] = {};
uint8_t g_replayOutputHead = 0U;
uint8_t g_replayOutputTail = 0U;
uint8_t g_replayOutputCount = 0U;

int16_t quantizeSigned(float value, float scale) {
  if (!isfinite(value)) return 0;
  const float scaled = value * scale;
  if (scaled > 32767.0f) return 32767;
  if (scaled < -32768.0f) return -32768;
  return (int16_t)lroundf(scaled);
}

uint16_t quantizeUnsigned(float value, float scale) {
  if (!isfinite(value) || value <= 0.0f) return 0U;
  const float scaled = value * scale;
  if (scaled > 65535.0f) return 65535U;
  return (uint16_t)lroundf(scaled);
}

void packFusionReplayDiagnostics(telem::TelemetryFullStateV1& payload,
                                 const imu_fusion::FusionReplayDebug* dbg_ptr) {
  imu_fusion::FusionReplayDebug dbg{};
  if (dbg_ptr) {
    dbg = *dbg_ptr;
  } else {
    imu_fusion::getFusionReplayDebug(dbg);
  }
  payload.reserved0 = quantizeUnsigned(dbg.magneticErrorDeg, 100.0f);

  int16_t packed[7] = {};
  packed[0] = quantizeSigned(dbg.accelBodyX, 1000.0f);
  packed[1] = quantizeSigned(dbg.accelBodyY, 1000.0f);
  packed[2] = quantizeSigned(dbg.accelBodyZ, 1000.0f);
  packed[3] = quantizeSigned(dbg.magFusionX, 100.0f);
  packed[4] = quantizeSigned(dbg.magFusionY, 100.0f);
  packed[5] = quantizeSigned(dbg.magFusionZ, 100.0f);
  packed[6] = (int16_t)quantizeUnsigned(dbg.accelerationErrorDeg, 100.0f);
  memcpy(payload.reserved1, packed, sizeof(packed));
}

uint16_t clampRateHz(uint16_t rateHz) {
  if (rateHz < kMinStreamRateHz) return kMinStreamRateHz;
  if (rateHz > kMaxStreamRateHz) return kMaxStreamRateHz;
  return rateHz;
}

void applyFusionSettings(const telem::CmdSetFusionSettingsV1& cmd) {
  g_dbg.cmdSetFusion++;
  (void)imu_fusion::setFusionSettings(
      cmd.gain, cmd.accelerationRejection, cmd.magneticRejection, cmd.recoveryTriggerPeriod);
}

void applyStreamRate(const telem::CmdSetStreamRateV1& cmd) {
  g_dbg.cmdSetStreamRate++;
  g_streamRateHz = clampRateHz(cmd.ws_rate_hz);
  g_logRateHz = clampRateHz(cmd.log_rate_hz);
}

void handleReplayControl(const telem::ReplayControlRecord160& replay) {
  const uint8_t* payload = replay.payload.payload;
  switch (replay.payload.command_id) {
    case telem::CMD_SET_FUSION_SETTINGS: {
      if (replay.payload.payload_len < sizeof(telem::CmdSetFusionSettingsV1)) {
        g_dbg.lenErr++;
        return;
      }
      telem::CmdSetFusionSettingsV1 cmd = {};
      memcpy(&cmd, payload, sizeof(cmd));
      applyFusionSettings(cmd);
      return;
    }
    case telem::CMD_GET_FUSION_SETTINGS:
      g_dbg.cmdGetFusion++;
      return;
    case telem::CMD_SET_STREAM_RATE: {
      if (replay.payload.payload_len < sizeof(telem::CmdSetStreamRateV1)) {
        g_dbg.lenErr++;
        return;
      }
      telem::CmdSetStreamRateV1 cmd = {};
      memcpy(&cmd, payload, sizeof(cmd));
      applyStreamRate(cmd);
      return;
    }
    default:
      g_dbg.unknownMsg++;
      return;
  }
}

void setReplayActive(bool active) {
  if (g_replayActive == active) return;
  g_replayActive = active;
  imu_fusion::setReplayMode(active);
  if (!active) {
    g_replayOutputHead = 0U;
    g_replayOutputTail = 0U;
    g_replayOutputCount = 0U;
  }
}

void queueReplayOutputMeta(const ReplayOutputMeta& meta) {
  if (g_replayOutputCount >= kReplayOutputQueueDepth) {
    g_replayOutputTail = (uint8_t)((g_replayOutputTail + 1U) % kReplayOutputQueueDepth);
    g_replayOutputCount--;
  }
  g_replayOutputQueue[g_replayOutputHead] = meta;
  g_replayOutputHead = (uint8_t)((g_replayOutputHead + 1U) % kReplayOutputQueueDepth);
  g_replayOutputCount++;
}

void applyReplayInput(State& s, const telem::ReplayInputRecord160& replay) {
  const telem::ReplayInputPayloadV1& p = replay.payload;
  ReplayInputMetaV1 meta = {};
  memcpy(&meta, p.reserved, sizeof(meta));
  const uint32_t now_ms = millis();
  g_lastReplayRxMs = now_ms;
  setReplayActive(true);

  if ((p.present_mask & (telem::kSensorPresentImu | telem::kSensorPresentMag)) != 0U) {
    (void)imu_fusion::submitReplaySample(
        (float)p.accel_milli_mps2[0] * 0.001f,
        (float)p.accel_milli_mps2[1] * 0.001f,
        (float)p.accel_milli_mps2[2] * 0.001f,
        (float)p.gyro_milli_dps[0] * 0.001f,
        (float)p.gyro_milli_dps[1] * 0.001f,
        (float)p.gyro_milli_dps[2] * 0.001f,
        (float)p.mag_milli_uT[0] * 0.001f,
        (float)p.mag_milli_uT[1] * 0.001f,
        (float)p.mag_milli_uT[2] * 0.001f,
        replay.hdr.t_us);
  }

  if ((p.present_mask & telem::kSensorPresentGps) != 0U) {
    s.iTOW = p.iTOW_ms;
    s.fixType = p.fixType;
    s.numSV = p.numSV;
    s.lat = p.lat_1e7;
    s.lon = p.lon_1e7;
    s.hMSL = p.hMSL_mm;
    s.gSpeed = p.gSpeed_mms;
    s.headMot = p.headMot_1e5deg;
    s.hAcc = p.hAcc_mm;
    s.sAcc = p.sAcc_mms;
    s.last_gps_ms = now_ms;
  }

  if ((p.present_mask & telem::kSensorPresentBaro) != 0U) {
    s.baro_temp_c = (float)p.baro_temp_milli_c * 0.001f;
    s.baro_press_hpa = (float)p.baro_press_milli_hpa * 0.001f;
    s.baro_alt_m = (float)p.baro_alt_mm * 0.001f;
    s.baro_vsi_mps = (float)p.baro_vsi_milli_mps * 0.001f;
    s.last_baro_ms = now_ms;
  }

  ReplayOutputMeta out = {};
  out.seq = replay.hdr.seq;
  out.t_us = replay.hdr.t_us;
  out.last_gps_ms = meta.last_gps_ms;
  out.last_imu_ms = meta.last_imu_ms;
  out.last_baro_ms = meta.last_baro_ms;
  out.present_mask = p.present_mask;
  out.iTOW_ms = p.iTOW_ms;
  out.fixType = p.fixType;
  out.numSV = p.numSV;
  out.lat_1e7 = p.lat_1e7;
  out.lon_1e7 = p.lon_1e7;
  out.hMSL_mm = p.hMSL_mm;
  out.gSpeed_mms = p.gSpeed_mms;
  out.headMot_1e5deg = p.headMot_1e5deg;
  out.hAcc_mm = p.hAcc_mm;
  out.sAcc_mms = p.sAcc_mms;
  out.accel_x_mps2 = (float)p.accel_milli_mps2[0] * 0.001f;
  out.accel_y_mps2 = (float)p.accel_milli_mps2[1] * 0.001f;
  out.accel_z_mps2 = (float)p.accel_milli_mps2[2] * 0.001f;
  out.gyro_x_dps = (float)p.gyro_milli_dps[0] * 0.001f;
  out.gyro_y_dps = (float)p.gyro_milli_dps[1] * 0.001f;
  out.gyro_z_dps = (float)p.gyro_milli_dps[2] * 0.001f;
  out.mag_x_uT = (float)p.mag_milli_uT[0] * 0.001f;
  out.mag_y_uT = (float)p.mag_milli_uT[1] * 0.001f;
  out.mag_z_uT = (float)p.mag_milli_uT[2] * 0.001f;
  out.baro_temp_c = (float)p.baro_temp_milli_c * 0.001f;
  out.baro_press_hpa = (float)p.baro_press_milli_hpa * 0.001f;
  out.baro_alt_m = (float)p.baro_alt_mm * 0.001f;
  out.baro_vsi_mps = (float)p.baro_vsi_milli_mps * 0.001f;
  queueReplayOutputMeta(out);
}

}  // namespace

void begin() {
  g_dbg = {};
  g_streamRateHz = kDefaultStreamRateHz;
  g_logRateHz = kDefaultLogRateHz;
  g_replayActive = false;
  g_lastReplayRxMs = 0U;
  spi_bridge::begin();
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

bool sendFastState(const State& s, uint32_t seq, uint32_t t_us,
                   const ReplayOutputMeta* replay_meta,
                   const imu_fusion::FusionReplayDebug* replay_diag) {
#if !ENABLE_MIRROR
  (void)s;
  (void)seq;
  (void)t_us;
  (void)replay_meta;
  return false;
#else
  (void)seq;
  (void)t_us;
  telem::TelemetryFullStateV1 payload = {};
  payload.roll_deg = s.roll;
  payload.pitch_deg = s.pitch;
  payload.yaw_deg = s.yaw;
  payload.mag_heading_deg = s.mag_heading;
  payload.iTOW_ms = replay_meta ? replay_meta->iTOW_ms : s.iTOW;
  payload.fixType = replay_meta ? replay_meta->fixType : s.fixType;
  payload.numSV = replay_meta ? replay_meta->numSV : s.numSV;
  payload.lat_1e7 = replay_meta ? replay_meta->lat_1e7 : s.lat;
  payload.lon_1e7 = replay_meta ? replay_meta->lon_1e7 : s.lon;
  payload.hMSL_mm = replay_meta ? replay_meta->hMSL_mm : s.hMSL;
  payload.gSpeed_mms = replay_meta ? replay_meta->gSpeed_mms : s.gSpeed;
  payload.headMot_1e5deg = replay_meta ? replay_meta->headMot_1e5deg : s.headMot;
  payload.hAcc_mm = replay_meta ? replay_meta->hAcc_mm : s.hAcc;
  payload.sAcc_mms = replay_meta ? replay_meta->sAcc_mms : s.sAcc;
  payload.gps_parse_errors = s.gps_parse_errors;
  payload.mirror_tx_ok = s.mirror_tx_ok;
  payload.mirror_drop_count = s.mirror_drop_count;
  payload.last_gps_ms = replay_meta ? replay_meta->last_gps_ms : s.last_gps_ms;
  payload.last_imu_ms = replay_meta ? replay_meta->last_imu_ms : s.last_imu_ms;
  payload.last_baro_ms = replay_meta ? replay_meta->last_baro_ms : s.last_baro_ms;
  payload.baro_temp_c = replay_meta ? replay_meta->baro_temp_c : s.baro_temp_c;
  payload.baro_press_hpa = replay_meta ? replay_meta->baro_press_hpa : s.baro_press_hpa;
  payload.baro_alt_m = replay_meta ? replay_meta->baro_alt_m : s.baro_alt_m;
  payload.baro_vsi_mps = replay_meta ? replay_meta->baro_vsi_mps : s.baro_vsi_mps;
  imu_fusion::getFusionSettings(payload.fusion_gain, payload.fusion_accel_rej, payload.fusion_mag_rej,
                                payload.fusion_recovery_period);
  payload.accel_x_mps2 = replay_meta ? replay_meta->accel_x_mps2 : s.accel_x_mps2;
  payload.accel_y_mps2 = replay_meta ? replay_meta->accel_y_mps2 : s.accel_y_mps2;
  payload.accel_z_mps2 = replay_meta ? replay_meta->accel_z_mps2 : s.accel_z_mps2;
  payload.gyro_x_dps = replay_meta ? replay_meta->gyro_x_dps : s.gyro_x_dps;
  payload.gyro_y_dps = replay_meta ? replay_meta->gyro_y_dps : s.gyro_y_dps;
  payload.gyro_z_dps = replay_meta ? replay_meta->gyro_z_dps : s.gyro_z_dps;
  payload.mag_x_uT = replay_meta ? replay_meta->mag_x_uT : s.mag_x_uT;
  payload.mag_y_uT = replay_meta ? replay_meta->mag_y_uT : s.mag_y_uT;
  payload.mag_z_uT = replay_meta ? replay_meta->mag_z_uT : s.mag_z_uT;
  if (replay_meta) {
    payload.raw_present_mask = (uint16_t)replay_meta->present_mask;
  } else {
    if (s.last_imu_ms != 0U) payload.raw_present_mask |= telem::kSensorPresentImu;
    if (s.last_imu_ms != 0U) payload.raw_present_mask |= telem::kSensorPresentMag;
    if (s.last_gps_ms != 0U) payload.raw_present_mask |= telem::kSensorPresentGps;
    if (s.last_baro_ms != 0U) payload.raw_present_mask |= telem::kSensorPresentBaro;
  }

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

  if (s.fixType >= 3U) payload.flags |= telem::kStateFlagGpsFix3d;
  if (fusionInitialising) payload.flags |= telem::kStateFlagFusionInitialising;
  if (fusionAngularRecovery) payload.flags |= telem::kStateFlagFusionAngularRecovery;
  if (fusionAccelerationRecovery) payload.flags |= telem::kStateFlagFusionAccelerationRecovery;
  if (fusionMagneticRecovery) payload.flags |= telem::kStateFlagFusionMagneticRecovery;
  if (fusionAccelerationError) payload.flags |= telem::kStateFlagFusionAccelerationError;
  if (fusionAccelerometerIgnored) payload.flags |= telem::kStateFlagFusionAccelerometerIgnored;
  if (fusionMagneticError) payload.flags |= telem::kStateFlagFusionMagneticError;
  if (fusionMagnetometerIgnored) payload.flags |= telem::kStateFlagFusionMagnetometerIgnored;
  packFusionReplayDiagnostics(payload, replay_diag);

  return spi_bridge::pushStateRecord(reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
#endif
}

void pollRx(State& s) {
#if !ENABLE_MIRROR
  (void)s;
  return;
#else
  spi_bridge::poll();
  uint8_t record[kRecordBytes] = {};
  while (spi_bridge::popReplayRecord(record, sizeof(record))) {
    g_dbg.rxBytes += sizeof(record);
    g_dbg.framesOk++;

    telem::ReplayRecordHeaderV1 hdr = {};
    memcpy(&hdr, record, sizeof(hdr));
    if (hdr.magic != telem::kReplayMagic || hdr.version != telem::kReplayVersion) {
      g_dbg.unknownMsg++;
      continue;
    }

    if (hdr.kind == (uint8_t)telem::ReplayRecordKind::Input) {
      g_dbg.replayInputFrames++;
      telem::ReplayInputRecord160 replay = {};
      memcpy(&replay, record, sizeof(replay));
      applyReplayInput(s, replay);
      continue;
    }

    if (hdr.kind == (uint8_t)telem::ReplayRecordKind::Control) {
      g_dbg.replayControlFrames++;
      telem::ReplayControlRecord160 replay = {};
      memcpy(&replay, record, sizeof(replay));
      handleReplayControl(replay);
      continue;
    }

    g_dbg.unknownMsg++;
  }

  if (g_replayActive && g_lastReplayRxMs != 0U && (uint32_t)(millis() - g_lastReplayRxMs) > kReplayIdleTimeoutMs) {
    g_lastReplayRxMs = 0U;
    setReplayActive(false);
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

bool replayActive() {
  return g_replayActive;
}

bool takeReplayOutputMeta(ReplayOutputMeta& out) {
  if (g_replayOutputCount == 0U) return false;
  out = g_replayOutputQueue[g_replayOutputTail];
  g_replayOutputTail = (uint8_t)((g_replayOutputTail + 1U) % kReplayOutputQueueDepth);
  g_replayOutputCount--;
  return true;
}

}  // namespace mirror



