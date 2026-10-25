#include "symbols/symbol_locator.h"

namespace tachyon::symbols {

Result<std::filesystem::path> SymbolLocator::locate(const Module& module) {
  return makeError(ErrorCode::NotFound, "symbol location is not implemented yet (WP-21): " + module.path);
}

}  // namespace tachyon::symbols
