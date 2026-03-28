#include "teensy_api.h"

#include <string.h>

namespace {

bool floatNearAbs(float a, float b, float tol) {
  return fabsf(a - b) <= tol;
}

uint32_t carrySignatureValue(uint32_t seq) {
  uint32_t x = seq ^ 0xA5A55A5AU;
  x *= 2654435761UL;
  x ^= (x >> 16);
  return x & 0x1FFFFFFFUL;
}

void fillCarrySignatureRecord(uint32_t index, telem::ReplayInputRecord160& replay) {
  memset(&replay, 0, sizeof(replay));
  replay.hdr.magic = telem::kReplayMagic;
  replay.hdr.version = telem::kReplayVersion;
  replay.hdr.kind = (uint8_t)telem::ReplayRecordKind::Input;
  replay.hdr.seq = 0xA5000000UL + index;
  replay.hdr.t_us = 0x51000000UL + index * 1003U;

  auto& p = replay.payload;
  p.present_mask = telem::kSensorPresentImu | telem::kSensorPresentMag |
                   telem::kSensorPresentGps | telem::kSensorPresentBaro;
  p.source_flags = 0x12340000UL | index;
  p.imu_seq = 0x20000000UL + index;
  p.gps_seq = 0x30000000UL + index;
  p.baro_seq = 0x40000000UL + index;
  p.accel_milli_mps2[0] = 1111 + (int32_t)index;
  p.accel_milli_mps2[1] = -2222 - (int32_t)index;
  p.accel_milli_mps2[2] = 3333 + (int32_t)(index * 2U);
  p.gyro_milli_dps[0] = 4444 + (int32_t)index;
  p.gyro_milli_dps[1] = -5555 - (int32_t)(index * 2U);
  p.gyro_milli_dps[2] = 6666 + (int32_t)(index * 3U);
  p.mag_milli_uT[0] = 7777 + (int32_t)index;
  p.mag_milli_uT[1] = -8888 + (int32_t)index;
  p.mag_milli_uT[2] = 9999 - (int32_t)index;
  p.iTOW_ms = 500000U + 17U * index;
  p.fixType = 3U;
  p.numSV = (uint8_t)(10U + (index % 5U));
  p.gps_flags = 0xB0U + (uint16_t)index;
  p.lat_1e7 = 372281500 + (int32_t)(11U * index);
  p.lon_1e7 = -1218883000 - (int32_t)(13U * index);
  p.hMSL_mm = 90000 + (int32_t)(7U * index);
  p.gSpeed_mms = 400 + (int32_t)(3U * index);
  p.headMot_1e5deg = (int32_t)carrySignatureValue(replay.hdr.seq);
  p.hAcc_mm = 2100U + 5U * index;
  p.sAcc_mms = 300U + 7U * index;
  p.baro_temp_milli_c = 25000 + (int32_t)(19U * index);
  p.baro_press_milli_hpa = 1005000 + (int32_t)(29U * index);
  p.baro_alt_mm = 68000 + (int32_t)(31U * index);
  p.baro_vsi_milli_mps = -120 + (int32_t)(2U * index);
  const uint32_t last_gps_ms = 1000U + 41U * index;
  const uint32_t last_imu_ms = 2000U + 43U * index;
  const uint32_t last_baro_ms = 3000U + 47U * index;
  memcpy(p.reserved + 0, &last_gps_ms, sizeof(last_gps_ms));
  memcpy(p.reserved + 4, &last_imu_ms, sizeof(last_imu_ms));
  memcpy(p.reserved + 8, &last_baro_ms, sizeof(last_baro_ms));
}

bool waitForCarrySignatureState(uint32_t expected_seq, uint32_t expected_t_us,
                                teensy_link::PendingState& out, uint32_t timeout_ms) {
  const uint32_t deadline = millis() + timeout_ms;
  while ((int32_t)(millis() - deadline) < 0) {
    teensy_link::PendingState pending = {};
    while (teensy_link::popPendingState(pending)) {
      if ((pending.state.flags & telem::kStateFlagReplayOutput) != 0U &&
          pending.seq == expected_seq &&
          pending.t_us == expected_t_us) {
        out = pending;
        return true;
      }
    }
    delay(2);
  }
  return false;
}

}  // namespace

namespace teensy_api {

bool waitForAck(uint16_t command, uint32_t ack_seq_before, uint32_t timeout_ms, CommandAckResult& out) {
  const uint32_t deadline = millis() + timeout_ms;
  while ((int32_t)(millis() - deadline) < 0) {
    const auto snap = teensy_link::snapshot();
    if (snap.has_ack && snap.ack_command == command && snap.ack_rx_seq != ack_seq_before) {
      out.ack_seen = true;
      out.ack_ok = snap.ack_ok;
      out.command = command;
      out.ack_code = snap.ack_code;
      out.snapshot = snap;
      return true;
    }
    delay(5);
  }
  out.snapshot = teensy_link::snapshot();
  return false;
}

bool getFusionSettings(CommandAckResult& out, uint32_t timeout_ms) {
  const uint32_t ack_seq_before = teensy_link::snapshot().ack_rx_seq;
  out = {};
  out.command = telem::CMD_GET_FUSION_SETTINGS;
  out.tx_ok = teensy_link::sendGetFusionSettings();
  if (!out.tx_ok) return false;
  return waitForAck(out.command, ack_seq_before, timeout_ms, out);
}

bool setCaptureSettings(uint16_t hz, CommandAckResult& out, uint32_t timeout_ms) {
  const uint32_t ack_seq_before = teensy_link::snapshot().ack_rx_seq;
  out = {};
  out.command = telem::CMD_SET_CAPTURE_SETTINGS;
  out.tx_ok = teensy_link::sendSetCaptureSettings(telem::CmdSetCaptureSettingsV1{hz, 0U});
  if (!out.tx_ok) return false;
  return waitForAck(out.command, ack_seq_before, timeout_ms, out);
}

bool saveCaptureSettings(CommandAckResult& out, uint32_t timeout_ms) {
  const uint32_t ack_seq_before = teensy_link::snapshot().ack_rx_seq;
  out = {};
  out.command = telem::CMD_SAVE_CAPTURE_SETTINGS;
  out.tx_ok = teensy_link::sendSaveCaptureSettings();
  if (!out.tx_ok) return false;
  return waitForAck(out.command, ack_seq_before, timeout_ms, out);
}

bool setStreamRate(uint16_t ws_rate_hz, uint16_t log_rate_hz,
                   CommandAckResult& out, uint32_t timeout_ms) {
  const uint32_t ack_seq_before = teensy_link::snapshot().ack_rx_seq;
  out = {};
  out.command = telem::CMD_SET_STREAM_RATE;
  telem::CmdSetStreamRateV1 cmd = {};
  cmd.ws_rate_hz = ws_rate_hz;
  cmd.log_rate_hz = log_rate_hz;
  out.tx_ok = teensy_link::sendSetStreamRate(cmd);
  if (!out.tx_ok) return false;
  return waitForAck(out.command, ack_seq_before, timeout_ms, out);
}

bool setFusionSettings(const telem::CmdSetFusionSettingsV1& cmd,
                       CommandAckResult& out, uint32_t timeout_ms) {
  const uint32_t ack_seq_before = teensy_link::snapshot().ack_rx_seq;
  out = {};
  out.command = telem::CMD_SET_FUSION_SETTINGS;
  out.tx_ok = teensy_link::sendSetFusionSettings(cmd);
  if (!out.tx_ok) return false;
  return waitForAck(out.command, ack_seq_before, timeout_ms, out);
}

void printStatus(Stream& out) {
  const auto snap = teensy_link::snapshot();
  out.printf("TAPI STATUS has=%u seq=%lu t_us=%lu ack=%u ack_seq=%lu ack_cmd=%u ack_ok=%u ack_code=%lu rx_ok=%lu raw_drain=%lu state_drain=%lu poll_runs=%lu poll_gap_max=%lu\r\n",
             snap.has_state ? 1U : 0U,
             (unsigned long)snap.seq,
             (unsigned long)snap.t_us,
             snap.has_ack ? 1U : 0U,
             (unsigned long)snap.ack_rx_seq,
             (unsigned)snap.ack_command,
             snap.ack_ok ? 1U : 0U,
             (unsigned long)snap.ack_code,
             (unsigned long)snap.stats.frames_ok,
             (unsigned long)snap.stats.raw_records_drained,
             (unsigned long)snap.stats.state_records_drained,
             (unsigned long)snap.stats.poll_runs,
             (unsigned long)snap.stats.max_poll_gap_ms);
}

CarrySummary runCarrySignatureTest(Stream& out, uint8_t count) {
  CarrySummary summary = {};
  const uint8_t runs = count ? count : 1U;
  teensy_link::clearPendingStates();
  for (uint32_t i = 0; i < runs; ++i) {
    telem::ReplayInputRecord160 replay = {};
    fillCarrySignatureRecord(i, replay);
    summary.sent++;
    if (!teensy_link::sendReplayInputRecord(replay)) {
      out.printf("TAPI CARRY idx=%lu ok=0 reason=send_failed\r\n", (unsigned long)i);
      summary.fail++;
      continue;
    }

    teensy_link::PendingState pending = {};
    if (!waitForCarrySignatureState(replay.hdr.seq, replay.hdr.t_us, pending, 1000U)) {
      out.printf("TAPI CARRY idx=%lu ok=0 reason=timeout seq=%lu t_us=%lu\r\n",
                 (unsigned long)i,
                 (unsigned long)replay.hdr.seq,
                 (unsigned long)replay.hdr.t_us);
      summary.timeout++;
      summary.fail++;
      continue;
    }

    summary.received++;
    const auto& s = pending.state;
    const auto& p = replay.payload;
    uint32_t last_gps_ms = 0U;
    uint32_t last_imu_ms = 0U;
    uint32_t last_baro_ms = 0U;
    memcpy(&last_gps_ms, p.reserved + 0, sizeof(last_gps_ms));
    memcpy(&last_imu_ms, p.reserved + 4, sizeof(last_imu_ms));
    memcpy(&last_baro_ms, p.reserved + 8, sizeof(last_baro_ms));

    const bool stamp_ok = pending.seq == replay.hdr.seq && pending.t_us == replay.hdr.t_us;
    const bool gps_ok =
        s.iTOW_ms == p.iTOW_ms &&
        s.fixType == p.fixType &&
        s.numSV == p.numSV &&
        s.lat_1e7 == p.lat_1e7 &&
        s.lon_1e7 == p.lon_1e7 &&
        s.hMSL_mm == p.hMSL_mm &&
        s.gSpeed_mms == p.gSpeed_mms &&
        s.headMot_1e5deg == p.headMot_1e5deg &&
        s.hAcc_mm == p.hAcc_mm &&
        s.sAcc_mms == p.sAcc_mms;
    const bool baro_ok =
        floatNearAbs(s.baro_temp_c, (float)p.baro_temp_milli_c * 0.001f, 0.0005f) &&
        floatNearAbs(s.baro_press_hpa, (float)p.baro_press_milli_hpa * 0.001f, 0.0005f) &&
        floatNearAbs(s.baro_alt_m, (float)p.baro_alt_mm * 0.001f, 0.0005f) &&
        floatNearAbs(s.baro_vsi_mps, (float)p.baro_vsi_milli_mps * 0.001f, 0.0005f);
    const bool imu_ok =
        floatNearAbs(s.accel_x_mps2, (float)p.accel_milli_mps2[0] * 0.001f, 0.0005f) &&
        floatNearAbs(s.accel_y_mps2, (float)p.accel_milli_mps2[1] * 0.001f, 0.0005f) &&
        floatNearAbs(s.accel_z_mps2, (float)p.accel_milli_mps2[2] * 0.001f, 0.0005f) &&
        floatNearAbs(s.gyro_x_dps, (float)p.gyro_milli_dps[0] * 0.001f, 0.0005f) &&
        floatNearAbs(s.gyro_y_dps, (float)p.gyro_milli_dps[1] * 0.001f, 0.0005f) &&
        floatNearAbs(s.gyro_z_dps, (float)p.gyro_milli_dps[2] * 0.001f, 0.0005f) &&
        floatNearAbs(s.mag_x_uT, (float)p.mag_milli_uT[0] * 0.001f, 0.0005f) &&
        floatNearAbs(s.mag_y_uT, (float)p.mag_milli_uT[1] * 0.001f, 0.0005f) &&
        floatNearAbs(s.mag_z_uT, (float)p.mag_milli_uT[2] * 0.001f, 0.0005f);
    const bool meta_ok =
        s.last_gps_ms == last_gps_ms &&
        s.last_imu_ms == last_imu_ms &&
        s.last_baro_ms == last_baro_ms;
    const bool mask_ok = s.raw_present_mask == (uint16_t)p.present_mask;

    const bool ok = stamp_ok && gps_ok && baro_ok && imu_ok && meta_ok && mask_ok;
    if (ok) {
      summary.pass++;
    } else {
      summary.fail++;
    }
    out.printf(
        "TAPI CARRY idx=%lu ok=%u stamp=%u gps=%u baro=%u imu=%u meta=%u mask=%u seq=%lu t_us=%lu iTOW=%lu lat=%ld lon=%ld baro_temp=%.3f raw_mask=%u\r\n",
        (unsigned long)i,
        ok ? 1U : 0U,
        stamp_ok ? 1U : 0U,
        gps_ok ? 1U : 0U,
        baro_ok ? 1U : 0U,
        imu_ok ? 1U : 0U,
        meta_ok ? 1U : 0U,
        mask_ok ? 1U : 0U,
        (unsigned long)pending.seq,
        (unsigned long)pending.t_us,
        (unsigned long)s.iTOW_ms,
        (long)s.lat_1e7,
        (long)s.lon_1e7,
        (double)s.baro_temp_c,
        (unsigned)s.raw_present_mask);
  }

  out.printf("TAPI CARRY RESULT ok=%u sent=%lu recv=%lu pass=%lu fail=%lu timeout=%lu count=%u\r\n",
             summary.fail == 0U ? 1U : 0U,
             (unsigned long)summary.sent,
             (unsigned long)summary.received,
             (unsigned long)summary.pass,
             (unsigned long)summary.fail,
             (unsigned long)summary.timeout,
             (unsigned)runs);
  return summary;
}

CarrySummary runCarrySequenceCsvTest(Stream& out, uint32_t duration_ms, uint8_t window_limit) {
  CarrySummary summary = {};
  if (duration_ms == 0U) duration_ms = 2000U;
  if (window_limit == 0U) window_limit = 16U;
  constexpr size_t kMaxRows = 4096U;

  struct CarrySeqCsvRow {
    uint32_t seq;
    uint32_t expected_t_us;
    uint32_t actual_t_us;
    int32_t expected_headmot;
    int32_t actual_headmot;
    uint8_t stamp_ok;
    uint8_t rand_ok;
    uint8_t flags;
    uint8_t reserved;
  };

  CarrySeqCsvRow* rows = (CarrySeqCsvRow*)malloc(sizeof(CarrySeqCsvRow) * kMaxRows);
  if (!rows) {
    out.println("TAPI CARRYCSV RESULT ok=0 reason=alloc_failed");
    summary.fail = 1U;
    return summary;
  }

  teensy_link::clearPendingStates();
  memset(rows, 0, sizeof(CarrySeqCsvRow) * kMaxRows);
  uint32_t inflight = 0U;
  uint32_t next_index = 0U;
  const uint32_t start_ms = millis();
  const uint32_t stop_send_ms = start_ms + duration_ms;
  const uint32_t drain_deadline_ms = stop_send_ms + 1000U;

  while ((int32_t)(millis() - drain_deadline_ms) < 0) {
    const uint32_t now_ms = millis();
    while ((int32_t)(now_ms - stop_send_ms) < 0 && inflight < window_limit && summary.sent < kMaxRows) {
      telem::ReplayInputRecord160 replay = {};
      fillCarrySignatureRecord(next_index, replay);
      if (!teensy_link::sendReplayInputRecord(replay)) {
        break;
      }
      summary.sent++;
      inflight++;
      next_index++;
    }

    teensy_link::PendingState pending = {};
    bool drained_any = false;
    while (teensy_link::popPendingState(pending)) {
      drained_any = true;
      if ((pending.state.flags & telem::kStateFlagReplayOutput) == 0U) continue;
      if (summary.received >= kMaxRows) continue;

      CarrySeqCsvRow& row = rows[summary.received++];
      row.seq = pending.seq;
      row.expected_t_us = 0x51000000UL + (pending.seq - 0xA5000000UL) * 1003U;
      row.actual_t_us = pending.t_us;
      row.expected_headmot = (int32_t)carrySignatureValue(pending.seq);
      row.actual_headmot = pending.state.headMot_1e5deg;
      row.stamp_ok = (pending.t_us == row.expected_t_us) ? 1U : 0U;
      row.rand_ok = (pending.state.headMot_1e5deg == row.expected_headmot) ? 1U : 0U;
      row.flags = pending.state.flags & telem::kStateFlagReplayOutput ? 1U : 0U;
      if (row.stamp_ok && row.rand_ok) {
        summary.pass++;
      } else {
        summary.fail++;
      }
      if (inflight > 0U) inflight--;
    }

    if ((int32_t)(now_ms - stop_send_ms) >= 0 && inflight == 0U && !drained_any) {
      break;
    }
    delay(1);
  }

  if (summary.sent > summary.received) summary.timeout = summary.sent - summary.received;

  out.println("seq,expected_t_us,actual_t_us,expected_headmot,actual_headmot,stamp_ok,rand_ok");
  for (uint32_t i = 0; i < summary.received; ++i) {
    const CarrySeqCsvRow& row = rows[i];
    out.printf("%lu,%lu,%lu,%ld,%ld,%u,%u\r\n",
               (unsigned long)row.seq,
               (unsigned long)row.expected_t_us,
               (unsigned long)row.actual_t_us,
               (long)row.expected_headmot,
               (long)row.actual_headmot,
               (unsigned)row.stamp_ok,
               (unsigned)row.rand_ok);
  }
  out.printf("TAPI CARRYCSV RESULT ok=%u sent=%lu recv=%lu pass=%lu fail=%lu timeout=%lu duration_ms=%lu window=%u\r\n",
             (summary.fail == 0U && summary.timeout == 0U) ? 1U : 0U,
             (unsigned long)summary.sent,
             (unsigned long)summary.received,
             (unsigned long)summary.pass,
             (unsigned long)summary.fail,
             (unsigned long)summary.timeout,
             (unsigned long)duration_ms,
             (unsigned)window_limit);
  free(rows);
  return summary;
}

ReplayBatchBenchmarkSummary runReplayBatchBenchmark(Stream& out, uint32_t duration_ms,
                                                   uint16_t batch_hz, uint16_t records_per_batch) {
  ReplayBatchBenchmarkSummary summary = {};
  if (duration_ms == 0U) duration_ms = 5000U;
  if (batch_hz == 0U) batch_hz = 100U;
  if (records_per_batch == 0U) records_per_batch = 16U;

  summary.duration_ms = duration_ms;
  summary.batch_hz = batch_hz;
  summary.records_per_batch = records_per_batch;

  const uint32_t period_us = (batch_hz > 0U) ? (1000000UL / (uint32_t)batch_hz) : 10000UL;
  const uint32_t start_ms = millis();
  const uint32_t stop_send_ms = start_ms + duration_ms;
  const uint32_t drain_deadline_ms = stop_send_ms + 2000U;
  uint32_t next_batch_us = micros();
  uint32_t next_index = 0U;

  teensy_link::clearPendingStates();

  while ((int32_t)(millis() - drain_deadline_ms) < 0) {
    const uint32_t now_ms = millis();
    const uint32_t now_us = micros();
    if ((int32_t)(now_ms - stop_send_ms) < 0 && (int32_t)(now_us - next_batch_us) >= 0) {
      uint16_t sent_this_batch = 0U;
      while (sent_this_batch < records_per_batch) {
        telem::ReplayInputRecord160 replay = {};
        fillCarrySignatureRecord(next_index, replay);
        if (!teensy_link::sendReplayInputRecord(replay)) {
          break;
        }
        summary.sent++;
        sent_this_batch++;
        next_index++;
      }
      next_batch_us += period_us;
    }

    teensy_link::PendingState pending = {};
    bool drained_any = false;
    while (teensy_link::popPendingState(pending)) {
      drained_any = true;
      if ((pending.state.flags & telem::kStateFlagReplayOutput) == 0U) continue;
      summary.received++;
      const uint32_t expected_t_us = 0x51000000UL + (pending.seq - 0xA5000000UL) * 1003U;
      const int32_t expected_headmot = (int32_t)carrySignatureValue(pending.seq);
      const bool stamp_ok = pending.t_us == expected_t_us;
      const bool rand_ok = pending.state.headMot_1e5deg == expected_headmot;
      if (stamp_ok && rand_ok) {
        summary.pass++;
      } else {
        summary.fail++;
      }
    }

    if ((int32_t)(now_ms - stop_send_ms) >= 0 && !drained_any && summary.received >= summary.sent) {
      break;
    }
    delay(1);
  }

  if (summary.sent > summary.received) summary.timeout = summary.sent - summary.received;
  summary.elapsed_ms = millis() - start_ms;
  if (summary.elapsed_ms > 0U) {
    summary.validated_rps = (1000.0f * (float)summary.pass) / (float)summary.elapsed_ms;
  }
  out.printf(
      "TAPI REPLAYBENCH RESULT ok=%u batch_hz=%u batch_records=%u duration_ms=%lu elapsed_ms=%lu sent=%lu recv=%lu pass=%lu fail=%lu timeout=%lu validated_rps=%.1f\r\n",
      (summary.fail == 0U && summary.timeout == 0U) ? 1U : 0U,
      (unsigned)summary.batch_hz,
      (unsigned)summary.records_per_batch,
      (unsigned long)summary.duration_ms,
      (unsigned long)summary.elapsed_ms,
      (unsigned long)summary.sent,
      (unsigned long)summary.received,
      (unsigned long)summary.pass,
      (unsigned long)summary.fail,
      (unsigned long)summary.timeout,
      (double)summary.validated_rps);
  return summary;
}

}  // namespace teensy_api
