# WP-70: real perf_event_open captures of the testbed (label linux_privileged). They skip themselves, with the
# reason, when the kernel refuses perf_event_open (unprivileged CI runner, container without CAP_PERFMON,
# kernel.perf_event_paranoid too high). To run them on a GitHub ubuntu runner:
#   sudo sysctl kernel.perf_event_paranoid=-1 && ctest --preset linux-release -L linux_privileged
tachyon_add_test(perf_capture_linux_tests
  SOURCES perf_capture_test.cpp
  DEPS tachyon::capture_linux_perf tachyon::capture tachyon::symbols tachyon::model tachyon::trace
  LABELS linux_privileged)
if(TARGET perf_capture_linux_tests)
  target_compile_definitions(perf_capture_linux_tests PRIVATE TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>")
  add_dependencies(perf_capture_linux_tests tachyon-testbed)
endif()

# WP-71: scheduler events (sched:sched_switch / sched_waking tracepoints, PERF_RECORD_SWITCH fallback) of the
# testbed `lock` and `spin` scenarios. The tracepoint tests skip, with the reason, without tracefs or tracepoint
# permission (the CI privileged step needs paranoid -1 and a mounted tracefs: it fails when any test skips); the
# fallback tests need only perf_event_open.
tachyon_add_test(sched_capture_linux_tests
  SOURCES sched_capture_test.cpp
  DEPS tachyon::capture_linux_perf tachyon::capture tachyon::analysis tachyon::model tachyon::trace
  LABELS linux_privileged)
if(TARGET sched_capture_linux_tests)
  target_compile_definitions(sched_capture_linux_tests PRIVATE TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>")
  add_dependencies(sched_capture_linux_tests tachyon-testbed)
endif()
