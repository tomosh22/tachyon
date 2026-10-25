// Tachyon engine integration example (WP-55): a fake engine with a job system, a fiber scheduler, a lock
// wrapper, frame marks, streaming I/O, a hitch trigger and console commands, all instrumented the way
// docs/engine-integration.md recommends.
//
// Built twice by CI: with TACHYON_ENABLED=1 linked against tachyon::sdk, and with TACHYON_ENABLED=0 linked
// against nothing (the include path only). Every call into the SDK goes through the small `prof` layer below,
// which is the only place an engine needs to know about Tachyon.
//
// Usage: engine_hooks_example [--frames N] [--workers N] [--console "<command>"]
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tachyon/tachyon.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

// ------------------------------------------------------------------------------------------------------
// Engine profiling layer: the only code that touches Tachyon directly.
// ------------------------------------------------------------------------------------------------------
namespace engine::prof {

// Default colors per subsystem (see the guide). 0xRRGGBBAA via TACHYON_MAKE_COLOR.
constexpr uint32_t kColorJobs = TACHYON_MAKE_COLOR(0x4C, 0xAF, 0x50);
constexpr uint32_t kColorRender = TACHYON_MAKE_COLOR(0x21, 0x96, 0xF3);
constexpr uint32_t kColorSync = TACHYON_MAKE_COLOR(0xF4, 0x43, 0x36);
constexpr uint32_t kColorMarker = TACHYON_MAKE_COLOR(0xE9, 0x1E, 0x63);

// Thread-safe. Names the calling thread for the OS (debuggers, ETW, perf) and for Tachyon. Call first thing
// in every thread entry point, before any scope.
void setThreadName(const char* name) {
#if defined(_WIN32)
  wchar_t wide[64] = {};
  MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 63);
  SetThreadDescription(GetCurrentThread(), wide);
#else
  char shortName[16] = {};  // Linux limits comm names to 15 characters; Tachyon keeps the full name.
  std::strncpy(shortName, name, sizeof(shortName) - 1);
  pthread_setname_np(pthread_self(), shortName);
#endif
#if TACHYON_ENABLED
  Tachyon_SetCurrentThreadName(name);
#endif
}

// Thread-safe. Instant marker on the timeline (worker wake-ups, level loads, ...).
void marker(const char* staticName, const char* data, uint32_t color) {
#if TACHYON_ENABLED
  Tachyon_Marker(staticName, data, color);
#else
  (void)staticName;
  (void)data;
  (void)color;
#endif
}

// Thread-safe. Numeric time series (GPU frame time, queue depth, ...).
void plot(const char* staticName, double value) {
#if TACHYON_ENABLED
  Tachyon_Plot(staticName, value);
#else
  (void)staticName;
  (void)value;
#endif
}

// Thread-safe. Returns true when the agent accepted the flush (flight recorder); may block up to ~200 ms
// when no agent is listening, so never call it from a latency-critical path more than once in a while.
bool requestFlush(const char* reason) {
#if TACHYON_ENABLED
  return Tachyon_RequestFlush(reason) != 0;
#else
  (void)reason;
  return false;
#endif
}

// Thread-safe. True when an agent is draining instrumentation events.
bool isConnected() {
#if TACHYON_ENABLED
  return Tachyon_IsConnected() != 0;
#else
  return false;
#endif
}

// Thread-safe. Fiber lifetime and switch hooks; see FiberScheduler for the call order.
void registerFiber(uint64_t id) {
#if TACHYON_ENABLED
  Tachyon_RegisterFiber(id);
#else
  (void)id;
#endif
}

// Thread-safe.
void unregisterFiber(uint64_t id) {
#if TACHYON_ENABLED
  Tachyon_UnregisterFiber(id);
#else
  (void)id;
#endif
}

// Thread-safe.
void beginFiberSwitch(uint64_t current, uint64_t next) {
#if TACHYON_ENABLED
  Tachyon_BeginFiberSwitch(current, next);
#else
  (void)current;
  (void)next;
#endif
}

// Thread-safe.
void endFiberSwitch(uint64_t resumed) {
#if TACHYON_ENABLED
  Tachyon_EndFiberSwitch(resumed);
#else
  (void)resumed;
#endif
}

}  // namespace engine::prof

// An existing PROFILE_SCOPE-style layer maps onto the Tachyon macros like this (see the guide).
#define ENGINE_PROFILE_SCOPE(name) TACHYON_SCOPE(name)
#define ENGINE_PROFILE_SCOPE_COLOR(name, color) TACHYON_SCOPE_COLOR(name, color)

namespace engine {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

void burn(std::chrono::microseconds d) {  // stand-in for real work
  std::this_thread::sleep_for(d);
}

// ------------------------------------------------------------------------------------------------------
// Lock wrapper: the wait for the lock shows up as a lock-wait event with the lock's address.
// ------------------------------------------------------------------------------------------------------
class ProfiledMutex {
 public:
  explicit ProfiledMutex(const char* staticName) : m_name(staticName) {}
  ProfiledMutex(const ProfiledMutex&) = delete;
  ProfiledMutex& operator=(const ProfiledMutex&) = delete;

  // Thread-safe. The wait (not the hold time) is the profiled interval: the scope ends once we own the lock.
  void lock() {
    TACHYON_LOCK_SCOPE(m_mutex, m_name);
    m_mutex.lock();
  }
  // Thread-safe.
  void unlock() { m_mutex.unlock(); }
  // Thread-safe.
  const char* name() const { return m_name; }

 private:
  std::mutex m_mutex;
  const char* m_name;
};

// ------------------------------------------------------------------------------------------------------
// Fake job system: worker threads, a queue, one scope per job named after the job.
// ------------------------------------------------------------------------------------------------------
struct JobDesc {
  const char* name;  // static string: used as the scope id (interned by pointer)
  uint32_t color;
  void (*fn)(uint32_t instance);
};

class JobSystem {
 public:
  explicit JobSystem(unsigned workers) {
    for (unsigned i = 0; i < workers; ++i) m_threads.emplace_back([this, i] { workerMain(i); });
  }
  ~JobSystem() {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stop = true;
    }
    m_wake.notify_all();
    for (std::thread& t : m_threads) t.join();
  }
  JobSystem(const JobSystem&) = delete;
  JobSystem& operator=(const JobSystem&) = delete;

  // Thread-safe. Queues one job instance.
  void submit(const JobDesc& job, uint32_t instance) {
    m_pending.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_queue.push_back({&job, instance});
    }
    m_wake.notify_one();
  }

  // Thread-safe. Blocks until every submitted job has finished.
  void waitIdle() {
    std::unique_lock<std::mutex> lock(m_doneMutex);
    m_done.wait(lock, [this] { return m_pending.load(std::memory_order_acquire) == 0; });
  }

 private:
  struct Item {
    const JobDesc* job;
    uint32_t instance;
  };

  void workerMain(unsigned index) {
    char name[32];
    std::snprintf(name, sizeof(name), "Worker %u", index);
    prof::setThreadName(name);
    for (;;) {
      Item item{};
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_queue.empty() && !m_stop) {
          m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
          // Worker wake-up marker: lets the viewer line up "job submitted" with "worker woke".
          prof::marker("JobSystem.WorkerWake", nullptr, prof::kColorMarker);
        }
        if (m_queue.empty()) return;
        item = m_queue.front();
        m_queue.pop_front();
      }
      runJob(item);
      if (m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(m_doneMutex);
        m_done.notify_all();
      }
    }
  }

  static void runJob(const Item& item) {
#if TACHYON_ENABLED
    // Job name is the (static) id; per-instance details are copied into `data`.
    char data[48];
    std::snprintf(data, sizeof(data), "instance=%u", item.instance);
    TACHYON_SCOPE_DATA(item.job->name, data);
#endif
    item.job->fn(item.instance);
  }

  std::vector<std::thread> m_threads;
  std::mutex m_mutex;
  std::condition_variable m_wake;
  std::deque<Item> m_queue;
  bool m_stop = false;
  std::atomic<int> m_pending{0};
  std::mutex m_doneMutex;
  std::condition_variable m_done;
};

ProfiledMutex g_physicsLock("Lock.PhysicsWorld");
uint64_t g_physicsBodies = 0;

void jobAnimate(uint32_t) {
  ENGINE_PROFILE_SCOPE("Animate.Blend");  // nested scope inside the job scope
  burn(std::chrono::microseconds(300));
}

void jobPhysics(uint32_t) {
  burn(std::chrono::microseconds(150));
  g_physicsLock.lock();  // contended on purpose: shows up as a lock wait
  ++g_physicsBodies;
  burn(std::chrono::microseconds(100));
  g_physicsLock.unlock();
}

void jobCull(uint32_t) {
  burn(std::chrono::microseconds(200));
}

const JobDesc kAnimateJob{"Job.Animate", prof::kColorJobs, &jobAnimate};
const JobDesc kPhysicsJob{"Job.Physics", prof::kColorJobs, &jobPhysics};
const JobDesc kCullJob{"Job.Cull", prof::kColorRender, &jobCull};

// ------------------------------------------------------------------------------------------------------
// Fake fiber scheduler. Real code calls SwitchToFiber / swapcontext / its own assembly where marked.
// Rules: Register at creation, Unregister at destruction; around each switch the thread that leaves the
// current fiber calls BeginFiberSwitch(current, next), and the code that resumes `next` calls
// EndFiberSwitch(next) as its first action after the switch returns.
// ------------------------------------------------------------------------------------------------------
class FiberScheduler {
 public:
  FiberScheduler() {
    prof::registerFiber(kSchedulerFiber);
    for (uint64_t i = 0; i < kFiberCount; ++i) prof::registerFiber(fiberId(i));
  }
  ~FiberScheduler() {
    for (uint64_t i = 0; i < kFiberCount; ++i) prof::unregisterFiber(fiberId(i));
    prof::unregisterFiber(kSchedulerFiber);
  }
  FiberScheduler(const FiberScheduler&) = delete;
  FiberScheduler& operator=(const FiberScheduler&) = delete;

  // Not thread-safe (call from the scheduler thread only). Runs every fiber for one step.
  void runRound() {
    for (uint64_t i = 0; i < kFiberCount; ++i) {
      switchTo(fiberId(i));
      fiberStep(i);
      switchTo(kSchedulerFiber);
    }
  }

 private:
  static constexpr uint64_t kFiberCount = 3;
  static constexpr uint64_t kSchedulerFiber = 0xF1BE0000ull;  // ids are any 64-bit value unique per fiber
  static uint64_t fiberId(uint64_t i) { return 0xF1BE1000ull + i; }

  void switchTo(uint64_t next) {
    prof::beginFiberSwitch(m_current, next);
    // <-- SwitchToFiber(next) / swapcontext(...) goes here; the call returns on the resumed side.
    m_current = next;
    prof::endFiberSwitch(next);
  }

  static void fiberStep(uint64_t index) {
    char data[32];
    std::snprintf(data, sizeof(data), "fiber=%llu", static_cast<unsigned long long>(index));
    TACHYON_SCOPE_DATA("Fiber.Step", data);
    burn(std::chrono::microseconds(100));
  }

  uint64_t m_current = kSchedulerFiber;
};

// ------------------------------------------------------------------------------------------------------
// Streaming / I/O: one scope per request, asset path as data.
// ------------------------------------------------------------------------------------------------------
void streamAsset([[maybe_unused]] const std::string& path) {
  TACHYON_SCOPE_DATA("IO.ReadAsset", path.c_str());
  burn(std::chrono::microseconds(400));  // stand-in for the read + decompress
}

class IoThread {
 public:
  IoThread() : m_thread([this] { run(); }) {}
  ~IoThread() {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
  }
  IoThread(const IoThread&) = delete;
  IoThread& operator=(const IoThread&) = delete;

  // Thread-safe.
  void request(std::string path) {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_queue.push_back(std::move(path));
    }
    m_cv.notify_one();
  }

 private:
  void run() {
    prof::setThreadName("IO");
    for (;;) {
      std::string path;
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
        if (m_queue.empty()) return;
        path = std::move(m_queue.front());
        m_queue.pop_front();
      }
      streamAsset(path);
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<std::string> m_queue;
  bool m_stop = false;
  std::thread m_thread;
};

// ------------------------------------------------------------------------------------------------------
// Render thread: consumes frames submitted by the main thread and marks its own frame boundary.
// ------------------------------------------------------------------------------------------------------
class RenderThread {
 public:
  RenderThread() : m_thread([this] { run(); }) {}
  ~RenderThread() {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
  }
  RenderThread(const RenderThread&) = delete;
  RenderThread& operator=(const RenderThread&) = delete;

  // Thread-safe. Hands frame `frameNumber` to the render thread.
  void submit(uint64_t frameNumber) {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_frames.push_back(frameNumber);
    }
    m_cv.notify_one();
  }

 private:
  void run() {
    prof::setThreadName("Render");
    for (;;) {
      uint64_t frame = 0;
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_stop || !m_frames.empty(); });
        if (m_frames.empty()) return;
        frame = m_frames.front();
        m_frames.pop_front();
      }
      TACHYON_FRAME("Render", frame);  // frame mark at the start of each render-thread frame
      {
        TACHYON_SCOPE_COLOR("Render.Submit", prof::kColorRender);
        burn(std::chrono::microseconds(500));
      }
      // GPU frame time is recorded as a plot in v1 (no calibration call in the frozen API).
      prof::plot("Render.GpuFrameMs", 0.5 + 0.01 * static_cast<double>(frame % 7));
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<uint64_t> m_frames;
  bool m_stop = false;
  std::thread m_thread;
};

// ------------------------------------------------------------------------------------------------------
// Console commands: flush goes through the SDK; start/stop shell out to tachyon-cli.
// ------------------------------------------------------------------------------------------------------
// Not thread-safe. Runs `tachyon-cli <args>` and returns its exit code (-1 when it cannot be started).
int runCli(const std::string& args) {
  const char* exe = std::getenv("TACHYON_CLI");  // path override; default relies on PATH
  const std::string cmd = std::string(exe != nullptr ? exe : "tachyon-cli") + " " + args;
  return std::system(cmd.c_str());
}

long long currentPid() {
#if defined(_WIN32)
  return static_cast<long long>(GetCurrentProcessId());
#else
  return static_cast<long long>(getpid());
#endif
}

// Not thread-safe (console thread). Returns the command's exit status; 0 means success.
int consoleCommand(const std::string& line) {
  if (line == "prof.flush") return prof::requestFlush("console") ? 0 : 1;
  if (line == "prof.status") return prof::isConnected() ? 0 : 1;
  if (line.rfind("prof.start", 0) == 0) {
    const std::string out = line.size() > 11 ? line.substr(11) : "capture.tachyon";
    return runCli("start --mode attach --pid " + std::to_string(currentPid()) + " --out " + out);
  }
  if (line == "prof.stop") return runCli("stop --wait");
  std::fprintf(stderr, "unknown console command: %s\n", line.c_str());
  return 2;
}

// ------------------------------------------------------------------------------------------------------
// Main loop with the hitch trigger.
// ------------------------------------------------------------------------------------------------------
int runEngine(unsigned frames, unsigned workers) {
  constexpr double kBudgetMs = 16.6;
  constexpr double kFlushCooldownMs = 5000.0;
  JobSystem jobs(workers);
  FiberScheduler fibers;
  IoThread io;
  RenderThread render;
  double sinceFlushMs = kFlushCooldownMs;
  unsigned hitchFlushes = 0;

  for (uint64_t frame = 0; frame < frames; ++frame) {
    const Clock::time_point t0 = Clock::now();
    TACHYON_FRAME("Main", frame);  // frame mark at the start of each main-thread frame
    {
      TACHYON_SCOPE_COLOR("Main.Update", prof::kColorJobs);
      for (uint32_t i = 0; i < 4; ++i) jobs.submit(kAnimateJob, i);
      for (uint32_t i = 0; i < 4; ++i) jobs.submit(kPhysicsJob, i);
      for (uint32_t i = 0; i < 2; ++i) jobs.submit(kCullJob, i);
      fibers.runRound();
      jobs.waitIdle();
    }
    if (frame % 4 == 0) io.request("assets/textures/rock_" + std::to_string(frame) + ".dds");
    render.submit(frame);
    if (frame == 5) burn(std::chrono::milliseconds(25));  // deliberate hitch for the demo
    burn(std::chrono::milliseconds(2));

    const double frameMs = msSince(t0);
    prof::plot("Main.FrameMs", frameMs);
    sinceFlushMs += frameMs;
    if (frameMs > kBudgetMs && sinceFlushMs >= kFlushCooldownMs) {
      // Hitch trigger: mark it, then ask the agent to dump the flight-recorder window. Rate-limited, because
      // the call is a synchronous round trip to the agent.
      prof::marker("Hitch", "frame over budget", prof::kColorSync);
      if (prof::requestFlush("hitch")) ++hitchFlushes;
      sinceFlushMs = 0.0;
    }
  }
  std::printf("frames=%u physicsBodies=%llu agentConnected=%d hitchFlushes=%u lock=%s\n", frames,
              static_cast<unsigned long long>(g_physicsBodies), prof::isConnected() ? 1 : 0, hitchFlushes,
              g_physicsLock.name());
  return 0;
}

}  // namespace engine

int main(int argc, char** argv) {
  unsigned frames = 30;
  unsigned workers = 3;
  const char* console = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      frames = static_cast<unsigned>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
      workers = static_cast<unsigned>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--console") == 0 && i + 1 < argc) {
      console = argv[++i];
    }
  }
  if (workers == 0) workers = 1;
  engine::prof::setThreadName("Main");
  if (console != nullptr) return engine::consoleCommand(console);
  return engine::runEngine(frames, workers);
}
