tachyon_add_library(trace
  SOURCES string_table.cpp callstack_interner.cpp trace_writer.cpp trace_reader.cpp
  PUBLIC_DEPS tachyon::core
  PRIVATE_DEPS tachyon::zstd)
