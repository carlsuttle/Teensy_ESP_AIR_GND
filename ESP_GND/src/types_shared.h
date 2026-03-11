#pragma once

#include <Arduino.h>

namespace telem {

static constexpr uint32_t kMagic = 0x54454C4DUL;  // "TELM"
static constexpr uint16_t kVersion = 1;
static constexpr uint32_t kWsStateMagic = 0x57535445UL;  // "WSTE"
static constexpr uint16_t kWsStateVersion = 2;
static constexpr uint8_t kRadioChannel = 6;
static constexpr uint16_t kEspNowMaxDataLen = 250;
static constexpr uint8_t kLinkMetaFlagPeerKnown = 1U << 0;
static constexpr uint8_t kLinkMetaFlagRadioReady = 1U << 1;
static constexpr uint8_t kLinkMetaFlagRecorderOn = 1U << 2;
static constexpr uint8_t kLinkMetaFlagRssiValid = 1U << 3;

enum MsgType : uint16_t {
  TELEM_FULL_STATE = 1,
  TELEM_EVENT = 2,
  TELEM_META = 3,
  TELEM_FUSION_SETTINGS = 4,
  CMD_SET_FUSION_SETTINGS = 100,
  CMD_GET_FUSION_SETTINGS = 101,
  CMD_SET_STREAM_RATE = 102,
  CMD_RESET_NETWORK = 103,
  LINK_HELLO = 150,
  ACK = 200,
  NACK = 201
};

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

struct LinkHelloPayloadV1 {
  uint8_t unit_id;
  uint8_t flags;
  uint16_t reserved;
  uint32_t session_id;
};

struct LinkMetaPayloadV1 {
  int16_t gnd_ap_rssi_dbm;
  uint8_t flags;
  uint8_t reserved0;
  uint32_t scan_age_ms;
  uint32_t link_age_ms;
};

struct WsStateHeaderV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t payload_len;
  uint32_t ws_seq;
};

struct WsStateHeaderV2 {
  uint32_t magic;
  uint16_t version;
  uint16_t header_len;
  uint16_t payload_len;
  uint16_t flags;
  uint32_t ws_seq;
  uint32_t state_seq;
  uint32_t source_t_us;
  uint32_t esp_rx_ms;
};
#pragma pack(pop)

}  // namespace telem
