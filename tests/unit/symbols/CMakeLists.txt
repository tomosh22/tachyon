tachyon_add_test(symbols_tests
  SOURCES dia_provider_test.cpp dia_symbol_table_test.cpp function_key_test.cpp json_provider_test.cpp lookup_caller_test.cpp
          symbol_service_test.cpp symbol_keys_test.cpp cancel_test.cpp
          symbol_locator_test.cpp
  DEPS tachyon::symbols tachyon::model tachyon::trace nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(symbols_tests PRIVATE ws2_32)
endif()

# DWARF symbol provider (WP-72): Linux + libdw only. The fixture programs are compiled here (they are only parsed,
# never run): PIE and non-PIE with debug info, one without debug info (symbol-table fallback), and, when g++ is
# available, one built with -freorder-blocks-and-partition so that a function really is split into hot/cold ranges.
if(TACHYON_ENABLE_LIBDW)
  set(TACHYON_DWARF_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/dwarf_fixtures")
  set(_dwarf_fx_src "${CMAKE_CURRENT_SOURCE_DIR}/dwarf_fixtures/fixture_main.cpp")
  set(_dwarf_fx_hdr "${CMAKE_CURRENT_SOURCE_DIR}/dwarf_fixtures/fixture_inl.h")

  function(tachyon_dwarf_fixture name)
    cmake_parse_arguments(ARG "NO_PIE;NO_DEBUG" "" "" ${ARGN})
    add_executable(${name} ${_dwarf_fx_src})
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TACHYON_DWARF_FIXTURE_DIR}")
    # Only parsed, never run: keep them small and identical in every preset (no sanitizer runtime inside).
    target_compile_options(${name} PRIVATE -O2 -fno-omit-frame-pointer -fno-sanitize=all)
    target_link_options(${name} PRIVATE -fno-sanitize=all)
    if(NOT ARG_NO_DEBUG)
      target_compile_options(${name} PRIVATE -g)
    else()
      target_compile_options(${name} PRIVATE -g0)
    endif()
    if(ARG_NO_PIE)
      set_property(TARGET ${name} PROPERTY POSITION_INDEPENDENT_CODE OFF)
      target_compile_options(${name} PRIVATE -fno-pie)
      target_link_options(${name} PRIVATE -no-pie)
    else()
      target_compile_options(${name} PRIVATE -fPIE)
      target_link_options(${name} PRIVATE -pie)
    endif()
    target_link_options(${name} PRIVATE -Wl,--build-id=sha1)
  endfunction()

  tachyon_dwarf_fixture(dwarf_fixture_pie)
  tachyon_dwarf_fixture(dwarf_fixture_nopie NO_PIE)
  tachyon_dwarf_fixture(dwarf_fixture_nodebug NO_DEBUG)
  set(_dwarf_fx_targets dwarf_fixture_pie dwarf_fixture_nopie dwarf_fixture_nodebug)

  find_program(TACHYON_GXX g++)
  if(TACHYON_GXX)
    add_custom_command(
      OUTPUT "${TACHYON_DWARF_FIXTURE_DIR}/dwarf_fixture_split"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${TACHYON_DWARF_FIXTURE_DIR}"
      COMMAND ${TACHYON_GXX} -std=c++17 -O2 -g -freorder-blocks-and-partition -fno-pie -no-pie -Wl,--build-id=sha1
              "${_dwarf_fx_src}" -o "${TACHYON_DWARF_FIXTURE_DIR}/dwarf_fixture_split"
      DEPENDS "${_dwarf_fx_src}" "${_dwarf_fx_hdr}"
      COMMENT "Building the hot/cold split DWARF fixture with g++"
      VERBATIM)
    add_custom_target(dwarf_fixture_split ALL DEPENDS "${TACHYON_DWARF_FIXTURE_DIR}/dwarf_fixture_split")
    list(APPEND _dwarf_fx_targets dwarf_fixture_split)
  endif()

  tachyon_add_test(dwarf_symbols_tests
    SOURCES dwarf_elf_test.cpp dwarf_provider_test.cpp dwarf_libdw_test.cpp dwarf_locator_test.cpp
            dwarf_robustness_test.cpp
    DEPS tachyon::symbols tachyon::model tachyon::trace)
  if(TARGET dwarf_symbols_tests)
    add_dependencies(dwarf_symbols_tests ${_dwarf_fx_targets})
    if(TARGET tachyon-testbed)
      add_dependencies(dwarf_symbols_tests tachyon-testbed)
      target_compile_definitions(dwarf_symbols_tests PRIVATE
        TACHYON_TESTBED_EXE="$<TARGET_FILE:tachyon-testbed>"
        TACHYON_TESTBED_SOURCE_DIR="${PROJECT_SOURCE_DIR}/apps/testbed/testbed")
    endif()
    target_compile_definitions(dwarf_symbols_tests PRIVATE
      TACHYON_DWARF_FIXTURE_DIR="${TACHYON_DWARF_FIXTURE_DIR}"
      TACHYON_DWARF_FIXTURE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/dwarf_fixtures")
  endif()
endif()
