# WP-63: exporter tests. The hand-model helpers are shared with the analysis tests.
tachyon_add_test(export_tests
  SOURCES json_writer_test.cpp speedscope_test.cpp chrome_trace_test.cpp
  DEPS tachyon::export tachyon::analysis tachyon::symbols tachyon::model tachyon::trace nlohmann_json::nlohmann_json)
if(TARGET export_tests)
  target_include_directories(export_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/analysis)
endif()
