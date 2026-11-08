if(NOT TACHYON_BUILD_VIEWER)
  return()
endif()

tachyon_add_perf_test(viewer_perf_tests SOURCES timeline_perf_test.cpp interaction_perf_test.cpp callgraph_perf_test.cpp functions_perf_test.cpp stats_perf_test.cpp inst_perf_test.cpp source_perf_test.cpp live_perf_test.cpp ../../unit/agent/process_util.cpp DEPS tachyon::viewer_core tachyon::analysis tachyon::agent_core tachyon::control tachyon::capture)

# WP-65b: the live perf test replays a fixture through an in-process Agent (the helpers of the viewer unit tests).
if(TARGET viewer_perf_tests AND TARGET tachyon-agent)
  add_dependencies(viewer_perf_tests tachyon-agent)
  target_compile_definitions(viewer_perf_tests PRIVATE TACHYON_AGENT_EXE="$<TARGET_FILE:tachyon-agent>")
  target_include_directories(viewer_perf_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit)
endif()
