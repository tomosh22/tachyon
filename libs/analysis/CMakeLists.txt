tachyon_add_library(analysis
  SOURCES aggregation.cpp attribution.cpp fibers.cpp frames.cpp function_registry.cpp instances.cpp locks.cpp lod.cpp query_cache.cpp query_executor.cpp
          scope_blend.cpp scope_pairing.cpp stats.cpp
          thread_states.cpp wait_reasons.cpp
  PUBLIC_DEPS tachyon::core tachyon::trace tachyon::model tachyon::symbols)
