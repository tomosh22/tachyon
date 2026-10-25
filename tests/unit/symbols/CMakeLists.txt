tachyon_add_test(symbols_tests
  SOURCES function_key_test.cpp json_provider_test.cpp symbol_service_test.cpp
  DEPS tachyon::symbols tachyon::model tachyon::trace nlohmann_json::nlohmann_json)
