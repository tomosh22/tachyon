# WP-13: launch / attach / child following through the real tachyon-cli and a real `tachyon-agent --backend etw`.
# Needs an elevated runner (the tests skip themselves otherwise).

# Process-tree helper (spins, spawns children, dumps its environment).
tachyon_add_executable(tachyon-test-spawner SOURCES spawner_main.cpp)

tachyon_add_test(launch_cli_windows_tests
  SOURCES launch_cli_test.cpp ../../unit/agent/process_util.cpp
  DEPS tachyon::capture tachyon::control tachyon::etlconvert tachyon::trace tachyon::core
       nlohmann_json::nlohmann_json
  LABELS windows)
target_compile_definitions(launch_cli_windows_tests PRIVATE
  TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>"
  TACHYON_SPAWNER_EXE="$<TARGET_FILE:tachyon-test-spawner>"
  TACHYON_AGENT_EXE="$<TARGET_FILE:tachyon-agent>"
  TACHYON_CLI_EXE="$<TARGET_FILE:tachyon-cli>")
add_dependencies(launch_cli_windows_tests tachyon-testbed tachyon-test-spawner tachyon-agent tachyon-cli)
