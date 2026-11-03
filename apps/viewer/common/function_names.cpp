#include "viewer/common/function_names.h"

#include <cstdio>

namespace tachyon::viewer::common {

NameParts splitFunctionName(const analysis::QueryContext* context, const symbols::FunctionKey& key,
                            bool rootIsNoStack) {
  NameParts parts;
  if (key == analysis::kRootKey) {
    parts.full = parts.function = rootIsNoStack ? "<no stack>" : "<root>";
  } else if (key == analysis::kKernelKey) {
    parts.full = parts.function = "<kernel>";
  } else if (!context) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(key.value));
    parts.full = parts.function = buf;
  } else {
    parts.full = analysis::functionKeyName(*context, key);
    parts.inlined = key.isInline();
    parts.scope = key.isScope();
    parts.function = parts.full;
    // A scope is "scope:<id>" in the analysis; the table shows the id next to the scope icon.
    if (parts.scope && parts.full.compare(0, 6, "scope:") == 0) parts.function = parts.full.substr(6);
    if (key.module.valid() && !key.isScope()) {
      const size_t bang = parts.full.find('!');
      if (bang != std::string::npos) {
        parts.module = parts.full.substr(0, bang);
        // An unresolved frame shows as module!0x<rva>: keep it whole in the Function column.
        const bool unresolved = parts.full.compare(bang + 1, 2, "0x") == 0;
        if (!unresolved) parts.function = parts.full.substr(bang + 1);
      }
    }
  }
  return parts;
}

}  // namespace tachyon::viewer::common
