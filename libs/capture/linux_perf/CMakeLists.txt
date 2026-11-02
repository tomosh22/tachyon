# LinuxPerfBackend (WP-70): perf_event_open sampling, spool, conversion to .tachyon. Linux only; included from
# libs/capture/CMakeLists.txt.
find_package(Threads REQUIRED)
tachyon_add_library(capture_linux_perf
  SOURCES drain.cpp launch.cpp linux_perf_backend.cpp perf_converter.cpp perf_records.cpp perf_system.cpp
          proc_snapshot.cpp ring_buffer.cpp spool.cpp
  PUBLIC_DEPS tachyon::capture tachyon::core tachyon::trace Threads::Threads
  PRIVATE_DEPS tachyon::symbols nlohmann_json::nlohmann_json)
