#include "ws_server.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <WiFi.h>

#include "config_store.h"
#include "types_shared.h"
#include "radio_link.h"

namespace ws_server {
namespace {

AsyncWebServer g_server(80);
AsyncWebSocket g_ws_ctrl("/ws_ctrl");
AsyncWebSocket g_ws_state("/ws_state");
constexpr uint16_t kFixedDownlinkRateHz = 30U;
constexpr uint16_t kFixedUiRateHz = 30U;

uint32_t g_ws_state_seq = 0;
uint32_t g_last_state_broadcast_ms = 0;

struct EventRow {
  uint32_t ms = 0;
  char socket[8] = {};
  char event[16] = {};
  uint32_t code = 0;
  char detail[64] = {};
};

EventRow g_ws_events[64] = {};
size_t g_ws_event_head = 0;
size_t g_ws_event_count = 0;

void pushEvent(const char* socket, const char* event, uint32_t code = 0, const char* detail = "") {
  EventRow& row = g_ws_events[g_ws_event_head];
  row.ms = millis();
  strncpy(row.socket, socket, sizeof(row.socket) - 1);
  row.socket[sizeof(row.socket) - 1] = '\0';
  strncpy(row.event, event, sizeof(row.event) - 1);
  row.event[sizeof(row.event) - 1] = '\0';
  row.code = code;
  strncpy(row.detail, detail, sizeof(row.detail) - 1);
  row.detail[sizeof(row.detail) - 1] = '\0';
  g_ws_event_head = (g_ws_event_head + 1U) % (sizeof(g_ws_events) / sizeof(g_ws_events[0]));
  if (g_ws_event_count < (sizeof(g_ws_events) / sizeof(g_ws_events[0]))) g_ws_event_count++;
}

void sendCtrlJson(AsyncWebSocketClient* client, const JsonDocument& doc) {
  String text;
  serializeJson(doc, text);
  if (client) client->text(text);
}

void appendLogStatus(JsonDocument& doc, const telem::LogStatusPayloadV1& status) {
  JsonObject log = doc["log_status"].to<JsonObject>();
  log["active"] = (status.flags & telem::kLogStatusFlagActive) != 0U;
  log["requested"] = (status.flags & telem::kLogStatusFlagRequested) != 0U;
  log["backend_ready"] = (status.flags & telem::kLogStatusFlagBackendReady) != 0U;
  log["media_present"] = (status.flags & telem::kLogStatusFlagMediaPresent) != 0U;
  log["last_command"] = status.last_command;
  log["session_id"] = status.session_id;
  log["bytes_written"] = status.bytes_written;
  log["free_bytes"] = status.free_bytes;
  log["last_change_ms"] = status.last_change_ms;
}

void broadcastConfig(AsyncWebSocketClient* client = nullptr) {
  const AppConfig& cfg = config_store::get();
  JsonDocument doc;
  doc["type"] = "config";
  doc["source_rate_hz"] = cfg.source_rate_hz;
  doc["capture_rate_hz"] = cfg.source_rate_hz;
  doc["ui_rate_hz"] = kFixedUiRateHz;
  doc["log_rate_hz"] = cfg.log_rate_hz;
  doc["download_rate_hz"] = kFixedDownlinkRateHz;
  doc["log_mode"] = 1;
  doc["radio_state_only"] = cfg.radio_state_only != 0U;

  if (client) {
    sendCtrlJson(client, doc);
    return;
  }

  String text;
  serializeJson(doc, text);
  g_ws_ctrl.textAll(text);
}

void sendAck(AsyncWebSocketClient* client,
             const char* cmd,
             bool ok,
             uint32_t code = 0,
             const telem::FusionSettingsV1* fusion = nullptr,
             const telem::LogStatusPayloadV1* log_status = nullptr) {
  JsonDocument doc;
  doc["type"] = "ack";
  doc["cmd"] = cmd ? cmd : "";
  doc["ok"] = ok;
  doc["code"] = code;
  if (fusion) {
    JsonObject f = doc["fusion"].to<JsonObject>();
    f["gain"] = fusion->gain;
    f["accelerationRejection"] = fusion->accelerationRejection;
    f["magneticRejection"] = fusion->magneticRejection;
    f["recoveryTriggerPeriod"] = fusion->recoveryTriggerPeriod;
  }
  if (log_status) {
    appendLogStatus(doc, *log_status);
  }
  sendCtrlJson(client, doc);
}

void handleCtrlMessage(AsyncWebSocketClient* client, const char* text, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, text, len)) return;
  const char* type = doc["type"] | "";

  if (strcmp(type, "ping") == 0) {
    JsonDocument pong;
    pong["type"] = "pong";
    sendCtrlJson(client, pong);
    return;
  }

  if (strcmp(type, "get_fusion") == 0) {
    const bool requested = radio_link::sendGetFusionSettings();
    const auto snap = radio_link::snapshot();
    telem::FusionSettingsV1 fusion = {};
    bool has_fusion = false;
    if (snap.has_fusion_settings) {
      fusion = snap.fusion_settings;
      has_fusion = true;
    } else if (snap.has_state) {
      fusion.gain = snap.state.fusion_gain;
      fusion.accelerationRejection = snap.state.fusion_accel_rej;
      fusion.magneticRejection = snap.state.fusion_mag_rej;
      fusion.recoveryTriggerPeriod = snap.state.fusion_recovery_period;
      has_fusion = true;
    }
    sendAck(client, "get_fusion", requested, has_fusion ? 0 : 1, has_fusion ? &fusion : nullptr);
    return;
  }

  if (strcmp(type, "get_log_status") == 0) {
    const bool requested = radio_link::sendGetLogStatus();
    const auto snap = radio_link::snapshot();
    sendAck(client,
            "get_log_status",
            requested,
            snap.has_log_status ? 0U : 1U,
            nullptr,
            snap.has_log_status ? &snap.log_status : nullptr);
    return;
  }

  if (strcmp(type, "set_fusion") == 0) {
    telem::CmdSetFusionSettingsV1 cmd = {};
    const bool has_nested = !doc["fusion"].isNull();
    if (has_nested) {
      JsonObject fusion = doc["fusion"];
      if (fusion["gain"].isNull() || fusion["accelerationRejection"].isNull() ||
          fusion["magneticRejection"].isNull() || fusion["recoveryTriggerPeriod"].isNull()) {
        sendAck(client, "set_fusion", false, 2, nullptr);
        return;
      }
      cmd.gain = fusion["gain"].as<float>();
      cmd.accelerationRejection = fusion["accelerationRejection"].as<float>();
      cmd.magneticRejection = fusion["magneticRejection"].as<float>();
      cmd.recoveryTriggerPeriod = fusion["recoveryTriggerPeriod"].as<uint16_t>();
    } else {
      if (doc["gain"].isNull() || doc["accelerationRejection"].isNull() || doc["magneticRejection"].isNull() ||
          doc["recoveryTriggerPeriod"].isNull()) {
        sendAck(client, "set_fusion", false, 2, nullptr);
        return;
      }
      cmd.gain = doc["gain"].as<float>();
      cmd.accelerationRejection = doc["accelerationRejection"].as<float>();
      cmd.magneticRejection = doc["magneticRejection"].as<float>();
      cmd.recoveryTriggerPeriod = doc["recoveryTriggerPeriod"].as<uint16_t>();
    }
    const bool ok = radio_link::sendSetFusionSettings(cmd);
    Serial.printf("SET_FUSION tx_ok=%u gain=%.3f accRej=%.2f magRej=%.2f rec=%u\n",
                  ok ? 1U : 0U,
                  (double)cmd.gain,
                  (double)cmd.accelerationRejection,
                  (double)cmd.magneticRejection,
                  (unsigned)cmd.recoveryTriggerPeriod);
    if (ok) {
      (void)radio_link::sendGetFusionSettings();
    }
    sendAck(client, "set_fusion", ok, ok ? 0 : 1, nullptr);
    return;
  }

  if (strcmp(type, "start_log") == 0) {
    const bool ok = radio_link::sendLogStart();
    if (ok) {
      (void)radio_link::sendGetLogStatus();
    }
    sendAck(client, "start_log", ok, ok ? 0U : 1U, nullptr);
    return;
  }

  if (strcmp(type, "stop_log") == 0) {
    const bool ok = radio_link::sendLogStop();
    if (ok) {
      (void)radio_link::sendGetLogStatus();
    }
    sendAck(client, "stop_log", ok, ok ? 0U : 1U, nullptr);
    return;
  }
}

void onCtrlEvent(AsyncWebSocket* server,
                 AsyncWebSocketClient* client,
                 AwsEventType type,
                 void* arg,
                 uint8_t* data,
                 size_t len) {
  (void)server;
  if (type == WS_EVT_CONNECT) {
    pushEvent("ctrl", "open", client ? client->id() : 0U);
    broadcastConfig(client);
    return;
  }
  if (type == WS_EVT_DISCONNECT) {
    pushEvent("ctrl", "close", client ? client->id() : 0U);
    return;
  }
  if (type != WS_EVT_DATA || !arg || !data || len == 0) return;

  const AwsFrameInfo* info = reinterpret_cast<const AwsFrameInfo*>(arg);
  if (!info->final || info->index != 0 || info->opcode != WS_TEXT) return;
  handleCtrlMessage(client, reinterpret_cast<const char*>(data), len);
}

void onStateEvent(AsyncWebSocket* server,
                  AsyncWebSocketClient* client,
                  AwsEventType type,
                  void* arg,
                  uint8_t* data,
                  size_t len) {
  (void)server;
  (void)arg;
  (void)data;
  (void)len;
  if (type == WS_EVT_CONNECT) {
    pushEvent("state", "open", client ? client->id() : 0U);
  } else if (type == WS_EVT_DISCONNECT) {
    pushEvent("state", "close", client ? client->id() : 0U);
  }
}

void serveDiagCsv(AsyncWebServerRequest* request) {
  const auto snap = radio_link::snapshot();
  const bool air_link_fresh =
      snap.stats.last_rx_ms != 0U && (uint32_t)(millis() - snap.stats.last_rx_ms) <= 3000U;
  String csv;
  csv.reserve(768);
  csv += "metric,value\n";
  csv += "transport,ESP-NOW\n";
  csv += "air_link_fresh," + String(air_link_fresh ? 1 : 0) + "\n";
  csv += "ap_clients," + String(WiFi.softAPgetStationNum()) + "\n";
  csv += "rx_packets," + String(snap.stats.rx_packets) + "\n";
  csv += "rx_bytes," + String(snap.stats.rx_bytes) + "\n";
  csv += "frames_ok," + String(snap.stats.frames_ok) + "\n";
  csv += "state_packets," + String(snap.stats.state_packets) + "\n";
  csv += "state_seq_gap," + String(snap.stats.state_seq_gap) + "\n";
  csv += "state_seq_rewind," + String(snap.stats.state_seq_rewind) + "\n";
  csv += "len_err," + String(snap.stats.len_err) + "\n";
  csv += "unknown_msg," + String(snap.stats.unknown_msg) + "\n";
  csv += "drop," + String(snap.stats.drop) + "\n";
  csv += "last_rx_ms," + String(snap.stats.last_rx_ms) + "\n";
  csv += "air_radio_ready," +
         String((snap.link_meta.flags & telem::kLinkMetaFlagRadioReady) ? 1 : 0) + "\n";
  csv += "air_peer_known," +
         String((snap.link_meta.flags & telem::kLinkMetaFlagPeerKnown) ? 1 : 0) + "\n";
  csv += "air_recorder_on," +
         String((snap.link_meta.flags & telem::kLinkMetaFlagRecorderOn) ? 1 : 0) + "\n";
  csv += "air_rssi_valid," +
         String((snap.link_meta.flags & telem::kLinkMetaFlagRssiValid) ? 1 : 0) + "\n";
  csv += "air_rssi_dbm," + String((int)snap.link_meta.gnd_ap_rssi_dbm) + "\n";
  csv += "air_scan_age_ms," + String((unsigned long)snap.link_meta.scan_age_ms) + "\n";
  csv += "air_link_age_ms," + String((unsigned long)snap.link_meta.link_age_ms) + "\n";
  csv += "radio_rtt_ms," + String((unsigned long)snap.radio_rtt_ms) + "\n";
  csv += "radio_rtt_avg_ms," + String((unsigned long)snap.radio_rtt_avg_ms) + "\n";
  csv += "last_radio_pong_ms," + String((unsigned long)snap.last_radio_pong_ms) + "\n";
  csv += "air_sender_mac," + radio_link::lastSenderMac() + "\n";
  csv += "air_target_mac," + radio_link::targetSenderMac() + "\n";
  csv += "air_log_active," + String((snap.log_status.flags & telem::kLogStatusFlagActive) ? 1 : 0) + "\n";
  csv += "air_log_requested," + String((snap.log_status.flags & telem::kLogStatusFlagRequested) ? 1 : 0) + "\n";
  csv += "air_log_backend_ready," + String((snap.log_status.flags & telem::kLogStatusFlagBackendReady) ? 1 : 0) + "\n";
  csv += "air_log_media_present," + String((snap.log_status.flags & telem::kLogStatusFlagMediaPresent) ? 1 : 0) + "\n";
  csv += "air_log_last_command," + String((unsigned)snap.log_status.last_command) + "\n";
  csv += "air_log_session_id," + String((unsigned long)snap.log_status.session_id) + "\n";
  csv += "air_log_bytes_written," + String((unsigned long)snap.log_status.bytes_written) + "\n";
  csv += "air_log_free_bytes," + String((unsigned long)snap.log_status.free_bytes) + "\n";
  csv += "air_log_last_change_ms," + String((unsigned long)snap.log_status.last_change_ms) + "\n";
  request->send(200, "text/csv", csv);
}

void serveWsEventsCsv(AsyncWebServerRequest* request) {
  String csv;
  csv.reserve(2048);
  csv += "ms,socket,event,code,detail\n";
  const size_t capacity = sizeof(g_ws_events) / sizeof(g_ws_events[0]);
  const size_t start = (g_ws_event_count < capacity) ? 0U : g_ws_event_head;
  for (size_t i = 0; i < g_ws_event_count; ++i) {
    const EventRow& row = g_ws_events[(start + i) % capacity];
    csv += String(row.ms) + ",";
    csv += row.socket;
    csv += ",";
    csv += row.event;
    csv += ",";
    csv += String(row.code);
    csv += ",\"";
    csv += row.detail;
    csv += "\"\n";
  }
  request->send(200, "text/csv", csv);
}

void broadcastState() {
  const uint32_t now = millis();
  const uint32_t interval_ms = 1000UL / (uint32_t)kFixedUiRateHz;
  if ((now - g_last_state_broadcast_ms) < interval_ms) return;
  g_last_state_broadcast_ms = now;

  const auto snap = radio_link::snapshot();
  if (!snap.has_state) return;

  telem::WsStateHeaderV2 hdr = {};
  hdr.magic = telem::kWsStateMagic;
  hdr.version = telem::kWsStateVersion;
  hdr.header_len = sizeof(hdr);
  hdr.payload_len = sizeof(snap.state);
  hdr.flags = 0;
  hdr.ws_seq = ++g_ws_state_seq;
  hdr.state_seq = snap.seq;
  hdr.source_t_us = snap.t_us;
  hdr.esp_rx_ms = snap.stats.last_rx_ms;

  uint8_t frame[sizeof(hdr) + sizeof(snap.state)] = {};
  memcpy(frame, &hdr, sizeof(hdr));
  memcpy(frame + sizeof(hdr), &snap.state, sizeof(snap.state));
  g_ws_state.binaryAll(frame, sizeof(frame));
}

}  // namespace

void begin() {
  g_ws_ctrl.onEvent(onCtrlEvent);
  g_ws_state.onEvent(onStateEvent);
  g_server.addHandler(&g_ws_ctrl);
  g_server.addHandler(&g_ws_state);

  g_server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    const auto snap = radio_link::snapshot();
    const bool air_link_fresh =
        snap.stats.last_rx_ms != 0U && (uint32_t)(millis() - snap.stats.last_rx_ms) <= 3000U;
    JsonDocument doc;
    doc["transport"] = "ESP-NOW";
    doc["radio_state_only"] = config_store::get().radio_state_only != 0U;
    doc["air_link_fresh"] = air_link_fresh;
    doc["ap_clients"] = WiFi.softAPgetStationNum();
    doc["has_state"] = snap.has_state;
    doc["has_link_meta"] = snap.has_link_meta;
    doc["seq"] = snap.seq;
    doc["t_us"] = snap.t_us;
    doc["link_rx"] = snap.stats.rx_packets;
    doc["ok"] = snap.stats.frames_ok;
    doc["state_packets"] = snap.stats.state_packets;
    doc["state_seq_gap"] = snap.stats.state_seq_gap;
    doc["state_seq_rewind"] = snap.stats.state_seq_rewind;
    doc["len_err"] = snap.stats.len_err;
    doc["unknown_msg"] = snap.stats.unknown_msg;
    doc["drop"] = snap.stats.drop;
    doc["air_radio_ready"] = (snap.link_meta.flags & telem::kLinkMetaFlagRadioReady) != 0U;
    doc["air_peer_known"] = (snap.link_meta.flags & telem::kLinkMetaFlagPeerKnown) != 0U;
    doc["air_recorder_on"] = (snap.link_meta.flags & telem::kLinkMetaFlagRecorderOn) != 0U;
    doc["air_rssi_valid"] = (snap.link_meta.flags & telem::kLinkMetaFlagRssiValid) != 0U;
    doc["air_rssi_dbm"] = snap.link_meta.gnd_ap_rssi_dbm;
    doc["air_scan_age_ms"] = snap.link_meta.scan_age_ms;
    doc["air_link_age_ms"] = snap.link_meta.link_age_ms;
    doc["radio_rtt_ms"] = snap.radio_rtt_ms;
    doc["radio_rtt_avg_ms"] = snap.radio_rtt_avg_ms;
    doc["last_radio_pong_ms"] = snap.last_radio_pong_ms;
    doc["air_sender_mac"] = radio_link::lastSenderMac();
    doc["air_target_mac"] = radio_link::targetSenderMac();
    doc["has_log_status"] = snap.has_log_status;
    doc["air_log_active"] = (snap.log_status.flags & telem::kLogStatusFlagActive) != 0U;
    doc["air_log_requested"] = (snap.log_status.flags & telem::kLogStatusFlagRequested) != 0U;
    doc["air_log_backend_ready"] = (snap.log_status.flags & telem::kLogStatusFlagBackendReady) != 0U;
    doc["air_log_media_present"] = (snap.log_status.flags & telem::kLogStatusFlagMediaPresent) != 0U;
    doc["air_log_last_command"] = snap.log_status.last_command;
    doc["air_log_session_id"] = snap.log_status.session_id;
    doc["air_log_bytes_written"] = snap.log_status.bytes_written;
    doc["air_log_free_bytes"] = snap.log_status.free_bytes;
    doc["air_log_last_change_ms"] = snap.log_status.last_change_ms;
    String text;
    serializeJson(doc, text);
    request->send(200, "application/json", text);
  });

  g_server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* request) {
    const AppConfig& cfg = config_store::get();
    JsonDocument doc;
    doc["source_rate_hz"] = cfg.source_rate_hz;
    doc["capture_rate_hz"] = cfg.source_rate_hz;
    doc["ui_rate_hz"] = kFixedUiRateHz;
    doc["log_rate_hz"] = cfg.log_rate_hz;
    doc["download_rate_hz"] = kFixedDownlinkRateHz;
    doc["log_mode"] = 1;
    doc["radio_state_only"] = cfg.radio_state_only != 0U;
    String text;
    serializeJson(doc, text);
    request->send(200, "application/json", text);
  });

  g_server.on(
      "/api/config",
      HTTP_POST,
      [](AsyncWebServerRequest* request) { request->send(200, "application/json", "{\"ok\":1}"); },
      nullptr,
      [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        static String body;
        if (index == 0) body = "";
        body.concat(reinterpret_cast<const char*>(data), len);
        if ((index + len) != total) return;

        JsonDocument doc;
        if (deserializeJson(doc, body)) {
          request->send(400, "application/json", "{\"ok\":0}");
          return;
        }

        AppConfig cfg = config_store::get();
        if (!doc["source_rate_hz"].isNull()) cfg.source_rate_hz = doc["source_rate_hz"].as<uint16_t>();
        if (!doc["radio_state_only"].isNull()) cfg.radio_state_only = doc["radio_state_only"].as<bool>() ? 1U : 0U;
        cfg.log_mode = 1U;
        config_store::update(cfg);
        radio_link::reconfigure(cfg);

        telem::CmdSetStreamRateV1 cmd = {};
        cmd.ws_rate_hz = cfg.source_rate_hz;
        cmd.log_rate_hz = cfg.log_rate_hz;
        telem::CmdSetRadioModeV1 radio_mode = {};
        radio_mode.state_only = cfg.radio_state_only ? 1U : 0U;
        radio_mode.control_rate_hz = 2U;
        radio_mode.telem_rate_hz = kFixedDownlinkRateHz;
        (void)radio_link::sendSetRadioMode(radio_mode);
        (void)radio_link::sendSetStreamRate(cmd);

        broadcastConfig();
      });

  g_server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", "[]");
  });
  g_server.on("/api/download", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "not available");
  });
  g_server.on("/api/delete", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", "{\"ok\":1}");
  });
  g_server.on("/api/reset_air_network", HTTP_POST, [](AsyncWebServerRequest* request) {
    const bool ok = radio_link::sendResetNetwork();
    request->send(ok ? 200 : 503, "application/json", ok ? "{\"ok\":1}" : "{\"ok\":0}");
  });
  g_server.on("/api/diag", HTTP_GET, serveDiagCsv);
  g_server.on("/api/ws_events", HTTP_GET, serveWsEventsCsv);
  g_server.on("/api/reset_counters", HTTP_POST, [](AsyncWebServerRequest* request) {
    resetCounters();
    request->send(200, "application/json", "{\"ok\":1}");
  });

  g_server.serveStatic("/", LittleFS, "/")
      .setDefaultFile("index.html")
      .setTryGzipFirst(false)
      .setCacheControl("no-cache, no-store, must-revalidate");
  g_server.begin();
}

void loop() {
  g_ws_ctrl.cleanupClients();
  g_ws_state.cleanupClients();
  broadcastState();
}

uint32_t clientCount() {
  return g_ws_ctrl.count() + g_ws_state.count();
}

void resetCounters() {
  g_ws_state_seq = 0;
  g_ws_event_head = 0;
  g_ws_event_count = 0;
  radio_link::resetStats();
}

}  // namespace ws_server
