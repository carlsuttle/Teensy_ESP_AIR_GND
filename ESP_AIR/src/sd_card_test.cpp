#include "sd_card_test.h"

#include "sd_api.h"
#include "sd_backend.h"

namespace sd_card_test {
namespace {
using File = sd_api::File;

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
  status.cs_pin = sd_backend::csPin();
  status.sck_pin = sd_backend::sckPin();
  status.miso_pin = sd_backend::misoPin();
  status.mosi_pin = sd_backend::mosiPin();
}

void copyBackendStatus(const sd_backend::Status& backend, Status& status) {
  fillPinStatus(status);
  status.init_hz = backend.init_hz;
  status.spi_configured = backend.spi_configured;
  status.begin_ok = backend.begin_ok;
  status.card_type = backend.card_type;
  status.card_size_bytes = backend.card_size_bytes;
  status.total_bytes = backend.total_bytes;
  status.used_bytes = backend.used_bytes;
}

bool tryBegin(Status& status) {
  status = {};
  sd_backend::Status backend = {};
  const bool ok = sd_backend::begin(&backend);
  copyBackendStatus(backend, status);
  return ok;
}

bool writeBinaryProbe(Status& status) {
  ProbeRecord record = {
      0x53445052UL,  // "SDPR"
      millis(),
      1UL,
  };

  File file = sd_api::open(kProbePath, sd_api::OpenMode::write);
  if (!file) return false;

  const size_t expected = sizeof(record);
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(&record), expected);
  file.flush();
  file.close();

  if (written != expected) {
    sd_api::remove(kProbePath);
    return false;
  }

  status.write_bytes = (uint32_t)written;
  status.write_ok = true;
  (void)sd_api::remove(kProbePath);
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
