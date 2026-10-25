tachyon_add_test(analysis_tests
  SOURCES thread_states_test.cpp thread_states_fixture_test.cpp wait_reasons_test.cpp
  DEPS tachyon::analysis tachyon::model tachyon::trace nlohmann_json::nlohmann_json)
