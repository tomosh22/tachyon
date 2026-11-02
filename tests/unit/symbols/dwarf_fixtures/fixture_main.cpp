// Fixture program for the DWARF provider tests. Built several ways (PIE, non-PIE, without debug info, split
// hot/cold with gcc). The tests locate lines through the "@marker" comments, so keep the markers on the lines
// they describe.
#include "fixture_inl.h"

#include <cstdio>
#include <cstdlib>

volatile int g_sink = 0;

namespace fx {

struct Widget {
  int work(int x);
  int member = 3;
};

}  // namespace fx

// Out-of-line member: DW_TAG_subprogram with DW_AT_specification into the class.
int fx::Widget::work(int x) {  // @widget_work
  return x * member + outerHelper(x);
}

namespace {
__attribute__((noinline)) int anonHelper(int x) {  // @anon_helper
  return x + outerHelper(x + 1);
}
}  // namespace

extern "C" __attribute__((noinline)) int hot_c(int x) {  // @hot_c
  int acc = 0;
  for (int i = 0; i < x; ++i) acc += i * 7;  // @hot_c_loop
  return acc;
}

extern "C" __attribute__((noinline)) int with_inlines(int x) {  // @with_inlines
  int a = outerHelper(x);                                       // @with_inlines_call
  int b = innerHelper(a);                                       // @with_inlines_call2
  return a + b;
}

__attribute__((noinline, cold)) int cold_fn(int x) {  // @cold_fn
  std::fprintf(stderr, "cold %d\n", x);
  return x - 1;
}

// With gcc -O2 -freorder-blocks-and-partition the unlikely branch moves to a .cold part: DW_AT_ranges with
// two pieces. (With clang it stays one range; the test accepts both.)
extern "C" __attribute__((noinline)) int split_fn(int x) {  // @split_fn
  if (__builtin_expect(x == 123456789, 0)) {
    g_sink = cold_fn(x);  // @split_cold_call
    std::abort();
  }
  return x * 11 + 1;  // @split_hot_ret
}

int main(int argc, char** argv) {
  int x = argc + (argv[0] != nullptr ? 1 : 0);
  fx::Widget w;
  g_sink = w.work(x) + anonHelper(x) + hot_c(x) + with_inlines(x) + split_fn(x);
  if (argc > 5) g_sink += cold_fn(x);
  return g_sink == 0x7fffffff ? 1 : 0;
}
