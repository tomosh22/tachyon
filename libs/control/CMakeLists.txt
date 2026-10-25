tachyon_add_library(control
  SOURCES protocol.cpp framed_connection.cpp posix_transport.cpp windows_transport.cpp server.cpp client.cpp
  PUBLIC_DEPS tachyon::core tachyon::capture
  PRIVATE_DEPS nlohmann_json::nlohmann_json)
find_package(Threads REQUIRED)
target_link_libraries(tachyon_control PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(tachyon_control PUBLIC advapi32)
endif()
