#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

namespace {

const char* kSsid = "carlsiPhone";
const char* kPass = "YOUR_PASSWORD";
const uint32_t kConnectTimeoutMs = 20000;
const uint32_t kRetryDelayMs = 5000;
const uint16_t kHttpPort = 80;

WebServer server(kHttpPort);

volatile bool gWifiConnected = false;
volatile bool gHaveIp = false;
IPAddress gLocalIp;
IPAddress gSubnetMask;
IPAddress gGatewayIp;
String gLastFailure = "none";
uint32_t gHttpHitCount = 0;
uint32_t gConnectAttempt = 0;
uint32_t gConnectStartMs = 0;
uint32_t gNextRetryMs = 0;
uint32_t gLastStatusMs = 0;

const char* wifiEventName(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      return "sta_start";
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      return "sta_connected";
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      return "sta_disconnected";
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      return "got_ip";
    default:
      return "other";
  }
}

const char* disconnectReasonLabel(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_UNSPECIFIED:
      return "UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE:
      return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:
      return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE:
      return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_TOOMANY:
      return "ASSOC_TOOMANY";
    case WIFI_REASON_NOT_AUTHED:
      return "NOT_AUTHED";
    case WIFI_REASON_NOT_ASSOCED:
      return "NOT_ASSOCED";
    case WIFI_REASON_ASSOC_LEAVE:
      return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED:
      return "ASSOC_NOT_AUTHED";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD:
      return "DISASSOC_PWRCAP_BAD";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
      return "DISASSOC_SUPCHAN_BAD";
    case WIFI_REASON_IE_INVALID:
      return "IE_INVALID";
    case WIFI_REASON_MIC_FAILURE:
      return "MIC_FAILURE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
      return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
      return "GROUP_KEY_TIMEOUT";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
      return "IE_IN_4WAY_DIFFERS";
    case WIFI_REASON_GROUP_CIPHER_INVALID:
      return "GROUP_CIPHER_INVALID";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
      return "PAIRWISE_CIPHER_INVALID";
    case WIFI_REASON_AKMP_INVALID:
      return "AKMP_INVALID";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
      return "UNSUPP_RSN_IE_VERSION";
    case WIFI_REASON_INVALID_RSN_IE_CAP:
      return "INVALID_RSN_IE_CAP";
    case WIFI_REASON_802_1X_AUTH_FAILED:
      return "AUTH_8021X_FAILED";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
      return "CIPHER_SUITE_REJECTED";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
      return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
      return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
      return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
      return "CONNECTION_FAIL";
    case WIFI_REASON_AP_TSF_RESET:
      return "AP_TSF_RESET";
    case WIFI_REASON_ROAMING:
      return "ROAMING";
    default:
      return "UNKNOWN";
  }
}

String currentIpString() {
  return gHaveIp ? gLocalIp.toString() : String("none");
}

void logHttpHit(const char* path) {
  ++gHttpHitCount;
  Serial.printf("HTTP hit path=%s from=%s total=%lu\n",
                path,
                server.client().remoteIP().toString().c_str(),
                static_cast<unsigned long>(gHttpHitCount));
}

void handleRoot() {
  logHttpHit("/");

  String body;
  body.reserve(128);
  body += "XIAO ESP32-S3 hotspot test OK\n";
  body += "ip=";
  body += currentIpString();
  body += "\nssid=";
  body += kSsid;
  body += "\nrssi=";
  body += gWifiConnected ? String(WiFi.RSSI()) : String("none");
  body += "\nhttp_hits=";
  body += String(gHttpHitCount);
  body += "\n";

  server.send(200, "text/plain", body);
}

void handlePing() {
  logHttpHit("/ping");
  server.send(200, "text/plain", "pong\n");
}

void handleNotFound() {
  const String path = server.uri();
  ++gHttpHitCount;
  Serial.printf("HTTP hit path=%s from=%s total=%lu\n",
                path.c_str(),
                server.client().remoteIP().toString().c_str(),
                static_cast<unsigned long>(gHttpHitCount));
  server.send(404, "text/plain", "not found\n");
}

void startHttpServer() {
  static bool started = false;
  if (started) {
    return;
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/ping", HTTP_GET, handlePing);
  server.onNotFound(handleNotFound);
  server.begin();
  started = true;
  Serial.printf("HTTP server_started port=%u\n", kHttpPort);
}

void beginStaConnect() {
  ++gConnectAttempt;
  gConnectStartMs = millis();
  gNextRetryMs = 0;
  gLastFailure = "none";

  Serial.printf("NET connect ssid=%s timeout_ms=%lu attempt=%lu\n",
                kSsid,
                static_cast<unsigned long>(kConnectTimeoutMs),
                static_cast<unsigned long>(gConnectAttempt));

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.begin(kSsid, kPass);
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.printf("NET evt %s\n", wifiEventName(static_cast<arduino_event_id_t>(event)));

  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      break;

    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      gWifiConnected = true;
      gLastFailure = "none";
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      gWifiConnected = false;
      gHaveIp = false;
      gLocalIp = IPAddress();
      gSubnetMask = IPAddress();
      gGatewayIp = IPAddress();
      gLastFailure = disconnectReasonLabel(info.wifi_sta_disconnected.reason);
      Serial.printf("NET evt sta_disconnected reason=%u label=%s\n",
                    info.wifi_sta_disconnected.reason,
                    disconnectReasonLabel(info.wifi_sta_disconnected.reason));
      gNextRetryMs = millis() + kRetryDelayMs;
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      gWifiConnected = true;
      gHaveIp = true;
      gLocalIp = IPAddress(info.got_ip.ip_info.ip.addr);
      gSubnetMask = IPAddress(info.got_ip.ip_info.netmask.addr);
      gGatewayIp = IPAddress(info.got_ip.ip_info.gw.addr);
      Serial.printf("NET evt got_ip ip=%s mask=%s gw=%s\n",
                    gLocalIp.toString().c_str(),
                    gSubnetMask.toString().c_str(),
                    gGatewayIp.toString().c_str());
      Serial.printf("NET success ssid=%s ip=%s\n", kSsid, gLocalIp.toString().c_str());
      startHttpServer();
      break;

    default:
      break;
  }
}

void printStatus() {
  const bool connected = (WiFi.status() == WL_CONNECTED) && gHaveIp;
  if (connected) {
    Serial.printf("STAT wifi=connected ip=%s rssi=%d uptime_ms=%lu http_hits=%lu\n",
                  currentIpString().c_str(),
                  WiFi.RSSI(),
                  static_cast<unsigned long>(millis()),
                  static_cast<unsigned long>(gHttpHitCount));
  } else {
    Serial.printf("STAT wifi=disconnected ip=none uptime_ms=%lu http_hits=%lu last_fail=%s\n",
                  static_cast<unsigned long>(millis()),
                  static_cast<unsigned long>(gHttpHitCount),
                  gLastFailure.c_str());
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("XIAO HOTSPOT TEST START");
  Serial.println("NET mode=STA");

  WiFi.persistent(false);
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setAutoReconnect(false);

  beginStaConnect();
}

void loop() {
  if (gHaveIp) {
    server.handleClient();
  }

  const uint32_t now = millis();
  const wl_status_t status = WiFi.status();

  if (!gHaveIp && status != WL_CONNECTED && gConnectStartMs != 0 &&
      (now - gConnectStartMs) >= kConnectTimeoutMs && gNextRetryMs == 0) {
    gLastFailure = "connect_timeout";
    Serial.println("NET fail reason=connect_timeout");
    gNextRetryMs = now + kRetryDelayMs;
  }

  if (!gHaveIp && gNextRetryMs != 0 && now >= gNextRetryMs) {
    Serial.printf("NET retry attempt=%lu delay_ms=%lu\n",
                  static_cast<unsigned long>(gConnectAttempt + 1),
                  static_cast<unsigned long>(kRetryDelayMs));
    beginStaConnect();
  }

  if (now - gLastStatusMs >= 1000) {
    gLastStatusMs = now;
    printStatus();
  }
}
