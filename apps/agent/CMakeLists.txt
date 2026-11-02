# tachyon-agent: capture control daemon (WP-10). agent_core is a static library so tests can drive it in-process.
add_library(tachyon_agent_core STATIC agent.cpp privileges.cpp)
add_library(tachyon::agent_core ALIAS tachyon_agent_core)
target_include_directories(tachyon_agent_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tachyon_agent_core PUBLIC tachyon::control tachyon::capture tachyon::core tachyon::inst)
target_compile_features(tachyon_agent_core PUBLIC cxx_std_20)
tachyon_set_warnings(tachyon_agent_core)

tachyon_add_executable(tachyon-agent SOURCES main.cpp DEPS tachyon::agent_core)

# WP-70: the Linux perf backend (`--backend perf`); other platforms keep the stub.
if(TARGET tachyon::capture_linux_perf)
  target_link_libraries(tachyon-agent PRIVATE tachyon::capture_linux_perf)
  target_compile_definitions(tachyon-agent PRIVATE TACHYON_HAVE_PERF_BACKEND=1)
else()
  target_compile_definitions(tachyon-agent PRIVATE TACHYON_HAVE_PERF_BACKEND=0)
endif()
