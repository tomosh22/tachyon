// Per-thread single-producer/single-consumer byte ring holding wire frames (inst_wire.h), and the
// producer-side helpers used on the hot path. The producer is the owning thread, the consumer is the
// runtime's flush thread (or, after the flush thread has stopped, the thread running the final drain).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>

#include "inst_wire.h"
#include "sdk_platform.h"

namespace tachyon::sdk {

constexpr uint32_t kStrTableBits = 10;  // per-thread "already defined" string table
constexpr uint32_t kStrTableSize = 1u << kStrTableBits;
constexpr uint32_t kStrTableMaxLoad = kStrTableSize * 3 / 4;
constexpr uint32_t kMaxTrackedDepth = 256;  // deeper scope nesting is not recorded
constexpr uint64_t kDefaultRingBytes = 64 * 1024;
constexpr uint64_t kMinRingBytes = 16 * 1024;  // must hold the largest frame plus end-credit headroom
constexpr uint64_t kMaxRingBytes = 64ull * 1024 * 1024;

// Open-scope bookkeeping. Per thread; saved/restored per fiber across fiber switches.
struct ScopeStack {
  uint32_t depth = 0;        // open Begin calls (recorded or dropped)
  uint32_t emittedOpen = 0;  // open scopes whose Begin was recorded: each needs 20 bytes reserved for its End
  uint32_t openDropped = 0;  // open scopes whose Begin was dropped (their End is dropped too)
  uint64_t droppedBits[kMaxTrackedDepth / 64] = {0, 0, 0, 0};

  void reset() { *this = ScopeStack(); }
};

struct alignas(64) ThreadRing {
  // ---- producer-owned ----
  std::atomic<uint64_t> head{0};  // total bytes ever written; published with release
  uint64_t cachedTail = 0;
  uint8_t* buf = nullptr;
  uint64_t cap = 0;   // power of two
  uint64_t mask = 0;  // cap - 1
  uint32_t tid = 0;
  uint32_t strCount = 0;
  uint64_t lastBeginPos = ~0ull;       // position of the most recent recorded ScopeBegin frame (test hooks)
  std::atomic<uint64_t> dropTotal{0};  // records dropped so far; single writer (the producer)
  ScopeStack stack;
  uint64_t strTable[kStrTableSize] = {};  // keys of strings already defined in this ring

  // ---- consumer-owned ----
  alignas(64) std::atomic<uint64_t> tail{0};  // total bytes consumed; published with release
  uint64_t dropReported = 0;

  // ---- shared ----
  std::atomic<bool> retired{false};  // set by the owning thread's exit; never cleared
};

// Producer: free bytes check. `bytes` must include any headroom the caller wants to keep. Thread-safe only
// for the owning thread.
inline bool ringReserve(ThreadRing* r, uint64_t bytes) {
  const uint64_t head = r->head.load(std::memory_order_relaxed);
  if (TACHYON_SDK_LIKELY(r->cap - (head - r->cachedTail) >= bytes)) return true;
  r->cachedTail = r->tail.load(std::memory_order_acquire);
  return r->cap - (head - r->cachedTail) >= bytes;
}

// Producer: copies n bytes to ring position `pos` (monotonic position, wrapped internally).
inline void ringPut(ThreadRing* r, uint64_t pos, const void* src, size_t n) {
  const size_t off = static_cast<size_t>(pos & r->mask);
  const size_t first = static_cast<size_t>(r->cap) - off;
  if (TACHYON_SDK_LIKELY(n <= first)) {
    memcpy(r->buf + off, src, n);
  } else {
    memcpy(r->buf + off, src, first);
    memcpy(r->buf, static_cast<const uint8_t*>(src) + first, n - first);
  }
}

// Consumer or test hook: copies n bytes starting at monotonic position `pos`.
inline void ringGet(const ThreadRing* r, uint64_t pos, void* dst, size_t n) {
  const size_t off = static_cast<size_t>(pos & r->mask);
  const size_t first = static_cast<size_t>(r->cap) - off;
  if (n <= first) {
    memcpy(dst, r->buf + off, n);
  } else {
    memcpy(dst, r->buf + off, first);
    memcpy(static_cast<uint8_t*>(dst) + first, r->buf, n - first);
  }
}

inline void ringPublish(ThreadRing* r, uint64_t newHead) {
  r->head.store(newHead, std::memory_order_release);
}

// Producer: counts a dropped record.
inline void ringCountDrop(ThreadRing* r, uint64_t n = 1) {
  r->dropTotal.store(r->dropTotal.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

#pragma pack(push, 1)
template <typename P>
struct Frame {
  inst::FrameHeader header;
  P payload;
};
#pragma pack(pop)

// Hash of a string key into the per-thread table.
inline uint32_t strSlot(uint64_t key) {
  return static_cast<uint32_t>((key * 0x9E3779B97F4A7C15ull) >> (64 - kStrTableBits));
}

inline bool strKnown(const ThreadRing* r, uint64_t key) {
  uint32_t i = strSlot(key);
  for (;;) {
    const uint64_t v = r->strTable[i];
    if (v == key) return true;
    if (v == 0) return false;
    i = (i + 1) & (kStrTableSize - 1);
  }
}

inline void strInsert(ThreadRing* r, uint64_t key) {
  if (TACHYON_SDK_UNLIKELY(r->strCount >= kStrTableMaxLoad)) {
    // Forget everything: strings are simply re-defined on next use, the consumer de-duplicates.
    memset(r->strTable, 0, sizeof(r->strTable));
    r->strCount = 0;
  }
  uint32_t i = strSlot(key);
  while (r->strTable[i] != 0) i = (i + 1) & (kStrTableSize - 1);
  r->strTable[i] = key;
  ++r->strCount;
}

}  // namespace tachyon::sdk
