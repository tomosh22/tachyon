# Unit tests: tests/unit/<lib>/CMakeLists.txt, one <topic>_test.cpp per topic.
tachyon_add_subdirectories(${CMAKE_CURRENT_SOURCE_DIR}/unit)
# Integration tests that drive several components end to end: tests/integration/<area>/CMakeLists.txt.
tachyon_add_subdirectories(${CMAKE_CURRENT_SOURCE_DIR}/integration)
# Perf budget tests: tests/perf/<area>/CMakeLists.txt using tachyon_add_perf_test (label "perf").
tachyon_add_subdirectories(${CMAKE_CURRENT_SOURCE_DIR}/perf)
# Windows-only tests: tests/windows/<area>/CMakeLists.txt.
if(WIN32)
  tachyon_add_subdirectories(${CMAKE_CURRENT_SOURCE_DIR}/windows)
endif()
# Linux-only tests that need the kernel (perf_event_open): tests/linux/<area>/CMakeLists.txt.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET tachyon::capture_linux_perf AND TACHYON_ENABLE_LIBDW)
  tachyon_add_subdirectories(${CMAKE_CURRENT_SOURCE_DIR}/linux)
endif()
