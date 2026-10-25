if(NOT TACHYON_BUILD_VIEWER)
  return()
endif()

tachyon_add_test(viewer_tests
  SOURCES selection_bus_test.cpp settings_test.cpp job_queue_test.cpp app_test.cpp misc_test.cpp
          timeline_math_test.cpp timeline_rows_test.cpp timeline_colors_test.cpp timeline_widget_test.cpp
          timeline_data_test.cpp interaction_model_test.cpp interaction_arrow_test.cpp
          interaction_runner_test.cpp interaction_widget_test.cpp
          callgraph_model_test.cpp callgraph_widget_test.cpp
  DEPS tachyon::viewer_core tachyon::analysis tachyon::symbols)

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
