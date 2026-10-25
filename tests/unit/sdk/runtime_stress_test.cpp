// Multi-thread stress: many threads emit nested scopes carrying "t<thread>-<seq>" data into a fake agent
// whose reader validates the stream incrementally (the full stream is too large to keep in memory). Checks
// for corruption, per-thread sequence order, Begin/End matching, and exact accounting of drops.
#include "runtime_fixture.h"

#include <tachyon/tachyon.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#ifndef TACHYON_STRESS_SCOPES_PER_THREAD
#define TACHYON_STRESS_SCOPES_PER_THREAD 1000000
#endif

namespace tachyon {
namespace {

using inst::FrameType;
using sdk_testing::RuntimeTest;

constexpr int kThreads = 16;
constexpr int kOuterPerThread = TACHYON_STRESS_SCOPES_PER_THREAD;

struct PerTid {
  int threadIndex = -1;
  uint64_t lastSeq = 0;
  bool haveSeq = false;
  uint64_t begins = 0, ends = 0, dropped = 0;
  uint64_t depth = 0;
  uint64_t lastTs = 0;
};

// Validates frames as they arrive (called from the fake agent's single reader thread per connection).
class StreamValidator {
 public:
  StreamValidator() : m_splitter([this](FrameType t, const uint8_t* p, uint32_t n) { onFrame(t, p, n); }) {}

  void feed(const uint8_t* data, size_t size) { m_splitter.feed(data, size); }

  // Parses exactly "t<digits>-<digits>".
  static bool parseData(const std::string& s, int& threadIndex, unsigned long long& seq) {
    size_t i = 0;
    if (s.empty() || s[i++] != 't') return false;
    size_t digits = 0;
    int ti = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ti = ti * 10 + (s[i++] - '0');
      ++digits;
    }
    if (digits == 0 || i >= s.size() || s[i++] != '-') return false;
    unsigned long long sq = 0;
    digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      sq = sq * 10 + static_cast<unsigned long long>(s[i++] - '0');
      ++digits;
    }
    if (digits == 0 || i != s.size()) return false;
    threadIndex = ti;
    seq = sq;
    return true;
  }

  void fail(const std::string& what) {
    if (m_errors.size() < 10) m_errors.push_back(what);
  }

  void onFrame(FrameType type, const uint8_t* p, uint32_t n) {
    if (!m_headerSeen) {
      inst::StreamHeaderPayload h;
      if (type != FrameType::StreamHeader || n < sizeof(h)) return fail("first frame is not a StreamHeader");
      std::memcpy(&h, p, sizeof(h));
      if (h.magic != inst::kStreamMagic) fail("bad magic");
      m_headerSeen = true;
      return;
    }
    switch (type) {
      case FrameType::StringDef: {
        inst::StringDefPayload d;
        if (n < sizeof(d)) return fail("short StringDef");
        std::memcpy(&d, p, sizeof(d));
        if (n != sizeof(d) + d.len) return fail("bad StringDef length");
        if (!m_defined.insert(uint64_t{d.ptr_key}).second) fail("duplicate StringDef");
        return;
      }
      case FrameType::ScopeBegin: {
        inst::ScopeBeginPayload b;
        if (n < sizeof(b)) return fail("short ScopeBegin");
        std::memcpy(&b, p, sizeof(b));
        if (n != sizeof(b) + b.data_len) return fail("bad ScopeBegin length");
        if (m_defined.count(uint64_t{b.id_key}) == 0) return fail("Begin references undefined string");
        PerTid& t = m_threads[uint32_t{b.tid}];
        const std::string data(reinterpret_cast<const char*>(p + sizeof(b)), b.data_len);
        int threadIndex = -1;
        unsigned long long seq = 0;
        if (!parseData(data, threadIndex, seq)) return fail("corrupt data: " + data);
        if (t.threadIndex == -1) t.threadIndex = threadIndex;
        if (t.threadIndex != threadIndex) fail("records from two threads share a tid");
        if (t.haveSeq && seq <= t.lastSeq) fail("sequence not increasing");
        t.haveSeq = true;
        t.lastSeq = seq;
        if (b.ts < t.lastTs) fail("timestamp went backwards");
        t.lastTs = b.ts;
        ++t.begins;
        ++t.depth;
        return;
      }
      case FrameType::ScopeEnd: {
        inst::ScopeEndPayload e;
        if (n != sizeof(e)) return fail("bad ScopeEnd length");
        std::memcpy(&e, p, sizeof(e));
        PerTid& t = m_threads[uint32_t{e.tid}];
        if (t.depth == 0) return fail("End without Begin");
        if (e.ts < t.lastTs) fail("timestamp went backwards");
        t.lastTs = e.ts;
        --t.depth;
        ++t.ends;
        return;
      }
      case FrameType::Dropped: {
        inst::DroppedPayload d;
        if (n != sizeof(d)) return fail("bad Dropped length");
        std::memcpy(&d, p, sizeof(d));
        m_threads[uint32_t{d.tid}].dropped += d.count;
        return;
      }
      case FrameType::ThreadName: return;
      default: return fail("unexpected frame type");
    }
  }

  inst::test_support::FrameSplitter m_splitter;
  bool m_headerSeen = false;
  std::set<uint64_t> m_defined;
  std::map<uint32_t, PerTid> m_threads;
  std::vector<std::string> m_errors;
};

TEST(RuntimeStress, SixteenThreadsNoCorruptionAndExactAccounting) {
  struct Restore {
    ~Restore() {
      Tachyon_Internal_Restart();
      sdk_testing::setEnv("TACHYON_AGENT_PIPE", "no-such-agent-for-sdk-tests");
      sdk_testing::setEnv("TACHYON_SIDECAR", "sdk_tests.tachyon-inst");
    }
  } restore;
  sdk_testing::unsetEnv("TACHYON_DISABLE");
  sdk_testing::unsetEnv("TACHYON_RING_KB");

  StreamValidator validator;
  std::mutex validatorMutex;  // one connection => one reader thread, but keep it explicit
  auto agent = std::make_unique<sdk_testing::FakeAgent>(sdk_testing::uniqueAgentAddress("stress"),
                                                        [&](const uint8_t* data, size_t size) {
                                                          std::lock_guard<std::mutex> lock(validatorMutex);
                                                          validator.feed(data, size);
                                                        });
  ASSERT_TRUE(agent->ok());
  sdk_testing::setEnv("TACHYON_AGENT_PIPE", agent->address());
  sdk_testing::setEnv("TACHYON_SIDECAR", "off");
  Tachyon_Internal_Restart();
  ASSERT_EQ(Tachyon_Internal_WaitReady(5000), 1);

  struct Emitted {
    uint32_t tid = 0;
    uint64_t scopes = 0;
  };
  std::vector<Emitted> emitted(kThreads);
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      emitted[static_cast<size_t>(t)].tid = sdk_testing::osThreadId();
      while (!go.load()) std::this_thread::yield();
      uint64_t seq = 0;
      uint64_t scopes = 0;
      char buf[48];
      for (int i = 0; i < kOuterPerThread; ++i) {
        std::snprintf(buf, sizeof(buf), "t%d-%llu", t, static_cast<unsigned long long>(++seq));
        Tachyon_BeginEvent("stress-outer", buf, 0);
        ++scopes;
        if (i % 4 == 0) {
          std::snprintf(buf, sizeof(buf), "t%d-%llu", t, static_cast<unsigned long long>(++seq));
          Tachyon_BeginEvent("stress-inner", buf, 0);
          ++scopes;
          (void)Tachyon_EndEvent();
        }
        (void)Tachyon_EndEvent();
        if ((i & 255) == 255)
          std::this_thread::yield();  // let the single drain thread compete with 16 producers
      }
      emitted[static_cast<size_t>(t)].scopes = scopes;
    });
  }
  go = true;
  for (auto& t : threads) t.join();

  Tachyon_Internal_Restart();  // final drain, closes the stream
  ASSERT_TRUE(agent->waitForInstClosed(1, 30000));
  agent.reset();

  std::lock_guard<std::mutex> lock(validatorMutex);
  EXPECT_TRUE(validator.m_headerSeen);
  EXPECT_EQ(validator.m_splitter.error(), "");
  EXPECT_EQ(validator.m_splitter.leftoverBytes(), 0u);
  for (const std::string& e : validator.m_errors) ADD_FAILURE() << e;

  uint64_t totalEmitted = 0, totalReceived = 0, totalDropped = 0;
  for (const Emitted& e : emitted) {
    const PerTid& t = validator.m_threads[uint32_t{e.tid}];
    EXPECT_EQ(t.begins, t.ends) << "tid " << e.tid;
    EXPECT_EQ(t.depth, 0u) << "tid " << e.tid;
    // Exact per-thread accounting: every scope is two records.
    EXPECT_EQ(t.begins + t.ends + t.dropped, 2 * e.scopes) << "tid " << e.tid;
    totalEmitted += 2 * e.scopes;
    totalReceived += t.begins + t.ends;
    totalDropped += t.dropped;
  }
  EXPECT_EQ(totalReceived + totalDropped, totalEmitted);
  EXPECT_GT(totalReceived, 0u);
  RecordProperty("emitted_records", std::to_string(totalEmitted));
  RecordProperty("received_records", std::to_string(totalReceived));
  RecordProperty("dropped_records", std::to_string(totalDropped));
  std::printf("[stress] emitted=%llu received=%llu dropped=%llu\n",
              static_cast<unsigned long long>(totalEmitted), static_cast<unsigned long long>(totalReceived),
              static_cast<unsigned long long>(totalDropped));
}

}  // namespace
}  // namespace tachyon
