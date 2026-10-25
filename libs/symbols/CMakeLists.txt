find_package(Threads REQUIRED)
set(TACHYON_SYMBOLS_HTTP_SOURCES http_client_curl.cpp)
if(WIN32)
  set(TACHYON_SYMBOLS_HTTP_SOURCES http_client_winhttp.cpp)
else()
  find_package(CURL REQUIRED)
endif()
tachyon_add_library(symbols
  SOURCES demangle.cpp function_key.cpp http_client.cpp json_symbol_provider.cpp range_table.cpp
          symbol_keys.cpp symbol_locator.cpp symbol_provider.cpp symbol_service.cpp symbol_strings.cpp
          ${TACHYON_SYMBOLS_HTTP_SOURCES}
  PUBLIC_DEPS tachyon::core tachyon::trace tachyon::model Threads::Threads
  PRIVATE_DEPS nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(tachyon_symbols PRIVATE winhttp)
else()
  target_link_libraries(tachyon_symbols PRIVATE CURL::libcurl)
endif()
