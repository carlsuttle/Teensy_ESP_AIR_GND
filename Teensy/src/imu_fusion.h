#pragma once

#include <Arduino.h>
#include "state.h"
#include "types_shared.h"

namespace imu_fusion {

enum class DebugMagMode : uint8_t {
  Live = 0,
  SyntheticEarth = 1,
};

struct FusionMagDebug {
  float bodyX;
  float bodyY;
  float bodyZ;
  float fusionX;
  float fusionY;
  float fusionZ;
  float earthFromBodyX;
  float earthFromBodyY;
  float earthFromBodyZ;
  float earthFromFusionX;
  float earthFromFusionY;
  float earthFromFusionZ;
  float earthFromBodyHeading;
  float earthFromFusionHeading;
};

struct FusionReplayDebug {
  float accelBodyX;
  float accelBodyY;
  float accelBodyZ;
  float magFusionX;
  float magFusionY;
  float magFusionZ;
  float accelerationErrorDeg;
  float magneticErrorDeg;
  bool accelerometerIgnored;
  bool magnetometerIgnored;
};

struct ImuConfig {
  uint8_t accOdr;
  uint8_t accRange;
  uint8_t accBwp;
  uint8_t accFilterPerf;
  uint8_t gyrOdr;
  uint8_t gyrRange;
  uint8_t gyrBwp;
  uint8_t gyrNoisePerf;
  uint8_t gyrFilterPerf;
};

struct CaptureSettings {
  uint16_t sourceRateHz = 0U;
};

struct SourcePerfSnapshot {
  uint32_t read_accel_gyro_avg_us = 0U;
  uint32_t read_accel_gyro_max_us = 0U;
  uint32_t read_mag_avg_us = 0U;
  uint32_t read_mag_max_us = 0U;
  uint32_t read_frame_avg_us = 0U;
  uint32_t read_frame_max_us = 0U;
  uint32_t apply_frame_avg_us = 0U;
  uint32_t apply_frame_max_us = 0U;
  uint32_t fusion_ahrs_avg_us = 0U;
  uint32_t fusion_ahrs_max_us = 0U;
};

struct SourceFlowSnapshot {
  uint32_t scheduled_ticks = 0U;
  uint32_t successful_reads = 0U;
  uint32_t applied_updates = 0U;
  uint32_t frame_drops = 0U;
  uint32_t raw_record_drops = 0U;
};

bool begin(Stream* dbg = &Serial);
void updateSourceRate(State& s);
void update400Hz(State& s);
bool setSourceRateHz(uint16_t requested_hz, uint16_t* applied_hz = nullptr);
bool isSupportedSourceRateHz(uint16_t hz);
size_t supportedSourceRateCount();
uint16_t supportedSourceRateHzAt(size_t index);
uint16_t sourceRateHz();
uint32_t sourcePeriodUs();
uint32_t sourceReadCount();
uint32_t sourceUpdateCount();
void getSourcePerfSnapshot(SourcePerfSnapshot& out);
void getSourceFlowSnapshot(SourceFlowSnapshot& out);
void setReplayMode(bool active);
bool replayMode();
bool takeReplayDebug(FusionReplayDebug& out);
bool takeRawReplayInput(telem::ReplayInputRecord160& out);
bool submitReplaySample(float ax_mps2, float ay_mps2, float az_mps2,
                        float gx_dps, float gy_dps, float gz_dps,
                        float mx_uT, float my_uT, float mz_uT,
                        uint32_t sample_t_us);

bool readRawAccelGyro(float out6[6]);   // ax,ay,az[m/s^2], gx,gy,gz[dps]
bool readRawGyro(float out3[3]); // gx,gy,gz[dps]
bool readCorrectedAccelGyro(float out6[6]); // calibration-corrected ax,ay,az[g], gx,gy,gz[dps]
bool readRawCounts(int16_t out6[6]); // ax,ay,az[counts], gx,gy,gz[counts]
bool readRawMag(float& mx, float& my, float& mz); // uT
bool readTemperatureC(float& tempC);
void getGyroBiasDps(float& gx, float& gy, float& gz);
void getAccelBiasMps2(float& ax, float& ay, float& az);
void getGyroScale(float& lsbPerDps, float& dpsPerLsb);
void getFusionSettings(float& gain, float& accelRejection, float& magRejection, uint16_t& recoveryPeriod);
void getFusionFlags(bool& initialising, bool& angularRecovery, bool& accelerationRecovery, bool& magneticRecovery);
void getFusionHealthFlags(bool& accelerationError, bool& accelerometerIgnored, bool& magneticError, bool& magnetometerIgnored);
bool setFusionSettings(float gain, float accelRejection, float magRejection, uint16_t recoveryPeriod);
bool loadPersistedFusionSettings();
bool savePersistedFusionSettings();
bool getCaptureSettings(CaptureSettings& cfg);
bool setCaptureSettings(const CaptureSettings& cfg, uint16_t* applied_hz = nullptr);
bool loadPersistedCaptureSettings();
bool savePersistedCaptureSettings();
bool getImuConfig(ImuConfig& cfg);
bool setImuConfig(const ImuConfig& cfg);
bool getImuSampleRates(float& accHz, float& gyrHz);

void setAccelGyroOffsets(const float offsets6[6]);
void setAccelScale(float scale);
float getAccelScale();
void setHardIronOffset(float x, float y, float z);
void getHardIronOffset(float& x, float& y, float& z);
float computeHeadingDeg(float magX, float magY);
void getMagHeadingInputs(float& magX, float& magY, float& magZ);
void getFusionHeadingDebug(float& eulerYaw, float& matrixColHeading, float& matrixRowHeading, float& tiltCompHeading);
void getFusionMagDebug(FusionMagDebug& out);
void getFusionReplayDebug(FusionReplayDebug& out);
void setDebugMagLive();
void setDebugMagSyntheticEarth(float north, float east, float down);
DebugMagMode getDebugMagMode();
void getDebugMagSyntheticEarth(float& north, float& east, float& down);

}  // namespace imu_fusion
