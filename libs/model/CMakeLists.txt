tachyon_add_library(model
  SOURCES model.cpp model_loader.cpp
  PUBLIC_DEPS tachyon::core tachyon::trace
  PRIVATE_DEPS nlohmann_json::nlohmann_json)
