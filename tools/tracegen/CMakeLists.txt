# tracegen: deterministic synthetic trace generator (WP-04). The generator lives in a static library so unit
# tests can drive it in-process; main.cpp is the CLI.
add_library(tachyon_tracegen_core STATIC
  generator.cpp
  manifest.cpp
  reference.cpp
  scenario.cpp
  sha256.cpp
  tally.cpp
  timeline.cpp)
add_library(tachyon::tracegen_core ALIAS tachyon_tracegen_core)
target_include_directories(tachyon_tracegen_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tachyon_tracegen_core PUBLIC tachyon::core tachyon::trace nlohmann_json::nlohmann_json)
target_compile_features(tachyon_tracegen_core PUBLIC cxx_std_20)
tachyon_set_warnings(tachyon_tracegen_core)

tachyon_add_executable(tracegen SOURCES main.cpp DEPS tachyon::tracegen_core)

# Fixtures are NOT committed (no Git LFS): the scenario JSON files and a SHA-256 manifest live in
# tests/fixtures/tracegen, and the binaries are regenerated into ${TACHYON_GENERATED_FIXTURE_DIR} at build time.
if(TACHYON_BUILD_TESTS)
  set(TRACEGEN_SCENARIO_DIR ${PROJECT_SOURCE_DIR}/tests/fixtures/tracegen)
  file(MAKE_DIRECTORY ${TACHYON_GENERATED_FIXTURE_DIR})
  set(TRACEGEN_FIXTURES tiny medium scopes fibers frames recursion partial)
  set(TRACEGEN_OUTPUTS "")
  foreach(name IN LISTS TRACEGEN_FIXTURES)
    set(outputs
      ${TACHYON_GENERATED_FIXTURE_DIR}/${name}.tachyon
      ${TACHYON_GENERATED_FIXTURE_DIR}/${name}.expected.json
      ${TACHYON_GENERATED_FIXTURE_DIR}/${name}.symbols.json)
    add_custom_command(
      OUTPUT ${outputs}
      COMMAND tracegen --out-dir ${TACHYON_GENERATED_FIXTURE_DIR} ${TRACEGEN_SCENARIO_DIR}/${name}.json
      DEPENDS tracegen ${TRACEGEN_SCENARIO_DIR}/${name}.json
      COMMENT "tracegen: generating fixture '${name}'"
      VERBATIM)
    list(APPEND TRACEGEN_OUTPUTS ${outputs})
  endforeach()
  add_custom_target(tachyon_fixtures ALL DEPENDS ${TRACEGEN_OUTPUTS})

  # 64 threads x 600 s at 8 kHz (~307 M samples, several GB of streaming work): opt-in, never built by default.
  set(large_outputs
    ${TACHYON_GENERATED_FIXTURE_DIR}/large.tachyon
    ${TACHYON_GENERATED_FIXTURE_DIR}/large.expected.json
    ${TACHYON_GENERATED_FIXTURE_DIR}/large.symbols.json)
  add_custom_command(
    OUTPUT ${large_outputs}
    COMMAND tracegen --out-dir ${TACHYON_GENERATED_FIXTURE_DIR} ${TRACEGEN_SCENARIO_DIR}/large.json
    DEPENDS tracegen ${TRACEGEN_SCENARIO_DIR}/large.json
    COMMENT "tracegen: generating fixture 'large' (slow)"
    VERBATIM)
  add_custom_target(tachyon_fixtures_large DEPENDS ${large_outputs})
endif()
