#include "inst/sidecar_paths.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>
#include <utility>

namespace tachyon::inst {

namespace fs = std::filesystem;

namespace {

// Position of the extension dot in a file name, or npos. Same rule as the SDK: the last '.' of the name.
size_t extensionDot(const std::string& name) {
  return name.find_last_of('.');
}

}  // namespace

fs::path numberedSidecarPath(const fs::path& base, uint32_t n) {
  std::string name = base.filename().string();
  const std::string tag = "." + std::to_string(n);
  const size_t dot = extensionDot(name);
  if (dot != std::string::npos) {
    name.insert(dot, tag);
  } else {
    name += tag;
  }
  fs::path out = base;
  out.replace_filename(name);
  return out;
}

std::vector<fs::path> expandSidecarSiblings(const fs::path& base) {
  std::vector<fs::path> result{base};
  const std::string name = base.filename().string();
  const size_t dot = extensionDot(name);
  const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
  const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
  const std::string prefix = stem + ".";
  fs::path dir = base.parent_path();
  if (dir.empty()) dir = ".";

  std::vector<std::pair<uint64_t, fs::path>> found;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string candidate = it->path().filename().string();
    if (candidate.size() <= prefix.size() + ext.size()) continue;
    if (candidate.compare(0, prefix.size(), prefix) != 0) continue;
    if (!ext.empty() && candidate.compare(candidate.size() - ext.size(), ext.size(), ext) != 0) continue;
    const std::string digits = candidate.substr(prefix.size(), candidate.size() - prefix.size() - ext.size());
    if (digits.empty() || digits.size() > 9 || !std::all_of(digits.begin(), digits.end(), [](char c) {
          return std::isdigit(static_cast<unsigned char>(c)) != 0;
        }))
      continue;
    const uint64_t n = std::stoull(digits);
    if (n == 0) continue;
    std::error_code fileEc;
    if (!it->is_regular_file(fileEc)) continue;
    found.emplace_back(n, it->path());
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& entry : found)
    result.push_back(base.has_parent_path() ? base.parent_path() / entry.second.filename()
                                            : entry.second.filename());
  return result;
}

}  // namespace tachyon::inst
