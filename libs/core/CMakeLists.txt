tachyon_add_library(core
  SOURCES arena.cpp hash.cpp log.cpp time.cpp
  PUBLIC_DEPS fmt::fmt
  PRIVATE_DEPS tachyon::xxhash)
