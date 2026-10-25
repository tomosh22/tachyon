# WP-52: instrumentation ingestion (spool, decode, remap, clock mapping, merge, raw stream server).
tachyon_add_test(inst_tests
  SOURCES frame_stream_test.cpp clock_mapper_test.cpp sidecar_paths_test.cpp spool_test.cpp decoder_test.cpp
          merge_test.cpp ingest_test.cpp
  DEPS tachyon::inst tachyon::trace nlohmann_json::nlohmann_json)

# Perf budget (label "perf"): spool / decode / merge throughput. Lives here because WP-52 owns tests/unit/inst.
tachyon_add_perf_test(inst_perf_tests
  SOURCES ingest_perf_test.cpp
  DEPS tachyon::inst tachyon::trace)
