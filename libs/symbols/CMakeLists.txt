find_package(Threads REQUIRED)
tachyon_add_library(symbols
  SOURCES demangle.cpp function_key.cpp json_symbol_provider.cpp range_table.cpp symbol_locator.cpp
          symbol_provider.cpp symbol_service.cpp symbol_strings.cpp
  PUBLIC_DEPS tachyon::core tachyon::trace tachyon::model Threads::Threads
  PRIVATE_DEPS nlohmann_json::nlohmann_json)
