#include "symbols/symbol_provider.h"

namespace tachyon::symbols {

const ResolvedAddress& unresolvedAddress() {
  static const ResolvedAddress kUnresolved{};
  return kUnresolved;
}

}  // namespace tachyon::symbols
