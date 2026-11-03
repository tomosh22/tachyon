# WP-63: speedscope and Chrome trace event exporters. The library directory is `export` (the plan's name); the
# C++ namespace is tachyon::exporters because `export` is a C++20 keyword.
tachyon_add_library(export
  SOURCES chrome_trace.cpp json_writer.cpp speedscope.cpp
  PUBLIC_DEPS tachyon::core tachyon::trace tachyon::model tachyon::analysis tachyon::symbols)
