# ETL to .tachyon converter (WP-12). Everything but the ProcessTrace driver (etl_driver_windows.cpp, guarded by
# TACHYON_PLATFORM_WINDOWS) is platform independent and unit-tested on Linux.
tachyon_add_library(etlconvert
  SOURCES conversion_pipeline.cpp device_path.cpp etl_converter.cpp etl_driver_windows.cpp hex_events.cpp
          module_table.cpp pe_info.cpp stack_matcher.cpp trace_metadata.cpp
  PUBLIC_DEPS tachyon::core tachyon::trace
  PRIVATE_DEPS nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(tachyon_etlconvert PUBLIC advapi32)
endif()
find_package(Threads REQUIRED)
target_link_libraries(tachyon_etlconvert PUBLIC Threads::Threads)
