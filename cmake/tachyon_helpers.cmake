# Helpers shared by every library, app, and test.

# Fixtures generated at build time by tracegen (WP-04) land here. Committed fixtures stay under tests/fixtures.
set(TACHYON_GENERATED_FIXTURE_DIR "${PROJECT_BINARY_DIR}/fixtures")

# tachyon_add_library(<name> SOURCES ... [PUBLIC_DEPS ...] [PRIVATE_DEPS ...])
# Creates static library tachyon_<name> with alias tachyon::<name>; include root is libs/ so headers are
# included as "<name>/<header>.h".
function(tachyon_add_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
  add_library(tachyon_${name} STATIC ${ARG_SOURCES})
  add_library(tachyon::${name} ALIAS tachyon_${name})
  target_include_directories(tachyon_${name} PUBLIC ${PROJECT_SOURCE_DIR}/libs)
  target_link_libraries(tachyon_${name} PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
  target_compile_features(tachyon_${name} PUBLIC cxx_std_20)
  tachyon_set_warnings(tachyon_${name})
endfunction()

# tachyon_add_executable(<name> SOURCES ... [DEPS ...])
function(tachyon_add_executable name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE ${ARG_DEPS})
  target_compile_features(${name} PRIVATE cxx_std_20)
  tachyon_set_warnings(${name})
endfunction()

# tachyon_add_test(<name> SOURCES ... [DEPS ...] [LABELS ...])
# Unit test executable linked with GoogleTest; TACHYON_FIXTURE_DIR points at tests/fixtures.
function(tachyon_add_test name)
  if(NOT TACHYON_BUILD_TESTS)
    return()
  endif()
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;LABELS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE ${ARG_DEPS} GTest::gtest GTest::gmock GTest::gtest_main)
  target_compile_features(${name} PRIVATE cxx_std_20)
  target_compile_definitions(${name} PRIVATE TACHYON_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
                                             TACHYON_GENERATED_FIXTURE_DIR="${TACHYON_GENERATED_FIXTURE_DIR}"
                                             TACHYON_BINARY_DIR="${PROJECT_BINARY_DIR}")
  # Tests that read generated fixtures must be built after them (target defined by tools/tracegen, WP-04).
  if(TARGET tachyon_fixtures)
    add_dependencies(${name} tachyon_fixtures)
  endif()
  tachyon_set_warnings(${name})
  if(NOT ARG_LABELS)
    set(ARG_LABELS unit)
  endif()
  gtest_discover_tests(${name} PROPERTIES LABELS "${ARG_LABELS}" DISCOVERY_TIMEOUT 60)
endfunction()

# tachyon_add_perf_test(<name> SOURCES ... [DEPS ...]): same as tachyon_add_test, labelled "perf".
# Perf tests assert budgets; they fail CI on a 2x regression (Part 2.4).
function(tachyon_add_perf_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  tachyon_add_test(${name} SOURCES ${ARG_SOURCES} DEPS ${ARG_DEPS} LABELS perf)
endfunction()

# Adds every immediate subdirectory of <dir> that contains a CMakeLists.txt, in sorted order.
# Lets work packages add a component without editing a shared CMakeLists.txt.
function(tachyon_add_subdirectories dir)
  file(GLOB _children LIST_DIRECTORIES true RELATIVE ${dir} ${dir}/*)
  list(SORT _children)
  foreach(child IN LISTS _children)
    if(IS_DIRECTORY ${dir}/${child} AND EXISTS ${dir}/${child}/CMakeLists.txt)
      add_subdirectory(${dir}/${child})
    endif()
  endforeach()
endfunction()
