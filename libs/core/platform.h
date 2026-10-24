// Platform, compiler, and attribute macros. Every macro is always defined to 0 or 1 so code can use `#if`.
#pragma once

#if defined(_WIN32)
#define TACHYON_PLATFORM_WINDOWS 1
#else
#define TACHYON_PLATFORM_WINDOWS 0
#endif

#if defined(__linux__)
#define TACHYON_PLATFORM_LINUX 1
#else
#define TACHYON_PLATFORM_LINUX 0
#endif

#if defined(_M_X64) || defined(__x86_64__)
#define TACHYON_ARCH_X64 1
#else
#define TACHYON_ARCH_X64 0
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define TACHYON_COMPILER_MSVC 1
#else
#define TACHYON_COMPILER_MSVC 0
#endif

#if defined(__clang__)
#define TACHYON_COMPILER_CLANG 1
#else
#define TACHYON_COMPILER_CLANG 0
#endif

#if defined(__GNUC__) && !defined(__clang__)
#define TACHYON_COMPILER_GCC 1
#else
#define TACHYON_COMPILER_GCC 0
#endif

#if TACHYON_COMPILER_MSVC
#define TACHYON_NOINLINE __declspec(noinline)
#define TACHYON_FORCEINLINE __forceinline
#define TACHYON_LIKELY(x) (x)
#define TACHYON_UNLIKELY(x) (x)
#define TACHYON_RETURN_ADDRESS() _ReturnAddress()
#else
#define TACHYON_NOINLINE __attribute__((noinline))
#define TACHYON_FORCEINLINE inline __attribute__((always_inline))
#define TACHYON_LIKELY(x) __builtin_expect(!!(x), 1)
#define TACHYON_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define TACHYON_RETURN_ADDRESS() __builtin_return_address(0)
#endif

#if TACHYON_COMPILER_MSVC
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#endif

namespace tachyon {

// On-disk / protocol platform identifiers (FileHeader.platform, Part 3.1).
enum class PlatformId : unsigned { Windows = 1, Linux = 2 };

// Thread-safe: constant.
constexpr PlatformId currentPlatform() {
#if TACHYON_PLATFORM_WINDOWS
  return PlatformId::Windows;
#else
  return PlatformId::Linux;
#endif
}

}  // namespace tachyon
