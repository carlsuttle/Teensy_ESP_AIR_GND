#include "radio_link.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <string.h>

#include "config_store.h"
#include "control_plane.h"
#include "log_store.h"
#include "replay_bridge.h"
#include "sd_file_api.h"
#include "sd_backend.h"
#include "time_service.h"
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
constexpr uint16_t kUnifiedDownlinkRateHz = 30U;
constexpr uint16_t kUnifiedGpsRateHz = 10U;
constexpr uint8_t kDefaultRadioControlRateHz = 2U;
constexpr uint8_t kSendFailThreshold = 24U;
constexpr size_t kTxQueueCapacity = 24U;
constexpr uint32_t kSendRetryBackoffMs = 2U;
constexpr uint32_t kSendCompleteTimeoutMs = 200U;
constexpr uint32_t kFileListChunkQueueWaitMs = 5000U;

struct RxFrame {
  uint8_t mac[6] = {};
  uint16_t len = 0U;
  uint8_t data[telem::kEspNowMaxDataLen] = {};
};

struct TxFrame {
  uint8_t mac[6] = {};
  uint16_t len = 0U;
  uint8_t data[telem::kEspNowMaxDataLen] = {};
};

struct LatestTelemetryFrame {
  bool valid = false;
  uint8_t mac[6] = {};
  uint16_t len = 0U;
  uint8_t data[telem::kEspNowMaxDataLen] = {};
  uint32_t seq = 0U;
  uint16_t msg_type = 0U;
};

portMUX_TYPE g_rx_mux = portMUX_INITIALIZER_UNLOCKED;
RxFrame g_rx_queue[kRxQueueCapacity];
volatile uint8_t g_rx_head = 0U;
volatile uint8_t g_rx_tail = 0U;
volatile uint32_t g_rx_drop_events = 0U;
volatile uint32_t g_send_ok_events = 0U;
volatile uint32_t g_send_fail_events = 0U;

Stats g_stats;
TxFrame g_tx_queue[kTxQueueCapacity];
LatestTelemetryFrame g_latest_telem = {};
uint8_t g_gnd_mac[6] = {};
uint8_t g_last_sender_mac[6] = {};
bool g_has_gnd_mac = false;
bool g_has_last_sender_mac = false;
bool g_espnow_ready = false;
bool g_network_reset_requested = false;
bool g_state_only_mode = false;
bool g_recorder_enabled = false;
bool g_verbose = true;
bool g_log_requested = false;
bool g_log_backend_ready = false;
bool g_log_media_present = false;
bool g_rssi_valid = false;
bool g_file_list_transfer_active = false;
sd_file_api::FileListPage g_file_list_page = {};
telem::StorageStatusPayloadV1 g_storage_status_payload = {};
bool g_control_has_ack = false;
bool g_control_ack_ok = false;
bool g_radio_lr_mode = true;
uint8_t g_consecutive_send_failures = 0U;
uint8_t g_radio_control_rate_hz = kDefaultRadioControlRateHz;
uint32_t g_last_hello_tx_ms = 0U;
uint32_t g_last_telem_tx_ms = 0U;
uint32_t g_last_gps_tx_ms = 0U;
uint32_t g_last_control_tx_ms = 0U;
uint32_t g_last_rssi_sample_ms = 0U;
uint32_t g_last_state_seq = 0U;
uint32_t g_last_state_log_seq = 0U;
uint32_t g_last_state_log_ms = 0U;
uint32_t g_last_ack_seq = 0U;
uint32_t g_tx_seq = 0U;
uint32_t g_session_id = 0U;
uint32_t g_log_session_id = 0U;
uint32_t g_log_last_change_ms = 0U;
uint16_t g_log_last_command = 0U;
uint16_t g_control_ack_command = 0U;
uint16_t g_radio_telem_rate_hz = kUnifiedDownlinkRateHz;
int16_t g_gnd_ap_rssi_dbm = 0;
uint32_t g_control_ack_code = 0U;
uint8_t g_tx_head = 0U;
uint8_t g_tx_tail = 0U;
bool g_tx_in_flight = false;
uint32_t g_tx_in_flight_started_ms = 0U;
uint32_t g_next_tx_attempt_ms = 0U;
uint32_t g_tx_in_flight_seq = 0U;
uint16_t g_tx_in_flight_msg_type = 0U;
volatile uint32_t g_send_ok_last_elapsed_ms = 0U;
volatile uint32_t g_send_ok_last_seq = 0U;
volatile uint16_t g_send_ok_last_msg_type = 0U;
volatile uint32_t g_send_fail_last_elapsed_ms = 0U;
volatile uint32_t g_send_fail_last_seq = 0U;
volatile uint16_t g_send_fail_last_msg_type = 0U;

bool stateOnlyModeEnabled() {
  return g_state_only_mode;
}

bool isTelemetryMsgType(telem::MsgType type) {
  return type == telem::TELEM_FULL_STATE || type == telem::TELEM_UNIFIED_DOWNLINK;
}

const char* msgTypeText(uint16_t msg_type) {
  switch ((telem::MsgType)msg_type) {
    case telem::TELEM_FULL_STATE: return "state";
    case telem::TELEM_UNIFIED_DOWNLINK: return "unified";
    case telem::LINK_HELLO: return "hello";
    case telem::ACK: return "ack";
    case telem::NACK: return "nack";
    default: return "other";
  }
}

bool isBroadcastMac(const uint8_t* mac) {
  return mac && memcmp(mac, kBroadcastMac, sizeof(kBroadcastMac)) == 0;
}

bool isZeroMac(const uint8_t* mac) {
  static const uint8_t kZeroMac[6] = {};
  return !mac || memcmp(mac, kZeroMac, sizeof(kZeroMac)) == 0;
}

bool isNewerSeq(uint32_t seq, uint32_t last_seq) {
  if (last_seq == 0U) return true;
  return (int32_t)(seq - last_seq) > 0;
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

bool initEspNow();
void clearPeerState();
bool sendFrame(telem::MsgType type, const void* payload, size_t payload_len, uint32_t seq, uint32_t t_us);
void sendReplayStatusFrame();
void sendLogFileListFrames(uint16_t offset = 0U, uint16_t limit = 32U);
void sendStorageStatusFrame();
void drainAsyncEvents();
void pumpTx();

struct FileListStreamContext {
  telem::LogFileListChunkPayloadV1 page = {};
  uint16_t page_count = 0U;
  uint16_t page_offset = 0U;
  uint16_t total_files = 0U;
  bool has_more = false;
  bool failed = false;
};

bool flushFileListPage(FileListStreamContext& ctx, bool complete) {
  ctx.page.offset = ctx.page_offset;
  ctx.page.total_files = complete ? ctx.total_files : 0U;
  ctx.page.flags = complete ? telem::kLogFileListFlagComplete : 0U;
  if (ctx.has_more) ctx.page.flags |= telem::kLogFileListFlagTruncated;
  ctx.page.entries_in_chunk = ctx.page_count;
  const uint32_t wait_started_ms = millis();
  while (!sendFrame(telem::TELEM_LOG_FILE_LIST, &ctx.page, sizeof(ctx.page), 0U, micros())) {
    drainAsyncEvents();
    pumpTx();
    delay(2);
    if ((uint32_t)(millis() - wait_started_ms) >= kFileListChunkQueueWaitMs) {
      ctx.failed = true;
      return false;
    }
  }
  ctx.page_offset = (uint16_t)(ctx.page_offset + ctx.page_count);
  ctx.page_count = 0U;
  ctx.page.offset = 0U;
  ctx.page.total_files = 0U;
  ctx.page.flags = 0U;
  ctx.page.entries_in_chunk = 0U;
  memset(ctx.page.entries, 0, sizeof(ctx.page.entries));
  return true;
}

bool appendFileListEntry(const telem::LogFileInfoV1& entry, uint16_t index, void* ctx_ptr) {
  FileListStreamContext& ctx = *reinterpret_cast<FileListStreamContext*>(ctx_ptr);
  ctx.total_files = (uint16_t)(index + 1U);
  if (ctx.page_count < telem::kLogFileChunkEntries) {
    ctx.page.entries[ctx.page_count] = entry;
    ctx.page_count++;
  }
  if (ctx.page_count >= telem::kLogFileChunkEntries) {
    return flushFileListPage(ctx, false);
  }
  return true;
}

uint8_t desiredProtocol() {
  const uint8_t kNormalMask = (uint8_t)(WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  return g_radio_lr_mode ? (uint8_t)(kNormalMask | WIFI_PROTOCOL_LR) : kNormalMask;
}

void applyRadioProtocol() {
  (void)esp_wifi_set_protocol(WIFI_IF_STA, desiredProtocol());
}

size_t txQueueCountLocked() {
  if (g_tx_head >= g_tx_tail) return (size_t)(g_tx_head - g_tx_tail);
  return (size_t)(kTxQueueCapacity - g_tx_tail + g_tx_head);
}

size_t txQueueFreeLocked() {
  return (kTxQueueCapacity - 1U) - txQueueCountLocked();
}

bool enqueueTxFrame(const uint8_t* mac, const uint8_t* data, size_t len) {
  if (!mac || !data || len == 0U || len > telem::kEspNowMaxDataLen) return false;

  bool queued = false;
  portENTER_CRITICAL(&g_rx_mux);
  const uint8_t next_head = (uint8_t)((g_tx_head + 1U) % kTxQueueCapacity);
  if (next_head != g_tx_tail) {
    TxFrame& frame = g_tx_queue[g_tx_head];
    memcpy(frame.mac, mac, sizeof(frame.mac));
    frame.len = (uint16_t)len;
    memcpy(frame.data, data, len);
    g_tx_head = next_head;
    queued = true;
  }
  portEXIT_CRITICAL(&g_rx_mux);
  if (!queued && g_verbose) {
    const size_t qcount = txQueueCountLocked();
    const size_t qfree = txQueueFreeLocked();
    Serial.printf("AIRTXQ full qcount=%u qfree=%u inflight=%u next_ms=%lu\n",
                  (unsigned)qcount,
                  (unsigned)qfree,
                  g_tx_in_flight ? 1U : 0U,
                  (unsigned long)g_next_tx_attempt_ms);
  }
  return queued;
}

bool peekTxFrame(TxFrame& out) {
  bool have_frame = false;
  portENTER_CRITICAL(&g_rx_mux);
  if (g_tx_tail != g_tx_head) {
    out = g_tx_queue[g_tx_tail];
    have_frame = true;
  }
  portEXIT_CRITICAL(&g_rx_mux);
  return have_frame;
}

void popTxFrame() {
  portENTER_CRITICAL(&g_rx_mux);
  if (g_tx_tail != g_tx_head) {
    g_tx_tail = (uint8_t)((g_tx_tail + 1U) % kTxQueueCapacity);
  }
  portEXIT_CRITICAL(&g_rx_mux);
}

void clearTxQueue() {
  portENTER_CRITICAL(&g_rx_mux);
  g_tx_head = 0U;
  g_tx_tail = 0U;
  g_tx_in_flight = false;
  g_latest_telem = {};
  portEXIT_CRITICAL(&g_rx_mux);
  g_tx_in_flight_started_ms = 0U;
  g_next_tx_attempt_ms = 0U;
  g_tx_in_flight_seq = 0U;
  g_tx_in_flight_msg_type = 0U;
}

uint32_t peerSilenceMs() {
  return g_stats.last_rx_ms ? (uint32_t)(millis() - g_stats.last_rx_ms) : 0xFFFFFFFFUL;
}

void noteSendFailure(uint32_t count) {
  if (!g_has_gnd_mac || count == 0U) return;
  const uint32_t total = (uint32_t)g_consecutive_send_failures + count;
  g_consecutive_send_failures = (total > 0xFFU) ? 0xFFU : (uint8_t)total;
  if (stateOnlyModeEnabled()) return;
  if (g_consecutive_send_failures >= kSendFailThreshold && peerSilenceMs() >= kPeerStaleMs) {
    clearPeerState();
  }
}

void clearPeerState() {
  memset(g_gnd_mac, 0, sizeof(g_gnd_mac));
  memset(g_last_sender_mac, 0, sizeof(g_last_sender_mac));
  g_has_gnd_mac = false;
  g_has_last_sender_mac = false;
  g_consecutive_send_failures = 0U;
  g_control_has_ack = false;
  g_control_ack_command = 0U;
  g_control_ack_code = 0U;
  g_control_ack_ok = false;
  g_last_hello_tx_ms = 0U;
  g_last_telem_tx_ms = 0U;
  g_last_gps_tx_ms = 0U;
  g_last_control_tx_ms = 0U;
  clearTxQueue();
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
  const uint32_t elapsed_ms =
      (g_tx_in_flight_started_ms != 0U) ? (uint32_t)(millis() - g_tx_in_flight_started_ms) : 0U;
  if (status == ESP_NOW_SEND_SUCCESS) {
    g_send_ok_last_elapsed_ms = elapsed_ms;
    g_send_ok_last_seq = g_tx_in_flight_seq;
    g_send_ok_last_msg_type = g_tx_in_flight_msg_type;
    g_send_ok_events++;
  } else {
    g_send_fail_last_elapsed_ms = elapsed_ms;
    g_send_fail_last_seq = g_tx_in_flight_seq;
    g_send_fail_last_msg_type = g_tx_in_flight_msg_type;
    g_send_fail_events++;
  }
}

bool takeLatestTelemetryFrame(TxFrame& out) {
  bool have_frame = false;
  portENTER_CRITICAL(&g_rx_mux);
  if (g_latest_telem.valid) {
    memcpy(out.mac, g_latest_telem.mac, sizeof(out.mac));
    out.len = g_latest_telem.len;
    memcpy(out.data, g_latest_telem.data, g_latest_telem.len);
    g_tx_in_flight_seq = g_latest_telem.seq;
    g_tx_in_flight_msg_type = g_latest_telem.msg_type;
    g_latest_telem.valid = false;
    have_frame = true;
  }
  portEXIT_CRITICAL(&g_rx_mux);
  return have_frame;
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
  if (send_ok > 0U || send_fail > 0U) {
    g_tx_in_flight = false;
    g_tx_in_flight_started_ms = 0U;
  }
  if (send_ok > 0U) {
    if (g_verbose) {
      Serial.printf("AIRTX cb_ok seq=%lu elapsed_ms=%lu type=%s qfree=%u\n",
                    (unsigned long)g_send_ok_last_seq,
                    (unsigned long)g_send_ok_last_elapsed_ms,
                    msgTypeText(g_send_ok_last_msg_type),
                    (unsigned)txQueueFree());
    }
    g_consecutive_send_failures = 0U;
  }
  if (send_fail > 0U) {
    g_stats.tx_drop += send_fail;
    if (g_verbose) {
      Serial.printf("AIRTX cb_fail count=%lu seq=%lu elapsed_ms=%lu type=%s qfree=%u inflight=%u silence_ms=%lu\n",
                    (unsigned long)send_fail,
                    (unsigned long)g_send_fail_last_seq,
                    (unsigned long)g_send_fail_last_elapsed_ms,
                    msgTypeText(g_send_fail_last_msg_type),
                    (unsigned)txQueueFree(),
                    g_tx_in_flight ? 1U : 0U,
                    (unsigned long)peerSilenceMs());
    }
    noteSendFailure(send_fail);
  }
}

void pumpTx() {
  const uint32_t now = millis();
  if (g_tx_in_flight) {
    if (g_tx_in_flight_started_ms != 0U &&
        (uint32_t)(now - g_tx_in_flight_started_ms) >= kSendCompleteTimeoutMs) {
      if (g_verbose) {
        Serial.printf("AIRTX timeout seq=%lu wait_ms=%lu type=%s qfree=%u qcount=%u\n",
                      (unsigned long)g_tx_in_flight_seq,
                      (unsigned long)(now - g_tx_in_flight_started_ms),
                      msgTypeText(g_tx_in_flight_msg_type),
                      (unsigned)txQueueFree(),
                      (unsigned)txQueueCountLocked());
      }
      g_tx_in_flight = false;
      g_tx_in_flight_started_ms = 0U;
      g_next_tx_attempt_ms = now + kSendRetryBackoffMs;
      g_stats.tx_drop++;
      noteSendFailure(1U);
    } else {
      return;
    }
  }
  if (g_next_tx_attempt_ms != 0U && (int32_t)(now - g_next_tx_attempt_ms) < 0) return;

  TxFrame frame = {};
  bool sending_latest_telemetry = takeLatestTelemetryFrame(frame);
  if (!sending_latest_telemetry) {
    if (!peekTxFrame(frame)) return;
    telem::FrameHeader hdr = {};
    memcpy(&hdr, frame.data, sizeof(hdr));
    g_tx_in_flight_seq = hdr.seq;
    g_tx_in_flight_msg_type = hdr.msg_type;
  }
  if (!initEspNow()) return;

  const esp_err_t err = esp_now_send(frame.mac, frame.data, frame.len);
  if (err == ESP_OK) {
    if (!sending_latest_telemetry) {
      popTxFrame();
    }
    g_tx_in_flight = true;
    g_tx_in_flight_started_ms = now;
    g_next_tx_attempt_ms = 0U;
    g_stats.tx_packets++;
    g_stats.tx_bytes += (uint32_t)frame.len;
    return;
  }

  if (err == ESP_ERR_ESPNOW_NO_MEM) {
    if (g_verbose) {
      Serial.printf("AIRTX nomem seq=%lu type=%s qfree=%u qcount=%u inflight=%u\n",
                    (unsigned long)g_tx_in_flight_seq,
                    msgTypeText(g_tx_in_flight_msg_type),
                    (unsigned)txQueueFree(),
                    (unsigned)txQueueCountLocked(),
                    g_tx_in_flight ? 1U : 0U);
    }
    if (sending_latest_telemetry) {
      portENTER_CRITICAL(&g_rx_mux);
      if (!g_latest_telem.valid || isNewerSeq(g_tx_in_flight_seq, g_latest_telem.seq)) {
        memcpy(g_latest_telem.mac, frame.mac, sizeof(frame.mac));
        g_latest_telem.len = frame.len;
        memcpy(g_latest_telem.data, frame.data, frame.len);
        g_latest_telem.seq = g_tx_in_flight_seq;
        g_latest_telem.msg_type = g_tx_in_flight_msg_type;
        g_latest_telem.valid = true;
      }
      portEXIT_CRITICAL(&g_rx_mux);
    }
    g_next_tx_attempt_ms = now + kSendRetryBackoffMs;
    return;
  }

  if (!sending_latest_telemetry) {
    popTxFrame();
  }
  g_next_tx_attempt_ms = now + kSendRetryBackoffMs;
  g_stats.tx_drop++;
  if (g_verbose) {
    Serial.printf("AIRTX send_err err=%d seq=%lu type=%s qfree=%u qcount=%u inflight=%u\n",
                  (int)err,
                  (unsigned long)g_tx_in_flight_seq,
                  msgTypeText(g_tx_in_flight_msg_type),
                  (unsigned)txQueueFree(),
                  (unsigned)txQueueCountLocked(),
                  g_tx_in_flight ? 1U : 0U);
  }
  noteSendFailure(1U);
  if (err == ESP_ERR_ESPNOW_NOT_INIT) g_espnow_ready = false;
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
  if (isTelemetryMsgType(type)) {
    portENTER_CRITICAL(&g_rx_mux);
    memcpy(g_latest_telem.mac, mac, sizeof(g_latest_telem.mac));
    g_latest_telem.len = (uint16_t)frame_len;
    memcpy(g_latest_telem.data, buf, frame_len);
    g_latest_telem.seq = hdr.seq;
    g_latest_telem.msg_type = hdr.msg_type;
    g_latest_telem.valid = true;
    portEXIT_CRITICAL(&g_rx_mux);
    pumpTx();
    return true;
  }
  if (!enqueueTxFrame(mac, buf, frame_len)) {
    g_stats.tx_drop++;
    if (g_verbose) {
      Serial.printf("AIRTX enqueue_drop type=%u seq=%lu len=%u peer=%s\n",
                    (unsigned)type,
                    (unsigned long)hdr.seq,
                    (unsigned)frame_len,
                    macToString(mac).c_str());
    }
    return false;
  }
  pumpTx();
  return true;
}

bool sendHelloTo(const uint8_t* mac) {
  telem::LinkHelloPayloadV1 hello = {};
  hello.unit_id = kUnitAir;
  hello.session_id = g_session_id;
  return sendFrameTo(mac, telem::LINK_HELLO, &hello, sizeof(hello), 0U, micros());
}

uint8_t currentMetaFlags() {
  const log_store::RecorderStatus recorder = log_store::recorderStatus();
  uint8_t flags = 0U;
  if (g_has_gnd_mac) flags |= telem::kLinkMetaFlagPeerKnown;
  if (g_espnow_ready) flags |= telem::kLinkMetaFlagRadioReady;
  if (recorder.active) flags |= telem::kLinkMetaFlagRecorderOn;
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

uint8_t currentLogFlags() {
  return control_plane::currentLogStatusPayload(millis()).flags;
}

telem::LogStatusPayloadV1 currentLogStatus() {
  return control_plane::currentLogStatusPayload(millis());
}

void markLogStatusDirty() {
  g_last_control_tx_ms = 0U;
}

void sampleGroundRssi() {
  const uint32_t now = millis();
  if (!g_espnow_ready) return;
  if (g_radio_lr_mode) return;
  if (g_tx_in_flight) return;
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

uint32_t rateIntervalMs(uint16_t hz) {
  if (hz == 0U) return 1000U;
  return 1000UL / (uint32_t)hz;
}

void captureSnapshotAck(const teensy_link::Snapshot& snap) {
  if (!snap.has_ack || snap.ack_rx_seq == g_last_ack_seq) return;
  g_last_ack_seq = snap.ack_rx_seq;
  g_control_has_ack = true;
  g_control_ack_command = snap.ack_command;
  g_control_ack_code = snap.ack_code;
  g_control_ack_ok = snap.ack_ok;
}

void fillLiveStatus(telem::DownlinkStatusV1& status) {
  status = {};
  const telem::LogStatusPayloadV1 log_status = currentLogStatus();
  status.log_flags = log_status.flags;
  status.log_session_id = log_status.session_id;
  status.log_bytes_written = log_status.bytes_written;
  time_service::fillDownlinkStatus(status, millis());
}

void fillFastState(const telem::TelemetryStateRecord& state, telem::DownlinkFastStateV1& fast) {
  fast.roll_deg = state.roll_deg;
  fast.pitch_deg = state.pitch_deg;
  fast.yaw_deg = state.yaw_deg;
  fast.mag_heading_deg = state.mag_heading_deg;
  fast.last_imu_ms = state.last_imu_ms;
  fast.baro_temp_c = state.baro_temp_c;
  fast.baro_press_hpa = state.baro_press_hpa;
  fast.baro_alt_m = state.baro_alt_m;
  fast.baro_vsi_mps = state.baro_vsi_mps;
  fast.last_baro_ms = state.last_baro_ms;
  fast.flags = state.flags;
}

void fillGpsState(const telem::TelemetryStateRecord& state, telem::DownlinkGpsStateV1& gps) {
  gps.iTOW_ms = state.iTOW_ms;
  gps.fixType = state.fixType;
  gps.numSV = state.numSV;
  gps.lat_1e7 = state.lat_1e7;
  gps.lon_1e7 = state.lon_1e7;
  gps.hMSL_mm = state.hMSL_mm;
  gps.gSpeed_mms = state.gSpeed_mms;
  gps.headMot_1e5deg = state.headMot_1e5deg;
  gps.hAcc_mm = state.hAcc_mm;
  gps.sAcc_mms = state.sAcc_mms;
  gps.gps_parse_errors = state.gps_parse_errors;
  gps.last_gps_ms = state.last_gps_ms;
}

void fillExtendedState(const telem::TelemetryStateRecord& state, telem::DownlinkExtendedStateV2& ext) {
  ext.fusion_gain = state.fusion_gain;
  ext.fusion_accel_rej = state.fusion_accel_rej;
  ext.fusion_mag_rej = state.fusion_mag_rej;
  ext.fusion_recovery_period = state.fusion_recovery_period;
  ext.raw_present_mask = state.raw_present_mask;
  const telem::GpsCalendarTime gps_time = telem::gpsCalendarTime(state);
  ext.gps_year = gps_time.year;
  ext.gps_month = gps_time.month;
  ext.gps_day = gps_time.day;
  ext.gps_hour = gps_time.hour;
  ext.gps_min = gps_time.minute;
  ext.gps_sec = gps_time.second;
}

bool maybeSendUnifiedDownlink(const teensy_link::Snapshot& snap) {
  const uint32_t now = millis();
  g_stats.publish_attempts++;
  g_stats.last_publish_attempt_ms = now;
  g_stats.last_publish_age_ms = snap.stats.last_rx_ms ? (uint32_t)(now - snap.stats.last_rx_ms) : 0U;

  if (!snap.has_state) {
    g_stats.publish_skip_no_state++;
    return false;
  }
  if (!g_has_gnd_mac) {
    g_stats.publish_skip_no_peer++;
    return false;
  }

  const uint32_t telem_interval_ms = rateIntervalMs(g_radio_telem_rate_hz);
  if (g_last_telem_tx_ms != 0U && (uint32_t)(now - g_last_telem_tx_ms) < telem_interval_ms) {
    g_stats.publish_skip_rate++;
    return false;
  }

  telem::UnifiedDownlinkBaseV1 base = {};
  base.source_seq = snap.seq;
  fillFastState(snap.state, base.fast);

  uint8_t payload[telem::kEspNowMaxDataLen - sizeof(telem::FrameHeader)] = {};
  size_t payload_len = sizeof(base);
  bool included_gps = false;
  bool included_extended = false;
  bool included_status = false;
  memcpy(payload, &base, sizeof(base));

  const uint32_t gps_interval_ms = rateIntervalMs(kUnifiedGpsRateHz);
  if (g_last_gps_tx_ms == 0U || (uint32_t)(now - g_last_gps_tx_ms) >= gps_interval_ms) {
    telem::DownlinkGpsStateV1 gps = {};
    fillGpsState(snap.state, gps);
    memcpy(payload + payload_len, &gps, sizeof(gps));
    payload_len += sizeof(gps);
    reinterpret_cast<telem::UnifiedDownlinkBaseV1*>(payload)->section_flags |= telem::kUnifiedDownlinkFlagHasGps;
    included_gps = true;
  }

#if TELEM_ACTIVE_SCHEMA_ID == TELEM_SCHEMA_ID_RELEASE_0_03_CANDIDATE
  {
    telem::DownlinkExtendedStateV2 ext = {};
    fillExtendedState(snap.state, ext);
    memcpy(payload + payload_len, &ext, sizeof(ext));
    payload_len += sizeof(ext);
    reinterpret_cast<telem::UnifiedDownlinkBaseV1*>(payload)->section_flags |=
        telem::kUnifiedDownlinkFlagHasExtended;
    included_extended = true;
  }
#endif

  const uint32_t control_interval_ms = rateIntervalMs(g_radio_control_rate_hz);
  if (g_last_control_tx_ms == 0U || (uint32_t)(now - g_last_control_tx_ms) >= control_interval_ms) {
    telem::DownlinkStatusV1 status = {};
    fillLiveStatus(status);
    memcpy(payload + payload_len, &status, sizeof(status));
    payload_len += sizeof(status);
    reinterpret_cast<telem::UnifiedDownlinkBaseV1*>(payload)->section_flags |= telem::kUnifiedDownlinkFlagHasStatus;
    included_status = true;
  }

  if (!sendFrame(telem::TELEM_UNIFIED_DOWNLINK, payload, payload_len, 0U, snap.t_us)) return false;
  const uint32_t prev_seq = g_last_state_log_seq;
  const uint32_t prev_ms = g_last_state_log_ms;
  const uint32_t delta_seq = prev_seq ? (snap.seq - prev_seq) : 0U;
  const uint32_t delta_ms = prev_ms ? (now - prev_ms) : 0U;
  if (g_verbose) {
    Serial.printf("AIRTX seq=%lu dseq=%lu dms=%lu tus=%lu mode=unified\n",
                  (unsigned long)snap.seq,
                  (unsigned long)delta_seq,
                  (unsigned long)delta_ms,
                  (unsigned long)snap.t_us);
  }
  g_last_state_log_seq = snap.seq;
  g_last_state_log_ms = now;
  g_last_state_seq = snap.seq;
  g_last_telem_tx_ms = now;
  g_stats.tx_unified_packets++;
  g_stats.publish_ok++;
  g_stats.last_source_seq = snap.seq;
  g_stats.last_source_t_us = snap.t_us;
  g_stats.last_tx_ms = now;
  if (included_gps) g_last_gps_tx_ms = now;
  (void)included_extended;
  if (included_status) {
    g_last_control_tx_ms = now;
  }
  return true;
}

bool maybeSendStateOnlyDownlink(const teensy_link::Snapshot& snap) {
  const uint32_t now = millis();
  g_stats.publish_attempts++;
  g_stats.last_publish_attempt_ms = now;
  g_stats.last_publish_age_ms = snap.stats.last_rx_ms ? (uint32_t)(now - snap.stats.last_rx_ms) : 0U;

  if (!snap.has_state) {
    g_stats.publish_skip_no_state++;
    return false;
  }
  if (!g_has_gnd_mac) {
    g_stats.publish_skip_no_peer++;
    return false;
  }

  const uint32_t telem_interval_ms = rateIntervalMs(g_radio_telem_rate_hz);
  if (g_last_telem_tx_ms != 0U && (uint32_t)(now - g_last_telem_tx_ms) < telem_interval_ms) {
    g_stats.publish_skip_rate++;
    return false;
  }

  if (!isNewerSeq(snap.seq, g_last_state_seq)) {
    g_stats.publish_skip_not_new++;
    return false;
  }

  const bool ok = sendFrame(telem::TELEM_FULL_STATE, &snap.state, sizeof(snap.state), snap.seq, snap.t_us);
  if (!ok) return false;

  const uint32_t prev_seq = g_last_state_log_seq;
  const uint32_t prev_ms = g_last_state_log_ms;
  const uint32_t delta_seq = prev_seq ? (snap.seq - prev_seq) : 0U;
  const uint32_t delta_ms = prev_ms ? (now - prev_ms) : 0U;
  if (g_verbose) {
    Serial.printf("AIRTX seq=%lu dseq=%lu dms=%lu tus=%lu mode=state\n",
                  (unsigned long)snap.seq,
                  (unsigned long)delta_seq,
                  (unsigned long)delta_ms,
                  (unsigned long)snap.t_us);
  }
  g_last_state_log_seq = snap.seq;
  g_last_state_log_ms = now;
  g_last_state_seq = snap.seq;
  g_last_telem_tx_ms = now;
  g_stats.tx_state_packets++;
  g_stats.publish_ok++;
  g_stats.last_source_seq = snap.seq;
  g_stats.last_source_t_us = snap.t_us;
  g_stats.last_tx_ms = now;
  return true;
}

bool sendFrame(telem::MsgType type, const void* payload, size_t payload_len, uint32_t seq, uint32_t t_us) {
  if (!g_has_gnd_mac) return false;
  return sendFrameTo(g_gnd_mac, type, payload, payload_len, seq, t_us);
}

void sendReplayStatusFrame() {
  const telem::ReplayStatusPayloadV1 payload = replay_bridge::currentPayload();
  (void)sendFrame(telem::TELEM_REPLAY_STATUS, &payload, sizeof(payload), 0U, micros());
}

void sendCurrentFileListFrames() {
  FileListStreamContext ctx = {};
  ctx.page_offset = g_file_list_page.offset;
  ctx.total_files = g_file_list_page.total_files;
  ctx.has_more = g_file_list_page.has_more;
  for (uint16_t i = 0U; i < g_file_list_page.returned_files; ++i) {
    ctx.page.entries[ctx.page_count] = g_file_list_page.entries[i];
    ctx.page_count++;
    if (ctx.page_count >= telem::kLogFileChunkEntries) {
      if (!flushFileListPage(ctx, false)) {
        return;
      }
    }
  }
  (void)flushFileListPage(ctx, true);
}

void sendLogFileListFrames(uint16_t offset, uint16_t limit) {
  g_file_list_transfer_active = true;
  control_plane::Request request = {};
  request.request_id = micros();
  request.source = control_plane::SourceInterface::Radio;
  request.category = control_plane::RequestCategory::FileSd;
  request.action = control_plane::RequestAction::FileListPage;
  request.command_id = telem::CMD_GET_LOG_FILE_LIST;
  request.offset = offset;
  request.limit = limit;
  control_plane::Result result = {};
  (void)control_plane::submit(request, result);
  const telem::SdApiStatusCode page_code = (telem::SdApiStatusCode)result.code;
  if (page_code != telem::SdApiStatusCode::OK && page_code != telem::SdApiStatusCode::NO_FILES) {
    g_file_list_transfer_active = false;
    return;
  }
  g_file_list_page = control_plane::lastFileListPage();
  sendCurrentFileListFrames();
  g_file_list_transfer_active = false;
}

const telem::StorageStatusPayloadV1& currentStorageStatusPayload() {
  memset(&g_storage_status_payload, 0, sizeof(g_storage_status_payload));
  control_plane::Request request = {};
  request.request_id = micros();
  request.source = control_plane::SourceInterface::Radio;
  request.category = control_plane::RequestCategory::FileSd;
  request.action = control_plane::RequestAction::StorageStatus;
  request.command_id = telem::CMD_GET_STORAGE_STATUS;
  control_plane::Result result = {};
  if (control_plane::submit(request, result) && result.has_storage_status) {
    g_storage_status_payload = result.storage_status;
  }
  return g_storage_status_payload;
}

void sendStorageStatusFrame() {
  const telem::StorageStatusPayloadV1& payload = currentStorageStatusPayload();
  (void)sendFrame(telem::TELEM_STORAGE_STATUS, &payload, sizeof(payload), 0U, micros());
}

void sendCommandAck(uint16_t command, bool ok, uint32_t code, uint32_t seq, uint32_t t_us) {
  telem::AckPayloadV1 ack = {};
  ack.command = command;
  ack.ok = ok ? 1U : 0U;
  ack.code = code;

  if (stateOnlyModeEnabled()) {
    if (command != telem::CMD_SET_STREAM_RATE && command != telem::CMD_SET_RADIO_MODE) return;
    (void)sendFrame(ok ? telem::ACK : telem::NACK, &ack, sizeof(ack), seq, t_us);
    return;
  }

  // Emit an immediate ACK/NACK frame so command completion does not depend on the
  // next scheduled control-status downlink.
  (void)sendFrame(ok ? telem::ACK : telem::NACK, &ack, sizeof(ack), seq, t_us);
  g_control_has_ack = true;
  g_control_ack_command = command;
  g_control_ack_code = code;
  g_control_ack_ok = ok;
}

void sendControlPlaneDispositionAck(uint16_t command,
                                    const control_plane::Result& result,
                                    uint32_t seq,
                                    uint32_t t_us) {
  const bool accepted = result.disposition == control_plane::DispositionStatus::Accepted;
  sendCommandAck(command, accepted, result.disposition_code, seq, t_us);
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
      control_plane::Request request = {};
      request.request_id = hdr.seq;
      request.source = control_plane::SourceInterface::Radio;
      request.category = control_plane::RequestCategory::Fusion;
      request.action = control_plane::RequestAction::FusionSet;
      request.command_id = hdr.msg_type;
      request.fusion = cmd;
      control_plane::Result result = {};
      (void)control_plane::submit(request, result);
      sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      break;
    }
    case telem::CMD_GET_FUSION_SETTINGS:
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        return;
      }
      {
        control_plane::Request request = {};
        request.request_id = hdr.seq;
        request.source = control_plane::SourceInterface::Radio;
        request.category = control_plane::RequestCategory::Fusion;
        request.action = control_plane::RequestAction::FusionGet;
        request.command_id = hdr.msg_type;
        control_plane::Result result = {};
        (void)control_plane::submit(request, result);
        sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      }
      break;
    case telem::CMD_SET_STREAM_RATE: {
      if (hdr.payload_len != sizeof(telem::CmdSetStreamRateV1)) {
        g_stats.rx_bad_len++;
        return;
      }
      telem::CmdSetStreamRateV1 cmd = {};
      memcpy(&cmd, payload, sizeof(cmd));
      if (teensy_link::sendSetStreamRate(cmd)) {
        log_store::enqueueReplayControl(hdr.msg_type, hdr.seq, hdr.t_us, &cmd, sizeof(cmd),
                                        telem::kReplayControlFlagSourceGui | telem::kReplayControlFlagSourceRadio);
      }
      break;
    }
    case telem::CMD_SET_RADIO_MODE: {
      if (hdr.payload_len != sizeof(telem::CmdSetRadioModeV1)) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, 1U, hdr.seq, micros());
        return;
      }
      telem::CmdSetRadioModeV1 cmd = {};
      memcpy(&cmd, payload, sizeof(cmd));
      const bool new_state_only = cmd.state_only != 0U;
      const uint8_t new_control_rate_hz =
          constrain((uint8_t)(cmd.control_rate_hz == 0U ? kDefaultRadioControlRateHz : cmd.control_rate_hz), 1U, 10U);
      const uint16_t new_telem_rate_hz =
          new_state_only ? constrain((uint16_t)cmd.telem_rate_hz, 1U, 30U) : kUnifiedDownlinkRateHz;
      const bool new_radio_lr_mode = cmd.radio_lr_mode != 0U;
      const bool changed = new_state_only != g_state_only_mode || new_control_rate_hz != g_radio_control_rate_hz ||
                           new_telem_rate_hz != g_radio_telem_rate_hz || new_radio_lr_mode != g_radio_lr_mode;
      g_state_only_mode = new_state_only;
      g_radio_control_rate_hz = new_control_rate_hz;
      g_radio_telem_rate_hz = new_telem_rate_hz;
      g_radio_lr_mode = new_radio_lr_mode;
      applyRadioProtocol();
      if (changed) {
        g_last_hello_tx_ms = 0U;
        g_last_telem_tx_ms = 0U;
        g_last_gps_tx_ms = 0U;
        g_last_control_tx_ms = 0U;
        markLogStatusDirty();
      }
      sendCommandAck(hdr.msg_type, true, 0U, hdr.seq, micros());
      break;
    }
    case telem::CMD_LOG_START:
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, 1U, hdr.seq, micros());
        return;
      }
      {
        control_plane::Request request = {};
        request.request_id = hdr.seq;
        request.source = control_plane::SourceInterface::Radio;
        request.category = control_plane::RequestCategory::Recording;
        request.action = control_plane::RequestAction::RecordStart;
        request.command_id = hdr.msg_type;
        control_plane::Result result = {};
        (void)control_plane::submit(request, result);
        markLogStatusDirty();
        sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      }
      break;
    case telem::CMD_LOG_STOP:
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, 1U, hdr.seq, micros());
        return;
      }
      {
        control_plane::Request request = {};
        request.request_id = hdr.seq;
        request.source = control_plane::SourceInterface::Radio;
        request.category = control_plane::RequestCategory::Recording;
        request.action = control_plane::RequestAction::RecordStop;
        request.command_id = hdr.msg_type;
        control_plane::Result result = {};
        (void)control_plane::submit(request, result);
        markLogStatusDirty();
        sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      }
      break;
    case telem::CMD_GET_LOG_STATUS:
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, 1U, hdr.seq, micros());
        return;
      }
      {
        control_plane::Request request = {};
        request.request_id = hdr.seq;
        request.source = control_plane::SourceInterface::Radio;
        request.category = control_plane::RequestCategory::Recording;
        request.action = control_plane::RequestAction::RecordStatus;
        request.command_id = hdr.msg_type;
        control_plane::Result result = {};
        (void)control_plane::submit(request, result);
        sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      }
      break;
    case telem::CMD_RADIO_PING:
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, 1U, hdr.seq, micros());
        return;
      }
      sendCommandAck(hdr.msg_type, true, 0U, hdr.seq, micros());
      break;
    case telem::CMD_REPLAY_START:
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, 1U, hdr.seq, micros());
        return;
      }
      {
        control_plane::Request request = {};
        request.request_id = hdr.seq;
        request.source = control_plane::SourceInterface::Radio;
        request.category = control_plane::RequestCategory::Replay;
        request.action = control_plane::RequestAction::ReplayStartLatest;
        request.command_id = hdr.msg_type;
        control_plane::Result result = {};
        (void)control_plane::submit(request, result);
        sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      }
      sendReplayStatusFrame();
      break;
    case telem::CMD_REPLAY_STOP:
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, 1U, hdr.seq, micros());
        return;
      }
      {
        control_plane::Request request = {};
        request.request_id = hdr.seq;
        request.source = control_plane::SourceInterface::Radio;
        request.category = control_plane::RequestCategory::Replay;
        request.action = control_plane::RequestAction::ReplayStop;
        request.command_id = hdr.msg_type;
        control_plane::Result result = {};
        (void)control_plane::submit(request, result);
        sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      }
      sendReplayStatusFrame();
      break;
    case telem::CMD_GET_REPLAY_STATUS:
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, 1U, hdr.seq, micros());
        return;
      }
      {
        control_plane::Request request = {};
        request.request_id = hdr.seq;
        request.source = control_plane::SourceInterface::Radio;
        request.category = control_plane::RequestCategory::Replay;
        request.action = control_plane::RequestAction::ReplayStatus;
        request.command_id = hdr.msg_type;
        control_plane::Result result = {};
        (void)control_plane::submit(request, result);
        sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      }
      sendReplayStatusFrame();
      break;
    case telem::CMD_GET_LOG_FILE_LIST:
      if (hdr.payload_len != 0U && hdr.payload_len != sizeof(telem::CmdGetLogFileListV1)) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, (uint32_t)telem::SdApiStatusCode::INVALID_ARGUMENT, hdr.seq, micros());
        return;
      }
      {
        telem::CmdGetLogFileListV1 cmd = {};
        if (hdr.payload_len == sizeof(cmd)) {
          memcpy(&cmd, payload, sizeof(cmd));
        }
        const uint16_t offset = cmd.offset;
        const uint16_t limit = (cmd.limit == 0U) ? 32U : cmd.limit;
        control_plane::Request request = {};
        request.request_id = hdr.seq;
        request.source = control_plane::SourceInterface::Radio;
        request.category = control_plane::RequestCategory::FileSd;
        request.action = control_plane::RequestAction::FileListPage;
        request.command_id = hdr.msg_type;
        request.offset = offset;
        request.limit = limit;
        control_plane::Result result = {};
        (void)control_plane::submit(request, result);
        if (!result.ok &&
            result.code != (uint32_t)telem::SdApiStatusCode::NO_FILES) {
          sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
          return;
        }
        g_file_list_page = control_plane::lastFileListPage();
        sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
        g_file_list_transfer_active = true;
        sendCurrentFileListFrames();
        g_file_list_transfer_active = false;
      }
      break;
    case telem::CMD_GET_STORAGE_STATUS:
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, (uint32_t)telem::SdApiStatusCode::INVALID_ARGUMENT, hdr.seq, micros());
        return;
      }
      {
        control_plane::Request request = {};
        request.request_id = hdr.seq;
        request.source = control_plane::SourceInterface::Radio;
        request.category = control_plane::RequestCategory::FileSd;
        request.action = control_plane::RequestAction::StorageStatus;
        request.command_id = hdr.msg_type;
        control_plane::Result result = {};
        (void)control_plane::submit(request, result);
        sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      }
      sendStorageStatusFrame();
      break;
    case telem::CMD_MOUNT_MEDIA: {
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, (uint32_t)telem::SdApiStatusCode::INVALID_ARGUMENT, hdr.seq, micros());
        return;
      }
      control_plane::Request request = {};
      request.request_id = hdr.seq;
      request.source = control_plane::SourceInterface::Radio;
      request.category = control_plane::RequestCategory::FileSd;
      request.action = control_plane::RequestAction::MountMedia;
      request.command_id = hdr.msg_type;
      control_plane::Result result = {};
      (void)control_plane::submit(request, result);
      sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      sendStorageStatusFrame();
      if (result.ok) sendLogFileListFrames();
      break;
    }
    case telem::CMD_EJECT_MEDIA: {
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, (uint32_t)telem::SdApiStatusCode::INVALID_ARGUMENT, hdr.seq, micros());
        return;
      }
      control_plane::Request request = {};
      request.request_id = hdr.seq;
      request.source = control_plane::SourceInterface::Radio;
      request.category = control_plane::RequestCategory::FileSd;
      request.action = control_plane::RequestAction::EjectMedia;
      request.command_id = hdr.msg_type;
      control_plane::Result result = {};
      (void)control_plane::submit(request, result);
      sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      sendStorageStatusFrame();
      sendLogFileListFrames();
      break;
    }
    case telem::CMD_DELETE_LOG_FILE: {
      if (hdr.payload_len != sizeof(telem::CmdNamedFileV1)) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, (uint32_t)telem::SdApiStatusCode::INVALID_ARGUMENT, hdr.seq, micros());
        return;
      }
      telem::CmdNamedFileV1 cmd = {};
      memcpy(&cmd, payload, sizeof(cmd));
      control_plane::Request request = {};
      request.request_id = hdr.seq;
      request.source = control_plane::SourceInterface::Radio;
      request.category = control_plane::RequestCategory::FileSd;
      request.action = control_plane::RequestAction::DeleteFile;
      request.command_id = hdr.msg_type;
      strlcpy(request.name, cmd.name, sizeof(request.name));
      control_plane::Result result = {};
      (void)control_plane::submit(request, result);
      sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      sendStorageStatusFrame();
      sendLogFileListFrames();
      break;
    }
    case telem::CMD_RENAME_LOG_FILE: {
      if (hdr.payload_len != sizeof(telem::CmdRenameLogFileV1)) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, (uint32_t)telem::SdApiStatusCode::INVALID_ARGUMENT, hdr.seq, micros());
        return;
      }
      telem::CmdRenameLogFileV1 cmd = {};
      memcpy(&cmd, payload, sizeof(cmd));
      control_plane::Request request = {};
      request.request_id = hdr.seq;
      request.source = control_plane::SourceInterface::Radio;
      request.category = control_plane::RequestCategory::FileSd;
      request.action = control_plane::RequestAction::RenameFile;
      request.command_id = hdr.msg_type;
      strlcpy(request.name, cmd.src_name, sizeof(request.name));
      strlcpy(request.aux_name, cmd.dst_name, sizeof(request.aux_name));
      control_plane::Result result = {};
      (void)control_plane::submit(request, result);
      sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      sendStorageStatusFrame();
      sendLogFileListFrames();
      break;
    }
    case telem::CMD_EXPORT_LOG_CSV: {
      if (hdr.payload_len != sizeof(telem::CmdNamedFileV1)) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, (uint32_t)telem::SdApiStatusCode::INVALID_ARGUMENT, hdr.seq, micros());
        return;
      }
      telem::CmdNamedFileV1 cmd = {};
      memcpy(&cmd, payload, sizeof(cmd));
      control_plane::Request request = {};
      request.request_id = hdr.seq;
      request.source = control_plane::SourceInterface::Radio;
      request.category = control_plane::RequestCategory::FileSd;
      request.action = control_plane::RequestAction::ExportCsv;
      request.command_id = hdr.msg_type;
      strlcpy(request.name, cmd.name, sizeof(request.name));
      control_plane::Result result = {};
      (void)control_plane::submit(request, result);
      sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      sendStorageStatusFrame();
      sendLogFileListFrames();
      break;
    }
    case telem::CMD_SET_RECORD_PREFIX: {
      if (hdr.payload_len != sizeof(telem::CmdRecordPrefixV1)) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, 1U, hdr.seq, micros());
        return;
      }
      telem::CmdRecordPrefixV1 cmd = {};
      memcpy(&cmd, payload, sizeof(cmd));
      control_plane::Request request = {};
      request.request_id = hdr.seq;
      request.source = control_plane::SourceInterface::Radio;
      request.category = control_plane::RequestCategory::FileSd;
      request.action = control_plane::RequestAction::SetRecordPrefix;
      request.command_id = hdr.msg_type;
      strlcpy(request.prefix, cmd.prefix, sizeof(request.prefix));
      control_plane::Result result = {};
      (void)control_plane::submit(request, result);
      sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      sendStorageStatusFrame();
      break;
    }
    case telem::CMD_REPLAY_START_FILE: {
      if (hdr.payload_len != sizeof(telem::CmdNamedFileV1)) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, 1U, hdr.seq, micros());
        return;
      }
      telem::CmdNamedFileV1 cmd = {};
      memcpy(&cmd, payload, sizeof(cmd));
      control_plane::Request request = {};
      request.request_id = hdr.seq;
      request.source = control_plane::SourceInterface::Radio;
      request.category = control_plane::RequestCategory::Replay;
      request.action = control_plane::RequestAction::ReplayStartFile;
      request.command_id = hdr.msg_type;
      strlcpy(request.name, cmd.name, sizeof(request.name));
      control_plane::Result result = {};
      (void)control_plane::submit(request, result);
      sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      sendReplayStatusFrame();
      break;
    }
    case telem::CMD_REPLAY_PAUSE:
      if (hdr.payload_len != 0U) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, 1U, hdr.seq, micros());
        return;
      }
      {
        control_plane::Request request = {};
        request.request_id = hdr.seq;
        request.source = control_plane::SourceInterface::Radio;
        request.category = control_plane::RequestCategory::Replay;
        request.action = control_plane::RequestAction::ReplayPause;
        request.command_id = hdr.msg_type;
        control_plane::Result result = {};
        (void)control_plane::submit(request, result);
        sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      }
      sendReplayStatusFrame();
      break;
    case telem::CMD_REPLAY_SEEK_REL: {
      if (hdr.payload_len != sizeof(telem::CmdReplaySeekRelV1)) {
        g_stats.rx_bad_len++;
        sendCommandAck(hdr.msg_type, false, 1U, hdr.seq, micros());
        return;
      }
      telem::CmdReplaySeekRelV1 cmd = {};
      memcpy(&cmd, payload, sizeof(cmd));
      control_plane::Request request = {};
      request.request_id = hdr.seq;
      request.source = control_plane::SourceInterface::Radio;
      request.category = control_plane::RequestCategory::Replay;
      request.action = control_plane::RequestAction::ReplaySeekRelative;
      request.command_id = hdr.msg_type;
      request.delta_records = cmd.delta_records;
      control_plane::Result result = {};
      (void)control_plane::submit(request, result);
      sendControlPlaneDispositionAck(hdr.msg_type, result, hdr.seq, micros());
      sendReplayStatusFrame();
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
  } else if (!stateOnlyModeEnabled() &&
             (g_stats.last_rx_ms == 0U || (uint32_t)(now - g_stats.last_rx_ms) >= kPeerStaleMs)) {
    sent = sendHelloTo(g_gnd_mac);
  }
  if (sent) g_last_hello_tx_ms = now;
}

}  // namespace

void begin(const AppConfig& cfg) {
  memset(&g_stats, 0, sizeof(g_stats));
  clearPeerState();
  g_espnow_ready = false;
  g_network_reset_requested = false;
  g_state_only_mode = cfg.radio_state_only != 0U;
  g_radio_lr_mode = cfg.radio_lr_mode != 0U;
  g_radio_control_rate_hz = kDefaultRadioControlRateHz;
  g_radio_telem_rate_hz = g_state_only_mode ? constrain((uint16_t)cfg.log_rate_hz, 1U, 30U) : kUnifiedDownlinkRateHz;
  g_control_has_ack = false;
  g_control_ack_command = 0U;
  g_control_ack_code = 0U;
  g_control_ack_ok = false;
  g_last_state_seq = 0U;
  g_last_state_log_seq = 0U;
  g_last_state_log_ms = 0U;
  g_tx_in_flight_seq = 0U;
  g_tx_in_flight_msg_type = 0U;
  g_last_ack_seq = 0U;
  g_tx_seq = 0U;
  g_session_id = esp_random();
  (void)initEspNow();
}

void reconfigure(const AppConfig& cfg) {
  g_state_only_mode = cfg.radio_state_only != 0U;
  g_radio_lr_mode = cfg.radio_lr_mode != 0U;
  g_radio_control_rate_hz = kDefaultRadioControlRateHz;
  g_radio_telem_rate_hz = g_state_only_mode ? constrain((uint16_t)cfg.log_rate_hz, 1U, 30U) : kUnifiedDownlinkRateHz;
  g_last_state_log_seq = 0U;
  g_last_state_log_ms = 0U;
  g_tx_in_flight_seq = 0U;
  g_tx_in_flight_msg_type = 0U;
  applyRadioProtocol();
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
  pumpTx();
}

void publish(const teensy_link::Snapshot& snap) {
  noteSourceSnapshot(snap.seq, snap.t_us, snap.stats.last_rx_ms);
  captureSnapshotAck(snap);
  if (g_file_list_transfer_active) {
    return;
  }
  if (replay_bridge::takeStatusDirty()) {
    sendReplayStatusFrame();
  }
  if (stateOnlyModeEnabled()) {
    (void)maybeSendStateOnlyDownlink(snap);
    return;
  }
  (void)maybeSendUnifiedDownlink(snap);
}

bool publishState(const telem::TelemetryStateRecord& state, uint32_t seq, uint32_t t_us) {
  const uint32_t now = millis();
  g_stats.publish_attempts++;
  g_stats.last_publish_attempt_ms = now;
  if (!isNewerSeq(seq, g_last_state_seq)) {
    g_stats.publish_skip_not_new++;
    return true;
  }
  if (!g_has_gnd_mac) {
    g_stats.publish_skip_no_peer++;
    return false;
  }
  const bool ok = sendFrame(telem::TELEM_FULL_STATE, &state, sizeof(state), seq, t_us);
  if (ok) {
    g_last_state_seq = seq;
    g_stats.tx_state_packets++;
    g_stats.publish_ok++;
    g_stats.last_source_seq = seq;
    g_stats.last_source_t_us = t_us;
    g_stats.last_tx_ms = now;
  }
  return ok;
}

bool publishStressState(const telem::TelemetryStateRecord& state, uint32_t seq, uint32_t t_us) {
  const bool ok = g_has_gnd_mac && sendFrame(telem::TELEM_FULL_STATE, &state, sizeof(state), seq, t_us);
  if (ok) {
    g_stats.tx_state_packets++;
    g_stats.publish_ok++;
    g_stats.last_source_seq = seq;
    g_stats.last_source_t_us = t_us;
    g_stats.last_tx_ms = millis();
  }
  return ok;
}

Stats stats() { return g_stats; }

void noteSourceSnapshot(uint32_t seq, uint32_t t_us, uint32_t last_rx_ms) {
  g_stats.source_snapshots_seen++;
  g_stats.latest_source_seq_seen = seq;
  g_stats.latest_source_t_us_seen = t_us;
  g_stats.latest_source_rx_ms_seen = last_rx_ms;
  g_stats.latest_source_seen_ms = millis();
  g_stats.last_publish_age_ms = last_rx_ms ? (uint32_t)(g_stats.latest_source_seen_ms - last_rx_ms) : 0U;
}

size_t txQueueFree() {
  size_t free_slots = 0U;
  portENTER_CRITICAL(&g_rx_mux);
  free_slots = txQueueFreeLocked();
  portEXIT_CRITICAL(&g_rx_mux);
  return free_slots;
}

bool stateOnlyMode() { return g_state_only_mode; }

bool longRangeMode() { return g_radio_lr_mode; }

bool hasPeer() { return g_has_gnd_mac; }

String peerMac() { return macToString(g_gnd_mac); }

bool radioReady() { return g_espnow_ready; }

void setVerbose(bool enabled) { g_verbose = enabled; }

bool verbose() { return g_verbose; }

void setRecorderEnabled(bool enabled) {
  log_store::setEnabled(enabled);
  if (!enabled) {
    g_log_requested = false;
  }
  control_plane::noteRecorderFeatureEnabled(enabled, millis());
  g_log_last_change_ms = millis();
  markLogStatusDirty();
}

bool takeNetworkResetRequest() {
  const bool requested = g_network_reset_requested;
  g_network_reset_requested = false;
  return requested;
}

void resetNetworkState() {
  shutdownEspNow();
  clearPeerState();
  g_stats.tx_state_packets = 0U;
  g_stats.tx_unified_packets = 0U;
  g_stats.source_snapshots_seen = 0U;
  g_stats.latest_source_seq_seen = 0U;
  g_stats.latest_source_t_us_seen = 0U;
  g_stats.latest_source_rx_ms_seen = 0U;
  g_stats.latest_source_seen_ms = 0U;
  g_stats.publish_attempts = 0U;
  g_stats.publish_ok = 0U;
  g_stats.publish_skip_no_state = 0U;
  g_stats.publish_skip_no_peer = 0U;
  g_stats.publish_skip_rate = 0U;
  g_stats.publish_skip_not_new = 0U;
  g_stats.last_source_seq = 0U;
  g_stats.last_source_t_us = 0U;
  g_stats.last_tx_ms = 0U;
  g_stats.last_publish_attempt_ms = 0U;
  g_stats.last_publish_age_ms = 0U;
  g_last_state_seq = 0U;
  g_last_ack_seq = 0U;
  g_control_has_ack = false;
  g_control_ack_command = 0U;
  g_control_ack_code = 0U;
  g_control_ack_ok = false;
  g_rssi_valid = false;
  g_last_rssi_sample_ms = 0U;
  g_stats.last_rx_ms = 0U;
}

}  // namespace radio_link
