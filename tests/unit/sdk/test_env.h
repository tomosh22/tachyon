// Test-only environment helpers shared by the SDK runtime tests.
#pragma once

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tachyon::sdk_testing {

// Not thread-safe: call from the test thread only.
inline void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
  SetEnvironmentVariableA(name, value.c_str());
#else
  ::setenv(name, value.c_str(), 1);
#endif
}

inline void unsetEnv(const char* name) {
#if defined(_WIN32)
  SetEnvironmentVariableA(name, nullptr);
#else
  ::unsetenv(name);
#endif
}

}  // namespace tachyon::sdk_testing
