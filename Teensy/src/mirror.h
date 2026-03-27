#pragma once

#include <Arduino.h>
#include "state.h"

namespace imu_fusion {
struct FusionReplayDebug;
}

namespace mirror {

struct ReplayOutputMeta {
  uint32_t seq;
  uint32_t t_us;
  uint32_t last_gps_ms;
  uint32_t last_imu_ms;
  uint32_t last_baro_ms;
  uint32_t present_mask;
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
  float accel_x_mps2;
  float accel_y_mps2;
  float accel_z_mps2;
  float gyro_x_dps;
  float gyro_y_dps;
  float gyro_z_dps;
  float mag_x_uT;
  float mag_y_uT;
  float mag_z_uT;
  float baro_temp_c;
  float baro_press_hpa;
  float baro_alt_m;
  float baro_vsi_mps;
};

struct RxDebugStats {
  uint32_t rxBytes;
  uint32_t framesOk;
  uint32_t cobsErr;
  uint32_t lenErr;
  uint32_t crcErr;
  uint32_t unknownMsg;
  uint32_t cmdSetFusion;
  uint32_t cmdGetFusion;
  uint32_t cmdSetStreamRate;
  uint32_t replayInputFrames;
  uint32_t replayControlFrames;
  uint32_t ackSent;
  uint32_t nackSent;
  uint16_t lastMsgType;
};

struct ReplayPerfSnapshot {
  uint32_t poll_rx_avg_us;
  uint32_t poll_rx_max_us;
  uint32_t apply_input_avg_us;
  uint32_t apply_input_max_us;
  uint32_t submit_sample_avg_us;
  uint32_t submit_sample_max_us;
  uint32_t queue_meta_avg_us;
  uint32_t queue_meta_max_us;
  uint32_t send_state_avg_us;
  uint32_t send_state_max_us;
  uint32_t push_state_avg_us;
  uint32_t push_state_max_us;
  uint32_t replay_inputs_per_poll_avg;
  uint32_t replay_inputs_per_poll_max;
  uint32_t replay_ctrls_per_poll_avg;
  uint32_t replay_ctrls_per_poll_max;
  uint32_t replay_output_queue_depth_max;
};

void begin();
void pollRx(State& s);
bool sendFastState(const State& s, uint32_t seq, uint32_t t_us,
                   const ReplayOutputMeta* replay_meta = nullptr,
                   const imu_fusion::FusionReplayDebug* replay_diag = nullptr);
uint16_t crc16Ccitt(const uint8_t* data, uint16_t len);
RxDebugStats getRxDebugStats();
uint16_t streamRateHz();
uint16_t logRateHz();
uint32_t streamPeriodUs();
bool replayActive();
bool takeReplayOutputMeta(ReplayOutputMeta& out);
void getReplayPerfSnapshot(ReplayPerfSnapshot& out);
void resetReplayPerf();

}  // namespace mirror
