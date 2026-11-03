// Shared helpers of the WP-63 exporter tests.
#pragma once

#include "analysis/query.h"
#include "analysis/thread_states.h"
#include "export/export.h"
#include "json_schema_validator.h"
#include "model/model.h"
#include "symbols/symbol_service.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace tachyon::exporttest {

inline std::filesystem::path generatedFixture(const std::string& file) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / file;
}

// A tracegen fixture loaded with thread states and its symbols resolved.
struct LoadedFixture {
  explicit LoadedFixture(const std::string& name, bool withSymbols = true)
      : svc(std::make_unique<symbols::SymbolService>(std::vector<std::string>{}, std::filesystem::path{})) {
    auto r = model::TraceModel::load(generatedFixture(name + ".tachyon"));
    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
    if (r.ok()) m = std::move(*r);
    EXPECT_TRUE(analysis::buildThreadStates(m).ok());
    if (withSymbols) EXPECT_TRUE(svc->addSymbolFileOverride(generatedFixture(name + ".symbols.json")).ok());
    svc->resolveAllAsync(m);
    svc->waitAll();
    ctx = std::make_unique<analysis::QueryContext>(m, *svc);
  }
  model::TraceModel m;
  std::unique_ptr<symbols::SymbolService> svc;
  std::unique_ptr<analysis::QueryContext> ctx;
};

inline std::string schemaPath() {
  return std::string(TACHYON_FIXTURE_DIR) + "/export/speedscope-file-format-schema.json";
}

inline SchemaValidator loadSpeedscopeSchema() {
  std::ifstream f(schemaPath());
  EXPECT_TRUE(f.good()) << schemaPath();
  return SchemaValidator(nlohmann::json::parse(f));
}

// Runs the speedscope exporter into a string; the result must be ok.
inline std::string speedscopeText(const analysis::QueryContext& ctx, const analysis::Filter& filter = {},
                                  exporters::ExportStats* stats = nullptr) {
  std::ostringstream os;
  auto r = exporters::exportSpeedscope(ctx, filter, os);
  EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
  if (r.ok() && stats != nullptr) *stats = *r;
  return os.str();
}

inline std::string chromeText(const model::TraceModel& m, const symbols::SymbolService& svc,
                              const analysis::Filter& filter = {}, exporters::ExportStats* stats = nullptr) {
  std::ostringstream os;
  auto r = exporters::exportChromeTrace(m, svc, filter, os);
  EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
  if (r.ok() && stats != nullptr) *stats = *r;
  return os.str();
}

inline nlohmann::json parseOrFail(const std::string& text) {
  auto j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  EXPECT_FALSE(j.is_discarded()) << "output is not valid JSON";
  return j;
}

}  // namespace tachyon::exporttest
