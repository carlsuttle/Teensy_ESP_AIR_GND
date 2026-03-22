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

RxDebugStats g_dbg = {};
uint16_t g_streamRateHz = kDefaultStreamRateHz;
uint16_t g_logRateHz = kDefaultLogRateHz;

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

}  // namespace

void begin() {
  g_dbg = {};
  g_streamRateHz = kDefaultStreamRateHz;
  g_logRateHz = kDefaultLogRateHz;
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

bool sendFastState(const State& s, uint32_t seq, uint32_t t_us) {
#if !ENABLE_MIRROR
  (void)s;
  (void)seq;
  (void)t_us;
  return false;
#else
  (void)seq;
  (void)t_us;
  telem::TelemetryFullStateV1 payload = {};
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
  payload.accel_x_mps2 = s.accel_x_mps2;
  payload.accel_y_mps2 = s.accel_y_mps2;
  payload.accel_z_mps2 = s.accel_z_mps2;
  payload.gyro_x_dps = s.gyro_x_dps;
  payload.gyro_y_dps = s.gyro_y_dps;
  payload.gyro_z_dps = s.gyro_z_dps;
  payload.mag_x_uT = s.mag_x_uT;
  payload.mag_y_uT = s.mag_y_uT;
  payload.mag_z_uT = s.mag_z_uT;
  if (s.last_imu_ms != 0U) payload.raw_present_mask |= telem::kSensorPresentImu;
  if (s.last_imu_ms != 0U) payload.raw_present_mask |= telem::kSensorPresentMag;
  if (s.last_gps_ms != 0U) payload.raw_present_mask |= telem::kSensorPresentGps;
  if (s.last_baro_ms != 0U) payload.raw_present_mask |= telem::kSensorPresentBaro;

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

  return spi_bridge::pushStateRecord(reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
#endif
}

void pollRx() {
#if !ENABLE_MIRROR
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



