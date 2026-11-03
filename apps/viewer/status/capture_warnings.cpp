#include "viewer/status/capture_warnings.h"

#include <nlohmann/json.hpp>

#include <cctype>
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

// Free-text warnings that only repeat a structured line (WP-67): the perf converter writes its lost-events
// and throttle sentences both as fields and as `convert_stats.warnings` text, and the agent hands the same
// text to `capture_done`.
bool containsNoCase(const std::string& text, std::string_view needle) {
  if (needle.size() > text.size()) return false;
  for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
    size_t j = 0;
    while (j < needle.size() && std::tolower(static_cast<unsigned char>(text[i + j])) ==
                                    std::tolower(static_cast<unsigned char>(needle[j])))
      ++j;
    if (j == needle.size()) return true;
  }
  return false;
}

bool saysEventsLost(const std::string& t) {
  return containsNoCase(t, "PERF_RECORD_LOST") || containsNoCase(t, "ring buffers overflowed") ||
         containsNoCase(t, "events were lost") || containsNoCase(t, "buffers were lost") ||
         containsNoCase(t, "samples/records were lost");
}
bool saysThrottled(const std::string& t) {
  return containsNoCase(t, "throttled");
}

struct Collector {
  std::vector<CaptureWarning> out;
  bool haveLost = false;
  bool haveThrottle = false;

  void addStructured(CaptureWarningKind kind, std::string text) {
    if (kind == CaptureWarningKind::EventsLost) haveLost = true;
    if (kind == CaptureWarningKind::Throttled) haveThrottle = true;
    out.push_back({kind, std::move(text)});
  }
  // A free-text warning; dropped when it repeats an earlier one or a structured line.
  void addText(CaptureWarningKind kind, std::string text) {
    if (haveLost && saysEventsLost(text)) return;
    if (haveThrottle && saysThrottled(text)) return;
    // Case-insensitive: the agent re-emits the trace's merge warnings as "instrumentation: ..." while the
    // trace's own copy reads "Instrumentation: ...".
    for (const CaptureWarning& w : out)
      if (w.text.size() == text.size() && containsNoCase(w.text, text)) return;
    out.push_back({kind, std::move(text)});
  }
  void addStrings(const json& array, CaptureWarningKind kind, const std::string& prefix) {
    if (!array.is_array()) return;
    for (const json& w : array) {
      if (w.is_string()) addText(kind, prefix + w.get<std::string>());
    }
  }
};

}  // namespace

std::vector<CaptureWarning> collectCaptureWarnings(const model::TraceModel& model,
                                                   const std::vector<std::string>& agentWarnings) {
  Collector c;
  std::vector<CaptureWarning>& out = c.out;
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
    c.addStructured(CaptureWarningKind::EventsLost,
                    std::to_string(lost) +
                        " events were lost during capture; samples, stacks or context "
                        "switches may be missing.");
  }
  if (const auto host = info.find("host"); host != info.end() && host->is_object()) {
    if (const uint64_t buffers = asCount(*host, "etl_buffers_lost"); buffers > 0) {
      c.addStructured(CaptureWarningKind::EventsLost,
                      std::to_string(buffers) + " ETW buffers were lost during capture.");
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
      c.addStructured(CaptureWarningKind::Throttled, std::move(text));
    }
  }

  // Dropped instrumentation records: the merge reports and the markers the decoder inserted.
  uint64_t dropped = 0;
  if (const auto inst = info.find("instrumentation"); inst != info.end() && inst->is_object()) {
    if (const auto merges = inst->find("merges"); merges != inst->end() && merges->is_array()) {
      for (const json& merge : *merges) {
        if (!merge.is_object()) continue;
        dropped += asCount(merge, "dropped_records");
        if (const auto w = merge.find("warnings"); w != merge.end())
          c.addStrings(*w, CaptureWarningKind::InstrumentationWarning, "Instrumentation: ");
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

  if (const auto stats = info.find("convert_stats"); stats != info.end() && stats->is_object()) {
    if (const auto w = stats->find("warnings"); w != stats->end())
      c.addStrings(*w, CaptureWarningKind::ConverterWarning, "");
  }
  if (const auto w = info.find("warnings"); w != info.end())
    c.addStrings(*w, CaptureWarningKind::ConverterWarning, "");
  for (const std::string& w : agentWarnings) c.addText(CaptureWarningKind::AgentWarning, w);

  if (out.size() > kMaxCaptureWarnings) {
    const size_t extra = out.size() - kMaxCaptureWarnings;
    out.resize(kMaxCaptureWarnings);
    out.push_back({CaptureWarningKind::ConverterWarning, "... and " + std::to_string(extra) + " more."});
  }
  return out;
}

}  // namespace tachyon::viewer
