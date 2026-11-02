#include "viewer/status/capture_warnings.h"

#include <nlohmann/json.hpp>

#include <string_view>

namespace tachyon::viewer {

namespace {

using json = nlohmann::json;

constexpr std::string_view kDroppedMarkerName = "tachyon.dropped";  // libs/inst/decoder.h

uint64_t asCount(const json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number()) return 0;
  if (it->is_number_unsigned()) return it->get<uint64_t>();
  const double v = it->get<double>();
  return v > 0 ? static_cast<uint64_t>(v) : 0;
}

void addStrings(const json& array, CaptureWarningKind kind, const std::string& prefix,
                std::vector<CaptureWarning>& out) {
  if (!array.is_array()) return;
  for (const json& w : array) {
    if (w.is_string()) out.push_back({kind, prefix + w.get<std::string>()});
  }
}

}  // namespace

std::vector<CaptureWarning> collectCaptureWarnings(const model::TraceModel& model) {
  std::vector<CaptureWarning> out;
  if (model.isPartial) {
    out.push_back({CaptureWarningKind::Partial,
                   "This trace is partial (the capture did not finish writing it): data after the last "
                   "complete chunk is missing."});
  }

  json info;
  if (!model.captureInfo.raw_json.empty()) info = json::parse(model.captureInfo.raw_json, nullptr, false);
  if (info.is_discarded() || !info.is_object()) info = json::object();

  uint64_t lost = model.captureInfo.events_lost;
  if (lost == 0) lost = asCount(info, "events_lost");
  if (lost > 0) {
    out.push_back({CaptureWarningKind::EventsLost,
                   std::to_string(lost) + " events were lost during capture; samples, stacks or context "
                                          "switches may be missing."});
  }
  if (const auto host = info.find("host"); host != info.end() && host->is_object()) {
    if (const uint64_t buffers = asCount(*host, "etl_buffers_lost"); buffers > 0) {
      out.push_back({CaptureWarningKind::EventsLost,
                     std::to_string(buffers) + " ETW buffers were lost during capture."});
    }
  }
  if (const auto throttle = info.find("throttle"); throttle != info.end() && throttle->is_object()) {
    const uint64_t events = asCount(*throttle, "events");
    if (events > 0) {
      std::string text = "The kernel throttled sampling " + std::to_string(events) + " time(s) (" +
                         std::to_string(asCount(*throttle, "throttled_ms")) + " ms";
      if (const uint64_t missed = asCount(*throttle, "estimated_missed_samples"); missed > 0)
        text += ", about " + std::to_string(missed) + " samples missed";
      text += ").";
      out.push_back({CaptureWarningKind::Throttled, std::move(text)});
    }
  }

  // Dropped instrumentation records: the merge reports and the markers the decoder inserted.
  uint64_t dropped = 0;
  std::vector<CaptureWarning> mergeWarnings;
  if (const auto inst = info.find("instrumentation"); inst != info.end() && inst->is_object()) {
    if (const auto merges = inst->find("merges"); merges != inst->end() && merges->is_array()) {
      for (const json& merge : *merges) {
        if (!merge.is_object()) continue;
        dropped += asCount(merge, "dropped_records");
        if (const auto w = merge.find("warnings"); w != merge.end())
          addStrings(*w, CaptureWarningKind::InstrumentationWarning, "Instrumentation: ", mergeWarnings);
      }
    }
  }
  uint64_t droppedMarkers = 0;
  for (const model::Marker& m : model.markers) {
    if (model.strings.get(m.name) == kDroppedMarkerName) ++droppedMarkers;
  }
  if (dropped > 0 || droppedMarkers > 0) {
    std::string text = "Instrumentation records were dropped";
    if (dropped > 0) text += " (" + std::to_string(dropped) + " records)";
    if (droppedMarkers > 0)
      text += "; " + std::to_string(droppedMarkers) + " tachyon.dropped marker(s) show where";
    text += ".";
    out.push_back({CaptureWarningKind::DroppedInstrumentation, std::move(text)});
  }
  for (CaptureWarning& w : mergeWarnings) out.push_back(std::move(w));

  if (const auto stats = info.find("convert_stats"); stats != info.end() && stats->is_object()) {
    if (const auto w = stats->find("warnings"); w != stats->end())
      addStrings(*w, CaptureWarningKind::ConverterWarning, "", out);
  }
  if (const auto w = info.find("warnings"); w != info.end())
    addStrings(*w, CaptureWarningKind::ConverterWarning, "", out);

  if (out.size() > kMaxCaptureWarnings) {
    const size_t extra = out.size() - kMaxCaptureWarnings;
    out.resize(kMaxCaptureWarnings);
    out.push_back({CaptureWarningKind::ConverterWarning, "... and " + std::to_string(extra) + " more."});
  }
  return out;
}

}  // namespace tachyon::viewer
