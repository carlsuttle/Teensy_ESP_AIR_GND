#include "time_service.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

namespace time_service {
namespace {

constexpr uint8_t kTrustFixTypeMin = 3U;
constexpr uint8_t kTrustConsecutiveSamples = 3U;
constexpr uint32_t kResyncMinIntervalMs = 60000U;
constexpr uint32_t kResyncThresholdSeconds = 2U;
constexpr uint32_t kRecentSetRejectWindowMs = 10000U;
constexpr uint32_t kRecentSetRejectDeltaSeconds = 300U;
constexpr uint16_t kSaneYearMin = 2024U;
constexpr uint16_t kSaneYearMax = 2099U;

class Service {
 public:
  explicit Service(bool apply_system_clock) : apply_system_clock_(apply_system_clock) {}

  void setVerbose(bool enabled) { verbose_ = enabled; }

  void begin() {
    setenv("TZ", "UTC0", 1);
    tzset();
    reset();
  }

  void reset() {
    time_state_ = (uint8_t)telem::TimeStateCode::UNSET;
    time_source_ = (uint8_t)telem::TimeSourceCode::NONE;
    time_flags_ = 0U;
    candidate_count_ = 0U;
    candidate_epoch_s_ = 0U;
    candidate_time_ = {};
    last_trusted_gps_ = {};
    last_trusted_epoch_s_ = 0U;
    time_last_set_ms_ = 0U;
    time_sync_count_ = 0U;
    simulated_epoch_s_ = 0U;
    simulated_epoch_set_ms_ = 0U;
    printed_calendar_seen_ = false;
    printed_reject_ = false;
  }

  void ingestGpsSample(uint8_t fix_type,
                       const telem::GpsCalendarTime& gps_time,
                       uint16_t state_flags,
                       uint32_t now_ms) {
    const bool replay_output = (state_flags & telem::kStateFlagReplayOutput) != 0U;
    const bool calendar_present = gpsCalendarPresent(gps_time);
    updateFlag(telem::kTimeStatusFlagGpsCalendarPresent, calendar_present);

    if (calendar_present && !printed_calendar_seen_ && apply_system_clock_ && verbose_) {
      printed_calendar_seen_ = true;
      Serial.printf("TIME calendar_seen utc=%04u-%02u-%02u %02u:%02u:%02u\r\n",
                    (unsigned)gps_time.year,
                    (unsigned)gps_time.month,
                    (unsigned)gps_time.day,
                    (unsigned)gps_time.hour,
                    (unsigned)gps_time.minute,
                    (unsigned)gps_time.second);
    }

    uint32_t epoch_s = 0U;
    const bool gps_plausible = calendarSane(gps_time) && gpsCalendarToEpoch(gps_time, epoch_s);
    const bool gps_trust_input = !replay_output && fix_type >= kTrustFixTypeMin && gps_plausible;
    updateFlag(telem::kTimeStatusFlagGpsTimeValid, gps_trust_input);

    if (!gps_trust_input) {
      candidate_count_ = 0U;
      candidate_epoch_s_ = 0U;
      candidate_time_ = {};
      if ((time_flags_ & telem::kTimeStatusFlagSystemTimeSet) != 0U) {
        setState((uint8_t)telem::TimeStateCode::HOLDOVER, now_ms, "holdover");
      } else {
        setState((uint8_t)(calendar_present ? telem::TimeStateCode::GPS_TENTATIVE : telem::TimeStateCode::UNSET),
                 now_ms,
                 nullptr);
      }
      return;
    }

    if (candidate_count_ == 0U || !candidateMatches(epoch_s)) {
      candidate_count_ = 1U;
      candidate_epoch_s_ = epoch_s;
      candidate_time_ = gps_time;
      if ((time_flags_ & telem::kTimeStatusFlagSystemTimeSet) == 0U) {
        setState((uint8_t)telem::TimeStateCode::GPS_TENTATIVE, now_ms, nullptr);
      }
      return;
    }

    candidate_count_++;
    candidate_epoch_s_ = epoch_s;
    candidate_time_ = gps_time;
    if ((time_flags_ & telem::kTimeStatusFlagSystemTimeSet) == 0U) {
      setState((uint8_t)telem::TimeStateCode::GPS_TENTATIVE, now_ms, nullptr);
    }

    if (candidate_count_ < kTrustConsecutiveSamples) return;

    const uint32_t current_epoch_s = clockEpochSeconds(now_ms);
    if ((time_flags_ & telem::kTimeStatusFlagSystemTimeSet) != 0U &&
        time_last_set_ms_ != 0U &&
        (uint32_t)(now_ms - time_last_set_ms_) <= kRecentSetRejectWindowMs) {
      const uint32_t delta_s = absDiffSeconds(epoch_s, current_epoch_s);
      if (delta_s > kRecentSetRejectDeltaSeconds) {
        if (apply_system_clock_ && verbose_ && !printed_reject_) {
          printed_reject_ = true;
          Serial.printf("TIME reject wild_jump gps=%lu sys=%lu delta_s=%lu\r\n",
                        (unsigned long)epoch_s,
                        (unsigned long)current_epoch_s,
                        (unsigned long)delta_s);
        }
        return;
      }
    }
    printed_reject_ = false;

    const bool have_clock = (time_flags_ & telem::kTimeStatusFlagSystemTimeSet) != 0U;
    const uint32_t delta_s = absDiffSeconds(epoch_s, current_epoch_s);
    const bool should_initial_set = !have_clock;
    const bool should_resync =
        have_clock && (uint32_t)(now_ms - time_last_set_ms_) >= kResyncMinIntervalMs &&
        delta_s >= kResyncThresholdSeconds;
    if (should_initial_set || should_resync) {
      if (!setClockEpoch(epoch_s, now_ms)) return;
      last_trusted_epoch_s_ = epoch_s;
      last_trusted_gps_ = gps_time;
      time_source_ = (uint8_t)telem::TimeSourceCode::GPS;
      if (apply_system_clock_ && verbose_) {
        Serial.printf("TIME sync source=gps state=%s epoch=%lu sync_count=%lu\r\n",
                      should_initial_set ? "initial" : "resync",
                      (unsigned long)epoch_s,
                      (unsigned long)time_sync_count_);
      }
    } else {
      last_trusted_epoch_s_ = epoch_s;
      last_trusted_gps_ = gps_time;
      time_source_ = (uint8_t)telem::TimeSourceCode::GPS;
    }

    setState((uint8_t)telem::TimeStateCode::GPS_VALID,
             now_ms,
             (time_state_ == (uint8_t)telem::TimeStateCode::HOLDOVER) ? "gps_regained" : "gps_valid");
  }

  StatusSnapshot snapshot(uint32_t now_ms) const {
    StatusSnapshot out = {};
    out.time_state = time_state_;
    out.time_source = time_source_;
    out.time_flags = time_flags_;
    out.system_time_utc_s = clockEpochSeconds(now_ms);
    out.time_last_set_ms = time_last_set_ms_;
    out.time_last_set_age_ms =
        time_last_set_ms_ != 0U ? (uint32_t)(now_ms - time_last_set_ms_) : 0xFFFFFFFFUL;
    out.time_sync_count = time_sync_count_;
    out.last_trusted_gps = last_trusted_gps_;
    return out;
  }

 private:
  bool gpsCalendarPresent(const telem::GpsCalendarTime& gps_time) const {
    return gps_time.year != 0U && gps_time.month != 0U && gps_time.day != 0U;
  }

  bool calendarSane(const telem::GpsCalendarTime& gps_time) const {
    if (gps_time.year < kSaneYearMin || gps_time.year > kSaneYearMax) return false;
    if (gps_time.month < 1U || gps_time.month > 12U) return false;
    if (gps_time.day < 1U || gps_time.day > 31U) return false;
    if (gps_time.hour > 23U || gps_time.minute > 59U || gps_time.second > 59U) return false;
    return true;
  }

  bool gpsCalendarToEpoch(const telem::GpsCalendarTime& gps_time, uint32_t& out_epoch_s) const {
    struct tm tm_utc = {};
    tm_utc.tm_year = (int)gps_time.year - 1900;
    tm_utc.tm_mon = (int)gps_time.month - 1;
    tm_utc.tm_mday = (int)gps_time.day;
    tm_utc.tm_hour = (int)gps_time.hour;
    tm_utc.tm_min = (int)gps_time.minute;
    tm_utc.tm_sec = (int)gps_time.second;
    tm_utc.tm_isdst = 0;
    const time_t epoch = mktime(&tm_utc);
    if (epoch < 0) return false;
    struct tm check = {};
    if (gmtime_r(&epoch, &check) == nullptr) return false;
    if ((uint16_t)(check.tm_year + 1900) != gps_time.year ||
        (uint8_t)(check.tm_mon + 1) != gps_time.month ||
        (uint8_t)check.tm_mday != gps_time.day ||
        (uint8_t)check.tm_hour != gps_time.hour ||
        (uint8_t)check.tm_min != gps_time.minute ||
        (uint8_t)check.tm_sec != gps_time.second) {
      return false;
    }
    out_epoch_s = (uint32_t)epoch;
    return true;
  }

  uint32_t absDiffSeconds(uint32_t a, uint32_t b) const {
    return (a >= b) ? (a - b) : (b - a);
  }

  bool candidateMatches(uint32_t epoch_s) const {
    return absDiffSeconds(candidate_epoch_s_, epoch_s) <= 1U;
  }

  bool setClockEpoch(uint32_t epoch_s, uint32_t now_ms) {
    if (apply_system_clock_) {
      struct timeval tv = {};
      tv.tv_sec = (time_t)epoch_s;
      tv.tv_usec = 0;
      if (settimeofday(&tv, nullptr) != 0) {
        if (verbose_) {
          Serial.printf("TIME set_failed epoch=%lu\r\n", (unsigned long)epoch_s);
        }
        return false;
      }
    }
    simulated_epoch_s_ = epoch_s;
    simulated_epoch_set_ms_ = now_ms;
    time_last_set_ms_ = now_ms;
    time_sync_count_++;
    updateFlag(telem::kTimeStatusFlagSystemTimeSet, true);
    return true;
  }

  uint32_t clockEpochSeconds(uint32_t now_ms) const {
    if (apply_system_clock_) {
      const time_t now = time(nullptr);
      return now > 0 ? (uint32_t)now : 0U;
    }
    if ((time_flags_ & telem::kTimeStatusFlagSystemTimeSet) == 0U) return 0U;
    return simulated_epoch_s_ + ((uint32_t)(now_ms - simulated_epoch_set_ms_) / 1000U);
  }

  void updateFlag(uint8_t mask, bool enabled) {
    if (enabled) {
      time_flags_ |= mask;
    } else {
      time_flags_ &= (uint8_t)~mask;
    }
  }

  void setState(uint8_t next_state, uint32_t now_ms, const char* reason) {
    const uint8_t prev_state = time_state_;
    time_state_ = next_state;
    if (prev_state != next_state && apply_system_clock_ && verbose_ && reason && reason[0] != '\0') {
      Serial.printf("TIME state=%s reason=%s age_ms=%lu\r\n",
                    telem::timeStateText(next_state),
                    reason,
                    (unsigned long)(time_last_set_ms_ != 0U ? (uint32_t)(now_ms - time_last_set_ms_) : 0xFFFFFFFFUL));
    }
  }

  bool apply_system_clock_ = false;
  uint8_t time_state_ = (uint8_t)telem::TimeStateCode::UNSET;
  uint8_t time_source_ = (uint8_t)telem::TimeSourceCode::NONE;
  uint8_t time_flags_ = 0U;
  uint8_t candidate_count_ = 0U;
  uint32_t candidate_epoch_s_ = 0U;
  telem::GpsCalendarTime candidate_time_ = {};
  telem::GpsCalendarTime last_trusted_gps_ = {};
  uint32_t last_trusted_epoch_s_ = 0U;
  uint32_t time_last_set_ms_ = 0U;
  uint32_t time_sync_count_ = 0U;
  uint32_t simulated_epoch_s_ = 0U;
  uint32_t simulated_epoch_set_ms_ = 0U;
  bool printed_calendar_seen_ = false;
  bool printed_reject_ = false;
  bool verbose_ = true;
};

Service g_service(true);

telem::GpsCalendarTime makeGpsTime(uint16_t year,
                                   uint8_t month,
                                   uint8_t day,
                                   uint8_t hour,
                                   uint8_t minute,
                                   uint8_t second) {
  telem::GpsCalendarTime gps_time = {};
  gps_time.year = year;
  gps_time.month = month;
  gps_time.day = day;
  gps_time.hour = hour;
  gps_time.minute = minute;
  gps_time.second = second;
  return gps_time;
}

bool selfTestCase(Stream& out, const char* name, bool ok, const char* detail) {
  out.printf("TIMETEST case=%s ok=%u detail=%s\r\n", name, ok ? 1U : 0U, detail ? detail : "");
  return ok;
}

}  // namespace

void begin() {
  g_service.begin();
}

void setVerbose(bool enabled) {
  g_service.setVerbose(enabled);
}

void ingestState(const telem::TelemetryStateRecord& state, bool has_state, uint32_t now_ms) {
  if (!has_state) {
    g_service.ingestGpsSample(0U, {}, 0U, now_ms);
    return;
  }
  g_service.ingestGpsSample(state.fixType, telem::gpsCalendarTime(state), state.flags, now_ms);
}

void ingestGpsSample(uint8_t fix_type,
                     const telem::GpsCalendarTime& gps_time,
                     uint16_t state_flags,
                     uint32_t now_ms) {
  g_service.ingestGpsSample(fix_type, gps_time, state_flags, now_ms);
}

StatusSnapshot snapshot(uint32_t now_ms) {
  return g_service.snapshot(now_ms);
}

bool systemTimeUtcSeconds(uint32_t& out_utc_s) {
  out_utc_s = snapshot(millis()).system_time_utc_s;
  return out_utc_s != 0U;
}

void fillDownlinkStatus(telem::DownlinkStatusV1& status, uint32_t now_ms) {
  const StatusSnapshot ts = snapshot(now_ms);
  status.time_state = ts.time_state;
  status.time_source = ts.time_source;
  status.time_flags = ts.time_flags;
  status.system_time_utc_s = ts.system_time_utc_s;
  status.time_last_set_age_ms = ts.time_last_set_age_ms;
  status.time_sync_count = (ts.time_sync_count > 0xFFFFUL) ? 0xFFFFU : (uint16_t)ts.time_sync_count;
}

void fillStorageStatus(telem::StorageStatusPayloadV1& status, uint32_t now_ms) {
  const StatusSnapshot ts = snapshot(now_ms);
  status.time_state = ts.time_state;
  status.time_source = ts.time_source;
  status.time_flags = ts.time_flags;
  status.system_time_utc_s = ts.system_time_utc_s;
  status.time_last_set_age_ms = ts.time_last_set_age_ms;
  status.time_sync_count = (ts.time_sync_count > 0xFFFFUL) ? 0xFFFFU : (uint16_t)ts.time_sync_count;
}

bool runSelfTest(Stream& out) {
  Service test(false);
  test.begin();
  bool ok = true;

  {
    const StatusSnapshot s0 = test.snapshot(0U);
    ok &= selfTestCase(out,
                       "startup_unset",
                       s0.time_state == (uint8_t)telem::TimeStateCode::UNSET &&
                           (s0.time_flags & telem::kTimeStatusFlagSystemTimeSet) == 0U,
                       telem::timeStateText(s0.time_state));
  }

  {
    const telem::GpsCalendarTime gps = makeGpsTime(2026U, 4U, 2U, 12U, 0U, 0U);
    test.ingestGpsSample(3U, gps, 0U, 1000U);
    test.ingestGpsSample(3U, gps, 0U, 1100U);
    StatusSnapshot s1 = test.snapshot(1100U);
    ok &= selfTestCase(out,
                       "tentative_before_trust",
                       s1.time_state == (uint8_t)telem::TimeStateCode::GPS_TENTATIVE,
                       telem::timeStateText(s1.time_state));
    test.ingestGpsSample(3U, makeGpsTime(2026U, 4U, 2U, 12U, 0U, 1U), 0U, 1200U);
    s1 = test.snapshot(1200U);
    ok &= selfTestCase(out,
                       "delayed_lock_to_valid",
                       s1.time_state == (uint8_t)telem::TimeStateCode::GPS_VALID &&
                           (s1.time_flags & telem::kTimeStatusFlagSystemTimeSet) != 0U &&
                           s1.time_sync_count == 1U,
                       telem::timeStateText(s1.time_state));
  }

  {
    test.ingestGpsSample(0U, {}, 0U, 5000U);
    const StatusSnapshot s2 = test.snapshot(5000U);
    ok &= selfTestCase(out,
                       "holdover_after_loss",
                       s2.time_state == (uint8_t)telem::TimeStateCode::HOLDOVER &&
                           (s2.time_flags & telem::kTimeStatusFlagSystemTimeSet) != 0U,
                       telem::timeStateText(s2.time_state));
  }

  {
    Service reject(false);
    reject.begin();
    const telem::GpsCalendarTime gps = makeGpsTime(2026U, 4U, 2U, 12U, 0U, 0U);
    reject.ingestGpsSample(3U, gps, 0U, 1000U);
    reject.ingestGpsSample(3U, gps, 0U, 1100U);
    reject.ingestGpsSample(3U, makeGpsTime(2026U, 4U, 2U, 12U, 0U, 1U), 0U, 1200U);
    reject.ingestGpsSample(3U, makeGpsTime(2100U, 1U, 1U, 0U, 0U, 0U), 0U, 2000U);
    const StatusSnapshot s3 = reject.snapshot(2000U);
    ok &= selfTestCase(out,
                       "invalid_jump_rejected",
                       s3.time_sync_count == 1U && s3.time_state == (uint8_t)telem::TimeStateCode::HOLDOVER,
                       telem::timeStateText(s3.time_state));
  }

  out.printf("TIMETEST RESULT ok=%u\r\n", ok ? 1U : 0U);
  return ok;
}

}  // namespace time_service
