#include "symbols/dia/msvc_undecorate.h"

#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>

#include <mutex>
#endif

namespace tachyon::symbols {

#if TACHYON_PLATFORM_WINDOWS
namespace {
// DbgHelp documents all of its functions as single-threaded. This lock is the one piece of process-wide state
// of the DIA provider; it guards no data, only the call.
std::mutex g_dbghelpMutex;
}  // namespace
#endif

std::string undecorateMsvc(std::string_view name) {
#if TACHYON_PLATFORM_WINDOWS
  if (name.empty() || name[0] != '?') return std::string(name);
  const std::string copy(name);
  char out[2048];
  DWORD n = 0;
  {
    std::lock_guard<std::mutex> lock(g_dbghelpMutex);
    n = UnDecorateSymbolName(copy.c_str(), out, static_cast<DWORD>(sizeof(out)), UNDNAME_NAME_ONLY);
  }
  if (n == 0) return copy;
  return std::string(out, n);
#else
  return std::string(name);
#endif
}

}  // namespace tachyon::symbols
