// Public C API (sdk/include/tachyon/tachyon.h) over the per-thread rings. Hot path: one relaxed load of the
// global state, one thread_local load, a few stores, one clock read, one release store; no locks, no
// allocation after the thread's first call.
#include <stddef.h>
#include <string.h>

#include "inst_wire.h"
#include "sdk_internal.h"
#include "sdk_platform.h"
#include "sdk_ring.h"
#include "sdk_runtime.h"
#include "tachyon/tachyon.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define TACHYON_SDK_NOINLINE __declspec(noinline)
#define TACHYON_SDK_FORCEINLINE __forceinline
#define TACHYON_SDK_CALLER() _ReturnAddress()
#else
#define TACHYON_SDK_NOINLINE __attribute__((noinline))
#define TACHYON_SDK_FORCEINLINE inline __attribute__((always_inline))
#define TACHYON_SDK_CALLER() __builtin_return_address(0)
#endif

extern "C" const char kTachyonLockEventId[] = "Tachyon.LockAcquire";

namespace {

using tachyon::sdk::Frame;
using tachyon::sdk::kStateActive;
using tachyon::sdk::kStateUninit;
using tachyon::sdk::nowTicks;
using tachyon::sdk::ScopeStack;
using tachyon::sdk::ThreadRing;
namespace inst = tachyon::inst;

constexpr size_t kMaxData = inst::kMaxDataBytes;
const char kNullId[] = "(null)";

// ---- per-thread ring access -----------------------------------------------------------------------------

thread_local ThreadRing* t_ring = nullptr;
thread_local bool t_dead = false;  // the thread's exit guard has run: never register again

struct ExitGuard {
  ~ExitGuard() {
    t_dead = true;
    ThreadRing* ring = t_ring;  // null when a test restart already retired it
    t_ring = nullptr;
    if (ring != nullptr) ring->retired.store(true, std::memory_order_release);
  }
};

uint64_t endCredit(const ThreadRing* r) {
  return static_cast<uint64_t>(r->stack.emittedOpen) * inst::kScopeEndFrameBytes;
}

// Writes a fixed-size frame (+ optional trailing bytes) if it fits while keeping the End-frame headroom.
template <typename P>
bool emitFrame(ThreadRing* r, inst::FrameType type, const P& payload, const void* extra, size_t extraLen) {
  const uint64_t total = sizeof(Frame<P>) + extraLen;
  if (!tachyon::sdk::ringReserve(r, total + endCredit(r))) {
    tachyon::sdk::ringCountDrop(r);
    return false;
  }
  Frame<P> f;
  f.header.type = static_cast<uint32_t>(type);
  f.header.byte_len = static_cast<uint32_t>(sizeof(P) + extraLen);
  f.payload = payload;
  const uint64_t pos = r->head.load(std::memory_order_relaxed);
  tachyon::sdk::ringPut(r, pos, &f, sizeof(f));
  if (extraLen != 0) tachyon::sdk::ringPut(r, pos + sizeof(f), extra, extraLen);
  tachyon::sdk::ringPublish(r, pos + total);
  return true;
}

size_t boundedLength(const char* s, size_t limit) {
  const void* nul = memchr(s, 0, limit);
  return nul != nullptr ? static_cast<size_t>(static_cast<const char*>(nul) - s) : limit;
}

void writeThreadName(ThreadRing* r, const char* name, size_t len) {
  if (len > inst::kMaxThreadNameBytes) len = inst::kMaxThreadNameBytes;
  inst::ThreadNamePayload p;
  p.ts = nowTicks();
  p.tid = r->tid;
  p.name_len = static_cast<uint16_t>(len);
  emitFrame(r, inst::FrameType::ThreadName, p, name, len);
}

// Cold path: runtime start, ring allocation, OS thread name capture.
TACHYON_SDK_NOINLINE ThreadRing* registerSlow() {
  if (t_dead) return nullptr;
  tachyon::sdk::Runtime* rt = tachyon::sdk::runtime();
  if (rt == nullptr || tachyon::sdk::g_state.load(std::memory_order_relaxed) != kStateActive) return nullptr;
  ThreadRing* ring = rt->registerThread();
  if (ring == nullptr) return nullptr;
  thread_local ExitGuard guard;
  (void)guard;
  t_ring = ring;
  const std::string osName = tachyon::sdk::currentOsThreadName();
  if (!osName.empty()) writeThreadName(ring, osName.data(), osName.size());
  return ring;
}

// Cold path of the state check: first call ever (starts the runtime) or runtime disabled.
TACHYON_SDK_NOINLINE ThreadRing* acquireSlowState() {
  if (tachyon::sdk::g_state.load(std::memory_order_relaxed) == kStateUninit) tachyon::sdk::runtime();
  if (tachyon::sdk::g_state.load(std::memory_order_relaxed) != kStateActive) return nullptr;
  ThreadRing* r = t_ring;
  return r != nullptr ? r : registerSlow();
}

TACHYON_SDK_FORCEINLINE ThreadRing* acquireRing() {
  if (TACHYON_SDK_UNLIKELY(tachyon::sdk::g_state.load(std::memory_order_relaxed) != kStateActive))
    return acquireSlowState();
  ThreadRing* r = t_ring;
  if (TACHYON_SDK_UNLIKELY(r == nullptr)) return registerSlow();
  return r;
}

// ---- strings --------------------------------------------------------------------------------------------

// Writes a StringDef frame into this thread's ring. `len` == SIZE_MAX means NUL-terminated.
TACHYON_SDK_NOINLINE bool defineString(ThreadRing* r, uint64_t key, const char* s, size_t len) {
  len = len == SIZE_MAX ? boundedLength(s, kMaxData) : (len > kMaxData ? kMaxData : len);
  inst::StringDefPayload p;
  p.ptr_key = key;
  p.len = static_cast<uint32_t>(len);
  if (!emitFrame(r, inst::FrameType::StringDef, p, s, len)) return false;
  tachyon::sdk::strInsert(r, key);
  return true;
}

// Resolves the wire key of a static string and makes sure it is defined in this ring.
TACHYON_SDK_FORCEINLINE bool ensureString(ThreadRing* r, const char* s, uint64_t key, size_t len) {
  if (TACHYON_SDK_LIKELY(tachyon::sdk::strKnown(r, key))) return true;
  return defineString(r, key, s, len);
}

TACHYON_SDK_FORCEINLINE uint64_t keyNul(const char* s) {
  return inst::makeStringKey(reinterpret_cast<uintptr_t>(s), 0);
}

// ---- scopes ---------------------------------------------------------------------------------------------

void markDropped(ThreadRing* r, uint32_t depth) {
  ScopeStack& st = r->stack;
  st.droppedBits[depth >> 6] |= 1ull << (depth & 63);
  ++st.openDropped;
  tachyon::sdk::ringCountDrop(r);
}

// Records a scope begin. `idKey` identifies `id`; `idLen` is SIZE_MAX for NUL-terminated ids.
TACHYON_SDK_FORCEINLINE void beginImpl(ThreadRing* r, const char* id, uint64_t idKey, size_t idLen,
                                       const char* data, size_t dataLen, uint32_t color, const void* caller) {
  ScopeStack& st = r->stack;
  const uint32_t depth = st.depth++;
  if (TACHYON_SDK_UNLIKELY(depth >= tachyon::sdk::kMaxTrackedDepth)) {
    tachyon::sdk::ringCountDrop(r);
    return;
  }
  if (TACHYON_SDK_UNLIKELY(!ensureString(r, id, idKey, idLen))) {
    markDropped(r, depth);
    return;
  }
  const uint64_t total = sizeof(Frame<inst::ScopeBeginPayload>) + dataLen;
  // Keep room for this scope's End frame and the End frames of every recorded open scope.
  if (TACHYON_SDK_UNLIKELY(!tachyon::sdk::ringReserve(r, total + endCredit(r) + inst::kScopeEndFrameBytes))) {
    markDropped(r, depth);
    return;
  }
  Frame<inst::ScopeBeginPayload> f;
  f.header.type = static_cast<uint32_t>(inst::FrameType::ScopeBegin);
  f.header.byte_len = static_cast<uint32_t>(sizeof(inst::ScopeBeginPayload) + dataLen);
  f.payload.tid = r->tid;
  f.payload.color_rgba = color;
  f.payload.id_key = idKey;
  f.payload.caller_addr = reinterpret_cast<uintptr_t>(caller);
  f.payload.data_len = static_cast<uint16_t>(dataLen);
  f.payload.ts = nowTicks();
  const uint64_t pos = r->head.load(std::memory_order_relaxed);
  tachyon::sdk::ringPut(r, pos, &f, sizeof(f));
  if (dataLen != 0) tachyon::sdk::ringPut(r, pos + sizeof(f), data, dataLen);
  r->lastBeginPos = pos;
  ++st.emittedOpen;
  tachyon::sdk::ringPublish(r, pos + total);
}

TACHYON_SDK_FORCEINLINE void endImpl(ThreadRing* r) {
  ScopeStack& st = r->stack;
  if (TACHYON_SDK_LIKELY(st.depth != 0)) {
    const uint32_t depth = --st.depth;
    if (TACHYON_SDK_UNLIKELY(st.openDropped != 0 || depth >= tachyon::sdk::kMaxTrackedDepth)) {
      if (depth >= tachyon::sdk::kMaxTrackedDepth) {
        tachyon::sdk::ringCountDrop(r);
        return;
      }
      uint64_t& word = st.droppedBits[depth >> 6];
      const uint64_t bit = 1ull << (depth & 63);
      if ((word & bit) != 0) {  // its Begin was dropped: drop the End too
        word &= ~bit;
        --st.openDropped;
        tachyon::sdk::ringCountDrop(r);
        return;
      }
    }
    if (st.emittedOpen != 0) --st.emittedOpen;
  }
  // Space for the End of a recorded Begin was reserved by that Begin, so this only fails for unmatched Ends
  // (or fibers that migrated without the switch API).
  const uint64_t total = inst::kScopeEndFrameBytes;
  if (TACHYON_SDK_UNLIKELY(!tachyon::sdk::ringReserve(r, total))) {
    tachyon::sdk::ringCountDrop(r);
    return;
  }
  Frame<inst::ScopeEndPayload> f;
  f.header.type = static_cast<uint32_t>(inst::FrameType::ScopeEnd);
  f.header.byte_len = sizeof(inst::ScopeEndPayload);
  f.payload.tid = r->tid;
  f.payload.ts = nowTicks();
  const uint64_t pos = r->head.load(std::memory_order_relaxed);
  tachyon::sdk::ringPut(r, pos, &f, sizeof(f));
  tachyon::sdk::ringPublish(r, pos + total);
}

// ---- test hook support ----------------------------------------------------------------------------------

bool readLastBegin(Frame<inst::ScopeBeginPayload>& f, char* data) {
  const ThreadRing* r = t_ring;
  if (r == nullptr || r->lastBeginPos == ~0ull) return false;
  tachyon::sdk::ringGet(r, r->lastBeginPos, &f, sizeof(f));
  const size_t n = f.payload.data_len <= kMaxData ? f.payload.data_len : kMaxData;
  if (n != 0) tachyon::sdk::ringGet(r, r->lastBeginPos + sizeof(f), data, n);
  data[n] = '\0';
  return true;
}

void formatHexAddress(const void* addr, char* buf, size_t* len) {
  char digits[16];
  uintptr_t v = reinterpret_cast<uintptr_t>(addr);
  int n = 0;
  do {
    digits[n++] = "0123456789abcdef"[v & 0xF];
    v >>= 4;
  } while (v != 0 && n < 16);
  size_t o = 0;
  buf[o++] = '0';
  buf[o++] = 'x';
  while (n > 0) buf[o++] = digits[--n];
  buf[o] = '\0';
  *len = o;
}

void emitFiber(ThreadRing* r, inst::FiberEventKind kind, uint64_t fiber, uint64_t other) {
  inst::FiberEventPayload p;
  memset(&p, 0, sizeof(p));
  p.ts = nowTicks();
  p.tid = r->tid;
  p.kind = static_cast<uint8_t>(kind);
  p.fiber_id = fiber;
  p.other_fiber_id = other;
  emitFrame(r, inst::FrameType::FiberEvent, p, nullptr, 0);
}

}  // namespace

extern "C" {

// ---- test-only accessors (sdk_internal.h) ----------------------------------------------------------------

const void* Tachyon_Internal_LastBeginCaller(void) {
  Frame<inst::ScopeBeginPayload> f;
  char data[kMaxData + 1];
  if (!readLastBegin(f, data)) return nullptr;
  return reinterpret_cast<const void*>(static_cast<uintptr_t>(f.payload.caller_addr));
}
void Tachyon_Internal_ResetThreadState(void) {
  if (t_ring != nullptr) {
    t_ring->lastBeginPos = ~0ull;
    t_ring->stack.reset();
  }
}
uint32_t Tachyon_Internal_LastBeginColor(void) {
  Frame<inst::ScopeBeginPayload> f;
  char data[kMaxData + 1];
  return readLastBegin(f, data) ? f.payload.color_rgba : 0;
}
const char* Tachyon_Internal_LastBeginData(void) {
  static thread_local char t_data[kMaxData + 1];
  Frame<inst::ScopeBeginPayload> f;
  if (!readLastBegin(f, t_data)) t_data[0] = '\0';
  return t_data;
}
int Tachyon_Internal_WaitReady(int timeoutMs) {
  tachyon::sdk::Runtime* rt = tachyon::sdk::runtime();
  return rt != nullptr ? rt->waitReady(timeoutMs) : 0;
}
void Tachyon_Internal_Flush(void) {
  if (tachyon::sdk::Runtime* rt = tachyon::sdk::runtimeIfCreated()) rt->flushNow();
}
void Tachyon_Internal_Shutdown(void) {
  if (tachyon::sdk::Runtime* rt = tachyon::sdk::runtimeIfCreated()) rt->shutdown();
}
void Tachyon_Internal_Restart(void) {
  if (t_ring != nullptr) {
    t_ring->retired.store(true, std::memory_order_release);
    t_ring = nullptr;
  }
  tachyon::sdk::restartRuntimeForTests();
}
const char* Tachyon_Internal_SidecarPath(void) {
  static thread_local std::string t_path;
  t_path.clear();
  if (tachyon::sdk::Runtime* rt = tachyon::sdk::runtimeIfCreated()) t_path = rt->sidecarPath();
  return t_path.c_str();
}
uint64_t Tachyon_Internal_ThreadDropCount(void) {
  return t_ring != nullptr ? t_ring->dropTotal.load(std::memory_order_relaxed) : 0;
}
uint64_t Tachyon_Internal_RingBytes(void) {
  return t_ring != nullptr ? t_ring->cap : 0;
}

// ---- public API ------------------------------------------------------------------------------------------

void Tachyon_SetCurrentThreadName(const char* utf8) {
  if (utf8 == nullptr) return;
  ThreadRing* r = acquireRing();
  if (r == nullptr) return;
  writeThreadName(r, utf8, boundedLength(utf8, inst::kMaxThreadNameBytes));
}
void Tachyon_SetCurrentThreadName_N(const char* utf8, uint16_t len) {
  if (utf8 == nullptr) return;
  ThreadRing* r = acquireRing();
  if (r == nullptr) return;
  writeThreadName(r, utf8, len);
}

// Must remain noinline and must read its own return address directly: this is the scope's caller address.
TACHYON_SDK_NOINLINE void Tachyon_BeginEvent(const char* id, const char* data, uint32_t color) {
  const void* caller = TACHYON_SDK_CALLER();
  ThreadRing* r = acquireRing();
  if (TACHYON_SDK_UNLIKELY(r == nullptr)) return;
  if (TACHYON_SDK_UNLIKELY(id == nullptr)) id = kNullId;
  const size_t dataLen = data != nullptr ? boundedLength(data, kMaxData) : 0;
  beginImpl(r, id, keyNul(id), SIZE_MAX, data, dataLen, color, caller);
}

TACHYON_SDK_NOINLINE void Tachyon_BeginEvent_N(const char* id, uint16_t idLen, const char* data,
                                               uint16_t dataLen, uint32_t color) {
  const void* caller = TACHYON_SDK_CALLER();
  ThreadRing* r = acquireRing();
  if (TACHYON_SDK_UNLIKELY(r == nullptr)) return;
  if (TACHYON_SDK_UNLIKELY(id == nullptr)) {
    id = kNullId;
    idLen = static_cast<uint16_t>(sizeof(kNullId) - 1);
  }
  const size_t clippedId = idLen > kMaxData ? kMaxData : idLen;
  size_t dl = data != nullptr ? dataLen : 0;
  if (dl > kMaxData) dl = kMaxData;
  beginImpl(r, id, inst::makeStringKey(reinterpret_cast<uintptr_t>(id), static_cast<uint32_t>(clippedId) + 1),
            clippedId, data, dl, color, caller);
}

TachyonSuppressTailCall Tachyon_EndEvent(void) {
  TachyonSuppressTailCall result = {{0, 0, 0}};
  ThreadRing* r = acquireRing();
  if (TACHYON_SDK_LIKELY(r != nullptr)) endImpl(r);
  return result;
}

void Tachyon_RegisterFiber(uint64_t fiberId) {
  ThreadRing* r = acquireRing();
  if (r == nullptr) return;
  emitFiber(r, inst::FiberEventKind::Register, fiberId, 0);
  if (tachyon::sdk::Runtime* rt = tachyon::sdk::runtimeIfCreated()) rt->registerFiber(fiberId);
}
void Tachyon_UnregisterFiber(uint64_t fiberId) {
  ThreadRing* r = acquireRing();
  if (r == nullptr) return;
  emitFiber(r, inst::FiberEventKind::Unregister, fiberId, 0);
  if (tachyon::sdk::Runtime* rt = tachyon::sdk::runtimeIfCreated()) rt->unregisterFiber(fiberId);
}
// The thread leaves `currentFiber`: save its open-scope bookkeeping and start with an empty stack.
void Tachyon_BeginFiberSwitch(uint64_t currentFiber, uint64_t nextFiber) {
  ThreadRing* r = acquireRing();
  if (r == nullptr) return;
  emitFiber(r, inst::FiberEventKind::SwitchBegin, currentFiber, nextFiber);
  if (tachyon::sdk::Runtime* rt = tachyon::sdk::runtimeIfCreated()) rt->saveFiber(currentFiber, r->stack);
  r->stack.reset();
}
// The thread resumes `fiberId` (possibly a different thread than the one that left it).
void Tachyon_EndFiberSwitch(uint64_t fiberId) {
  ThreadRing* r = acquireRing();
  if (r == nullptr) return;
  emitFiber(r, inst::FiberEventKind::SwitchEnd, fiberId, 0);
  tachyon::sdk::Runtime* rt = tachyon::sdk::runtimeIfCreated();
  if (rt == nullptr || !rt->loadFiber(fiberId, r->stack)) r->stack.reset();
}

void Tachyon_FrameMark(const char* name, uint64_t frameNumber) {
  ThreadRing* r = acquireRing();
  if (r == nullptr) return;
  if (name == nullptr) name = kNullId;
  const uint64_t key = keyNul(name);
  if (!ensureString(r, name, key, SIZE_MAX)) {
    tachyon::sdk::ringCountDrop(r);
    return;
  }
  inst::FrameMarkPayload p;
  p.ts = nowTicks();
  p.tid = r->tid;
  p.name_key = key;
  p.frame_number = frameNumber;
  emitFrame(r, inst::FrameType::FrameMark, p, nullptr, 0);
}

void Tachyon_Marker(const char* name, const char* data, uint32_t color) {
  ThreadRing* r = acquireRing();
  if (r == nullptr) return;
  if (name == nullptr) name = kNullId;
  const uint64_t key = keyNul(name);
  if (!ensureString(r, name, key, SIZE_MAX)) {
    tachyon::sdk::ringCountDrop(r);
    return;
  }
  const size_t dataLen = data != nullptr ? boundedLength(data, kMaxData) : 0;
  inst::MarkerPayload p;
  p.ts = nowTicks();
  p.tid = r->tid;
  p.name_key = key;
  p.color_rgba = color;
  p.data_len = static_cast<uint16_t>(dataLen);
  emitFrame(r, inst::FrameType::Marker, p, data, dataLen);
}

void Tachyon_Plot(const char* name, double value) {
  ThreadRing* r = acquireRing();
  if (r == nullptr) return;
  if (name == nullptr) name = kNullId;
  const uint64_t key = keyNul(name);
  if (!ensureString(r, name, key, SIZE_MAX)) {
    tachyon::sdk::ringCountDrop(r);
    return;
  }
  inst::PlotPayload p;
  p.ts = nowTicks();
  p.name_key = key;
  p.value = value;
  emitFrame(r, inst::FrameType::Plot, p, nullptr, 0);
}

// Lock wait = BeginEvent with the hex address as data and the reserved color sentinel as the kind flag; the
// id is lockName (static) or the reserved "Tachyon.LockAcquire". Noinline and reads its own return address,
// like Tachyon_BeginEvent. The hex string is formatted on the stack: no allocation.
TACHYON_SDK_NOINLINE void Tachyon_LockAcquireBegin(const void* lockAddr, const char* lockName) {
  const void* caller = TACHYON_SDK_CALLER();
  ThreadRing* r = acquireRing();
  if (TACHYON_SDK_UNLIKELY(r == nullptr)) return;
  const char* id = lockName != nullptr ? lockName : kTachyonLockEventId;
  char buf[2 + 16 + 1];
  size_t len = 0;
  formatHexAddress(lockAddr, buf, &len);
  beginImpl(r, id, keyNul(id), SIZE_MAX, buf, len, TACHYON_LOCK_COLOR_SENTINEL, caller);
}
void Tachyon_LockAcquireEnd(const void* lockAddr) {
  (void)lockAddr;
  ThreadRing* r = acquireRing();
  if (r != nullptr) endImpl(r);
}

int Tachyon_IsConnected(void) {
  tachyon::sdk::Runtime* rt = tachyon::sdk::runtime();
  return rt != nullptr && rt->agentConnected() ? 1 : 0;
}

// Synchronous: performs one control-channel round trip (flush command) on the calling thread and returns
// whether the agent answered ok. Never called on the recording hot path.
int Tachyon_RequestFlush(const char* reason) {
  tachyon::sdk::Runtime* rt = tachyon::sdk::runtime();
  return rt != nullptr ? rt->requestFlush(reason) : 0;
}

int Tachyon_GetAPI(int version, TachyonFunctions* out) {
  if (out == nullptr) return 0;
  memset(out, 0, sizeof(*out));
  if (version != TACHYON_API_VERSION) return 0;
  out->SetCurrentThreadName = &Tachyon_SetCurrentThreadName;
  out->SetCurrentThreadName_N = &Tachyon_SetCurrentThreadName_N;
  out->BeginEvent = &Tachyon_BeginEvent;
  out->BeginEvent_N = &Tachyon_BeginEvent_N;
  out->EndEvent = &Tachyon_EndEvent;
  out->RegisterFiber = &Tachyon_RegisterFiber;
  out->UnregisterFiber = &Tachyon_UnregisterFiber;
  out->BeginFiberSwitch = &Tachyon_BeginFiberSwitch;
  out->EndFiberSwitch = &Tachyon_EndFiberSwitch;
  out->FrameMark = &Tachyon_FrameMark;
  out->Marker = &Tachyon_Marker;
  out->Plot = &Tachyon_Plot;
  out->LockAcquireBegin = &Tachyon_LockAcquireBegin;
  out->LockAcquireEnd = &Tachyon_LockAcquireEnd;
  out->IsConnected = &Tachyon_IsConnected;
  out->RequestFlush = &Tachyon_RequestFlush;
  return 1;
}

}  // extern "C"
