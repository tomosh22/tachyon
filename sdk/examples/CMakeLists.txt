# SDK usage examples (WP-55). Included from sdk/CMakeLists.txt when TACHYON_BUILD_TESTS is ON.
set(TACHYON_SDK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../include)
find_package(Threads REQUIRED)

# Enabled variant: linked against the static SDK.
add_executable(engine_hooks_example engine_hooks_example.cpp)
target_link_libraries(engine_hooks_example PRIVATE tachyon::sdk)
target_compile_features(engine_hooks_example PRIVATE cxx_std_17)
tachyon_set_warnings(engine_hooks_example)

# Disabled variant: TACHYON_ENABLED=0, linked against nothing (only the header's include path).
add_executable(engine_hooks_example_disabled engine_hooks_example.cpp)
target_include_directories(engine_hooks_example_disabled PRIVATE ${TACHYON_SDK_INCLUDE_DIR})
target_compile_definitions(engine_hooks_example_disabled PRIVATE TACHYON_ENABLED=0)
target_compile_features(engine_hooks_example_disabled PRIVATE cxx_std_17)
target_link_libraries(engine_hooks_example_disabled PRIVATE Threads::Threads)
tachyon_set_warnings(engine_hooks_example_disabled)

# Dynamic loading through tachyon_loader.h; needs the shared library at run time.
add_executable(dynamic_load_example dynamic_load_example.cpp)
target_include_directories(dynamic_load_example PRIVATE ${TACHYON_SDK_INCLUDE_DIR})
target_link_libraries(dynamic_load_example PRIVATE ${CMAKE_DL_LIBS})
target_compile_features(dynamic_load_example PRIVATE cxx_std_17)
add_dependencies(dynamic_load_example tachyon_sdk_shared)
tachyon_set_warnings(dynamic_load_example)

# Run the enabled variant briefly; the sidecar goes into the build tree, never the source tree.
set(_check ${CMAKE_CURRENT_SOURCE_DIR}/check_sidecar.cmake)
add_test(NAME sdk_example_engine_hooks_sidecar
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:engine_hooks_example>
                 -DSIDECAR=${CMAKE_CURRENT_BINARY_DIR}/engine_hooks_example.tachyon-inst "-DARGS=--frames;12" -P
                 ${_check})
add_test(NAME sdk_example_dynamic_load_sidecar
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:dynamic_load_example>
                 -DSIDECAR=${CMAKE_CURRENT_BINARY_DIR}/dynamic_load_example.tachyon-inst
                 "-DARGS=$<TARGET_FILE:tachyon_sdk_shared>" -P ${_check})
# The disabled variant must simply run.
add_test(NAME sdk_example_engine_hooks_disabled COMMAND engine_hooks_example_disabled --frames 6)
# Point the SDK at an unreachable agent so the tests never talk to a real one.
set_tests_properties(
  sdk_example_engine_hooks_sidecar sdk_example_dynamic_load_sidecar sdk_example_engine_hooks_disabled
  PROPERTIES ENVIRONMENT "TACHYON_AGENT_PIPE=${CMAKE_CURRENT_BINARY_DIR}/no-agent.sock" TIMEOUT 60)

# MSVC deprecates getenv (C4996); the examples only read plain environment variables.
if(MSVC)
  foreach(t engine_hooks_example engine_hooks_example_disabled dynamic_load_example)
    target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endforeach()
endif()
