// Consumer side of a perf mmap ring buffer (WP-70).
//
// The kernel producer advances data_head (a byte counter that only grows) after it finished writing a record;
// the consumer advances data_tail after copying. The data area is a power-of-two number of bytes; a record
// may straddle the end of the area, so the bytes are copied out in logical order into a contiguous buffer
// where the parser sees whole records. The counters are plain uint64_t locations (the kernel's
// perf_event_mmap_page fields in production, a local struct in tests) accessed with acquire/release
// semantics.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tachyon::capture::perf {

enum class RingStatus : uint8_t {
  Empty,    // nothing new
  Data,     // bytes were appended to the output
  Overrun,  // head - tail exceeded the data size: the producer lapped the consumer (should not happen: the
            // kernel does not overwrite unread data, it drops records and emits LOST); the tail was moved to
            // head and nothing was appended
};

// Thread-safe: pure function. Appends the bytes of [tail, head) from the ring's data area to `out`.
// Precondition: head >= tail and head - tail <= dataSize (checked; violating it returns Overrun).
RingStatus copyRingRange(const uint8_t* data, uint64_t dataSize, uint64_t tail, uint64_t head,
                         std::vector<uint8_t>& out);

class RingConsumer {
 public:
  // `head` / `tail` point at the producer's data_head and the consumer's data_tail. All pointers must outlive
  // the consumer.
  RingConsumer(const uint8_t* data, uint64_t dataSize, uint64_t* head, uint64_t* tail)
      : m_data(data), m_size(dataSize), m_head(head), m_tail(tail) {}

  // Not thread-safe (single consumer). Reads data_head, copies the new bytes to `out`, publishes data_tail.
  RingStatus drain(std::vector<uint8_t>& out);

  // Not thread-safe. Bytes currently waiting (head - tail).
  uint64_t pending() const;
  // Thread-safe: may be read while another thread drains.
  uint64_t overruns() const { return m_overruns.load(std::memory_order_relaxed); }
  uint64_t bytesConsumed() const { return m_consumed.load(std::memory_order_relaxed); }

 private:
  const uint8_t* m_data;
  uint64_t m_size;
  uint64_t* m_head;
  uint64_t* m_tail;
  std::atomic<uint64_t> m_overruns{0};
  std::atomic<uint64_t> m_consumed{0};
};

}  // namespace tachyon::capture::perf
