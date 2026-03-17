#include "sd_card_test.h"

#include <SD.h>
#include <SPI.h>

namespace sd_card_test {
namespace {

// Temporary bench-test wiring. These pins should match the current physical
// hookup, not the XIAO's default SPI silk labels.
constexpr uint8_t kSdCsPin = 10;
constexpr uint8_t kSdSckPin = 43;
constexpr uint8_t kSdMisoPin = 44;
constexpr uint8_t kSdMosiPin = 9;

constexpr uint32_t kInitFrequenciesHz[] = {
    400000UL,
    1000000UL,
    4000000UL,
    10000000UL,
};

constexpr char kProbePath[] = "/sdprobe.bin";

struct ProbeRecord {
  uint32_t magic;
  uint32_t millis_boot;
  uint32_t counter;
};

const char* cardTypeName(uint8_t cardType) {
  switch (cardType) {
    case CARD_MMC: return "MMC";
    case CARD_SD: return "SDSC";
    case CARD_SDHC: return "SDHC/SDXC";
    case CARD_NONE: return "NONE";
    default: return "UNKNOWN";
  }
}

uint64_t bytesToMiB(uint64_t bytes) {
  return bytes / (1024ULL * 1024ULL);
}

void fillPinStatus(Status& status) {
  status.cs_pin = kSdCsPin;
  status.sck_pin = kSdSckPin;
  status.miso_pin = kSdMisoPin;
  status.mosi_pin = kSdMosiPin;
}

bool beginAtFrequency(Status& status, uint32_t hz) {
  fillPinStatus(status);
  status.init_hz = hz;
  status.spi_configured = false;
  status.begin_ok = false;
  status.card_type = CARD_NONE;
  status.card_size_bytes = 0;
  status.total_bytes = 0;
  status.used_bytes = 0;

  SD.end();
  SPI.end();
  SPI.begin(kSdSckPin, kSdMisoPin, kSdMosiPin, kSdCsPin);
  status.spi_configured = true;

  if (!SD.begin(kSdCsPin, SPI, hz)) {
    return false;
  }

  status.begin_ok = true;
  status.card_type = SD.cardType();
  status.card_size_bytes = SD.cardSize();
  status.total_bytes = SD.totalBytes();
  status.used_bytes = SD.usedBytes();
  return true;
}

bool tryBegin(Status& status) {
  status = {};
  fillPinStatus(status);
  for (uint32_t hz : kInitFrequenciesHz) {
    if (beginAtFrequency(status, hz)) {
      return true;
    }
  }
  return false;
}

bool writeBinaryProbe(Status& status) {
  ProbeRecord record = {
      0x53445052UL,  // "SDPR"
      millis(),
      1UL,
  };

  File file = SD.open(kProbePath, FILE_WRITE);
  if (!file) return false;

  const size_t expected = sizeof(record);
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(&record), expected);
  file.flush();
  file.close();

  if (written != expected) {
    SD.remove(kProbePath);
    return false;
  }

  status.write_bytes = (uint32_t)written;
  status.write_ok = true;
  (void)SD.remove(kProbePath);
  return true;
}

}  // namespace

bool probe(Status& status) {
  return tryBegin(status);
}

bool writeProbe(Status& status) {
  if (!tryBegin(status)) return false;
  return writeBinaryProbe(status);
}

void printProbeReport(Stream& out, const Status& status) {
  out.printf("SDPROBE pins cs=%u sck=%u miso=%u mosi=%u\r\n",
             (unsigned)status.cs_pin,
             (unsigned)status.sck_pin,
             (unsigned)status.miso_pin,
             (unsigned)status.mosi_pin);

  out.printf("SDPROBE spi_configured=%u init_hz=%lu\r\n",
             status.spi_configured ? 1U : 0U,
             (unsigned long)status.init_hz);

  if (!status.begin_ok) {
    out.println("SDPROBE begin_ok=0 card=unavailable");
    return;
  }

  out.printf("SDPROBE begin_ok=1 card_type=%s(%u) card_mib=%llu total_mib=%llu used_mib=%llu\r\n",
             cardTypeName(status.card_type),
             (unsigned)status.card_type,
             bytesToMiB(status.card_size_bytes),
             bytesToMiB(status.total_bytes),
             bytesToMiB(status.used_bytes));

  out.printf("SDPROBE write_ok=%u write_bytes=%lu\r\n",
             status.write_ok ? 1U : 0U,
             (unsigned long)status.write_bytes);
}

}  // namespace sd_card_test
