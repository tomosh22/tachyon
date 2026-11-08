if(NOT TACHYON_BUILD_VIEWER)
  return()
endif()

# WP-65b: the viewer's Live mode against the real agent (--backend perf) and the testbed's spin scenario: the
# Live dialog, a Live document that follows the capture (latency, window, flat memory, symbols loaded once per
# module, shares vs the final trace). Skips, with the reason, without system-wide perf_event_open.
tachyon_add_test(live_viewer_linux_tests
  SOURCES live_viewer_test.cpp ../../unit/agent/process_util.cpp
  DEPS tachyon::viewer_core tachyon::capture_linux_perf tachyon::capture tachyon::control tachyon::model
       tachyon::symbols tachyon::trace
  LABELS linux_privileged)
if(TARGET live_viewer_linux_tests)
  target_compile_definitions(live_viewer_linux_tests PRIVATE TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>"
                                                             TACHYON_AGENT_EXE="$<TARGET_FILE:tachyon-agent>")
  target_include_directories(live_viewer_linux_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit)
  add_dependencies(live_viewer_linux_tests tachyon-testbed tachyon-agent)
endif()
