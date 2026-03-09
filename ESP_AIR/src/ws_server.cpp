#include "ws_server.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "config_store.h"
#include "log_store.h"
#include "types_shared.h"
#include "uart_telem.h"

namespace ws_server {
namespace {

AsyncWebServer g_server(80);
AsyncWebSocket g_ws_ctrl("/ws_ctrl");
AsyncWebSocket g_ws_state("/ws_state");
uint32_t g_last_ws_ms = 0;
uint32_t g_last_side_ms = 0;
uint32_t g_last_fps_ms = 0;
uint32_t g_last_frames_ok = 0;
float g_rx_fps = 0.0f;
uint32_t g_ws_seq = 0;
uint32_t g_ws_drop_frames = 0;
uint32_t g_ws_sent_frames = 0;
uint32_t g_ws_backpressure_hits = 0;
uint32_t g_ws_queue_max = 0;
uint32_t g_ws_queue_cur = 0;
uint32_t g_ws_connect_count = 0;
uint32_t g_ws_disconnect_count = 0;
uint32_t g_ws_error_count = 0;
uint32_t g_ws_last_disconnect_ms = 0;
uint32_t g_ws_last_error_ms = 0;
uint16_t g_ws_last_error_code = 0;
uint32_t g_ctrl_connect_count = 0;
uint32_t g_ctrl_disconnect_count = 0;
uint32_t g_ctrl_error_count = 0;
uint32_t g_ctrl_last_disconnect_ms = 0;
uint32_t g_ctrl_last_error_ms = 0;
uint16_t g_ctrl_last_error_code = 0;
uint32_t g_last_pub_state_seq = 0;
uint32_t g_last_pub_ack_seq = 0;
uint32_t g_last_cleanup_ms = 0;
uint32_t g_last_state_pub_ms = 0;
uint32_t g_last_diag_sample_ms = 0;
uint32_t g_ws_state_stall_resets = 0;

struct StateClientStall {
  uint32_t client_id = 0;
  uint32_t queued_since_ms = 0;
  uint8_t strike_count = 0;
};

static constexpr uint32_t kStateSocketStallMs = 250U;
static constexpr size_t kStateClientStallCapacity = 8;
StateClientStall g_state_client_stalls[kStateClientStallCapacity] = {};

struct DiagSample {
  uint32_t ms;
  float rx_fps;
  uint32_t source_rate_hz_cfg;
  uint32_t ui_rate_hz_cfg;
  uint32_t log_rate_hz_cfg;
  uint32_t log_mode;
  uint32_t ctrl_clients;
  uint32_t state_clients;
  uint32_t ws_backpressure;
  uint32_t ws_drop;
  uint32_t ws_queue_cur;
  uint32_t ws_queue_max;
  uint32_t snap_seq;
  uint32_t pub_seq;
  uint32_t uart_state_age_ms;
  uint32_t pub_state_age_ms;
  uint32_t ctrl_disconnects;
  uint32_t state_disconnects;
  uint32_t log_queue_cur;
  uint32_t log_queue_max;
  uint32_t log_enqueued;
  uint32_t log_dropped;
  uint32_t log_records_written;
  uint32_t log_bytes_written;
  uint32_t fs_open_last_ms;
  uint32_t fs_open_max_ms;
  uint32_t fs_write_last_ms;
  uint32_t fs_write_max_ms;
  uint32_t fs_close_last_ms;
  uint32_t fs_close_max_ms;
  uint32_t fs_delete_last_ms;
  uint32_t fs_delete_max_ms;
  uint32_t fs_download_last_ms;
  uint32_t fs_download_max_ms;
  int32_t wifi_rssi_dbm;
};

static constexpr size_t kDiagCapacity = 900;
DiagSample g_diag_ring[kDiagCapacity] = {};
size_t g_diag_head = 0;
size_t g_diag_count = 0;

struct WsEventSample {
  uint32_t ms;
  char socket_kind;
  char event_kind;
  uint32_t client_id;
  uint16_t code;
};

static constexpr size_t kWsEventCapacity = 256;
WsEventSample g_ws_event_ring[kWsEventCapacity] = {};
size_t g_ws_event_head = 0;
size_t g_ws_event_count = 0;

int apClientRssiDbm();

uint32_t browserSessionCount() {
  const uint32_t ctrlCount = g_ws_ctrl.count();
  const uint32_t stateCount = g_ws_state.count();
  return ctrlCount > stateCount ? ctrlCount : stateCount;
}

void recordWsEvent(char socketKind, char eventKind, uint32_t clientId, uint16_t code = 0U) {
  WsEventSample s = {};
  s.ms = millis();
  s.socket_kind = socketKind;
  s.event_kind = eventKind;
  s.client_id = clientId;
  s.code = code;
  g_ws_event_ring[g_ws_event_head] = s;
  g_ws_event_head = (g_ws_event_head + 1U) % kWsEventCapacity;
  if (g_ws_event_count < kWsEventCapacity) g_ws_event_count++;
}

StateClientStall* findStateClientStall(uint32_t clientId, bool create) {
  StateClientStall* freeSlot = nullptr;
  for (size_t i = 0; i < kStateClientStallCapacity; ++i) {
    StateClientStall& slot = g_state_client_stalls[i];
    if (slot.client_id == clientId) return &slot;
    if (!freeSlot && slot.client_id == 0U) freeSlot = &slot;
  }
  if (!create) return nullptr;
  if (freeSlot) {
    freeSlot->client_id = clientId;
    freeSlot->queued_since_ms = 0U;
    freeSlot->strike_count = 0U;
    return freeSlot;
  }
  return nullptr;
}

void clearStateClientStall(uint32_t clientId) {
  StateClientStall* slot = findStateClientStall(clientId, false);
  if (!slot) return;
  slot->client_id = 0U;
  slot->queued_since_ms = 0U;
  slot->strike_count = 0U;
}

void recordDiagSample(const uart_telem::Snapshot& snap, uint32_t now) {
  if ((uint32_t)(now - g_last_diag_sample_ms) < 1000U) return;
  g_last_diag_sample_ms = now;
  const AppConfig& cfg = config_store::get();

  DiagSample s = {};
  s.ms = now;
  s.rx_fps = g_rx_fps;
  s.source_rate_hz_cfg = cfg.source_rate_hz;
  s.ui_rate_hz_cfg = cfg.ui_rate_hz;
  s.log_rate_hz_cfg = cfg.log_rate_hz;
  s.log_mode = cfg.log_mode;
  s.ctrl_clients = g_ws_ctrl.count();
  s.state_clients = g_ws_state.count();
  s.ws_backpressure = g_ws_backpressure_hits;
  s.ws_drop = g_ws_drop_frames;
  s.ws_queue_cur = g_ws_queue_cur;
  s.ws_queue_max = g_ws_queue_max;
  s.snap_seq = snap.seq;
  s.pub_seq = g_last_pub_state_seq;
  s.uart_state_age_ms = snap.stats.last_rx_ms ? (now - snap.stats.last_rx_ms) : 0U;
  s.pub_state_age_ms = g_last_state_pub_ms ? (now - g_last_state_pub_ms) : 0U;
  s.ctrl_disconnects = g_ctrl_disconnect_count;
  s.state_disconnects = g_ws_disconnect_count;
  const auto logStats = log_store::stats();
  s.log_queue_cur = logStats.queue_cur;
  s.log_queue_max = logStats.queue_max;
  s.log_enqueued = logStats.enqueued;
  s.log_dropped = logStats.dropped;
  s.log_records_written = logStats.records_written;
  s.log_bytes_written = logStats.bytes_written;
  s.fs_open_last_ms = logStats.fs_open_last_ms;
  s.fs_open_max_ms = logStats.fs_open_max_ms;
  s.fs_write_last_ms = logStats.fs_write_last_ms;
  s.fs_write_max_ms = logStats.fs_write_max_ms;
  s.fs_close_last_ms = logStats.fs_close_last_ms;
  s.fs_close_max_ms = logStats.fs_close_max_ms;
  s.fs_delete_last_ms = logStats.fs_delete_last_ms;
  s.fs_delete_max_ms = logStats.fs_delete_max_ms;
  s.fs_download_last_ms = logStats.fs_download_last_ms;
  s.fs_download_max_ms = logStats.fs_download_max_ms;
  s.wifi_rssi_dbm = apClientRssiDbm();

  g_diag_ring[g_diag_head] = s;
  g_diag_head = (g_diag_head + 1U) % kDiagCapacity;
  if (g_diag_count < kDiagCapacity) g_diag_count++;
}

int apClientRssiDbm() {
  wifi_sta_list_t sta_list = {};
  if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) return 0;
  if (sta_list.num == 0) return 0;

  int sum = 0;
  for (int i = 0; i < sta_list.num; ++i) {
    sum += sta_list.sta[i].rssi;
  }
  return sum / sta_list.num;
}

bool sendWsJson(AsyncWebSocketClient* client, const JsonDocument& doc) {
  if (!client || client->status() != WS_CONNECTED) return false;
  if (!client->canSend() || client->queueIsFull()) return false;
  String out;
  serializeJson(doc, out);
  return client->text(out);
}

bool sendWsBinary(AsyncWebSocketClient* client, const uint8_t* data, size_t len) {
  if (!client || client->status() != WS_CONNECTED) return false;
  if (!client->canSend() || client->queueIsFull()) return false;
  return client->binary(data, len);
}

void updateRxFps(const uart_telem::Snapshot& snap, uint32_t now) {
  if ((uint32_t)(now - g_last_fps_ms) < 1000U) return;
  const uint32_t dframes = (snap.stats.frames_ok >= g_last_frames_ok) ? (snap.stats.frames_ok - g_last_frames_ok) : 0U;
  const uint32_t dt = now - g_last_fps_ms;
  g_rx_fps = dt ? (1000.0f * (float)dframes / (float)dt) : 0.0f;
  g_last_frames_ok = snap.stats.frames_ok;
  g_last_fps_ms = now;
}

bool sendStateSnapshot(AsyncWebSocketClient* client, const uart_telem::Snapshot& snap) {
  if (!client || !snap.has_state) return false;
  struct WsStateFrameV2 {
    telem::WsStateHeaderV2 hdr;
    telem::TelemetryFullStateV1 state;
  } frame = {};
  frame.hdr.magic = telem::kWsStateMagic;
  frame.hdr.version = telem::kWsStateVersion;
  frame.hdr.header_len = (uint16_t)sizeof(frame.hdr);
  frame.hdr.payload_len = (uint16_t)sizeof(frame.state);
  frame.hdr.flags = 0U;
  frame.hdr.ws_seq = ++g_ws_seq;
  frame.hdr.state_seq = snap.seq;
  frame.hdr.source_t_us = snap.t_us;
  frame.hdr.esp_rx_ms = snap.stats.last_rx_ms;
  frame.state = snap.state;
  return sendWsBinary(client, reinterpret_cast<const uint8_t*>(&frame), sizeof(frame));
}

void sendSideSnapshot(AsyncWebSocketClient* client, const uart_telem::Snapshot& snap) {
  const auto logStats = log_store::stats();
  const uint32_t now = millis();
  updateRxFps(snap, now);
  StaticJsonDocument<512> d;
  d["type"] = "side";
  JsonObject stats = d["st"].to<JsonObject>();
  stats["rf"] = g_rx_fps;
  stats["wc"] = browserSessionCount();
  stats["cc"] = g_ws_ctrl.count();
  stats["sc"] = g_ws_state.count();
  stats["lm"] = config_store::get().log_mode;
  stats["ce"] = snap.stats.crc_err;
  stats["co"] = snap.stats.cobs_err;
  stats["le"] = snap.stats.len_err;
  stats["ue"] = snap.stats.unknown_msg;
  stats["dr"] = snap.stats.drop;
  stats["wd"] = g_ws_drop_frames;
  stats["ws"] = g_ws_sent_frames;
  stats["wb"] = g_ws_backpressure_hits;
  stats["wqc"] = g_ws_queue_cur;
  stats["wq"] = g_ws_queue_max;
  stats["wdc"] = g_ws_disconnect_count;
  stats["cdc"] = g_ctrl_disconnect_count;
  stats["lqc"] = logStats.queue_cur;
  stats["lqm"] = logStats.queue_max;
  stats["lqe"] = logStats.enqueued;
  stats["lqd"] = logStats.dropped;
  stats["lrw"] = logStats.records_written;
  stats["lbw"] = logStats.bytes_written;
  stats["lqf"] = logStats.flushes;
  stats["fol"] = logStats.fs_open_last_ms;
  stats["fom"] = logStats.fs_open_max_ms;
  stats["fwl"] = logStats.fs_write_last_ms;
  stats["fwm"] = logStats.fs_write_max_ms;
  stats["fcl"] = logStats.fs_close_last_ms;
  stats["fcm"] = logStats.fs_close_max_ms;
  stats["fdl"] = logStats.fs_delete_last_ms;
  stats["fdm"] = logStats.fs_delete_max_ms;
  stats["frl"] = logStats.fs_download_last_ms;
  stats["frm"] = logStats.fs_download_max_ms;
  stats["sra"] = snap.stats.last_rx_ms ? (now - snap.stats.last_rx_ms) : 0U;
  stats["spa"] = g_last_state_pub_ms ? (now - g_last_state_pub_ms) : 0U;
  stats["shs"] = snap.has_state ? 1U : 0U;
  stats["ssq"] = snap.seq;
  stats["psq"] = g_last_pub_state_seq;

  JsonObject link = d["l"].to<JsonObject>();
  link["wr"] = apClientRssiDbm();
  link["lr"] = snap.stats.last_rx_ms;

  if (snap.has_ack) {
    JsonObject ack = d["ca"].to<JsonObject>();
    ack["r"] = snap.ack_rx_seq;
    ack["c"] = snap.ack_command;
    ack["o"] = snap.ack_ok;
    ack["cd"] = snap.ack_code;
  }
  (void)sendWsJson(client, d);
}

void sendConfig(AsyncWebSocketClient* client) {
  const AppConfig& c = config_store::get();
  StaticJsonDocument<384> d;
  d["type"] = "config";
  d["ap_ssid"] = c.ap_ssid;
  d["uart_baud"] = c.uart_baud;
  d["source_rate_hz"] = c.source_rate_hz;
  d["ui_rate_hz"] = c.ui_rate_hz;
  d["log_mode"] = c.log_mode;
  d["log_rate_hz"] = c.log_rate_hz;
  d["max_log_bytes"] = c.max_log_bytes;
  (void)sendWsJson(client, d);
}

void wsCtrlEvent(AsyncWebSocket* serverRef, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data,
                 size_t len) {
  (void)serverRef;
  if (type == WS_EVT_CONNECT) {
    g_ctrl_connect_count++;
    recordWsEvent('c', 'o', client ? client->id() : 0U);
    if (client) client->setCloseClientOnQueueFull(false);
    sendConfig(client);
    const auto snap = uart_telem::snapshot();
    sendSideSnapshot(client, snap);
    return;
  }
  if (type == WS_EVT_DISCONNECT) {
    g_ctrl_disconnect_count++;
    g_ctrl_last_disconnect_ms = millis();
    recordWsEvent('c', 'x', client ? client->id() : 0U);
    return;
  }
  if (type == WS_EVT_ERROR) {
    g_ctrl_error_count++;
    g_ctrl_last_error_ms = millis();
    g_ctrl_last_error_code = arg ? *((uint16_t*)arg) : 0U;
    recordWsEvent('c', 'e', client ? client->id() : 0U, g_ctrl_last_error_code);
    return;
  }
  if (type != WS_EVT_DATA || !data || len == 0) return;
  AwsFrameInfo* info = (AwsFrameInfo*)arg;
  if (!info || !info->final || info->index != 0 || info->opcode != WS_TEXT) return;

  StaticJsonDocument<384> in;
  if (deserializeJson(in, data, len) != DeserializationError::Ok) return;
  const char* t = in["type"] | "";
  const char* cmdName = in["cmd"] | "";
  const bool hasReqId = !in["req_id"].isNull();
  const uint32_t reqId = in["req_id"] | 0U;

  if (strcmp(t, "ping") == 0) {
    StaticJsonDocument<128> out;
    out["type"] = "pong";
    if (hasReqId) out["req_id"] = reqId;
    out["t_ms"] = in["t_ms"] | 0;
    out["srv_ms"] = millis();
    out["rssi"] = apClientRssiDbm();
    (void)sendWsJson(client, out);
    return;
  }

  if (strcmp(t, "set_fusion") == 0 || (strcmp(t, "cmd") == 0 && strcmp(cmdName, "setFusionSettings") == 0)) {
    telem::CmdSetFusionSettingsV1 cmd = {};
    JsonVariantConst src = in;
    if (in["settings"].is<JsonObjectConst>()) src = in["settings"];
    cmd.gain = src["gain"] | 0.0f;
    cmd.accelerationRejection = src["accelerationRejection"] | 0.0f;
    cmd.magneticRejection = src["magneticRejection"] | 0.0f;
    cmd.recoveryTriggerPeriod = src["recoveryTriggerPeriod"] | 0;
    const bool ok = uart_telem::sendSetFusionSettings(cmd);
    Serial.printf("SET_FUSION tx_ok=%u gain=%.3f accRej=%.2f magRej=%.2f rec=%u\n",
                  ok ? 1U : 0U, (double)cmd.gain, (double)cmd.accelerationRejection,
                  (double)cmd.magneticRejection, (unsigned)cmd.recoveryTriggerPeriod);
    StaticJsonDocument<128> out;
    out["type"] = "ack";
    out["cmd"] = "set_fusion";
    out["ok"] = ok;
    if (hasReqId) out["req_id"] = reqId;
    (void)sendWsJson(client, out);
    return;
  }

  if (strcmp(t, "get_fusion") == 0 || (strcmp(t, "cmd") == 0 && strcmp(cmdName, "getFusionSettings") == 0)) {
    const bool ok = uart_telem::sendGetFusionSettings();
    const auto snap = uart_telem::snapshot();
    StaticJsonDocument<192> out;
    out["type"] = "ack";
    out["cmd"] = "get_fusion";
    out["ok"] = ok;
    if (hasReqId) out["req_id"] = reqId;
    if (snap.has_fusion_settings || snap.has_state) {
      JsonObject fusion = out["fusion"].to<JsonObject>();
      if (snap.has_fusion_settings) {
        fusion["gain"] = snap.fusion_settings.gain;
        fusion["accelerationRejection"] = snap.fusion_settings.accelerationRejection;
        fusion["magneticRejection"] = snap.fusion_settings.magneticRejection;
        fusion["recoveryTriggerPeriod"] = snap.fusion_settings.recoveryTriggerPeriod;
      } else {
        fusion["gain"] = snap.state.fusion_gain;
        fusion["accelerationRejection"] = snap.state.fusion_accel_rej;
        fusion["magneticRejection"] = snap.state.fusion_mag_rej;
        fusion["recoveryTriggerPeriod"] = snap.state.fusion_recovery_period;
      }
    }
    (void)sendWsJson(client, out);
    return;
  }

  if (strcmp(t, "set_rate") == 0) {
    AppConfig cfg = config_store::get();
    cfg.source_rate_hz = in["source_rate_hz"] | cfg.source_rate_hz;
    cfg.ui_rate_hz = in["ui_rate_hz"] | cfg.ui_rate_hz;
    cfg.log_rate_hz = in["log_rate_hz"] | cfg.log_rate_hz;
    cfg.log_mode = in["log_mode"] | cfg.log_mode;
    config_store::update(cfg);
    log_store::setConfig(cfg);
    telem::CmdSetStreamRateV1 cmd = {};
    cmd.ws_rate_hz = cfg.source_rate_hz;
    cmd.log_rate_hz = cfg.log_rate_hz;
    uart_telem::sendSetStreamRate(cmd);
    StaticJsonDocument<128> out;
    out["type"] = "ack";
    out["cmd"] = "set_rate";
    out["ok"] = true;
    if (hasReqId) out["req_id"] = reqId;
    (void)sendWsJson(client, out);
    sendConfig(client);
    return;
  }
}

void wsStateEvent(AsyncWebSocket* serverRef, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data,
                  size_t len) {
  (void)serverRef;
  (void)arg;
  (void)data;
  (void)len;
  if (type == WS_EVT_CONNECT) {
    g_ws_connect_count++;
    recordWsEvent('s', 'o', client ? client->id() : 0U);
    if (client) client->setCloseClientOnQueueFull(false);
    if (client) {
      StateClientStall* slot = findStateClientStall(client->id(), true);
      if (slot) {
        slot->queued_since_ms = 0U;
        slot->strike_count = 0U;
      }
    }
    const auto snap = uart_telem::snapshot();
    if (sendStateSnapshot(client, snap)) {
      g_ws_sent_frames++;
    }
    return;
  }
  if (type == WS_EVT_DISCONNECT) {
    g_ws_disconnect_count++;
    g_ws_last_disconnect_ms = millis();
    recordWsEvent('s', 'x', client ? client->id() : 0U);
    if (client) clearStateClientStall(client->id());
    return;
  }
  if (type == WS_EVT_ERROR) {
    g_ws_error_count++;
    g_ws_last_error_ms = millis();
    g_ws_last_error_code = arg ? *((uint16_t*)arg) : 0U;
    recordWsEvent('s', 'e', client ? client->id() : 0U, g_ws_last_error_code);
    if (client) clearStateClientStall(client->id());
    return;
  }
}

void apiStatus(AsyncWebServerRequest* req) {
  const auto snap = uart_telem::snapshot();
  StaticJsonDocument<512> d;
  d["rx_fps"] = g_rx_fps;
  d["ws_clients"] = browserSessionCount();
  d["last_rx_ms"] = snap.stats.last_rx_ms;
  d["rx_bytes"] = snap.stats.rx_bytes;
  d["frames_ok"] = snap.stats.frames_ok;
  d["crc_err"] = snap.stats.crc_err;
  d["cobs_err"] = snap.stats.cobs_err;
  d["len_err"] = snap.stats.len_err;
  d["unknown_msg"] = snap.stats.unknown_msg;
  d["drop"] = snap.stats.drop;
  String out;
  serializeJson(d, out);
  req->send(200, "application/json", out);
}

void apiConfigGet(AsyncWebServerRequest* req) {
  const AppConfig& c = config_store::get();
  StaticJsonDocument<384> d;
  d["ap_ssid"] = c.ap_ssid;
  d["ap_pass"] = c.ap_pass;
  d["uart_rx_pin"] = c.uart_rx_pin;
  d["uart_tx_pin"] = c.uart_tx_pin;
  d["uart_baud"] = c.uart_baud;
  d["source_rate_hz"] = c.source_rate_hz;
  d["ui_rate_hz"] = c.ui_rate_hz;
  d["log_mode"] = c.log_mode;
  d["log_rate_hz"] = c.log_rate_hz;
  d["max_log_bytes"] = c.max_log_bytes;
  String out;
  serializeJson(d, out);
  req->send(200, "application/json", out);
}

void apiConfigPost(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
  (void)index;
  (void)total;
  StaticJsonDocument<512> in;
  if (deserializeJson(in, data, len) != DeserializationError::Ok) {
    req->send(400, "application/json", "{\"ok\":false}");
    return;
  }
  AppConfig cfg = config_store::get();
  if (in["ap_ssid"].is<const char*>()) strlcpy(cfg.ap_ssid, in["ap_ssid"], sizeof(cfg.ap_ssid));
  if (in["ap_pass"].is<const char*>()) strlcpy(cfg.ap_pass, in["ap_pass"], sizeof(cfg.ap_pass));
  cfg.uart_baud = in["uart_baud"] | cfg.uart_baud;
  cfg.source_rate_hz = in["source_rate_hz"] | cfg.source_rate_hz;
  cfg.ui_rate_hz = in["ui_rate_hz"] | cfg.ui_rate_hz;
  cfg.log_mode = in["log_mode"] | cfg.log_mode;
  cfg.log_rate_hz = in["log_rate_hz"] | cfg.log_rate_hz;
  cfg.max_log_bytes = in["max_log_bytes"] | cfg.max_log_bytes;
  config_store::update(cfg);
  log_store::setConfig(cfg);
  uart_telem::reconfigure(cfg);
  telem::CmdSetStreamRateV1 cmd = {};
  cmd.ws_rate_hz = cfg.source_rate_hz;
  cmd.log_rate_hz = cfg.log_rate_hz;
  (void)uart_telem::sendSetStreamRate(cmd);
  req->send(200, "application/json", "{\"ok\":true}");
}

void apiFiles(AsyncWebServerRequest* req) {
  AsyncWebServerResponse* res = req->beginResponse(200, "application/json", log_store::filesJson());
  res->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  res->addHeader("Pragma", "no-cache");
  req->send(res);
}

void apiDiag(AsyncWebServerRequest* req) {
  String out;
  out.reserve(65536);
  out += "ms,rx_fps,source_rate_hz_cfg,ui_rate_hz_cfg,log_rate_hz_cfg,log_mode,ctrl_clients,state_clients,ws_backpressure,ws_drop,ws_queue_cur,ws_queue_max,snap_seq,pub_seq,uart_state_age_ms,pub_state_age_ms,ctrl_disconnects,state_disconnects,log_queue_cur,log_queue_max,log_enqueued,log_dropped,log_records_written,log_bytes_written,fs_open_last_ms,fs_open_max_ms,fs_write_last_ms,fs_write_max_ms,fs_close_last_ms,fs_close_max_ms,fs_delete_last_ms,fs_delete_max_ms,fs_download_last_ms,fs_download_max_ms,wifi_rssi_dbm\n";
  const size_t start = (g_diag_count == kDiagCapacity) ? g_diag_head : 0U;
  for (size_t i = 0; i < g_diag_count; ++i) {
    const size_t idx = (start + i) % kDiagCapacity;
    const DiagSample& s = g_diag_ring[idx];
    out += String(s.ms);
    out += ',';
    out += String(s.rx_fps, 2);
    out += ',';
    out += String(s.source_rate_hz_cfg);
    out += ',';
    out += String(s.ui_rate_hz_cfg);
    out += ',';
    out += String(s.log_rate_hz_cfg);
    out += ',';
    out += String(s.log_mode);
    out += ',';
    out += String(s.ctrl_clients);
    out += ',';
    out += String(s.state_clients);
    out += ',';
    out += String(s.ws_backpressure);
    out += ',';
    out += String(s.ws_drop);
    out += ',';
    out += String(s.ws_queue_cur);
    out += ',';
    out += String(s.ws_queue_max);
    out += ',';
    out += String(s.snap_seq);
    out += ',';
    out += String(s.pub_seq);
    out += ',';
    out += String(s.uart_state_age_ms);
    out += ',';
    out += String(s.pub_state_age_ms);
    out += ',';
    out += String(s.ctrl_disconnects);
    out += ',';
    out += String(s.state_disconnects);
    out += ',';
    out += String(s.log_queue_cur);
    out += ',';
    out += String(s.log_queue_max);
    out += ',';
    out += String(s.log_enqueued);
    out += ',';
    out += String(s.log_dropped);
    out += ',';
    out += String(s.log_records_written);
    out += ',';
    out += String(s.log_bytes_written);
    out += ',';
    out += String(s.fs_open_last_ms);
    out += ',';
    out += String(s.fs_open_max_ms);
    out += ',';
    out += String(s.fs_write_last_ms);
    out += ',';
    out += String(s.fs_write_max_ms);
    out += ',';
    out += String(s.fs_close_last_ms);
    out += ',';
    out += String(s.fs_close_max_ms);
    out += ',';
    out += String(s.fs_delete_last_ms);
    out += ',';
    out += String(s.fs_delete_max_ms);
    out += ',';
    out += String(s.fs_download_last_ms);
    out += ',';
    out += String(s.fs_download_max_ms);
    out += ',';
    out += String(s.wifi_rssi_dbm);
    out += '\n';
  }
  req->send(200, "text/csv", out);
}

void apiWsEvents(AsyncWebServerRequest* req) {
  String out;
  out.reserve(24576);
  out += "ms,socket,event,client_id,code\n";
  const size_t start = (g_ws_event_count == kWsEventCapacity) ? g_ws_event_head : 0U;
  for (size_t i = 0; i < g_ws_event_count; ++i) {
    const size_t idx = (start + i) % kWsEventCapacity;
    const WsEventSample& s = g_ws_event_ring[idx];
    out += String(s.ms);
    out += ',';
    out += s.socket_kind;
    out += ',';
    out += s.event_kind;
    out += ',';
    out += String(s.client_id);
    out += ',';
    out += String(s.code);
    out += '\n';
  }
  req->send(200, "text/csv", out);
}

void resetWsCounters() {
  g_last_fps_ms = millis();
  g_last_frames_ok = uart_telem::snapshot().stats.frames_ok;
  g_rx_fps = 0.0f;
  g_ws_seq = 0;
  g_ws_drop_frames = 0;
  g_ws_sent_frames = 0;
  g_ws_backpressure_hits = 0;
  g_ws_queue_max = 0;
  g_ws_queue_cur = 0;
  g_ws_connect_count = 0;
  g_ws_disconnect_count = 0;
  g_ws_error_count = 0;
  g_ws_last_disconnect_ms = 0;
  g_ws_last_error_ms = 0;
  g_ws_last_error_code = 0;
  g_ctrl_connect_count = 0;
  g_ctrl_disconnect_count = 0;
  g_ctrl_error_count = 0;
  g_ctrl_last_disconnect_ms = 0;
  g_ctrl_last_error_ms = 0;
  g_ctrl_last_error_code = 0;
  g_last_pub_ack_seq = 0;
  g_ws_state_stall_resets = 0;
  g_diag_head = 0;
  g_diag_count = 0;
  g_ws_event_head = 0;
  g_ws_event_count = 0;
  for (size_t i = 0; i < kStateClientStallCapacity; ++i) {
    g_state_client_stalls[i] = {};
  }
  log_store::resetStats();
}

void apiResetCounters(AsyncWebServerRequest* req) {
  resetWsCounters();
  AsyncWebServerResponse* res = req->beginResponse(200, "application/json", "{\"ok\":true}");
  res->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  res->addHeader("Pragma", "no-cache");
  req->send(res);
}

void apiDelete(AsyncWebServerRequest* req) {
  if (!req->hasParam("name")) {
    AsyncWebServerResponse* res = req->beginResponse(400, "application/json", "{\"ok\":false}");
    res->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    req->send(res);
    return;
  }
  const String name = req->getParam("name")->value();
  const bool ok = log_store::deleteFileByName(name);
  AsyncWebServerResponse* res =
      req->beginResponse(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  res->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  req->send(res);
}

void apiDownload(AsyncWebServerRequest* req) {
  if (!req->hasParam("name")) {
    req->send(400, "text/plain", "missing name");
    return;
  }
  log_store::sendDownload(req, req->getParam("name")->value());
}

void broadcastBinaryState() {
  const auto snap = uart_telem::snapshot();
  if (!snap.has_state) return;
  const bool newState = (snap.seq != g_last_pub_state_seq);
  if (!newState) return;

  const uint32_t now = millis();
  updateRxFps(snap, now);

  uint32_t sentClients = 0;
  g_ws_queue_cur = 0;
  AsyncWebSocketClient* stalledClients[kStateClientStallCapacity] = {};
  size_t stalledCount = 0;
  for (AsyncWebSocketClient& client : g_ws_state.getClients()) {
    if (client.status() != WS_CONNECTED) continue;
    StateClientStall* stall = findStateClientStall(client.id(), true);
    const uint32_t qlen = client.queueLen();
    if (qlen > g_ws_queue_cur) g_ws_queue_cur = qlen;
    if (qlen > g_ws_queue_max) g_ws_queue_max = qlen;
    if (qlen > 0U) {
      if (stall && stall->queued_since_ms == 0U) {
        stall->queued_since_ms = now;
      } else if (stall && (uint32_t)(now - stall->queued_since_ms) >= kStateSocketStallMs) {
        stall->queued_since_ms = now;
        if (stall->strike_count < 0xFFU) stall->strike_count++;
        if (stall->strike_count == 1U) {
          recordWsEvent('s', 'u', client.id(), 0U);
        } else if (stalledCount < kStateClientStallCapacity) {
          stalledClients[stalledCount++] = &client;
        }
      }
      continue;
    }
    if (stall) {
      stall->queued_since_ms = 0U;
      stall->strike_count = 0U;
    }
    if (!client.canSend() || client.queueIsFull()) {
      g_ws_backpressure_hits++;
      g_ws_drop_frames++;
      continue;
    }
    if (sendStateSnapshot(&client, snap)) {
      sentClients++;
    } else {
      g_ws_drop_frames++;
    }
  }

  for (size_t i = 0; i < stalledCount; ++i) {
    AsyncWebSocketClient* client = stalledClients[i];
    if (!client) continue;
    g_ws_state_stall_resets++;
    recordWsEvent('s', 't', client->id(), 0U);
    clearStateClientStall(client->id());
    client->close();
  }
  if (sentClients == 0) return;
  g_ws_sent_frames += sentClients;
  g_last_pub_state_seq = snap.seq;
  g_last_state_pub_ms = now;
}

void broadcastSideJson() {
  const auto snap = uart_telem::snapshot();
  const auto logStats = log_store::stats();
  const uint32_t now = millis();
  updateRxFps(snap, now);
  const bool sideDue = (uint32_t)(now - g_last_side_ms) >= 1000U;
  const bool newAck = (snap.has_ack && snap.ack_rx_seq != g_last_pub_ack_seq);
  if (!sideDue && !newAck) return;

  StaticJsonDocument<512> d;
  d["type"] = "side";
  JsonObject stats = d["st"].to<JsonObject>();
  stats["rf"] = g_rx_fps;
  stats["wc"] = browserSessionCount();
  stats["cc"] = g_ws_ctrl.count();
  stats["sc"] = g_ws_state.count();
  stats["lm"] = config_store::get().log_mode;
  stats["ce"] = snap.stats.crc_err;
  stats["co"] = snap.stats.cobs_err;
  stats["le"] = snap.stats.len_err;
  stats["ue"] = snap.stats.unknown_msg;
  stats["dr"] = snap.stats.drop;
  stats["wd"] = g_ws_drop_frames;
  stats["ws"] = g_ws_sent_frames;
  stats["wb"] = g_ws_backpressure_hits;
  stats["wqc"] = g_ws_queue_cur;
  stats["wq"] = g_ws_queue_max;
  stats["wdc"] = g_ws_disconnect_count;
  stats["cdc"] = g_ctrl_disconnect_count;
  stats["lqc"] = logStats.queue_cur;
  stats["lqm"] = logStats.queue_max;
  stats["lqe"] = logStats.enqueued;
  stats["lqd"] = logStats.dropped;
  stats["lrw"] = logStats.records_written;
  stats["lbw"] = logStats.bytes_written;
  stats["lqf"] = logStats.flushes;
  stats["fol"] = logStats.fs_open_last_ms;
  stats["fom"] = logStats.fs_open_max_ms;
  stats["fwl"] = logStats.fs_write_last_ms;
  stats["fwm"] = logStats.fs_write_max_ms;
  stats["fcl"] = logStats.fs_close_last_ms;
  stats["fcm"] = logStats.fs_close_max_ms;
  stats["fdl"] = logStats.fs_delete_last_ms;
  stats["fdm"] = logStats.fs_delete_max_ms;
  stats["frl"] = logStats.fs_download_last_ms;
  stats["frm"] = logStats.fs_download_max_ms;
  stats["sra"] = snap.stats.last_rx_ms ? (now - snap.stats.last_rx_ms) : 0U;
  stats["spa"] = g_last_state_pub_ms ? (now - g_last_state_pub_ms) : 0U;
  stats["shs"] = snap.has_state ? 1U : 0U;
  stats["ssq"] = snap.seq;
  stats["psq"] = g_last_pub_state_seq;

  JsonObject link = d["l"].to<JsonObject>();
  link["wr"] = apClientRssiDbm();
  link["lr"] = snap.stats.last_rx_ms;

  if (snap.has_ack) {
    JsonObject ack = d["ca"].to<JsonObject>();
    ack["r"] = snap.ack_rx_seq;
    ack["c"] = snap.ack_command;
    ack["o"] = snap.ack_ok;
    ack["cd"] = snap.ack_code;
  }

  uint32_t sentClients = 0;
  for (AsyncWebSocketClient& client : g_ws_ctrl.getClients()) {
    if (!sendWsJson(&client, d)) continue;
    sentClients++;
  }
  if (sentClients == 0) return;
  if (sideDue) g_last_side_ms = now;
  if (newAck) g_last_pub_ack_seq = snap.ack_rx_seq;
}

}  // namespace

void begin() {
  g_ws_ctrl.onEvent(wsCtrlEvent);
  g_ws_state.onEvent(wsStateEvent);
  g_server.addHandler(&g_ws_ctrl);
  g_server.addHandler(&g_ws_state);

  g_server.on("/api/status", HTTP_GET, apiStatus);
  g_server.on("/api/config", HTTP_GET, apiConfigGet);
  g_server.on("/api/files", HTTP_GET, apiFiles);
  g_server.on("/api/diag", HTTP_GET, apiDiag);
  g_server.on("/api/ws_events", HTTP_GET, apiWsEvents);
  g_server.on("/api/reset_counters", HTTP_POST, apiResetCounters);
  g_server.on("/api/delete", HTTP_GET, apiDelete);
  g_server.on("/api/download", HTTP_GET, apiDownload);
  g_server.on(
      "/api/config", HTTP_POST,
      [](AsyncWebServerRequest* req) {},
      nullptr, apiConfigPost);
  g_server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  g_server.begin();
}

void loop() {
  const uint32_t now = millis();
  if ((uint32_t)(now - g_last_cleanup_ms) >= 10000U) {
    g_ws_ctrl.cleanupClients();
    g_ws_state.cleanupClients();
    g_last_cleanup_ms = now;
  }

  if (g_ws_ctrl.count() > 0) {
    broadcastSideJson();
  }

  recordDiagSample(uart_telem::snapshot(), now);

  if (g_ws_state.count() == 0) return;
  const AppConfig& cfg = config_store::get();
  const uint32_t period = (uint32_t)(1000U / (cfg.ui_rate_hz ? cfg.ui_rate_hz : 1U));
  if ((uint32_t)(now - g_last_ws_ms) < period) return;
  g_last_ws_ms = now;
  broadcastBinaryState();
}

uint32_t clientCount() { return browserSessionCount(); }

}  // namespace ws_server
