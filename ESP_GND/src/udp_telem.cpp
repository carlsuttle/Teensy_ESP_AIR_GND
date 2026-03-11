#include "udp_telem.h"

#include <string.h>

namespace udp_telem {
namespace {

WiFiUDP g_udp;
Snapshot g_snapshot;
IPAddress g_last_sender_ip;
uint16_t g_last_sender_port = 0;
IPAddress g_default_sender_ip(192, 168, 4, 2);
uint16_t g_default_sender_port = 9000;
uint16_t g_listen_port = 0;
uint32_t g_tx_seq = 0;

bool restartListener(uint16_t port) {
  g_udp.stop();
  g_listen_port = port;
  return g_udp.begin(port) == 1;
}

bool learnedSenderValid() {
  return g_last_sender_port != 0U && g_last_sender_ip != INADDR_NONE;
}

IPAddress currentTargetIp() {
  if (learnedSenderValid()) return g_last_sender_ip;
  return g_default_sender_ip;
}

uint16_t currentTargetPort() {
  if (learnedSenderValid()) return g_last_sender_port;
  return g_default_sender_port;
}

bool sendFrame(telem::MsgType type, const void* payload, size_t payload_len) {
  if (payload_len > 1024U) return false;
  const IPAddress target_ip = currentTargetIp();
  const uint16_t target_port = currentTargetPort();
  if (target_ip == INADDR_NONE || target_port == 0U) return false;

  uint8_t buf[sizeof(telem::FrameHeader) + 1024U] = {};
  telem::FrameHeader hdr = {};
  hdr.magic = telem::kMagic;
  hdr.version = telem::kVersion;
  hdr.msg_type = static_cast<uint16_t>(type);
  hdr.payload_len = (uint16_t)payload_len;
  hdr.seq = ++g_tx_seq;
  hdr.t_us = micros();
  memcpy(buf, &hdr, sizeof(hdr));
  if (payload && payload_len > 0) {
    memcpy(buf + sizeof(hdr), payload, payload_len);
  }

  if (!g_udp.beginPacket(target_ip, target_port)) return false;
  const size_t wrote = g_udp.write(buf, sizeof(hdr) + payload_len);
  if (!g_udp.endPacket()) return false;
  return wrote == (sizeof(hdr) + payload_len);
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

}  // namespace

void begin(const AppConfig& cfg) {
  memset(&g_snapshot, 0, sizeof(g_snapshot));
  g_last_sender_ip = IPAddress();
  g_last_sender_port = 0;
  g_default_sender_ip = IPAddress(192, 168, 4, 2);
  g_default_sender_port = cfg.udp_listen_port;
  g_tx_seq = 0;
  restartListener(cfg.udp_listen_port);
}

void reconfigure(const AppConfig& cfg) {
  if (cfg.udp_listen_port != g_listen_port) {
    restartListener(cfg.udp_listen_port);
  }
}

void restart(const AppConfig& cfg) {
  restartListener(cfg.udp_listen_port);
}

void poll() {
  int packet_size = g_udp.parsePacket();
  while (packet_size > 0) {
    if (packet_size > 1500) {
      g_snapshot.stats.drop++;
      while (packet_size-- > 0) (void)g_udp.read();
      packet_size = g_udp.parsePacket();
      continue;
    }

    uint8_t buf[1500] = {};
    const int got = g_udp.read(buf, sizeof(buf));
    if (got < (int)sizeof(telem::FrameHeader)) {
      g_snapshot.stats.len_err++;
      packet_size = g_udp.parsePacket();
      continue;
    }

    g_snapshot.stats.rx_packets++;
    g_snapshot.stats.rx_bytes += (uint32_t)got;
    g_last_sender_ip = g_udp.remoteIP();
    g_last_sender_port = g_udp.remotePort();

    telem::FrameHeader hdr = {};
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != telem::kMagic || hdr.version != telem::kVersion) {
      g_snapshot.stats.unknown_msg++;
      packet_size = g_udp.parsePacket();
      continue;
    }
    if ((size_t)got != sizeof(hdr) + hdr.payload_len) {
      g_snapshot.stats.len_err++;
      packet_size = g_udp.parsePacket();
      continue;
    }

    applyFrame(hdr, buf + sizeof(hdr));
    packet_size = g_udp.parsePacket();
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

bool hasLearnedSender() { return learnedSenderValid(); }

IPAddress targetSenderIp() { return currentTargetIp(); }

uint16_t targetSenderPort() { return currentTargetPort(); }

IPAddress lastSenderIp() { return g_last_sender_ip; }

uint16_t lastSenderPort() { return g_last_sender_port; }

}  // namespace udp_telem
