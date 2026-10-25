// tracegen: deterministic synthetic .tachyon trace generator (WP-04). See README.md.
#include "generator.h"
#include "manifest.h"
#include "scenario.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void usage() {
  std::fputs(
      "usage: tracegen [--out-dir DIR] [--no-verify] [--manifest FILE] <scenario.json>...\n"
      "  Generates <name>.tachyon, <name>.expected.json and <name>.symbols.json for each scenario.\n"
      "  --out-dir DIR     output directory (default: current directory)\n"
      "  --no-verify       skip the brute-force reference cross-check\n"
      "  --manifest FILE   write the SHA-256 manifest of the generated files to FILE\n",
      stderr);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace tachyon::tracegen;
  std::filesystem::path outDir = ".";
  std::filesystem::path manifestPath;
  GenerateOptions options;
  std::vector<std::string> scenarios;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--out-dir" && i + 1 < argc) {
      outDir = argv[++i];
    } else if (a == "--manifest" && i + 1 < argc) {
      manifestPath = argv[++i];
    } else if (a == "--no-verify") {
      options.verify = false;
    } else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else if (!a.empty() && a[0] == '-') {
      usage();
      return 2;
    } else {
      scenarios.push_back(a);
    }
  }
  if (scenarios.empty()) {
    usage();
    return 2;
  }

  nlohmann::json manifest = nlohmann::json::object();
  for (const std::string& path : scenarios) {
    auto scenario = loadScenario(path);
    if (!scenario.ok()) {
      std::fprintf(stderr, "tracegen: %s\n", scenario.error().toString().c_str());
      return 1;
    }
    auto report = generateFixture(*scenario, outDir, options);
    if (!report.ok()) {
      std::fprintf(stderr, "tracegen: %s\n", report.error().toString().c_str());
      return 1;
    }
    std::printf("%-8s samples=%llu size=%.2f MiB generate=%.2fs verify=%.2fs\n", scenario->name.c_str(),
                static_cast<unsigned long long>(report->samples),
                static_cast<double>(report->tachyonBytes) / (1024.0 * 1024.0), report->generateSeconds,
                report->verifySeconds);
    if (!manifestPath.empty()) {
      auto entry = manifestEntry(*report);
      if (!entry.ok()) {
        std::fprintf(stderr, "tracegen: %s\n", entry.error().toString().c_str());
        return 1;
      }
      manifest[scenario->name] = *entry;
    }
  }
  if (!manifestPath.empty()) {
    const nlohmann::json doc{{"schema", "tachyon-fixture-manifest-1"}, {"scenarios", manifest}};
    auto r = writeJsonFile(manifestPath, doc, 2);
    if (!r.ok()) {
      std::fprintf(stderr, "tracegen: %s\n", r.error().toString().c_str());
      return 1;
    }
  }
  return 0;
}
