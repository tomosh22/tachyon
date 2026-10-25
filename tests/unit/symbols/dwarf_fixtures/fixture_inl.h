// Header-only helpers for the DWARF provider fixtures. Both are always_inline so that every call site gets a
// DW_TAG_inlined_subroutine chain (outerHelper -> innerHelper) with call-site lines.
#pragma once

static inline __attribute__((always_inline)) int innerHelper(int x) {
  int y = x * 3 + 1;  // @inner_body
  return y ^ (x >> 2);
}

static inline __attribute__((always_inline)) int outerHelper(int x) {
  int z = innerHelper(x) + 7;  // @outer_calls_inner
  return z * 5;
}
