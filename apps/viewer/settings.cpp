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

void readString(const json& j, const char* key, std::string& out) {
  const auto it = j.find(key);
  if (it == j.end()) return;
  if (it->is_string()) {
    out = it->get<std::string>();
  } else {
    TACHYON_LOG_WARN("settings: 'capture.{}' is not a string, using default", key);
  }
}

void readBool(const json& j, const char* key, bool& out) {
  const auto it = j.find(key);
  if (it == j.end()) return;
  if (it->is_boolean()) {
    out = it->get<bool>();
  } else {
    TACHYON_LOG_WARN("settings: 'capture.{}' is not a boolean, using default", key);
  }
}

void readUint(const json& j, const char* key, uint32_t& out, uint32_t maxValue) {
  const auto it = j.find(key);
  if (it == j.end()) return;
  if (it->is_number() && std::isfinite(it->get<double>()) && it->get<double>() >= 0.0) {
    out = static_cast<uint32_t>(std::min(it->get<double>(), static_cast<double>(maxValue)));
  } else {
    TACHYON_LOG_WARN("settings: 'capture.{}' is not a non-negative number, using default", key);
  }
}

CaptureSettings readCaptureSettings(const json& j) {
  CaptureSettings c;
  if (!j.is_object()) {
    TACHYON_LOG_WARN("settings: 'capture' is not an object, using defaults");
    return c;
  }
  readString(j, "capturesDir", c.capturesDir);
  readString(j, "agentAddress", c.agentAddress);
  readString(j, "remoteAddress", c.remoteAddress);
  readString(j, "remoteTokenFile", c.remoteTokenFile);
  readString(j, "agentExe", c.agentExe);
  readString(j, "agentBackend", c.agentBackend);
  c.agentArgs = readStringArray(j, "agentArgs", c.agentArgs);
  readBool(j, "autoConnect", c.autoConnect);
  readBool(j, "autoOpen", c.autoOpen);
  readString(j, "exe", c.exe);
  readString(j, "args", c.args);
  readString(j, "cwd", c.cwd);
  c.env = readStringArray(j, "env", c.env);
  readBool(j, "startImmediately", c.startImmediately);
  readUint(j, "samplingHz", c.samplingHz, 1'000'000);
  readUint(j, "circularMb", c.circularMb, 1'000'000);
  readBool(j, "followChildren", c.followChildren);
  readString(j, "childFilter", c.childFilter);
  readBool(j, "kernelStacks", c.kernelStacks);
  return c;
}

json captureSettingsToJson(const CaptureSettings& c) {
  json j;
  j["capturesDir"] = c.capturesDir;
  j["agentAddress"] = c.agentAddress;
  j["remoteAddress"] = c.remoteAddress;
  j["remoteTokenFile"] = c.remoteTokenFile;
  j["agentExe"] = c.agentExe;
  j["agentBackend"] = c.agentBackend;
  j["agentArgs"] = c.agentArgs;
  j["autoConnect"] = c.autoConnect;
  j["autoOpen"] = c.autoOpen;
  j["exe"] = c.exe;
  j["args"] = c.args;
  j["cwd"] = c.cwd;
  j["env"] = c.env;
  j["startImmediately"] = c.startImmediately;
  j["samplingHz"] = c.samplingHz;
  j["circularMb"] = c.circularMb;
  j["followChildren"] = c.followChildren;
  j["childFilter"] = c.childFilter;
  j["kernelStacks"] = c.kernelStacks;
  return j;
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
  s.symbolSearchPaths = readStringArray(j, "symbolSearchPaths", s.symbolSearchPaths);
  s.sourceRoots = readStringArray(j, "sourceRoots", s.sourceRoots);
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
  if (const auto it = j.find("blendScopes"); it != j.end()) {
    if (it->is_boolean()) {
      s.blendScopes = it->get<bool>();
    } else {
      TACHYON_LOG_WARN("settings: 'blendScopes' is not a boolean, using default");
    }
  }
  if (const auto it = j.find("frameBudgetMs"); it != j.end()) {
    if (it->is_number() && std::isfinite(it->get<double>()) && it->get<double>() > 0.0) {
      s.frameBudgetMs = std::clamp(it->get<double>(), 0.1, 10000.0);
    } else {
      TACHYON_LOG_WARN("settings: 'frameBudgetMs' is not a positive number, using default");
    }
  }
  if (const auto it = j.find("capture"); it != j.end()) s.capture = readCaptureSettings(*it);
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
  j["symbolSearchPaths"] = symbolSearchPaths;
  j["cacheDir"] = cacheDir;
  json mappings = json::array();
  for (const SourcePathMapping& m : sourcePathMappings) mappings.push_back({{"from", m.from}, {"to", m.to}});
  j["sourcePathMappings"] = mappings;
  j["sourceRoots"] = sourceRoots;
  j["theme"] = themeName(theme);
  j["uiScale"] = uiScale;
  j["recentFiles"] = recentFiles;
  j["blendScopes"] = blendScopes;
  j["frameBudgetMs"] = frameBudgetMs;
  j["capture"] = captureSettingsToJson(capture);

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

bool Settings::addSymbolSearchPath(const std::string& path) {
  if (path.empty() ||
      std::find(symbolSearchPaths.begin(), symbolSearchPaths.end(), path) != symbolSearchPaths.end())
    return false;
  symbolSearchPaths.push_back(path);
  return true;
}

bool Settings::addSourcePathMapping(const SourcePathMapping& mapping) {
  if (mapping.from.empty() || mapping.to.empty() ||
      std::find(sourcePathMappings.begin(), sourcePathMappings.end(), mapping) != sourcePathMappings.end())
    return false;
  sourcePathMappings.push_back(mapping);
  return true;
}

bool Settings::addSourceRoot(const std::string& root) {
  if (root.empty() || std::find(sourceRoots.begin(), sourceRoots.end(), root) != sourceRoots.end())
    return false;
  sourceRoots.push_back(root);
  return true;
}

void Settings::addRecentFile(const std::string& path) {
  std::erase(recentFiles, path);
  recentFiles.insert(recentFiles.begin(), path);
  if (recentFiles.size() > kMaxRecentFiles) recentFiles.resize(kMaxRecentFiles);
}

}  // namespace tachyon::viewer
