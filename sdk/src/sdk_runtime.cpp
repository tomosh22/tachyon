// SDK client runtime: flush thread, agent handshake, sidecar fallback, string de-duplication.
//
// Control-channel client: the SDK deliberately does NOT link tachyon_control. That library pulls in
// nlohmann_json, tachyon_core and tachyon_capture (C++20, exceptions) and would bloat a library that is
// embedded in the engine and whose shared form must export nothing but the C API. The SDK needs exactly two
// control requests (inst_hello, flush) and two reply fields ("ok", "pipe"), so a ~60 line JSON-lines client
// lives here instead (jsonField below). It follows docs/specs/control-protocol.md; integration is covered by
// tests/unit/sdk/runtime_agent_test.cpp, which runs the real tachyon_control Server as the fake agent.
#include "sdk_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <new>

#include "tachyon/tachyon.h"

#if defined(_WIN32)
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace tachyon::sdk {

std::atomic<uint32_t> g_state{kStateUninit};

namespace {

using Clock = std::chrono::steady_clock;

uint64_t nowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

constexpr int kWriteTimeoutMs = 2000;
constexpr size_t kOutFlushBytes = 256 * 1024;
constexpr size_t kBusyDrainBytes = 16 * 1024;
constexpr int kAgentRetryIntervalMs = 1000;
constexpr int kInitialAgentTimeoutMs = 200;
constexpr int kRetryAgentTimeoutMs = 100;

std::atomic<Runtime*> g_runtime{nullptr};

std::mutex& creationMutex() {
  static std::mutex* const m = new std::mutex;  // intentionally leaked: usable during process exit
  return *m;
}

void atExitHandler() {
  Runtime* r = g_runtime.load(std::memory_order_acquire);
  if (r != nullptr) r->shutdown();
}

// ---- minimal JSON object field extraction ------------------------------------------------------------

struct JsonValue {
  bool found = false;
  bool isString = false;
  std::string text;  // decoded string, or the raw token for non-strings
};

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Parses a JSON string starting at s[i] == '"'. Advances i past the closing quote. False when malformed.
bool parseJsonString(const std::string& s, size_t& i, std::string& out) {
  out.clear();
  if (i >= s.size() || s[i] != '"') return false;
  ++i;
  while (i < s.size()) {
    const char c = s[i++];
    if (c == '"') return true;
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (i >= s.size()) return false;
    const char e = s[i++];
    switch (e) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'u': {
        if (i + 4 > s.size()) return false;
        uint32_t cp = 0;
        for (int k = 0; k < 4; ++k) {
          const char h = s[i++];
          cp <<= 4;
          if (h >= '0' && h <= '9')
            cp |= static_cast<uint32_t>(h - '0');
          else if (h >= 'a' && h <= 'f')
            cp |= static_cast<uint32_t>(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F')
            cp |= static_cast<uint32_t>(h - 'A' + 10);
          else
            return false;
        }
        appendUtf8(out, cp);
        break;
      }
      default: out.push_back(e); break;  // \" \\ \/
    }
  }
  return false;
}

void skipSpace(const std::string& s, size_t& i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
}

// Finds `key` among the top-level members of the JSON object `line`.
JsonValue jsonField(const std::string& line, const char* key) {
  JsonValue result;
  size_t i = 0;
  skipSpace(line, i);
  if (i >= line.size() || line[i] != '{') return result;
  ++i;
  for (;;) {
    skipSpace(line, i);
    if (i >= line.size() || line[i] == '}') return result;
    std::string k;
    if (!parseJsonString(line, i, k)) return result;
    skipSpace(line, i);
    if (i >= line.size() || line[i] != ':') return result;
    ++i;
    skipSpace(line, i);
    JsonValue v;
    v.found = true;
    if (i < line.size() && line[i] == '"') {
      v.isString = true;
      if (!parseJsonString(line, i, v.text)) return JsonValue();
    } else {
      // number / literal / nested value: skip to the next top-level ',' or '}'.
      const size_t start = i;
      int depth = 0;
      while (i < line.size()) {
        const char c = line[i];
        if (c == '"') {
          std::string ignored;
          if (!parseJsonString(line, i, ignored)) return JsonValue();
          continue;
        }
        if (c == '{' || c == '[') ++depth;
        if (c == '}' || c == ']') {
          if (depth == 0) break;
          --depth;
        }
        if (c == ',' && depth == 0) break;
        ++i;
      }
      v.text = line.substr(start, i - start);
      while (!v.text.empty() && (v.text.back() == ' ' || v.text.back() == '\t')) v.text.pop_back();
    }
    if (k == key) return v;
    skipSpace(line, i);
    if (i < line.size() && line[i] == ',') ++i;
  }
}

std::string jsonEscape(const char* s) {
  std::string out;
  if (s == nullptr) return out;
  for (; *s != '\0'; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

// Reads one '\n'-terminated line from `ch` into `line`, using `pending` as carry-over buffer. Returns 1 on a
// line, 0 on EOF, -1 on timeout, -2 on error.
int readLine(Channel& ch, std::string& pending, std::string& line, uint64_t deadlineMs) {
  for (;;) {
    const size_t nl = pending.find('\n');
    if (nl != std::string::npos) {
      line.assign(pending, 0, nl);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      pending.erase(0, nl + 1);
      return 1;
    }
    const uint64_t now = nowMs();
    if (now >= deadlineMs) return -1;
    char buf[2048];
    const int n = ch.readSome(buf, sizeof(buf), static_cast<int>(deadlineMs - now));
    if (n > 0) {
      pending.append(buf, static_cast<size_t>(n));
      if (pending.size() > (1u << 20)) return -2;
      continue;
    }
    return n;  // 0 EOF, -1 timeout, -2 error
  }
}

// Sends `request` (no newline) and waits for the response whose "id" equals `id`; event lines and other ids
// are skipped. Returns the response line or false.
bool controlCall(Channel& ch, std::string& pending, int64_t id, const std::string& request, int timeoutMs,
                 std::string& response) {
  std::string out = request;
  out.push_back('\n');
  if (!ch.writeAll(out.data(), out.size(), 1000)) return false;
  const uint64_t deadline = nowMs() + static_cast<uint64_t>(timeoutMs);
  std::string line;
  for (;;) {
    if (readLine(ch, pending, line, deadline) != 1) return false;
    const JsonValue idv = jsonField(line, "id");
    if (idv.found && !idv.isString && idv.text == std::to_string(id)) {
      response = std::move(line);
      return true;
    }
  }
}

bool responseOk(const std::string& response) {
  const JsonValue ok = jsonField(response, "ok");
  return ok.found && !ok.isString && ok.text == "true";
}

}  // namespace

// --------------------------------------------------------------------------------------------------------
// Configuration
// --------------------------------------------------------------------------------------------------------

RuntimeConfig loadRuntimeConfig() {
  RuntimeConfig cfg;
  std::string v;
  if (getEnv("TACHYON_DISABLE", v) && (v == "1" || v == "true" || v == "TRUE" || v == "on"))
    cfg.disabled = true;
  cfg.agentAddress = defaultAgentAddress();
  if (getEnv("TACHYON_SIDECAR", v) && !v.empty()) {
    if (v == "off" || v == "OFF" || v == "0") {
      cfg.sidecarEnabled = false;
    } else {
      cfg.sidecarPath = v;
    }
  }
  if (cfg.sidecarEnabled && cfg.sidecarPath.empty()) {
    std::string dir = currentDirectory();
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir.push_back('/');
    cfg.sidecarPath = dir + executableName() + "-" + std::to_string(currentProcessId()) + ".tachyon-inst";
  }
  if (getEnv("TACHYON_RING_KB", v) && !v.empty()) {
    char* end = nullptr;
    const unsigned long long kb = strtoull(v.c_str(), &end, 10);
    if (end != v.c_str() && kb > 0) {
      uint64_t bytes = kb * 1024ull;
      if (bytes < kMinRingBytes) bytes = kMinRingBytes;
      if (bytes > kMaxRingBytes) bytes = kMaxRingBytes;
      uint64_t pow2 = kMinRingBytes;
      while (pow2 < bytes) pow2 <<= 1;
      cfg.ringBytes = pow2;
    }
  }
  return cfg;
}

// --------------------------------------------------------------------------------------------------------
// Runtime lifetime
// --------------------------------------------------------------------------------------------------------

Runtime* runtimeIfCreated() {
  return g_runtime.load(std::memory_order_acquire);
}

Runtime* runtime() {
  Runtime* r = g_runtime.load(std::memory_order_acquire);
  if (r != nullptr) return r;
  std::lock_guard<std::mutex> lock(creationMutex());
  r = g_runtime.load(std::memory_order_acquire);
  if (r != nullptr) return r;
  if (g_state.load(std::memory_order_relaxed) == kStateDisabled) return nullptr;
  RuntimeConfig cfg = loadRuntimeConfig();
  if (cfg.disabled) {
    g_state.store(kStateDisabled, std::memory_order_release);
    return nullptr;
  }
  Runtime* created = new (std::nothrow) Runtime(std::move(cfg));
  if (created == nullptr) {
    g_state.store(kStateDisabled, std::memory_order_release);
    return nullptr;
  }
  g_runtime.store(created, std::memory_order_release);
  g_state.store(kStateActive, std::memory_order_release);
  if (!created->start()) g_state.store(kStateDisabled, std::memory_order_release);
  std::atexit(&atExitHandler);
  return created;
}

void restartRuntimeForTests() {
  Runtime* old = g_runtime.load(std::memory_order_acquire);
  if (old != nullptr) old->shutdown();
  std::lock_guard<std::mutex> lock(creationMutex());
  // Keep the old runtime reachable (it is never destroyed, see sdk_runtime.h).
  static std::vector<Runtime*>* const oldRuntimes = new std::vector<Runtime*>;
  if (old != nullptr) oldRuntimes->push_back(old);
  g_runtime.store(nullptr, std::memory_order_release);
  g_state.store(kStateUninit, std::memory_order_release);
}

Runtime::Runtime(RuntimeConfig config) : m_config(std::move(config)) {
  m_startTicks = nowTicks();
  m_out.reserve(kOutFlushBytes + 4096);
}

Runtime::~Runtime() = default;  // never invoked for the process-wide instance

bool Runtime::start() {
#if defined(_WIN32)
  // Without this the 1 ms drain cadence degrades to the default 15.6 ms timer tick.
  m_timerPeriodRaised = timeBeginPeriod(1) == TIMERR_NOERROR;
#endif
  try {
    m_thread = std::thread([this] { run(); });
  } catch (...) {
    return false;
  }
  m_started = true;
  return true;
}

// --------------------------------------------------------------------------------------------------------
// Rings
// --------------------------------------------------------------------------------------------------------

ThreadRing* Runtime::registerThread() {
  ThreadRing* ring = new (std::nothrow) ThreadRing;
  if (ring == nullptr) return nullptr;
  ring->buf = static_cast<uint8_t*>(::operator new(static_cast<size_t>(m_config.ringBytes), std::nothrow));
  if (ring->buf == nullptr) {
    delete ring;
    return nullptr;
  }
  ring->cap = m_config.ringBytes;
  ring->mask = m_config.ringBytes - 1;
  ring->tid = currentThreadId();
  std::lock_guard<std::mutex> lock(m_ringsMutex);
  m_rings.push_back(ring);
  return ring;
}

void Runtime::retireRing(ThreadRing* ring) {
  ring->retired.store(true, std::memory_order_release);
}

// --------------------------------------------------------------------------------------------------------
// Fibers
// --------------------------------------------------------------------------------------------------------

void Runtime::registerFiber(uint64_t fiberId) {
  FiberShard& s = fiberShard(fiberId);
  std::lock_guard<std::mutex> lock(s.mutex);
  s.map[fiberId] = ScopeStack();
}

void Runtime::unregisterFiber(uint64_t fiberId) {
  FiberShard& s = fiberShard(fiberId);
  std::lock_guard<std::mutex> lock(s.mutex);
  s.map.erase(fiberId);
}

void Runtime::saveFiber(uint64_t fiberId, const ScopeStack& stack) {
  FiberShard& s = fiberShard(fiberId);
  std::lock_guard<std::mutex> lock(s.mutex);
  s.map[fiberId] = stack;  // creates the entry for fibers that were never registered explicitly
}

bool Runtime::loadFiber(uint64_t fiberId, ScopeStack& out) {
  FiberShard& s = fiberShard(fiberId);
  std::lock_guard<std::mutex> lock(s.mutex);
  auto it = s.map.find(fiberId);
  if (it == s.map.end()) return false;
  out = it->second;
  return true;
}

// --------------------------------------------------------------------------------------------------------
// Flush thread
// --------------------------------------------------------------------------------------------------------

void Runtime::wake() {
  std::lock_guard<std::mutex> lock(m_wakeMutex);
  m_wakeCv.notify_all();
}

void Runtime::run() {
  establishSink();
  while (!m_stop.load(std::memory_order_acquire)) {
    if (m_sinkKind != SinkKind::Agent && nowMs() >= m_nextAgentRetryMs) {
      m_nextAgentRetryMs = nowMs() + kAgentRetryIntervalMs;
      tryAgent(kRetryAgentTimeoutMs);
    }
    const size_t drained = drainAll();
    writeOut();
    if (++m_pumpCounter >= 64) {
      m_pumpCounter = 0;
      pumpControl();
    }
    m_cycles.fetch_add(1, std::memory_order_release);
    // 1 ms cadence while idle; keep going without sleeping while the rings are producing faster than that.
    if (drained >= kBusyDrainBytes) {
      std::this_thread::yield();
      continue;
    }
    std::unique_lock<std::mutex> lock(m_wakeMutex);
    m_wakeCv.wait_for(lock, std::chrono::milliseconds(1));
  }
  {
    std::lock_guard<std::mutex> lock(m_exitMutex);
    m_threadDone = true;
  }
  m_exitCv.notify_all();
}

void Runtime::setReady(SinkKind kind) {
  m_sinkKind = kind;
  m_agentConnected.store(kind == SinkKind::Agent, std::memory_order_release);
  if (g_runtime.load(std::memory_order_acquire) == this && !m_stop.load(std::memory_order_acquire)) {
    g_state.store(kind == SinkKind::None ? kStateDisabled : kStateActive, std::memory_order_release);
  }
  m_ready.store(kind == SinkKind::Agent ? 1 : kind == SinkKind::Sidecar ? 2 : 3, std::memory_order_release);
}

void Runtime::establishSink() {
  m_nextAgentRetryMs = nowMs() + kAgentRetryIntervalMs;
  if (tryAgent(kInitialAgentTimeoutMs)) return;
  if (m_config.sidecarEnabled && trySidecar()) return;
  setReady(SinkKind::None);
}

// Writes the StreamHeader plus every known StringDef / ThreadName frame to a fresh stream.
bool Runtime::beginStream(Channel& channel) {
  std::vector<uint8_t> pre;
  inst::FrameHeader fh{static_cast<uint32_t>(inst::FrameType::StreamHeader),
                       sizeof(inst::StreamHeaderPayload)};
  inst::StreamHeaderPayload sh{};
  sh.magic = inst::kStreamMagic;
  sh.wire_version = inst::kWireVersion;
  sh.pid = currentProcessId();
#if defined(_WIN32)
  sh.platform = static_cast<uint32_t>(inst::Platform::Windows);
#else
  sh.platform = static_cast<uint32_t>(inst::Platform::Linux);
#endif
  sh.clock_frequency_hz = ticksFrequency();
  sh.process_start_ts = m_startTicks;
  sh.api_version = TACHYON_API_VERSION;
  sh.reserved = 0;
  pre.resize(sizeof(fh) + sizeof(sh));
  memcpy(pre.data(), &fh, sizeof(fh));
  memcpy(pre.data() + sizeof(fh), &sh, sizeof(sh));
  for (const auto& kv : m_stringFrames) pre.insert(pre.end(), kv.second.begin(), kv.second.end());
  for (const auto& kv : m_threadNameFrames) pre.insert(pre.end(), kv.second.begin(), kv.second.end());
  return channel.writeAll(pre.data(), pre.size(), kWriteTimeoutMs);
}

bool Runtime::trySidecar() {
  if (m_config.sidecarPath.empty()) return false;
  // The first open uses the configured path; every later open (agent lost after a sidecar phase) goes to
  // <stem>.<n>.<ext> so earlier phases are never truncated.
  std::string path = m_config.sidecarPath;
  if (m_sidecarOpens > 0) {
    const size_t sep = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    const std::string tag = "." + std::to_string(m_sidecarOpens);
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) {
      path.insert(dot, tag);
    } else {
      path += tag;
    }
  }
  Channel file;
  if (!file.openFile(path) || !beginStream(file)) return false;
  ++m_sidecarOpens;
  m_sink = std::move(file);
  {
    std::lock_guard<std::mutex> lock(m_pathMutex);
    m_activeSidecarPath = path;
  }
  setReady(SinkKind::Sidecar);
  return true;
}

bool Runtime::tryAgent(int timeoutMs) {
  if (m_config.agentAddress.empty()) return false;
  Channel ctl;
  if (!ctl.connect(m_config.agentAddress, timeoutMs, true)) return false;
  std::string pending;
  std::string response;
  const std::string hello = "{\"id\":1,\"cmd\":\"inst_hello\",\"pid\":" + std::to_string(currentProcessId()) +
                            ",\"api_version\":" + std::to_string(TACHYON_API_VERSION) + "}";
  if (!controlCall(ctl, pending, 1, hello, timeoutMs, response) || !responseOk(response)) return false;
  const JsonValue pipe = jsonField(response, "pipe");
  if (!pipe.found || !pipe.isString || pipe.text.empty()) return false;

  Channel inst;
  const uint64_t deadline = nowMs() + static_cast<uint64_t>(timeoutMs < 100 ? 100 : timeoutMs);
  for (;;) {
    if (inst.connect(pipe.text, 50, false)) break;
    if (nowMs() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!beginStream(inst)) return false;

  // Switch: the agent replaces the sidecar from here on.
  m_out.clear();
  m_sink = std::move(inst);
  {
    std::lock_guard<std::mutex> lock(m_controlMutex);
    m_control = std::move(ctl);
  }
  setReady(SinkKind::Agent);
  return true;
}

void Runtime::onSinkFailure() {
  const bool wasAgent = m_sinkKind == SinkKind::Agent;
  m_sink.close();
  m_out.clear();
  m_nextAgentRetryMs = nowMs() + kAgentRetryIntervalMs;
  if (wasAgent) {
    std::lock_guard<std::mutex> lock(m_controlMutex);
    m_control.close();
  }
  setReady(SinkKind::None);
  if (wasAgent && m_config.sidecarEnabled) trySidecar();
}

void Runtime::appendOut(const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  m_out.insert(m_out.end(), p, p + size);
  if (m_out.size() >= kOutFlushBytes) writeOut();
}

void Runtime::writeOut() {
  if (m_out.empty()) return;
  if (!m_sink.isOpen()) {
    m_out.clear();
    return;
  }
  const bool ok = m_sink.writeAll(m_out.data(), m_out.size(), kWriteTimeoutMs);
  m_out.clear();
  if (!ok) onSinkFailure();
}

void Runtime::processFrames(const uint8_t* data, size_t size) {
  size_t off = 0;
  while (off + sizeof(inst::FrameHeader) <= size) {
    inst::FrameHeader fh;
    memcpy(&fh, data + off, sizeof(fh));
    const size_t total = sizeof(fh) + fh.byte_len;
    if (off + total > size) break;  // cannot happen: rings only contain whole frames
    const uint8_t* frame = data + off;
    bool forward = true;
    if (fh.type == static_cast<uint32_t>(inst::FrameType::StringDef) &&
        fh.byte_len >= sizeof(inst::StringDefPayload)) {
      uint64_t key;
      memcpy(&key, frame + sizeof(fh), sizeof(key));
      auto ins = m_stringFrames.try_emplace(key);
      if (ins.second) {
        ins.first->second.assign(frame, frame + total);
      } else {
        forward = false;  // already defined in this process: once per stream
      }
    } else if (fh.type == static_cast<uint32_t>(inst::FrameType::ThreadName) &&
               fh.byte_len >= sizeof(inst::ThreadNamePayload)) {
      uint32_t tid;
      memcpy(&tid, frame + sizeof(fh) + 8, sizeof(tid));
      m_threadNameFrames[tid].assign(frame, frame + total);
    }
    if (forward && m_sink.isOpen()) appendOut(frame, total);
    off += total;
  }
}

size_t Runtime::drainRing(ThreadRing* ring, bool& retiredDrained) {
  const bool retired = ring->retired.load(std::memory_order_acquire);
  const uint64_t head = ring->head.load(std::memory_order_acquire);
  const uint64_t tail = ring->tail.load(std::memory_order_relaxed);
  const size_t n = static_cast<size_t>(head - tail);
  if (n != 0) {
    if (m_scratch.size() < n) m_scratch.resize(n);
    ringGet(ring, tail, m_scratch.data(), n);
    processFrames(m_scratch.data(), n);
    ring->tail.store(head, std::memory_order_release);
  }
  const uint64_t dropped = ring->dropTotal.load(std::memory_order_relaxed);
  if (dropped != ring->dropReported) {
    Frame<inst::DroppedPayload> f;
    f.header = {static_cast<uint32_t>(inst::FrameType::Dropped), sizeof(inst::DroppedPayload)};
    f.payload.ts = nowTicks();
    f.payload.tid = ring->tid;
    f.payload.count = dropped - ring->dropReported;
    ring->dropReported = dropped;
    if (m_sink.isOpen()) appendOut(&f, sizeof(f));
  }
  retiredDrained = retired;
  return n;
}

size_t Runtime::drainAll() {
  {
    std::lock_guard<std::mutex> lock(m_ringsMutex);
    m_snapshot = m_rings;
  }
  size_t total = 0;
  std::vector<ThreadRing*> dead;
  for (ThreadRing* ring : m_snapshot) {
    bool done = false;
    total += drainRing(ring, done);
    if (done) dead.push_back(ring);
  }
  if (!dead.empty()) {
    {
      std::lock_guard<std::mutex> lock(m_ringsMutex);
      for (ThreadRing* d : dead) {
        for (size_t i = 0; i < m_rings.size(); ++i) {
          if (m_rings[i] == d) {
            m_rings[i] = m_rings.back();
            m_rings.pop_back();
            break;
          }
        }
      }
    }
    for (ThreadRing* d : dead) {
      ::operator delete(d->buf);
      delete d;
    }
  }
  return total;
}

void Runtime::pumpControl() {
  std::unique_lock<std::mutex> lock(m_controlMutex, std::try_to_lock);
  if (!lock.owns_lock() || !m_control.isOpen()) return;
  char buf[2048];
  for (;;) {
    const int n = m_control.readSome(buf, sizeof(buf), 0);  // discard unsolicited events
    if (n > 0) continue;
    if (n == 0 || n == -2) m_control.close();  // agent went away; the inst stream notices on its own
    return;
  }
}

// --------------------------------------------------------------------------------------------------------
// Shutdown
// --------------------------------------------------------------------------------------------------------

bool Runtime::waitThreadExit(int timeoutMs) {
  const uint64_t deadline = nowMs() + static_cast<uint64_t>(timeoutMs);
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(m_exitMutex);
      if (m_threadDone) return true;
      m_exitCv.wait_for(lock, std::chrono::milliseconds(10));
      if (m_threadDone) return true;
    }
#if defined(_WIN32)
    // The OS may already have terminated the thread (DLL detach during ExitProcess).
    if (WaitForSingleObject(static_cast<HANDLE>(m_thread.native_handle()), 0) == WAIT_OBJECT_0) return true;
#endif
    if (nowMs() >= deadline) return false;
  }
}

void Runtime::shutdown() {
  std::lock_guard<std::mutex> lock(m_shutdownMutex);
  if (m_shutDown) return;
  m_shutDown = true;
  if (g_runtime.load(std::memory_order_acquire) == this)
    g_state.store(kStateDisabled, std::memory_order_release);
  m_stop.store(true, std::memory_order_release);
  wake();
  if (m_started) {
    if (!waitThreadExit(3000)) {
      m_thread.detach();  // stuck in a write to a wedged agent: give up on the final drain
      return;
    }
    m_thread.join();
  }
#if defined(_WIN32)
  if (m_timerPeriodRaised) timeEndPeriod(1);
#endif
  // The flush thread is gone; this thread is now the (only) consumer.
  for (int i = 0; i < 8; ++i) {
    if (drainAll() == 0) break;
  }
  drainAll();  // pick up final Dropped reports
  writeOut();
  m_sink.close();
  {
    std::lock_guard<std::mutex> cl(m_controlMutex);
    m_control.close();
  }
  m_agentConnected.store(false, std::memory_order_release);
}

// --------------------------------------------------------------------------------------------------------
// Flush request and test hooks
// --------------------------------------------------------------------------------------------------------

int Runtime::requestFlush(const char* reason) {
  std::lock_guard<std::mutex> lock(m_controlMutex);
  if (m_shutDown) return 0;
  if (!m_control.isOpen()) {
    if (!m_control.connect(m_config.agentAddress, kInitialAgentTimeoutMs, true)) return 0;
  }
  const int64_t id = ++m_nextRequestId;
  const std::string request = "{\"id\":" + std::to_string(id) + ",\"cmd\":\"flush\",\"name\":\"" +
                              jsonEscape(reason) + "\",\"seconds\":10}";
  std::string pending;
  std::string response;
  if (!controlCall(m_control, pending, id, request, 3000, response)) {
    m_control.close();
    return 0;
  }
  return responseOk(response) ? 1 : 0;
}

int Runtime::waitReady(int timeoutMs) {
  const uint64_t deadline = nowMs() + static_cast<uint64_t>(timeoutMs);
  while (m_ready.load(std::memory_order_acquire) == 0 && nowMs() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const int r = m_ready.load(std::memory_order_acquire);
  return r == 1 ? 1 : r == 2 ? 2 : 0;
}

void Runtime::flushNow() {
  if (!m_started || m_shutDown) return;
  const uint64_t target = m_cycles.load(std::memory_order_acquire) + 3;
  const uint64_t deadline = nowMs() + 5000;
  while (m_cycles.load(std::memory_order_acquire) < target && nowMs() < deadline) {
    wake();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::string Runtime::sidecarPath() {
  std::lock_guard<std::mutex> lock(m_pathMutex);
  return m_activeSidecarPath;
}

}  // namespace tachyon::sdk
