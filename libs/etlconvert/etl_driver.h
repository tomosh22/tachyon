// Internal: the Windows ProcessTrace driver behind EtlConverter::convert().
#pragma once

#include "core/platform.h"
#include "etlconvert/etl_converter.h"

namespace tachyon::etlconvert::detail {

#if TACHYON_PLATFORM_WINDOWS
// Not thread-safe per call. Runs the metadata pass and the main pass over `etl` and writes `out`.
Result<ConvertStats> convertEtlFile(const std::filesystem::path& etl, const std::filesystem::path& out,
                                    const ConvertOptions& options);
#endif

}  // namespace tachyon::etlconvert::detail
