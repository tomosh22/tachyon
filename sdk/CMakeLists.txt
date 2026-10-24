# Public SDK: static (tachyon_sdk) and shared (tachyon_sdk_shared) libraries exporting the same C API.
set(TACHYON_SDK_SOURCES src/sdk_impl.cpp)

add_library(tachyon_sdk STATIC ${TACHYON_SDK_SOURCES})
add_library(tachyon::sdk ALIAS tachyon_sdk)

add_library(tachyon_sdk_shared SHARED ${TACHYON_SDK_SOURCES})
add_library(tachyon::sdk_shared ALIAS tachyon_sdk_shared)
target_compile_definitions(tachyon_sdk_shared PRIVATE TACHYON_SDK_BUILD_SHARED)
set_target_properties(tachyon_sdk_shared PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

foreach(t tachyon_sdk tachyon_sdk_shared)
  target_include_directories(${t} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_features(${t} PRIVATE cxx_std_17)
  tachyon_set_warnings(${t})
endforeach()
# Windows consumers of the shared library need dllimport.
if(WIN32)
  target_compile_definitions(tachyon_sdk_shared INTERFACE TACHYON_SDK_USE_SHARED)
endif()
