#pragma once

#include <Arduino.h>

namespace telem {

static constexpr uint32_t kMagic = 0x54454C4DUL;  // "TELM"
static constexpr uint16_t kVersion = 1;
static constexpr uint32_t kWsStateMagic = 0x57535445UL;  // "WSTE"
static constexpr uint16_t kWsStateVersion = 2;
static constexpr uint8_t kRadioChannel = 6;
static constexpr uint16_t kEspNowMaxDataLen = 250;
static constexpr uint16_t kStateFlagGpsFix3d = 1U << 0;
static constexpr uint16_t kStateFlagFusionInitialising = 1U << 1;
static constexpr uint16_t kStateFlagFusionAngularRecovery = 1U << 2;
static constexpr uint16_t kStateFlagFusionAccelerationRecovery = 1U << 3;
static constexpr uint16_t kStateFlagFusionMagneticRecovery = 1U << 4;
static constexpr uint8_t kLinkMetaFlagPeerKnown = 1U << 0;
static constexpr uint8_t kLinkMetaFlagRadioReady = 1U << 1;
static constexpr uint8_t kLinkMetaFlagRecorderOn = 1U << 2;
static constexpr uint8_t kLinkMetaFlagRssiValid = 1U << 3;
static constexpr uint8_t kLogStatusFlagActive = 1U << 0;
static constexpr uint8_t kLogStatusFlagRequested = 1U << 1;
static constexpr uint8_t kLogStatusFlagBackendReady = 1U << 2;
static constexpr uint8_t kLogStatusFlagMediaPresent = 1U << 3;
static constexpr uint32_t kLogBytesUnknown = 0xFFFFFFFFUL;

enum MsgType : uint16_t {
  TELEM_FULL_STATE = 1,
  TELEM_EVENT = 2,
  TELEM_META = 3,
  TELEM_FUSION_SETTINGS = 4,
  TELEM_LOG_STATUS = 5,
  TELEM_CONTROL_STATUS = 6,
  TELEM_UNIFIED_DOWNLINK = 7,
  CMD_SET_FUSION_SETTINGS = 100,
  CMD_GET_FUSION_SETTINGS = 101,
  CMD_SET_STREAM_RATE = 102,
  CMD_RESET_NETWORK = 103,
  CMD_LOG_START = 104,
  CMD_LOG_STOP = 105,
  CMD_GET_LOG_STATUS = 106,
  CMD_RADIO_PING = 107,
  CMD_SET_RADIO_MODE = 108,
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

static constexpr uint8_t kUnifiedDownlinkFlagHasGps = 1U << 0;
static constexpr uint8_t kUnifiedDownlinkFlagHasControl = 1U << 1;

struct DownlinkFastStateV1 {
  float roll_deg;
  float pitch_deg;
  float yaw_deg;
  uint32_t last_imu_ms;
  float baro_temp_c;
  float baro_press_hpa;
  float baro_alt_m;
  float baro_vsi_mps;
  uint32_t last_baro_ms;
  uint16_t flags;
};

struct DownlinkGpsStateV1 {
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
  uint32_t last_gps_ms;
};

struct UnifiedDownlinkBaseV1 {
  uint8_t section_flags;
  uint8_t reserved0;
  uint16_t reserved1;
  uint32_t source_seq;
  DownlinkFastStateV1 fast;
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

struct CmdSetRadioModeV1 {
  uint8_t state_only;
  uint8_t control_rate_hz;
  uint16_t telem_rate_hz;
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

struct LogStatusPayloadV1 {
  uint8_t flags;
  uint8_t reserved0;
  uint16_t last_command;
  uint32_t session_id;
  uint32_t bytes_written;
  uint32_t free_bytes;
  uint32_t last_change_ms;
};

static constexpr uint8_t kControlStatusFlagHasAck = 1U << 0;
static constexpr uint8_t kControlStatusFlagAckOk = 1U << 1;
static constexpr uint8_t kControlStatusFlagHasFusion = 1U << 2;
static constexpr uint8_t kControlStatusFlagHasLinkMeta = 1U << 3;
static constexpr uint8_t kControlStatusFlagHasLogStatus = 1U << 4;

struct ControlStatusPayloadV1 {
  uint8_t flags;
  uint8_t control_rate_hz;
  uint16_t ack_command;
  uint32_t ack_code;
  FusionSettingsV1 fusion;
  LinkMetaPayloadV1 link_meta;
  LogStatusPayloadV1 log_status;
  uint32_t mirror_tx_ok;
  uint32_t mirror_drop_count;
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
