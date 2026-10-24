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
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES LABELS unit)
endfunction()

sdk_header_test(sdk_header_c99_enabled header_c_test.c 1)
sdk_header_test(sdk_header_c99_disabled header_c_test.c 0)
sdk_header_test(sdk_header_cxx17_enabled header_cpp_test.cpp 1)
sdk_header_test(sdk_header_cxx17_disabled header_cpp_test.cpp 0)

tachyon_add_test(sdk_tests SOURCES sdk_test.cpp DEPS tachyon::sdk ${CMAKE_DL_LIBS})
if(TARGET sdk_tests)
  add_dependencies(sdk_tests tachyon_sdk_shared tachyon_sdk_nosymbol_lib)
  target_include_directories(sdk_tests PRIVATE ${PROJECT_SOURCE_DIR}/sdk/src)
  target_compile_definitions(
    sdk_tests PRIVATE TACHYON_TEST_SHARED_LIB="$<TARGET_FILE:tachyon_sdk_shared>"
                      TACHYON_TEST_NO_SYMBOL_LIB="$<TARGET_FILE:tachyon_sdk_nosymbol_lib>")
endif()
