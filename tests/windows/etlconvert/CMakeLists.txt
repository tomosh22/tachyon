# Records a real kernel ETL of tachyon-testbed (needs an elevated runner; skips otherwise) and converts it.
tachyon_add_test(etlconvert_windows_tests
  SOURCES etlconvert_real_test.cpp
  DEPS tachyon::etlconvert tachyon::etlstat_core dbghelp
  LABELS windows)
target_compile_definitions(etlconvert_windows_tests PRIVATE TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>")
add_dependencies(etlconvert_windows_tests tachyon-testbed)
