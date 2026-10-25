#include "viewer/settings.h"

#include "core/log.h"
#include "viewer/paths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace tachyon::viewer {

using json = nlohmann::json;

const char* themeName(ThemeKind theme) {
  return theme == ThemeKind::Light ? "light" : "dark";
}

namespace {

std::vector<std::string> readStringArray(const json& j, const char* key, std::vector<std::string> fallback) {
  const auto it = j.find(key);
  if (it == j.end()) return fallback;
  if (!it->is_array()) {
    TACHYON_LOG_WARN("settings: '{}' is not an array, using default", key);
    return fallback;
  }
  std::vector<std::string> out;
  for (const json& e : *it) {
    if (e.is_string()) out.push_back(e.get<std::string>());
  }
  return out;
}

}  // namespace

Settings Settings::load(const std::filesystem::path& path) {
  Settings s;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return s;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    TACHYON_LOG_WARN("settings: cannot read {}, using defaults", pathToUtf8(path));
    return s;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  const json j = json::parse(buffer.str(), nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    TACHYON_LOG_WARN("settings: {} is not valid JSON, using defaults", pathToUtf8(path));
    return s;
  }

  s.symbolServers = readStringArray(j, "symbolServers", s.symbolServers);
  s.recentFiles = readStringArray(j, "recentFiles", s.recentFiles);
  if (s.recentFiles.size() > kMaxRecentFiles) s.recentFiles.resize(kMaxRecentFiles);

  if (const auto it = j.find("cacheDir"); it != j.end()) {
    if (it->is_string()) {
      s.cacheDir = it->get<std::string>();
    } else {
      TACHYON_LOG_WARN("settings: 'cacheDir' is not a string, using default");
    }
  }
  if (const auto it = j.find("theme"); it != j.end()) {
    if (it->is_string() && it->get<std::string>() == "light") {
      s.theme = ThemeKind::Light;
    } else if (!it->is_string() || it->get<std::string>() != "dark") {
      TACHYON_LOG_WARN("settings: unknown theme, using dark");
    }
  }
  if (const auto it = j.find("uiScale"); it != j.end()) {
    if (it->is_number()) {
      const double v = it->get<double>();
      if (std::isfinite(v)) {
        s.uiScale = static_cast<float>(
            std::clamp(v, static_cast<double>(kMinUiScale), static_cast<double>(kMaxUiScale)));
      }
    } else {
      TACHYON_LOG_WARN("settings: 'uiScale' is not a number, using default");
    }
  }
  if (const auto it = j.find("sourcePathMappings"); it != j.end()) {
    if (it->is_array()) {
      s.sourcePathMappings.clear();
      for (const json& e : *it) {
        if (!e.is_object()) continue;
        const auto from = e.find("from");
        const auto to = e.find("to");
        if (from != e.end() && to != e.end() && from->is_string() && to->is_string()) {
          s.sourcePathMappings.push_back({from->get<std::string>(), to->get<std::string>()});
        }
      }
    } else {
      TACHYON_LOG_WARN("settings: 'sourcePathMappings' is not an array, using default");
    }
  }
  return s;
}

Result<void> Settings::save(const std::filesystem::path& path) const {
  json j;
  j["symbolServers"] = symbolServers;
  j["cacheDir"] = cacheDir;
  json mappings = json::array();
  for (const SourcePathMapping& m : sourcePathMappings) mappings.push_back({{"from", m.from}, {"to", m.to}});
  j["sourcePathMappings"] = mappings;
  j["theme"] = themeName(theme);
  j["uiScale"] = uiScale;
  j["recentFiles"] = recentFiles;

  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return makeError(ErrorCode::IoError, "cannot create " + pathToUtf8(path.parent_path()));
  }
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return makeError(ErrorCode::IoError, "cannot write " + pathToUtf8(tmp));
    out << j.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
    out.flush();
    if (!out) return makeError(ErrorCode::IoError, "write failed for " + pathToUtf8(tmp));
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return makeError(ErrorCode::IoError, "cannot replace " + pathToUtf8(path));
  }
  return ok();
}

void Settings::addRecentFile(const std::string& path) {
  std::erase(recentFiles, path);
  recentFiles.insert(recentFiles.begin(), path);
  if (recentFiles.size() > kMaxRecentFiles) recentFiles.resize(kMaxRecentFiles);
}

}  // namespace tachyon::viewer
