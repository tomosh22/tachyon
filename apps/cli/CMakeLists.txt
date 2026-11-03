# tachyon-cli: thin agent client (WP-10; extended by WP-13). cli_core is a static library so tests can drive it.
add_library(tachyon_cli_core STATIC cli.cpp doctor.cpp export_command.cpp)
add_library(tachyon::cli_core ALIAS tachyon_cli_core)
target_include_directories(tachyon_cli_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tachyon_cli_core PUBLIC tachyon::control tachyon::capture tachyon::core tachyon::inst
  tachyon::export tachyon::analysis tachyon::symbols tachyon::model)
target_link_libraries(tachyon_cli_core PRIVATE nlohmann_json::nlohmann_json)
if(TARGET tachyon::capture_linux_perf)  # doctor's trial perf_event_open calls (WP-73)
  target_link_libraries(tachyon_cli_core PUBLIC tachyon::capture_linux_perf)
endif()
target_compile_features(tachyon_cli_core PUBLIC cxx_std_20)
tachyon_set_warnings(tachyon_cli_core)

tachyon_add_executable(tachyon-cli SOURCES main.cpp DEPS tachyon::cli_core)
