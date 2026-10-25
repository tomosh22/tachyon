// Replaces the global operator new/delete of the analysis_tests binary with versions that keep a count of the
// live requested bytes, so tests can compare the QueryCache byte estimate with the real heap footprint.
#include "alloc_counter.h"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<int64_t> g_liveBytes{0};
constexpr size_t kHeader = 16;  // keeps the default 16-byte alignment

void* allocate(size_t n) {
  void* p = std::malloc(n + kHeader);
  if (p == nullptr) throw std::bad_alloc();
  *static_cast<size_t*>(p) = n;
  g_liveBytes.fetch_add(static_cast<int64_t>(n), std::memory_order_relaxed);
  return static_cast<char*>(p) + kHeader;
}

void release(void* p) noexcept {
  if (p == nullptr) return;
  char* base = static_cast<char*>(p) - kHeader;
  g_liveBytes.fetch_sub(static_cast<int64_t>(*reinterpret_cast<size_t*>(base)), std::memory_order_relaxed);
  std::free(base);
}
}  // namespace

namespace tachyon::testutil {
int64_t liveHeapBytes() {
  return g_liveBytes.load(std::memory_order_relaxed);
}
}  // namespace tachyon::testutil

void* operator new(size_t n) {
  return allocate(n);
}
void* operator new[](size_t n) {
  return allocate(n);
}
void operator delete(void* p) noexcept {
  release(p);
}
void operator delete[](void* p) noexcept {
  release(p);
}
void operator delete(void* p, size_t) noexcept {
  release(p);
}
void operator delete[](void* p, size_t) noexcept {
  release(p);
}
void* operator new(size_t n, const std::nothrow_t&) noexcept {
  try {
    return allocate(n);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
  try {
    return allocate(n);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
  release(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
  release(p);
}
