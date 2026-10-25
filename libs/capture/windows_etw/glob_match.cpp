#include "capture/windows_etw/glob_match.h"

#include <cstddef>

namespace tachyon::capture::etw {

namespace {

char fold(char c, bool ci) {
  return ci && c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

// Iterative matcher with a single backtrack point (the last '*'): no recursion, so hostile patterns such as
// "*a*a*a*a*b" cannot blow the stack or go exponential.
bool globMatch(std::string_view pattern, std::string_view text, bool caseInsensitive) {
  size_t p = 0, t = 0;
  size_t starP = std::string_view::npos, starT = 0;
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      starP = p++;
      starT = t;
    } else if (p < pattern.size() &&
               (pattern[p] == '?' || fold(pattern[p], caseInsensitive) == fold(text[t], caseInsensitive))) {
      ++p;
      ++t;
    } else if (starP != std::string_view::npos) {
      p = starP + 1;
      t = ++starT;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

}  // namespace tachyon::capture::etw
