# WP-11: starts real ETW kernel sessions; needs an elevated runner (tests skip themselves otherwise).
tachyon_add_test(etw_capture_windows_tests
  SOURCES etw_capture_test.cpp
  DEPS tachyon::agent_core tachyon::capture tachyon::control tachyon::etlconvert tachyon::trace
  LABELS windows)
target_compile_definitions(etw_capture_windows_tests PRIVATE TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>")
add_dependencies(etw_capture_windows_tests tachyon-testbed)
