tachyon_add_test(agent_tests
  SOURCES agent_test.cpp agent_concurrency_test.cpp cli_test.cpp acceptance_test.cpp launch_agent_test.cpp
          inst_agent_test.cpp process_util.cpp
  DEPS tachyon::agent_core tachyon::cli_core tachyon::capture tachyon::control tachyon::trace tachyon::inst)

if(TARGET agent_tests)
  # The acceptance test launches the real executables.
  add_dependencies(agent_tests tachyon-agent tachyon-cli)
  target_compile_definitions(agent_tests PRIVATE TACHYON_AGENT_EXE="$<TARGET_FILE:tachyon-agent>"
                                                 TACHYON_CLI_EXE="$<TARGET_FILE:tachyon-cli>")
endif()
