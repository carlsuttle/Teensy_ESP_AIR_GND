#include "spi_bridge.h"

#include <DMAChannel.h>
#include <string.h>

namespace spi_bridge {
namespace {

constexpr uint32_t kMsgMagic = 0x53504931UL;
constexpr uint16_t kProtocolVersion = 1U;
constexpr uint16_t kRecordBytes = 160U;
constexpr uint16_t kTransactionBytes = 8192U;
constexpr uint16_t kTxRingDepth = 512U;
constexpr uint16_t kRxRingDepth = 512U;
constexpr uint32_t kReadyPulseLowGapUs = 20U;
constexpr uint8_t kSpiSck = 13U;
constexpr uint8_t kSpiSdi = 12U;
constexpr uint8_t kSpiSdo = 11U;
constexpr uint8_t kSpiCs = 10U;
constexpr uint8_t kReadyOut = 2U;
constexpr uint32_t kLpspiClearFlags = LPSPI_SR_DMF | LPSPI_SR_REF | LPSPI_SR_TEF |
                                      LPSPI_SR_TCF | LPSPI_SR_FCF | LPSPI_SR_WCF |
                                      LPSPI_SR_RDF | LPSPI_SR_TDF;

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

struct StatusPayload {
  uint16_t tx_free;
  uint16_t rx_free;
  uint16_t tx_overflow;
  uint16_t rx_overflow;
  uint32_t last_tx_seq;
  uint32_t last_rx_seq;
  uint32_t mode;
  uint32_t reserved;
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
    noInterrupts();
    head_ = 0U;
    tail_ = 0U;
    max_occupancy_ = 0U;
    interrupts();
  }

  bool push(const uint8_t* record_bytes) {
    if (!record_bytes) return false;
    bool ok = false;
    noInterrupts();
    const uint16_t next = (uint16_t)((head_ + 1U) & (kTxRingDepth - 1U));
    if (next != tail_) {
      memcpy(storage_[head_], record_bytes, kRecordBytes);
      head_ = next;
      const uint16_t occ = (uint16_t)((head_ - tail_) & (kTxRingDepth - 1U));
      if (occ > max_occupancy_) max_occupancy_ = occ;
      ok = true;
    }
    interrupts();
    return ok;
  }

  bool pop(uint8_t* out) {
    if (!out) return false;
    bool ok = false;
    noInterrupts();
    if (tail_ != head_) {
      memcpy(out, storage_[tail_], kRecordBytes);
      tail_ = (uint16_t)((tail_ + 1U) & (kTxRingDepth - 1U));
      ok = true;
    }
    interrupts();
    return ok;
  }

  bool peekContiguous(const uint8_t*& out_ptr, uint16_t& out_count) const {
    out_ptr = nullptr;
    out_count = 0U;
    noInterrupts();
    if (tail_ != head_) {
      out_ptr = storage_[tail_];
      out_count = (head_ > tail_) ? (uint16_t)(head_ - tail_) : (uint16_t)(kTxRingDepth - tail_);
    }
    interrupts();
    return out_count != 0U;
  }

  void popMany(uint16_t count) {
    noInterrupts();
    const uint16_t occ = (uint16_t)((head_ - tail_) & (kTxRingDepth - 1U));
    if (count > occ) count = occ;
    tail_ = (uint16_t)((tail_ + count) & (kTxRingDepth - 1U));
    interrupts();
  }

  uint16_t freeSlots() const {
    noInterrupts();
    const uint16_t occ = (uint16_t)((head_ - tail_) & (kTxRingDepth - 1U));
    interrupts();
    return (uint16_t)((kTxRingDepth - 1U) - occ);
  }

  uint16_t maxOccupancy() const {
    noInterrupts();
    const uint16_t v = max_occupancy_;
    interrupts();
    return v;
  }

  bool empty() const {
    noInterrupts();
    const bool empty = head_ == tail_;
    interrupts();
    return empty;
  }

 private:
  alignas(4) uint8_t storage_[kTxRingDepth][kRecordBytes] = {};
  volatile uint16_t head_ = 0U;
  volatile uint16_t tail_ = 0U;
  uint16_t max_occupancy_ = 0U;
};

inline void clearLpspiStatus() {
  LPSPI4_SR = kLpspiClearFlags;
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

inline size_t wordAlignedLen(size_t len) {
  return (len + 3U) & ~size_t(3U);
}

uint32_t crc32Compute(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8U; ++b) {
      const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

DMAMEM static uint32_t g_tx_dma_words[kTransactionBytes / sizeof(uint32_t)] __attribute__((aligned(32)));
DMAMEM static uint32_t g_rx_dma_words[kTransactionBytes / sizeof(uint32_t)] __attribute__((aligned(32)));
DMAChannel g_tx_dma;
DMAChannel g_rx_dma;
RecordRing g_state_tx_ring;
RecordRing g_raw_tx_ring;
RecordRing g_replay_rx_ring;
volatile bool g_ready_state = false;
volatile bool g_transaction_armed = false;
volatile bool g_transaction_complete = false;
uint8_t g_pending_state_pop_count = 0U;
uint8_t g_pending_raw_pop_count = 0U;
uint32_t g_tx_message_seq = 0U;
uint32_t g_last_activity_us = 0U;
Stats g_stats = {};

void configurePins() {
  pinMode(kReadyOut, OUTPUT);
  digitalWriteFast(kReadyOut, LOW);

  CORE_PIN10_CONFIG = 3;
  CORE_PIN11_CONFIG = 3;
  CORE_PIN12_CONFIG = 3;
  CORE_PIN13_CONFIG = 3;

  IOMUXC_LPSPI4_PCS0_SELECT_INPUT = 0;
  IOMUXC_LPSPI4_SDO_SELECT_INPUT = 0;
  IOMUXC_LPSPI4_SDI_SELECT_INPUT = 0;
  IOMUXC_LPSPI4_SCK_SELECT_INPUT = 0;
}

void lpspiIsr() {
  const uint32_t sr = LPSPI4_SR;
  const uint32_t clear = sr & (LPSPI_SR_REF | LPSPI_SR_TEF | LPSPI_SR_TCF | LPSPI_SR_DMF |
                               LPSPI_SR_RDF | LPSPI_SR_TDF | LPSPI_SR_FCF | LPSPI_SR_WCF);
  if (clear != 0U) {
    LPSPI4_SR = clear;
  }
}

void rxDmaIsr() {
  g_rx_dma.clearInterrupt();
  g_rx_dma.clearComplete();
  g_tx_dma.clearComplete();
  g_rx_dma.disable();
  g_tx_dma.disable();
  LPSPI4_DER = 0U;
  clearLpspiStatus();

  g_transaction_armed = false;
  g_transaction_complete = true;
  g_last_activity_us = micros();
  g_ready_state = false;
  digitalWriteFast(kReadyOut, LOW);
}

void configureLpspi4() {
  CCM_CCGR1 |= CCM_CCGR1_LPSPI4(CCM_CCGR_ON);
  LPSPI4_CR = LPSPI_CR_RST;
  LPSPI4_CR = 0U;
  LPSPI4_IER = 0U;
  LPSPI4_DER = 0U;
  LPSPI4_CFGR0 = 0U;
  LPSPI4_CFGR1 = 0U;
  LPSPI4_FCR = LPSPI_FCR_RXWATER(0) | LPSPI_FCR_TXWATER(0);
  LPSPI4_TCR = LPSPI_TCR_PCS(0) | LPSPI_TCR_FRAMESZ(31);
  clearLpspiStatus();

  attachInterruptVector(IRQ_LPSPI4, lpspiIsr);
  NVIC_SET_PRIORITY(IRQ_LPSPI4, 32);
  NVIC_ENABLE_IRQ(IRQ_LPSPI4);

  LPSPI4_CR = LPSPI_CR_DBGEN | LPSPI_CR_MEN;
  LPSPI4_IER = LPSPI_IER_REIE | LPSPI_IER_TEIE;
  LPSPI4_DER = 0U;
}

void configureDma() {
  g_tx_dma.disable();
  g_rx_dma.disable();

  g_tx_dma.sourceBuffer(g_tx_dma_words, kTransactionBytes);
  g_tx_dma.destination((volatile uint32_t&)LPSPI4_TDR);
  g_tx_dma.triggerAtHardwareEvent(DMAMUX_SOURCE_LPSPI4_TX);
  g_tx_dma.disableOnCompletion();
  g_tx_dma.clearComplete();
  g_tx_dma.clearError();

  g_rx_dma.source((volatile uint32_t&)LPSPI4_RDR);
  g_rx_dma.destinationBuffer(g_rx_dma_words, kTransactionBytes);
  g_rx_dma.triggerAtHardwareEvent(DMAMUX_SOURCE_LPSPI4_RX);
  g_rx_dma.disableOnCompletion();
  g_rx_dma.attachInterrupt(rxDmaIsr, 32);
  g_rx_dma.interruptAtCompletion();
  g_rx_dma.clearInterrupt();
  g_rx_dma.clearComplete();
  g_rx_dma.clearError();

  DMAPriorityOrder(g_rx_dma, g_tx_dma);
}

void buildTxFrame() {
  memset(g_tx_dma_words, 0, kTransactionBytes);
  g_pending_state_pop_count = 0U;
  g_pending_raw_pop_count = 0U;

  auto* header = reinterpret_cast<SpiMsgHeader*>(g_tx_dma_words);
  uint8_t* payload = reinterpret_cast<uint8_t*>(g_tx_dma_words) + sizeof(SpiMsgHeader);
  header->magic = kMsgMagic;
  header->version = kProtocolVersion;
  header->type = kMsgNone;
  header->payload_len = 0U;
  header->flags = g_replay_rx_ring.freeSlots();
  header->seq = ++g_tx_message_seq;
  header->crc32 = 0U;

  const uint8_t* src = nullptr;
  uint16_t contiguous = 0U;
  // Live fused state must win over raw replay-input samples. If raw records
  // are preferred here, the AIR side can be flooded with type-5 payloads and
  // never see any type-1 state frames, which makes the radio/UI path look
  // connected but permanently stale.
  if (g_state_tx_ring.peekContiguous(src, contiguous) && contiguous != 0U) {
    const uint16_t records_to_send = (contiguous < kMaxRecordsPerPayload) ? contiguous : kMaxRecordsPerPayload;
    const uint16_t payload_len = (uint16_t)(records_to_send * kRecordBytes);
    memcpy(payload, src, payload_len);
    header->type = kMsgStateData;
    header->payload_len = payload_len;
    header->crc32 = crc32Compute(payload, payload_len);
    g_pending_state_pop_count = (uint8_t)records_to_send;
    return;
  }

  if (g_raw_tx_ring.peekContiguous(src, contiguous) && contiguous != 0U) {
    const uint16_t records_to_send = (contiguous < kMaxRecordsPerPayload) ? contiguous : kMaxRecordsPerPayload;
    const uint16_t payload_len = (uint16_t)(records_to_send * kRecordBytes);
    memcpy(payload, src, payload_len);
    header->type = kMsgReplayInputData;
    header->payload_len = payload_len;
    header->crc32 = crc32Compute(payload, payload_len);
    g_pending_raw_pop_count = (uint8_t)records_to_send;
    return;
  }

  StatusPayload status = {};
  status.tx_free = g_raw_tx_ring.freeSlots();
  status.rx_free = g_replay_rx_ring.freeSlots();
  status.tx_overflow = (uint16_t)((g_stats.tx_overflows > 0xFFFFU) ? 0xFFFFU : g_stats.tx_overflows);
  status.rx_overflow = (uint16_t)((g_stats.rx_overflows > 0xFFFFU) ? 0xFFFFU : g_stats.rx_overflows);
  memcpy(payload, &status, sizeof(status));
  header->type = kMsgStatus;
  header->payload_len = sizeof(status);
  header->crc32 = crc32Compute(payload, sizeof(status));
}

void parseRxFrame() {
  byteSwapWordsInPlace(reinterpret_cast<uint8_t*>(g_rx_dma_words), sizeof(SpiMsgHeader));
  const auto* header = reinterpret_cast<const SpiMsgHeader*>(g_rx_dma_words);
  if (header->magic != kMsgMagic || header->version != kProtocolVersion || header->payload_len > kMaxPayloadBytes) {
    g_stats.rx_type_errors++;
    return;
  }

  uint8_t* payload = reinterpret_cast<uint8_t*>(g_rx_dma_words) + sizeof(SpiMsgHeader);
  if (header->payload_len != 0U) {
    byteSwapWordsInPlace(payload, wordAlignedLen(header->payload_len));
    const uint32_t crc = crc32Compute(payload, header->payload_len);
    if (crc != header->crc32) {
      g_stats.rx_crc_errors++;
      return;
    }
  }

  if (header->type == kMsgReplayData) {
    if (header->payload_len == 0U || (header->payload_len % kRecordBytes) != 0U) {
      g_stats.rx_type_errors++;
      return;
    }
    for (uint16_t offset = 0U; offset < header->payload_len; offset = (uint16_t)(offset + kRecordBytes)) {
      if (!g_replay_rx_ring.push(payload + offset)) {
        g_stats.rx_overflows++;
        continue;
      }
      g_stats.rx_records++;
    }
    return;
  }

  if (header->type != kMsgNone && header->type != kMsgStatus && header->type != kMsgControl) {
    g_stats.rx_type_errors++;
  }
}

}  // namespace

void begin() {
  g_state_tx_ring.reset();
  g_raw_tx_ring.reset();
  g_replay_rx_ring.reset();
  g_stats = {};
  g_ready_state = false;
  g_transaction_armed = false;
  g_transaction_complete = false;
  g_pending_state_pop_count = 0U;
  g_pending_raw_pop_count = 0U;
  g_tx_message_seq = 0U;
  g_last_activity_us = micros();

  configurePins();
  configureLpspi4();
  configureDma();
}

void poll() {
  bool completed = false;
  noInterrupts();
  if (g_transaction_complete) {
    g_transaction_complete = false;
    completed = true;
  }
  interrupts();

  if (completed) {
    arm_dcache_delete(g_rx_dma_words, kTransactionBytes);
    parseRxFrame();
    if (g_pending_raw_pop_count != 0U) {
      g_raw_tx_ring.popMany(g_pending_raw_pop_count);
      g_stats.tx_records += g_pending_raw_pop_count;
      g_pending_raw_pop_count = 0U;
    }
    if (g_pending_state_pop_count != 0U) {
      g_state_tx_ring.popMany(g_pending_state_pop_count);
      g_stats.tx_records += g_pending_state_pop_count;
      g_pending_state_pop_count = 0U;
    }
    g_last_activity_us = micros();
  }

  if ((uint32_t)(micros() - g_last_activity_us) >= kReadyPulseLowGapUs) {
    bool can_arm = false;
    noInterrupts();
    can_arm = (!g_transaction_armed) && (!g_transaction_complete);
    interrupts();

    if (can_arm) {
      buildTxFrame();
      arm_dcache_flush_delete(g_tx_dma_words, kTransactionBytes);
      arm_dcache_delete(g_rx_dma_words, kTransactionBytes);

      noInterrupts();
      if ((!g_transaction_armed) && (!g_transaction_complete)) {
        g_transaction_complete = false;
        g_transaction_armed = true;
        g_tx_dma.disable();
        g_rx_dma.disable();
        g_tx_dma.clearComplete();
        g_rx_dma.clearComplete();
        g_tx_dma.clearError();
        g_rx_dma.clearError();
        g_rx_dma.clearInterrupt();

        LPSPI4_CR = LPSPI_CR_DBGEN | LPSPI_CR_MEN | LPSPI_CR_RRF | LPSPI_CR_RTF;
        LPSPI4_CR = LPSPI_CR_DBGEN | LPSPI_CR_MEN;
        clearLpspiStatus();

        g_tx_dma.enable();
        g_rx_dma.enable();
        LPSPI4_DER = LPSPI_DER_RDDE | LPSPI_DER_TDDE;

        g_ready_state = true;
        digitalWriteFast(kReadyOut, HIGH);
      }
      interrupts();
    }
  }

  g_stats.ready_high = g_ready_state;
}

bool pushStateRecord(const uint8_t* record_bytes, size_t len) {
  if (!record_bytes || len != kRecordBytes) return false;
  const bool ok = g_state_tx_ring.push(record_bytes);
  if (!ok) g_stats.tx_overflows++;
  return ok;
}

bool pushRawRecord(const uint8_t* record_bytes, size_t len) {
  if (!record_bytes || len != kRecordBytes) return false;
  const bool ok = g_raw_tx_ring.push(record_bytes);
  if (!ok) g_stats.tx_overflows++;
  return ok;
}

bool popReplayRecord(uint8_t* record_out, size_t len) {
  if (!record_out || len != kRecordBytes) return false;
  return g_replay_rx_ring.pop(record_out);
}

uint16_t replayFreeSlots() {
  return g_replay_rx_ring.freeSlots();
}

Stats stats() {
  return g_stats;
}

}  // namespace spi_bridge



