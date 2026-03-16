#include "radio_link.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <string.h>

namespace radio_link {
namespace {

constexpr uint8_t kUnitAir = 1U;
constexpr uint8_t kUnitGnd = 2U;
constexpr uint8_t kBroadcastMac[6] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
constexpr size_t kRxQueueCapacity = 8U;
constexpr uint8_t kSendFailThreshold = 6U;
constexpr uint32_t kRadioPingIntervalMs = 1000U;
constexpr uint32_t kRadioPingTimeoutMs = 1000U;
constexpr uint8_t kRadioRttSampleCount = 8U;

struct RxFrame {
  uint8_t mac[6] = {};
  uint16_t len = 0U;
  uint8_t data[telem::kEspNowMaxDataLen] = {};
};

portMUX_TYPE g_rx_mux = portMUX_INITIALIZER_UNLOCKED;
RxFrame g_rx_queue[kRxQueueCapacity];
volatile uint8_t g_rx_head = 0U;
volatile uint8_t g_rx_tail = 0U;
volatile uint32_t g_rx_drop_events = 0U;
volatile uint32_t g_send_ok_events = 0U;
volatile uint32_t g_send_fail_events = 0U;

Snapshot g_snapshot;
uint8_t g_air_mac[6] = {};
uint8_t g_last_sender_mac[6] = {};
bool g_has_air_mac = false;
bool g_has_last_sender_mac = false;
bool g_espnow_ready = false;
bool g_radio_lr_mode = true;
uint8_t g_consecutive_send_failures = 0U;
uint32_t g_tx_seq = 0U;
uint32_t g_session_id = 0U;
uint32_t g_air_session_id = 0U;
uint32_t g_last_state_seq = 0U;
bool g_radio_ping_pending = false;
bool g_has_air_session_id = false;
bool g_has_last_state_seq = false;
uint32_t g_last_radio_ping_tx_ms = 0U;
uint32_t g_last_radio_ping_attempt_ms = 0U;
uint32_t g_last_radio_ping_seq = 0U;
uint16_t g_radio_rtt_samples[kRadioRttSampleCount] = {};
uint8_t g_radio_rtt_head = 0U;
uint8_t g_radio_rtt_count = 0U;

bool stateOnlyModeEnabled() {
  return config_store::get().radio_state_only != 0U;
}

uint8_t desiredProtocol() {
  return (uint8_t)(WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
}

void applyRadioProtocol() {
  (void)esp_wifi_set_protocol(WIFI_IF_AP, desiredProtocol());
}

bool isBroadcastMac(const uint8_t* mac) {
  return mac && memcmp(mac, kBroadcastMac, sizeof(kBroadcastMac)) == 0;
}

bool isZeroMac(const uint8_t* mac) {
  static const uint8_t kZeroMac[6] = {};
  return !mac || memcmp(mac, kZeroMac, sizeof(kZeroMac)) == 0;
}

void copyMac(uint8_t* dst, const uint8_t* src) {
  if (dst && src) memcpy(dst, src, 6U);
}

String macToString(const uint8_t* mac) {
  if (isZeroMac(mac)) return String("none");
  char text[18];
  snprintf(text,
           sizeof(text),
           "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],
           mac[1],
           mac[2],
           mac[3],
           mac[4],
           mac[5]);
  return String(text);
}

void clearPeerState() {
  memset(g_air_mac, 0, sizeof(g_air_mac));
  memset(g_last_sender_mac, 0, sizeof(g_last_sender_mac));
  g_has_air_mac = false;
  g_has_last_sender_mac = false;
  g_air_session_id = 0U;
  g_last_state_seq = 0U;
  g_consecutive_send_failures = 0U;
  g_radio_ping_pending = false;
  g_has_air_session_id = false;
  g_has_last_state_seq = false;
  g_last_radio_ping_tx_ms = 0U;
  g_last_radio_ping_attempt_ms = 0U;
  g_last_radio_ping_seq = 0U;
  memset(g_radio_rtt_samples, 0, sizeof(g_radio_rtt_samples));
  g_radio_rtt_head = 0U;
  g_radio_rtt_count = 0U;
  g_snapshot.radio_rtt_ms = 0U;
  g_snapshot.radio_rtt_avg_ms = 0U;
  g_snapshot.last_radio_pong_ms = 0U;
}

void resetStateSequenceTracking(bool preserveCurrentState) {
  if (preserveCurrentState && g_snapshot.has_state) {
    g_last_state_seq = g_snapshot.seq;
    g_has_last_state_seq = true;
  } else {
    g_last_state_seq = 0U;
    g_has_last_state_seq = false;
  }
}

void resetStatsInternal() {
  const uint32_t last_rx_ms = g_snapshot.stats.last_rx_ms;
  g_snapshot.stats = {};
  g_snapshot.stats.last_rx_ms = last_rx_ms;
  g_radio_ping_pending = false;
  g_last_radio_ping_tx_ms = 0U;
  g_last_radio_ping_attempt_ms = 0U;
  g_last_radio_ping_seq = 0U;
  memset(g_radio_rtt_samples, 0, sizeof(g_radio_rtt_samples));
  g_radio_rtt_head = 0U;
  g_radio_rtt_count = 0U;
  g_snapshot.radio_rtt_ms = 0U;
  g_snapshot.radio_rtt_avg_ms = 0U;
  g_snapshot.last_radio_pong_ms = 0U;
  g_snapshot.uplink_ping_sent = 0U;
  g_snapshot.uplink_ping_ok = 0U;
  g_snapshot.uplink_ping_timeout = 0U;
  g_snapshot.uplink_ping_miss_streak = 0U;
  g_snapshot.last_uplink_ack_ms = 0U;
  resetStateSequenceTracking(true);
}

void resetRadioRttTracking() {
  g_radio_ping_pending = false;
  g_last_radio_ping_tx_ms = 0U;
  g_last_radio_ping_attempt_ms = 0U;
  g_last_radio_ping_seq = 0U;
  memset(g_radio_rtt_samples, 0, sizeof(g_radio_rtt_samples));
  g_radio_rtt_head = 0U;
  g_radio_rtt_count = 0U;
  g_snapshot.radio_rtt_ms = 0U;
  g_snapshot.radio_rtt_avg_ms = 0U;
  g_snapshot.last_radio_pong_ms = 0U;
  g_snapshot.uplink_ping_miss_streak = 0U;
}

bool ensurePeer(const uint8_t* mac) {
  if (isZeroMac(mac)) return false;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, sizeof(peer.peer_addr));
  peer.channel = telem::kRadioChannel;
  peer.ifidx = WIFI_IF_AP;
  peer.encrypt = false;
  if (esp_now_is_peer_exist(mac)) {
    return esp_now_mod_peer(&peer) == ESP_OK;
  }
  return esp_now_add_peer(&peer) == ESP_OK;
}

void onDataRecv(const uint8_t* mac_addr, const uint8_t* data, int data_len) {
  if (!mac_addr || !data || data_len <= 0 || data_len > (int)telem::kEspNowMaxDataLen) return;

  bool queued = false;
  portENTER_CRITICAL_ISR(&g_rx_mux);
  const uint8_t next_head = (uint8_t)((g_rx_head + 1U) % kRxQueueCapacity);
  if (next_head != g_rx_tail) {
    RxFrame& frame = g_rx_queue[g_rx_head];
    memcpy(frame.mac, mac_addr, sizeof(frame.mac));
    frame.len = (uint16_t)data_len;
    memcpy(frame.data, data, (size_t)data_len);
    g_rx_head = next_head;
    queued = true;
  }
  portEXIT_CRITICAL_ISR(&g_rx_mux);

  if (!queued) g_rx_drop_events++;
}

void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
  (void)mac_addr;
  if (status == ESP_NOW_SEND_SUCCESS) {
    g_send_ok_events++;
  } else {
    g_send_fail_events++;
  }
}

bool popRxFrame(RxFrame& out) {
  bool have_frame = false;
  portENTER_CRITICAL(&g_rx_mux);
  if (g_rx_tail != g_rx_head) {
    out = g_rx_queue[g_rx_tail];
    g_rx_tail = (uint8_t)((g_rx_tail + 1U) % kRxQueueCapacity);
    have_frame = true;
  }
  portEXIT_CRITICAL(&g_rx_mux);
  return have_frame;
}

void drainAsyncEvents() {
  uint32_t rx_drop = 0U;
  uint32_t send_ok = 0U;
  uint32_t send_fail = 0U;

  portENTER_CRITICAL(&g_rx_mux);
  rx_drop = g_rx_drop_events;
  g_rx_drop_events = 0U;
  send_ok = g_send_ok_events;
  g_send_ok_events = 0U;
  send_fail = g_send_fail_events;
  g_send_fail_events = 0U;
  portEXIT_CRITICAL(&g_rx_mux);

  if (rx_drop > 0U) {
    g_snapshot.stats.drop += rx_drop;
  }
  if (send_ok > 0U) {
    g_consecutive_send_failures = 0U;
  }
  if (send_fail > 0U && g_has_air_mac) {
    const uint32_t total = (uint32_t)g_consecutive_send_failures + send_fail;
    g_consecutive_send_failures = (total > 0xFFU) ? 0xFFU : (uint8_t)total;
    if (g_consecutive_send_failures >= kSendFailThreshold) clearPeerState();
  }
}

bool initEspNow() {
  if (g_espnow_ready) return true;

  if (esp_now_init() != ESP_OK) {
    return false;
  }
  if (esp_now_register_recv_cb(onDataRecv) != ESP_OK) {
    esp_now_deinit();
    return false;
  }
  if (esp_now_register_send_cb(onDataSent) != ESP_OK) {
    esp_now_deinit();
    return false;
  }
  if (!ensurePeer(kBroadcastMac)) {
    esp_now_deinit();
    return false;
  }

  applyRadioProtocol();
  g_espnow_ready = true;
  return true;
}

bool learnPeer(const uint8_t* mac) {
  if (isZeroMac(mac) || isBroadcastMac(mac)) return false;
  if (!initEspNow()) return false;
  if (!ensurePeer(mac)) return false;

  copyMac(g_last_sender_mac, mac);
  g_has_last_sender_mac = true;
  if (!g_has_air_mac || memcmp(g_air_mac, mac, sizeof(g_air_mac)) != 0) {
    copyMac(g_air_mac, mac);
    g_has_air_mac = true;
  }
  g_consecutive_send_failures = 0U;
  return true;
}

bool sendFrameTo(const uint8_t* mac, telem::MsgType type, const void* payload, size_t payload_len) {
  constexpr size_t kMaxPayloadLen = telem::kEspNowMaxDataLen - sizeof(telem::FrameHeader);
  if (payload_len > kMaxPayloadLen || isZeroMac(mac)) return false;
  if (!initEspNow()) return false;

  uint8_t buf[telem::kEspNowMaxDataLen] = {};
  telem::FrameHeader hdr = {};
  hdr.magic = telem::kMagic;
  hdr.version = telem::kVersion;
  hdr.msg_type = static_cast<uint16_t>(type);
  hdr.payload_len = (uint16_t)payload_len;
  hdr.seq = ++g_tx_seq;
  hdr.t_us = micros();
  memcpy(buf, &hdr, sizeof(hdr));
  if (payload && payload_len > 0U) {
    memcpy(buf + sizeof(hdr), payload, payload_len);
  }

  const size_t frame_len = sizeof(hdr) + payload_len;
  const esp_err_t err = esp_now_send(mac, buf, frame_len);
  if (err != ESP_OK) {
    if (!isBroadcastMac(mac)) {
      g_consecutive_send_failures++;
      if (g_consecutive_send_failures >= kSendFailThreshold) clearPeerState();
    }
    if (err == ESP_ERR_ESPNOW_NOT_INIT) g_espnow_ready = false;
    return false;
  }
  return true;
}

bool sendHelloTo(const uint8_t* mac) {
  telem::LinkHelloPayloadV1 hello = {};
  hello.unit_id = kUnitGnd;
  hello.session_id = g_session_id;
  return sendFrameTo(mac, telem::LINK_HELLO, &hello, sizeof(hello));
}

bool sendFrame(telem::MsgType type, const void* payload, size_t payload_len) {
  if (!g_has_air_mac) return false;
  return sendFrameTo(g_air_mac, type, payload, payload_len);
}

void recordRadioRttSample(uint32_t rtt_ms) {
  g_snapshot.radio_rtt_ms = rtt_ms;
  g_snapshot.last_radio_pong_ms = millis();
  g_snapshot.last_uplink_ack_ms = g_snapshot.last_radio_pong_ms;
  g_snapshot.uplink_ping_ok++;
  g_snapshot.uplink_ping_miss_streak = 0U;
  g_radio_rtt_samples[g_radio_rtt_head] = (uint16_t)((rtt_ms > 0xFFFFU) ? 0xFFFFU : rtt_ms);
  g_radio_rtt_head = (uint8_t)((g_radio_rtt_head + 1U) % kRadioRttSampleCount);
  if (g_radio_rtt_count < kRadioRttSampleCount) g_radio_rtt_count++;

  uint32_t sum = 0U;
  for (uint8_t i = 0; i < g_radio_rtt_count; ++i) {
    sum += g_radio_rtt_samples[i];
  }
  g_snapshot.radio_rtt_avg_ms = g_radio_rtt_count ? (sum / g_radio_rtt_count) : 0U;
}

bool sendRadioPing() {
  if (!g_has_air_mac) return false;
  const uint32_t seq = g_tx_seq + 1U;
  if (!sendFrameTo(g_air_mac, telem::CMD_RADIO_PING, nullptr, 0U)) return false;
  g_radio_ping_pending = true;
  g_last_radio_ping_seq = seq;
  g_last_radio_ping_tx_ms = millis();
  g_last_radio_ping_attempt_ms = g_last_radio_ping_tx_ms;
  g_snapshot.uplink_ping_sent++;
  return true;
}

void handleHello(const uint8_t* mac, const telem::FrameHeader& hdr, const uint8_t* payload) {
  if (hdr.payload_len != sizeof(telem::LinkHelloPayloadV1)) {
    g_snapshot.stats.len_err++;
    return;
  }

  telem::LinkHelloPayloadV1 hello = {};
  memcpy(&hello, payload, sizeof(hello));
  if (hello.unit_id != kUnitAir) {
    g_snapshot.stats.unknown_msg++;
    return;
  }
  if (!g_has_air_session_id || hello.session_id != g_air_session_id) {
    g_air_session_id = hello.session_id;
    g_has_air_session_id = true;
    resetStateSequenceTracking(false);
  }

  if (learnPeer(mac)) {
    (void)sendHelloTo(mac);
  }
}

void applyControlStatusPayload(const telem::ControlStatusPayloadV1& control, const telem::FrameHeader& hdr) {
  if ((control.flags & telem::kControlStatusFlagHasAck) != 0U) {
    const bool ack_ok = (control.flags & telem::kControlStatusFlagAckOk) != 0U;
    if (control.ack_command == telem::CMD_RADIO_PING) {
      if (g_radio_ping_pending && ack_ok) {
        const uint32_t rtt_ms = (uint32_t)(millis() - g_last_radio_ping_tx_ms);
        recordRadioRttSample(rtt_ms);
      } else if (g_radio_ping_pending && !ack_ok) {
        g_snapshot.uplink_ping_timeout++;
        g_snapshot.uplink_ping_miss_streak++;
      }
      g_radio_ping_pending = false;
    }
    g_snapshot.has_ack = true;
    g_snapshot.ack_command = control.ack_command;
    g_snapshot.ack_ok = (control.flags & telem::kControlStatusFlagAckOk) != 0U;
    g_snapshot.ack_code = control.ack_code;
    g_snapshot.ack_rx_seq = hdr.seq;
  }
  if ((control.flags & telem::kControlStatusFlagHasFusion) != 0U) {
    g_snapshot.fusion_settings = control.fusion;
    g_snapshot.has_fusion_settings = true;
    g_snapshot.state.fusion_gain = control.fusion.gain;
    g_snapshot.state.fusion_accel_rej = control.fusion.accelerationRejection;
    g_snapshot.state.fusion_mag_rej = control.fusion.magneticRejection;
    g_snapshot.state.fusion_recovery_period = control.fusion.recoveryTriggerPeriod;
  }
  if ((control.flags & telem::kControlStatusFlagHasLinkMeta) != 0U) {
    g_snapshot.link_meta = control.link_meta;
    g_snapshot.has_link_meta = true;
  }
  if ((control.flags & telem::kControlStatusFlagHasLogStatus) != 0U) {
    g_snapshot.log_status = control.log_status;
    g_snapshot.has_log_status = true;
  }
  g_snapshot.state.mirror_tx_ok = control.mirror_tx_ok;
  g_snapshot.state.mirror_drop_count = control.mirror_drop_count;
}

void applyUnifiedDownlink(const telem::FrameHeader& hdr, const uint8_t* payload) {
  if (hdr.payload_len < sizeof(telem::UnifiedDownlinkBaseV1)) {
    g_snapshot.stats.len_err++;
    return;
  }

  telem::UnifiedDownlinkBaseV1 base = {};
  memcpy(&base, payload, sizeof(base));

  size_t offset = sizeof(base);
  telem::DownlinkGpsStateV1 gps = {};
  telem::ControlStatusPayloadV1 control = {};

  if ((base.section_flags & telem::kUnifiedDownlinkFlagHasGps) != 0U) {
    if (offset + sizeof(gps) > hdr.payload_len) {
      g_snapshot.stats.len_err++;
      return;
    }
    memcpy(&gps, payload + offset, sizeof(gps));
    offset += sizeof(gps);
  }

  if ((base.section_flags & telem::kUnifiedDownlinkFlagHasControl) != 0U) {
    if (offset + sizeof(control) > hdr.payload_len) {
      g_snapshot.stats.len_err++;
      return;
    }
    memcpy(&control, payload + offset, sizeof(control));
    offset += sizeof(control);
  }

  if (offset != hdr.payload_len) {
    g_snapshot.stats.len_err++;
    return;
  }

  if (g_has_last_state_seq) {
    if (hdr.seq > g_last_state_seq) {
      g_snapshot.stats.state_seq_gap += (hdr.seq - g_last_state_seq - 1U);
      g_last_state_seq = hdr.seq;
    } else {
      g_snapshot.stats.state_seq_rewind++;
    }
  } else {
    g_last_state_seq = hdr.seq;
    g_has_last_state_seq = true;
  }

  g_snapshot.state.roll_deg = base.fast.roll_deg;
  g_snapshot.state.pitch_deg = base.fast.pitch_deg;
  g_snapshot.state.yaw_deg = base.fast.yaw_deg;
  g_snapshot.state.mag_heading_deg = base.fast.mag_heading_deg;
  g_snapshot.state.last_imu_ms = base.fast.last_imu_ms;
  g_snapshot.state.baro_temp_c = base.fast.baro_temp_c;
  g_snapshot.state.baro_press_hpa = base.fast.baro_press_hpa;
  g_snapshot.state.baro_alt_m = base.fast.baro_alt_m;
  g_snapshot.state.baro_vsi_mps = base.fast.baro_vsi_mps;
  g_snapshot.state.last_baro_ms = base.fast.last_baro_ms;
  g_snapshot.state.flags = base.fast.flags;

  if ((base.section_flags & telem::kUnifiedDownlinkFlagHasGps) != 0U) {
    g_snapshot.state.iTOW_ms = gps.iTOW_ms;
    g_snapshot.state.fixType = gps.fixType;
    g_snapshot.state.numSV = gps.numSV;
    g_snapshot.state.lat_1e7 = gps.lat_1e7;
    g_snapshot.state.lon_1e7 = gps.lon_1e7;
    g_snapshot.state.hMSL_mm = gps.hMSL_mm;
    g_snapshot.state.gSpeed_mms = gps.gSpeed_mms;
    g_snapshot.state.headMot_1e5deg = gps.headMot_1e5deg;
    g_snapshot.state.hAcc_mm = gps.hAcc_mm;
    g_snapshot.state.sAcc_mms = gps.sAcc_mms;
    g_snapshot.state.gps_parse_errors = gps.gps_parse_errors;
    g_snapshot.state.last_gps_ms = gps.last_gps_ms;
  }

  if ((base.section_flags & telem::kUnifiedDownlinkFlagHasControl) != 0U) {
    applyControlStatusPayload(control, hdr);
  }

  g_snapshot.has_state = true;
  g_snapshot.seq = base.source_seq;
  g_snapshot.t_us = hdr.t_us;
  g_snapshot.stats.frames_ok++;
  g_snapshot.stats.state_packets++;
  g_snapshot.stats.last_rx_ms = millis();
}

void applyFrame(const telem::FrameHeader& hdr, const uint8_t* payload) {
  switch ((telem::MsgType)hdr.msg_type) {
    case telem::TELEM_FULL_STATE:
      if (hdr.payload_len != sizeof(telem::TelemetryFullStateV1)) {
        g_snapshot.stats.len_err++;
        return;
      }
      if (g_has_last_state_seq) {
        if (hdr.seq > g_last_state_seq) {
          g_snapshot.stats.state_seq_gap += (hdr.seq - g_last_state_seq - 1U);
          g_last_state_seq = hdr.seq;
        } else {
          g_snapshot.stats.state_seq_rewind++;
        }
      } else {
        g_last_state_seq = hdr.seq;
        g_has_last_state_seq = true;
      }
      memcpy(&g_snapshot.state, payload, sizeof(g_snapshot.state));
      g_snapshot.has_state = true;
      g_snapshot.seq = hdr.seq;
      g_snapshot.t_us = hdr.t_us;
      g_snapshot.stats.frames_ok++;
      g_snapshot.stats.state_packets++;
      g_snapshot.stats.last_rx_ms = millis();
      break;
    case telem::TELEM_UNIFIED_DOWNLINK:
      applyUnifiedDownlink(hdr, payload);
      break;
    case telem::TELEM_FUSION_SETTINGS:
      if (hdr.payload_len != sizeof(telem::FusionSettingsV1)) {
        g_snapshot.stats.len_err++;
        return;
      }
      memcpy(&g_snapshot.fusion_settings, payload, sizeof(g_snapshot.fusion_settings));
      g_snapshot.has_fusion_settings = true;
      g_snapshot.stats.frames_ok++;
      g_snapshot.stats.last_rx_ms = millis();
      break;
    case telem::TELEM_META:
      if (hdr.payload_len != sizeof(telem::LinkMetaPayloadV1)) {
        g_snapshot.stats.len_err++;
        return;
      }
      memcpy(&g_snapshot.link_meta, payload, sizeof(g_snapshot.link_meta));
      g_snapshot.has_link_meta = true;
      g_snapshot.stats.frames_ok++;
      g_snapshot.stats.last_rx_ms = millis();
      break;
    case telem::TELEM_LOG_STATUS:
      if (hdr.payload_len != sizeof(telem::LogStatusPayloadV1)) {
        g_snapshot.stats.len_err++;
        return;
      }
      memcpy(&g_snapshot.log_status, payload, sizeof(g_snapshot.log_status));
      g_snapshot.has_log_status = true;
      g_snapshot.stats.frames_ok++;
      g_snapshot.stats.last_rx_ms = millis();
      break;
    case telem::TELEM_CONTROL_STATUS: {
      if (hdr.payload_len != sizeof(telem::ControlStatusPayloadV1)) {
        g_snapshot.stats.len_err++;
        return;
      }
      telem::ControlStatusPayloadV1 control = {};
      memcpy(&control, payload, sizeof(control));
      applyControlStatusPayload(control, hdr);
      g_snapshot.stats.frames_ok++;
      g_snapshot.stats.last_rx_ms = millis();
      break;
    }
    case telem::ACK:
    case telem::NACK: {
      if (hdr.payload_len != sizeof(telem::AckPayloadV1)) {
        g_snapshot.stats.len_err++;
        return;
      }
      telem::AckPayloadV1 ack = {};
      memcpy(&ack, payload, sizeof(ack));
      if (ack.command == telem::CMD_RADIO_PING) {
        if (g_radio_ping_pending && hdr.seq == g_last_radio_ping_seq && ack.ok != 0U) {
          const uint32_t rtt_ms = (uint32_t)(millis() - g_last_radio_ping_tx_ms);
          recordRadioRttSample(rtt_ms);
        } else if (ack.ok == 0U) {
          g_snapshot.uplink_ping_timeout++;
          g_snapshot.uplink_ping_miss_streak++;
        }
        g_radio_ping_pending = false;
        g_snapshot.stats.frames_ok++;
        g_snapshot.stats.last_rx_ms = millis();
        return;
      }
      g_snapshot.has_ack = true;
      g_snapshot.ack_command = ack.command;
      g_snapshot.ack_ok = ack.ok != 0U;
      g_snapshot.ack_code = ack.code;
      g_snapshot.ack_rx_seq = hdr.seq;
      g_snapshot.stats.frames_ok++;
      g_snapshot.stats.last_rx_ms = millis();
      break;
    }
    default:
      g_snapshot.stats.unknown_msg++;
      break;
  }
}

void handleIncomingFrame(const RxFrame& frame) {
  g_snapshot.stats.rx_packets++;
  g_snapshot.stats.rx_bytes += frame.len;
  (void)learnPeer(frame.mac);

  if (frame.len < sizeof(telem::FrameHeader)) {
    g_snapshot.stats.len_err++;
    return;
  }

  telem::FrameHeader hdr = {};
  memcpy(&hdr, frame.data, sizeof(hdr));
  if (hdr.magic != telem::kMagic || hdr.version != telem::kVersion) {
    g_snapshot.stats.unknown_msg++;
    return;
  }
  if ((size_t)frame.len != sizeof(hdr) + hdr.payload_len) {
    g_snapshot.stats.len_err++;
    return;
  }

  const uint8_t* payload = frame.data + sizeof(hdr);
  if ((telem::MsgType)hdr.msg_type == telem::LINK_HELLO) {
    handleHello(frame.mac, hdr, payload);
    return;
  }

  applyFrame(hdr, payload);
}

void maybeSendRadioPing() {
  const uint32_t now = millis();
  if (!g_has_air_mac) return;
  if (g_radio_ping_pending) {
    if ((uint32_t)(now - g_last_radio_ping_tx_ms) >= kRadioPingTimeoutMs) {
      g_radio_ping_pending = false;
      g_snapshot.uplink_ping_timeout++;
      g_snapshot.uplink_ping_miss_streak++;
    }
    return;
  }
  if (g_last_radio_ping_attempt_ms != 0U &&
      (uint32_t)(now - g_last_radio_ping_attempt_ms) < kRadioPingIntervalMs) {
    return;
  }
  (void)sendRadioPing();
}

}  // namespace

void begin(const AppConfig& cfg) {
  memset(&g_snapshot, 0, sizeof(g_snapshot));
  clearPeerState();
  g_espnow_ready = false;
  g_radio_lr_mode = cfg.radio_lr_mode != 0U;
  g_tx_seq = 0U;
  g_session_id = esp_random();
  (void)initEspNow();
}

void reconfigure(const AppConfig& cfg) {
  g_radio_lr_mode = cfg.radio_lr_mode != 0U;
  applyRadioProtocol();
  if (!initEspNow()) return;
  if (cfg.radio_state_only) {
    resetRadioRttTracking();
  }
  (void)ensurePeer(kBroadcastMac);
  if (g_has_air_mac) {
    (void)ensurePeer(g_air_mac);
  }
}

void restart(const AppConfig& cfg) {
  clearPeerState();
  reconfigure(cfg);
}

void poll() {
  drainAsyncEvents();

  RxFrame frame = {};
  while (popRxFrame(frame)) {
    handleIncomingFrame(frame);
  }
  maybeSendRadioPing();
}

Snapshot snapshot() { return g_snapshot; }

void resetStats() { resetStatsInternal(); }

bool sendSetFusionSettings(const telem::CmdSetFusionSettingsV1& cmd) {
  return sendFrame(telem::CMD_SET_FUSION_SETTINGS, &cmd, sizeof(cmd));
}

bool sendGetFusionSettings() { return sendFrame(telem::CMD_GET_FUSION_SETTINGS, nullptr, 0U); }

bool sendSetStreamRate(const telem::CmdSetStreamRateV1& cmd) {
  return sendFrame(telem::CMD_SET_STREAM_RATE, &cmd, sizeof(cmd));
}

bool sendSetRadioMode(const telem::CmdSetRadioModeV1& cmd) {
  return sendFrame(telem::CMD_SET_RADIO_MODE, &cmd, sizeof(cmd));
}

bool sendResetNetwork() { return sendFrame(telem::CMD_RESET_NETWORK, nullptr, 0U); }

bool sendLogStart() { return sendFrame(telem::CMD_LOG_START, nullptr, 0U); }

bool sendLogStop() { return sendFrame(telem::CMD_LOG_STOP, nullptr, 0U); }

bool sendGetLogStatus() { return sendFrame(telem::CMD_GET_LOG_STATUS, nullptr, 0U); }

bool hasLearnedSender() { return g_has_air_mac; }

String targetSenderMac() { return macToString(g_air_mac); }

String lastSenderMac() { return macToString(g_last_sender_mac); }

}  // namespace radio_link
