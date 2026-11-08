# WP-11: starts real ETW kernel sessions; needs an elevated runner (tests skip themselves otherwise).
tachyon_add_test(etw_capture_windows_tests
  SOURCES etw_capture_test.cpp
  DEPS tachyon::agent_core tachyon::capture tachyon::control tachyon::etlconvert tachyon::trace
  LABELS windows)
target_compile_definitions(etw_capture_windows_tests PRIVATE TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>")
add_dependencies(etw_capture_windows_tests tachyon-testbed)

# WP-65w: the ETW real-time live source against real kernel sessions: the live view's Windows acceptance through
# the real agent + client + LiveModel (latency, window, shares vs the final trace, no gap), live vs plain final
# traces, attach rundown, circular flush, tachyon-cli live. Needs an elevated runner (skips otherwise) and DIA
# (the shares are symbolised).
if(TACHYON_ENABLE_DIA)
  tachyon_add_test(etw_live_windows_tests
    SOURCES live_etw_test.cpp ../../unit/agent/process_util.cpp
    DEPS tachyon::capture tachyon::control tachyon::etlconvert tachyon::model tachyon::symbols tachyon::trace
         tachyon::core nlohmann_json::nlohmann_json
    LABELS windows)
  if(TARGET etw_live_windows_tests)
    target_compile_definitions(etw_live_windows_tests PRIVATE
      TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>"
      TACHYON_AGENT_EXE="$<TARGET_FILE:tachyon-agent>"
      TACHYON_CLI_EXE="$<TARGET_FILE:tachyon-cli>")
    add_dependencies(etw_live_windows_tests tachyon-testbed tachyon-agent tachyon-cli)
    tachyon_copy_dia_dll(etw_live_windows_tests)
  endif()
endif()
