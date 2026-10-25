# tachyon-agent: capture control daemon (WP-10). agent_core is a static library so tests can drive it in-process.
add_library(tachyon_agent_core STATIC agent.cpp privileges.cpp)
add_library(tachyon::agent_core ALIAS tachyon_agent_core)
target_include_directories(tachyon_agent_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tachyon_agent_core PUBLIC tachyon::control tachyon::capture tachyon::core tachyon::inst)
target_compile_features(tachyon_agent_core PUBLIC cxx_std_20)
tachyon_set_warnings(tachyon_agent_core)

tachyon_add_executable(tachyon-agent SOURCES main.cpp DEPS tachyon::agent_core)
