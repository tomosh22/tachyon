tachyon_add_test(capture_tests
  SOURCES mock_backend_test.cpp
  DEPS tachyon::capture tachyon::trace)

# WP-11: platform-independent parts of the ETW session controller (sizing, flags, naming, orphan matching,
# flush/restart phase logic, time-window conversion over hex fixtures).
tachyon_add_test(capture_etw_tests
  SOURCES session_config_test.cpp session_lifecycle_test.cpp time_window_test.cpp windows_etw_backend_test.cpp
  DEPS tachyon::capture tachyon::etlconvert tachyon::trace nlohmann_json::nlohmann_json)
