// Tachyon dynamic-load example (WP-55): loads the SDK shared library at run time with tachyon_loader.h and
// profiles through the returned function table. If the library cannot be loaded, profiling silently becomes a
// no-op and the program still runs.
//
// Usage: dynamic_load_example [path-to-shared-library]
//        (default: $TACHYON_SDK_LIBRARY, then the platform's default library name)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "tachyon/tachyon_loader.h"

#if defined(_MSC_VER) && !defined(__clang__)
#define EXAMPLE_FORCEINLINE __forceinline
#else
#define EXAMPLE_FORCEINLINE inline __attribute__((always_inline))
#endif

namespace {

// The function table. Filled once at startup, before any thread uses it; read-only afterwards.
TachyonFunctions g_tachyon = {};

// RAII scope over the function table. The constructor and destructor must be force-inlined (like
// tachyon::Scope) so that the return address captured by BeginEvent lies in the profiled function.
class DynScope {
 public:
  EXAMPLE_FORCEINLINE DynScope(const char* id, const char* data = nullptr,
                               uint32_t color = TACHYON_DEFAULT_COLOR) {
    if (g_tachyon.BeginEvent != nullptr) g_tachyon.BeginEvent(id, data, color);
  }
  EXAMPLE_FORCEINLINE ~DynScope() {
    if (g_tachyon.EndEvent != nullptr) (void)g_tachyon.EndEvent();
  }
  DynScope(const DynScope&) = delete;
  DynScope& operator=(const DynScope&) = delete;
};

void simulateWork(int iteration) {
  char data[32];
  std::snprintf(data, sizeof(data), "iteration=%d", iteration);
  DynScope scope("Example.Work", data, TACHYON_MAKE_COLOR(0x4C, 0xAF, 0x50));
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : std::getenv("TACHYON_SDK_LIBRARY");
  if (path == nullptr) {
#if defined(_WIN32)
    path = "tachyon_sdk_shared.dll";
#else
    path = "libtachyon_sdk_shared.so";
#endif
  }
  // Call from main() or a normal thread, never from DllMain / a static initializer (loader-lock rules).
  const bool loaded = Tachyon_LoadFrom(path, &g_tachyon) == 1;
  std::printf("tachyon %s (%s)\n", loaded ? "loaded" : "not available, profiling disabled", path);

  if (loaded) g_tachyon.SetCurrentThreadName("Main");
  for (int frame = 0; frame < 20; ++frame) {
    if (loaded) g_tachyon.FrameMark("Main", static_cast<uint64_t>(frame));
    simulateWork(frame);
  }
  if (loaded) std::printf("connected to agent: %d\n", g_tachyon.IsConnected());
  return 0;
}
