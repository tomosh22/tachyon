// Display names of FunctionKeys, split the way the analysis tables show them (extracted from the Call Graph
// by WP-44).
#pragma once

#include "analysis/query.h"
#include "symbols/function_key.h"

#include <string>

namespace tachyon::viewer::common {

struct NameParts {
  std::string function, module, full;
  bool inlined = false;
  bool scope = false;  // an instrumentation scope ({kScopeModule, id}); shown with the scope icon (WP-54)
};

// Thread-safe (const access to the context). `full` is analysis::functionKeyName; `module` is the part before
// '!' (empty for pseudo, scope and unmapped keys); `function` is the part after it, except that an unresolved
// frame (module!0x<rva>) keeps its whole name and a scope shows its id without the "scope:" prefix. A null
// context gives the hex value.
//
// kRootKey is the Call Graph's "<root>". In the function tables (Function List, Butterfly self) the same key
// is the frameless samples: pass `rootIsNoStack` to name it "<no stack>" there (WP-33b).
NameParts splitFunctionName(const analysis::QueryContext* context, const symbols::FunctionKey& key,
                            bool rootIsNoStack = false);

}  // namespace tachyon::viewer::common
