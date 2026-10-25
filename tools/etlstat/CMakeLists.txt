# etlstat: ETW spike tool (WP-01). The pure logic lives in a static library so unit tests can link it on any
# platform; only etw_platform.cpp touches advapi32 (and only on Windows).
add_library(tachyon_etlstat_core STATIC
  cli.cpp
  etw_hex.cpp
  etw_payload.cpp
  etw_platform.cpp
  etw_stats.cpp
  etw_types.cpp
  report.cpp
  session_config.cpp
  wait_reason.cpp)
add_library(tachyon::etlstat_core ALIAS tachyon_etlstat_core)
target_include_directories(tachyon_etlstat_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tachyon_etlstat_core PUBLIC tachyon::core fmt::fmt nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(tachyon_etlstat_core PUBLIC advapi32)
endif()
target_compile_features(tachyon_etlstat_core PUBLIC cxx_std_20)
tachyon_set_warnings(tachyon_etlstat_core)

tachyon_add_executable(etlstat SOURCES main.cpp DEPS tachyon::etlstat_core)
