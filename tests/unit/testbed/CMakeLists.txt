tachyon_add_test(testbed_tests SERIAL
  SOURCES calibration_test.cpp report_test.cpp scenarios_test.cpp
  DEPS tachyon::testbed_core)

# Runs the real tachyon-testbed binary (`--scenario all --duration 5`, about 5 s) and validates its JSON.
tachyon_add_test(testbed_integration_tests
  SOURCES run_all_test.cpp
  DEPS tachyon::testbed_core
  LABELS integration)
if(TARGET testbed_integration_tests)
  add_dependencies(testbed_integration_tests tachyon-testbed)
  target_compile_definitions(testbed_integration_tests PRIVATE TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>")
endif()
