# WP-70 perf budget: perf spool conversion (Linux only).
if(TARGET tachyon::capture_linux_perf)
  tachyon_add_perf_test(linux_perf_perf_tests
    SOURCES linux_perf_convert_perf_test.cpp
    DEPS tachyon::capture_linux_perf tachyon::trace)
endif()
