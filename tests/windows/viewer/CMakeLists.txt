if(NOT TACHYON_BUILD_VIEWER)
  return()
endif()

# The Source panel on the real DIA provider (WP-45): hot_a of the testbed renders its source file from the
# checkout. Skipped entirely when DIA is disabled.
if(TACHYON_ENABLE_DIA AND TARGET tachyon-testbed)
  tachyon_add_test(viewer_source_windows_tests
    SOURCES source_dia_test.cpp
    DEPS tachyon::viewer_core tachyon::symbols tachyon::etlconvert tachyon::trace tachyon::disasm
    LABELS windows)
  if(TARGET viewer_source_windows_tests)
    target_compile_definitions(viewer_source_windows_tests PRIVATE
      TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>"
      TACHYON_TESTBED_SCENARIOS_CPP="${PROJECT_SOURCE_DIR}/apps/testbed/testbed/scenarios.cpp")
    add_dependencies(viewer_source_windows_tests tachyon-testbed)
    tachyon_copy_dia_dll(viewer_source_windows_tests)
  endif()
endif()
