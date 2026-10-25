// Symbol file location (spec 3.6). WP-20 only declares the interface; WP-21 implements local paths, the
// symstore cache and HTTP servers behind it.
#pragma once

#include "core/result.h"
#include "symbols/symbol_provider.h"

#include <filesystem>
#include <string>
#include <vector>

namespace tachyon::symbols {

class SymbolLocator {
 public:
  SymbolLocator() = default;
  SymbolLocator(std::vector<std::string> servers, std::filesystem::path cacheDir)
      : m_servers(std::move(servers)), m_cacheDir(std::move(cacheDir)) {}

  // Thread-safe. Finds the symbol file for `module`. WP-20 stub: always returns NotFound.
  Result<std::filesystem::path> locate(const Module& module);

 private:
  std::vector<std::string> m_servers;
  std::filesystem::path m_cacheDir;
};

}  // namespace tachyon::symbols
