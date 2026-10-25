// Allocator that backs large arrays with 2 MiB-aligned memory and asks Linux for transparent huge pages.
#pragma once

#include "core/platform.h"

#include <cstddef>
#include <cstdlib>
#include <new>
#include <type_traits>

#if TACHYON_PLATFORM_LINUX
#include <sys/mman.h>
#endif

namespace tachyon::trace {

// Stateless; all instances compare equal. Thread-safe.
template <typename T>
struct HugePageAllocator {
  using value_type = T;
  using is_always_equal = std::true_type;

  HugePageAllocator() = default;
  template <typename U>
  HugePageAllocator(const HugePageAllocator<U>&) {}

  static constexpr size_t kHugeSize = size_t{2} * 1024 * 1024;

  // Thread-safe.
  T* allocate(size_t n) {
    const size_t bytes = n * sizeof(T);
#if TACHYON_PLATFORM_LINUX
    if (bytes >= kHugeSize) {
      const size_t rounded = (bytes + kHugeSize - 1) & ~(kHugeSize - 1);
      void* p = std::aligned_alloc(kHugeSize, rounded);
      if (!p) throw std::bad_alloc();
      madvise(p, rounded, MADV_HUGEPAGE);  // best effort; failure only costs speed
      return static_cast<T*>(p);
    }
#endif
    return static_cast<T*>(::operator new(bytes));
  }

  // Thread-safe.
  void deallocate(T* p, [[maybe_unused]] size_t n) {
#if TACHYON_PLATFORM_LINUX
    if (n * sizeof(T) >= kHugeSize) {
      std::free(p);
      return;
    }
#endif
    ::operator delete(p);
  }

  template <typename U>
  bool operator==(const HugePageAllocator<U>&) const {
    return true;
  }
};

}  // namespace tachyon::trace
