#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <tcpip_adapter.h>

#include "config_store.h"
#include "radio_link.h"
#include "ws_server.h"

namespace {

uint8_t g_last_station_count = 0xFFU;
uint32_t g_last_stat_ms = 0;
uint32_t g_last_air_probe_ms = 0;
bool g_stats_streaming = false;
bool g_link_streaming = false;
bool g_air_ready = false;
bool g_air_wait_announced = false;
bool g_air_bootstrap_active = true;
uint8_t g_air_probe_attempts = 0;
constexpr uint32_t kAirProbeRetryMs = 1000U;
constexpr uint8_t kAirProbeMaxAttempts = 12U;

void logApState() {
  const uint8_t station_count = WiFi.softAPgetStationNum();
  if (station_count == g_last_station_count) return;
  g_last_station_count = station_count;
  Serial.printf("AP ssid=%s ip=%s clients=%u\n",
                config_store::get().ap_ssid,
                WiFi.softAPIP().toString().c_str(),
                (unsigned)station_count);
}

void printConsoleHelp() {
  Serial.println("GND COMMANDS:");
  Serial.println("  help / h  - show command list");
  Serial.println("  kickair   - resend current stream-rate command to AIR");
  Serial.println("  resetair  - send AIR network reset command");
  Serial.println("  relink    - restart GND radio link state");
  Serial.println("  seelink   - start 1Hz AIR link metadata stream");
  Serial.println("  stats     - start 1Hz status stream");
  Serial.println("  x         - stop status stream");
}

bool sendConfiguredStreamRateToAir() {
  const AppConfig& cfg = config_store::get();
  telem::CmdSetStreamRateV1 cmd = {};
  cmd.ws_rate_hz = cfg.source_rate_hz;
  cmd.log_rate_hz = cfg.source_rate_hz;
  return radio_link::sendSetStreamRate(cmd);
}

void handleConsoleCommands() {
  static String line;
  while (Serial.available() > 0) {
    const char ch = (char)Serial.read();
    if (ch == '\r' || ch == '\n') {
      line.trim();
      if (line.equalsIgnoreCase("help") || line.equalsIgnoreCase("h")) {
        printConsoleHelp();
      } else if (line.equalsIgnoreCase("kickair")) {
        const AppConfig& cfg = config_store::get();
        const bool ok = sendConfiguredStreamRateToAir();
        Serial.printf("KICKAIR tx_ok=%u target=%s ws_hz=%u log_hz=%u\n",
                      ok ? 1U : 0U,
                      radio_link::targetSenderMac().c_str(),
                      (unsigned)cfg.source_rate_hz,
                      (unsigned)cfg.source_rate_hz);
      } else if (line.equalsIgnoreCase("resetair")) {
        const bool ok = radio_link::sendResetNetwork();
        Serial.printf("RESETAIR tx_ok=%u target=%s\n",
                      ok ? 1U : 0U,
                      radio_link::targetSenderMac().c_str());
      } else if (line.equalsIgnoreCase("relink")) {
        const AppConfig& cfg = config_store::get();
        radio_link::restart(cfg);
        Serial.printf("RELINK target=%s\n", radio_link::targetSenderMac().c_str());
      } else if (line.equalsIgnoreCase("stats")) {
        g_stats_streaming = true;
        g_link_streaming = false;
        Serial.println("STATS START");
      } else if (line.equalsIgnoreCase("seelink")) {
        g_link_streaming = true;
        g_stats_streaming = false;
        Serial.println("SEELINK START");
      } else if (line.equalsIgnoreCase("x")) {
        if (g_stats_streaming) Serial.println("STATS STOP");
        if (g_link_streaming) Serial.println("SEELINK STOP");
        g_stats_streaming = false;
        g_link_streaming = false;
      }
      line = "";
    } else if (isPrintable((unsigned char)ch)) {
      line += ch;
    }
  }
}

void configureDhcpLeaseRange() {
  esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap_netif) {
    Serial.println("AP netif handle missing");
    return;
  }

  dhcps_lease_t lease = {};
  lease.enable = true;
  IP4_ADDR(&lease.start_ip, 192, 168, 4, 50);
  IP4_ADDR(&lease.end_ip, 192, 168, 4, 100);

  if (esp_netif_dhcps_stop(ap_netif) != ESP_OK) {
    Serial.println("DHCP server stop failed");
    return;
  }
  if (esp_netif_dhcps_option(ap_netif,
                             ESP_NETIF_OP_SET,
                             ESP_NETIF_REQUESTED_IP_ADDRESS,
                             &lease,
                             sizeof(lease)) != ESP_OK) {
    Serial.println("DHCP lease range set failed");
  }
  if (esp_netif_dhcps_start(ap_netif) != ESP_OK) {
    Serial.println("DHCP server restart failed");
    return;
  }
  Serial.println("DHCP lease range 192.168.4.50-192.168.4.100");
}

void updateAirReadiness() {
  const auto snap = radio_link::snapshot();
  const uint32_t now = millis();
  const bool fresh = snap.stats.last_rx_ms != 0U && (uint32_t)(now - snap.stats.last_rx_ms) <= 3000U;

  if (fresh) {
    if (!g_air_ready) {
      Serial.printf("GND READY air_link sender=%s seq=%lu t_us=%lu\n",
                    radio_link::lastSenderMac().c_str(),
                    (unsigned long)snap.seq,
                    (unsigned long)snap.t_us);
      g_air_ready = true;
      g_air_wait_announced = false;
    }
    g_air_bootstrap_active = false;
    g_air_probe_attempts = 0U;
    g_last_air_probe_ms = 0U;
    return;
  }

  if (!g_air_wait_announced) {
    Serial.printf("GND WAIT air_packets target=%s\n", radio_link::targetSenderMac().c_str());
    g_air_wait_announced = true;
  }

  if (radio_link::hasLearnedSender()) {
    g_air_bootstrap_active = true;
  } else {
    g_air_probe_attempts = 0U;
    g_last_air_probe_ms = 0U;
  }
  if (g_air_bootstrap_active && radio_link::hasLearnedSender() && g_air_probe_attempts < kAirProbeMaxAttempts &&
      (g_last_air_probe_ms == 0U || (uint32_t)(now - g_last_air_probe_ms) >= kAirProbeRetryMs)) {
    const AppConfig& cfg = config_store::get();
    telem::CmdSetStreamRateV1 cmd = {};
    cmd.ws_rate_hz = cfg.source_rate_hz;
    cmd.log_rate_hz = cfg.source_rate_hz;
    if (radio_link::sendSetStreamRate(cmd)) {
      g_air_probe_attempts++;
      g_last_air_probe_ms = now;
    }
  }

  g_air_ready = false;
}

void printStats() {
  const auto snap = radio_link::snapshot();
  Serial.printf(
      "STAT unit=GND seq=%lu t_us=%lu has=%u ack=%u cmd=%u ack_ok=%u code=%lu "
      "rx_bytes=%lu ok=%lu crc=%u cobs=%u len=%lu unk=%lu drop=%lu link_tx=%u link_rx=%lu link_drop=%u\n",
      (unsigned long)snap.seq,
      (unsigned long)snap.t_us,
      snap.has_state ? 1U : 0U,
      snap.has_ack ? 1U : 0U,
      (unsigned)snap.ack_command,
      snap.ack_ok ? 1U : 0U,
      (unsigned long)snap.ack_code,
      (unsigned long)snap.stats.rx_bytes,
      (unsigned long)snap.stats.frames_ok,
      0U,
      0U,
      (unsigned long)snap.stats.len_err,
      (unsigned long)snap.stats.unknown_msg,
      (unsigned long)snap.stats.drop,
      0U,
      (unsigned long)snap.stats.rx_packets,
      0U);
}

void printLinkMeta() {
  const auto snap = radio_link::snapshot();
  Serial.printf(
      "SEELINK has=%u seq=%lu t_us=%lu ack=%u cmd=%u ack_ok=%u code=%lu fusion=%u sender=%s "
      "last_rx_ms=%lu packets=%lu ok=%lu rssi=%d recorder=%u\n",
      snap.has_state ? 1U : 0U,
      (unsigned long)snap.seq,
      (unsigned long)snap.t_us,
      snap.has_ack ? 1U : 0U,
      (unsigned)snap.ack_command,
      snap.ack_ok ? 1U : 0U,
      (unsigned long)snap.ack_code,
      snap.has_fusion_settings ? 1U : 0U,
      radio_link::lastSenderMac().c_str(),
      (unsigned long)snap.stats.last_rx_ms,
      (unsigned long)snap.stats.rx_packets,
      (unsigned long)snap.stats.frames_ok,
      (snap.link_meta.flags & telem::kLinkMetaFlagRssiValid) ? (int)snap.link_meta.gnd_ap_rssi_dbm : 0,
      (snap.link_meta.flags & telem::kLinkMetaFlagRecorderOn) ? 1U : 0U);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("ESP_GND boot");

  config_store::begin();
  const AppConfig& cfg = config_store::get();

  const IPAddress local_ip(192, 168, 4, 1);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  constexpr int kApChannel = 6;
  constexpr int kApMaxConnections = 4;

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  (void)esp_wifi_set_ps(WIFI_PS_NONE);
  (void)esp_wifi_set_max_tx_power(78);
  if (!WiFi.softAPConfig(local_ip, gateway, subnet)) {
    Serial.println("AP static IP config failed");
  }
  if (!WiFi.softAP(cfg.ap_ssid, cfg.ap_pass, kApChannel, 0, kApMaxConnections)) {
    Serial.println("softAP start failed");
  }
  configureDhcpLeaseRange();

  Serial.printf("AP ssid=%s ip=%s channel=%d\n",
                cfg.ap_ssid,
                WiFi.softAPIP().toString().c_str(),
                kApChannel);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  radio_link::begin(cfg);
  Serial.printf("GND READY ap ip=%s channel=%u dhcp=192.168.4.50-192.168.4.100\n",
                WiFi.softAPIP().toString().c_str(),
                (unsigned)kApChannel);
  Serial.printf("GND WAIT air_packets target=%s\n", radio_link::targetSenderMac().c_str());
  g_air_wait_announced = true;

  ws_server::begin();
  printConsoleHelp();
}

void loop() {
  handleConsoleCommands();
  logApState();
  radio_link::poll();
  updateAirReadiness();
  ws_server::loop();

  const uint32_t now = millis();
  if ((g_stats_streaming || g_link_streaming) && (now - g_last_stat_ms) >= 1000UL) {
    g_last_stat_ms = now;
    if (g_stats_streaming) printStats();
    if (g_link_streaming) printLinkMeta();
  }
}
