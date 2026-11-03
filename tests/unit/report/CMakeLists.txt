# WP-62: tachyon-report. The CLI tests drive report::run in-process and, for the exit codes, the real
# executable; the lock tests reuse the HandModel helpers of the analysis tests.
tachyon_add_test(report_tests
  SOURCES baseline_test.cpp markdown_test.cpp report_builder_test.cpp report_cli_test.cpp report_json_test.cpp
  DEPS tachyon::report tachyon::analysis tachyon::symbols tachyon::model tachyon::trace
       nlohmann_json::nlohmann_json)

if(TARGET report_tests)
  target_include_directories(report_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/analysis)
  add_dependencies(report_tests tachyon-report tracegen)
  target_compile_definitions(report_tests PRIVATE
    TACHYON_REPORT_SCHEMA="${PROJECT_SOURCE_DIR}/apps/report/report.schema.json"
    TACHYON_REPORT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
    TACHYON_REPORT_EXE="$<TARGET_FILE:tachyon-report>"
    TACHYON_TRACEGEN_EXE="$<TARGET_FILE:tracegen>")
endif()
