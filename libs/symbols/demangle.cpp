#include "symbols/demangle.h"

#include "symbols/dia/msvc_undecorate.h"

#if __has_include(<cxxabi.h>)
#include <cxxabi.h>

#include <cstdlib>
#define TACHYON_HAVE_CXXABI 1
#endif

namespace tachyon::symbols {

std::string demangleMsvc(std::string_view mangled) {
  return undecorateMsvc(mangled);  // DbgHelp on Windows, pass-through elsewhere
}

std::string demangleItanium(std::string_view mangled) {
#ifdef TACHYON_HAVE_CXXABI
  if (mangled.size() >= 2 && mangled[0] == '_' && mangled[1] == 'Z') {
    const std::string copy(mangled);
    int status = 0;
    char* out = abi::__cxa_demangle(copy.c_str(), nullptr, nullptr, &status);
    if (out != nullptr) {
      std::string result = status == 0 ? std::string(out) : copy;
      std::free(out);
      return result;
    }
  }
#endif
  return std::string(mangled);
}

namespace {
bool endsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
}  // namespace

std::string stripParameterList(std::string_view name) {
  std::string_view s = name;
  const auto trim = [&] {
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  };
  trim();
  // " [clone .cold]" / " [clone .isra.0]" suffixes (possibly several).
  while (endsWith(s, "]")) {
    const size_t open = s.rfind(" [clone ");
    if (open == std::string_view::npos) break;
    s = s.substr(0, open);
    trim();
  }
  // Trailing member qualifiers.
  for (bool again = true; again;) {
    again = false;
    for (const std::string_view q : {" const", " volatile", " noexcept", " &&", " &", "&&", "&"}) {
      if (endsWith(s, q) && s.size() > q.size()) {
        s.remove_suffix(q.size());
        trim();
        again = true;
      }
    }
  }
  if (s.empty() || s.back() != ')') return std::string(name);
  int depth = 0;
  for (size_t i = s.size(); i > 0; --i) {
    const char c = s[i - 1];
    if (c == ')') {
      ++depth;
    } else if (c == '(') {
      if (--depth == 0) {
        if (i == 1) return std::string(name);  // nothing before the parameter list
        return std::string(s.substr(0, i - 1));
      }
    }
  }
  return std::string(name);  // unbalanced
}

std::string demangle(std::string_view name) {
  if (!name.empty() && name[0] == '?') return demangleMsvc(name);
  if (name.size() >= 2 && name[0] == '_' && name[1] == 'Z') return demangleItanium(name);
  return std::string(name);
}

}  // namespace tachyon::symbols
