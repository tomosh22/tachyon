// Shared helpers for the symbols tests: fixture loading and a brute-force reference over symbols.json.
#pragma once

#include "model/model.h"
#include "symbols/symbol_service.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::symbols::testutil {

inline std::filesystem::path fixtureFile(const std::string& name, const char* suffix) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / (name + suffix);
}

inline nlohmann::json readJson(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  EXPECT_TRUE(in.good()) << p;
  return nlohmann::json::parse(in);
}

inline model::TraceModel loadModel(const std::string& name) {
  auto r = model::TraceModel::load(fixtureFile(name, ".tachyon"));
  EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
  return r.ok() ? std::move(r).value() : model::TraceModel{};
}

// What a brute-force linear scan of symbols.json says about (module, rva).
struct Reference {
  bool resolved = false;
  std::string function;
  uint64_t startRva = 0;
  std::string file;
  uint32_t line = 0;
  std::vector<std::string> inlineFunctions;  // innermost first
};

class ReferenceSymbols {
 public:
  explicit ReferenceSymbols(const std::filesystem::path& symbolsJson) : m_doc(readJson(symbolsJson)) {}

  Reference lookup(uint32_t moduleIdx, uint64_t rva) const {
    Reference out;
    for (const auto& mod : m_doc["modules"]) {
      if (mod["module_idx"].get<uint32_t>() != moduleIdx) continue;
      for (const auto& fn : mod["functions"]) {
        const uint64_t start = fn["start_rva"].get<uint64_t>();
        if (rva < start || rva >= start + fn["size"].get<uint64_t>()) continue;
        out.resolved = true;
        out.function = fn["name"].get<std::string>();
        out.startRva = start;
        out.file = fn["file"].get<std::string>();
        out.line = fn["line"].get<uint32_t>();
        for (const auto& l : fn["lines"]) {
          const uint64_t ls = l["rva"].get<uint64_t>();
          if (rva >= ls && rva < ls + l["size"].get<uint64_t>()) out.line = l["line"].get<uint32_t>();
        }
        for (const auto& in : fn["inlines"]) {
          const uint64_t is = in["rva"].get<uint64_t>();
          if (rva >= is && rva < is + in["size"].get<uint64_t>()) {
            for (const auto& c : in["chain"]) out.inlineFunctions.push_back(c["function"].get<std::string>());
          }
        }
        return out;
      }
    }
    return out;
  }

 private:
  nlohmann::json m_doc;
};

}  // namespace tachyon::symbols::testutil
