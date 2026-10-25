# Third-party dependencies, pinned to the versions approved in IMPLEMENTATION_PLAN.md Part 2.3.
# Core dependencies are fetched eagerly; heavier ones are fetched on demand through tachyon_require_<dep>().
include(FetchContent)
set(FETCHCONTENT_QUIET ON)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# Third-party targets must never inherit the project's -Werror.
function(tachyon_silence_third_party)
  foreach(tgt IN LISTS ARGN)
    if(TARGET ${tgt})
      get_target_property(_aliased ${tgt} ALIASED_TARGET)
      if(_aliased)
        set(tgt ${_aliased})
      endif()
      get_target_property(_type ${tgt} TYPE)
      if(NOT _type STREQUAL "INTERFACE_LIBRARY")
        set_target_properties(${tgt} PROPERTIES COMPILE_WARNING_AS_ERROR OFF CXX_CLANG_TIDY "")
      endif()
    endif()
  endforeach()
endfunction()

# --- fmt 10.x ----------------------------------------------------------------
FetchContent_Declare(fmt
  URL https://github.com/fmtlib/fmt/releases/download/10.2.1/fmt-10.2.1.zip
  URL_HASH SHA256=312151a2d13c8327f5c9c586ac6cf7cddc1658e8f53edae0ec56509c8fa516c9
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)

# --- nlohmann/json 3.11.x -----------------------------------------------------
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

# --- zstd 1.5.x ---------------------------------------------------------------
FetchContent_Declare(zstd
  URL https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
  URL_HASH SHA256=8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1
  SOURCE_SUBDIR build/cmake
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)

# --- xxHash 0.8.x (header-only use via XXH_INLINE_ALL) -------------------------
FetchContent_Declare(xxhash
  GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
  GIT_TAG bbb27a5efb85b92a0486cf361a8635715a53f6ba # v0.8.2
  GIT_SHALLOW FALSE)

# --- GoogleTest 1.14+ ---------------------------------------------------------
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(fmt nlohmann_json zstd)
tachyon_silence_third_party(fmt libzstd_static)

FetchContent_GetProperties(xxhash)
if(NOT xxhash_POPULATED)
  FetchContent_Populate(xxhash)
endif()
add_library(tachyon_xxhash INTERFACE)
target_include_directories(tachyon_xxhash SYSTEM INTERFACE ${xxhash_SOURCE_DIR})
add_library(tachyon::xxhash ALIAS tachyon_xxhash)

add_library(tachyon_zstd INTERFACE)
target_link_libraries(tachyon_zstd INTERFACE libzstd_static)
target_include_directories(tachyon_zstd SYSTEM INTERFACE ${zstd_SOURCE_DIR}/lib)
add_library(tachyon::zstd ALIAS tachyon_zstd)

if(TACHYON_BUILD_TESTS)
  FetchContent_MakeAvailable(googletest)
  tachyon_silence_third_party(gtest gtest_main gmock gmock_main)
endif()

# --- On-demand dependencies -----------------------------------------------------
# Zydis 4.x (amalgamated build, MIT). Call from libs/disasm (WP-34).
function(tachyon_require_zydis)
  if(TARGET tachyon::zydis)
    return()
  endif()
  FetchContent_Declare(zydis
    URL https://github.com/zyantific/zydis/releases/download/v4.1.0/zydis-amalgamated.tar.gz
    URL_HASH SHA256=aa9b82be3a37a2998bd8e16cf583bbf2b6c3d80e97dc20504169dc32ca1ced59
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_GetProperties(zydis)
  if(NOT zydis_POPULATED)
    FetchContent_Populate(zydis)
  endif()
  file(GLOB_RECURSE _zydis_c "${zydis_SOURCE_DIR}/*.c")
  file(GLOB_RECURSE _zydis_h "${zydis_SOURCE_DIR}/Zydis.h")
  list(GET _zydis_h 0 _zydis_header)
  get_filename_component(_zydis_inc "${_zydis_header}" DIRECTORY)
  add_library(tachyon_zydis STATIC ${_zydis_c})
  target_include_directories(tachyon_zydis SYSTEM PUBLIC ${_zydis_inc})
  target_compile_definitions(tachyon_zydis PUBLIC ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD)
  set_target_properties(tachyon_zydis PROPERTIES COMPILE_WARNING_AS_ERROR OFF POSITION_INDEPENDENT_CODE ON)
  add_library(tachyon::zydis ALIAS tachyon_zydis)
endfunction()

# SDL2 2.30.x: system package on Linux (libsdl2-dev), fetched from source elsewhere. Call from apps/viewer (WP-40).
function(tachyon_require_sdl2)
  if(TARGET tachyon::sdl2)
    return()
  endif()
  if(NOT WIN32)
    find_package(SDL2 2.30 CONFIG QUIET)
  endif()
  if(NOT SDL2_FOUND)
    FetchContent_Declare(sdl2
      URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
      URL_HASH SHA256=24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(sdl2)
  endif()
  add_library(tachyon_sdl2 INTERFACE)
  if(TARGET SDL2::SDL2)
    target_link_libraries(tachyon_sdl2 INTERFACE SDL2::SDL2)
  else()
    target_link_libraries(tachyon_sdl2 INTERFACE SDL2::SDL2-static)
  endif()
  if(TARGET SDL2::SDL2main)
    target_link_libraries(tachyon_sdl2 INTERFACE SDL2::SDL2main)
  endif()
  add_library(tachyon::sdl2 ALIAS tachyon_sdl2)
endfunction()

# Dear ImGui 1.91.x docking branch. Provides tachyon::imgui with the SDL2 + OpenGL3 backends (WP-40).
function(tachyon_require_imgui)
  if(TARGET tachyon::imgui)
    return()
  endif()
  tachyon_require_sdl2()
  find_package(OpenGL REQUIRED)
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG 368123ab06b2b573d585e52f84cd782c5c006697 # v1.91.5-docking
    GIT_SHALLOW FALSE)
  FetchContent_GetProperties(imgui)
  if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
  endif()
  add_library(tachyon_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(tachyon_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(tachyon_imgui PUBLIC tachyon::sdl2 OpenGL::GL)
  set_target_properties(tachyon_imgui PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
  add_library(tachyon::imgui ALIAS tachyon_imgui)
endfunction()

# --- DIA SDK (Windows only, ships with Visual Studio 2022) ------------------------------------------------------
# Finds <VS>/DIA SDK via, in order: -DTACHYON_DIA_SDK_DIR=..., $ENV{VSINSTALLDIR} (set by vcvars / msvc-dev-cmd),
# vswhere. Sets TACHYON_DIA_INCLUDE_DIR (dia2.h) and TACHYON_DIA_DLL (bin/amd64/msdia140.dll) and fails the
# configure with a clear message when either is missing. Nothing from the SDK is committed; msdia140.dll is
# copied next to the test executables at build time by tachyon_copy_dia_dll().
function(tachyon_find_dia_sdk)
  if(TACHYON_DIA_DLL AND TACHYON_DIA_INCLUDE_DIR)
    return()
  endif()
  set(_roots "")
  if(TACHYON_DIA_SDK_DIR)
    list(APPEND _roots "${TACHYON_DIA_SDK_DIR}")
  endif()
  if(DEFINED ENV{VSINSTALLDIR})
    file(TO_CMAKE_PATH "$ENV{VSINSTALLDIR}" _vsdir)
    list(APPEND _roots "${_vsdir}/DIA SDK")
  endif()
  set(_pf86 "$ENV{ProgramFiles\(x86\)}")
  if(_pf86 AND EXISTS "${_pf86}/Microsoft Visual Studio/Installer/vswhere.exe")
    execute_process(
      COMMAND "${_pf86}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -property installationPath
      OUTPUT_VARIABLE _vswhere_out OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_vswhere_out)
      file(TO_CMAKE_PATH "${_vswhere_out}" _vswhere_out)
      list(APPEND _roots "${_vswhere_out}/DIA SDK")
    endif()
  endif()
  foreach(_root IN LISTS _roots)
    if(EXISTS "${_root}/include/dia2.h" AND EXISTS "${_root}/bin/amd64/msdia140.dll")
      set(TACHYON_DIA_INCLUDE_DIR "${_root}/include" CACHE INTERNAL "DIA SDK include directory")
      set(TACHYON_DIA_DLL "${_root}/bin/amd64/msdia140.dll" CACHE INTERNAL "msdia140.dll (x64)")
      message(STATUS "DIA SDK: ${_root}")
      return()
    endif()
  endforeach()
  message(FATAL_ERROR
    "TACHYON_ENABLE_DIA is ON but the DIA SDK was not found. Install the 'C++ ATL/MSVC' components of Visual "
    "Studio 2022 (they include 'DIA SDK'), run from a VS developer prompt (VSINSTALLDIR), or pass "
    "-DTACHYON_DIA_SDK_DIR=<VS>/DIA SDK, or configure with -DTACHYON_ENABLE_DIA=OFF. Searched: ${_roots}")
endfunction()

# tachyon_copy_dia_dll(<target>): copies msdia140.dll next to the target's executable after each build.
function(tachyon_copy_dia_dll target)
  if(NOT WIN32 OR NOT TACHYON_ENABLE_DIA)
    return()
  endif()
  tachyon_find_dia_sdk()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TACHYON_DIA_DLL}" "$<TARGET_FILE_DIR:${target}>"
    VERBATIM)
endfunction()

# --- elfutils libdw / libelf (Linux only, LGPL, dynamically linked) ------------------------------------------
# Used by the DWARF symbol provider (libs/symbols/dwarf). Discovery through pkg-config only.
if(TACHYON_ENABLE_LIBDW)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(TACHYON_LIBDW REQUIRED IMPORTED_TARGET GLOBAL libdw libelf)
endif()
