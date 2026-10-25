# WP-52: real ETW capture of `tachyon-testbed --instrument` through a real tachyon-agent; scopes and samples must
# end up in one trace. Needs an elevated runner (the test skips itself otherwise).
tachyon_add_test(inst_windows_tests
  SOURCES ingest_windows_test.cpp ../../unit/agent/process_util.cpp
  DEPS tachyon::capture tachyon::control tachyon::etlconvert tachyon::trace tachyon::inst tachyon::core
       nlohmann_json::nlohmann_json
  LABELS windows)
target_compile_definitions(inst_windows_tests PRIVATE
  TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>"
  TACHYON_AGENT_EXE="$<TARGET_FILE:tachyon-agent>"
  TACHYON_CLI_EXE="$<TARGET_FILE:tachyon-cli>")
add_dependencies(inst_windows_tests tachyon-testbed tachyon-agent tachyon-cli)
