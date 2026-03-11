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
uint8_t g_consecutive_send_failures = 0U;
uint32_t g_tx_seq = 0U;
uint32_t g_session_id = 0U;

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
  g_consecutive_send_failures = 0U;
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

  if (learnPeer(mac)) {
    (void)sendHelloTo(mac);
  }
}

void applyFrame(const telem::FrameHeader& hdr, const uint8_t* payload) {
  switch ((telem::MsgType)hdr.msg_type) {
    case telem::TELEM_FULL_STATE:
      if (hdr.payload_len != sizeof(telem::TelemetryFullStateV1)) {
        g_snapshot.stats.len_err++;
        return;
      }
      memcpy(&g_snapshot.state, payload, sizeof(g_snapshot.state));
      g_snapshot.has_state = true;
      g_snapshot.seq = hdr.seq;
      g_snapshot.t_us = hdr.t_us;
      g_snapshot.stats.frames_ok++;
      g_snapshot.stats.last_rx_ms = millis();
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
    case telem::ACK:
    case telem::NACK: {
      if (hdr.payload_len != sizeof(telem::AckPayloadV1)) {
        g_snapshot.stats.len_err++;
        return;
      }
      telem::AckPayloadV1 ack = {};
      memcpy(&ack, payload, sizeof(ack));
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

}  // namespace

void begin(const AppConfig& cfg) {
  (void)cfg;
  memset(&g_snapshot, 0, sizeof(g_snapshot));
  clearPeerState();
  g_espnow_ready = false;
  g_tx_seq = 0U;
  g_session_id = esp_random();
  (void)initEspNow();
}

void reconfigure(const AppConfig& cfg) {
  (void)cfg;
  if (!initEspNow()) return;
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
}

Snapshot snapshot() { return g_snapshot; }

bool sendSetFusionSettings(const telem::CmdSetFusionSettingsV1& cmd) {
  return sendFrame(telem::CMD_SET_FUSION_SETTINGS, &cmd, sizeof(cmd));
}

bool sendGetFusionSettings() { return sendFrame(telem::CMD_GET_FUSION_SETTINGS, nullptr, 0U); }

bool sendSetStreamRate(const telem::CmdSetStreamRateV1& cmd) {
  return sendFrame(telem::CMD_SET_STREAM_RATE, &cmd, sizeof(cmd));
}

bool sendResetNetwork() { return sendFrame(telem::CMD_RESET_NETWORK, nullptr, 0U); }

bool hasLearnedSender() { return g_has_air_mac; }

String targetSenderMac() { return macToString(g_air_mac); }

String lastSenderMac() { return macToString(g_last_sender_mac); }

}  // namespace radio_link
