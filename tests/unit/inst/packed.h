// Test-only: reads a member of a packed trace record by value. gtest macros bind references to their
// arguments, which is undefined for members of #pragma pack(1) structs that are not naturally aligned
// (trace-format.md, Alignment); passing the member through pv() copies it first.
#pragma once

namespace tachyon::inst::testing_util {

template <typename T>
T pv(T value) {
  return value;
}

}  // namespace tachyon::inst::testing_util
