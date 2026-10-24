/* Compiles the public headers as C99 (extensions off). With TACHYON_ENABLED=1 it also links the static SDK
 * and exercises every entry point; with 0 it must build and run without linking the SDK at all. */
#include <stdio.h>

#include <tachyon/tachyon.h>
#include <tachyon/tachyon_loader.h>

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
  } while (0)

typedef char tachyon_test_functions_size[(sizeof(TachyonFunctions) == 16 * sizeof(void*)) ? 1 : -1];

int main(void) {
  CHECK(TACHYON_API_VERSION == 0x00010000);
  CHECK(TACHYON_MAKE_COLOR(1, 2, 3) == 0x010203FFu);
  CHECK(sizeof(TachyonSuppressTailCall) == 24);
#if TACHYON_ENABLED
  {
    TachyonFunctions fns;
    int x = 0;
    Tachyon_SetCurrentThreadName("main");
    Tachyon_SetCurrentThreadName_N("main", 4);
    Tachyon_BeginEvent("scope", "data", TACHYON_DEFAULT_COLOR);
    Tachyon_BeginEvent_N("scope2", 6, NULL, 0, TACHYON_MAKE_COLOR(255, 0, 0));
    (void)Tachyon_EndEvent();
    (void)Tachyon_EndEvent();
    Tachyon_RegisterFiber(1);
    Tachyon_BeginFiberSwitch(1, 2);
    Tachyon_EndFiberSwitch(2);
    Tachyon_UnregisterFiber(1);
    Tachyon_FrameMark("frame", 7);
    Tachyon_Marker("m", NULL, TACHYON_DEFAULT_COLOR);
    Tachyon_Plot("p", 1.5);
    Tachyon_LockAcquireBegin(&x, "lock");
    Tachyon_LockAcquireEnd(&x);
    CHECK(Tachyon_IsConnected() == 0);
    CHECK(Tachyon_RequestFlush("test") == 0);
    CHECK(Tachyon_GetAPI(TACHYON_API_VERSION, &fns) == 1);
    CHECK(fns.BeginEvent == &Tachyon_BeginEvent);
    CHECK(Tachyon_GetAPI(TACHYON_API_VERSION + 1, &fns) == 0);
    CHECK(fns.BeginEvent == 0 && fns.RequestFlush == 0);
  }
#else
  CHECK(TACHYON_ENABLED == 0);
#endif
  return 0;
}
