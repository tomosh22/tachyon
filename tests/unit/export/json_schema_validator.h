// A small JSON Schema (draft-07) validator over nlohmann::json, enough for the vendored speedscope schema
// (tests/fixtures/export/speedscope-file-format-schema.json): type, const, enum, required, properties,
// items (single schema), anyOf, local "#/definitions/..." $ref. Unknown keywords are ignored; properties
// not listed are allowed (the schema has no additionalProperties).
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace tachyon::exporttest {

class SchemaValidator {
 public:
  explicit SchemaValidator(nlohmann::json schema) : m_root(std::move(schema)) {}

  // Returns an empty string when `doc` is valid, otherwise a description of the first violation.
  std::string validate(const nlohmann::json& doc) const { return check(m_root, doc, "$"); }

 private:
  const nlohmann::json& resolve(const nlohmann::json& schema) const {
    const nlohmann::json* cur = &schema;
    while (cur->is_object() && cur->contains("$ref")) {
      const std::string ref = (*cur)["$ref"].get<std::string>();
      if (ref.rfind("#/", 0) != 0) return *cur;
      const nlohmann::json* target = &m_root;
      size_t pos = 2;
      while (pos <= ref.size()) {
        size_t slash = ref.find('/', pos);
        if (slash == std::string::npos) slash = ref.size();
        target = &target->at(ref.substr(pos, slash - pos));
        pos = slash + 1;
      }
      cur = target;
    }
    return *cur;
  }

  static bool typeMatches(const std::string& type, const nlohmann::json& v) {
    if (type == "object") return v.is_object();
    if (type == "array") return v.is_array();
    if (type == "string") return v.is_string();
    if (type == "number") return v.is_number();
    if (type == "integer") return v.is_number_integer();
    if (type == "boolean") return v.is_boolean();
    if (type == "null") return v.is_null();
    return true;
  }

  std::string check(const nlohmann::json& rawSchema, const nlohmann::json& v, const std::string& path) const {
    const nlohmann::json& schema = resolve(rawSchema);
    if (!schema.is_object()) return {};
    if (schema.contains("type") && !typeMatches(schema["type"].get<std::string>(), v))
      return path + ": expected type " + schema["type"].get<std::string>();
    if (schema.contains("const") && v != schema["const"]) return path + ": const mismatch";
    if (schema.contains("enum")) {
      bool found = false;
      for (const auto& e : schema["enum"]) found = found || e == v;
      if (!found) return path + ": not in enum";
    }
    if (schema.contains("anyOf")) {
      std::string first;
      bool any = false;
      for (const auto& alt : schema["anyOf"]) {
        const std::string r = check(alt, v, path);
        if (r.empty()) {
          any = true;
          break;
        }
        if (first.empty()) first = r;
      }
      if (!any) return path + ": no anyOf alternative matches (first: " + first + ")";
    }
    if (v.is_object()) {
      if (schema.contains("required")) {
        for (const auto& r : schema["required"])
          if (!v.contains(r.get<std::string>()))
            return path + ": missing required property " + r.get<std::string>();
      }
      if (schema.contains("properties")) {
        for (const auto& [key, sub] : schema["properties"].items()) {
          if (!v.contains(key)) continue;
          const std::string r = check(sub, v[key], path + "." + key);
          if (!r.empty()) return r;
        }
      }
    }
    if (v.is_array() && schema.contains("items")) {
      for (size_t i = 0; i < v.size(); ++i) {
        const std::string r = check(schema["items"], v[i], path + "[" + std::to_string(i) + "]");
        if (!r.empty()) return r;
      }
    }
    return {};
  }

  nlohmann::json m_root;
};

}  // namespace tachyon::exporttest
