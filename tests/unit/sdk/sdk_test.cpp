// SDK behaviour tests: caller-address capture, data handling, loader, thread isolation.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include <tachyon/tachyon.h>
#include <tachyon/tachyon_loader.h>

#include "sdk_internal.h"

namespace {

// Caller-address bounds check.
//
// The helper functions below are compiled without optimisation (optnone / O0) so that basic blocks keep
// source order, and they take the addresses of two labels (GNU labels-as-values) placed immediately before
// and after the call under test. A return address recorded by the callee must then satisfy lo < ret <= hi,
// independent of the optimisation level of the rest of the build (-O2, ASan). The function is also
// noinline/noclone so it has a real frame. On MSVC (no labels-as-values) the test is skipped; see the WP-50
// report.
#if defined(__GNUC__) || defined(__clang__)
#define TACHYON_TEST_HAS_LABELS 1
#if defined(__clang__)
#define TACHYON_TEST_NOOPT __attribute__((noinline, optnone))
#else
#define TACHYON_TEST_NOOPT __attribute__((noinline, noclone, optimize("O0")))
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wgnu-label-as-value"

struct Bounds {
  const void* lo;
  const void* hi;
  const void* recorded;
};

TACHYON_TEST_NOOPT Bounds callRawBegin() {
  Bounds b{};
  Tachyon_Internal_ResetThreadState();
  b.lo = &&before;
before:
  Tachyon_BeginEvent("raw", "payload", TACHYON_DEFAULT_COLOR);
  b.hi = &&after;
after:
  b.recorded = Tachyon_Internal_LastBeginCaller();
  (void)Tachyon_EndEvent();
  return b;
}

TACHYON_TEST_NOOPT Bounds callBeginN() {
  Bounds b{};
  Tachyon_Internal_ResetThreadState();
  b.lo = &&before;
before:
  Tachyon_BeginEvent_N("rawN", 4, "xy", 2, TACHYON_DEFAULT_COLOR);
  b.hi = &&after;
after:
  b.recorded = Tachyon_Internal_LastBeginCaller();
  (void)Tachyon_EndEvent();
  return b;
}

TACHYON_TEST_NOOPT Bounds callScopeMacro() {
  Bounds b{};
  Tachyon_Internal_ResetThreadState();
  b.lo = &&before;
before: {
  TACHYON_SCOPE("macro");
  b.recorded = Tachyon_Internal_LastBeginCaller();
  b.hi = &&after;
}
after:
  return b;
}

#pragma GCC diagnostic pop

void expectWithin(const Bounds& b) {
  ASSERT_NE(b.recorded, nullptr);
  const auto lo = reinterpret_cast<uintptr_t>(b.lo);
  const auto hi = reinterpret_cast<uintptr_t>(b.hi);
  const auto ret = reinterpret_cast<uintptr_t>(b.recorded);
  EXPECT_LT(lo, ret);
  EXPECT_LE(ret, hi);
  EXPECT_LT(hi - lo, 4096u);  // sanity: the labelled region is tiny
}

TEST(SdkCallerAddress, RawBeginRecordsCallSite) {
  expectWithin(callRawBegin());
}
TEST(SdkCallerAddress, BeginNRecordsCallSite) {
  expectWithin(callBeginN());
}
TEST(SdkCallerAddress, ScopeMacroRecordsUserFunction) {
  expectWithin(callScopeMacro());
}
#else
TEST(SdkCallerAddress, SkippedWithoutLabelsAsValues) {
  GTEST_SKIP() << "needs GNU labels-as-values";
}
#endif

// Each thread has an independent slot, and the caller address differs per call site.
TEST(SdkCallerAddress, PerThreadSlots) {
  Tachyon_Internal_ResetThreadState();
  Tachyon_BeginEvent("main", nullptr, TACHYON_DEFAULT_COLOR);
  const void* mainCaller = Tachyon_Internal_LastBeginCaller();
  (void)Tachyon_EndEvent();
  const void* threadCaller = nullptr;
  const void* threadInitial = reinterpret_cast<const void*>(1);
  std::thread t([&] {
    threadInitial = Tachyon_Internal_LastBeginCaller();
    Tachyon_BeginEvent("thread", nullptr, TACHYON_DEFAULT_COLOR);
    threadCaller = Tachyon_Internal_LastBeginCaller();
    (void)Tachyon_EndEvent();
  });
  t.join();
  EXPECT_EQ(threadInitial, nullptr);
  EXPECT_NE(threadCaller, nullptr);
  EXPECT_NE(threadCaller, mainCaller);
  EXPECT_EQ(Tachyon_Internal_LastBeginCaller(), mainCaller);
}

TEST(SdkBegin, DataIsCopiedAndTruncated) {
  Tachyon_BeginEvent("d", "hello", 0x11223344u);
  EXPECT_STREQ(Tachyon_Internal_LastBeginData(), "hello");
  EXPECT_EQ(Tachyon_Internal_LastBeginColor(), 0x11223344u);
  Tachyon_BeginEvent("d", nullptr, TACHYON_DEFAULT_COLOR);
  EXPECT_STREQ(Tachyon_Internal_LastBeginData(), "");
  const std::string big(6000, 'x');
  Tachyon_BeginEvent("d", big.c_str(), TACHYON_DEFAULT_COLOR);
  EXPECT_EQ(std::strlen(Tachyon_Internal_LastBeginData()), 4096u);
  Tachyon_BeginEvent_N("d", 1, big.data(), static_cast<uint16_t>(5000), TACHYON_DEFAULT_COLOR);
  EXPECT_EQ(std::strlen(Tachyon_Internal_LastBeginData()), 4096u);
  Tachyon_BeginEvent_N("d", 1, "abcdef", 3, TACHYON_DEFAULT_COLOR);
  EXPECT_STREQ(Tachyon_Internal_LastBeginData(), "abc");
}

TEST(SdkBegin, LockAcquireUsesHexAddressAndSentinel) {
  int lock = 0;
  Tachyon_LockAcquireBegin(&lock, "lock");
  char expected[40];
  std::snprintf(expected, sizeof(expected), "0x%llx",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&lock)));
  EXPECT_STREQ(Tachyon_Internal_LastBeginData(), expected);
  EXPECT_EQ(Tachyon_Internal_LastBeginColor(), TACHYON_LOCK_COLOR_SENTINEL);
  Tachyon_LockAcquireEnd(&lock);
  Tachyon_LockAcquireBegin(nullptr, nullptr);
  EXPECT_STREQ(Tachyon_Internal_LastBeginData(), "0x0");
}

TEST(SdkApi, StatusFunctions) {
  EXPECT_EQ(Tachyon_IsConnected(), 0);
  EXPECT_EQ(Tachyon_RequestFlush("why"), 0);
  EXPECT_EQ(sizeof(TachyonSuppressTailCall), 24u);
  EXPECT_EQ(sizeof(TachyonFunctions), 16 * sizeof(void*));
}

TEST(SdkApi, GetApiVersionCheck) {
  TachyonFunctions f;
  std::memset(&f, 0xAB, sizeof(f));
  EXPECT_EQ(Tachyon_GetAPI(TACHYON_API_VERSION, &f), 1);
  EXPECT_EQ(f.EndEvent, &Tachyon_EndEvent);
  EXPECT_EQ(f.RequestFlush, &Tachyon_RequestFlush);
  std::memset(&f, 0xAB, sizeof(f));
  EXPECT_EQ(Tachyon_GetAPI(0x00020000, &f), 0);
  TachyonFunctions zero;
  std::memset(&zero, 0, sizeof(zero));
  EXPECT_EQ(std::memcmp(&f, &zero, sizeof(f)), 0);
  EXPECT_EQ(Tachyon_GetAPI(TACHYON_API_VERSION, nullptr), 0);
}

TEST(SdkLoader, LoadsSharedLibrary) {
  TachyonFunctions f;
  std::memset(&f, 0xAB, sizeof(f));
  ASSERT_EQ(Tachyon_LoadFrom(TACHYON_TEST_SHARED_LIB, &f), 1);
  ASSERT_NE(f.BeginEvent, nullptr);
  ASSERT_NE(f.EndEvent, nullptr);
  // Every slot is populated.
  const void* const* slots = reinterpret_cast<const void* const*>(&f);
  for (size_t i = 0; i < sizeof(f) / sizeof(void*); ++i) EXPECT_NE(slots[i], nullptr) << "slot " << i;
  f.SetCurrentThreadName("loaded");
  f.BeginEvent("x", "y", TACHYON_DEFAULT_COLOR);
  (void)f.EndEvent();
  EXPECT_EQ(f.IsConnected(), 0);
  EXPECT_EQ(f.RequestFlush("r"), 0);
  // The shared library has its own implementation, distinct from the static one linked into this test.
  EXPECT_NE(reinterpret_cast<const void*>(f.BeginEvent), reinterpret_cast<const void*>(&Tachyon_BeginEvent));
}

TEST(SdkLoader, FailureZeroesTable) {
  TachyonFunctions f;
  std::memset(&f, 0xAB, sizeof(f));
  TachyonFunctions zero;
  std::memset(&zero, 0, sizeof(zero));

  // Missing file.
  EXPECT_EQ(Tachyon_LoadFrom("/nonexistent/libnope.so", &f), 0);
  EXPECT_EQ(std::memcmp(&f, &zero, sizeof(f)), 0);

  // Existing library without the Tachyon_GetAPI symbol.
  std::memset(&f, 0xAB, sizeof(f));
  EXPECT_EQ(Tachyon_LoadFrom(TACHYON_TEST_NO_SYMBOL_LIB, &f), 0);
  EXPECT_EQ(std::memcmp(&f, &zero, sizeof(f)), 0);

  // Null arguments.
  std::memset(&f, 0xAB, sizeof(f));
  EXPECT_EQ(Tachyon_LoadFrom(nullptr, &f), 0);
  EXPECT_EQ(std::memcmp(&f, &zero, sizeof(f)), 0);
  EXPECT_EQ(Tachyon_LoadFrom(TACHYON_TEST_SHARED_LIB, nullptr), 0);
}

}  // namespace
