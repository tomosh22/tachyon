// Compiles the public headers as C++17. With TACHYON_ENABLED=0 the macros must compile to nothing, so this is
// built without linking the SDK: any reference to a Tachyon_* function would fail to link.
#include <cstdio>

#include <tachyon/tachyon.h>
#include <tachyon/tachyon_loader.h>

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
      return 1;                                                              \
    }                                                                        \
  } while (0)

static_assert(sizeof(TachyonSuppressTailCall) == 24, "24-byte tail call suppressor");

static int work() {
  TACHYON_FUNCTION();
  TACHYON_SCOPE("a");
  TACHYON_SCOPE_DATA("b", "payload");
  TACHYON_SCOPE_COLOR("c", TACHYON_MAKE_COLOR(1, 2, 3));
  static int lock = 0;
  (void)lock;
  TACHYON_LOCK_SCOPE(lock, "lock");
  TACHYON_FRAME("frame", 1);
  return 3;
}

int main() {
#if TACHYON_ENABLED
  CHECK(TACHYON_ENABLED == 1);
#else
  CHECK(TACHYON_ENABLED == 0);
#endif
  CHECK(work() == 3);
  // Scope objects may be named directly in either configuration.
  tachyon::Scope s("direct");
  (void)s;
  return 0;
}
