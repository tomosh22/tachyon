tachyon_add_test(capture_tests
  SOURCES mock_backend_test.cpp
  DEPS tachyon::capture tachyon::trace)

# WP-11: platform-independent parts of the ETW session controller (sizing, flags, naming, orphan matching,
# flush/restart phase logic, time-window conversion over hex fixtures).
tachyon_add_test(capture_etw_tests
  SOURCES session_config_test.cpp session_lifecycle_test.cpp time_window_test.cpp windows_etw_backend_test.cpp
          glob_match_test.cpp launch_util_test.cpp process_tree_test.cpp
  DEPS tachyon::capture tachyon::etlconvert tachyon::trace nlohmann_json::nlohmann_json)

# WP-70: the Linux perf_event_open backend. Everything here runs without privileges (a fake perf layer
# stands in for the kernel); the privileged capture tests live in tests/linux/capture.
if(TARGET tachyon::capture_linux_perf)
  tachyon_add_test(capture_linux_perf_tests
    SOURCES linux_perf_records_test.cpp linux_perf_spool_test.cpp linux_perf_proc_test.cpp
            linux_perf_convert_test.cpp linux_perf_backend_test.cpp linux_perf_agent_test.cpp
            linux_perf_sched_format_test.cpp linux_perf_sched_convert_test.cpp linux_perf_sched_backend_test.cpp
            linux_perf_retention_test.cpp linux_perf_live_test.cpp
    DEPS tachyon::capture_linux_perf tachyon::agent_core tachyon::analysis tachyon::capture tachyon::control
         tachyon::model tachyon::symbols tachyon::trace nlohmann_json::nlohmann_json)
endif()
