tachyon_add_test(model_tests
  SOURCES model_load_test.cpp model_robustness_test.cpp model_determinism_test.cpp
  DEPS tachyon::model tachyon::trace nlohmann_json::nlohmann_json)
