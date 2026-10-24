/*
 * Header-only runtime loader for the Tachyon SDK shared library. Compiles as C99 and C++17.
 *
 *   TachyonFunctions fns;
 *   if (Tachyon_LoadFrom("libtachyon_sdk_shared.so", &fns)) fns.BeginEvent("x", NULL, TACHYON_DEFAULT_COLOR);
 *
 * Returns 1 on success. On any failure (missing file, missing symbol, version mismatch) returns 0 and leaves
 * `out` zero-filled. On success the library is intentionally never unloaded: the returned pointers must stay
 * valid. Thread safety: safe to call concurrently (each call fills its own `out`).
 */
#ifndef TACHYON_SDK_TACHYON_LOADER_H
#define TACHYON_SDK_TACHYON_LOADER_H

#include <string.h>

#include "tachyon.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static inline int Tachyon_LoadFrom(const char* path, TachyonFunctions* out) {
  Tachyon_GetAPI_Func getApi = 0;
  if (out == 0) return 0;
  memset(out, 0, sizeof(*out));
  if (path == 0) return 0;
#if defined(_WIN32)
  {
    HMODULE lib = LoadLibraryA(path);
    FARPROC sym;
    if (lib == 0) return 0;
    sym = GetProcAddress(lib, "Tachyon_GetAPI");
    if (sym == 0) {
      FreeLibrary(lib);
      return 0;
    }
    memcpy(&getApi, &sym, sizeof(getApi));
    if (getApi(TACHYON_API_VERSION, out) != 1) {
      memset(out, 0, sizeof(*out));
      FreeLibrary(lib);
      return 0;
    }
  }
#else
  {
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    void* sym;
    if (lib == 0) return 0;
    sym = dlsym(lib, "Tachyon_GetAPI");
    if (sym == 0) {
      dlclose(lib);
      return 0;
    }
    memcpy(&getApi, &sym, sizeof(getApi));
    if (getApi(TACHYON_API_VERSION, out) != 1) {
      memset(out, 0, sizeof(*out));
      dlclose(lib);
      return 0;
    }
  }
#endif
  return 1;
}

#endif /* TACHYON_SDK_TACHYON_LOADER_H */
