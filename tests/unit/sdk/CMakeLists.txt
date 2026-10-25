# SDK tests. Header compile tests are real executables built with strict language modes and run via ctest.
set(SDK_INCLUDE ${PROJECT_SOURCE_DIR}/sdk/include)

# Library with no Tachyon_GetAPI symbol, used to test the loader's missing-symbol path.
add_library(tachyon_sdk_nosymbol_lib SHARED nosymbol_lib.c)
tachyon_set_warnings(tachyon_sdk_nosymbol_lib)

# name | language std property | enabled | link sdk
function(sdk_header_test name source enabled)
  add_executable(${name} ${source})
  target_include_directories(${name} PRIVATE ${SDK_INCLUDE})
  target_compile_definitions(${name} PRIVATE TACHYON_ENABLED=${enabled})
  target_link_libraries(${name} PRIVATE ${CMAKE_DL_LIBS})
  if(enabled)
    target_link_libraries(${name} PRIVATE tachyon::sdk)
  endif()
  if(source MATCHES "\\.c$")
    set_target_properties(${name} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  else()
    set_target_properties(${name} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  endif()
  tachyon_set_warnings(${name})
  if(MSVC)
    # The CHECK(<constant expression>) assertions are intentional.
    target_compile_options(${name} PRIVATE /wd4127)
  endif()
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES LABELS unit)
endfunction()

sdk_header_test(sdk_header_c99_enabled header_c_test.c 1)
sdk_header_test(sdk_header_c99_disabled header_c_test.c 0)
sdk_header_test(sdk_header_cxx17_enabled header_cpp_test.cpp 1)
sdk_header_test(sdk_header_cxx17_disabled header_cpp_test.cpp 0)

tachyon_add_test(sdk_tests SOURCES sdk_test.cpp sdk_test_env.cpp DEPS tachyon::sdk ${CMAKE_DL_LIBS})
if(TARGET sdk_tests)
  add_dependencies(sdk_tests tachyon_sdk_shared tachyon_sdk_nosymbol_lib)
  if(MSVC)
    target_compile_options(sdk_tests PRIVATE /wd4127)
  endif()
  target_include_directories(sdk_tests PRIVATE ${PROJECT_SOURCE_DIR}/sdk/src)
  target_compile_definitions(
    sdk_tests PRIVATE TACHYON_TEST_SHARED_LIB="$<TARGET_FILE:tachyon_sdk_shared>"
                      TACHYON_TEST_NO_SYMBOL_LIB="$<TARGET_FILE:tachyon_sdk_nosymbol_lib>")
endif()

# Runtime tests (WP-51): fake agent (real tachyon_control server) + test-only stream reader.
set(SDK_RUNTIME_DEPS tachyon::sdk tachyon::control tachyon::core)
tachyon_add_test(sdk_runtime_tests
  SOURCES runtime_sidecar_test.cpp runtime_agent_test.cpp runtime_drop_test.cpp
  DEPS ${SDK_RUNTIME_DEPS})
tachyon_add_test(sdk_stress_tests SOURCES runtime_stress_test.cpp DEPS ${SDK_RUNTIME_DEPS})
foreach(t sdk_runtime_tests sdk_stress_tests)
  if(TARGET ${t})
    target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/sdk/src)
    if(MSVC)
      target_compile_options(${t} PRIVATE /wd4127)
    endif()
  endif()
endforeach()
# 16 threads x 1 M scopes in release; a tenth of that under sanitizers to keep CI time bounded.
if(TARGET sdk_stress_tests)
  if(CMAKE_CXX_FLAGS MATCHES "sanitize")
    target_compile_definitions(sdk_stress_tests PRIVATE TACHYON_STRESS_SCOPES_PER_THREAD=100000)
  endif()
endif()
