#include <Arduino.h>
#include <ctype.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <bmi2_defs.h>

#include "config.h"
#include "state.h"
#include "mirror.h"
#include "gps_ubx.h"
#include "imu_fusion.h"
#include "MagCal.h"
#include "telemetry_crsf.h"

namespace {
State g_state = {};
Adafruit_BMP280 g_bmp;
bool g_baro_ok = false;
bool g_imu_ok = false;
uint32_t g_mirror_seq = 0;
uint32_t g_last_mirror_seq = 0;
uint32_t g_last_mirror_t_us = 0;

elapsedMicros g_summary_timer_us;
elapsedMicros g_mirror_timer_us;

constexpr uint32_t SUMMARY_PERIOD_US = 500000; // 2 Hz

constexpr float BARO_QNH_HPA = 1013.25f;
constexpr float BARO_ALT_TAU_S = 2.0f;
constexpr float BARO_VSI_TAU_S = 2.0f;
constexpr float kGravityMps2 = 9.80665f;
float g_last_baro_alt_m = NAN;

constexpr int kCalibStructAddr = 64;
constexpr uint32_t kCalibMagic = 0x43414C31UL;  // "CAL1"
constexpr uint16_t kCalibVersion = 2;

enum class CommandMode : uint8_t {
  Idle,
  CalMag,
  CalImu,
  GetHdg,
  ShowYawCmp,
  SpdTest,
  ShowCrsfIn,
  ShowImuData,
  ShowImuError,
  TestImuRot,
  EspComTest
};

CommandMode g_mode = CommandMode::Idle;
uint32_t g_mode_start_ms = 0;
uint32_t g_mode_last_print_ms = 0;
bool g_stats_streaming = false;
uint32_t g_spdtest_next_us = 0;
uint32_t g_spdtest_sent = 0;
uint32_t g_spdtest_drop = 0;

struct CalImuCtx {
  bool active = false;
  uint32_t sample_count = 0;
  double sum[6] = {0, 0, 0, 0, 0, 0};
} g_cal_imu;

struct CalMagCtx {
  bool active = false;
  uint32_t last_print_ms = 0;
} g_cal_mag;

struct RunningStats {
  uint32_t n = 0;
  double mean = 0.0;
  double m2 = 0.0;
  void reset() {
    n = 0;
    mean = 0.0;
    m2 = 0.0;
  }
  void push(double x) {
    n++;
    const double d = x - mean;
    mean += d / (double)n;
    const double d2 = x - mean;
    m2 += d * d2;
  }
  double stddev() const {
    if (n < 2) return 0.0;
    return sqrt(m2 / (double)(n - 1));
  }
};

struct ImuErrorStats {
  RunningStats gnorm;
  RunningStats gxRaw;
  RunningStats gyRaw;
  RunningStats gzRaw;
  RunningStats gxCorr;
  RunningStats gyCorr;
  RunningStats gzCorr;
  RunningStats wmag;
  RunningStats tempC;
  RunningStats gyRawCount;
  uint32_t n = 0;
  void reset() {
    gnorm.reset();
    gxRaw.reset();
    gyRaw.reset();
    gzRaw.reset();
    gxCorr.reset();
    gyCorr.reset();
    gzCorr.reset();
    wmag.reset();
    tempC.reset();
    gyRawCount.reset();
    n = 0;
  }
  void push(double gnormIn,
            double gxRawIn, double gyRawIn, double gzRawIn,
            double gxCorrIn, double gyCorrIn, double gzCorrIn,
            double wmagIn, float tempIn, int16_t gyRawCountIn) {
    n++;
    gnorm.push(gnormIn);
    gxRaw.push(gxRawIn);
    gyRaw.push(gyRawIn);
    gzRaw.push(gzRawIn);
    gxCorr.push(gxCorrIn);
    gyCorr.push(gyCorrIn);
    gzCorr.push(gzCorrIn);
    wmag.push(wmagIn);
    if (!isnan(tempIn)) tempC.push((double)tempIn);
    gyRawCount.push((double)gyRawCountIn);
  }
};

constexpr uint32_t kImuFastN = 200;   // 0.5 s at 400 Hz
constexpr uint32_t kImuBiasN = 2000;  // 5.0 s at 400 Hz
constexpr uint32_t kImuDiagPeriodUs = 2500;  // 400 Hz scheduled read
constexpr float kStationaryGAbs = 0.03f;
constexpr float kStationaryWMaxDps = 0.5f;
constexpr float kBiasAbortNonStationaryRatio = 0.05f;
constexpr float kBiasMaxGStd = 0.01f; // optional micro-vibration guard
constexpr uint32_t kStationarySettleMs = 2000;

struct ImuErrorCtx {
  ImuErrorStats fast;
  ImuErrorStats bias;
  RunningStats dtUs;
  uint32_t dtMinUs = UINT32_MAX;
  uint32_t dtMaxUs = 0;
  uint32_t lastSampleUs = 0;
  uint32_t nextSampleUs = 0;
  bool biasActive = false;
  uint32_t biasSeen = 0;
  uint32_t biasNonStationary = 0;
  uint32_t stationarySinceMs = 0;
  float lastTempC = NAN;
  uint32_t lastTempReadMs = 0;
  float biasGxDps = 0.0f;
  float biasGyDps = 0.0f;
  float biasGzDps = 0.0f;
  void resetAll() {
    fast.reset();
    bias.reset();
    dtUs.reset();
    dtMinUs = UINT32_MAX;
    dtMaxUs = 0;
    lastSampleUs = 0;
    nextSampleUs = 0;
    biasActive = false;
    biasSeen = 0;
    biasNonStationary = 0;
    stationarySinceMs = 0;
    lastTempC = NAN;
    lastTempReadMs = 0;
    biasGxDps = biasGyDps = biasGzDps = 0.0f;
  }
  void startBiasWindow() {
    bias.reset();
    biasActive = true;
    biasSeen = 0;
    biasNonStationary = 0;
  }
  void endBiasWindow() {
    bias.reset();
    biasActive = false;
    biasSeen = 0;
    biasNonStationary = 0;
  }
} g_imu_err;

struct ImuRotTestCtx {
  bool biasReady = false;
  bool rotating = false;
  uint32_t settleSamples = 0;
  uint32_t stillSamples = 0;
  uint32_t lastUs = 0;
  float biasX = 0.0f, biasY = 0.0f, biasZ = 0.0f;
  double sumX = 0.0, sumY = 0.0, sumZ = 0.0;
  float angleDeg = 0.0f;
  void reset() {
    biasReady = false;
    rotating = false;
    settleSamples = 0;
    stillSamples = 0;
    lastUs = 0;
    biasX = biasY = biasZ = 0.0f;
    sumX = sumY = sumZ = 0.0;
    angleDeg = 0.0f;
  }
} g_rot_test;

struct EspComTestCtx {
  bool active = false;
  uint32_t probeSeq = 0;
  uint32_t probesSent = 0;
  uint32_t rxBytes = 0;
  uint32_t startMs = 0;
  uint32_t lastProbeMs = 0;
  uint32_t lastPrintMs = 0;
  uint32_t lastRxMs = 0;
  bool gotAck = false;
  char line[96] = {};
  uint8_t lineIdx = 0;
  void reset() {
    active = false;
    probeSeq = 0;
    probesSent = 0;
    rxBytes = 0;
    startMs = 0;
    lastProbeMs = 0;
    lastPrintMs = 0;
    lastRxMs = 0;
    gotAck = false;
    lineIdx = 0;
    line[0] = '\0';
  }
} g_esp_com_test;

struct PersistedCalibration {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  float accelGyroOffsets[6];
  float accelScale;
  float magHardIronMinX;
  float magHardIronMaxX;
  float magHardIronMinY;
  float magHardIronMaxY;
  float magHardIronMinZ;
  float magHardIronMaxZ;
  float magHardIronOffX;
  float magHardIronOffY;
  float magHardIronOffZ;
  float magHardIronConfidence;
  uint8_t magHardIronValid;
  uint8_t reserved[3];
  uint32_t checksum;
};

// Legacy EEPROM layout used by calibration version 1.
struct PersistedCalibrationV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  float accelGyroOffsets[6];
  float magHardIronMinX;
  float magHardIronMaxX;
  float magHardIronMinY;
  float magHardIronMaxY;
  float magHardIronMinZ;
  float magHardIronMaxZ;
  float magHardIronOffX;
  float magHardIronOffY;
  float magHardIronOffZ;
  float magHardIronConfidence;
  uint8_t magHardIronValid;
  uint8_t reserved[3];
  uint32_t checksum;
};

MagCal g_mag_cal;

void printSummary2Hz();
void printImuConfig();
void printSetImuCfgUsage();
bool applySetImuCfg(const char* cmd);
void runTeensyLoopbackTest();

void printFusionSettingsSummary(const char* source) {
  float gain = 0.0f, accelRejection = 0.0f, magRejection = 0.0f;
  uint16_t recoveryPeriod = 0U;
  imu_fusion::getFusionSettings(gain, accelRejection, magRejection, recoveryPeriod);
  Serial.printf("FUSION %s: gain=%.3f accRej=%.1f magRej=%.1f rec=%u (%.2fs)\r\n",
                source ? source : "active",
                (double)gain,
                (double)accelRejection,
                (double)magRejection,
                (unsigned)recoveryPeriod,
                (double)((float)recoveryPeriod / 400.0f));
}

bool writeLineNonBlocking(Stream& out, const char* line) {
  if (!line) return false;
  const size_t len = strlen(line);
  if (len == 0) return true;
  if (out.availableForWrite() < (int)len) return false;
  return out.write(reinterpret_cast<const uint8_t*>(line), len) == len;
}

struct BufferedLogger {
  static constexpr uint8_t kDepth = 32;
  static constexpr size_t kLineLen = 320;
  char lines[kDepth][kLineLen] = {};
  uint8_t head = 0;
  uint8_t tail = 0;
  uint32_t dropped = 0;

  bool enqueue(const char* line) {
    if (!line) return false;
    const uint8_t next = (uint8_t)((head + 1U) % kDepth);
    if (next == tail) {
      dropped++;
      return false;
    }
    const size_t n = strnlen(line, kLineLen - 1U);
    memcpy(lines[head], line, n);
    lines[head][n] = '\0';
    head = next;
    return true;
  }

  bool enqueuef(const char* fmt, ...) {
    if (!fmt) return false;
    char line[kLineLen];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return false;
    return enqueue(line);
  }

  void service(Stream& out, uint8_t maxLinesPerCall = 4U) {
    while (tail != head && maxLinesPerCall > 0U) {
      if (!writeLineNonBlocking(out, lines[tail])) return;
      tail = (uint8_t)((tail + 1U) % kDepth);
      maxLinesPerCall--;
    }
  }
} g_logger;

bool startsWith(const char* s, const char* prefix) {
  if (!s || !prefix) return false;
  while (*prefix) {
    if (*s++ != *prefix++) return false;
  }
  return true;
}

bool parseKeyU32(const char* s, const char* key, uint32_t& out) {
  if (!s || !key) return false;
  const char* p = strstr(s, key);
  if (!p) return false;
  p += strlen(key);
  if (*p != '=') return false;
  ++p;
  char* end = nullptr;
  const unsigned long v = strtoul(p, &end, 0);
  if (end == p) return false;
  out = (uint32_t)v;
  return true;
}

uint8_t mapAccOdrFromHz(uint32_t hz, bool& ok) {
  ok = true;
  switch (hz) {
    case 25: return BMI2_ACC_ODR_25HZ;
    case 50: return BMI2_ACC_ODR_50HZ;
    case 100: return BMI2_ACC_ODR_100HZ;
    case 200: return BMI2_ACC_ODR_200HZ;
    case 400: return BMI2_ACC_ODR_400HZ;
    case 800: return BMI2_ACC_ODR_800HZ;
    case 1600: return BMI2_ACC_ODR_1600HZ;
    default: ok = false; return 0;
  }
}

uint8_t mapGyrOdrFromHz(uint32_t hz, bool& ok) {
  ok = true;
  switch (hz) {
    case 25: return BMI2_GYR_ODR_25HZ;
    case 50: return BMI2_GYR_ODR_50HZ;
    case 100: return BMI2_GYR_ODR_100HZ;
    case 200: return BMI2_GYR_ODR_200HZ;
    case 400: return BMI2_GYR_ODR_400HZ;
    case 800: return BMI2_GYR_ODR_800HZ;
    case 1600: return BMI2_GYR_ODR_1600HZ;
    case 3200: return BMI2_GYR_ODR_3200HZ;
    default: ok = false; return 0;
  }
}

uint8_t mapAccRangeFromG(uint32_t g, bool& ok) {
  ok = true;
  switch (g) {
    case 2: return BMI2_ACC_RANGE_2G;
    case 4: return BMI2_ACC_RANGE_4G;
    case 8: return BMI2_ACC_RANGE_8G;
    case 16: return BMI2_ACC_RANGE_16G;
    default: ok = false; return 0;
  }
}

uint8_t mapGyrRangeFromDps(uint32_t dps, bool& ok) {
  ok = true;
  switch (dps) {
    case 125: return BMI2_GYR_RANGE_125;
    case 250: return BMI2_GYR_RANGE_250;
    case 500: return BMI2_GYR_RANGE_500;
    case 1000: return BMI2_GYR_RANGE_1000;
    case 2000: return BMI2_GYR_RANGE_2000;
    default: ok = false; return 0;
  }
}

const char* accRangeLabel(uint8_t range) {
  switch (range) {
    case BMI2_ACC_RANGE_2G: return "2g";
    case BMI2_ACC_RANGE_4G: return "4g";
    case BMI2_ACC_RANGE_8G: return "8g";
    case BMI2_ACC_RANGE_16G: return "16g";
    default: return "unknown";
  }
}

const char* gyrRangeLabel(uint8_t range) {
  switch (range) {
    case BMI2_GYR_RANGE_125: return "125dps";
    case BMI2_GYR_RANGE_250: return "250dps";
    case BMI2_GYR_RANGE_500: return "500dps";
    case BMI2_GYR_RANGE_1000: return "1000dps";
    case BMI2_GYR_RANGE_2000: return "2000dps";
    default: return "unknown";
  }
}

float expectedGyroLsbPerDps(uint8_t range) {
  switch (range) {
    case BMI2_GYR_RANGE_2000: return 16.384f;
    case BMI2_GYR_RANGE_1000: return 32.768f;
    case BMI2_GYR_RANGE_500: return 65.536f;
    case BMI2_GYR_RANGE_250: return 131.072f;
    case BMI2_GYR_RANGE_125: return 262.144f;
    default: return NAN;
  }
}

uint32_t crc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = -(int32_t)(crc & 1U);
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

uint32_t calibrationChecksum(const PersistedCalibration& cfg) {
  return crc32(reinterpret_cast<const uint8_t*>(&cfg), offsetof(PersistedCalibration, checksum));
}

uint32_t calibrationChecksumV1(const PersistedCalibrationV1& cfg) {
  return crc32(reinterpret_cast<const uint8_t*>(&cfg), offsetof(PersistedCalibrationV1, checksum));
}

void initDefaultCalibration(PersistedCalibration& cfg) {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = kCalibMagic;
  cfg.version = kCalibVersion;
  cfg.size = sizeof(PersistedCalibration);
  cfg.accelScale = 1.0f;
}

bool loadCalibrationConfig(PersistedCalibration& cfg) {
  PersistedCalibration v2{};
  EEPROM.get(kCalibStructAddr, v2);
  if (v2.magic == kCalibMagic &&
      v2.version == kCalibVersion &&
      v2.size == sizeof(PersistedCalibration) &&
      v2.checksum == calibrationChecksum(v2)) {
    cfg = v2;
    if (!isfinite(cfg.accelScale) || cfg.accelScale <= 0.0f) cfg.accelScale = 1.0f;
    return true;
  }

  PersistedCalibrationV1 v1{};
  EEPROM.get(kCalibStructAddr, v1);
  if (v1.magic != kCalibMagic ||
      v1.version != 1 ||
      v1.size != sizeof(PersistedCalibrationV1) ||
      v1.checksum != calibrationChecksumV1(v1)) {
    return false;
  }

  initDefaultCalibration(cfg);
  memcpy(cfg.accelGyroOffsets, v1.accelGyroOffsets, sizeof(cfg.accelGyroOffsets));
  cfg.magHardIronMinX = v1.magHardIronMinX;
  cfg.magHardIronMaxX = v1.magHardIronMaxX;
  cfg.magHardIronMinY = v1.magHardIronMinY;
  cfg.magHardIronMaxY = v1.magHardIronMaxY;
  cfg.magHardIronMinZ = v1.magHardIronMinZ;
  cfg.magHardIronMaxZ = v1.magHardIronMaxZ;
  cfg.magHardIronOffX = v1.magHardIronOffX;
  cfg.magHardIronOffY = v1.magHardIronOffY;
  cfg.magHardIronOffZ = v1.magHardIronOffZ;
  cfg.magHardIronConfidence = v1.magHardIronConfidence;
  cfg.magHardIronValid = v1.magHardIronValid;
  cfg.accelScale = 1.0f;
  return true;
}

void saveCalibrationConfig(PersistedCalibration& cfg) {
  cfg.magic = kCalibMagic;
  cfg.version = kCalibVersion;
  cfg.size = sizeof(PersistedCalibration);
  cfg.checksum = calibrationChecksum(cfg);
  EEPROM.put(kCalibStructAddr, cfg);
}

void saveMagCalibrationResult(const MagCalResult& result) {
  PersistedCalibration cfg{};
  if (!loadCalibrationConfig(cfg)) {
    initDefaultCalibration(cfg);
  }
  cfg.magHardIronMinX = result.minX;
  cfg.magHardIronMaxX = result.maxX;
  cfg.magHardIronMinY = result.minY;
  cfg.magHardIronMaxY = result.maxY;
  cfg.magHardIronMinZ = result.minZ;
  cfg.magHardIronMaxZ = result.maxZ;
  cfg.magHardIronOffX = result.offX;
  cfg.magHardIronOffY = result.offY;
  cfg.magHardIronOffZ = result.offZ;
  cfg.magHardIronConfidence = result.confidence;
  cfg.magHardIronValid = 1;
  saveCalibrationConfig(cfg);
}

void loadCalibrationAtBoot() {
  PersistedCalibration cfg{};
  if (!loadCalibrationConfig(cfg)) {
    imu_fusion::setAccelScale(1.0f);
    Serial.println("CAL LOAD: none");
    return;
  }

  imu_fusion::setAccelGyroOffsets(cfg.accelGyroOffsets);
  imu_fusion::setAccelScale(cfg.accelScale);
  Serial.printf("IMU CAL LOADED: accBias=(%.4f, %.4f, %.4f) gyroBias=(%.4f, %.4f, %.4f) accScale=%.6f\r\n",
                (double)cfg.accelGyroOffsets[0], (double)cfg.accelGyroOffsets[1], (double)cfg.accelGyroOffsets[2],
                (double)cfg.accelGyroOffsets[3], (double)cfg.accelGyroOffsets[4], (double)cfg.accelGyroOffsets[5],
                (double)cfg.accelScale);
  if (cfg.magHardIronValid) {
    imu_fusion::setHardIronOffset(cfg.magHardIronOffX, cfg.magHardIronOffY, cfg.magHardIronOffZ);
    Serial.printf("MAG CAL LOADED: off=(%.3f, %.3f, %.3f), conf=%.2f\r\n",
                  (double)cfg.magHardIronOffX, (double)cfg.magHardIronOffY, (double)cfg.magHardIronOffZ,
                  (double)cfg.magHardIronConfidence);
  }
}

void printCommandHelp() {
  Serial.println("FAST COMMANDS:");
  Serial.println("  help / h   - show this command list");
  Serial.println("  calmag     - start magnetometer calibration");
  Serial.println("  calimu     - start accel/gyro calibration");
  Serial.println("  teensyloopback - run Serial3 TX/RX loopback test");
  Serial.println("  gethdg     - print heading monitor once per second");
  Serial.println("  showyawcmp - print raw gyro/raw mag/corrected mag/fusion together");
  Serial.println("  showimucfg - print live accel/gyro config");
  Serial.println("  setimucfg  - set accel/gyro config (see usage)");
  Serial.println("  showimudata- print corrected accel/gyro data");
  Serial.println("  showimuerror- FAST/BIAS IMU noise+bias stats");
  Serial.println("  testimurot - 2s still bias + rotate Y-axis test");
  Serial.println("  espcomtest - poll Serial3 for ESPTEST_ACK handshake");
  Serial.println("  spdtest    - stream benchmark line at 100 Hz");
  Serial.println("  showcrsfin - show CRSF RX frame stats");
  Serial.println("  x          - exit active mode");
  Serial.println("  stats      - start 2Hz summary stream");
  Serial.println("  zero       - zero attitude state");
}

void printImuConfig() {
  imu_fusion::ImuConfig cfg{};
  float accHz = 0.0f, gyrHz = 0.0f;
  float lsbPerDps = 0.0f, dpsPerLsb = 0.0f;
  const float accScale = imu_fusion::getAccelScale();
  float bgx = 0.0f, bgy = 0.0f, bgz = 0.0f;
  float bax = 0.0f, bay = 0.0f, baz = 0.0f;
  imu_fusion::getGyroBiasDps(bgx, bgy, bgz);
  imu_fusion::getAccelBiasMps2(bax, bay, baz);
  if (!imu_fusion::getImuConfig(cfg) || !imu_fusion::getImuSampleRates(accHz, gyrHz)) {
    Serial.println("IMU CFG read failed");
    return;
  }
  imu_fusion::getGyroScale(lsbPerDps, dpsPerLsb);
  const float expectedLsb = expectedGyroLsbPerDps(cfg.gyrRange);
  const bool scaleMatch = !isnan(expectedLsb) && fabsf(expectedLsb - lsbPerDps) < 1e-3f;

  Serial.printf("IMU CFG ACC odr=0x%02X (~%.2fHz) range=0x%02X(%s) bwp=0x%02X perf=%u\r\n",
                (unsigned)cfg.accOdr, (double)accHz, (unsigned)cfg.accRange, accRangeLabel(cfg.accRange),
                (unsigned)cfg.accBwp, (unsigned)cfg.accFilterPerf);
  Serial.printf("IMU CFG GYR odr=0x%02X (~%.2fHz) gyro_range_cfg=0x%02X(%s) bwp=0x%02X noise=%u perf=%u lsb_per_dps=%.3f dps_per_lsb=%.6f scale_match=%u\r\n",
                (unsigned)cfg.gyrOdr, (double)gyrHz, (unsigned)cfg.gyrRange, gyrRangeLabel(cfg.gyrRange),
                (unsigned)cfg.gyrBwp, (unsigned)cfg.gyrNoisePerf, (unsigned)cfg.gyrFilterPerf,
                (double)lsbPerDps, (double)dpsPerLsb, (unsigned)(scaleMatch ? 1U : 0U));
  Serial.printf("IMU CAL acc_scale=%.6f\r\n", (double)accScale);
  Serial.printf("IMU CAL accel_bias_mps2=(%+.4f, %+.4f, %+.4f) gyro_bias_dps=(%+.4f, %+.4f, %+.4f)\r\n",
                (double)bax, (double)bay, (double)baz, (double)bgx, (double)bgy, (double)bgz);
}

void printSetImuCfgUsage() {
  Serial.println("setimucfg usage:");
  Serial.println("  setimucfg acc_hz=<25|50|100|200|400|800|1600> acc_range=<2|4|8|16> gyr_hz=<25|50|100|200|400|800|1600|3200> gyr_range=<125|250|500|1000|2000>");
  Serial.println("  optional raw overrides: acc_odr=<hex/dec> gyr_odr=<hex/dec> acc_bwp=<n> gyr_bwp=<n> acc_perf=<0|1> gyr_noise=<0|1> gyr_perf=<0|1>");
}

bool applySetImuCfg(const char* cmd) {
  imu_fusion::ImuConfig cfg{};
  if (!imu_fusion::getImuConfig(cfg)) return false;

  uint32_t v = 0;
  bool ok = true;

  if (parseKeyU32(cmd, "acc_hz", v)) {
    cfg.accOdr = mapAccOdrFromHz(v, ok);
    if (!ok) return false;
  }
  if (parseKeyU32(cmd, "gyr_hz", v)) {
    cfg.gyrOdr = mapGyrOdrFromHz(v, ok);
    if (!ok) return false;
  }
  if (parseKeyU32(cmd, "acc_range", v)) {
    cfg.accRange = mapAccRangeFromG(v, ok);
    if (!ok) return false;
  }
  if (parseKeyU32(cmd, "gyr_range", v)) {
    cfg.gyrRange = mapGyrRangeFromDps(v, ok);
    if (!ok) return false;
  }

  if (parseKeyU32(cmd, "acc_odr", v)) cfg.accOdr = (uint8_t)v;
  if (parseKeyU32(cmd, "gyr_odr", v)) cfg.gyrOdr = (uint8_t)v;
  if (parseKeyU32(cmd, "acc_bwp", v)) cfg.accBwp = (uint8_t)v;
  if (parseKeyU32(cmd, "gyr_bwp", v)) cfg.gyrBwp = (uint8_t)v;
  if (parseKeyU32(cmd, "acc_perf", v)) cfg.accFilterPerf = (uint8_t)v;
  if (parseKeyU32(cmd, "gyr_noise", v)) cfg.gyrNoisePerf = (uint8_t)v;
  if (parseKeyU32(cmd, "gyr_perf", v)) cfg.gyrFilterPerf = (uint8_t)v;

  return imu_fusion::setImuConfig(cfg);
}

void setMode(CommandMode mode) {
  g_mode = mode;
  g_mode_start_ms = millis();
  g_mode_last_print_ms = 0;

  if (mode != CommandMode::CalImu) g_cal_imu.active = false;
  if (mode != CommandMode::CalMag) g_cal_mag.active = false;
  if (mode != CommandMode::EspComTest) g_esp_com_test.reset();
  if (mode != CommandMode::SpdTest) {
    g_spdtest_next_us = 0;
    g_spdtest_sent = 0;
    g_spdtest_drop = 0;
  }

  switch (g_mode) {
    case CommandMode::Idle:
      Serial.println("MODE IDLE");
      break;
    case CommandMode::CalMag:
      Serial.println("CALMAG START");
      Serial.println("Rotate sensor through all orientations.");
      break;
    case CommandMode::CalImu:
      Serial.println("CALIMU START");
      Serial.println("Keep unit stationary.");
      break;
    case CommandMode::GetHdg:
      Serial.println("GETHDG START");
      break;
    case CommandMode::ShowYawCmp:
      Serial.println("SHOWYAWCMP START");
      Serial.println("YAWCMP\tgx_raw\tgy_raw\tgz_raw\tmx_raw\tmy_raw\tmz_raw\tmx_corr\tmy_corr\tmz_corr\traw_hdg\tfusion_roll\tfusion_pitch\tfusion_yaw\tfusion_hdg_col\tfusion_hdg_row\tfusion_hdg_tc\tmag_heading\tmagE_body_hdg\tmagE_fusion_hdg");
      break;
    case CommandMode::SpdTest:
      Serial.println("SPDTEST START");
      break;
    case CommandMode::ShowCrsfIn:
      Serial.println("SHOWCRSFIN START");
      break;
    case CommandMode::ShowImuData:
      Serial.println("SHOWIMUDATA START");
      Serial.println("IMU\tax_g\t\tay_g\t\taz_g\t\tgx_dps\t\tgy_dps\t\tgz_dps");
      break;
    case CommandMode::ShowImuError:
      g_imu_err.resetAll();
      Serial.println("SHOWIMUERROR START");
      Serial.println("IMUFAST\tN\t|g|corr_mean\t|g|corr_std\t|g|corr-1\tgx_raw_dps\tgy_raw_dps\tgz_raw_dps\tgx_corr_dps\tgy_corr_dps\tgz_corr_dps\tbias_x_dps\tbias_y_dps\tbias_z_dps\tgy_raw_counts\tdps_per_lsb\twmean_dps\twstd_dps\ttemp_C\tdt_us_mean\tdt_us_min\tdt_us_max\thz_est");
      Serial.println("IMUBIAS\tN\tok\t|g|corr_mean\t|g|corr_std\tg_scale_err\tgx_raw_dps\tgy_raw_dps\tgz_raw_dps\tgx_corr_dps\tgy_corr_dps\tgz_corr_dps\tbias_x_dps\tbias_y_dps\tbias_z_dps\tgy_raw_counts\tdps_per_lsb\t|bias|_dps\tdrift_deg_min\ttemp_C");
      break;
    case CommandMode::TestImuRot:
      g_rot_test.reset();
      Serial.println("TESTIMUROT START");
      Serial.println("Hold still for 2s, then rotate around Y axis about 90 deg in ~1s and stop.");
      break;
    case CommandMode::EspComTest:
      g_esp_com_test.reset();
      g_esp_com_test.active = true;
      g_esp_com_test.startMs = millis();
      Serial.println("ESPCOMTEST START");
      Serial.println("Waiting for ESPTEST_ACK on Serial3 (mirror link).");
      Serial.println("Press 'x' to abort.");
      break;
  }
}

void normalizeCommand(char* s) {
  if (!s) return;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
    s[--n] = '\0';
  }
  size_t start = 0;
  while (s[start] == ' ' || s[start] == '\t') start++;
  if (start > 0) memmove(s, s + start, n - start + 1);
  for (size_t i = 0; s[i] != '\0'; ++i) s[i] = (char)tolower((unsigned char)s[i]);
}

void processCommand(const char* cmd) {
  if (!cmd || cmd[0] == '\0') return;
  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0) {
    printCommandHelp();
  } else if (strcmp(cmd, "stats") == 0) {
    g_stats_streaming = true;
    g_summary_timer_us = 0;
    Serial.println("STATS STREAM START (2Hz)");
  } else if (strcmp(cmd, "zero") == 0) {
    g_state.roll = 0.0f;
    g_state.pitch = 0.0f;
    g_state.yaw = 0.0f;
    Serial.println("attitude zeroed");
  } else if (strcmp(cmd, "x") == 0) {
    if (g_stats_streaming) {
      g_stats_streaming = false;
      Serial.println("STATS STREAM STOP");
    }
    if (g_mode == CommandMode::CalMag) {
      g_mag_cal.stopByUser();
      return;
    }
    if (g_mode == CommandMode::CalMag) Serial.println("CALMAG EXIT");
    if (g_mode == CommandMode::CalImu) Serial.println("CALIMU EXIT");
    if (g_mode == CommandMode::GetHdg) Serial.println("GETHDG EXIT");
    if (g_mode == CommandMode::ShowYawCmp) Serial.println("SHOWYAWCMP EXIT");
    if (g_mode == CommandMode::SpdTest) Serial.println("SPDTEST EXIT");
    if (g_mode == CommandMode::ShowCrsfIn) Serial.println("SHOWCRSFIN EXIT");
    if (g_mode == CommandMode::ShowImuData) Serial.println("SHOWIMUDATA EXIT");
    if (g_mode == CommandMode::ShowImuError) Serial.println("SHOWIMUERROR EXIT");
    if (g_mode == CommandMode::TestImuRot) Serial.println("TESTIMUROT EXIT");
    if (g_mode == CommandMode::EspComTest) Serial.println("ESPCOMTEST EXIT");
    setMode(CommandMode::Idle);
  } else if (strcmp(cmd, "calmag") == 0) {
    setMode(CommandMode::CalMag);
  } else if (strcmp(cmd, "calimu") == 0) {
    setMode(CommandMode::CalImu);
  } else if (strcmp(cmd, "teensyloopback") == 0) {
    runTeensyLoopbackTest();
  } else if (strcmp(cmd, "gethdg") == 0) {
    setMode(CommandMode::GetHdg);
  } else if (strcmp(cmd, "showyawcmp") == 0) {
    setMode(CommandMode::ShowYawCmp);
  } else if (strcmp(cmd, "spdtest") == 0) {
    setMode(CommandMode::SpdTest);
  } else if (strcmp(cmd, "showcrsfin") == 0) {
    setMode(CommandMode::ShowCrsfIn);
  } else if (strcmp(cmd, "showimucfg") == 0) {
    printImuConfig();
  } else if (startsWith(cmd, "setimucfg")) {
    const bool hasArgs = (strlen(cmd) > strlen("setimucfg"));
    if (!hasArgs) {
      printSetImuCfgUsage();
      return;
    }
    if (!applySetImuCfg(cmd)) {
      Serial.println("setimucfg failed (invalid value or IMU write error)");
      printSetImuCfgUsage();
      return;
    }
    Serial.println("setimucfg applied");
    printImuConfig();
  } else if (strcmp(cmd, "showimudata") == 0) {
    setMode(CommandMode::ShowImuData);
  } else if (strcmp(cmd, "showimuerror") == 0) {
    setMode(CommandMode::ShowImuError);
  } else if (strcmp(cmd, "testimurot") == 0) {
    setMode(CommandMode::TestImuRot);
  } else if (strcmp(cmd, "espcomtest") == 0) {
    setMode(CommandMode::EspComTest);
  } else {
    Serial.print("unknown cmd: ");
    Serial.println(cmd);
  }
}

void runTeensyLoopbackTest() {
#if !ENABLE_MIRROR
  Serial.println("TEENSYLOOPBACK ERROR: mirror serial disabled in build");
  return;
#else
  constexpr uint32_t kTimeoutMs = 120;
  constexpr size_t kLen = 32;
  uint8_t tx[kLen];
  uint8_t rx[kLen];
  memset(rx, 0, sizeof(rx));

  // Deterministic non-trivial payload for byte-level verification.
  for (size_t i = 0; i < kLen; ++i) {
    tx[i] = (uint8_t)((0xA5U + (uint8_t)(i * 17U)) ^ 0x5CU);
  }

  while (MIRROR_SERIAL.available() > 0) {
    (void)MIRROR_SERIAL.read();
  }
  MIRROR_SERIAL.clear();

  Serial.printf("TEENSYLOOPBACK START serial3=%lu tx=%u rx=%u len=%u timeout_ms=%lu\r\n",
                (unsigned long)MIRROR_BAUD,
                (unsigned)MIRROR_TX_PIN,
                (unsigned)MIRROR_RX_PIN,
                (unsigned)kLen,
                (unsigned long)kTimeoutMs);

  const size_t sent = MIRROR_SERIAL.write(tx, kLen);
  MIRROR_SERIAL.flush();
  if (sent != kLen) {
    Serial.printf("TEENSYLOOPBACK FAIL write sent=%u expected=%u\r\n",
                  (unsigned)sent, (unsigned)kLen);
    return;
  }

  size_t got = 0;
  const uint32_t t0 = millis();
  while (got < kLen && (uint32_t)(millis() - t0) < kTimeoutMs) {
    while (MIRROR_SERIAL.available() > 0 && got < kLen) {
      const int c = MIRROR_SERIAL.read();
      if (c >= 0) rx[got++] = (uint8_t)c;
    }
  }

  uint32_t mismatches = 0;
  for (size_t i = 0; i < got && i < kLen; ++i) {
    if (rx[i] != tx[i]) mismatches++;
  }

  if (got == kLen && mismatches == 0) {
    Serial.printf("TEENSYLOOPBACK PASS sent=%u recv=%u mismatches=0 elapsed_ms=%lu\r\n",
                  (unsigned)sent, (unsigned)got, (unsigned long)(millis() - t0));
    return;
  }

  Serial.printf("TEENSYLOOPBACK FAIL sent=%u recv=%u mismatches=%lu elapsed_ms=%lu\r\n",
                (unsigned)sent, (unsigned)got, (unsigned long)mismatches, (unsigned long)(millis() - t0));
  const size_t dumpN = (got < kLen) ? got : kLen;
  Serial.print("TX:");
  for (size_t i = 0; i < kLen; ++i) Serial.printf(" %02X", tx[i]);
  Serial.println();
  Serial.print("RX:");
  for (size_t i = 0; i < dumpN; ++i) Serial.printf(" %02X", rx[i]);
  Serial.println();
#endif
}

void processCommandStream(Stream& in, char* line, uint8_t& idx, size_t lineLen) {
  while (in.available() > 0) {
    const char c = (char)in.read();
    if (c == '\r' || c == '\n') {
      if (idx == 0) continue;
      line[idx] = '\0';
      idx = 0;
      normalizeCommand(line);
      processCommand(line);
    } else if ((size_t)idx + 1U < lineLen) {
      line[idx++] = c;
    }
  }
}

void handleCommandInputs() {
  static char usbLine[64];
  static uint8_t usbIdx = 0;
  processCommandStream(Serial, usbLine, usbIdx, sizeof(usbLine));
}

void gpsPollContinuous() {
  gps_ubx::poll(g_state);
}

void baroPollAndFilter() {
  if (!g_baro_ok) return;
  const uint32_t now = millis();
  const float temp_c = g_bmp.readTemperature();
  const float press_hpa = g_bmp.readPressure() / 100.0f;
  const float alt_raw_m = g_bmp.readAltitude(BARO_QNH_HPA);
  if (isnan(temp_c) || isnan(press_hpa) || isnan(alt_raw_m)) return;

  g_state.baro_temp_c = temp_c;
  g_state.baro_press_hpa = press_hpa;
  if (g_state.last_baro_ms == 0 || isnan(g_state.baro_alt_m) || isnan(g_last_baro_alt_m)) {
    g_state.baro_alt_m = alt_raw_m;
    g_last_baro_alt_m = g_state.baro_alt_m;
    g_state.baro_vsi_mps = 0.0f;
    g_state.last_baro_ms = now;
    return;
  }

  const uint32_t dt_ms = now - g_state.last_baro_ms;
  if (dt_ms < 20U) return;
  const float dt = (float)dt_ms / 1000.0f;
  const float alphaAlt = dt / (BARO_ALT_TAU_S + dt);
  const float alphaVsi = dt / (BARO_VSI_TAU_S + dt);
  g_state.baro_alt_m += alphaAlt * (alt_raw_m - g_state.baro_alt_m);
  const float vsi_raw = (g_state.baro_alt_m - g_last_baro_alt_m) / dt;
  g_state.baro_vsi_mps += alphaVsi * (vsi_raw - g_state.baro_vsi_mps);
  g_last_baro_alt_m = g_state.baro_alt_m;
  g_state.last_baro_ms = now;
}

void mirrorSendFastState() {
#if ENABLE_MIRROR
  if (g_mode == CommandMode::EspComTest) return;
  const uint32_t seq = g_mirror_seq++;
  const uint32_t t_us = micros();
  const bool ok = mirror::sendFastState(g_state, seq, t_us);
  if (ok) {
    g_last_mirror_seq = seq;
    g_last_mirror_t_us = t_us;
  }
  if (ok) g_state.mirror_tx_ok++;
  else g_state.mirror_drop_count++;
#endif
}

void serviceCommandMode() {
  const uint32_t now = millis();
  switch (g_mode) {
    case CommandMode::Idle:
      return;

    case CommandMode::CalImu: {
      if (!g_cal_imu.active) {
        g_cal_imu.active = true;
        g_cal_imu.sample_count = 0;
        memset(g_cal_imu.sum, 0, sizeof(g_cal_imu.sum));
      }
      float sample[6];
      if (imu_fusion::readRawAccelGyro(sample)) {
        for (int i = 0; i < 6; ++i) g_cal_imu.sum[i] += sample[i];
        g_cal_imu.sample_count++;
      }
      if (g_mode_last_print_ms == 0 || (uint32_t)(now - g_mode_last_print_ms) >= 500U) {
        g_mode_last_print_ms = now;
        Serial.printf("CALIMU running samples=%lu elapsed=%lums\r\n",
                      (unsigned long)g_cal_imu.sample_count,
                      (unsigned long)(now - g_mode_start_ms));
      }
      if (g_cal_imu.sample_count >= 500U) {
        float offsets[6];
        for (int i = 0; i < 6; ++i) offsets[i] = (float)(g_cal_imu.sum[i] / (double)g_cal_imu.sample_count);
        const float accel_norm = sqrtf(offsets[0] * offsets[0] + offsets[1] * offsets[1] + offsets[2] * offsets[2]);
        const float gravity_ref = (accel_norm > 1.0f) ? accel_norm : 9.80665f;
        const float accel_scale = (accel_norm > 1.0f) ? (kGravityMps2 / accel_norm) : 1.0f;
        // Store accel Z as bias (not absolute gravity reading) so reload is unambiguous.
        offsets[2] -= gravity_ref;
        imu_fusion::setAccelGyroOffsets(offsets);
        imu_fusion::setAccelScale(accel_scale);
        PersistedCalibration cfg{};
        if (!loadCalibrationConfig(cfg)) {
          initDefaultCalibration(cfg);
        }
        for (int i = 0; i < 6; ++i) cfg.accelGyroOffsets[i] = offsets[i];
        cfg.accelScale = accel_scale;
        saveCalibrationConfig(cfg);
        Serial.printf("CALIMU RESULT accBias=(%.4f, %.4f, %.4f) gyroBias=(%.4f, %.4f, %.4f) gRef=%.4f accScale=%.6f\r\n",
                      (double)offsets[0], (double)offsets[1], (double)offsets[2],
                      (double)offsets[3], (double)offsets[4], (double)offsets[5],
                      (double)gravity_ref, (double)accel_scale);
        Serial.println("Calibration complete.");
        Serial.println("CALIMU EXIT");
        setMode(CommandMode::Idle);
      }
      return;
    }

    case CommandMode::CalMag: {
      if (!g_cal_mag.active) {
        g_cal_mag.active = true;
        g_cal_mag.last_print_ms = 0;
        g_mag_cal.start();
        Serial.println("Calibration auto-stops at confidence >= 0.90 or press 'x' to stop.");
      }
      float mx, my, mz;
      if (imu_fusion::readRawMag(mx, my, mz)) {
        g_mag_cal.update(mx, my, mz);
      }
      const MagCalResult r = g_mag_cal.getResult();
      imu_fusion::setHardIronOffset(r.offX, r.offY, r.offZ);

      if (g_cal_mag.last_print_ms == 0 || (uint32_t)(now - g_cal_mag.last_print_ms) >= 150U) {
        g_cal_mag.last_print_ms = now;
        Serial.printf("CALMAG t=%.1fs n=%lu\r\n", (double)(r.elapsedMs / 1000.0f), (unsigned long)r.sampleCount);
        Serial.printf("min: X=% .3f Y=% .3f Z=% .3f\r\n", (double)r.minX, (double)r.minY, (double)r.minZ);
        Serial.printf("max: X=% .3f Y=% .3f Z=% .3f\r\n", (double)r.maxX, (double)r.maxY, (double)r.maxZ);
        Serial.printf("off: X=% .3f Y=% .3f Z=% .3f\r\n", (double)r.offX, (double)r.offY, (double)r.offZ);
        Serial.printf("rng: X=% .3f Y=% .3f Z=% .3f\r\n", (double)r.rngX, (double)r.rngY, (double)r.rngZ);
        Serial.printf("conf=%.2f  (press 'x' to stop)\r\n", (double)r.confidence);
      }

      if (g_mag_cal.isDone()) {
        saveMagCalibrationResult(r);
        Serial.println("CALMAG COMPLETE");
        Serial.printf("off: X=% .3f Y=% .3f Z=% .3f\r\n", (double)r.offX, (double)r.offY, (double)r.offZ);
        Serial.printf("confidence=%.2f\r\n", (double)r.confidence);
        Serial.println("SAVED to EEPROM");
        Serial.println("CALMAG EXIT");
        setMode(CommandMode::Idle);
      }
      return;
    }

    case CommandMode::GetHdg: {
      if (g_mode_last_print_ms == 0 || (uint32_t)(now - g_mode_last_print_ms) >= 1000U) {
        g_mode_last_print_ms = now;
        float mx = 0, my = 0, mz = 0;
        imu_fusion::getMagHeadingInputs(mx, my, mz);
        const float raw = imu_fusion::computeHeadingDeg(mx, my);
        Serial.printf("GETHDG raw=%.1f deg fusion=%.1f deg mag_heading=%.1f deg mag_in=(%.3f, %.3f, %.3f)\r\n",
                      (double)raw, (double)g_state.yaw, (double)g_state.mag_heading,
                      (double)mx, (double)my, (double)mz);
      }
      return;
    }

    case CommandMode::ShowYawCmp: {
      if (g_mode_last_print_ms == 0 || (uint32_t)(now - g_mode_last_print_ms) >= 100U) {
        g_mode_last_print_ms = now;
        float raw6[6] = {0, 0, 0, 0, 0, 0};
        float mxRaw = 0.0f, myRaw = 0.0f, mzRaw = 0.0f;
        float mxCorr = 0.0f, myCorr = 0.0f, mzCorr = 0.0f;
        float fusionYawEuler = 0.0f;
        float fusionHeadingCol = 0.0f;
        float fusionHeadingRow = 0.0f;
        float fusionHeadingTc = 0.0f;
        imu_fusion::FusionMagDebug magDebug{};
        const bool haveGyro = imu_fusion::readRawAccelGyro(raw6);
        const bool haveMagRaw = imu_fusion::readRawMag(mxRaw, myRaw, mzRaw);
        imu_fusion::getMagHeadingInputs(mxCorr, myCorr, mzCorr);
        imu_fusion::getFusionHeadingDebug(fusionYawEuler, fusionHeadingCol, fusionHeadingRow, fusionHeadingTc);
        imu_fusion::getFusionMagDebug(magDebug);
        const float rawHeading = imu_fusion::computeHeadingDeg(mxCorr, myCorr);
        Serial.printf("YAWCMP\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\r\n",
                      (double)(haveGyro ? raw6[3] : NAN),
                      (double)(haveGyro ? raw6[4] : NAN),
                      (double)(haveGyro ? raw6[5] : NAN),
                      (double)(haveMagRaw ? mxRaw : NAN),
                      (double)(haveMagRaw ? myRaw : NAN),
                      (double)(haveMagRaw ? mzRaw : NAN),
                      (double)mxCorr,
                      (double)myCorr,
                      (double)mzCorr,
                      (double)rawHeading,
                      (double)g_state.roll,
                      (double)g_state.pitch,
                      (double)fusionYawEuler,
                      (double)fusionHeadingCol,
                      (double)fusionHeadingRow,
                      (double)fusionHeadingTc,
                      (double)g_state.mag_heading,
                      (double)magDebug.earthFromBodyHeading,
                      (double)magDebug.earthFromFusionHeading);
      }
      return;
    }

    case CommandMode::SpdTest:
      {
        const uint32_t now_us = micros();
        if (g_spdtest_next_us == 0U) {
          g_spdtest_next_us = now_us;
          g_mode_last_print_ms = now;
        }

        while ((int32_t)(now_us - g_spdtest_next_us) >= 0) {
          char line[64];
          const int n = snprintf(line, sizeof(line), "SPDTEST t_us=%lu rssi_dbm=NA\r\n",
                                 (unsigned long)g_spdtest_next_us);
          if (n > 0) {
            if (writeLineNonBlocking(Serial, line)) g_spdtest_sent++;
            else g_spdtest_drop++;
          }
          g_spdtest_next_us += 10000U;  // 100 Hz
        }

        if ((uint32_t)(now - g_mode_last_print_ms) >= 1000U) {
          g_mode_last_print_ms = now;
          char status[80];
          const int n = snprintf(status, sizeof(status), "SPDTEST rate=100Hz sent=%lu drop=%lu\r\n",
                                 (unsigned long)g_spdtest_sent, (unsigned long)g_spdtest_drop);
          if (n > 0) (void)writeLineNonBlocking(Serial, status);
          g_spdtest_sent = 0;
          g_spdtest_drop = 0;
        }
      }
      return;

    case CommandMode::ShowCrsfIn:
      if (g_mode_last_print_ms == 0 || (uint32_t)(now - g_mode_last_print_ms) >= 1000U) {
        g_mode_last_print_ms = now;
        CrsfRxStats st{};
        telemetry_getCrsfRxStats(st);
        const uint32_t age = (st.lastFrameMs == 0) ? 0xFFFFFFFFUL : (uint32_t)(now - st.lastFrameMs);
        Serial.printf("CRSF IN bytes=%lu frames=%lu rc16=%lu lastType=0x%02X age=%lums\r\n",
                      (unsigned long)st.rxBytes,
                      (unsigned long)st.rxFrames,
                      (unsigned long)st.rxRcFrames,
                      (unsigned)st.lastType,
                      (unsigned long)age);
      }
      return;

    case CommandMode::ShowImuData: {
      if (g_mode_last_print_ms == 0 || (uint32_t)(now - g_mode_last_print_ms) >= 100U) {
        g_mode_last_print_ms = now;
        float d[6];
        if (imu_fusion::readCorrectedAccelGyro(d)) {
          Serial.printf("IMU\t%+10.6f\t%+10.6f\t%+10.6f\t%+10.6f\t%+10.6f\t%+10.6f\r\n",
                        (double)d[0], (double)d[1], (double)d[2],
                        (double)d[3], (double)d[4], (double)d[5]);
        } else {
          Serial.println("IMU read failed");
        }
      }
      return;
    }

    case CommandMode::ShowImuError: {
      const uint32_t nowUs = micros();
      if (g_imu_err.nextSampleUs == 0U) {
        g_imu_err.nextSampleUs = nowUs;
      }
      if ((int32_t)(nowUs - g_imu_err.nextSampleUs) < 0) {
        return;
      }
      g_imu_err.nextSampleUs += kImuDiagPeriodUs;

      float raw[6];
      if (imu_fusion::readRawAccelGyro(raw)) {
        const uint32_t sampleUs = micros();
        if (g_imu_err.lastSampleUs != 0U) {
          const uint32_t dtUs = (uint32_t)(sampleUs - g_imu_err.lastSampleUs);
          g_imu_err.dtUs.push((double)dtUs);
          if (dtUs < g_imu_err.dtMinUs) g_imu_err.dtMinUs = dtUs;
          if (dtUs > g_imu_err.dtMaxUs) g_imu_err.dtMaxUs = dtUs;
        }
        g_imu_err.lastSampleUs = sampleUs;

        float tempC = g_imu_err.lastTempC;
        if (g_imu_err.lastTempReadMs == 0U || (uint32_t)(now - g_imu_err.lastTempReadMs) >= 5000U) {
          float t = NAN;
          if (imu_fusion::readTemperatureC(t)) {
            g_imu_err.lastTempC = t;
            tempC = t;
          }
          g_imu_err.lastTempReadMs = now;
        }
        float bx = 0.0f, by = 0.0f, bz = 0.0f;
        float abx = 0.0f, aby = 0.0f, abz = 0.0f;
        imu_fusion::getGyroBiasDps(bx, by, bz);
        imu_fusion::getAccelBiasMps2(abx, aby, abz);
        float lsbPerDps = 0.0f, dpsPerLsb = 0.0f;
        imu_fusion::getGyroScale(lsbPerDps, dpsPerLsb);
        g_imu_err.biasGxDps = bx;
        g_imu_err.biasGyDps = by;
        g_imu_err.biasGzDps = bz;

        const double accScale = (double)imu_fusion::getAccelScale();
        const double axg = (((double)raw[0] - (double)abx) * accScale) / (double)kGravityMps2;
        const double ayg = (((double)raw[1] - (double)aby) * accScale) / (double)kGravityMps2;
        const double azg = (((double)raw[2] - (double)abz) * accScale) / (double)kGravityMps2;
        const double gxRaw = raw[3], gyRaw = raw[4], gzRaw = raw[5];
        const double gx = gxRaw - (double)bx;
        const double gy = gyRaw - (double)by;
        const double gz = gzRaw - (double)bz;
        const int16_t gyRawCount = (int16_t)lround(gyRaw / (double)dpsPerLsb);
        const double gnorm = sqrt(axg * axg + ayg * ayg + azg * azg);
        const double wmag = sqrt(gx * gx + gy * gy + gz * gz);
        const bool stationary = (fabs(gnorm - 1.0) < (double)kStationaryGAbs) && (wmag < (double)kStationaryWMaxDps);

        // FAST window: always accumulate.
        g_imu_err.fast.push(gnorm, gxRaw, gyRaw, gzRaw, gx, gy, gz, wmag, tempC, gyRawCount);
        if (g_imu_err.fast.n >= kImuFastN) {
          const double gMean = g_imu_err.fast.gnorm.mean;
          const double gStd = g_imu_err.fast.gnorm.stddev();
          const double dtMeanUs = g_imu_err.dtUs.n ? g_imu_err.dtUs.mean : NAN;
          const uint32_t dtMinUs = (g_imu_err.dtMinUs == UINT32_MAX) ? 0U : g_imu_err.dtMinUs;
          const uint32_t dtMaxUs = g_imu_err.dtMaxUs;
          // hz_est reflects scheduled diagnostics cadence, not BMI DRDY confirmation.
          const double hzEst = (dtMeanUs > 0.0) ? (1000000.0 / dtMeanUs) : NAN;
          (void)g_logger.enqueuef("IMUFAST\t%lu\t%.6f\t%.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%.1f\t%.6f\t%.6f\t%.6f\t%.2f\t%.1f\t%lu\t%lu\t%.1f\r\n",
                                  (unsigned long)g_imu_err.fast.n,
                                  gMean, gStd, gMean - 1.0,
                                  g_imu_err.fast.gxRaw.mean, g_imu_err.fast.gyRaw.mean, g_imu_err.fast.gzRaw.mean,
                                  g_imu_err.fast.gxCorr.mean, g_imu_err.fast.gyCorr.mean, g_imu_err.fast.gzCorr.mean,
                                  (double)g_imu_err.biasGxDps, (double)g_imu_err.biasGyDps, (double)g_imu_err.biasGzDps,
                                  g_imu_err.fast.gyRawCount.mean, (double)dpsPerLsb,
                                  g_imu_err.fast.wmag.mean, g_imu_err.fast.wmag.stddev(),
                                  g_imu_err.fast.tempC.n ? g_imu_err.fast.tempC.mean : NAN,
                                  dtMeanUs, (unsigned long)dtMinUs, (unsigned long)dtMaxUs, hzEst);
          g_imu_err.fast.reset();
          g_imu_err.dtUs.reset();
          g_imu_err.dtMinUs = UINT32_MAX;
          g_imu_err.dtMaxUs = 0;
        }

        // Continuous stationary settle timer before bias capture.
        if (stationary) {
          if (g_imu_err.stationarySinceMs == 0) g_imu_err.stationarySinceMs = now;
        } else {
          g_imu_err.stationarySinceMs = 0;
          if (g_imu_err.biasActive) g_imu_err.biasNonStationary++;
        }

        if (!g_imu_err.biasActive &&
            g_imu_err.stationarySinceMs != 0 &&
            (uint32_t)(now - g_imu_err.stationarySinceMs) >= kStationarySettleMs) {
          g_imu_err.startBiasWindow();
        }

        if (g_imu_err.biasActive) {
          g_imu_err.biasSeen++;
          if (stationary) {
            g_imu_err.bias.push(gnorm, gxRaw, gyRaw, gzRaw, gx, gy, gz, wmag, tempC, gyRawCount);
          } else {
            g_imu_err.biasNonStationary++;
          }

          if (g_imu_err.biasSeen >= kImuBiasN) {
            const double nonStatRatio = (double)g_imu_err.biasNonStationary / (double)g_imu_err.biasSeen;
            const double gStd = g_imu_err.bias.gnorm.stddev();
            const bool ok = (nonStatRatio <= (double)kBiasAbortNonStationaryRatio) &&
                            (g_imu_err.bias.n > 0) &&
                            (gStd <= (double)kBiasMaxGStd);
            if (ok) {
              const double bcx = g_imu_err.bias.gxCorr.mean;
              const double bcy = g_imu_err.bias.gyCorr.mean;
              const double bcz = g_imu_err.bias.gzCorr.mean;
              const double bmag = sqrt(bcx * bcx + bcy * bcy + bcz * bcz);
              const double drift_dpm = bmag * 60.0;
              const double gMean = g_imu_err.bias.gnorm.mean;
              (void)g_logger.enqueuef("IMUBIAS\t%lu\t1\t%.6f\t%.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%+.6f\t%.1f\t%.6f\t%.6f\t%.3f\t%.2f\r\n",
                                      (unsigned long)g_imu_err.biasSeen,
                                      gMean, gStd, gMean - 1.0,
                                      g_imu_err.bias.gxRaw.mean, g_imu_err.bias.gyRaw.mean, g_imu_err.bias.gzRaw.mean,
                                      g_imu_err.bias.gxCorr.mean, g_imu_err.bias.gyCorr.mean, g_imu_err.bias.gzCorr.mean,
                                      (double)g_imu_err.biasGxDps, (double)g_imu_err.biasGyDps, (double)g_imu_err.biasGzDps,
                                      g_imu_err.bias.gyRawCount.mean, (double)dpsPerLsb,
                                      bmag, drift_dpm,
                                      g_imu_err.bias.tempC.n ? g_imu_err.bias.tempC.mean : NAN);
            } else {
              (void)g_logger.enqueuef("IMUBIAS\t%lu\t0\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\r\n",
                                      (unsigned long)g_imu_err.biasSeen);
            }
            g_imu_err.endBiasWindow();
            // Require a new settle interval before next bias capture.
            g_imu_err.stationarySinceMs = stationary ? now : 0;
          }
        }
      }
      return;
    }

    case CommandMode::EspComTest: {
#if !ENABLE_MIRROR
      Serial.println("ESPCOMTEST ERROR: mirror serial disabled in build");
      setMode(CommandMode::Idle);
      return;
#else
      if (!g_esp_com_test.active) {
        g_esp_com_test.reset();
        g_esp_com_test.active = true;
        g_esp_com_test.startMs = now;
      }

      if (g_esp_com_test.lastProbeMs == 0U || (uint32_t)(now - g_esp_com_test.lastProbeMs) >= 1000U) {
        g_esp_com_test.lastProbeMs = now;
        g_esp_com_test.probesSent++;
        g_esp_com_test.probeSeq++;
        MIRROR_SERIAL.printf("ESPTEST? seq=%lu\r\n", (unsigned long)g_esp_com_test.probeSeq);
      }

      while (MIRROR_SERIAL.available() > 0) {
        const int c = MIRROR_SERIAL.read();
        if (c < 0) break;
        g_esp_com_test.rxBytes++;
        g_esp_com_test.lastRxMs = now;
        const char ch = (char)c;
        if (ch == '\r' || ch == '\n') {
          g_esp_com_test.line[g_esp_com_test.lineIdx] = '\0';
          if (strstr(g_esp_com_test.line, "ESPTEST_ACK") != nullptr) {
            g_esp_com_test.gotAck = true;
            break;
          }
          g_esp_com_test.lineIdx = 0;
          g_esp_com_test.line[0] = '\0';
          continue;
        }
        if (g_esp_com_test.lineIdx + 1U < sizeof(g_esp_com_test.line)) {
          g_esp_com_test.line[g_esp_com_test.lineIdx++] = ch;
          g_esp_com_test.line[g_esp_com_test.lineIdx] = '\0';
        }
      }

      if (g_esp_com_test.gotAck) {
        Serial.printf("ESPCOMTEST PASS probes=%lu rx_bytes=%lu elapsed_ms=%lu\r\n",
                      (unsigned long)g_esp_com_test.probesSent,
                      (unsigned long)g_esp_com_test.rxBytes,
                      (unsigned long)(now - g_esp_com_test.startMs));
        setMode(CommandMode::Idle);
        return;
      }

      if (g_esp_com_test.lastPrintMs == 0U || (uint32_t)(now - g_esp_com_test.lastPrintMs) >= 1000U) {
        g_esp_com_test.lastPrintMs = now;
        const uint32_t rxAgeMs = (g_esp_com_test.lastRxMs == 0U) ? 0xFFFFFFFFUL : (uint32_t)(now - g_esp_com_test.lastRxMs);
        Serial.printf("ESPCOMTEST waiting probes=%lu rx_bytes=%lu last_rx_age_ms=%lu\r\n",
                      (unsigned long)g_esp_com_test.probesSent,
                      (unsigned long)g_esp_com_test.rxBytes,
                      (unsigned long)rxAgeMs);
      }
      return;
#endif
    }

    case CommandMode::TestImuRot: {
      float raw[6];
      if (!imu_fusion::readRawAccelGyro(raw)) return;

      const uint32_t nowUs = micros();
      if (g_rot_test.lastUs == 0) g_rot_test.lastUs = nowUs;
      const float dt = (float)(nowUs - g_rot_test.lastUs) * 1.0e-6f;
      g_rot_test.lastUs = nowUs;

      const double wmag = sqrt((double)raw[3] * raw[3] + (double)raw[4] * raw[4] + (double)raw[5] * raw[5]);

      // Phase 1: still capture for 2 seconds (bias estimate).
      if (!g_rot_test.biasReady) {
        g_rot_test.settleSamples++;
        g_rot_test.sumX += raw[3];
        g_rot_test.sumY += raw[4];
        g_rot_test.sumZ += raw[5];
        if ((uint32_t)(now - g_mode_start_ms) >= 2000U && g_rot_test.settleSamples > 0) {
          const double invN = 1.0 / (double)g_rot_test.settleSamples;
          g_rot_test.biasX = (float)(g_rot_test.sumX * invN);
          g_rot_test.biasY = (float)(g_rot_test.sumY * invN);
          g_rot_test.biasZ = (float)(g_rot_test.sumZ * invN);
          g_rot_test.biasReady = true;
          g_mode_last_print_ms = now;
          (void)g_logger.enqueuef("TESTIMUROT bias_dps: x=%+.6f y=%+.6f z=%+.6f\r\n",
                                  (double)g_rot_test.biasX, (double)g_rot_test.biasY, (double)g_rot_test.biasZ);
          (void)g_logger.enqueue("Rotate around Y now (~90 deg), then stop.\r\n");
        }
        return;
      }

      const float gyCorr = raw[4] - g_rot_test.biasY;
      const bool moving = fabs((double)gyCorr) > 5.0;
      if (!g_rot_test.rotating && moving) {
        g_rot_test.rotating = true;
        g_rot_test.angleDeg = 0.0f;
        g_rot_test.stillSamples = 0;
        (void)g_logger.enqueue("TESTIMUROT motion detected, integrating...\r\n");
      }

      if (g_rot_test.rotating) {
        g_rot_test.angleDeg += gyCorr * dt;
        if (fabs((double)gyCorr) < 0.5 && wmag < 1.0) g_rot_test.stillSamples++;
        else g_rot_test.stillSamples = 0;

        if (g_rot_test.stillSamples >= 200) {  // ~0.5s still at 400Hz
          const float ang = g_rot_test.angleDeg;
          const float absAng = fabsf(ang);
          const float errPct = (absAng > 1e-3f) ? fabsf((absAng - 90.0f) / 90.0f) * 100.0f : 0.0f;
          (void)g_logger.enqueuef("TESTIMUROT result: angle_deg=%+.3f abs=%.3f err_vs_90=%.1f%%\r\n",
                                  (double)ang, (double)absAng, (double)errPct);
          (void)g_logger.enqueue("TESTIMUROT EXIT\r\n");
          setMode(CommandMode::Idle);
        }
      }
      return;
    }
  }
}

void printSummary2Hz() {
  const mirror::RxDebugStats mdbg = mirror::getRxDebugStats();

  Serial.printf(
      "STAT unit=TEENSY seq=%lu t_us=%lu has=1 ack=0 cmd=0 ack_ok=0 code=0 "
      "rx_bytes=%lu ok=%lu crc=%lu cobs=%lu len=%lu unk=%lu drop=%lu "
      "link_tx=0 link_rx=0 link_drop=0\r\n",
      (unsigned long)g_last_mirror_seq,
      (unsigned long)g_last_mirror_t_us,
      (unsigned long)mdbg.rxBytes,
      (unsigned long)mdbg.framesOk,
      (unsigned long)mdbg.crcErr,
      (unsigned long)mdbg.cobsErr,
      (unsigned long)mdbg.lenErr,
      (unsigned long)mdbg.unknownMsg,
      (unsigned long)0U);
}
}  // namespace

void setup() {
  Serial.begin(CONSOLE_BAUD);

  Serial.println("FAST Teensy Avionics");
  Serial.println("board=teensy40 fw=standalone");
#if ENABLE_MIRROR
  Serial.printf("mirror=enabled serial3=%lu tx=%u rx=%u\r\n",
                (unsigned long)MIRROR_BAUD, (unsigned)MIRROR_TX_PIN, (unsigned)MIRROR_RX_PIN);
#else
  Serial.println("mirror=disabled");
#endif
  Serial.printf("crsf=serial2 baud=%lu tx=%u rx=%u\r\n",
                (unsigned long)CRSF_BAUD, (unsigned)CRSF_TX_PIN, (unsigned)CRSF_RX_PIN);
  Serial.printf("gps=serial1 baud=%lu tx=%u rx=%u\r\n",
                (unsigned long)GPS_BAUD, (unsigned)GPS_TX_PIN, (unsigned)GPS_RX_PIN);
  Serial.printf("i2c=wire sda=%u scl=%u hz=%lu\r\n",
                (unsigned)I2C_SDA_PIN, (unsigned)I2C_SCL_PIN, (unsigned long)I2C_BUS_HZ);
  Serial.printf("loops: imu=400Hz(drdy) mirror=%uHz summary=2Hz\r\n", (unsigned)mirror::streamRateHz());

  Wire.setSDA(I2C_SDA_PIN);
  Wire.setSCL(I2C_SCL_PIN);
  Wire.begin();
  Wire.setClock(I2C_BUS_HZ);

  g_state.baro_temp_c = NAN;
  g_state.baro_press_hpa = NAN;
  g_state.baro_alt_m = NAN;
  g_state.baro_vsi_mps = NAN;
  g_baro_ok = g_bmp.begin(0x76) || g_bmp.begin(0x77);
  Serial.println(g_baro_ok ? "BARO BMP280: OK" : "BARO BMP280: not found");

  g_imu_ok = imu_fusion::begin(&Serial);
  Serial.println(g_imu_ok ? "IMU BMI270/BMM150: OK" : "IMU BMI270/BMM150: not found");
  if (g_imu_ok) {
    // Load persisted calibration first so printed config reflects active runtime calibration.
    loadCalibrationAtBoot();
    const bool fusionLoaded = imu_fusion::loadPersistedFusionSettings();
    printFusionSettingsSummary(fusionLoaded ? "loaded" : "default");
    printImuConfig();
  }

  gps_ubx::begin(GPS_SERIAL, GPS_BAUD);
  Serial.printf("GPS UBX parser: serial1 @ %lu\r\n", (unsigned long)GPS_BAUD);

  mirror::begin();
  telemetry_setup();
  printCommandHelp();

  g_summary_timer_us = 0;
  g_mirror_timer_us = 0;
}

void loop() {
  handleCommandInputs();
  mirror::pollRx();
  const bool imuDiagActive = (g_mode == CommandMode::ShowImuError) || (g_mode == CommandMode::TestImuRot);

  if (!imuDiagActive) {
    gpsPollContinuous();
    baroPollAndFilter();
    telemetry_loop(g_state);

    // IMU update is micros-scheduled at 400 Hz in imu_fusion::update400Hz(); call each loop.
    imu_fusion::update400Hz(g_state);
  }

  const uint32_t mirrorPeriodUs = mirror::streamPeriodUs();
  while (g_mirror_timer_us >= mirrorPeriodUs) {
    g_mirror_timer_us -= mirrorPeriodUs;
    if (!imuDiagActive) {
      mirrorSendFastState();
    }
  }

  if (!imuDiagActive && g_stats_streaming && g_summary_timer_us >= SUMMARY_PERIOD_US) {
    g_summary_timer_us -= SUMMARY_PERIOD_US;
    printSummary2Hz();
  }

  serviceCommandMode();
  g_logger.service(Serial, 8U);
}




