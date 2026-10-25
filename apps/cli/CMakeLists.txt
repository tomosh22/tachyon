# tachyon-cli: thin agent client (WP-10; extended by WP-13). cli_core is a static library so tests can drive it.
add_library(tachyon_cli_core STATIC cli.cpp)
add_library(tachyon::cli_core ALIAS tachyon_cli_core)
target_include_directories(tachyon_cli_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tachyon_cli_core PUBLIC tachyon::control tachyon::capture tachyon::core tachyon::inst)
target_compile_features(tachyon_cli_core PUBLIC cxx_std_20)
tachyon_set_warnings(tachyon_cli_core)

tachyon_add_executable(tachyon-cli SOURCES main.cpp DEPS tachyon::cli_core)
