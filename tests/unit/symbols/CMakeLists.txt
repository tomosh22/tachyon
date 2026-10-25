tachyon_add_test(symbols_tests
  SOURCES function_key_test.cpp lookup_caller_test.cpp json_provider_test.cpp symbol_service_test.cpp symbol_keys_test.cpp
          symbol_locator_test.cpp
  DEPS tachyon::symbols tachyon::model tachyon::trace nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(symbols_tests PRIVATE ws2_32)
endif()
