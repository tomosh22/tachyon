# Unit tests: tests/unit/<lib>/CMakeLists.txt, one <topic>_test.cpp per topic.
tachyon_add_subdirectories(${CMAKE_CURRENT_SOURCE_DIR}/unit)
# Perf budget tests: tests/perf/<area>/CMakeLists.txt using tachyon_add_perf_test (label "perf").
tachyon_add_subdirectories(${CMAKE_CURRENT_SOURCE_DIR}/perf)
# Windows-only tests: tests/windows/<area>/CMakeLists.txt.
if(WIN32)
  tachyon_add_subdirectories(${CMAKE_CURRENT_SOURCE_DIR}/windows)
endif()
