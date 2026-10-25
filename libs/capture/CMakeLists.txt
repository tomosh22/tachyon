tachyon_add_library(capture
  SOURCES mock/mock_backend.cpp
          windows_etw/etw_session.cpp windows_etw/glob_match.cpp windows_etw/launch.cpp
          windows_etw/launch_windows.cpp windows_etw/process_tree.cpp windows_etw/session_config.cpp windows_etw/session_lifecycle.cpp
          windows_etw/time_window.cpp windows_etw/windows_etw_backend.cpp
  PUBLIC_DEPS tachyon::core tachyon::etlconvert
  PRIVATE_DEPS nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(tachyon_capture PUBLIC advapi32)
endif()
