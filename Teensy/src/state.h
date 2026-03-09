#pragma once

#include <Arduino.h>

struct State {
  float roll;
  float pitch;
  float yaw;

  uint32_t iTOW;
  uint8_t fixType;
  uint8_t numSV;
  int32_t lat;
  int32_t lon;
  int32_t hMSL;
  int32_t gSpeed;
  int32_t headMot;
  uint32_t hAcc;
  uint32_t sAcc;

  uint32_t gps_parse_errors;
  uint32_t mirror_tx_ok;
  uint32_t mirror_drop_count;

  uint32_t last_gps_ms;
  uint32_t last_imu_ms;

  float baro_temp_c;
  float baro_press_hpa;
  float baro_alt_m;
  float baro_vsi_mps;
  uint32_t last_baro_ms;
};
