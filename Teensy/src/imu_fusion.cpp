#include "imu_fusion.h"
#include "config.h"

#include <EEPROM.h>
#include <math.h>
#include <stddef.h>
#include <Wire.h>
#include <BMI2_BMM1.h>
#include <Fusion.h>
#include <FusionAxes.h>
#include <FusionCompass.h>

namespace imu_fusion {
bool readRawAccelGyro(float out6[6]);
bool readRawMag(float& mx, float& my, float& mz);

namespace {
BMI2_BMM1_Class g_imu(Wire);
bool g_ready = false;
bool g_replayMode = false;

FusionOffset g_offset;
FusionAhrs g_ahrs;
constexpr float kDefaultFusionGain = 0.06f;
constexpr float kDefaultFusionAccelRejectDeg = 20.0f;
constexpr float kDefaultFusionMagRejectDeg = 60.0f;
constexpr uint16_t kDefaultFusionRecoverySamples = 1200U;
constexpr int kFusionSettingsAddr = 256;
constexpr uint32_t kFusionSettingsMagic = 0x46555331UL;  // "FUS1"
constexpr uint16_t kFusionSettingsVersion = 1U;
FusionAhrsSettings g_settings = {
    .convention = FusionConventionNed,
    .gain = kDefaultFusionGain,
    .gyroscopeRange = 250.0f,
    .accelerationRejection = kDefaultFusionAccelRejectDeg,
    .magneticRejection = kDefaultFusionMagRejectDeg,
    .recoveryTriggerPeriod = kDefaultFusionRecoverySamples,
};

struct PersistedFusionSettings {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  float gain;
  float accelerationRejection;
  float magneticRejection;
  uint16_t recoveryTriggerPeriod;
  uint16_t reserved;
  uint32_t checksum;
};

const FusionMatrix kGyroMisalignment = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
const FusionVector kGyroSensitivity = {1.0f, 1.0f, 1.0f};
FusionVector g_gyroOffset = {0.0f, 0.0f, 0.0f};

const FusionMatrix kAccelMisalignment = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
const FusionVector kAccelSensitivity = {1.0f, 1.0f, 1.0f};
FusionVector g_accelOffset = {0.0f, 0.0f, 0.0f};
float g_accelScale = 1.0f;

const FusionMatrix kSoftIronMatrix = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
FusionVector g_hardIronOffset = {0.0f, 0.0f, 0.0f};

float g_mx = 0.0f;
float g_my = 0.0f;
float g_mz = 0.0f;
FusionVector g_last_accel_body = {0.0f, 0.0f, 1.0f};
FusionVector g_last_mag_body = {1.0f, 0.0f, 0.0f};
FusionVector g_last_mag_fusion = {1.0f, 0.0f, 0.0f};
DebugMagMode g_debugMagMode = DebugMagMode::Live;
FusionVector g_debugSyntheticEarthMag = {1.0f, 0.0f, 0.5f};
uint32_t g_sampleRate = 100;
constexpr float kGravityMps2 = 9.80665f;
constexpr float kFusionDtSec = 0.0025f;  // 400 Hz
constexpr uint32_t kFusionPeriodUs = 2500U;
constexpr uint8_t kAccelLeastFilteredBwp = BMI2_ACC_OSR4_AVG1;
constexpr uint8_t kAccelLeastFilteredPerf = 1U;
constexpr uint8_t kGyroLeastFilteredBwp = BMI2_GYR_OSR4_MODE;
constexpr uint8_t kGyroLeastFilteredNoisePerf = 1U;
constexpr uint8_t kGyroLeastFilteredPerf = 1U;
constexpr float kMinFusionDtSec = 0.0005f;
constexpr float kMaxFusionDtSec = 0.05f;
float g_accLsbPerG = 16384.0f;
float g_gyrLsbPerDps = 16.384f;
constexpr FusionAxesAlignment kSensorToBodyAlignment = FusionAxesAlignmentPXNYNZ; // sensor X fwd, Y left, Z up -> body X fwd, Y right, Z down

uint32_t crc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = -(int32_t)(crc & 1U);
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

uint32_t fusionSettingsChecksum(const PersistedFusionSettings& cfg) {
  return crc32(reinterpret_cast<const uint8_t*>(&cfg), offsetof(PersistedFusionSettings, checksum));
}

void initDefaultFusionSettings(PersistedFusionSettings& cfg) {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = kFusionSettingsMagic;
  cfg.version = kFusionSettingsVersion;
  cfg.size = sizeof(PersistedFusionSettings);
  cfg.gain = kDefaultFusionGain;
  cfg.accelerationRejection = kDefaultFusionAccelRejectDeg;
  cfg.magneticRejection = kDefaultFusionMagRejectDeg;
  cfg.recoveryTriggerPeriod = kDefaultFusionRecoverySamples;
}

void applyFusionSettings(float gain, float accelRejection, float magRejection, uint16_t recoveryPeriod) {
  g_settings.gain = gain;
  g_settings.accelerationRejection = accelRejection;
  g_settings.magneticRejection = magRejection;
  g_settings.recoveryTriggerPeriod = recoveryPeriod;
  FusionAhrsSetSettings(&g_ahrs, &g_settings);
}

struct ImuFrame {
  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  float gx = 0.0f;
  float gy = 0.0f;
  float gz = 0.0f;
  float mx = 0.0f;
  float my = 0.0f;
  float mz = 0.0f;
  uint32_t t_us = 0U;
  bool valid = false;
  bool processed_body = false;
};

constexpr uint8_t kImuFrameQueueDepth = 4;
constexpr uint8_t kReplayDebugQueueDepth = 64;
constexpr bool kUseFrameAveraging = true;
constexpr bool kUseGyroOffsetFilter = true;
ImuFrame g_frameQueue[kImuFrameQueueDepth];
uint8_t g_frameHead = 0;
uint8_t g_frameTail = 0;
uint8_t g_frameCount = 0;
FusionReplayDebug g_replayDebugQueue[kReplayDebugQueueDepth] = {};
uint8_t g_replayDebugHead = 0U;
uint8_t g_replayDebugTail = 0U;
uint8_t g_replayDebugCount = 0U;
uint32_t g_nextReadUs = 0;
uint32_t g_nextFusionUs = 0;
uint32_t g_lastFusionUpdateUs = 0;

void clearFrameQueue() {
  g_frameHead = 0U;
  g_frameTail = 0U;
  g_frameCount = 0U;
}

void clearReplayDebugQueue() {
  g_replayDebugHead = 0U;
  g_replayDebugTail = 0U;
  g_replayDebugCount = 0U;
}

void queueReplayDebug() {
  FusionReplayDebug dbg = {};
  const FusionAhrsInternalStates internal = FusionAhrsGetInternalStates(&g_ahrs);
  dbg.accelBodyX = g_last_accel_body.axis.x;
  dbg.accelBodyY = g_last_accel_body.axis.y;
  dbg.accelBodyZ = g_last_accel_body.axis.z;
  dbg.magFusionX = g_last_mag_fusion.axis.x;
  dbg.magFusionY = g_last_mag_fusion.axis.y;
  dbg.magFusionZ = g_last_mag_fusion.axis.z;
  dbg.accelerationErrorDeg = internal.accelerationError;
  dbg.magneticErrorDeg = internal.magneticError;
  dbg.accelerometerIgnored = internal.accelerometerIgnored;
  dbg.magnetometerIgnored = internal.magnetometerIgnored;
  if (g_replayDebugCount >= kReplayDebugQueueDepth) {
    g_replayDebugTail = (uint8_t)((g_replayDebugTail + 1U) % kReplayDebugQueueDepth);
    g_replayDebugCount--;
  }
  g_replayDebugQueue[g_replayDebugHead] = dbg;
  g_replayDebugHead = (uint8_t)((g_replayDebugHead + 1U) % kReplayDebugQueueDepth);
  g_replayDebugCount++;
}

void resetFusionSchedulers() {
  g_nextReadUs = 0U;
  g_nextFusionUs = 0U;
  g_lastFusionUpdateUs = 0U;
}

void queueFrame(const ImuFrame& f) {
  if (g_frameCount >= kImuFrameQueueDepth) {
    // Drop oldest frame if producer outruns consumer.
    g_frameTail = (uint8_t)((g_frameTail + 1U) % kImuFrameQueueDepth);
    g_frameCount--;
  }
  g_frameQueue[g_frameHead] = f;
  g_frameHead = (uint8_t)((g_frameHead + 1U) % kImuFrameQueueDepth);
  g_frameCount++;
}

bool takeAveragedFrame(ImuFrame& out) {
  if (g_frameCount == 0U) return false;
  double sumAx = 0.0, sumAy = 0.0, sumAz = 0.0;
  double sumGx = 0.0, sumGy = 0.0, sumGz = 0.0;
  double sumMx = 0.0, sumMy = 0.0, sumMz = 0.0;
  uint32_t latest_t_us = 0U;
  uint8_t n = 0;
  bool all_processed_body = true;
  while (g_frameCount > 0U) {
    const ImuFrame& f = g_frameQueue[g_frameTail];
    if (f.valid) {
      sumAx += f.ax; sumAy += f.ay; sumAz += f.az;
      sumGx += f.gx; sumGy += f.gy; sumGz += f.gz;
      sumMx += f.mx; sumMy += f.my; sumMz += f.mz;
      latest_t_us = f.t_us;
      n++;
      all_processed_body = all_processed_body && f.processed_body;
    }
    g_frameTail = (uint8_t)((g_frameTail + 1U) % kImuFrameQueueDepth);
    g_frameCount--;
  }
  if (n == 0U) return false;
  const double invN = 1.0 / (double)n;
  out.ax = (float)(sumAx * invN);
  out.ay = (float)(sumAy * invN);
  out.az = (float)(sumAz * invN);
  out.gx = (float)(sumGx * invN);
  out.gy = (float)(sumGy * invN);
  out.gz = (float)(sumGz * invN);
  out.mx = (float)(sumMx * invN);
  out.my = (float)(sumMy * invN);
  out.mz = (float)(sumMz * invN);
  out.t_us = latest_t_us;
  out.valid = true;
  out.processed_body = all_processed_body;
  return true;
}

bool takeQueuedFrame(ImuFrame& out) {
  if (g_frameCount == 0U) return false;
  out = g_frameQueue[g_frameTail];
  g_frameTail = (uint8_t)((g_frameTail + 1U) % kImuFrameQueueDepth);
  g_frameCount--;
  return out.valid;
}

bool readImuFrame(ImuFrame& out) {
  float sensor[6];
  if (!readRawAccelGyro(sensor)) return false;

  float mxNew = g_mx, myNew = g_my, mzNew = g_mz;
  if (readRawMag(mxNew, myNew, mzNew)) {
    g_mx = mxNew;
    g_my = myNew;
    g_mz = mzNew;
  }

  out.ax = sensor[0];
  out.ay = sensor[1];
  out.az = sensor[2];
  out.gx = sensor[3];
  out.gy = sensor[4];
  out.gz = sensor[5];
  out.mx = g_mx;
  out.my = g_my;
  out.mz = g_mz;
  out.t_us = micros();
  out.valid = true;
  out.processed_body = false;
  return true;
}

bool takeLatestFrame(ImuFrame& out) {
  if (g_frameCount == 0U) return false;
  const uint8_t latestIndex = (uint8_t)((g_frameHead + kImuFrameQueueDepth - 1U) % kImuFrameQueueDepth);
  out = g_frameQueue[latestIndex];
  g_frameTail = g_frameHead;
  g_frameCount = 0U;
  return out.valid;
}

float accelLsbPerG(uint8_t range) {
  switch (range) {
    case BMI2_ACC_RANGE_2G: return 16384.0f;
    case BMI2_ACC_RANGE_4G: return 8192.0f;
    case BMI2_ACC_RANGE_8G: return 4096.0f;
    case BMI2_ACC_RANGE_16G: return 2048.0f;
    default: return 16384.0f;
  }
}

float gyroLsbPerDps(uint8_t range) {
  switch (range) {
    case BMI2_GYR_RANGE_2000: return 16.384f;
    case BMI2_GYR_RANGE_1000: return 32.768f;
    case BMI2_GYR_RANGE_500: return 65.536f;
    case BMI2_GYR_RANGE_250: return 131.072f;
    case BMI2_GYR_RANGE_125: return 262.144f;
    default: return 16.384f;
  }
}

float gyroRangeDps(uint8_t range) {
  switch (range) {
    case BMI2_GYR_RANGE_2000: return 2000.0f;
    case BMI2_GYR_RANGE_1000: return 1000.0f;
    case BMI2_GYR_RANGE_500: return 500.0f;
    case BMI2_GYR_RANGE_250: return 250.0f;
    case BMI2_GYR_RANGE_125: return 125.0f;
    default: return 2000.0f;
  }
}
}  // namespace

float computeHeadingDeg(float magX, float magY) {
  // Body frame is aircraft-standard X forward, Y right, Z down. Heading is
  // clockwise from North, so use +Y in atan2 for the body-frame horizontal
  // field components.
  float heading = atan2f(magY, magX) * 180.0f / (float)M_PI;
  if (heading < 0.0f) heading += 360.0f;
  return heading;
}

float wrapHeading360(float headingDeg) {
  while (headingDeg < 0.0f) headingDeg += 360.0f;
  while (headingDeg >= 360.0f) headingDeg -= 360.0f;
  return headingDeg;
}

float wrapHeading180(float headingDeg) {
  headingDeg = wrapHeading360(headingDeg);
  if (headingDeg > 180.0f) headingDeg -= 360.0f;
  return headingDeg;
}

FusionVector sensorFrameToBodyFrame(const FusionVector& sensor) {
  return FusionAxesSwap(sensor, kSensorToBodyAlignment);
}

FusionVector magBodyToFusionNed(const FusionVector& bodyMag) {
  // The calibrated debug heading uses aircraft body axes directly:
  //   X forward, Y right, Z down.
  // Fusion is also configured for NED, but its magnetic feedback convention is
  // effectively mirrored on the lateral body axis relative to our debug
  // heading calculation. Keep the debug/body-frame magnetometer unchanged and
  // apply the convention correction only on the vector passed into Fusion.
  return {bodyMag.axis.x, -bodyMag.axis.y, bodyMag.axis.z};
}

FusionVector calibratedMagSensorToBodyFrame(const FusionVector& magSensorRaw) {
  const FusionVector magSensorHardIronCorrected = {
      magSensorRaw.axis.x - g_hardIronOffset.axis.x,
      magSensorRaw.axis.y - g_hardIronOffset.axis.y,
      magSensorRaw.axis.z - g_hardIronOffset.axis.z};
  const FusionVector zeroHardIron = {0.0f, 0.0f, 0.0f};
  const FusionVector magSensorCalibrated =
      FusionCalibrationMagnetic(magSensorHardIronCorrected, kSoftIronMatrix, zeroHardIron);
  return sensorFrameToBodyFrame(magSensorCalibrated);
}

FusionVector rotateBodyVectorToEarthFrame(const FusionVector& bodyVector) {
  return FusionMatrixMultiplyVector(FusionQuaternionToMatrix(FusionAhrsGetQuaternion(&g_ahrs)), bodyVector);
}

FusionVector rotateEarthVectorToBodyFrame(const FusionVector& earthVector) {
  const FusionMatrix earthFromBody = FusionQuaternionToMatrix(FusionAhrsGetQuaternion(&g_ahrs));
  const FusionMatrix bodyFromEarth = {
      earthFromBody.element.xx, earthFromBody.element.yx, earthFromBody.element.zx,
      earthFromBody.element.xy, earthFromBody.element.yy, earthFromBody.element.zy,
      earthFromBody.element.xz, earthFromBody.element.yz, earthFromBody.element.zz,
  };
  return FusionMatrixMultiplyVector(bodyFromEarth, earthVector);
}

FusionVector currentMagBodyVector(float mx, float my, float mz) {
  if (g_debugMagMode == DebugMagMode::SyntheticEarth) {
    return rotateEarthVectorToBodyFrame(g_debugSyntheticEarthMag);
  }
  return calibratedMagSensorToBodyFrame({mx, my, mz});
}

void applyFrameToState(const ImuFrame& f, State& s) {
  FusionVector gyro = {f.gx, f.gy, f.gz};
  FusionVector accelBody = {f.ax, f.ay, f.az};
  FusionVector magBody = {};

  if (f.processed_body) {
    magBody = {f.mx, f.my, f.mz};
  } else {
    magBody = currentMagBodyVector(f.mx, f.my, f.mz);
    gyro = FusionCalibrationInertial(gyro, kGyroMisalignment, kGyroSensitivity, g_gyroOffset);
    accelBody = FusionCalibrationInertial(accelBody, kAccelMisalignment, kAccelSensitivity, g_accelOffset);
    gyro = sensorFrameToBodyFrame(gyro);
    accelBody = sensorFrameToBodyFrame(accelBody);
    accelBody.axis.x *= g_accelScale;
    accelBody.axis.y *= g_accelScale;
    accelBody.axis.z *= g_accelScale;
  }

  const FusionVector magFusion = magBodyToFusionNed(magBody);
  g_last_accel_body = accelBody;
  g_last_mag_body = magBody;
  g_last_mag_fusion = magFusion;
  FusionVector accel = accelBody;
  accel.axis.x /= kGravityMps2;
  accel.axis.y /= kGravityMps2;
  accel.axis.z /= kGravityMps2;
  if (!f.processed_body && kUseGyroOffsetFilter) {
    gyro = FusionOffsetUpdate(&g_offset, gyro);
  }

  const uint32_t sample_t_us = (f.t_us != 0U) ? f.t_us : micros();
  float dtSec = kFusionDtSec;
  if (g_lastFusionUpdateUs != 0U) {
    dtSec = (float)(sample_t_us - g_lastFusionUpdateUs) * 1.0e-6f;
    if (!isfinite(dtSec) || dtSec < kMinFusionDtSec || dtSec > kMaxFusionDtSec) {
      dtSec = kFusionDtSec;
    }
  }
  g_lastFusionUpdateUs = sample_t_us;

  const float headingDeg = computeHeadingDeg(magBody.axis.x, magBody.axis.y);
  FusionAhrsUpdate(&g_ahrs, gyro, accel, magFusion, dtSec);
  s.accel_x_mps2 = accelBody.axis.x;
  s.accel_y_mps2 = accelBody.axis.y;
  s.accel_z_mps2 = accelBody.axis.z;
  s.gyro_x_dps = gyro.axis.x;
  s.gyro_y_dps = gyro.axis.y;
  s.gyro_z_dps = gyro.axis.z;
  s.mag_x_uT = magBody.axis.x;
  s.mag_y_uT = magBody.axis.y;
  s.mag_z_uT = magBody.axis.z;
  const FusionQuaternion q = FusionAhrsGetQuaternion(&g_ahrs);
  const FusionEuler e = FusionQuaternionToEuler(q);
  const FusionMatrix m = FusionQuaternionToMatrix(q);
  const float fusionHeadingCol = wrapHeading360(FusionRadiansToDegrees(atan2f(m.element.yx, m.element.xx)));

  s.roll = e.angle.roll;
  s.pitch = e.angle.pitch;
  s.yaw = wrapHeading180(fusionHeadingCol);
  s.mag_heading = headingDeg;
  s.last_imu_ms = millis();
  if (g_replayMode && f.processed_body) {
    queueReplayDebug();
  }
}

bool begin(Stream* dbg) {
  Wire.setClock(I2C_BUS_HZ);
  const int rc = g_imu.begin(dbg);
  Wire.setClock(I2C_BUS_HZ);
  // BMI2_BMM1 library returns 1 on success (not BMI2_OK/0).
  if (rc < 0) {
    g_ready = false;
    return false;
  }

  g_sampleRate = (uint32_t)(g_imu.accelerationSampleRate() + 0.5f);
  if (g_sampleRate == 0U) g_sampleRate = 100U;
  g_ready = true;
  ImuConfig cfg{};
  if (getImuConfig(cfg)) {
    cfg.accBwp = kAccelLeastFilteredBwp;
    cfg.accFilterPerf = kAccelLeastFilteredPerf;
    cfg.gyrBwp = kGyroLeastFilteredBwp;
    cfg.gyrNoisePerf = kGyroLeastFilteredNoisePerf;
    cfg.gyrFilterPerf = kGyroLeastFilteredPerf;
    (void)setImuConfig(cfg);
  }
  if (getImuConfig(cfg)) {
    g_accLsbPerG = accelLsbPerG(cfg.accRange);
    g_gyrLsbPerDps = gyroLsbPerDps(cfg.gyrRange);
    g_settings.gyroscopeRange = gyroRangeDps(cfg.gyrRange);
  }

  FusionOffsetInitialise(&g_offset, g_sampleRate);
  FusionAhrsInitialise(&g_ahrs);
  FusionAhrsSetSettings(&g_ahrs, &g_settings);
  g_replayMode = false;
  clearFrameQueue();
  resetFusionSchedulers();
  return true;
}

void setReplayMode(bool active) {
  if (g_replayMode == active) return;
  g_replayMode = active;
  if (active) {
    // Start replay from a clean estimator state so the replayed sensor stream
    // is not blended with whatever live attitude/bias history was present.
    FusionOffsetInitialise(&g_offset, g_sampleRate);
    FusionAhrsInitialise(&g_ahrs);
    FusionAhrsSetSettings(&g_ahrs, &g_settings);
  }
  clearFrameQueue();
  clearReplayDebugQueue();
  resetFusionSchedulers();
}

bool replayMode() {
  return g_replayMode;
}

bool takeReplayDebug(FusionReplayDebug& out) {
  if (g_replayDebugCount == 0U) return false;
  out = g_replayDebugQueue[g_replayDebugTail];
  g_replayDebugTail = (uint8_t)((g_replayDebugTail + 1U) % kReplayDebugQueueDepth);
  g_replayDebugCount--;
  return true;
}

bool submitReplaySample(float ax_mps2, float ay_mps2, float az_mps2,
                        float gx_dps, float gy_dps, float gz_dps,
                        float mx_uT, float my_uT, float mz_uT,
                        uint32_t sample_t_us) {
  if (!g_ready) return false;
  if (!g_replayMode) {
    setReplayMode(true);
  }
  ImuFrame f = {};
  f.ax = ax_mps2;
  f.ay = ay_mps2;
  f.az = az_mps2;
  f.gx = gx_dps;
  f.gy = gy_dps;
  f.gz = gz_dps;
  f.mx = mx_uT;
  f.my = my_uT;
  f.mz = mz_uT;
  f.t_us = sample_t_us;
  f.valid = true;
  f.processed_body = true;
  queueFrame(f);
  return true;
}

bool readRawAccelGyro(float out6[6]) {
  if (!g_ready || !out6) return false;
  imu_data_t d{};
  const int rc = g_imu.readGyroAccel(d, true);  // raw counts
  if (rc != BMI2_OK) return false;

  // Convert raw counts -> physical units with float precision.
  out6[0] = ((float)d.acc.x / g_accLsbPerG) * kGravityMps2;
  out6[1] = ((float)d.acc.y / g_accLsbPerG) * kGravityMps2;
  out6[2] = ((float)d.acc.z / g_accLsbPerG) * kGravityMps2;
  out6[3] = (float)d.gyr.x / g_gyrLsbPerDps;
  out6[4] = (float)d.gyr.y / g_gyrLsbPerDps;
  out6[5] = (float)d.gyr.z / g_gyrLsbPerDps;
  return true;
}

bool readRawCounts(int16_t out6[6]) {
  if (!g_ready || !out6) return false;
  imu_data_t d{};
  const int rc = g_imu.readGyroAccel(d, true);
  if (rc != BMI2_OK) return false;
  out6[0] = d.acc.x;
  out6[1] = d.acc.y;
  out6[2] = d.acc.z;
  out6[3] = d.gyr.x;
  out6[4] = d.gyr.y;
  out6[5] = d.gyr.z;
  return true;
}

bool readCorrectedAccelGyro(float out6[6]) {
  if (!out6) return false;
  float raw[6];
  if (!readRawAccelGyro(raw)) return false;

  FusionVector gyro = {raw[3], raw[4], raw[5]};
  FusionVector accel = {raw[0], raw[1], raw[2]};
  gyro = FusionCalibrationInertial(gyro, kGyroMisalignment, kGyroSensitivity, g_gyroOffset);
  accel = FusionCalibrationInertial(accel, kAccelMisalignment, kAccelSensitivity, g_accelOffset);
  gyro = sensorFrameToBodyFrame(gyro);
  accel = sensorFrameToBodyFrame(accel);
  accel.axis.x *= g_accelScale;
  accel.axis.y *= g_accelScale;
  accel.axis.z *= g_accelScale;

  out6[0] = accel.axis.x;
  out6[1] = accel.axis.y;
  out6[2] = accel.axis.z;
  out6[3] = gyro.axis.x;
  out6[4] = gyro.axis.y;
  out6[5] = gyro.axis.z;
  // Expose accel in g for callers and consistency with Fusion input expectations.
  out6[0] /= kGravityMps2;
  out6[1] /= kGravityMps2;
  out6[2] /= kGravityMps2;
  return true;
}

bool readRawMag(float& mx, float& my, float& mz) {
  if (!g_ready) return false;
  const int rc = g_imu.readMagneticField(mx, my, mz);
  return rc == BMI2_OK;
}

bool readTemperatureC(float& tempC) {
  if (!g_ready) return false;
  return g_imu.readTemperatureC(tempC);
}

bool sampleAvailable() {
  if (!g_ready) return false;
  // Poll BMI270 status (DRDY bits) via library helper.
  return g_imu.gyroscopeAvailable();
}

void getGyroBiasDps(float& gx, float& gy, float& gz) {
  gx = g_gyroOffset.axis.x;
  gy = g_gyroOffset.axis.y;
  gz = g_gyroOffset.axis.z;
}

void getAccelBiasMps2(float& ax, float& ay, float& az) {
  ax = g_accelOffset.axis.x;
  ay = g_accelOffset.axis.y;
  az = g_accelOffset.axis.z;
}

void getGyroScale(float& lsbPerDps, float& dpsPerLsb) {
  lsbPerDps = g_gyrLsbPerDps;
  dpsPerLsb = (g_gyrLsbPerDps > 0.0f) ? (1.0f / g_gyrLsbPerDps) : 0.0f;
}

void getFusionSettings(float& gain, float& accelRejection, float& magRejection, uint16_t& recoveryPeriod) {
  gain = g_settings.gain;
  accelRejection = g_settings.accelerationRejection;
  magRejection = g_settings.magneticRejection;
  recoveryPeriod = g_settings.recoveryTriggerPeriod;
}

void getFusionFlags(bool& initialising, bool& angularRecovery, bool& accelerationRecovery, bool& magneticRecovery) {
  const FusionAhrsFlags flags = FusionAhrsGetFlags(&g_ahrs);
  initialising = flags.initialising;
  angularRecovery = flags.angularRateRecovery;
  accelerationRecovery = flags.accelerationRecovery;
  magneticRecovery = flags.magneticRecovery;
}

void getFusionHealthFlags(bool& accelerationError, bool& accelerometerIgnored, bool& magneticError, bool& magnetometerIgnored) {
  const FusionAhrsInternalStates internal = FusionAhrsGetInternalStates(&g_ahrs);
  accelerationError =
      (g_settings.accelerationRejection > 0.0f) && isfinite(internal.accelerationError) &&
      (internal.accelerationError >= g_settings.accelerationRejection);
  accelerometerIgnored = internal.accelerometerIgnored;
  magneticError =
      (g_settings.magneticRejection > 0.0f) && isfinite(internal.magneticError) &&
      (internal.magneticError >= g_settings.magneticRejection);
  magnetometerIgnored = internal.magnetometerIgnored;
}

bool setFusionSettings(float gain, float accelRejection, float magRejection, uint16_t recoveryPeriod) {
  if (!g_ready) return false;
  if (!isfinite(gain) || !isfinite(accelRejection) || !isfinite(magRejection)) return false;
  if (gain <= 0.0f) return false;
  if (accelRejection < 0.0f || magRejection < 0.0f) return false;
  if (recoveryPeriod == 0U) return false;

  applyFusionSettings(gain, accelRejection, magRejection, recoveryPeriod);
  return savePersistedFusionSettings();
}

bool loadPersistedFusionSettings() {
  if (!g_ready) return false;
  PersistedFusionSettings cfg{};
  EEPROM.get(kFusionSettingsAddr, cfg);
  if (cfg.magic != kFusionSettingsMagic ||
      cfg.version != kFusionSettingsVersion ||
      cfg.size != sizeof(PersistedFusionSettings) ||
      cfg.checksum != fusionSettingsChecksum(cfg)) {
    return false;
  }
  if (!isfinite(cfg.gain) || !isfinite(cfg.accelerationRejection) || !isfinite(cfg.magneticRejection)) return false;
  if (cfg.gain <= 0.0f || cfg.accelerationRejection < 0.0f || cfg.magneticRejection < 0.0f || cfg.recoveryTriggerPeriod == 0U) {
    return false;
  }
  applyFusionSettings(cfg.gain, cfg.accelerationRejection, cfg.magneticRejection, cfg.recoveryTriggerPeriod);
  return true;
}

bool savePersistedFusionSettings() {
  PersistedFusionSettings cfg{};
  initDefaultFusionSettings(cfg);
  cfg.gain = g_settings.gain;
  cfg.accelerationRejection = g_settings.accelerationRejection;
  cfg.magneticRejection = g_settings.magneticRejection;
  cfg.recoveryTriggerPeriod = g_settings.recoveryTriggerPeriod;
  cfg.checksum = fusionSettingsChecksum(cfg);
  EEPROM.put(kFusionSettingsAddr, cfg);
  return true;
}

bool getImuConfig(ImuConfig& cfg) {
  if (!g_ready) return false;
  if (!g_imu.getAccelConfig(cfg.accOdr, cfg.accRange, cfg.accBwp, cfg.accFilterPerf)) return false;
  if (!g_imu.getGyroConfig(cfg.gyrOdr, cfg.gyrRange, cfg.gyrBwp, cfg.gyrNoisePerf, cfg.gyrFilterPerf)) return false;
  return true;
}

bool setImuConfig(const ImuConfig& cfg) {
  if (!g_ready) return false;
  if (!g_imu.setAccelConfig(cfg.accOdr, cfg.accRange, cfg.accBwp, cfg.accFilterPerf)) return false;
  if (!g_imu.setGyroConfig(cfg.gyrOdr, cfg.gyrRange, cfg.gyrBwp, cfg.gyrNoisePerf, cfg.gyrFilterPerf)) return false;
  g_accLsbPerG = accelLsbPerG(cfg.accRange);
  g_gyrLsbPerDps = gyroLsbPerDps(cfg.gyrRange);
  g_settings.gyroscopeRange = gyroRangeDps(cfg.gyrRange);

  g_sampleRate = (uint32_t)(g_imu.accelerationSampleRate() + 0.5f);
  if (g_sampleRate == 0U) g_sampleRate = 100U;
  FusionOffsetInitialise(&g_offset, g_sampleRate);
  g_lastFusionUpdateUs = 0U;
  return true;
}

bool getImuSampleRates(float& accHz, float& gyrHz) {
  if (!g_ready) return false;
  accHz = g_imu.accelerationSampleRate();
  gyrHz = g_imu.gyroscopeSampleRate();
  return true;
}

void setAccelGyroOffsets(const float offsets6[6]) {
  if (!offsets6) return;
  g_gyroOffset = {offsets6[3], offsets6[4], offsets6[5]};
  // New format stores accel Z as bias directly. Older EEPROM entries stored absolute Z (~9.8).
  float accelZBias = offsets6[2];
  if (fabsf(accelZBias) > 2.0f) {
    accelZBias -= kGravityMps2;  // Backward compatibility for legacy saved calibration.
  }
  g_accelOffset = {offsets6[0], offsets6[1], accelZBias};
}

void setAccelScale(float scale) {
  if (!isfinite(scale) || scale <= 0.0f) {
    g_accelScale = 1.0f;
    return;
  }
  g_accelScale = scale;
}

float getAccelScale() {
  return g_accelScale;
}

void setHardIronOffset(float x, float y, float z) {
  g_hardIronOffset = {x, y, z};
}

void getHardIronOffset(float& x, float& y, float& z) {
  x = g_hardIronOffset.axis.x;
  y = g_hardIronOffset.axis.y;
  z = g_hardIronOffset.axis.z;
}

void getMagHeadingInputs(float& magX, float& magY, float& magZ) {
  if (g_debugMagMode == DebugMagMode::SyntheticEarth) {
    magX = g_last_mag_body.axis.x;
    magY = g_last_mag_body.axis.y;
    magZ = g_last_mag_body.axis.z;
    return;
  }
  float mx = 0.0f;
  float my = 0.0f;
  float mz = 0.0f;
  if (!readRawMag(mx, my, mz)) {
    magX = 0.0f;
    magY = 0.0f;
    magZ = 0.0f;
    return;
  }
  const FusionVector magBody = calibratedMagSensorToBodyFrame({mx, my, mz});
  magX = magBody.axis.x;
  magY = magBody.axis.y;
  magZ = magBody.axis.z;
}

void getFusionHeadingDebug(float& eulerYaw, float& matrixColHeading, float& matrixRowHeading, float& tiltCompHeading) {
  const FusionQuaternion q = FusionAhrsGetQuaternion(&g_ahrs);
  const FusionEuler e = FusionQuaternionToEuler(q);
  const FusionMatrix m = FusionQuaternionToMatrix(q);
  eulerYaw = e.angle.yaw;
  matrixColHeading = wrapHeading360(FusionRadiansToDegrees(atan2f(m.element.yx, m.element.xx)));
  matrixRowHeading = wrapHeading360(FusionRadiansToDegrees(atan2f(m.element.xy, m.element.xx)));
  tiltCompHeading = wrapHeading360(FusionCompassCalculateHeading(g_settings.convention, g_last_accel_body, g_last_mag_fusion));
}

void getFusionMagDebug(FusionMagDebug& out) {
  const FusionVector magEarthFromBody = rotateBodyVectorToEarthFrame(g_last_mag_body);
  const FusionVector magEarthFromFusion = rotateBodyVectorToEarthFrame(g_last_mag_fusion);
  out.bodyX = g_last_mag_body.axis.x;
  out.bodyY = g_last_mag_body.axis.y;
  out.bodyZ = g_last_mag_body.axis.z;
  out.fusionX = g_last_mag_fusion.axis.x;
  out.fusionY = g_last_mag_fusion.axis.y;
  out.fusionZ = g_last_mag_fusion.axis.z;
  out.earthFromBodyX = magEarthFromBody.axis.x;
  out.earthFromBodyY = magEarthFromBody.axis.y;
  out.earthFromBodyZ = magEarthFromBody.axis.z;
  out.earthFromFusionX = magEarthFromFusion.axis.x;
  out.earthFromFusionY = magEarthFromFusion.axis.y;
  out.earthFromFusionZ = magEarthFromFusion.axis.z;
  out.earthFromBodyHeading = computeHeadingDeg(magEarthFromBody.axis.x, magEarthFromBody.axis.y);
  out.earthFromFusionHeading = computeHeadingDeg(magEarthFromFusion.axis.x, magEarthFromFusion.axis.y);
}

void getFusionReplayDebug(FusionReplayDebug& out) {
  const FusionAhrsInternalStates internal = FusionAhrsGetInternalStates(&g_ahrs);
  out.accelBodyX = g_last_accel_body.axis.x;
  out.accelBodyY = g_last_accel_body.axis.y;
  out.accelBodyZ = g_last_accel_body.axis.z;
  out.magFusionX = g_last_mag_fusion.axis.x;
  out.magFusionY = g_last_mag_fusion.axis.y;
  out.magFusionZ = g_last_mag_fusion.axis.z;
  out.accelerationErrorDeg = internal.accelerationError;
  out.magneticErrorDeg = internal.magneticError;
  out.accelerometerIgnored = internal.accelerometerIgnored;
  out.magnetometerIgnored = internal.magnetometerIgnored;
}

void setDebugMagLive() {
  g_debugMagMode = DebugMagMode::Live;
}

void setDebugMagSyntheticEarth(float north, float east, float down) {
  g_debugSyntheticEarthMag = {north, east, down};
  g_debugMagMode = DebugMagMode::SyntheticEarth;
}

DebugMagMode getDebugMagMode() {
  return g_debugMagMode;
}

void getDebugMagSyntheticEarth(float& north, float& east, float& down) {
  north = g_debugSyntheticEarthMag.axis.x;
  east = g_debugSyntheticEarthMag.axis.y;
  down = g_debugSyntheticEarthMag.axis.z;
}

void update400Hz(State& s) {
  if (!g_ready) return;
  const uint32_t nowUs = micros();
  if (g_nextReadUs == 0U) {
    g_nextReadUs = nowUs;
    g_nextFusionUs = nowUs;
  }

  if (!g_replayMode) {
    uint32_t readGuard = 0;
    while ((int32_t)(nowUs - g_nextReadUs) >= 0 && readGuard < 4U) {
      ImuFrame f{};
      if (readImuFrame(f)) {
        queueFrame(f);
      }
      g_nextReadUs += kFusionPeriodUs;
      readGuard++;
    }
  }

  uint32_t fuseGuard = 0;
  while (((g_replayMode && g_frameCount > 0U) || (!g_replayMode && (int32_t)(nowUs - g_nextFusionUs) >= 0)) &&
         fuseGuard < 4U) {
    ImuFrame f{};
    const bool haveFrame = g_replayMode ? takeQueuedFrame(f)
                                        : (kUseFrameAveraging ? takeAveragedFrame(f) : takeLatestFrame(f));
    if (haveFrame) {
      applyFrameToState(f, s);
    }
    if (!g_replayMode) {
      g_nextFusionUs += kFusionPeriodUs;
    }
    fuseGuard++;
  }
}

}  // namespace imu_fusion
