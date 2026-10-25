tachyon_add_test(trace_tests
  SOURCES trace_format_test.cpp string_table_test.cpp callstack_interner_test.cpp trace_roundtrip_test.cpp
          trace_partial_test.cpp trace_corrupt_test.cpp
  DEPS tachyon::trace)
