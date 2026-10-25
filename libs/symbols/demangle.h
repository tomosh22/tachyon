// Demangler interface (WP-20). The defaults are pass-through, except that Itanium names are demangled with
// abi::__cxa_demangle when <cxxabi.h> is available. A real MSVC demangler (UnDecorateSymbolName) lands with
// the DIA provider.
#pragma once

#include <string>
#include <string_view>

namespace tachyon::symbols {

// Thread-safe: pure function. Pass-through for now; returns `mangled` unchanged.
std::string demangleMsvc(std::string_view mangled);

// Thread-safe: pure function. Demangles an Itanium-ABI name (e.g. "_ZN3Foo3barEv"); returns `mangled`
// unchanged when it is not a mangled name, cannot be demangled, or cxxabi is unavailable.
std::string demangleItanium(std::string_view mangled);

// Thread-safe: pure function. Picks the scheme by prefix: "?" is MSVC, "_Z" is Itanium, anything else is
// returned unchanged.
std::string demangle(std::string_view name);

}  // namespace tachyon::symbols
