find_package(Threads REQUIRED)
set(TACHYON_SYMBOLS_HTTP_SOURCES http_client_curl.cpp)
if(WIN32)
  set(TACHYON_SYMBOLS_HTTP_SOURCES http_client_winhttp.cpp)
else()
  find_package(CURL REQUIRED)
endif()
set(TACHYON_SYMBOLS_DIA_SOURCES dia/dia_errors.cpp dia/dia_provider.cpp dia/msvc_undecorate.cpp)
if(WIN32 AND TACHYON_ENABLE_DIA)
  tachyon_find_dia_sdk()
  list(APPEND TACHYON_SYMBOLS_DIA_SOURCES dia/dia_com_session.cpp)
endif()
tachyon_add_library(symbols
  SOURCES ${TACHYON_SYMBOLS_DIA_SOURCES} demangle.cpp function_key.cpp http_client.cpp json_symbol_provider.cpp range_table.cpp
          symbol_keys.cpp symbol_locator.cpp symbol_provider.cpp symbol_service.cpp symbol_strings.cpp
          ${TACHYON_SYMBOLS_HTTP_SOURCES}
  PUBLIC_DEPS tachyon::core tachyon::trace tachyon::model Threads::Threads
  PRIVATE_DEPS nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(tachyon_symbols PRIVATE winhttp dbghelp)
  if(TACHYON_ENABLE_DIA)
    target_compile_definitions(tachyon_symbols PUBLIC TACHYON_HAVE_DIA=1)
    target_compile_definitions(tachyon_symbols PRIVATE TACHYON_DIA_DLL_PATH="${TACHYON_DIA_DLL}")
    target_include_directories(tachyon_symbols SYSTEM PRIVATE "${TACHYON_DIA_INCLUDE_DIR}")
    target_link_libraries(tachyon_symbols PRIVATE ole32 oleaut32)
  endif()
else()
  target_link_libraries(tachyon_symbols PRIVATE CURL::libcurl)
endif()
