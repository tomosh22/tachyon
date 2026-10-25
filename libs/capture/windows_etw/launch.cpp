#include "capture/windows_etw/launch.h"

#include "core/platform.h"

#include <algorithm>

namespace tachyon::capture::etw {

std::string quoteArgument(std::string_view arg) {
  const bool needsQuotes = arg.empty() || arg.find_first_of(" \t\n\v\"") != std::string_view::npos;
  if (!needsQuotes) return std::string(arg);
  std::string out = "\"";
  size_t backslashes = 0;
  for (const char c : arg) {
    if (c == '\\') {
      ++backslashes;
      continue;
    }
    if (c == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      out.push_back(c);
    }
    backslashes = 0;
  }
  out.append(backslashes * 2, '\\');  // trailing backslashes must not escape the closing quote
  out.push_back('"');
  return out;
}

std::string buildCommandLine(std::string_view exe, const std::vector<std::string>& args) {
  std::string line = quoteArgument(exe);
  for (const std::string& a : args) {
    line.push_back(' ');
    line += quoteArgument(a);
  }
  return line;
}

std::vector<std::string> splitCommandLine(std::string_view s) {
  std::vector<std::string> out;
  size_t i = 0;
  const size_t n = s.size();
  for (;;) {
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= n) break;
    std::string arg;
    bool inQuotes = false;
    while (i < n) {
      size_t backslashes = 0;
      while (i < n && s[i] == '\\') {
        ++backslashes;
        ++i;
      }
      if (i < n && s[i] == '"') {
        arg.append(backslashes / 2, '\\');
        if (backslashes % 2 == 1) {
          arg.push_back('"');  // \" is a literal quote
        } else if (inQuotes && i + 1 < n && s[i + 1] == '"') {
          arg.push_back('"');  // "" inside quotes is a literal quote
          ++i;
        } else {
          inQuotes = !inQuotes;
        }
        ++i;
        continue;
      }
      arg.append(backslashes, '\\');
      if (i >= n) break;
      if (!inQuotes && (s[i] == ' ' || s[i] == '\t')) break;
      arg.push_back(s[i++]);
    }
    out.push_back(std::move(arg));
  }
  return out;
}

namespace {

std::string upperAscii(const std::string& s) {
  std::string u = s;
  for (char& c : u)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return u;
}

}  // namespace

std::vector<EnvVar> mergeEnvironment(const std::vector<EnvVar>& parent,
                                     const std::map<std::string, std::string>& overrides,
                                     bool caseInsensitive) {
  auto key = [&](const std::string& name) { return caseInsensitive ? upperAscii(name) : name; };
  std::vector<EnvVar> merged;
  merged.reserve(parent.size() + overrides.size());
  for (const EnvVar& v : parent)
    if (!v.first.empty()) merged.push_back(v);
  for (const auto& [name, value] : overrides) {
    if (name.empty()) continue;
    const std::string k = key(name);
    std::erase_if(merged, [&](const EnvVar& v) { return key(v.first) == k; });
    merged.emplace_back(name, value);
  }
  std::stable_sort(merged.begin(), merged.end(),
                   [&](const EnvVar& a, const EnvVar& b) { return key(a.first) < key(b.first); });
  return merged;
}

#if !TACHYON_PLATFORM_WINDOWS

// Non-Windows: the type exists so the platform-independent code compiles; nothing can be launched.
struct TargetProcess::Impl {};

TargetProcess::TargetProcess(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
TargetProcess::~TargetProcess() = default;
TargetProcess::TargetProcess(TargetProcess&&) noexcept = default;
TargetProcess& TargetProcess::operator=(TargetProcess&&) noexcept = default;

Result<TargetProcess> TargetProcess::launchSuspended(const LaunchSpec&) {
  return makeError(ErrorCode::Unsupported, "launching a target needs Windows");
}
Result<TargetProcess> TargetProcess::open(uint32_t) {
  return makeError(ErrorCode::Unsupported, "watching a target needs Windows");
}
uint32_t TargetProcess::pid() const {
  return 0;
}
Result<void> TargetProcess::resume() {
  return makeError(ErrorCode::Unsupported, "launching a target needs Windows");
}
bool TargetProcess::exited() const {
  return true;
}
bool TargetProcess::allExited() const {
  return true;
}
bool TargetProcess::tracksDescendants() const {
  return false;
}
void TargetProcess::terminate() {}

#endif

}  // namespace tachyon::capture::etw
