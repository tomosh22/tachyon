tachyon_add_test(etlconvert_tests
  SOURCES events_test.cpp hex_events_test.cpp pe_info_test.cpp device_path_test.cpp module_table_test.cpp
          stack_matcher_test.cpp pipeline_test.cpp target_interval_test.cpp
  DEPS tachyon::etlconvert nlohmann_json::nlohmann_json)
