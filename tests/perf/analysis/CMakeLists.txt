tachyon_add_perf_test(analysis_perf_tests SOURCES aggregation_perf_test.cpp lod_perf_test.cpp thread_states_perf_test.cpp query_cache_perf_test.cpp stats_perf_test.cpp scope_blend_perf_test.cpp
  DEPS tachyon::analysis tachyon::symbols tachyon::model)
# The WP-53 perf test reuses the hand-model helpers of the unit tests.
target_include_directories(analysis_perf_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/analysis)
