#include "sd_backend.h"

#include <SPI.h>

#include "sd_api.h"

namespace sd_backend {
namespace {

constexpr uint8_t kSdCsPin = 2;
constexpr uint8_t kSdSckPin = 7;
constexpr uint8_t kSdMisoPin = 8;
constexpr uint8_t kSdMosiPin = 9;

constexpr uint32_t kInitFrequenciesHz[] = {
    26000000UL,
    20000000UL,
};

bool g_mounted = false;
uint32_t g_init_hz = 0U;
SPIClass* g_sd_spi = nullptr;

void markUnmounted() {
  sd_api::end();
  if (g_sd_spi) g_sd_spi->end();
  g_mounted = false;
  g_init_hz = 0U;
}

void prepareSpiPinsForSdInit() {
  // Bias lines to known idle states before handing them to the SPI peripheral.
  pinMode(kSdCsPin, OUTPUT);
  digitalWrite(kSdCsPin, HIGH);
  pinMode(kSdSckPin, INPUT_PULLUP);
  pinMode(kSdMisoPin, INPUT_PULLUP);
  pinMode(kSdMosiPin, INPUT_PULLUP);
}

bool mountedMediaHealthy() {
  if (!g_mounted) return false;
  const uint8_t card_type = sd_api::cardType();
  if (card_type == CARD_NONE) {
    markUnmounted();
    return false;
  }
  sd_api::File root = sd_api::open("/");
  const bool ok = root && root.isDirectory();
  if (root) root.close();
  if (!ok) {
    markUnmounted();
    return false;
  }
  return true;
}

bool populateStatus(Status& status) {
  fillPinStatus(status);
  status.init_hz = g_init_hz;
  status.spi_configured = g_mounted;
  status.begin_ok = g_mounted;
  status.card_type = CARD_NONE;
  status.card_size_bytes = 0;
  status.total_bytes = 0;
  status.used_bytes = 0;
  if (!g_mounted) return false;
  if (!mountedMediaHealthy()) {
    status.init_hz = 0U;
    status.spi_configured = false;
    status.begin_ok = false;
    return false;
  }
  status.card_type = sd_api::cardType();
  status.card_size_bytes = sd_api::cardSize();
  status.total_bytes = sd_api::totalBytes();
  status.used_bytes = sd_api::usedBytes();
  return true;
}

bool beginAtFrequency(uint32_t hz) {
  if (!g_sd_spi) g_sd_spi = new SPIClass(HSPI);
  if (!g_sd_spi) {
    markUnmounted();
    return false;
  }
  markUnmounted();
  prepareSpiPinsForSdInit();
  delay(5);
  g_sd_spi->begin(kSdSckPin, kSdMisoPin, kSdMosiPin, kSdCsPin);
  digitalWrite(kSdCsPin, HIGH);
  delay(5);
  if (!sd_api::begin(kSdCsPin, *g_sd_spi, hz)) {
    g_sd_spi->end();
    g_mounted = false;
    g_init_hz = 0U;
    return false;
  }
  g_mounted = true;
  g_init_hz = hz;
  return true;
}

}  // namespace

uint8_t csPin() { return kSdCsPin; }
uint8_t sckPin() { return kSdSckPin; }
uint8_t misoPin() { return kSdMisoPin; }
uint8_t mosiPin() { return kSdMosiPin; }

void fillPinStatus(Status& status) {
  status.cs_pin = kSdCsPin;
  status.sck_pin = kSdSckPin;
  status.miso_pin = kSdMisoPin;
  status.mosi_pin = kSdMosiPin;
}

bool begin(Status* status) {
  if (g_mounted) {
    if (status) {
      (void)populateStatus(*status);
    }
    return true;
  }
  for (uint32_t hz : kInitFrequenciesHz) {
    if (beginAtFrequency(hz)) {
      if (status) {
        (void)populateStatus(*status);
      }
      return true;
    }
  }
  if (status) {
    status->init_hz = 0U;
    status->spi_configured = true;
    status->begin_ok = false;
    status->card_type = CARD_NONE;
    status->card_size_bytes = 0;
    status->total_bytes = 0;
    status->used_bytes = 0;
    fillPinStatus(*status);
  }
  return false;
}

bool refreshStatus(Status& status) {
  return populateStatus(status);
}

bool mount(Status* status) {
  return begin(status);
}

bool mounted() { return g_mounted; }

bool mediaPresent() {
  return mountedMediaHealthy();
}

MediaState mediaState() {
  if (g_mounted) {
    return mountedMediaHealthy() ? MediaState::ready : MediaState::error;
  }
  return MediaState::unmounted;
}

uint32_t mountedFrequencyHz() { return g_init_hz; }

void end() {
  markUnmounted();
}

bool eject() {
  markUnmounted();
  return true;
}

}  // namespace sd_backend




