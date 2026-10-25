// MSVC name undecoration (WP-22): UnDecorateSymbolName on Windows, pass-through elsewhere.
#pragma once

#include <string>
#include <string_view>

namespace tachyon::symbols {

// Thread-safe (serialized internally: DbgHelp is single-threaded). Undecorates a "?..." MSVC name to its
// qualified name without parameters or return type ("?bar@Foo@ns@@QEAAXXZ" -> "ns::Foo::bar"). Names that do
// not start with '?', names that fail to undecorate, and every name on non-Windows builds are returned
// unchanged.
std::string undecorateMsvc(std::string_view name);

}  // namespace tachyon::symbols
