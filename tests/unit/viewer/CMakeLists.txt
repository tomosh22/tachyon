if(NOT TACHYON_BUILD_VIEWER)
  return()
endif()

tachyon_add_test(viewer_tests
  SOURCES selection_bus_test.cpp settings_test.cpp job_queue_test.cpp app_test.cpp misc_test.cpp
          timeline_math_test.cpp timeline_rows_test.cpp timeline_colors_test.cpp timeline_widget_test.cpp
          timeline_data_test.cpp interaction_model_test.cpp interaction_arrow_test.cpp
          interaction_runner_test.cpp interaction_widget_test.cpp
          callgraph_model_test.cpp callgraph_widget_test.cpp functions_nostack_test.cpp
          functions_model_test.cpp functions_widget_test.cpp
          stats_fuzzy_test.cpp stats_model_test.cpp stats_widget_test.cpp
          status_banner_test.cpp status_bar_test.cpp status_symbols_test.cpp status_toast_test.cpp
          status_shortcuts_test.cpp status_followups_test.cpp
          inst_lanes_test.cpp inst_frames_plots_test.cpp inst_widget_test.cpp streaming_app_test.cpp memory_ceiling_test.cpp
          source_model_test.cpp source_widget_test.cpp
          capture_model_test.cpp session_explorer_catalog_test.cpp session_explorer_panel_test.cpp
          capture_agent_session_test.cpp capture_app_test.cpp capture_flight_recorder_test.cpp
          capture_remote_test.cpp
          ../agent/process_util.cpp
  DEPS tachyon::viewer_core tachyon::analysis tachyon::symbols tachyon::disasm tachyon::trace
       tachyon::agent_core tachyon::control tachyon::capture)

# WP-41: the capture tests run the real tachyon-agent (--backend mock) and an in-process Agent with the fake
# launch backend of the agent tests.
if(TARGET viewer_tests AND TARGET tachyon-agent)
  add_dependencies(viewer_tests tachyon-agent)
  target_compile_definitions(viewer_tests PRIVATE TACHYON_AGENT_EXE="$<TARGET_FILE:tachyon-agent>")
  target_include_directories(viewer_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit)
endif()

# WP-45: the Source panel tests run against the profiler testbed (real symbols, source lines and code bytes).
if(TARGET viewer_tests AND TARGET tachyon-testbed)
  add_dependencies(viewer_tests tachyon-testbed)
  target_compile_definitions(viewer_tests PRIVATE
    TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>"
    TACHYON_TESTBED_SOURCE_DIR="${PROJECT_SOURCE_DIR}/apps/testbed/testbed")
endif()
# The hot/cold split fixture of the DWARF provider tests (only when g++ is available).
if(TARGET viewer_tests AND TARGET dwarf_fixture_split)
  add_dependencies(viewer_tests dwarf_fixture_split)
  target_compile_definitions(viewer_tests PRIVATE
    TACHYON_SPLIT_FIXTURE="${PROJECT_BINARY_DIR}/tests/unit/symbols/dwarf_fixtures/dwarf_fixture_split")
endif()

# Headless end-to-end checks of the real binary: no display, Null renderer, 100 frames.
if(TARGET tachyon-viewer AND TACHYON_BUILD_TESTS)
  foreach(name IN ITEMS tiny medium)
    add_test(NAME viewer_selftest_${name}
      COMMAND tachyon-viewer --selftest ${TACHYON_GENERATED_FIXTURE_DIR}/${name}.tachyon)
    set_tests_properties(viewer_selftest_${name} PROPERTIES
      LABELS "unit;viewer"
      PASS_REGULAR_EXPRESSION "selftest: OK file=${name}.tachyon threads=[1-9][0-9]* samples=[1-9][0-9]* modules=[0-9]+ frames=100")
  endforeach()

  # A missing file must give a clean non-zero exit (not a crash): the script distinguishes the two.
  add_test(NAME viewer_selftest_missing_file
    COMMAND ${CMAKE_COMMAND} -DVIEWER=$<TARGET_FILE:tachyon-viewer>
            -DMISSING=${TACHYON_GENERATED_FIXTURE_DIR}/does-not-exist.tachyon
            -P ${CMAKE_CURRENT_SOURCE_DIR}/selftest_missing.cmake)
  set_tests_properties(viewer_selftest_missing_file PROPERTIES LABELS "unit;viewer")
endif()
