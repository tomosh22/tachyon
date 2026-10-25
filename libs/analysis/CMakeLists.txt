tachyon_add_library(analysis
  SOURCES lod.cpp thread_states.cpp wait_reasons.cpp
  PUBLIC_DEPS tachyon::core tachyon::trace tachyon::model)
