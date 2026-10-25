# Fetches ntdll.pdb from the Microsoft symbol server (skips when the network is unreachable).
tachyon_add_test(symbols_windows_tests
  SOURCES msdl_fetch_test.cpp winhttp_cancel_test.cpp
  DEPS tachyon::symbols tachyon::etlconvert
  LABELS windows)
if(TARGET symbols_windows_tests)
  target_link_libraries(symbols_windows_tests PRIVATE ws2_32)
endif()

# Real msdia140 against the testbed's PDB (WP-22). Skipped entirely when DIA is disabled.
if(TACHYON_ENABLE_DIA)
  tachyon_add_test(dia_windows_tests
    SOURCES dia_provider_test.cpp
    DEPS tachyon::symbols tachyon::etlconvert
    LABELS windows)
  if(TARGET dia_windows_tests)
    target_compile_definitions(dia_windows_tests PRIVATE
      TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>"
      TACHYON_TESTBED_SCENARIOS_CPP="${PROJECT_SOURCE_DIR}/apps/testbed/testbed/scenarios.cpp")
    add_dependencies(dia_windows_tests tachyon-testbed)
    tachyon_copy_dia_dll(dia_windows_tests)
  endif()
endif()
