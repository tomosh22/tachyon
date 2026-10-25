# WP-13: the full tachyon-cli command set. The in-process tests drive cli::run against an in-process agent with
# a scriptable backend; the end-to-end tests run the real tachyon-agent (--backend mock) and tachyon-cli.
tachyon_add_test(cli_tests
  SOURCES cli_commands_test.cpp cli_end_to_end_test.cpp merge_command_test.cpp ../agent/process_util.cpp
  DEPS tachyon::agent_core tachyon::cli_core tachyon::capture tachyon::control tachyon::trace tachyon::inst
       nlohmann_json::nlohmann_json)

if(TARGET cli_tests)
  add_dependencies(cli_tests tachyon-agent tachyon-cli)
  target_compile_definitions(cli_tests PRIVATE TACHYON_AGENT_EXE="$<TARGET_FILE:tachyon-agent>"
                                               TACHYON_CLI_EXE="$<TARGET_FILE:tachyon-cli>")
endif()
