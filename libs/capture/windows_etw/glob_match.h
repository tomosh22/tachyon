// Platform-independent glob matcher for child_filter (WP-13): only '*' (any run of characters, including
// none) and '?' (exactly one character) are special. Everything else, including '[', is literal.
#pragma once

#include <string_view>

namespace tachyon::capture::etw {

// Thread-safe: pure function. True when `text` matches `pattern` in full. Matching is byte-wise; with
// `caseInsensitive` ASCII letters compare equal regardless of case. An empty pattern matches only empty text.
bool globMatch(std::string_view pattern, std::string_view text, bool caseInsensitive);

}  // namespace tachyon::capture::etw
