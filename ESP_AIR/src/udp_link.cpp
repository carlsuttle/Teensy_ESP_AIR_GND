#include "udp_link.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <string.h>

#include "types_shared.h"

namespace udp_link {
namespace {

WiFiUDP g_udp;
Stats g_stats;
IPAddress g_peer_ip(192, 168, 4, 2);
uint16_t g_local_port = 9000U;
uint16_t g_peer_port = 9000U;
IPAddress g_last_cmd_ip;
uint16_t g_last_cmd_port = 0U;
uint32_t g_last_state_seq = 0U;
uint32_t g_last_ack_seq = 0U;
uint32_t g_last_fusion_seq = 0U;
uint32_t g_tx_seq = 0U;
uint32_t g_last_socket_restart_ms = 0U;
uint32_t g_last_wifi_recover_ms = 0U;
uint16_t g_consecutive_tx_failures = 0U;
bool g_network_reset_requested = false;
constexpr uint32_t kSocketRestartBackoffMs = 250U;
constexpr uint32_t kWifiRecoverBackoffMs = 2000U;
constexpr uint16_t kWifiRecoverFailureThreshold = 8U;

bool parseIp(const char* text, IPAddress& out) {
  if (!text || !*text) return false;
  return out.fromString(text);
}

bool wifiLinkHealthy() {
  if (WiFi.status() != WL_CONNECTED) return false;
  wifi_ap_record_t ap_info = {};
  return esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
}

bool restartSocket() {
  g_udp.stop();
  return g_udp.begin(g_local_port) == 1;
}

void restartSocketAfterFailure() {
  const uint32_t now = millis();
  if ((uint32_t)(now - g_last_socket_restart_ms) < kSocketRestartBackoffMs) return;
  g_last_socket_restart_ms = now;
  (void)restartSocket();
}

void recoverWifiAfterFailure() {
  const uint32_t now = millis();
  if ((uint32_t)(now - g_last_wifi_recover_ms) < kWifiRecoverBackoffMs) return;
  g_last_wifi_recover_ms = now;
  g_udp.stop();
  WiFi.disconnect(false, false);
}

IPAddress targetIp() {
  if (g_last_cmd_port != 0U) return g_last_cmd_ip;
  return g_peer_ip;
}

uint16_t targetPort() {
  if (g_last_cmd_port != 0U) return g_last_cmd_port;
  return g_peer_port;
}

bool sendFrame(telem::MsgType type, const void* payload, size_t payload_len, uint32_t seq, uint32_t t_us) {
  if (payload_len > 1024U) return false;
  if (!wifiLinkHealthy()) return false;
  IPAddress ip = targetIp();
  const uint16_t port = targetPort();
  if (ip == INADDR_NONE || port == 0U) return false;

  uint8_t buf[sizeof(telem::FrameHeader) + 1024U] = {};
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

  if (!g_udp.beginPacket(ip, port)) {
    g_stats.tx_drop++;
    g_consecutive_tx_failures++;
    restartSocketAfterFailure();
    if (g_consecutive_tx_failures >= kWifiRecoverFailureThreshold) {
      recoverWifiAfterFailure();
    }
    return false;
  }
  const size_t wrote = g_udp.write(buf, sizeof(hdr) + payload_len);
  if (!g_udp.endPacket() || wrote != sizeof(hdr) + payload_len) {
    g_stats.tx_drop++;
    g_consecutive_tx_failures++;
    restartSocketAfterFailure();
    if (g_consecutive_tx_failures >= kWifiRecoverFailureThreshold) {
      recoverWifiAfterFailure();
    }
    return false;
  }
  g_consecutive_tx_failures = 0U;
  g_stats.tx_packets++;
  g_stats.tx_bytes += (uint32_t)wrote;
  return true;
}

void sendCommandAck(uint16_t command, bool ok, uint32_t code, uint32_t seq, uint32_t t_us) {
  telem::AckPayloadV1 ack = {};
  ack.command = command;
  ack.ok = ok ? 1U : 0U;
  ack.code = code;
  const telem::MsgType type = ok ? telem::ACK : telem::NACK;
  (void)sendFrame(type, &ack, sizeof(ack), seq, t_us);
}

void handleCommand(const telem::FrameHeader& hdr, const uint8_t* payload) {
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
      g_stats.rx_unknown++;
      break;
  }
}

}  // namespace

void begin(const AppConfig& cfg) {
  memset(&g_stats, 0, sizeof(g_stats));
  g_last_cmd_ip = IPAddress();
  g_last_cmd_port = 0U;
  g_last_state_seq = 0U;
  g_last_ack_seq = 0U;
  g_last_fusion_seq = 0U;
  g_tx_seq = 0U;
  g_last_wifi_recover_ms = 0U;
  g_consecutive_tx_failures = 0U;
  g_network_reset_requested = false;
  reconfigure(cfg);
}

void reconfigure(const AppConfig& cfg) {
  IPAddress parsed;
  if (parseIp(cfg.gnd_ip, parsed)) {
    g_peer_ip = parsed;
  }
  g_local_port = cfg.udp_local_port;
  g_peer_port = cfg.udp_gnd_port;
  g_last_socket_restart_ms = 0U;
  g_consecutive_tx_failures = 0U;
  restartSocket();
}

void poll() {
  int packet_size = g_udp.parsePacket();
  while (packet_size > 0) {
    if (packet_size < (int)sizeof(telem::FrameHeader) || packet_size > 1500) {
      g_stats.rx_bad_len++;
      while (packet_size-- > 0) (void)g_udp.read();
      packet_size = g_udp.parsePacket();
      continue;
    }
    uint8_t buf[1500] = {};
    const int got = g_udp.read(buf, sizeof(buf));
    if (got < (int)sizeof(telem::FrameHeader)) {
      g_stats.rx_bad_len++;
      packet_size = g_udp.parsePacket();
      continue;
    }
    g_stats.rx_packets++;
    g_stats.rx_bytes += (uint32_t)got;
    g_stats.last_rx_ms = millis();
    g_last_cmd_ip = g_udp.remoteIP();
    g_last_cmd_port = g_udp.remotePort();

    telem::FrameHeader hdr = {};
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != telem::kMagic || hdr.version != telem::kVersion) {
      g_stats.rx_bad_magic++;
      packet_size = g_udp.parsePacket();
      continue;
    }
    if ((size_t)got != sizeof(hdr) + hdr.payload_len) {
      g_stats.rx_bad_len++;
      packet_size = g_udp.parsePacket();
      continue;
    }
    handleCommand(hdr, buf + sizeof(hdr));
    packet_size = g_udp.parsePacket();
  }
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
    const telem::MsgType type = snap.ack_ok ? telem::ACK : telem::NACK;
    g_last_ack_seq = snap.ack_rx_seq;
    (void)sendFrame(type, &ack, sizeof(ack), snap.ack_rx_seq, snap.t_us);
  }
}

bool publishState(const telem::TelemetryFullStateV1& state, uint32_t seq, uint32_t t_us) {
  if (seq == g_last_state_seq) return true;
  g_last_state_seq = seq;
  return sendFrame(telem::TELEM_FULL_STATE, &state, sizeof(state), seq, t_us);
}

bool publishStressState(const telem::TelemetryFullStateV1& state, uint32_t seq, uint32_t t_us) {
  return sendFrame(telem::TELEM_FULL_STATE, &state, sizeof(state), seq, t_us);
}

Stats stats() { return g_stats; }

bool takeNetworkResetRequest() {
  const bool requested = g_network_reset_requested;
  g_network_reset_requested = false;
  return requested;
}

void resetNetworkState() {
  g_udp.stop();
  g_last_cmd_ip = IPAddress();
  g_last_cmd_port = 0U;
  g_last_state_seq = 0U;
  g_last_ack_seq = 0U;
  g_last_fusion_seq = 0U;
  g_last_socket_restart_ms = 0U;
  g_last_wifi_recover_ms = 0U;
  g_consecutive_tx_failures = 0U;
  g_stats.last_rx_ms = 0U;
}

}  // namespace udp_link
