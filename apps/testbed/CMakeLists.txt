# tachyon-testbed: known-answer workload for validating profiler attribution (WP-02).
find_package(Threads REQUIRED)

add_library(tachyon_testbed_core STATIC
  testbed/calibration.cpp
  testbed/platform.cpp
  testbed/report.cpp
  testbed/scenarios.cpp)
add_library(tachyon::testbed_core ALIAS tachyon_testbed_core)
target_include_directories(tachyon_testbed_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tachyon_testbed_core PUBLIC tachyon::core nlohmann_json::nlohmann_json Threads::Threads)
target_compile_features(tachyon_testbed_core PUBLIC cxx_std_20)
tachyon_set_warnings(tachyon_testbed_core)

tachyon_add_executable(tachyon-testbed SOURCES main.cpp DEPS tachyon::testbed_core)

# Frame pointers, debug info, and no tail-call merging on the testbed targets only (profiler stack-walk fixtures).
foreach(_tb_target tachyon_testbed_core tachyon-testbed)
  if(MSVC)
    target_compile_options(${_tb_target} PRIVATE /Oy- /Zi)
  else()
    target_compile_options(${_tb_target} PRIVATE -fno-omit-frame-pointer -fno-optimize-sibling-calls -g)
  endif()
endforeach()
if(MSVC)
  target_link_options(tachyon-testbed PRIVATE /DEBUG)
endif()
