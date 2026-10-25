# tachyon-viewer: Dear ImGui + SDL2 + OpenGL 3.3 front end (WP-40). Everything except main.cpp lives in the
# tachyon_viewer_core static library so tests/unit/viewer can drive it headless with the Null renderer.
if(NOT TACHYON_BUILD_VIEWER)
  return()
endif()

tachyon_require_imgui()

add_library(tachyon_viewer_core STATIC
  analysis_context.cpp
  app.cpp
  builtin_panels.cpp
  callgraph/callgraph_model.cpp
  callgraph/callgraph_panel.cpp
  callgraph/subtree_resolver.cpp
  cli_args.cpp
  common/function_names.cpp
  common/imgui_util.cpp
  common/query_filter.cpp
  command_registry.cpp
  document.cpp
  file_browser.cpp
  functions/butterfly_model.cpp
  functions/butterfly_panel.cpp
  functions/function_list_model.cpp
  functions/function_list_panel.cpp
  interaction/arrow_geometry.cpp
  interaction/interaction_model.cpp
  interaction/interaction_panel.cpp
  interaction/longest_waits_runner.cpp
  job_queue.cpp
  null_renderer.cpp
  panel_registry.cpp
  paths.cpp
  sdl_gl_renderer.cpp
  selection_bus.cpp
  settings.cpp
  stats/fuzzy_matcher.cpp
  stats/function_stats_panel.cpp
  stats/instance_index.cpp
  stats/stats_model.cpp
  theme.cpp
  timeline/timeline_colors.cpp
  timeline/timeline_data.cpp
  timeline/timeline_math.cpp
  timeline/timeline_panel.cpp
  timeline/timeline_rows.cpp
  timeline/timeline_widget.cpp)
add_library(tachyon::viewer_core ALIAS tachyon_viewer_core)
# Include root is apps/ so headers are included as "viewer/<header>.h".
target_include_directories(tachyon_viewer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tachyon_viewer_core
  PUBLIC tachyon::model tachyon::analysis tachyon::symbols tachyon::core tachyon::imgui
  PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(tachyon_viewer_core PUBLIC cxx_std_20)
tachyon_set_warnings(tachyon_viewer_core)

# Console subsystem on Windows (no WIN32 flag) so --selftest output is visible; SDL2main supplies the entry
# point and hands main() UTF-8 arguments.
tachyon_add_executable(tachyon-viewer SOURCES main.cpp DEPS tachyon::viewer_core)
