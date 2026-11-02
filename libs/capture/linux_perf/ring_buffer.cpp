#include "capture/linux_perf/ring_buffer.h"

#include <algorithm>

namespace tachyon::capture::perf {

RingStatus copyRingRange(const uint8_t* data, uint64_t dataSize, uint64_t tail, uint64_t head,
                         std::vector<uint8_t>& out) {
  if (head == tail) return RingStatus::Empty;
  if (dataSize == 0 || head < tail || head - tail > dataSize) return RingStatus::Overrun;
  const uint64_t n = head - tail;
  const uint64_t start = tail % dataSize;
  const uint64_t first = std::min<uint64_t>(n, dataSize - start);
  const size_t base = out.size();
  out.resize(base + static_cast<size_t>(n));
  std::copy(data + start, data + start + first, out.begin() + static_cast<std::ptrdiff_t>(base));
  if (first < n) std::copy(data, data + (n - first), out.begin() + static_cast<std::ptrdiff_t>(base + first));
  return RingStatus::Data;
}

RingStatus RingConsumer::drain(std::vector<uint8_t>& out) {
  const uint64_t head = __atomic_load_n(m_head, __ATOMIC_ACQUIRE);
  const uint64_t tail = *m_tail;  // only this consumer writes it
  const RingStatus st = copyRingRange(m_data, m_size, tail, head, out);
  if (st == RingStatus::Data) {
    m_consumed.fetch_add(head - tail, std::memory_order_relaxed);
    __atomic_store_n(m_tail, head, __ATOMIC_RELEASE);
  } else if (st == RingStatus::Overrun) {
    m_overruns.fetch_add(1, std::memory_order_relaxed);
    __atomic_store_n(m_tail, head, __ATOMIC_RELEASE);
  }
  return st;
}

uint64_t RingConsumer::pending() const {
  const uint64_t head = __atomic_load_n(m_head, __ATOMIC_ACQUIRE);
  const uint64_t tail = *m_tail;
  return head >= tail ? head - tail : 0;
}

}  // namespace tachyon::capture::perf
