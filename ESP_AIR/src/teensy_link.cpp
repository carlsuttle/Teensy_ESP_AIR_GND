#include "teensy_link.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "config_store.h"
#include "log_store.h"
#include "replay_bridge.h"
#include "spi_bridge.h"

namespace teensy_link {
namespace {

uint32_t g_tx_seq = 1U;
uint32_t g_local_state_seq = 0U;
uint32_t g_local_ack_seq = 0U;

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
telem::TelemetryStateRecord g_state = {};
bool g_has_state = false;
uint32_t g_seq = 0U;
uint32_t g_t_us = 0U;
telem::FusionSettingsV1 g_fusion_settings = {};
bool g_has_fusion_settings = false;
uint32_t g_fusion_rx_seq = 0U;
bool g_fusion_request_pending = false;
uint32_t g_fusion_request_after_seq = 0U;
RxStats g_stats = {};
bool g_has_ack = false;
uint32_t g_ack_rx_seq = 0U;
uint16_t g_ack_command = 0U;
bool g_ack_ok = false;
uint32_t g_ack_code = 0U;
TaskHandle_t g_service_task = nullptr;
constexpr uint32_t kServicePeriodMs = 1U;
constexpr uint32_t kServiceTaskStack = 4096U;
constexpr UBaseType_t kServiceTaskPriority = 2U;
constexpr uint16_t kPendingStateDepth = 128U;
PendingState g_pending_states[kPendingStateDepth] = {};
uint16_t g_pending_head = 0U;
uint16_t g_pending_tail = 0U;
uint32_t g_state_diag_valid_count = 0U;
uint32_t g_state_diag_last_rx_ms = 0U;
uint32_t g_state_diag_prev_rx_ms = 0U;
uint32_t g_state_diag_last_dt_ms = 0U;
uint32_t g_state_diag_last_seq = 0U;
uint32_t g_state_diag_last_t_us = 0U;
uint32_t g_state_diag_last_batch_first_seq = 0U;
uint32_t g_state_diag_last_batch_last_seq = 0U;
uint16_t g_state_diag_last_batch_records = 0U;
uint16_t g_state_diag_last_batch_payload_len = 0U;
bool g_state_diag_last_replay_output = false;
bool g_state_diag_stall_warned = false;
bool g_state_diag_stale_warned = false;
uint32_t g_state_diag_last_summary_ms = 0U;
uint32_t g_state_diag_prev_summary_valid = 0U;
uint32_t g_state_diag_prev_summary_txn = 0U;

void setAckLocked(uint16_t command, bool ok, uint32_t code) {
  g_has_ack = true;
  g_ack_rx_seq = ++g_local_ack_seq;
  g_ack_command = command;
  g_ack_ok = ok;
  g_ack_code = code;
}

void updateFusionFromStateLocked(const telem::TelemetryStateRecord& state) {
  g_fusion_settings.gain = state.fusion_gain;
  g_fusion_settings.accelerationRejection = state.fusion_accel_rej;
  g_fusion_settings.magneticRejection = state.fusion_mag_rej;
  g_fusion_settings.recoveryTriggerPeriod = state.fusion_recovery_period;
  g_fusion_settings.reserved = 0U;
  g_has_fusion_settings = true;
  g_fusion_rx_seq = g_seq;
}

void queuePendingStateLocked(const telem::TelemetryStateRecord& state, uint32_t seq, uint32_t t_us) {
  const uint16_t next_head = (uint16_t)((g_pending_head + 1U) % kPendingStateDepth);
  if (next_head != g_pending_tail) {
    g_pending_states[g_pending_head].state = state;
    g_pending_states[g_pending_head].seq = seq;
    g_pending_states[g_pending_head].t_us = t_us;
    g_pending_head = next_head;
  } else {
    g_stats.drop++;
  }
}

bool queueReplayControl(uint16_t command_id, const void* payload, uint16_t payload_len) {
  if (payload_len > sizeof(telem::ReplayControlPayloadV1::payload)) return false;

  telem::ReplayControlRecord record = {};
  record.hdr.magic = telem::kReplayMagic;
  record.hdr.version = telem::kReplayVersion;
  record.hdr.kind = (uint8_t)telem::ReplayRecordKind::Control;
  record.hdr.flags = 0U;
  record.hdr.seq = g_tx_seq++;
  record.hdr.t_us = micros();
  record.payload.command_id = command_id;
  record.payload.payload_len = payload_len;
  record.payload.command_seq = record.hdr.seq;
  record.payload.received_t_us = record.hdr.t_us;
  record.payload.apply_flags = telem::kReplayControlFlagAccepted;
  if (payload_len != 0U && payload) {
    memcpy(record.payload.payload, payload, payload_len);
  }
  return spi_bridge::queueReplayRecord(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
}

void clearPendingStateQueueLocked() {
  g_pending_head = 0U;
  g_pending_tail = 0U;
}

void servicePoll() {
  const uint32_t poll_started_ms = millis();
  uint32_t state_records_drained = 0U;
  uint32_t raw_records_drained = 0U;
  uint32_t batch_first_seq = 0U;
  uint32_t batch_last_seq = 0U;
  uint16_t batch_records = 0U;
  uint16_t batch_payload_len = 0U;
  bool batch_last_replay_output = false;

  spi_bridge::poll();
  const bool standalone_bench = config_store::get().standalone_bench != 0U;
  const auto spi_before = spi_bridge::stats();
  batch_records = spi_before.last_state_record_count;
  batch_payload_len = spi_before.last_state_payload_len;
  uint8_t record[sizeof(telem::TelemetryStateRecord)] = {};
  while (spi_bridge::popStateRecord(record, sizeof(record))) {
    telem::TelemetryStateRecord tmp = {};
    memcpy(&tmp, record, sizeof(tmp));
    const bool replay_output = (tmp.flags & telem::kStateFlagReplayOutput) != 0U;
    uint32_t seq = ++g_local_state_seq;
    uint32_t t_us = micros();
    bool have_replay_stamp = false;
    if (replay_output) {
      telem::decodeReplaySourceStamp(tmp, seq, t_us);
      have_replay_stamp = (seq != 0U) || (t_us != 0U);
    }

    if (state_records_drained == 0U) {
      batch_first_seq = seq;
    }
    batch_last_seq = seq;
    batch_last_replay_output = replay_output;
    g_state_diag_valid_count++;
    g_state_diag_prev_rx_ms = g_state_diag_last_rx_ms;
    g_state_diag_last_rx_ms = poll_started_ms;
    g_state_diag_last_dt_ms =
        g_state_diag_prev_rx_ms ? (uint32_t)(poll_started_ms - g_state_diag_prev_rx_ms) : 0U;
    g_state_diag_last_seq = seq;
    g_state_diag_last_t_us = t_us;
    g_state_diag_last_replay_output = replay_output;
    g_state_diag_stall_warned = false;
    g_state_diag_stale_warned = false;

    portENTER_CRITICAL(&g_mux);
    g_state = tmp;
    g_has_state = true;
    g_seq = seq;
    g_t_us = t_us;
    g_stats.rx_bytes += sizeof(tmp);
    g_stats.frames_ok++;
    g_stats.last_rx_ms = millis();
    updateFusionFromStateLocked(tmp);
    if (g_fusion_request_pending && g_fusion_rx_seq > g_fusion_request_after_seq) {
      setAckLocked(telem::CMD_GET_FUSION_SETTINGS, true, 0U);
      g_fusion_request_pending = false;
    }
    queuePendingStateLocked(tmp, seq, t_us);
    portEXIT_CRITICAL(&g_mux);

    if (!standalone_bench && (!replay_output || have_replay_stamp)) {
      log_store::enqueueState(seq, t_us, tmp);
    }
    state_records_drained++;
  }

  uint8_t raw_record[sizeof(telem::ReplayInputRecord)] = {};
  while (spi_bridge::popRawRecord(raw_record, sizeof(raw_record))) {
    telem::ReplayInputRecord replay = {};
    memcpy(&replay, raw_record, sizeof(replay));
    log_store::enqueueReplayInput(replay.hdr.seq, replay.hdr.t_us, replay);
    raw_records_drained++;
  }

  portENTER_CRITICAL(&g_mux);
  const uint32_t last_poll_ms = g_stats.last_poll_ms;
  if (last_poll_ms != 0U) {
    const uint32_t gap_ms = poll_started_ms - last_poll_ms;
    if (gap_ms > g_stats.max_poll_gap_ms) g_stats.max_poll_gap_ms = gap_ms;
  }
  g_stats.last_poll_ms = poll_started_ms;
  g_stats.poll_runs++;
  g_stats.state_records_drained += state_records_drained;
  g_stats.raw_records_drained += raw_records_drained;
  portEXIT_CRITICAL(&g_mux);

  if (state_records_drained != 0U) {
    g_state_diag_last_batch_first_seq = batch_first_seq;
    g_state_diag_last_batch_last_seq = batch_last_seq;
    g_state_diag_last_batch_records = batch_records ? batch_records : (uint16_t)state_records_drained;
    g_state_diag_last_batch_payload_len =
        batch_payload_len ? batch_payload_len : (uint16_t)(state_records_drained * sizeof(telem::TelemetryStateRecord));
    g_state_diag_last_replay_output = batch_last_replay_output;
  }

  const uint32_t now = millis();
  if (g_state_diag_last_rx_ms != 0U) {
    const uint32_t age_ms = (uint32_t)(now - g_state_diag_last_rx_ms);
    if (!g_state_diag_stall_warned && age_ms > 100U) {
      Serial.printf("AIRRXSTALL seq=%lu age_ms=%lu valid=%lu batch_records=%u batch_len=%u replay=%u\n",
                    (unsigned long)g_state_diag_last_seq,
                    (unsigned long)age_ms,
                    (unsigned long)g_state_diag_valid_count,
                    (unsigned)g_state_diag_last_batch_records,
                    (unsigned)g_state_diag_last_batch_payload_len,
                    g_state_diag_last_replay_output ? 1U : 0U);
      g_state_diag_stall_warned = true;
    }
    if (!g_state_diag_stale_warned && age_ms > 300U) {
      Serial.printf("AIRRXSTALE seq=%lu age_ms=%lu valid=%lu batch_records=%u batch_len=%u replay=%u\n",
                    (unsigned long)g_state_diag_last_seq,
                    (unsigned long)age_ms,
                    (unsigned long)g_state_diag_valid_count,
                    (unsigned)g_state_diag_last_batch_records,
                    (unsigned)g_state_diag_last_batch_payload_len,
                    g_state_diag_last_replay_output ? 1U : 0U);
      g_state_diag_stale_warned = true;
    }
  }

  if (g_state_diag_last_summary_ms == 0U || (uint32_t)(now - g_state_diag_last_summary_ms) >= 1000U) {
    const auto spi_now = spi_bridge::stats();
    const uint32_t age_ms = g_state_diag_last_rx_ms ? (uint32_t)(now - g_state_diag_last_rx_ms) : 0U;
    const uint32_t recps = g_state_diag_valid_count - g_state_diag_prev_summary_valid;
    const uint32_t txps = spi_now.state_transactions_received - g_state_diag_prev_summary_txn;
    // Serial.printf(
        // (unsigned long)g_state_diag_last_seq,
        // (unsigned long)g_state_diag_last_t_us,
        // (unsigned long)g_state_diag_last_dt_ms,
        // (unsigned long)age_ms,
        // (unsigned long)g_state_diag_valid_count,
        // (unsigned)g_state_diag_last_batch_records,
        // (unsigned)g_state_diag_last_batch_payload_len,
        // (unsigned long)g_state_diag_last_batch_first_seq,
        // (unsigned long)g_state_diag_last_batch_last_seq,
        // g_state_diag_last_replay_output ? 1U : 0U,
        // (unsigned long)recps,
        // (unsigned long)txps,
        // (unsigned long)spi_now.state_records_received,
        // (unsigned long)spi_now.state_transactions_received);
    g_state_diag_last_summary_ms = now;
    g_state_diag_prev_summary_valid = g_state_diag_valid_count;
    g_state_diag_prev_summary_txn = spi_now.state_transactions_received;
  }
}

void serviceTask(void* param) {
  (void)param;
  for (;;) {
    servicePoll();
    vTaskDelay(pdMS_TO_TICKS(kServicePeriodMs));
  }
}

}  // namespace

void begin(const AppConfig& cfg) {
  (void)cfg;
  g_tx_seq = 1U;
  g_local_state_seq = 0U;
  g_local_ack_seq = 0U;
  g_state_diag_valid_count = 0U;
  g_state_diag_last_rx_ms = 0U;
  g_state_diag_prev_rx_ms = 0U;
  g_state_diag_last_dt_ms = 0U;
  g_state_diag_last_seq = 0U;
  g_state_diag_last_t_us = 0U;
  g_state_diag_last_batch_first_seq = 0U;
  g_state_diag_last_batch_last_seq = 0U;
  g_state_diag_last_batch_records = 0U;
  g_state_diag_last_batch_payload_len = 0U;
  g_state_diag_last_replay_output = false;
  g_state_diag_stall_warned = false;
  g_state_diag_stale_warned = false;
  g_state_diag_last_summary_ms = 0U;
  g_state_diag_prev_summary_valid = 0U;
  g_state_diag_prev_summary_txn = 0U;
  portENTER_CRITICAL(&g_mux);
  g_state = {};
  g_has_state = false;
  g_seq = 0U;
  g_t_us = 0U;
  g_fusion_settings = {};
  g_has_fusion_settings = false;
  g_fusion_rx_seq = 0U;
  g_fusion_request_pending = false;
  g_fusion_request_after_seq = 0U;
  g_stats = {};
  g_has_ack = false;
  g_ack_rx_seq = 0U;
  g_ack_command = 0U;
  g_ack_ok = false;
  g_ack_code = 0U;
  clearPendingStateQueueLocked();
  portEXIT_CRITICAL(&g_mux);
  spi_bridge::begin();
  if (!g_service_task) {
    xTaskCreatePinnedToCore(serviceTask, "teensy_link", kServiceTaskStack, nullptr,
                            kServiceTaskPriority, &g_service_task, 1);
  }
}

void reconfigure(const AppConfig& cfg) {
  begin(cfg);
}

void poll() {
  servicePoll();
}

void resync(bool drain_input) {
  (void)drain_input;
  g_state_diag_valid_count = 0U;
  g_state_diag_last_rx_ms = 0U;
  g_state_diag_prev_rx_ms = 0U;
  g_state_diag_last_dt_ms = 0U;
  g_state_diag_last_seq = 0U;
  g_state_diag_last_t_us = 0U;
  g_state_diag_last_batch_first_seq = 0U;
  g_state_diag_last_batch_last_seq = 0U;
  g_state_diag_last_batch_records = 0U;
  g_state_diag_last_batch_payload_len = 0U;
  g_state_diag_last_replay_output = false;
  g_state_diag_stall_warned = false;
  g_state_diag_stale_warned = false;
  g_state_diag_last_summary_ms = 0U;
  g_state_diag_prev_summary_valid = 0U;
  g_state_diag_prev_summary_txn = 0U;
  portENTER_CRITICAL(&g_mux);
  clearPendingStateQueueLocked();
  portEXIT_CRITICAL(&g_mux);
}

Snapshot snapshot() {
  Snapshot s = {};
  portENTER_CRITICAL(&g_mux);
  s.has_state = g_has_state;
  s.state = g_state;
  s.seq = g_seq;
  s.t_us = g_t_us;
  s.has_fusion_settings = g_has_fusion_settings;
  s.fusion_settings = g_fusion_settings;
  s.fusion_rx_seq = g_fusion_rx_seq;
  s.stats = g_stats;
  s.has_ack = g_has_ack;
  s.ack_rx_seq = g_ack_rx_seq;
  s.ack_command = g_ack_command;
  s.ack_ok = g_ack_ok;
  s.ack_code = g_ack_code;
  portEXIT_CRITICAL(&g_mux);
  return s;
}

bool popPendingState(PendingState& out) {
  bool ok = false;
  portENTER_CRITICAL(&g_mux);
  if (g_pending_tail != g_pending_head) {
    out = g_pending_states[g_pending_tail];
    g_pending_tail = (uint16_t)((g_pending_tail + 1U) % kPendingStateDepth);
    ok = true;
  }
  portEXIT_CRITICAL(&g_mux);
  return ok;
}

void clearPendingStates() {
  portENTER_CRITICAL(&g_mux);
  clearPendingStateQueueLocked();
  portEXIT_CRITICAL(&g_mux);
}

LoopbackResult runLoopbackTest(uint32_t timeout_ms) {
  (void)timeout_ms;
  LoopbackResult r = {};
  return r;
}

bool sendSetFusionSettings(const telem::CmdSetFusionSettingsV1& cmd) {
  const bool ok = queueReplayControl(telem::CMD_SET_FUSION_SETTINGS, &cmd, sizeof(cmd));
  if (ok) {
    portENTER_CRITICAL(&g_mux);
    g_fusion_settings.gain = cmd.gain;
    g_fusion_settings.accelerationRejection = cmd.accelerationRejection;
    g_fusion_settings.magneticRejection = cmd.magneticRejection;
    g_fusion_settings.recoveryTriggerPeriod = cmd.recoveryTriggerPeriod;
    g_fusion_settings.reserved = 0U;
    g_has_fusion_settings = true;
    g_fusion_rx_seq = ++g_local_ack_seq;
    setAckLocked(telem::CMD_SET_FUSION_SETTINGS, true, 0U);
    portEXIT_CRITICAL(&g_mux);
  }
  return ok;
}

bool sendGetFusionSettings() {
  const bool ok = queueReplayControl(telem::CMD_GET_FUSION_SETTINGS, nullptr, 0U);
  if (!ok) return false;

  portENTER_CRITICAL(&g_mux);
  g_fusion_request_pending = true;
  g_fusion_request_after_seq = g_fusion_rx_seq;
  portEXIT_CRITICAL(&g_mux);
  return true;
}

bool sendSetCaptureSettings(const telem::CmdSetCaptureSettingsV1& cmd) {
  const bool ok = queueReplayControl(telem::CMD_SET_CAPTURE_SETTINGS, &cmd, sizeof(cmd));
  if (ok) {
    portENTER_CRITICAL(&g_mux);
    setAckLocked(telem::CMD_SET_CAPTURE_SETTINGS, true, 0U);
    portEXIT_CRITICAL(&g_mux);
  }
  return ok;
}

bool sendSaveCaptureSettings() {
  const bool ok = queueReplayControl(telem::CMD_SAVE_CAPTURE_SETTINGS, nullptr, 0U);
  if (ok) {
    portENTER_CRITICAL(&g_mux);
    setAckLocked(telem::CMD_SAVE_CAPTURE_SETTINGS, true, 0U);
    portEXIT_CRITICAL(&g_mux);
  }
  return ok;
}

bool sendSetStreamRate(const telem::CmdSetStreamRateV1& cmd) {
  const bool ok = queueReplayControl(telem::CMD_SET_STREAM_RATE, &cmd, sizeof(cmd));
  if (ok) {
    portENTER_CRITICAL(&g_mux);
    setAckLocked(telem::CMD_SET_STREAM_RATE, true, 0U);
    portEXIT_CRITICAL(&g_mux);
  }
  return ok;
}

bool sendReplayInputRecord(const telem::ReplayInputRecord& record) {
  return spi_bridge::queueReplayRecord(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
}

bool sendReplayControlRecord(const telem::ReplayControlRecord& record) {
  return spi_bridge::queueReplayRecord(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
}

bool probeRxPin(uint8_t rx_pin, uint32_t baud, uint32_t dwell_ms, uint32_t& out_bytes) {
  (void)rx_pin;
  (void)baud;
  (void)dwell_ms;
  out_bytes = 0U;
  return false;
}

}  // namespace teensy_link
