// SDK client runtime: the registry of per-thread rings, the flush thread, the agent/sidecar connection and
// the string-definition de-duplication set.
//
// This is the one sanctioned exception to "no global mutable state / no singletons" (Part 2.4): the C ABI has
// no context parameter, so all process-wide state lives in a single explicitly constructed Runtime object,
// created lazily and thread-safely on first API use (Runtime* runtime()). It is never destroyed: the atexit
// handler shuts it down (drains and closes the sink) but leaves the memory valid, so threads that are still
// emitting while the process exits only ever touch live memory.
#pragma once

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "sdk_platform.h"
#include "sdk_ring.h"

namespace tachyon::sdk {

enum RuntimeState : uint32_t {
  kStateUninit = 0,    // no API call yet
  kStateActive = 1,    // recording (probing for a sink, or a sink is attached)
  kStateDisabled = 2,  // TACHYON_DISABLE, no sink available, or shut down: API calls return immediately
};

// The only hot-path global: every API call starts with one relaxed load of this.
extern std::atomic<uint32_t> g_state;

struct RuntimeConfig {
  bool disabled = false;       // TACHYON_DISABLE=1
  std::string agentAddress;    // TACHYON_AGENT_PIPE or the default address
  bool sidecarEnabled = true;  // TACHYON_SIDECAR != off
  std::string sidecarPath;     // resolved sidecar file path
  uint64_t ringBytes = kDefaultRingBytes;
};

// Reads the environment. Thread-safe.
RuntimeConfig loadRuntimeConfig();

class Runtime {
 public:
  explicit Runtime(RuntimeConfig config);
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // Thread-safe, call once. Starts the flush thread.
  bool start();
  // Thread-safe, idempotent. Final drain, close sink. Leaves ring memory valid.
  void shutdown();

  // Thread-safe. Allocates a ring and registers it in the global thread list. Null on allocation failure.
  ThreadRing* registerThread();
  // Thread-safe. Marks the ring retired; the flush thread frees it after draining.
  void retireRing(ThreadRing* ring);

  // Thread-safe. True while streaming to an agent.
  bool agentConnected() const { return m_agentConnected.load(std::memory_order_relaxed); }
  // Thread-safe, synchronous: one control-channel round trip (flush command).
  int requestFlush(const char* reason);

  // Fiber scope-stack bookkeeping. Thread-safe.
  void registerFiber(uint64_t fiberId);
  void unregisterFiber(uint64_t fiberId);
  void saveFiber(uint64_t fiberId, const ScopeStack& stack);
  bool loadFiber(uint64_t fiberId, ScopeStack& out);

  // Test hooks. Thread-safe.
  int waitReady(int timeoutMs);  // 1 agent, 2 sidecar, 0 none/timeout
  void flushNow();               // returns after the rings were drained into the sink
  std::string sidecarPath();     // path of the sidecar file currently/last open ("" if none)
  uint64_t ringBytes() const { return m_config.ringBytes; }

 private:
  enum class SinkKind { None, Agent, Sidecar };
  struct FiberShard {
    std::mutex mutex;
    std::unordered_map<uint64_t, ScopeStack> map;
  };

  void run();
  void establishSink();
  bool tryAgent(int timeoutMs);
  bool trySidecar();
  bool beginStream(Channel& channel);
  void onSinkFailure();
  void setReady(SinkKind kind);
  size_t drainAll();
  size_t drainRing(ThreadRing* ring, bool& retiredDrained);
  void processFrames(const uint8_t* data, size_t size);
  void appendOut(const void* data, size_t size);
  void writeOut();
  void pumpControl();
  void wake();
  bool waitThreadExit(int timeoutMs);
  FiberShard& fiberShard(uint64_t id) { return m_fiberShards[(id * 0x9E3779B97F4A7C15ull) >> 58]; }

  RuntimeConfig m_config;
  uint64_t m_startTicks = 0;

  // Ring registry.
  std::mutex m_ringsMutex;
  std::vector<ThreadRing*> m_rings;
  std::vector<ThreadRing*> m_snapshot;  // flush thread / final drain only

  // Flush thread.
  std::thread m_thread;
  std::atomic<bool> m_stop{false};
  std::mutex m_wakeMutex;
  std::condition_variable m_wakeCv;
  std::mutex m_exitMutex;
  std::condition_variable m_exitCv;
  bool m_threadDone = false;
  std::atomic<uint64_t> m_cycles{0};
  std::mutex m_shutdownMutex;
  std::atomic<bool> m_shutDown{false};
  bool m_started = false;
  bool m_timerPeriodRaised = false;  // Windows: timeBeginPeriod(1) in effect

  // Sink (flush thread, then the shutdown caller after the join).
  Channel m_sink;
  SinkKind m_sinkKind = SinkKind::None;
  uint64_t m_nextAgentRetryMs = 0;
  std::vector<uint8_t> m_out;
  std::vector<uint8_t> m_scratch;
  std::unordered_map<uint64_t, std::vector<uint8_t>> m_stringFrames;      // key -> complete StringDef frame
  std::unordered_map<uint32_t, std::vector<uint8_t>> m_threadNameFrames;  // tid -> complete ThreadName frame
  std::atomic<bool> m_agentConnected{false};
  std::atomic<int> m_ready{0};  // 0 undecided, 1 agent, 2 sidecar, 3 none
  std::mutex m_pathMutex;
  std::string m_activeSidecarPath;

  // Control channel (shared with Tachyon_RequestFlush).
  std::mutex m_controlMutex;
  Channel m_control;
  int64_t m_nextRequestId = 1;
  uint32_t m_pumpCounter = 0;

  FiberShard m_fiberShards[64];
};

// Thread-safe. The process-wide runtime; created on first call (does nothing and returns null when the
// environment disables the SDK). Returns the same pointer for the life of the process (see header comment).
Runtime* runtime();
// Thread-safe. The runtime if it already exists, else null (never creates it).
Runtime* runtimeIfCreated();

// Test hook: shuts the runtime down and re-arms lazy creation. Every other thread that ever emitted must have
// exited (or be quiescent forever); the calling thread's ring is dropped.
void restartRuntimeForTests();

}  // namespace tachyon::sdk
