#include "capture/windows_etw/rt_consumer.h"

#include "core/log.h"
#include "core/platform.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#if TACHYON_PLATFORM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// Order matters: wmistr.h before evntrace.h before evntcons.h.
#include <evntcons.h>
#include <evntrace.h>
#include <wmistr.h>

#include <string>

namespace tachyon::capture::etw {

static_assert(sizeof(etlconvert::Guid) == sizeof(GUID));

uint64_t traceClockNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart);
}

uint64_t traceClockFrequency() {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return static_cast<uint64_t>(f.QuadPart);
}

struct RealTimeConsumer::Impl {
  LiveEtwSource* source = nullptr;
  std::wstring name;  // OpenTrace keeps the pointer
  EVENT_TRACE_LOGFILEW logfile{};
  TRACEHANDLE handle = INVALID_PROCESSTRACE_HANDLE;
  std::thread thread;
  mutable std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool closed = false;
  std::atomic<uint64_t> events{0};
  std::atomic<uint64_t> buffers{0};
  std::atomic<uint64_t> cpuMicros{0};
  std::atomic<uint64_t> callbackTicks{0};  // traceClock ticks spent in onEventRecord
  std::atomic<uint32_t> status{0};
  uint64_t perfFreq = 0;
};

namespace {

void WINAPI onEventRecord(PEVENT_RECORD record) {
  auto* impl = static_cast<RealTimeConsumer::Impl*>(record->UserContext);
  const uint64_t began = traceClockNow();
  impl->events.fetch_add(1, std::memory_order_relaxed);
  etlconvert::EtwEvent e;
  std::memcpy(&e.provider, &record->EventHeader.ProviderId, sizeof(GUID));
  e.opcode = record->EventHeader.EventDescriptor.Opcode;
  e.version = record->EventHeader.EventDescriptor.Version;
  e.timestamp = static_cast<uint64_t>(record->EventHeader.TimeStamp.QuadPart);
  e.threadId = record->EventHeader.ThreadId;
  e.processId = record->EventHeader.ProcessId;
  e.cpu = record->BufferContext.ProcessorIndex;
  e.payload = etlconvert::Bytes(static_cast<const std::byte*>(record->UserData), record->UserDataLength);
  impl->source->submit(e);
  impl->callbackTicks.fetch_add(traceClockNow() - began, std::memory_order_relaxed);
}

// Called after each ETW buffer: the events of one buffer go to the decoder thread together.
ULONG WINAPI onBuffer(PEVENT_TRACE_LOGFILEW logfile) {
  auto* impl = static_cast<RealTimeConsumer::Impl*>(logfile->Context);
  impl->buffers.fetch_add(1, std::memory_order_relaxed);
  impl->source->endBuffer();
  return TRUE;  // keep going
}

}  // namespace

RealTimeConsumer::RealTimeConsumer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

RealTimeConsumer::~RealTimeConsumer() {
  stop(std::chrono::milliseconds(0));
}

Result<std::unique_ptr<RealTimeConsumer>> RealTimeConsumer::start(const std::string& loggerName,
                                                                  LiveEtwSource& source) {
  auto impl = std::make_unique<Impl>();
  impl->source = &source;
  {
    const int n =
        MultiByteToWideChar(CP_UTF8, 0, loggerName.data(), static_cast<int>(loggerName.size()), nullptr, 0);
    impl->name.assign(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, loggerName.data(), static_cast<int>(loggerName.size()), impl->name.data(),
                        n);
  }
  EVENT_TRACE_LOGFILEW& lf = impl->logfile;
  lf.LoggerName = impl->name.data();
  lf.ProcessTraceMode =
      PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
  lf.EventRecordCallback = onEventRecord;
  lf.BufferCallback = onBuffer;
  lf.Context = impl.get();
  impl->handle = OpenTraceW(&lf);
  if (impl->handle == INVALID_PROCESSTRACE_HANDLE) {
    const ULONG e = GetLastError();
    ErrorCode code = ErrorCode::IoError;
    if (e == ERROR_ACCESS_DENIED) code = ErrorCode::NoPrivilege;
    if (e == ERROR_WMI_INSTANCE_NOT_FOUND || e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
      code = ErrorCode::NotFound;
    return makeError(
        code, "OpenTrace(real-time '" + loggerName + "') failed with Windows error " + std::to_string(e));
  }
  impl->perfFreq = static_cast<uint64_t>(lf.LogfileHeader.PerfFreq.QuadPart);
  Impl* raw = impl.get();
  impl->thread = std::thread([raw] {
    TRACEHANDLE handle = raw->handle;
    const ULONG status = ProcessTrace(&handle, 1, nullptr, nullptr);
    raw->source->endBuffer();
    raw->status = status;
    raw->cpuMicros = static_cast<uint64_t>(currentThreadCpuSeconds() * 1e6);
    {
      std::lock_guard lock(raw->mutex);
      raw->done = true;
    }
    raw->cv.notify_all();
  });
  return std::unique_ptr<RealTimeConsumer>(new RealTimeConsumer(std::move(impl)));
}

void RealTimeConsumer::waitQuiet(std::chrono::milliseconds quiet, std::chrono::milliseconds maxWait) {
  Impl& s = *m_impl;
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + maxWait;
  uint64_t seen = s.buffers.load();
  auto lastChange = Clock::now();
  while (Clock::now() < deadline) {
    {
      std::lock_guard lock(s.mutex);
      if (s.done) return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const uint64_t now = s.buffers.load();
    if (now != seen) {
      seen = now;
      lastChange = Clock::now();
    } else if (Clock::now() - lastChange >= quiet) {
      return;
    }
  }
}

void RealTimeConsumer::stop(std::chrono::milliseconds drain) {
  Impl& s = *m_impl;
  {
    std::unique_lock lock(s.mutex);
    if (s.closed) return;
    s.closed = true;
    if (!s.cv.wait_for(lock, drain, [&] { return s.done; })) {
      lock.unlock();
      // Still running: closing the trace from another thread makes ProcessTrace return.
      CloseTrace(s.handle);
      lock.lock();
      if (!s.cv.wait_for(lock, std::chrono::seconds(10), [&] { return s.done; }))
        TACHYON_LOG_WARN("etw: the real-time consumer thread did not return after CloseTrace");
    } else {
      lock.unlock();
      CloseTrace(s.handle);
    }
  }
  if (s.thread.joinable()) s.thread.join();
}

RealTimeConsumerStats RealTimeConsumer::stats() const {
  const Impl& s = *m_impl;
  RealTimeConsumerStats st;
  st.events = s.events.load();
  st.buffers = s.buffers.load();
  st.cpuSeconds = static_cast<double>(s.cpuMicros.load()) * 1e-6;
  st.callbackSeconds =
      static_cast<double>(s.callbackTicks.load()) / static_cast<double>(traceClockFrequency());
  st.perfFreq = s.perfFreq;
  st.status = s.status.load();
  {
    std::lock_guard lock(s.mutex);
    st.finished = s.done;
  }
  return st;
}

}  // namespace tachyon::capture::etw

#else  // !TACHYON_PLATFORM_WINDOWS

#include <chrono>

namespace tachyon::capture::etw {

uint64_t traceClockNow() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

uint64_t traceClockFrequency() {
  return 1'000'000'000ull;
}

struct RealTimeConsumer::Impl {};

RealTimeConsumer::RealTimeConsumer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
RealTimeConsumer::~RealTimeConsumer() = default;

Result<std::unique_ptr<RealTimeConsumer>> RealTimeConsumer::start(const std::string&, LiveEtwSource&) {
  return makeError(ErrorCode::Unsupported, "the ETW real-time consumer requires Windows");
}

void RealTimeConsumer::stop(std::chrono::milliseconds) {}

RealTimeConsumerStats RealTimeConsumer::stats() const {
  return {};
}

void RealTimeConsumer::waitQuiet(std::chrono::milliseconds, std::chrono::milliseconds) {}

}  // namespace tachyon::capture::etw

#endif
