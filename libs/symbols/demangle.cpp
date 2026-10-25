#include "symbols/demangle.h"

#if __has_include(<cxxabi.h>)
#include <cxxabi.h>

#include <cstdlib>
#define TACHYON_HAVE_CXXABI 1
#endif

namespace tachyon::symbols {

std::string demangleMsvc(std::string_view mangled) {
  return std::string(mangled);
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

std::string demangle(std::string_view name) {
  if (!name.empty() && name[0] == '?') return demangleMsvc(name);
  if (name.size() >= 2 && name[0] == '_' && name[1] == 'Z') return demangleItanium(name);
  return std::string(name);
}

}  // namespace tachyon::symbols
