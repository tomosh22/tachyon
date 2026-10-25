#include "etlconvert/etl_converter.h"

#include "core/platform.h"
#include "etlconvert/etl_driver.h"
#include "etlconvert/hex_events.h"

namespace tachyon::etlconvert {

Result<ConvertStats> EtlConverter::convert(const std::filesystem::path& etl, const std::filesystem::path& out,
                                           const ConvertOptions& options) {
#if TACHYON_PLATFORM_WINDOWS
  return detail::convertEtlFile(etl, out, options);
#else
  (void)etl;
  (void)out;
  (void)options;
  return makeError(ErrorCode::Unsupported,
                   "EtlConverter::convert needs Windows (ProcessTrace); use convertHexEvents() for fixtures");
#endif
}

Result<ConvertStats> EtlConverter::convertHexEvents(const std::filesystem::path& hexEvents,
                                                    const std::filesystem::path& out,
                                                    const ConvertOptions& options, const EtlFileInfo& info) {
  TACHYON_TRY_ASSIGN(std::vector<HexEvent> events, loadHexEventFile(hexEvents));
  TACHYON_TRY_ASSIGN(std::unique_ptr<ConversionPipeline> pipeline,
                     ConversionPipeline::create(out, options, info));
  if (options.twoPass) {
    pipeline->beginPass(PipelinePass::Metadata);
    for (const HexEvent& e : events) pipeline->onEvent(e.view());
  }
  pipeline->beginPass(PipelinePass::Main);
  for (const HexEvent& e : events) pipeline->onEvent(e.view());
  pipeline->setFileInfo(info);
  return pipeline->finish();
}

}  // namespace tachyon::etlconvert
