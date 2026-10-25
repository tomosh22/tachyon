tachyon_add_test(analysis_tests
  SOURCES aggregation_test.cpp return_address_test.cpp aggregation_fixture_test.cpp attribution_test.cpp lod_test.cpp lod_fixture_test.cpp
          thread_states_test.cpp thread_states_fixture_test.cpp wait_reasons_test.cpp
          query_cache_test.cpp query_executor_test.cpp filter_test.cpp cancellation_test.cpp alloc_counter.cpp
          stats_test.cpp scope_stats_test.cpp frames_test.cpp instances_test.cpp
  DEPS tachyon::analysis tachyon::symbols tachyon::model tachyon::trace nlohmann_json::nlohmann_json)
