// Process-wide logging. The only global mutable state permitted in libraries (Part 2.4).
#pragma once

#include <fmt/format.h>

#include <functional>
#include <string_view>

namespace tachyon::core {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Off };

using LogSink = std::function<void(LogLevel, std::string_view message)>;

// Thread-safe. Messages below `level` are discarded. Default Info.
void setLogLevel(LogLevel level);
// Thread-safe.
LogLevel logLevel();
// Thread-safe. Replaces the sink (default writes to stderr). Pass nullptr to restore the default.
void setLogSink(LogSink sink);
// Thread-safe. Emits an already formatted message.
void logMessage(LogLevel level, std::string_view message);
// Thread-safe: pure function.
std::string_view logLevelName(LogLevel level);

// Thread-safe. Formats with fmt and emits if the level is enabled.
template <typename... Args>
void log(LogLevel level, fmt::format_string<Args...> format, Args&&... args) {
  if (static_cast<int>(level) < static_cast<int>(logLevel())) return;
  logMessage(level, fmt::format(format, std::forward<Args>(args)...));
}

}  // namespace tachyon::core

#define TACHYON_LOG_TRACE(...) ::tachyon::core::log(::tachyon::core::LogLevel::Trace, __VA_ARGS__)
#define TACHYON_LOG_DEBUG(...) ::tachyon::core::log(::tachyon::core::LogLevel::Debug, __VA_ARGS__)
#define TACHYON_LOG_INFO(...) ::tachyon::core::log(::tachyon::core::LogLevel::Info, __VA_ARGS__)
#define TACHYON_LOG_WARN(...) ::tachyon::core::log(::tachyon::core::LogLevel::Warn, __VA_ARGS__)
#define TACHYON_LOG_ERROR(...) ::tachyon::core::log(::tachyon::core::LogLevel::Error, __VA_ARGS__)
