if(NOT TACHYON_BUILD_VIEWER)
  return()
endif()

tachyon_add_perf_test(viewer_perf_tests SOURCES timeline_perf_test.cpp interaction_perf_test.cpp DEPS tachyon::viewer_core tachyon::analysis)
