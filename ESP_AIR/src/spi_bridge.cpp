#include "spi_bridge.h"

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

namespace spi_bridge {
namespace {

constexpr uint32_t kMsgMagic = 0x53504931UL;
constexpr uint16_t kProtocolVersion = 1U;
constexpr uint16_t kRecordBytes = 160U;
constexpr uint16_t kTransactionBytes = 8192U;
constexpr uint16_t kRingDepth = 128U;
constexpr uint32_t kDefaultSpiClockHz = 20000000UL;
constexpr uint32_t kDefaultTransactionRateHz = 100U;
constexpr uint32_t kReadyWaitTimeoutUs = 5000U;
constexpr uint32_t kReadySettleDelayUs = 2U;
constexpr gpio_num_t kSpiSck = GPIO_NUM_5;
constexpr gpio_num_t kSpiMiso = GPIO_NUM_6;
constexpr gpio_num_t kSpiMosi = GPIO_NUM_1;
constexpr gpio_num_t kSpiCs = GPIO_NUM_44;
constexpr gpio_num_t kReadyIn = GPIO_NUM_4;
constexpr BaseType_t kSpiTaskCore = 1;
constexpr UBaseType_t kSpiTaskPriority = 1U;
constexpr uint8_t kSpiDeviceQueueDepth = 2U;
constexpr uint8_t kSpiCsPretransCycles = 8U;
constexpr uint8_t kSpiCsPosttransCycles = 2U;

#pragma pack(push, 1)
struct SpiMsgHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint16_t payload_len;
  uint16_t flags;
  uint32_t seq;
  uint32_t crc32;
};
#pragma pack(pop)

constexpr size_t kMaxPayloadBytes = kTransactionBytes - sizeof(SpiMsgHeader);
constexpr uint16_t kMaxRecordsPerPayload = (uint16_t)(kMaxPayloadBytes / kRecordBytes);

enum MsgType : uint16_t {
  kMsgNone = 0U,
  kMsgStateData = 1U,
  kMsgReplayData = 2U,
  kMsgControl = 3U,
  kMsgStatus = 4U,
  kMsgReplayInputData = 5U,
};

class RecordRing {
 public:
  void reset() {
    portENTER_CRITICAL(&mux_);
    head_ = 0U;
    tail_ = 0U;
    max_occupancy_ = 0U;
    portEXIT_CRITICAL(&mux_);
  }

  bool push(const uint8_t* record_bytes) {
    if (!record_bytes) return false;
    bool ok = false;
    portENTER_CRITICAL(&mux_);
    const uint16_t next = (uint16_t)((head_ + 1U) & (kRingDepth - 1U));
    if (next != tail_) {
      memcpy(storage_[head_], record_bytes, kRecordBytes);
      head_ = next;
      const uint16_t occ = (uint16_t)((head_ - tail_) & (kRingDepth - 1U));
      if (occ > max_occupancy_) max_occupancy_ = occ;
      ok = true;
    }
    portEXIT_CRITICAL(&mux_);
    return ok;
  }

  bool pop(uint8_t* out) {
    if (!out) return false;
    bool ok = false;
    portENTER_CRITICAL(&mux_);
    if (tail_ != head_) {
      memcpy(out, storage_[tail_], kRecordBytes);
      tail_ = (uint16_t)((tail_ + 1U) & (kRingDepth - 1U));
      ok = true;
    }
    portEXIT_CRITICAL(&mux_);
    return ok;
  }

  uint16_t freeSlots() const {
    portENTER_CRITICAL(&mux_);
    const uint16_t occ = (uint16_t)((head_ - tail_) & (kRingDepth - 1U));
    portEXIT_CRITICAL(&mux_);
    return (uint16_t)((kRingDepth - 1U) - occ);
  }

  uint16_t maxOccupancy() const {
    portENTER_CRITICAL(&mux_);
    const uint16_t value = max_occupancy_;
    portEXIT_CRITICAL(&mux_);
    return value;
  }

 private:
  alignas(4) uint8_t storage_[kRingDepth][kRecordBytes] = {};
  volatile uint16_t head_ = 0U;
  volatile uint16_t tail_ = 0U;
  uint16_t max_occupancy_ = 0U;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

inline uint32_t nowUs() {
  return (uint32_t)esp_timer_get_time();
}

inline uint32_t computeIntervalUs(uint32_t rate_hz) {
  return (rate_hz == 0U) ? 1000000UL : (1000000UL / rate_hz);
}

void byteSwapWordsInPlace(uint8_t* data, size_t len) {
  if (!data) return;
  for (size_t i = 0; (i + 3U) < len; i += 4U) {
    const uint8_t b0 = data[i + 0U];
    const uint8_t b1 = data[i + 1U];
    data[i + 0U] = data[i + 3U];
    data[i + 1U] = data[i + 2U];
    data[i + 2U] = b1;
    data[i + 3U] = b0;
  }
}

uint32_t crc32Compute(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8U; ++b) {
      crc = (crc >> 1U) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1U));
    }
  }
  return ~crc;
}

class Bridge {
 public:
  void begin();
  void poll();
  bool popState(uint8_t* out) { return state_rx_ring_.pop(out); }
  bool popRaw(uint8_t* out) { return raw_rx_ring_.pop(out); }
  bool queueReplay(const uint8_t* record_bytes) { return replay_tx_ring_.push(record_bytes); }
  Stats stats() const { return stats_; }

 private:
  static void taskEntry(void* arg);
  static void IRAM_ATTR readyIsr(void* arg);
  void taskLoop();
  void IRAM_ATTR handleReadyIsr();
  bool configureDevice(uint32_t hz);
  bool waitForReadyHigh(uint32_t timeout_us) const;
  bool performTransaction();
  void buildTxFrame();
  void parseRxFrame();

  spi_device_handle_t device_ = nullptr;
  TaskHandle_t task_ = nullptr;
  RecordRing state_rx_ring_;
  RecordRing raw_rx_ring_;
  RecordRing replay_tx_ring_;
  Stats stats_ = {};
  uint32_t spi_clock_hz_ = kDefaultSpiClockHz;
  uint32_t transaction_rate_hz_ = kDefaultTransactionRateHz;
  volatile bool run_active_ = true;
  volatile uint32_t last_ready_isr_us_ = 0U;
  uint32_t next_transaction_due_us_ = 0U;
  uint16_t remote_replay_rx_free_ = 0xFFFFU;
  uint8_t* tx_buffer_ = nullptr;
  uint8_t* rx_buffer_ = nullptr;
  uint8_t pending_replay_count_ = 0U;
  uint8_t pending_replay_records_[kMaxRecordsPerPayload * kRecordBytes] = {};
};

Bridge g_bridge;

}  // namespace

void Bridge::begin() {
  state_rx_ring_.reset();
  raw_rx_ring_.reset();
  replay_tx_ring_.reset();
  stats_ = {};
  spi_clock_hz_ = kDefaultSpiClockHz;
  transaction_rate_hz_ = kDefaultTransactionRateHz;
  run_active_ = true;
  next_transaction_due_us_ = nowUs();
  remote_replay_rx_free_ = 0xFFFFU;
  pending_replay_count_ = 0U;

  tx_buffer_ = (uint8_t*)heap_caps_malloc(kTransactionBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  rx_buffer_ = (uint8_t*)heap_caps_malloc(kTransactionBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (tx_buffer_) memset(tx_buffer_, 0, kTransactionBytes);
  if (rx_buffer_) memset(rx_buffer_, 0, kTransactionBytes);

  spi_bus_config_t buscfg = {};
  buscfg.sclk_io_num = kSpiSck;
  buscfg.mosi_io_num = kSpiMosi;
  buscfg.miso_io_num = kSpiMiso;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = kTransactionBytes;
  (void)spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
  (void)configureDevice(spi_clock_hz_);

  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << kReadyIn;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_POSEDGE;
  (void)gpio_config(&io);
  (void)gpio_install_isr_service(0);
  (void)gpio_isr_handler_add(kReadyIn, readyIsr, this);

  xTaskCreatePinnedToCore(taskEntry, "spi_txn", 6144, this, kSpiTaskPriority, &task_, kSpiTaskCore);
}

bool Bridge::configureDevice(uint32_t hz) {
  if (device_) {
    spi_bus_remove_device(device_);
    device_ = nullptr;
  }

  spi_device_interface_config_t devcfg = {};
  devcfg.mode = 0;
  devcfg.clock_speed_hz = (int)hz;
  devcfg.spics_io_num = kSpiCs;
  devcfg.queue_size = kSpiDeviceQueueDepth;
  devcfg.cs_ena_pretrans = kSpiCsPretransCycles;
  devcfg.cs_ena_posttrans = kSpiCsPosttransCycles;
  if (spi_bus_add_device(SPI2_HOST, &devcfg, &device_) != ESP_OK) {
    device_ = nullptr;
    return false;
  }
  return true;
}

void Bridge::poll() {
  if (task_) xTaskNotifyGive(task_);
}

void Bridge::taskEntry(void* arg) {
  static_cast<Bridge*>(arg)->taskLoop();
}

void IRAM_ATTR Bridge::readyIsr(void* arg) {
  static_cast<Bridge*>(arg)->handleReadyIsr();
}

void IRAM_ATTR Bridge::handleReadyIsr() {
  last_ready_isr_us_ = nowUs();
  BaseType_t hpw = pdFALSE;
  vTaskNotifyGiveFromISR(task_, &hpw);
  if (hpw == pdTRUE) portYIELD_FROM_ISR();
}

bool Bridge::waitForReadyHigh(uint32_t timeout_us) const {
  const uint32_t start = nowUs();
  while (!gpio_get_level(kReadyIn)) {
    if ((uint32_t)(nowUs() - start) >= timeout_us) {
      return false;
    }
  }
  return true;
}

void Bridge::buildTxFrame() {
  auto* header = reinterpret_cast<SpiMsgHeader*>(tx_buffer_);
  uint8_t* payload = tx_buffer_ + sizeof(SpiMsgHeader);
  memset(tx_buffer_, 0, kTransactionBytes);
  header->magic = kMsgMagic;
  header->version = kProtocolVersion;
  header->type = kMsgNone;
  header->payload_len = 0U;
  header->flags = state_rx_ring_.freeSlots();
  header->seq = 0U;
  header->crc32 = 0U;

  if (pending_replay_count_ == 0U && remote_replay_rx_free_ != 0U) {
    const uint16_t batch_limit = (remote_replay_rx_free_ < kMaxRecordsPerPayload) ? remote_replay_rx_free_ : kMaxRecordsPerPayload;
    while (pending_replay_count_ < batch_limit) {
      if (!replay_tx_ring_.pop(pending_replay_records_ + (pending_replay_count_ * kRecordBytes))) {
        break;
      }
      pending_replay_count_++;
    }
  }

  if (pending_replay_count_ != 0U) {
    const uint16_t payload_len = (uint16_t)(pending_replay_count_ * kRecordBytes);
    memcpy(payload, pending_replay_records_, payload_len);
    header->type = kMsgReplayData;
    header->payload_len = payload_len;
    header->crc32 = crc32Compute(payload, payload_len);
  }
}

void Bridge::parseRxFrame() {
  byteSwapWordsInPlace(rx_buffer_, kTransactionBytes);
  const auto* header = reinterpret_cast<const SpiMsgHeader*>(rx_buffer_);
  stats_.last_magic = header->magic;
  stats_.last_version = header->version;
  stats_.last_type = header->type;
  stats_.last_len = header->payload_len;
  if (header->magic != kMsgMagic || header->version != kProtocolVersion || header->payload_len > kMaxPayloadBytes) {
    stats_.rx_type_errors++;
    return;
  }

  const uint8_t* payload = rx_buffer_ + sizeof(SpiMsgHeader);
  if (header->payload_len != 0U) {
    const uint32_t crc = crc32Compute(payload, header->payload_len);
    if (crc != header->crc32) {
      stats_.rx_crc_errors++;
      return;
    }
  }

  remote_replay_rx_free_ = header->flags;
  if (header->type == kMsgStateData) {
    if (header->payload_len == 0U || (header->payload_len % kRecordBytes) != 0U) {
      stats_.rx_type_errors++;
      return;
    }
    for (uint16_t offset = 0U; offset < header->payload_len; offset = (uint16_t)(offset + kRecordBytes)) {
      if (!state_rx_ring_.push(payload + offset)) {
        stats_.rx_overflows++;
        continue;
      }
      stats_.state_records_received++;
      stats_.last_state_rx_ms = millis();
    }
    return;
  }

  if (header->type == kMsgReplayInputData) {
    if (header->payload_len == 0U || (header->payload_len % kRecordBytes) != 0U) {
      stats_.rx_type_errors++;
      return;
    }
    for (uint16_t offset = 0U; offset < header->payload_len; offset = (uint16_t)(offset + kRecordBytes)) {
      if (!raw_rx_ring_.push(payload + offset)) {
        stats_.rx_overflows++;
        continue;
      }
      stats_.raw_records_received++;
    }
    return;
  }

  if (header->type != kMsgNone && header->type != kMsgStatus && header->type != kMsgControl) {
    stats_.rx_type_errors++;
  }
}

bool Bridge::performTransaction() {
  if (!device_ || !tx_buffer_ || !rx_buffer_) return false;
  buildTxFrame();
  if (!waitForReadyHigh(kReadyWaitTimeoutUs)) {
    return false;
  }
  delayMicroseconds(kReadySettleDelayUs);

  memset(rx_buffer_, 0, kTransactionBytes);
  spi_transaction_t t = {};
  t.length = kTransactionBytes * 8U;
  t.tx_buffer = tx_buffer_;
  t.rx_buffer = rx_buffer_;

  if (spi_device_queue_trans(device_, &t, portMAX_DELAY) != ESP_OK) {
    stats_.transaction_failures++;
    return false;
  }
  spi_transaction_t* result = nullptr;
  if (spi_device_get_trans_result(device_, &result, portMAX_DELAY) != ESP_OK || result != &t) {
    stats_.transaction_failures++;
    return false;
  }

  stats_.transactions_completed++;
  parseRxFrame();
  if (pending_replay_count_ != 0U) {
    stats_.replay_records_sent += pending_replay_count_;
    pending_replay_count_ = 0U;
  }
  return true;
}

void Bridge::taskLoop() {
  uint32_t transactions_since_yield = 0U;
  for (;;) {
    const uint32_t now = nowUs();
    if ((int32_t)(now - next_transaction_due_us_) >= 0) {
      if (performTransaction()) {
        next_transaction_due_us_ = nowUs() + computeIntervalUs(transaction_rate_hz_);
        transactions_since_yield++;
        if (transactions_since_yield >= 16U) {
          transactions_since_yield = 0U;
          vTaskDelay(1);
        }
      } else {
        transactions_since_yield = 0U;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
      }
      continue;
    }

    const uint32_t slack_us = next_transaction_due_us_ - now;
    transactions_since_yield = 0U;
    if (slack_us > 1000U) {
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    } else {
      delayMicroseconds(slack_us);
    }
  }
}

void begin() {
  g_bridge.begin();
}

void poll() {
  g_bridge.poll();
}

bool popStateRecord(uint8_t* record_out, size_t len) {
  if (!record_out || len != kRecordBytes) return false;
  return g_bridge.popState(record_out);
}

bool popRawRecord(uint8_t* record_out, size_t len) {
  if (!record_out || len != kRecordBytes) return false;
  return g_bridge.popRaw(record_out);
}

bool queueReplayRecord(const uint8_t* record_bytes, size_t len) {
  if (!record_bytes || len != kRecordBytes) return false;
  const bool ok = g_bridge.queueReplay(record_bytes);
  if (!ok) {
    Stats current = g_bridge.stats();
    current.tx_overflows++;
  }
  return ok;
}

Stats stats() {
  return g_bridge.stats();
}

}  // namespace spi_bridge







