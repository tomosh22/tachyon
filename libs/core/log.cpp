#include "core/log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace tachyon::core {
namespace {

std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::mutex g_sinkMutex;
LogSink g_sink;  // guarded by g_sinkMutex; empty means stderr

}  // namespace

void setLogLevel(LogLevel level) {
  g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel logLevel() {
  return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

void setLogSink(LogSink sink) {
  std::lock_guard lock(g_sinkMutex);
  g_sink = std::move(sink);
}

std::string_view logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    case LogLevel::Off: return "off";
  }
  return "?";
}

void logMessage(LogLevel level, std::string_view message) {
  if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(g_sinkMutex);
  if (g_sink) {
    g_sink(level, message);
    return;
  }
  const std::string_view name = logLevelName(level);
  std::fprintf(stderr, "[%.*s] %.*s\n", static_cast<int>(name.size()), name.data(),
               static_cast<int>(message.size()), message.data());
}

}  // namespace tachyon::core
