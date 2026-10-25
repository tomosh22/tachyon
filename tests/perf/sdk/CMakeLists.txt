tachyon_add_perf_test(sdk_perf_tests SOURCES sdk_overhead_perf_test.cpp DEPS tachyon::sdk tachyon::control tachyon::core)
if(TARGET sdk_perf_tests)
  target_include_directories(sdk_perf_tests PRIVATE ${PROJECT_SOURCE_DIR}/sdk/src ${PROJECT_SOURCE_DIR}/tests/unit/sdk)
endif()
