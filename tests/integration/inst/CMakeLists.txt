# WP-52 integration tests: the instrumented testbed against the agent (mock backend) and the offline merge.
tachyon_add_test(inst_integration_tests
  SOURCES testbed_ingest_test.cpp ../../unit/agent/process_util.cpp
  DEPS tachyon::agent_core tachyon::cli_core tachyon::capture tachyon::control tachyon::trace tachyon::inst
       nlohmann_json::nlohmann_json
  LABELS integration)

if(TARGET inst_integration_tests)
  add_dependencies(inst_integration_tests tachyon-testbed tachyon-agent tachyon-cli)
  target_compile_definitions(inst_integration_tests PRIVATE TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>"
                                                            TACHYON_AGENT_EXE="$<TARGET_FILE:tachyon-agent>"
                                                            TACHYON_CLI_EXE="$<TARGET_FILE:tachyon-cli>")
endif()
