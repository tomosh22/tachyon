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
};

// Thread-safe (const access to the context). `full` is analysis::functionKeyName; `module` is the part before
// '!' (empty for pseudo, scope and unmapped keys); `function` is the part after it, except that an unresolved
// frame (module!0x<rva>) keeps its whole name. A null context gives the hex value.
NameParts splitFunctionName(const analysis::QueryContext* context, const symbols::FunctionKey& key);

}  // namespace tachyon::viewer::common
