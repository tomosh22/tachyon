// Runtime behaviour with the sidecar sink: the recorded stream parses into the expected records.
#include "core/time.h"
#include "runtime_fixture.h"

#include <tachyon/tachyon.h>

#include <cstring>
#include <set>
#include <string>
#include <thread>

namespace tachyon {
namespace {

using inst::FrameType;
using inst::test_support::Event;
using inst::test_support::ParsedStream;
using sdk_testing::RuntimeTest;

class SidecarTest : public RuntimeTest {};

std::vector<const Event*> byType(const ParsedStream& ps, FrameType type) {
  return ps.select(type);
}

TEST_F(SidecarTest, HeaderDescribesTheProcessAndClock) {
  const auto path = startSidecar();
  const uint64_t before = core::nowTicks();
  Tachyon_BeginEvent("x", nullptr, TACHYON_DEFAULT_COLOR);
  (void)Tachyon_EndEvent();
  const ParsedStream ps = finishAndParse(path);
  EXPECT_TRUE(ps.errors.empty()) << (ps.errors.empty() ? "" : ps.errors.front());
  ASSERT_TRUE(ps.headerSeen);
  EXPECT_EQ(+ps.header.magic, inst::kStreamMagic);
  EXPECT_EQ(+ps.header.wire_version, inst::kWireVersion);
  EXPECT_EQ(+ps.header.pid, sdk_testing::currentPidValue());
  EXPECT_EQ(+ps.header.clock_frequency_hz, core::ticksFrequency());
  EXPECT_EQ(+ps.header.api_version, static_cast<uint32_t>(TACHYON_API_VERSION));
#if defined(_WIN32)
  EXPECT_EQ(+ps.header.platform, static_cast<uint32_t>(inst::Platform::Windows));
#else
  EXPECT_EQ(+ps.header.platform, static_cast<uint32_t>(inst::Platform::Linux));
#endif
  EXPECT_LE(+ps.header.process_start_ts, core::nowTicks());
  EXPECT_GE(+ps.header.process_start_ts + 10'000'000'000ull, before);  // started recently
}

TEST_F(SidecarTest, AllRecordKindsRoundTrip) {
  const auto path = startSidecar();
  Tachyon_SetCurrentThreadName("main-test-thread");
  const uint32_t tid = sdk_testing::osThreadId();

  Tachyon_BeginEvent("outer", "payload", 0x11223344u);
  Tachyon_BeginEvent_N("inner-and-more", 5, "datum", 3, TACHYON_MAKE_COLOR(1, 2, 3));
  (void)Tachyon_EndEvent();
  (void)Tachyon_EndEvent();
  Tachyon_FrameMark("frame", 42);
  Tachyon_Marker("marker", "marker-data", 0xAABBCCDDu);
  Tachyon_Marker("marker-no-data", nullptr, 0);
  Tachyon_Plot("plot", 2.5);
  int lock = 0;
  Tachyon_LockAcquireBegin(&lock, "my lock");
  Tachyon_LockAcquireEnd(&lock);
  Tachyon_LockAcquireBegin(&lock, nullptr);
  Tachyon_LockAcquireEnd(&lock);
  {
    TACHYON_SCOPE_DATA("macro-scope", "macro-data");
    TACHYON_LOCK_SCOPE(lock, "macro lock");
  }

  const ParsedStream ps = finishAndParse(path);
  ASSERT_TRUE(ps.errors.empty()) << ps.errors.front();

  // Thread name.
  bool nameSeen = false;
  for (const Event* e : byType(ps, FrameType::ThreadName))
    if (e->tid == tid && e->data == "main-test-thread") nameSeen = true;
  EXPECT_TRUE(nameSeen);

  // Scopes in order: outer, inner, lock, lock, macro scope, macro lock.
  std::vector<const Event*> begins = byType(ps, FrameType::ScopeBegin);
  std::vector<const Event*> ends = byType(ps, FrameType::ScopeEnd);
  ASSERT_EQ(begins.size(), 6u);
  ASSERT_EQ(ends.size(), 6u);
  EXPECT_EQ(begins[0]->id, "outer");
  EXPECT_EQ(begins[0]->data, "payload");
  EXPECT_EQ(begins[0]->color, 0x11223344u);
  EXPECT_EQ(begins[0]->tid, tid);
  EXPECT_NE(begins[0]->caller, 0u);
  EXPECT_EQ(begins[1]->id, "inner");  // _N: length-limited id
  EXPECT_EQ(begins[1]->data, "dat");
  EXPECT_EQ(begins[1]->color, TACHYON_MAKE_COLOR(1, 2, 3));
  char hex[40];
  std::snprintf(hex, sizeof(hex), "0x%llx",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&lock)));
  EXPECT_EQ(begins[2]->id, "my lock");
  EXPECT_EQ(begins[2]->data, hex);
  EXPECT_EQ(begins[2]->color, static_cast<uint32_t>(TACHYON_LOCK_COLOR_SENTINEL));
  EXPECT_EQ(begins[3]->id, "Tachyon.LockAcquire");
  EXPECT_EQ(begins[3]->color, static_cast<uint32_t>(TACHYON_LOCK_COLOR_SENTINEL));
  EXPECT_EQ(begins[4]->id, "macro-scope");
  EXPECT_EQ(begins[4]->data, "macro-data");
  EXPECT_EQ(begins[5]->id, "macro lock");
  uint64_t lastTs = 0;
  for (const Event& e : ps.events) {
    if (e.type != FrameType::ScopeBegin && e.type != FrameType::ScopeEnd) continue;
    EXPECT_GE(e.ts, lastTs);  // one thread: timestamps never go backwards
    lastTs = e.ts;
  }

  // Frame mark, markers, plot.
  const auto frames = byType(ps, FrameType::FrameMark);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0]->id, "frame");
  EXPECT_EQ(frames[0]->number, 42u);
  EXPECT_EQ(frames[0]->tid, tid);
  const auto markers = byType(ps, FrameType::Marker);
  ASSERT_EQ(markers.size(), 2u);
  EXPECT_EQ(markers[0]->id, "marker");
  EXPECT_EQ(markers[0]->data, "marker-data");
  EXPECT_EQ(markers[0]->color, 0xAABBCCDDu);
  EXPECT_EQ(markers[1]->id, "marker-no-data");
  EXPECT_EQ(markers[1]->data, "");
  const auto plots = byType(ps, FrameType::Plot);
  ASSERT_EQ(plots.size(), 1u);
  EXPECT_EQ(plots[0]->id, "plot");
  EXPECT_DOUBLE_EQ(plots[0]->value, 2.5);
}

TEST_F(SidecarTest, StringsAreDefinedOncePerStream) {
  const auto path = startSidecar();
  static const char kShared[] = "shared-id";
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([] {
      for (int i = 0; i < 1000; ++i) {
        Tachyon_BeginEvent(kShared, nullptr, TACHYON_DEFAULT_COLOR);
        (void)Tachyon_EndEvent();
      }
    });
  }
  for (auto& t : threads) t.join();
  const ParsedStream ps = finishAndParse(path);
  ASSERT_TRUE(ps.errors.empty()) << ps.errors.front();  // includes "duplicate StringDef"
  int defs = 0;
  for (const auto& kv : ps.strings)
    if (kv.second == "shared-id") ++defs;
  EXPECT_EQ(defs, 1);
  EXPECT_EQ(byType(ps, FrameType::ScopeBegin).size(), 4000u);
}

TEST_F(SidecarTest, SamePointerWithDifferentLengthsGetsDistinctKeys) {
  const auto path = startSidecar();
  static const char kText[] = "abcdefgh";
  Tachyon_BeginEvent_N(kText, 3, nullptr, 0, 0);
  (void)Tachyon_EndEvent();
  Tachyon_BeginEvent_N(kText, 6, nullptr, 0, 0);
  (void)Tachyon_EndEvent();
  Tachyon_BeginEvent(kText, nullptr, 0);
  (void)Tachyon_EndEvent();
  const ParsedStream ps = finishAndParse(path);
  ASSERT_TRUE(ps.errors.empty()) << ps.errors.front();
  const auto begins = byType(ps, FrameType::ScopeBegin);
  ASSERT_EQ(begins.size(), 3u);
  EXPECT_EQ(begins[0]->id, "abc");
  EXPECT_EQ(begins[1]->id, "abcdef");
  EXPECT_EQ(begins[2]->id, "abcdefgh");
  EXPECT_EQ(ps.strings.size(), 3u);
  // Keys: low 48 bits are the pointer, the high 16 bits the length tag.
  for (const auto& kv : ps.strings) {
    EXPECT_EQ(kv.first & inst::kPtrKeyPointerMask,
              reinterpret_cast<uintptr_t>(kText) & inst::kPtrKeyPointerMask);
  }
}

TEST_F(SidecarTest, ManyDistinctIdsExceedingThePerThreadTable) {
  const auto path = startSidecar();
  std::vector<std::string> ids;
  ids.reserve(3000);
  for (int i = 0; i < 3000; ++i) ids.push_back("id-" + std::to_string(i));  // stable storage
  for (int round = 0; round < 2; ++round) {
    for (const std::string& id : ids) {
      Tachyon_BeginEvent(id.c_str(), nullptr, 0);
      (void)Tachyon_EndEvent();
    }
  }
  const ParsedStream ps = finishAndParse(path);
  ASSERT_TRUE(ps.errors.empty()) << ps.errors.front();
  std::set<std::string> seen;
  for (const Event* b : byType(ps, FrameType::ScopeBegin)) {
    EXPECT_EQ(b->id.rfind("id-", 0), 0u) << b->id;
    seen.insert(b->id);
  }
  // Whatever the 64 KiB ring could hold arrived intact (the rest is counted as dropped).
  EXPECT_GT(seen.size(), 100u);
}

TEST_F(SidecarTest, DataAndNamesAreTruncated) {
  const auto path = startSidecar();
  const std::string big(6000, 'x');
  Tachyon_BeginEvent("trunc", big.c_str(), 0);
  (void)Tachyon_EndEvent();
  Tachyon_BeginEvent_N("truncN", 6, big.data(), 5000, 0);
  (void)Tachyon_EndEvent();
  Tachyon_Marker("trunc-marker", big.c_str(), 0);
  const std::string longName(1000, 'n');
  Tachyon_SetCurrentThreadName(longName.c_str());
  Tachyon_BeginEvent(nullptr, nullptr, 0);  // null id is tolerated
  (void)Tachyon_EndEvent();
  const ParsedStream ps = finishAndParse(path);
  ASSERT_TRUE(ps.errors.empty()) << ps.errors.front();
  const auto begins = byType(ps, FrameType::ScopeBegin);
  ASSERT_EQ(begins.size(), 3u);
  EXPECT_EQ(begins[0]->data.size(), 4096u);
  EXPECT_EQ(begins[1]->data.size(), 4096u);
  EXPECT_EQ(begins[2]->id, "(null)");
  EXPECT_EQ(byType(ps, FrameType::Marker)[0]->data.size(), 4096u);
  size_t longest = 0;
  for (const Event* e : byType(ps, FrameType::ThreadName)) longest = std::max(longest, e->data.size());
  EXPECT_EQ(longest, 256u);
}

TEST_F(SidecarTest, ThreadNamesAndConcurrentFirstUse) {
  const auto path = startSidecar();
  std::vector<std::thread> threads;
  std::vector<uint32_t> tids(8);
  std::atomic<bool> go{false};
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      tids[static_cast<size_t>(t)] = sdk_testing::osThreadId();
      while (!go.load()) std::this_thread::yield();
      const std::string name = "worker-" + std::to_string(t);
      Tachyon_SetCurrentThreadName_N(name.data(), static_cast<uint16_t>(name.size()));
      Tachyon_BeginEvent("work", name.c_str(), 0);
      (void)Tachyon_EndEvent();
    });
  }
  go = true;
  for (auto& t : threads) t.join();
  const ParsedStream ps = finishAndParse(path);
  ASSERT_TRUE(ps.errors.empty()) << ps.errors.front();
  for (int t = 0; t < 8; ++t) {
    bool found = false;
    for (const Event* e : byType(ps, FrameType::ThreadName))
      if (e->tid == tids[static_cast<size_t>(t)] && e->data == "worker-" + std::to_string(t)) found = true;
    EXPECT_TRUE(found) << "thread " << t;
  }
  EXPECT_EQ(byType(ps, FrameType::ScopeBegin).size(), 8u);
  EXPECT_EQ(byType(ps, FrameType::ScopeEnd).size(), 8u);
}

TEST_F(SidecarTest, FiberEventsAndScopeStateAcrossSwitches) {
  const auto path = startSidecar();
  Tachyon_RegisterFiber(1);
  Tachyon_RegisterFiber(2);
  Tachyon_BeginEvent("fiber1-scope", nullptr, 0);
  Tachyon_BeginFiberSwitch(1, 2);
  // Another thread resumes fiber 1 and ends its scope there.
  std::thread other([] {
    Tachyon_EndFiberSwitch(1);
    (void)Tachyon_EndEvent();
    Tachyon_BeginFiberSwitch(1, 2);
  });
  other.join();
  Tachyon_EndFiberSwitch(2);
  Tachyon_BeginEvent("fiber2-scope", nullptr, 0);
  (void)Tachyon_EndEvent();
  Tachyon_UnregisterFiber(1);
  Tachyon_UnregisterFiber(2);
  const ParsedStream ps = finishAndParse(path);
  ASSERT_TRUE(ps.errors.empty()) << ps.errors.front();
  // Ordering across threads is only guaranteed per thread, so select by kind and thread.
  const uint32_t mainTid = sdk_testing::osThreadId();
  uint32_t otherTid = 0;
  int counts[4] = {0, 0, 0, 0};
  for (const Event* e : byType(ps, FrameType::FiberEvent)) {
    ASSERT_LT(e->kind, 4);
    ++counts[e->kind];
    if (e->kind == 3 && e->number == 1) otherTid = e->tid;  // fiber 1 resumed on the other thread
    if (e->kind == 2) {
      EXPECT_EQ(e->number, 1u);
      EXPECT_EQ(e->other, 2u);
    }
  }
  EXPECT_EQ(counts[0], 2);
  EXPECT_EQ(counts[1], 2);
  EXPECT_EQ(counts[2], 2);
  EXPECT_EQ(counts[3], 2);
  EXPECT_NE(otherTid, 0u);
  EXPECT_NE(otherTid, mainTid);
  for (const Event* e : byType(ps, FrameType::FiberEvent))
    if (e->kind == 3 && e->number == 2) EXPECT_EQ(e->tid, mainTid);
  // The End of fiber1-scope was recorded on the other thread; fiber2's scope End on the main thread.
  const auto ends = byType(ps, FrameType::ScopeEnd);
  ASSERT_EQ(ends.size(), 2u);
  std::set<uint32_t> endTids = {ends[0]->tid, ends[1]->tid};
  EXPECT_EQ(endTids, (std::set<uint32_t>{mainTid, otherTid}));
}

TEST_F(SidecarTest, NestingDeeperThanTheTrackedLimitIsCountedAsDropped) {
  const auto path = startSidecar();
  constexpr int kDepth = 300;
  for (int i = 0; i < kDepth; ++i) Tachyon_BeginEvent("deep", nullptr, 0);
  for (int i = 0; i < kDepth; ++i) (void)Tachyon_EndEvent();
  const uint64_t dropped = Tachyon_Internal_ThreadDropCount();
  const ParsedStream ps = finishAndParse(path);
  ASSERT_TRUE(ps.errors.empty()) << ps.errors.front();
  EXPECT_EQ(byType(ps, FrameType::ScopeBegin).size(), 256u);
  EXPECT_EQ(byType(ps, FrameType::ScopeEnd).size(), 256u);
  EXPECT_EQ(dropped, 2u * (kDepth - 256));
  uint64_t reported = 0;
  for (const Event* e : byType(ps, FrameType::Dropped)) reported += e->number;
  EXPECT_EQ(reported, dropped);
}

TEST_F(SidecarTest, RingSizeFromEnvironment) {
  sdk_testing::setEnv("TACHYON_RING_KB", "100");
  startSidecar();
  Tachyon_BeginEvent("x", nullptr, 0);
  (void)Tachyon_EndEvent();
  EXPECT_EQ(Tachyon_Internal_RingBytes(), 128u * 1024);  // rounded up to a power of two
  sdk_testing::setEnv("TACHYON_RING_KB", "1");
  startSidecar("second.tachyon-inst");
  Tachyon_BeginEvent("x", nullptr, 0);
  (void)Tachyon_EndEvent();
  EXPECT_EQ(Tachyon_Internal_RingBytes(), 16u * 1024);  // clamped to the minimum
  sdk_testing::unsetEnv("TACHYON_RING_KB");
  startSidecar("third.tachyon-inst");
  Tachyon_BeginEvent("x", nullptr, 0);
  (void)Tachyon_EndEvent();
  EXPECT_EQ(Tachyon_Internal_RingBytes(), 64u * 1024);  // default
}

TEST_F(SidecarTest, DefaultSidecarPathIsExeNameAndPidInCwd) {
  sdk_testing::unsetEnv("TACHYON_SIDECAR");
  Tachyon_Internal_Restart();
  ASSERT_EQ(Tachyon_Internal_WaitReady(5000), 2);
  Tachyon_BeginEvent("x", nullptr, 0);
  (void)Tachyon_EndEvent();
  const std::filesystem::path path = Tachyon_Internal_SidecarPath();
  const std::string suffix = "-" + std::to_string(sdk_testing::currentPidValue()) + ".tachyon-inst";
  EXPECT_EQ(std::filesystem::weakly_canonical(path.parent_path()), std::filesystem::weakly_canonical(m_dir));
  ASSERT_GE(path.filename().string().size(), suffix.size());
  EXPECT_EQ(path.filename().string().substr(path.filename().string().size() - suffix.size()), suffix);
  EXPECT_GT(path.filename().string().size(), suffix.size());  // has an exe name
  const ParsedStream ps = finishAndParse(path);
  EXPECT_TRUE(ps.errors.empty());
  EXPECT_EQ(byType(ps, FrameType::ScopeBegin).size(), 1u);
}

TEST_F(SidecarTest, ShutdownWhileOtherThreadsAreStillEmittingIsSafe) {
  const auto path = startSidecar();
  std::atomic<bool> stop{false};
  std::atomic<int> started{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      ++started;
      while (!stop.load()) {
        Tachyon_BeginEvent("busy", "d", 0);
        (void)Tachyon_EndEvent();
      }
    });
  }
  while (started.load() < 4) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Tachyon_Internal_Shutdown();  // what the atexit handler does
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  stop = true;
  for (auto& t : threads) t.join();
  const ParsedStream ps = ParsedStream::parse(sdk_testing::readFile(path));
  EXPECT_TRUE(ps.errors.empty()) << (ps.errors.empty() ? "" : ps.errors.front());
  EXPECT_GT(byType(ps, FrameType::ScopeBegin).size(), 0u);
  EXPECT_EQ(Tachyon_IsConnected(), 0);
}

// ---- runtime disabled -------------------------------------------------------------------------------------

TEST_F(SidecarTest, DisabledByEnvironmentRecordsNothingAndAllocatesNothing) {
  sdk_testing::setEnv("TACHYON_DISABLE", "1");
  sdk_testing::setEnv("TACHYON_SIDECAR", (m_dir / "never.tachyon-inst").string());
  Tachyon_Internal_Restart();
  Tachyon_SetCurrentThreadName("ignored");
  Tachyon_BeginEvent("x", "y", 0);
  (void)Tachyon_EndEvent();
  Tachyon_FrameMark("f", 1);
  Tachyon_Marker("m", nullptr, 0);
  Tachyon_Plot("p", 1.0);
  Tachyon_RegisterFiber(1);
  Tachyon_LockAcquireBegin(&m_dir, nullptr);
  Tachyon_LockAcquireEnd(&m_dir);
  EXPECT_EQ(Tachyon_Internal_RingBytes(), 0u);  // no ring was allocated
  EXPECT_EQ(Tachyon_IsConnected(), 0);
  EXPECT_EQ(Tachyon_RequestFlush("nope"), 0);
  EXPECT_EQ(Tachyon_Internal_WaitReady(100), 0);
  EXPECT_FALSE(std::filesystem::exists(m_dir / "never.tachyon-inst"));
}

TEST_F(SidecarTest, NoAgentAndSidecarOffDisablesRecording) {
  sdk_testing::setEnv("TACHYON_SIDECAR", "off");
  Tachyon_Internal_Restart();
  EXPECT_EQ(Tachyon_Internal_WaitReady(5000), 0);
  Tachyon_BeginEvent("x", nullptr, 0);
  (void)Tachyon_EndEvent();
  EXPECT_EQ(Tachyon_IsConnected(), 0);
  size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(m_dir)) {
    (void)entry;
    ++files;
  }
  EXPECT_EQ(files, 0u);
}

}  // namespace
}  // namespace tachyon
