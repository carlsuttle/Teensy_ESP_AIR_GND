#include "radio_link.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <string.h>

#include "types_shared.h"

namespace radio_link {
namespace {

constexpr uint8_t kUnitAir = 1U;
constexpr uint8_t kUnitGnd = 2U;
constexpr uint8_t kBroadcastMac[6] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
constexpr size_t kRxQueueCapacity = 8U;
constexpr uint32_t kHelloIntervalMs = 500U;
constexpr uint32_t kPeerStaleMs = 2000U;
constexpr uint32_t kRssiSampleIntervalMs = 5000U;
constexpr uint32_t kMetaIntervalMs = 1000U;
constexpr uint8_t kSendFailThreshold = 6U;

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

Stats g_stats;
uint8_t g_gnd_mac[6] = {};
uint8_t g_last_sender_mac[6] = {};
bool g_has_gnd_mac = false;
bool g_has_last_sender_mac = false;
bool g_espnow_ready = false;
bool g_network_reset_requested = false;
bool g_recorder_enabled = false;
bool g_rssi_valid = false;
uint8_t g_consecutive_send_failures = 0U;
uint32_t g_last_hello_tx_ms = 0U;
uint32_t g_last_meta_tx_ms = 0U;
uint32_t g_last_rssi_sample_ms = 0U;
uint32_t g_last_state_seq = 0U;
uint32_t g_last_ack_seq = 0U;
uint32_t g_last_fusion_seq = 0U;
uint32_t g_tx_seq = 0U;
uint32_t g_session_id = 0U;
int16_t g_gnd_ap_rssi_dbm = 0;
int16_t g_last_meta_rssi_sent = -32768;
uint8_t g_last_meta_flags_sent = 0xFFU;

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
  memset(g_gnd_mac, 0, sizeof(g_gnd_mac));
  memset(g_last_sender_mac, 0, sizeof(g_last_sender_mac));
  g_has_gnd_mac = false;
  g_has_last_sender_mac = false;
  g_consecutive_send_failures = 0U;
  g_last_hello_tx_ms = 0U;
  g_last_meta_tx_ms = 0U;
}

bool ensurePeer(const uint8_t* mac) {
  if (isZeroMac(mac)) return false;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, sizeof(peer.peer_addr));
  peer.channel = telem::kRadioChannel;
  peer.ifidx = WIFI_IF_STA;
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
    g_stats.rx_unknown += rx_drop;
  }
  if (send_ok > 0U) {
    g_consecutive_send_failures = 0U;
  }
  if (send_fail > 0U) {
    g_stats.tx_drop += send_fail;
    if (g_has_gnd_mac) {
      const uint32_t total = (uint32_t)g_consecutive_send_failures + send_fail;
      g_consecutive_send_failures = (total > 0xFFU) ? 0xFFU : (uint8_t)total;
      if (g_consecutive_send_failures >= kSendFailThreshold) {
        clearPeerState();
      }
    }
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

  g_espnow_ready = true;
  return true;
}

void shutdownEspNow() {
  if (!g_espnow_ready) return;
  esp_now_deinit();
  g_espnow_ready = false;
}

bool learnPeer(const uint8_t* mac) {
  if (isZeroMac(mac) || isBroadcastMac(mac)) return false;
  if (!initEspNow()) return false;
  if (!ensurePeer(mac)) return false;

  copyMac(g_last_sender_mac, mac);
  g_has_last_sender_mac = true;
  if (!g_has_gnd_mac || memcmp(g_gnd_mac, mac, sizeof(g_gnd_mac)) != 0) {
    copyMac(g_gnd_mac, mac);
    g_has_gnd_mac = true;
  }
  g_consecutive_send_failures = 0U;
  return true;
}

bool sendFrameTo(const uint8_t* mac,
                 telem::MsgType type,
                 const void* payload,
                 size_t payload_len,
                 uint32_t seq,
                 uint32_t t_us) {
  constexpr size_t kMaxPayloadLen = telem::kEspNowMaxDataLen - sizeof(telem::FrameHeader);
  if (payload_len > kMaxPayloadLen || isZeroMac(mac)) return false;
  if (!initEspNow()) return false;

  uint8_t buf[telem::kEspNowMaxDataLen] = {};
  telem::FrameHeader hdr = {};
  hdr.magic = telem::kMagic;
  hdr.version = telem::kVersion;
  hdr.msg_type = static_cast<uint16_t>(type);
  hdr.payload_len = (uint16_t)payload_len;
  hdr.seq = seq ? seq : ++g_tx_seq;
  hdr.t_us = t_us;
  memcpy(buf, &hdr, sizeof(hdr));
  if (payload && payload_len > 0U) {
    memcpy(buf + sizeof(hdr), payload, payload_len);
  }

  const size_t frame_len = sizeof(hdr) + payload_len;
  const esp_err_t err = esp_now_send(mac, buf, frame_len);
  if (err != ESP_OK) {
    g_stats.tx_drop++;
    if (!isBroadcastMac(mac)) {
      g_consecutive_send_failures++;
      if (g_consecutive_send_failures >= kSendFailThreshold) clearPeerState();
    }
    if (err == ESP_ERR_ESPNOW_NOT_INIT) g_espnow_ready = false;
    return false;
  }

  g_stats.tx_packets++;
  g_stats.tx_bytes += (uint32_t)frame_len;
  return true;
}

bool sendHelloTo(const uint8_t* mac) {
  telem::LinkHelloPayloadV1 hello = {};
  hello.unit_id = kUnitAir;
  hello.session_id = g_session_id;
  return sendFrameTo(mac, telem::LINK_HELLO, &hello, sizeof(hello), 0U, micros());
}

uint8_t currentMetaFlags() {
  uint8_t flags = 0U;
  if (g_has_gnd_mac) flags |= telem::kLinkMetaFlagPeerKnown;
  if (g_espnow_ready) flags |= telem::kLinkMetaFlagRadioReady;
  if (g_recorder_enabled) flags |= telem::kLinkMetaFlagRecorderOn;
  if (g_rssi_valid) flags |= telem::kLinkMetaFlagRssiValid;
  return flags;
}

telem::LinkMetaPayloadV1 currentLinkMeta() {
  telem::LinkMetaPayloadV1 meta = {};
  meta.gnd_ap_rssi_dbm = g_gnd_ap_rssi_dbm;
  meta.flags = currentMetaFlags();
  meta.scan_age_ms = g_last_rssi_sample_ms ? (uint32_t)(millis() - g_last_rssi_sample_ms) : 0xFFFFFFFFUL;
  meta.link_age_ms = g_stats.last_rx_ms ? (uint32_t)(millis() - g_stats.last_rx_ms) : 0xFFFFFFFFUL;
  return meta;
}

void sampleGroundRssi() {
  const uint32_t now = millis();
  if (!g_espnow_ready) return;
  if ((uint32_t)(now - g_last_rssi_sample_ms) < kRssiSampleIntervalMs) return;

  const AppConfig& cfg = config_store::get();
  int16_t found_rssi = -127;
  bool found = false;
  const int16_t networks = WiFi.scanNetworks(false, false, false, 80U, telem::kRadioChannel, cfg.ap_ssid, nullptr);
  if (networks > 0) {
    for (int16_t i = 0; i < networks; ++i) {
      const String ssid = WiFi.SSID((uint8_t)i);
      if (ssid != cfg.ap_ssid) continue;
      uint8_t* bssid = WiFi.BSSID((uint8_t)i);
      if (g_has_gnd_mac && bssid && memcmp(bssid, g_gnd_mac, sizeof(g_gnd_mac)) != 0) continue;
      const int32_t rssi = WiFi.RSSI((uint8_t)i);
      if (!found || rssi > found_rssi) {
        found_rssi = (int16_t)rssi;
        found = true;
      }
    }
  }
  WiFi.scanDelete();

  g_last_rssi_sample_ms = now;
  g_rssi_valid = found;
  if (found) {
    g_gnd_ap_rssi_dbm = found_rssi;
  }
}

void maybeSendLinkMeta() {
  if (!g_has_gnd_mac) return;

  const telem::LinkMetaPayloadV1 meta = currentLinkMeta();
  const bool changed = (meta.flags != g_last_meta_flags_sent) || (meta.gnd_ap_rssi_dbm != g_last_meta_rssi_sent);
  const bool due = g_last_meta_tx_ms == 0U || (uint32_t)(millis() - g_last_meta_tx_ms) >= kMetaIntervalMs;
  if (!changed && !due) return;
  if (!sendFrameTo(g_gnd_mac, telem::TELEM_META, &meta, sizeof(meta), 0U, micros())) return;

  g_last_meta_tx_ms = millis();
  g_last_meta_flags_sent = meta.flags;
  g_last_meta_rssi_sent = meta.gnd_ap_rssi_dbm;
}

bool sendFrame(telem::MsgType type, const void* payload, size_t payload_len, uint32_t seq, uint32_t t_us) {
  if (!g_has_gnd_mac) return false;
  return sendFrameTo(g_gnd_mac, type, payload, payload_len, seq, t_us);
}

void sendCommandAck(uint16_t command, bool ok, uint32_t code, uint32_t seq, uint32_t t_us) {
  telem::AckPayloadV1 ack = {};
  ack.command = command;
  ack.ok = ok ? 1U : 0U;
  ack.code = code;
  (void)sendFrame(ok ? telem::ACK : telem::NACK, &ack, sizeof(ack), seq, t_us);
}

void handleHello(const uint8_t* mac, const telem::FrameHeader& hdr, const uint8_t* payload) {
  if (hdr.payload_len != sizeof(telem::LinkHelloPayloadV1)) {
    g_stats.rx_bad_len++;
    return;
  }

  telem::LinkHelloPayloadV1 hello = {};
  memcpy(&hello, payload, sizeof(hello));
  if (hello.unit_id != kUnitGnd) {
    g_stats.rx_unknown++;
    return;
  }

  if (learnPeer(mac)) {
    g_stats.last_rx_ms = millis();
  }
}

void handleCommand(const telem::FrameHeader& hdr, const uint8_t* payload) {
  bool handled = true;
  switch ((telem::MsgType)hdr.msg_type) {
    case telem::CMD_SET_FUSION_SETTINGS: {
      if (hdr.payload_len != sizeof(telem::CmdSetFusionSettingsV1)) {
        g_stats.rx_bad_len++;
        return;
      }
      telem::CmdSetFusionSettingsV1 cmd = {};
      memcpy(&cmd, payload, sizeof(cmd));
      (void)uart_telem::sendSetFusionSettings(cmd);
      break;
    }
    case telem::CMD_GET_FUSION_SETTINGS:
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        return;
      }
      (void)uart_telem::sendGetFusionSettings();
      break;
    case telem::CMD_SET_STREAM_RATE: {
      if (hdr.payload_len != sizeof(telem::CmdSetStreamRateV1)) {
        g_stats.rx_bad_len++;
        return;
      }
      telem::CmdSetStreamRateV1 cmd = {};
      memcpy(&cmd, payload, sizeof(cmd));
      (void)uart_telem::sendSetStreamRate(cmd);
      break;
    }
    case telem::CMD_RESET_NETWORK:
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, 1U, hdr.seq, micros());
        return;
      }
      sendCommandAck(hdr.msg_type, true, 0U, hdr.seq, micros());
      g_network_reset_requested = true;
      break;
    default:
      handled = false;
      break;
  }

  if (handled) {
    g_stats.last_rx_ms = millis();
  } else {
    g_stats.rx_unknown++;
  }
}

void handleIncomingFrame(const RxFrame& frame) {
  g_stats.rx_packets++;
  g_stats.rx_bytes += frame.len;
  (void)learnPeer(frame.mac);

  if (frame.len < sizeof(telem::FrameHeader)) {
    g_stats.rx_bad_len++;
    return;
  }

  telem::FrameHeader hdr = {};
  memcpy(&hdr, frame.data, sizeof(hdr));
  if (hdr.magic != telem::kMagic || hdr.version != telem::kVersion) {
    g_stats.rx_bad_magic++;
    return;
  }
  if ((size_t)frame.len != sizeof(hdr) + hdr.payload_len) {
    g_stats.rx_bad_len++;
    return;
  }

  const uint8_t* payload = frame.data + sizeof(hdr);
  if ((telem::MsgType)hdr.msg_type == telem::LINK_HELLO) {
    handleHello(frame.mac, hdr, payload);
    return;
  }
  handleCommand(hdr, payload);
}

void maybeSendHello() {
  const uint32_t now = millis();
  if ((uint32_t)(now - g_last_hello_tx_ms) < kHelloIntervalMs) return;

  bool sent = false;
  if (!g_has_gnd_mac) {
    sent = sendHelloTo(kBroadcastMac);
  } else if (g_stats.last_rx_ms == 0U || (uint32_t)(now - g_stats.last_rx_ms) >= kPeerStaleMs) {
    sent = sendHelloTo(g_gnd_mac);
  }
  if (sent) g_last_hello_tx_ms = now;
}

}  // namespace

void begin(const AppConfig& cfg) {
  (void)cfg;
  memset(&g_stats, 0, sizeof(g_stats));
  clearPeerState();
  g_espnow_ready = false;
  g_network_reset_requested = false;
  g_last_state_seq = 0U;
  g_last_ack_seq = 0U;
  g_last_fusion_seq = 0U;
  g_tx_seq = 0U;
  g_session_id = esp_random();
  (void)initEspNow();
}

void reconfigure(const AppConfig& cfg) {
  (void)cfg;
  if (!initEspNow()) return;
  (void)ensurePeer(kBroadcastMac);
  if (g_has_gnd_mac) {
    (void)ensurePeer(g_gnd_mac);
  }
}

void poll() {
  drainAsyncEvents();

  RxFrame frame = {};
  while (popRxFrame(frame)) {
    handleIncomingFrame(frame);
  }

  sampleGroundRssi();
  maybeSendHello();
}

void publish(const uart_telem::Snapshot& snap) {
  if (snap.has_state && snap.seq != g_last_state_seq) {
    (void)publishState(snap.state, snap.seq, snap.t_us);
  }
  if (snap.has_fusion_settings && snap.fusion_rx_seq != g_last_fusion_seq) {
    g_last_fusion_seq = snap.fusion_rx_seq;
    (void)sendFrame(telem::TELEM_FUSION_SETTINGS,
                    &snap.fusion_settings,
                    sizeof(snap.fusion_settings),
                    snap.fusion_rx_seq,
                    snap.t_us);
  }
  if (snap.has_ack && snap.ack_rx_seq != g_last_ack_seq) {
    telem::AckPayloadV1 ack = {};
    ack.command = snap.ack_command;
    ack.ok = snap.ack_ok ? 1U : 0U;
    ack.code = snap.ack_code;
    g_last_ack_seq = snap.ack_rx_seq;
    (void)sendFrame(snap.ack_ok ? telem::ACK : telem::NACK, &ack, sizeof(ack), snap.ack_rx_seq, snap.t_us);
  }
  maybeSendLinkMeta();
}

bool publishState(const telem::TelemetryFullStateV1& state, uint32_t seq, uint32_t t_us) {
  if (seq == g_last_state_seq) return true;
  if (!g_has_gnd_mac) return false;
  g_last_state_seq = seq;
  return sendFrame(telem::TELEM_FULL_STATE, &state, sizeof(state), seq, t_us);
}

bool publishStressState(const telem::TelemetryFullStateV1& state, uint32_t seq, uint32_t t_us) {
  return g_has_gnd_mac && sendFrame(telem::TELEM_FULL_STATE, &state, sizeof(state), seq, t_us);
}

Stats stats() { return g_stats; }

bool hasPeer() { return g_has_gnd_mac; }

String peerMac() { return macToString(g_gnd_mac); }

bool radioReady() { return g_espnow_ready; }

void setRecorderEnabled(bool enabled) { g_recorder_enabled = enabled; }

bool takeNetworkResetRequest() {
  const bool requested = g_network_reset_requested;
  g_network_reset_requested = false;
  return requested;
}

void resetNetworkState() {
  shutdownEspNow();
  clearPeerState();
  g_last_state_seq = 0U;
  g_last_ack_seq = 0U;
  g_last_fusion_seq = 0U;
  g_rssi_valid = false;
  g_last_rssi_sample_ms = 0U;
  g_last_meta_flags_sent = 0xFFU;
  g_last_meta_rssi_sent = -32768;
  g_stats.last_rx_ms = 0U;
}

}  // namespace radio_link
