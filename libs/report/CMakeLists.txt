# WP-62: CI report tool core. tachyon-report (apps/report) is a thin main() over report::run.
tachyon_add_library(report
  SOURCES analyze.cpp baseline.cpp markdown.cpp report_builder.cpp report_cli.cpp report_json.cpp
  PUBLIC_DEPS tachyon::core tachyon::model tachyon::analysis tachyon::symbols nlohmann_json::nlohmann_json)
