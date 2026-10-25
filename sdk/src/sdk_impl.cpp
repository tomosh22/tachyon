// Minimal WP-50 bodies: safe, thread-safe, allocation-free. WP-51 replaces these with ring buffers.
#include <stddef.h>
#include <string.h>

#include "sdk_internal.h"
#include "tachyon/tachyon.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define TACHYON_SDK_NOINLINE __declspec(noinline)
#define TACHYON_SDK_CALLER() _ReturnAddress()
#else
#define TACHYON_SDK_NOINLINE __attribute__((noinline))
#define TACHYON_SDK_CALLER() __builtin_return_address(0)
#endif

extern "C" const char kTachyonLockEventId[] = "Tachyon.LockAcquire";

namespace {

constexpr size_t kMaxDataBytes = 4096;

struct ThreadState {
  const void* lastCaller;
  uint32_t lastColor;
  char lastData[kMaxDataBytes + 1];
};

thread_local ThreadState t_state = {nullptr, 0, {0}};

size_t boundedLength(const char* s, size_t limit) {
  size_t n = 0;
  while (n < limit && s[n] != '\0') ++n;
  return n;
}

void recordData(const char* data, size_t len) {
  if (len > kMaxDataBytes) len = kMaxDataBytes;
  if (data != nullptr && len != 0) memcpy(t_state.lastData, data, len);
  t_state.lastData[len] = '\0';
}

}  // namespace

extern "C" {

const void* Tachyon_Internal_LastBeginCaller(void) {
  return t_state.lastCaller;
}
void Tachyon_Internal_ResetThreadState(void) {
  t_state.lastCaller = nullptr;
  t_state.lastColor = 0;
  t_state.lastData[0] = '\0';
}
uint32_t Tachyon_Internal_LastBeginColor(void) {
  return t_state.lastColor;
}
const char* Tachyon_Internal_LastBeginData(void) {
  return t_state.lastData;
}

void Tachyon_SetCurrentThreadName(const char* utf8) {
  (void)utf8;
}
void Tachyon_SetCurrentThreadName_N(const char* utf8, uint16_t len) {
  (void)utf8;
  (void)len;
}

// Must remain noinline and must read its own return address directly: this is the scope's caller address.
TACHYON_SDK_NOINLINE void Tachyon_BeginEvent(const char* id, const char* data, uint32_t color) {
  (void)id;
  t_state.lastCaller = TACHYON_SDK_CALLER();
  t_state.lastColor = color;
  recordData(data, data != nullptr ? boundedLength(data, kMaxDataBytes) : 0);
}

TACHYON_SDK_NOINLINE void Tachyon_BeginEvent_N(const char* id, uint16_t idLen, const char* data,
                                               uint16_t dataLen, uint32_t color) {
  (void)id;
  (void)idLen;
  t_state.lastCaller = TACHYON_SDK_CALLER();
  t_state.lastColor = color;
  recordData(data, dataLen);
}

TachyonSuppressTailCall Tachyon_EndEvent(void) {
  TachyonSuppressTailCall r = {{0, 0, 0}};
  return r;
}

void Tachyon_RegisterFiber(uint64_t fiberId) {
  (void)fiberId;
}
void Tachyon_UnregisterFiber(uint64_t fiberId) {
  (void)fiberId;
}
void Tachyon_BeginFiberSwitch(uint64_t currentFiber, uint64_t nextFiber) {
  (void)currentFiber;
  (void)nextFiber;
}
void Tachyon_EndFiberSwitch(uint64_t fiberId) {
  (void)fiberId;
}

void Tachyon_FrameMark(const char* name, uint64_t frameNumber) {
  (void)name;
  (void)frameNumber;
}
void Tachyon_Marker(const char* name, const char* data, uint32_t color) {
  (void)name;
  (void)data;
  (void)color;
}
void Tachyon_Plot(const char* name, double value) {
  (void)name;
  (void)value;
}

// Lock wait = BeginEvent with the hex address as data and the reserved color sentinel as the kind flag.
// (WP-51 will use lockName, or kTachyonLockEventId when NULL, as the event id.) The hex string is formatted
// into a stack buffer: no allocation. Noinline and reads its own return address, like Tachyon_BeginEvent.
TACHYON_SDK_NOINLINE void Tachyon_LockAcquireBegin(const void* lockAddr, const char* lockName) {
  char buf[2 + 16 + 1];
  char digits[16];
  uintptr_t v = reinterpret_cast<uintptr_t>(lockAddr);
  int n = 0;
  do {
    digits[n++] = "0123456789abcdef"[v & 0xF];
    v >>= 4;
  } while (v != 0 && n < 16);
  int o = 0;
  buf[o++] = '0';
  buf[o++] = 'x';
  while (n > 0) buf[o++] = digits[--n];
  buf[o] = '\0';
  t_state.lastCaller = TACHYON_SDK_CALLER();
  t_state.lastColor = TACHYON_LOCK_COLOR_SENTINEL;
  recordData(buf, static_cast<size_t>(o));
  (void)lockName;
}
void Tachyon_LockAcquireEnd(const void* lockAddr) {
  (void)lockAddr;
}

int Tachyon_IsConnected(void) {
  return 0;
}
int Tachyon_RequestFlush(const char* reason) {
  (void)reason;
  return 0;
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
