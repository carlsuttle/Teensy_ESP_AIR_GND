#include "imu_fusion.h"
#include "config.h"

#include <math.h>
#include <Wire.h>
#include <BMI2_BMM1.h>
#include <Fusion.h>

namespace imu_fusion {
bool readRawAccelGyro(float out6[6]);
bool readRawMag(float& mx, float& my, float& mz);

namespace {
BMI2_BMM1_Class g_imu(Wire);
bool g_ready = false;

FusionOffset g_offset;
FusionAhrs g_ahrs;
constexpr float kDefaultFusionGain = 0.06f;
constexpr float kDefaultFusionAccelRejectDeg = 20.0f;
constexpr float kDefaultFusionMagRejectDeg = 60.0f;
constexpr uint16_t kDefaultFusionRecoverySamples = 1200U;
FusionAhrsSettings g_settings = {
    .convention = FusionConventionNwu,
    .gain = kDefaultFusionGain,
    .gyroscopeRange = BMI2_GYR_RANGE_250,
    .accelerationRejection = kDefaultFusionAccelRejectDeg,
    .magneticRejection = kDefaultFusionMagRejectDeg,
    .recoveryTriggerPeriod = kDefaultFusionRecoverySamples,
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
uint32_t g_sampleRate = 100;
constexpr float kGravityMps2 = 9.80665f;
constexpr float kFusionDtSec = 0.0025f;  // 400 Hz
constexpr uint32_t kFusionPeriodUs = 2500U;
float g_accLsbPerG = 16384.0f;
float g_gyrLsbPerDps = 16.384f;

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
  bool valid = false;
};

constexpr uint8_t kImuFrameQueueDepth = 4;
ImuFrame g_frameQueue[kImuFrameQueueDepth];
uint8_t g_frameHead = 0;
uint8_t g_frameTail = 0;
uint8_t g_frameCount = 0;
uint32_t g_nextReadUs = 0;
uint32_t g_nextFusionUs = 0;

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
  uint8_t n = 0;
  while (g_frameCount > 0U) {
    const ImuFrame& f = g_frameQueue[g_frameTail];
    if (f.valid) {
      sumAx += f.ax; sumAy += f.ay; sumAz += f.az;
      sumGx += f.gx; sumGy += f.gy; sumGz += f.gz;
      sumMx += f.mx; sumMy += f.my; sumMz += f.mz;
      n++;
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
  out.valid = true;
  return true;
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
  out.valid = true;
  return true;
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
}  // namespace

float computeHeadingDeg(float magX, float magY) {
  float heading = atan2f(magY, magX) * 180.0f / (float)M_PI;
  if (heading < 0.0f) heading += 360.0f;
  return heading;
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
    g_accLsbPerG = accelLsbPerG(cfg.accRange);
    g_gyrLsbPerDps = gyroLsbPerDps(cfg.gyrRange);
  }

  FusionOffsetInitialise(&g_offset, g_sampleRate);
  FusionAhrsInitialise(&g_ahrs);
  FusionAhrsSetSettings(&g_ahrs, &g_settings);
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

bool setFusionSettings(float gain, float accelRejection, float magRejection, uint16_t recoveryPeriod) {
  if (!g_ready) return false;
  if (!isfinite(gain) || !isfinite(accelRejection) || !isfinite(magRejection)) return false;
  if (gain <= 0.0f) return false;
  if (accelRejection < 0.0f || magRejection < 0.0f) return false;
  if (recoveryPeriod == 0U) return false;

  g_settings.gain = gain;
  g_settings.accelerationRejection = accelRejection;
  g_settings.magneticRejection = magRejection;
  g_settings.recoveryTriggerPeriod = recoveryPeriod;
  FusionAhrsSetSettings(&g_ahrs, &g_settings);
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

  g_sampleRate = (uint32_t)(g_imu.accelerationSampleRate() + 0.5f);
  if (g_sampleRate == 0U) g_sampleRate = 100U;
  FusionOffsetInitialise(&g_offset, g_sampleRate);
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

void update400Hz(State& s) {
  if (!g_ready) return;
  const uint32_t nowUs = micros();
  if (g_nextReadUs == 0U) {
    g_nextReadUs = nowUs;
    g_nextFusionUs = nowUs;
  }

  uint32_t readGuard = 0;
  while ((int32_t)(nowUs - g_nextReadUs) >= 0 && readGuard < 4U) {
    ImuFrame f{};
    if (readImuFrame(f)) {
      queueFrame(f);
    }
    g_nextReadUs += kFusionPeriodUs;
    readGuard++;
  }

  uint32_t fuseGuard = 0;
  while ((int32_t)(nowUs - g_nextFusionUs) >= 0 && fuseGuard < 4U) {
    ImuFrame f{};
    // Use averaged buffered frames to reduce fusion aliasing from read/fuse phase mismatch.
    if (takeAveragedFrame(f)) {
      FusionVector gyro = {f.gx, f.gy, f.gz};
      FusionVector accel = {f.ax, f.ay, f.az};
      const FusionVector magRaw = {-f.mx, -f.my, -f.mz};
      const FusionVector magHard = {
          magRaw.axis.x + g_hardIronOffset.axis.x,
          magRaw.axis.y + g_hardIronOffset.axis.y,
          magRaw.axis.z + g_hardIronOffset.axis.z};
      const FusionVector zeroHard = {0.0f, 0.0f, 0.0f};
      FusionVector mag = FusionCalibrationMagnetic(magHard, kSoftIronMatrix, zeroHard);

      gyro = FusionCalibrationInertial(gyro, kGyroMisalignment, kGyroSensitivity, g_gyroOffset);
      accel = FusionCalibrationInertial(accel, kAccelMisalignment, kAccelSensitivity, g_accelOffset);
      accel.axis.x *= g_accelScale;
      accel.axis.y *= g_accelScale;
      accel.axis.z *= g_accelScale;
      accel.axis.x /= kGravityMps2;
      accel.axis.y /= kGravityMps2;
      accel.axis.z /= kGravityMps2;
      gyro = FusionOffsetUpdate(&g_offset, gyro);

      FusionAhrsUpdate(&g_ahrs, gyro, accel, mag, kFusionDtSec);
      const FusionEuler e = FusionQuaternionToEuler(FusionAhrsGetQuaternion(&g_ahrs));

      s.roll = e.angle.roll;
      s.pitch = e.angle.pitch;
      s.yaw = e.angle.yaw;
      s.last_imu_ms = millis();
    }
    g_nextFusionUs += kFusionPeriodUs;
    fuseGuard++;
  }
}

}  // namespace imu_fusion
