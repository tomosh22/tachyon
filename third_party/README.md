# third_party

Dependencies are fetched by `cmake/dependencies.cmake` (FetchContent, pinned versions with SHA256 where a release archive exists) into `build/_deps`. Only the approved list in IMPLEMENTATION_PLAN.md Part 2.3 may be added, and only by the orchestrator. Vendored sources, if ever needed, go here.

Eager: fmt 10.2.1, nlohmann/json 3.11.3, zstd 1.5.6, xxHash 0.8.2, GoogleTest 1.15.2 (tests).
On demand: `tachyon_require_zydis()` (Zydis 4.1.0), `tachyon_require_sdl2()` (system SDL2 on Linux, 2.30.9 source elsewhere), `tachyon_require_imgui()` (Dear ImGui v1.91.5-docking + SDL2/OpenGL3 backends).
System: libdw/libelf (Linux, `TACHYON_ENABLE_LIBDW`), libcurl (Linux), WinHTTP and DIA (Windows).
