#pragma once

#include <Arduino.h>
#include "state.h"

namespace imu_fusion {

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

bool begin(Stream* dbg = &Serial);
void update400Hz(State& s);

bool readRawAccelGyro(float out6[6]);   // ax,ay,az[m/s^2], gx,gy,gz[dps]
bool readCorrectedAccelGyro(float out6[6]); // calibration-corrected ax,ay,az[g], gx,gy,gz[dps]
bool readRawCounts(int16_t out6[6]); // ax,ay,az[counts], gx,gy,gz[counts]
bool readRawMag(float& mx, float& my, float& mz); // uT
bool readTemperatureC(float& tempC);
bool sampleAvailable();
void getGyroBiasDps(float& gx, float& gy, float& gz);
void getAccelBiasMps2(float& ax, float& ay, float& az);
void getGyroScale(float& lsbPerDps, float& dpsPerLsb);
void getFusionSettings(float& gain, float& accelRejection, float& magRejection, uint16_t& recoveryPeriod);
bool setFusionSettings(float gain, float accelRejection, float magRejection, uint16_t recoveryPeriod);
bool getImuConfig(ImuConfig& cfg);
bool setImuConfig(const ImuConfig& cfg);
bool getImuSampleRates(float& accHz, float& gyrHz);

void setAccelGyroOffsets(const float offsets6[6]);
void setAccelScale(float scale);
float getAccelScale();
void setHardIronOffset(float x, float y, float z);
void getHardIronOffset(float& x, float& y, float& z);
float computeHeadingDeg(float magX, float magY);

}  // namespace imu_fusion
