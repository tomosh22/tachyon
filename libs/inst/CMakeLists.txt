# tachyon_inst (WP-52): instrumentation ingestion shared by the agent and tachyon-cli: raw stream server,
# frame spool, wire decoder, clock mapping and the merge of decoded records into a .tachyon trace.
# The wire definitions come from the SDK's inst_wire.h (header-only, namespace tachyon::inst).
tachyon_add_library(inst
  SOURCES clock_mapper.cpp decoder.cpp frame_stream.cpp ingest.cpp merge.cpp raw_stream_server.cpp
          sidecar_paths.cpp spool.cpp
  PUBLIC_DEPS tachyon::core tachyon::trace
  PRIVATE_DEPS nlohmann_json::nlohmann_json)
target_include_directories(tachyon_inst PUBLIC ${PROJECT_SOURCE_DIR}/sdk/src)
find_package(Threads REQUIRED)
target_link_libraries(tachyon_inst PUBLIC Threads::Threads)
