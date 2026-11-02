# Disassembles hot_a of the testbed (PE bytes + DIA function bounds) and checks that synthetic sample hits land
# inside its bounds (WP-34). Skipped entirely when DIA is disabled.
if(TACHYON_ENABLE_DIA)
  tachyon_add_test(disasm_windows_tests
    SOURCES disasm_windows_test.cpp
    DEPS tachyon::disasm tachyon::analysis tachyon::symbols tachyon::etlconvert
    LABELS windows)
  if(TARGET disasm_windows_tests)
    target_compile_definitions(disasm_windows_tests PRIVATE
      TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>"
      TACHYON_TESTBED_SCENARIOS_CPP="${PROJECT_SOURCE_DIR}/apps/testbed/testbed/scenarios.cpp")
    target_include_directories(disasm_windows_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/analysis)
    add_dependencies(disasm_windows_tests tachyon-testbed)
    tachyon_copy_dia_dll(disasm_windows_tests)
  endif()
endif()
