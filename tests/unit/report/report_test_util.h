// Helpers shared by the WP-62 tests: fixture loading, a temp directory, and a small JSON Schema validator
// (the subset used by apps/report/report.schema.json).
#pragma once

#include "analysis/query.h"
#include "analysis/thread_states.h"
#include "report/report_builder.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace tachyon::report::testutil {

inline std::filesystem::path generated(const std::string& file) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / file;
}

// A fresh empty directory under the build tree, removed and recreated per call.
inline std::filesystem::path tempDir(const std::string& name) {
  const std::filesystem::path dir = std::filesystem::path(TACHYON_BINARY_DIR) / "report_test_tmp" / name;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir);
  return dir;
}

inline std::string readText(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

inline void writeText(const std::filesystem::path& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << s;
}

inline nlohmann::json readJson(const std::filesystem::path& p) {
  return nlohmann::json::parse(readText(p), nullptr, false);
}

// Fixture trace with its symbols, ready for queries.
struct Loaded {
  explicit Loaded(const std::string& name) : svc(std::vector<std::string>{}, std::filesystem::path{}) {
    auto r = model::TraceModel::load(generated(name + ".tachyon"));
    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
    if (r.ok()) m = std::move(*r);
    EXPECT_TRUE(analysis::buildThreadStates(m).ok());
    EXPECT_TRUE(svc.addSymbolFileOverride(generated(name + ".symbols.json")).ok());
    svc.resolveAllAsync(m);
    svc.waitAll();
    expected = readJson(generated(name + ".expected.json"));
  }
  model::TraceModel m;
  symbols::SymbolService svc;
  nlohmann::json expected;
};

// ---- JSON Schema subset validator -------------------------------------------------------------------------
// Supports: type (string or array), enum, const, required, properties, additionalProperties:false, items,
// maxItems, minimum, maximum, exclusiveMinimum, pattern, oneOf and local $ref ("#/definitions/..."). Returns
// error messages (empty = valid).
class SchemaValidator {
 public:
  explicit SchemaValidator(nlohmann::json schema) : m_root(std::move(schema)) {}

  std::vector<std::string> validate(const nlohmann::json& doc) const {
    std::vector<std::string> errors;
    check(m_root, doc, "$", errors);
    return errors;
  }

 private:
  static bool typeMatches(const std::string& t, const nlohmann::json& v) {
    if (t == "object") return v.is_object();
    if (t == "array") return v.is_array();
    if (t == "string") return v.is_string();
    if (t == "boolean") return v.is_boolean();
    if (t == "null") return v.is_null();
    if (t == "number") return v.is_number();
    if (t == "integer")
      return v.is_number_integer() || (v.is_number_float() && v.get<double>() == std::floor(v.get<double>()));
    return false;
  }

  const nlohmann::json& resolve(const std::string& ref) const {
    // "#/definitions/name"
    const std::string prefix = "#/definitions/";
    EXPECT_EQ(ref.rfind(prefix, 0), 0u) << ref;
    return m_root.at("definitions").at(ref.substr(prefix.size()));
  }

  void check(const nlohmann::json& s, const nlohmann::json& v, const std::string& path,
             std::vector<std::string>& errors) const {
    if (s.contains("$ref")) {
      check(resolve(s["$ref"].get<std::string>()), v, path, errors);
      return;
    }
    if (s.contains("const") && v != s["const"]) errors.push_back(path + ": const mismatch");
    if (s.contains("enum")) {
      bool found = false;
      for (const auto& e : s["enum"]) found = found || e == v;
      if (!found) errors.push_back(path + ": not in enum");
    }
    if (s.contains("type")) {
      bool ok = false;
      if (s["type"].is_string()) {
        ok = typeMatches(s["type"].get<std::string>(), v);
      } else {
        for (const auto& t : s["type"]) ok = ok || typeMatches(t.get<std::string>(), v);
      }
      if (!ok) {
        errors.push_back(path + ": wrong type");
        return;
      }
    }
    if (s.contains("oneOf")) {
      int matches = 0;
      for (const auto& alt : s["oneOf"]) {
        std::vector<std::string> sub;
        check(alt, v, path, sub);
        if (sub.empty()) ++matches;
      }
      if (matches != 1) errors.push_back(path + ": oneOf matched " + std::to_string(matches));
    }
    if (v.is_number()) {
      const double d = v.get<double>();
      if (s.contains("minimum") && d < s["minimum"].get<double>()) errors.push_back(path + ": below minimum");
      if (s.contains("maximum") && d > s["maximum"].get<double>()) errors.push_back(path + ": above maximum");
      if (s.contains("exclusiveMinimum") && d <= s["exclusiveMinimum"].get<double>())
        errors.push_back(path + ": not above exclusiveMinimum");
    }
    if (v.is_string() && s.contains("pattern")) {
      if (!std::regex_search(v.get<std::string>(), std::regex(s["pattern"].get<std::string>())))
        errors.push_back(path + ": pattern mismatch");
    }
    if (v.is_array()) {
      if (s.contains("maxItems") && v.size() > s["maxItems"].get<size_t>())
        errors.push_back(path + ": too long");
      if (s.contains("items"))
        for (size_t i = 0; i < v.size(); ++i)
          check(s["items"], v[i], path + "[" + std::to_string(i) + "]", errors);
    }
    if (v.is_object()) {
      if (s.contains("required"))
        for (const auto& r : s["required"])
          if (!v.contains(r.get<std::string>())) errors.push_back(path + ": missing " + r.get<std::string>());
      const nlohmann::json empty = nlohmann::json::object();
      const nlohmann::json& props = s.contains("properties") ? s["properties"] : empty;
      for (const auto& [k, child] : v.items()) {
        if (props.contains(k))
          check(props[k], child, path + "." + k, errors);
        else if (s.contains("additionalProperties") && s["additionalProperties"] == false)
          errors.push_back(path + ": unexpected key " + k);
      }
    }
  }

  nlohmann::json m_root;
};

inline SchemaValidator reportSchema() {
  return SchemaValidator(readJson(TACHYON_REPORT_SCHEMA));
}

}  // namespace tachyon::report::testutil
