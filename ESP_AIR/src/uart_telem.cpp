#include "uart_telem.h"

#include <string.h>

#include "config_store.h"
#include "log_store.h"
#include "sd_capture_test.h"
#include "spi_bridge.h"

namespace uart_telem {
namespace {

uint32_t g_tx_seq = 1U;
uint32_t g_local_state_seq = 0U;
uint32_t g_local_ack_seq = 0U;

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
telem::TelemetryFullStateV1 g_state = {};
bool g_has_state = false;
uint32_t g_seq = 0U;
uint32_t g_t_us = 0U;
telem::FusionSettingsV1 g_fusion_settings = {};
bool g_has_fusion_settings = false;
uint32_t g_fusion_rx_seq = 0U;
RxStats g_stats = {};
bool g_has_ack = false;
uint32_t g_ack_rx_seq = 0U;
uint16_t g_ack_command = 0U;
bool g_ack_ok = false;
uint32_t g_ack_code = 0U;
constexpr uint16_t kPendingStateDepth = 256U;
PendingState g_pending_states[kPendingStateDepth] = {};
uint16_t g_pending_head = 0U;
uint16_t g_pending_tail = 0U;

void setAckLocked(uint16_t command, bool ok, uint32_t code) {
  g_has_ack = true;
  g_ack_rx_seq = ++g_local_ack_seq;
  g_ack_command = command;
  g_ack_ok = ok;
  g_ack_code = code;
}

void updateFusionFromStateLocked(const telem::TelemetryFullStateV1& state) {
  g_fusion_settings.gain = state.fusion_gain;
  g_fusion_settings.accelerationRejection = state.fusion_accel_rej;
  g_fusion_settings.magneticRejection = state.fusion_mag_rej;
  g_fusion_settings.recoveryTriggerPeriod = state.fusion_recovery_period;
  g_fusion_settings.reserved = 0U;
  g_has_fusion_settings = true;
  g_fusion_rx_seq = g_seq;
}

void queuePendingStateLocked(const telem::TelemetryFullStateV1& state, uint32_t seq, uint32_t t_us) {
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

  telem::ReplayControlRecord160 record = {};
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

}  // namespace

void begin(const AppConfig& cfg) {
  (void)cfg;
  g_tx_seq = 1U;
  g_local_state_seq = 0U;
  g_local_ack_seq = 0U;
  portENTER_CRITICAL(&g_mux);
  g_state = {};
  g_has_state = false;
  g_seq = 0U;
  g_t_us = 0U;
  g_fusion_settings = {};
  g_has_fusion_settings = false;
  g_fusion_rx_seq = 0U;
  g_stats = {};
  g_has_ack = false;
  g_ack_rx_seq = 0U;
  g_ack_command = 0U;
  g_ack_ok = false;
  g_ack_code = 0U;
  clearPendingStateQueueLocked();
  portEXIT_CRITICAL(&g_mux);
  spi_bridge::begin();
}

void reconfigure(const AppConfig& cfg) {
  begin(cfg);
}

void poll() {
  spi_bridge::poll();
  uint8_t record[sizeof(telem::TelemetryFullStateV1)] = {};
  while (spi_bridge::popStateRecord(record, sizeof(record))) {
    telem::TelemetryFullStateV1 tmp = {};
    memcpy(&tmp, record, sizeof(tmp));
    const uint32_t seq = ++g_local_state_seq;
    const uint32_t t_us = micros();

    portENTER_CRITICAL(&g_mux);
    g_state = tmp;
    g_has_state = true;
    g_seq = seq;
    g_t_us = t_us;
    g_stats.rx_bytes += sizeof(tmp);
    g_stats.frames_ok++;
    g_stats.last_rx_ms = millis();
    updateFusionFromStateLocked(tmp);
    queuePendingStateLocked(tmp, seq, t_us);
    portEXIT_CRITICAL(&g_mux);

    log_store::enqueueState(seq, t_us, tmp);
    sd_capture_test::enqueueState(seq, t_us, tmp);
  }
}

void resync(bool drain_input) {
  (void)drain_input;
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
  portENTER_CRITICAL(&g_mux);
  const bool ok = g_has_state;
  if (ok) {
    updateFusionFromStateLocked(g_state);
    setAckLocked(telem::CMD_GET_FUSION_SETTINGS, true, 0U);
  } else {
    setAckLocked(telem::CMD_GET_FUSION_SETTINGS, false, 1U);
  }
  portEXIT_CRITICAL(&g_mux);
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

bool sendReplayInputRecord(const telem::ReplayInputRecord160& record) {
  return spi_bridge::queueReplayRecord(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
}

bool sendReplayControlRecord(const telem::ReplayControlRecord160& record) {
  return spi_bridge::queueReplayRecord(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
}

bool probeRxPin(uint8_t rx_pin, uint32_t baud, uint32_t dwell_ms, uint32_t& out_bytes) {
  (void)rx_pin;
  (void)baud;
  (void)dwell_ms;
  out_bytes = 0U;
  return false;
}

}  // namespace uart_telem