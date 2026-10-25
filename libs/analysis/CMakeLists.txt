tachyon_add_library(analysis
  SOURCES aggregation.cpp attribution.cpp lod.cpp thread_states.cpp wait_reasons.cpp
  PUBLIC_DEPS tachyon::core tachyon::trace tachyon::model tachyon::symbols)
