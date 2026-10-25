#include "testbed/platform.h"

#include <vector>

#if TACHYON_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif TACHYON_PLATFORM_LINUX
#include <pthread.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace tachyon::testbed {

#if TACHYON_PLATFORM_WINDOWS

uint64_t currentProcessId() {
  return GetCurrentProcessId();
}

uint64_t currentThreadId() {
  return GetCurrentThreadId();
}

void setCurrentThreadName(const std::string& name) {
  std::wstring wide(name.begin(), name.end());  // thread names in the testbed are ASCII
  SetThreadDescription(GetCurrentThread(), wide.c_str());
}

const char* platformName() {
  return "windows";
}

struct FiberGroup::Impl {
  struct Fib {
    LPVOID handle = nullptr;
    Entry entry = nullptr;
    void* arg = nullptr;
    Impl* owner = nullptr;
  };
  LPVOID mainFiber = nullptr;
  bool convertedThread = false;
  std::vector<std::unique_ptr<Fib>> fibers;

  static VOID CALLBACK trampoline(LPVOID param) {
    auto* fib = static_cast<Fib*>(param);
    fib->entry(fib->arg);
    for (;;)
      SwitchToFiber(fib->owner->mainFiber);  // an entry must never return; returning would exit the thread
  }
};

FiberGroup::FiberGroup() : m_impl(std::make_unique<Impl>()) {
  if (IsThreadAFiber()) {
    m_impl->mainFiber = GetCurrentFiber();
  } else {
    m_impl->mainFiber = ConvertThreadToFiber(nullptr);
    m_impl->convertedThread = m_impl->mainFiber != nullptr;
  }
}

FiberGroup::~FiberGroup() {
  for (auto& fib : m_impl->fibers) DeleteFiber(fib->handle);
  if (m_impl->convertedThread) ConvertFiberToThread();
}

bool FiberGroup::ok() const {
  return m_impl->mainFiber != nullptr;
}

int FiberGroup::add(Entry entry, void* arg) {
  auto fib = std::make_unique<Impl::Fib>();
  fib->entry = entry;
  fib->arg = arg;
  fib->owner = m_impl.get();
  fib->handle = CreateFiber(256 * 1024, &Impl::trampoline, fib.get());
  if (fib->handle == nullptr) return -1;
  m_impl->fibers.push_back(std::move(fib));
  return static_cast<int>(m_impl->fibers.size()) - 1;
}

void FiberGroup::resume(int index) {
  SwitchToFiber(m_impl->fibers[static_cast<size_t>(index)]->handle);
}

void FiberGroup::yield() {
  SwitchToFiber(m_impl->mainFiber);
}

#elif TACHYON_PLATFORM_LINUX

uint64_t currentProcessId() {
  return static_cast<uint64_t>(getpid());
}

uint64_t currentThreadId() {
  return static_cast<uint64_t>(syscall(SYS_gettid));
}

void setCurrentThreadName(const std::string& name) {
  pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}

const char* platformName() {
  return "linux";
}

namespace {
struct LinuxFiber {
  ucontext_t context{};
  std::unique_ptr<char[]> stack;
  FiberGroup::Entry entry = nullptr;
  void* arg = nullptr;
  ucontext_t* mainContext = nullptr;
};

thread_local LinuxFiber* t_startingFiber = nullptr;

void fiberTrampoline() {
  LinuxFiber* fib = t_startingFiber;
  fib->entry(fib->arg);
  for (;;) swapcontext(&fib->context, fib->mainContext);  // an entry must never return
}
}  // namespace

struct FiberGroup::Impl {
  ucontext_t mainContext{};
  std::vector<std::unique_ptr<LinuxFiber>> fibers;
  int current = -1;
};

FiberGroup::FiberGroup() : m_impl(std::make_unique<Impl>()) {}
FiberGroup::~FiberGroup() = default;

bool FiberGroup::ok() const {
  return true;
}

int FiberGroup::add(Entry entry, void* arg) {
  constexpr size_t kStackSize = 256 * 1024;
  auto fib = std::make_unique<LinuxFiber>();
  fib->entry = entry;
  fib->arg = arg;
  fib->mainContext = &m_impl->mainContext;
  fib->stack = std::make_unique<char[]>(kStackSize);
  if (getcontext(&fib->context) != 0) return -1;
  fib->context.uc_stack.ss_sp = fib->stack.get();
  fib->context.uc_stack.ss_size = kStackSize;
  fib->context.uc_link = nullptr;
  makecontext(&fib->context, &fiberTrampoline, 0);
  m_impl->fibers.push_back(std::move(fib));
  return static_cast<int>(m_impl->fibers.size()) - 1;
}

void FiberGroup::resume(int index) {
  LinuxFiber* fib = m_impl->fibers[static_cast<size_t>(index)].get();
  m_impl->current = index;
  t_startingFiber = fib;
  swapcontext(&m_impl->mainContext, &fib->context);
}

void FiberGroup::yield() {
  LinuxFiber* fib = m_impl->fibers[static_cast<size_t>(m_impl->current)].get();
  swapcontext(&fib->context, &m_impl->mainContext);
}

#else
#error "tachyon-testbed supports Windows and Linux only"
#endif

}  // namespace tachyon::testbed
