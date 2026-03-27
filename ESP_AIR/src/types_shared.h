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
static constexpr uint16_t kStateFlagFusionAccelerationError = 1U << 5;
static constexpr uint16_t kStateFlagFusionAccelerometerIgnored = 1U << 6;
static constexpr uint16_t kStateFlagFusionMagneticError = 1U << 7;
static constexpr uint16_t kStateFlagFusionMagnetometerIgnored = 1U << 8;
static constexpr uint8_t kLinkMetaFlagPeerKnown = 1U << 0;
static constexpr uint8_t kLinkMetaFlagRadioReady = 1U << 1;
static constexpr uint8_t kLinkMetaFlagRecorderOn = 1U << 2;
static constexpr uint8_t kLinkMetaFlagRssiValid = 1U << 3;
static constexpr uint8_t kLogStatusFlagActive = 1U << 0;
static constexpr uint8_t kLogStatusFlagRequested = 1U << 1;
static constexpr uint8_t kLogStatusFlagBackendReady = 1U << 2;
static constexpr uint8_t kLogStatusFlagMediaPresent = 1U << 3;
static constexpr uint8_t kLogStatusFlagBusy = 1U << 4;
static constexpr uint32_t kLogBytesUnknown = 0xFFFFFFFFUL;
static constexpr uint16_t kTelemetryStateRecordBytes = 160U;
static constexpr uint16_t kReplayRecordBytes = 160U;
static constexpr uint32_t kReplayMagic = 0x52504C59UL;  // "RPLY"
static constexpr uint16_t kReplayVersion = 1U;
static constexpr uint16_t kLogFileNameBytes = 96U;
static constexpr uint16_t kLogFileChunkEntries = 2U;
static constexpr uint16_t kRecordPrefixBytes = 24U;

static constexpr uint32_t kSensorPresentImu = 1UL << 0;
static constexpr uint32_t kSensorPresentMag = 1UL << 1;
static constexpr uint32_t kSensorPresentGps = 1UL << 2;
static constexpr uint32_t kSensorPresentBaro = 1UL << 3;

static constexpr uint32_t kReplayControlFlagAccepted = 1UL << 0;
static constexpr uint32_t kReplayControlFlagApplied = 1UL << 1;
static constexpr uint32_t kReplayControlFlagSourceGui = 1UL << 2;
static constexpr uint32_t kReplayControlFlagSourceRadio = 1UL << 3;

enum class ReplayRecordKind : uint8_t {
  Input = 1,
  Control = 2,
};

enum class LogRecordKind : uint16_t {
  State160 = 1,
  ReplayControl160 = 2,
  ReplayInput160 = 3,
};

enum MsgType : uint16_t {
  TELEM_FULL_STATE = 1,
  TELEM_EVENT = 2,
  TELEM_META = 3,
  TELEM_FUSION_SETTINGS = 4,
  TELEM_LOG_STATUS = 5,
  TELEM_CONTROL_STATUS = 6,
  TELEM_UNIFIED_DOWNLINK = 7,
  TELEM_REPLAY_STATUS = 8,
  TELEM_LOG_FILE_LIST = 9,
  TELEM_STORAGE_STATUS = 10,
  CMD_SET_FUSION_SETTINGS = 100,
  CMD_GET_FUSION_SETTINGS = 101,
  CMD_SET_STREAM_RATE = 102,
  CMD_RESET_NETWORK = 103,
  CMD_LOG_START = 104,
  CMD_LOG_STOP = 105,
  CMD_GET_LOG_STATUS = 106,
  CMD_RADIO_PING = 107,
  CMD_SET_RADIO_MODE = 108,
  CMD_REPLAY_START = 109,
  CMD_REPLAY_STOP = 110,
  CMD_GET_REPLAY_STATUS = 111,
  CMD_REPLAY_INPUT_RECORD = 112,
  CMD_REPLAY_CONTROL_RECORD = 113,
  CMD_GET_LOG_FILE_LIST = 114,
  CMD_DELETE_LOG_FILE = 115,
  CMD_RENAME_LOG_FILE = 116,
  CMD_REPLAY_START_FILE = 117,
  CMD_REPLAY_PAUSE = 118,
  CMD_REPLAY_SEEK_REL = 119,
  CMD_SET_CAPTURE_SETTINGS = 120,
  CMD_GET_CAPTURE_SETTINGS = 121,
  CMD_SAVE_CAPTURE_SETTINGS = 122,
  CMD_GET_STORAGE_STATUS = 123,
  CMD_MOUNT_MEDIA = 124,
  CMD_EJECT_MEDIA = 125,
  CMD_EXPORT_LOG_CSV = 126,
  CMD_SET_RECORD_PREFIX = 127,
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
  float mag_heading_deg;

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
  float accel_x_mps2;
  float accel_y_mps2;
  float accel_z_mps2;
  float gyro_x_dps;
  float gyro_y_dps;
  float gyro_z_dps;
  float mag_x_uT;
  float mag_y_uT;
  float mag_z_uT;
  uint16_t raw_present_mask;
  uint16_t reserved0;
  uint8_t reserved1[14];
};

static constexpr uint8_t kUnifiedDownlinkFlagHasGps = 1U << 0;
static constexpr uint8_t kUnifiedDownlinkFlagHasControl = 1U << 1;

struct DownlinkFastStateV1 {
  float roll_deg;
  float pitch_deg;
  float yaw_deg;
  float mag_heading_deg;
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

struct CaptureSettingsV1 {
  uint16_t source_rate_hz;
  uint16_t reserved;
};

using CmdSetCaptureSettingsV1 = CaptureSettingsV1;

struct CmdSetStreamRateV1 {
  uint16_t ws_rate_hz;
  uint16_t log_rate_hz;
};

struct CmdSetRadioModeV1 {
  uint8_t state_only;
  uint8_t control_rate_hz;
  uint8_t radio_lr_mode;
  uint8_t reserved0;
  uint16_t telem_rate_hz;
  uint16_t reserved1;
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

static constexpr uint8_t kReplayStatusFlagActive = 1U << 0;
static constexpr uint8_t kReplayStatusFlagFileOpen = 1U << 1;
static constexpr uint8_t kReplayStatusFlagAtEof = 1U << 2;
static constexpr uint8_t kReplayStatusFlagTeensyReplaySeen = 1U << 3;
static constexpr uint8_t kReplayStatusFlagPaused = 1U << 4;
static constexpr uint8_t kStorageStatusFlagMounted = 1U << 0;
static constexpr uint8_t kStorageStatusFlagBackendReady = 1U << 1;
static constexpr uint8_t kStorageStatusFlagMediaPresent = 1U << 2;
static constexpr uint8_t kStorageStatusFlagBusy = 1U << 3;

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

struct ReplayStatusPayloadV1 {
  uint8_t flags;
  uint8_t reserved0;
  uint16_t last_command;
  uint32_t session_id;
  uint32_t records_total;
  uint32_t records_sent;
  uint32_t last_error;
  uint32_t last_change_ms;
  char current_file[kLogFileNameBytes];
};

struct LogFileInfoV1 {
  uint32_t size_bytes;
  char name[kLogFileNameBytes];
};

struct LogFileListChunkPayloadV1 {
  uint16_t total_files;
  uint16_t chunk_index;
  uint16_t chunk_count;
  uint16_t entries_in_chunk;
  LogFileInfoV1 entries[kLogFileChunkEntries];
};

struct CmdNamedFileV1 {
  char name[kLogFileNameBytes];
};

struct CmdRenameLogFileV1 {
  char src_name[kLogFileNameBytes];
  char dst_name[kLogFileNameBytes];
};

struct CmdRecordPrefixV1 {
  char prefix[kRecordPrefixBytes];
};

struct StorageStatusPayloadV1 {
  uint8_t media_state;
  uint8_t flags;
  uint16_t reserved0;
  uint32_t init_hz;
  uint32_t free_bytes;
  uint32_t total_bytes;
  uint16_t file_count;
  uint16_t reserved1;
  char record_prefix[kRecordPrefixBytes];
  char next_record_name[kLogFileNameBytes];
};

struct CmdReplaySeekRelV1 {
  int32_t delta_records;
};

struct ReplayRecordHeaderV1 {
  uint32_t magic;
  uint16_t version;
  uint8_t kind;
  uint8_t flags;
  uint32_t seq;
  uint32_t t_us;
};

struct ReplayInputPayloadV1 {
  uint32_t present_mask;
  uint32_t source_flags;
  uint32_t imu_seq;
  uint32_t gps_seq;
  uint32_t baro_seq;
  int32_t accel_milli_mps2[3];
  int32_t gyro_milli_dps[3];
  int32_t mag_milli_uT[3];
  uint32_t iTOW_ms;
  uint8_t fixType;
  uint8_t numSV;
  uint16_t gps_flags;
  int32_t lat_1e7;
  int32_t lon_1e7;
  int32_t hMSL_mm;
  int32_t gSpeed_mms;
  int32_t headMot_1e5deg;
  uint32_t hAcc_mm;
  uint32_t sAcc_mms;
  int32_t baro_temp_milli_c;
  int32_t baro_press_milli_hpa;
  int32_t baro_alt_mm;
  int32_t baro_vsi_milli_mps;
  uint8_t reserved[36];
};

struct ReplayInputRecord160 {
  ReplayRecordHeaderV1 hdr;
  ReplayInputPayloadV1 payload;
};

struct ReplayControlPayloadV1 {
  uint16_t command_id;
  uint16_t payload_len;
  uint32_t command_seq;
  uint32_t received_t_us;
  uint32_t apply_flags;
  uint8_t payload[128];
};

struct ReplayControlRecord160 {
  ReplayRecordHeaderV1 hdr;
  ReplayControlPayloadV1 payload;
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

static_assert(sizeof(TelemetryFullStateV1) == kTelemetryStateRecordBytes, "TelemetryFullStateV1 must be 160 bytes");
static_assert(sizeof(ReplayRecordHeaderV1) == 16U, "ReplayRecordHeaderV1 must be 16 bytes");
static_assert(sizeof(ReplayInputPayloadV1) == 144U, "ReplayInputPayloadV1 must be 144 bytes");
static_assert(sizeof(ReplayInputRecord160) == kReplayRecordBytes, "ReplayInputRecord160 must be 160 bytes");
static_assert(sizeof(ReplayControlPayloadV1) == 144U, "ReplayControlPayloadV1 must be 144 bytes");
static_assert(sizeof(ReplayControlRecord160) == kReplayRecordBytes, "ReplayControlRecord160 must be 160 bytes");

}  // namespace telem
