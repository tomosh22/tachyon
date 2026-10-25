tachyon_add_perf_test(analysis_perf_tests SOURCES aggregation_perf_test.cpp lod_perf_test.cpp thread_states_perf_test.cpp query_cache_perf_test.cpp
  DEPS tachyon::analysis tachyon::symbols tachyon::model)
