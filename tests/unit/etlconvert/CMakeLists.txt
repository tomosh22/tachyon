tachyon_add_test(etlconvert_tests
  SOURCES events_test.cpp hex_events_test.cpp pe_info_test.cpp device_path_test.cpp module_table_test.cpp
          stack_matcher_test.cpp pipeline_test.cpp
  DEPS tachyon::etlconvert nlohmann_json::nlohmann_json)
# The packed on-disk records (libs/trace) default-construct StrongId members at unaligned addresses inside
# std::vector::resize in TraceReader's header templates; UBSan's alignment check flags that in this target only.
if(NOT MSVC)
  target_compile_options(etlconvert_tests PRIVATE -fno-sanitize=alignment)
endif()
