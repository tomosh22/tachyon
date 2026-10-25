#include "report.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>

#include "etw_types.h"
#include "wait_reason.h"

namespace tachyon::etlstat {

namespace {
std::string lowerBasename(const std::string& path) {
  size_t cut = path.find_last_of("/\\");
  std::string name = cut == std::string::npos ? path : path.substr(cut + 1);
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return name;
}

template <typename... Args>
void appendLine(std::string& out, fmt::format_string<Args...> format, Args&&... args) {
  out += fmt::format(format, std::forward<Args>(args)...);
  out += '\n';
}

double safeDiv(double a, double b) {
  return b > 0 ? a / b : 0.0;
}
}  // namespace

Report buildReport(const StatsSnapshot& s, const TraceFileInfo& info, const ReportOptions& o) {
  Report r;
  const double freq = static_cast<double>(info.perfFreq > 0 ? info.perfFreq : 10'000'000);
  r.sampleSeconds = s.sampledProfile > 0 ? static_cast<double>(s.lastSampleTs - s.firstSampleTs) / freq : 0.0;
  r.traceSeconds = s.totalEvents > 0 ? static_cast<double>(s.lastEventTs - s.firstEventTs) / freq : 0.0;

  double busySum = 0;
  for (size_t i = 0; i < s.cpus.size(); ++i) {
    const CpuStats& c = s.cpus[i];
    if (c.samples == 0) continue;
    CpuRate rate;
    rate.cpu = static_cast<uint32_t>(i);
    rate.samplesPerSecond = safeDiv(static_cast<double>(c.samples), r.sampleSeconds);
    rate.nonIdleFraction = safeDiv(static_cast<double>(c.nonIdleSamples), static_cast<double>(c.samples));
    rate.busy = rate.nonIdleFraction >= o.busyFraction;
    r.totalSamplesPerSecond += rate.samplesPerSecond;
    if (rate.busy) {
      r.minBusyRate =
          r.busyCpuCount == 0 ? rate.samplesPerSecond : std::min(r.minBusyRate, rate.samplesPerSecond);
      busySum += rate.samplesPerSecond;
      ++r.busyCpuCount;
    }
    r.cpus.push_back(rate);
  }
  r.meanBusyRate = r.busyCpuCount > 0 ? busySum / r.busyCpuCount : 0.0;

  const ParentStats& sp = s.parents[static_cast<size_t>(ParentKind::SampledProfile)];
  r.matchedFraction = safeDiv(static_cast<double>(sp.withStack), static_cast<double>(sp.total));
  r.matchedFractionNonIdle =
      safeDiv(static_cast<double>(s.sampledNonIdleWithStack), static_cast<double>(s.sampledNonIdle));

  if (o.pid || !o.image.empty()) {
    r.hasProcessFilter = true;
    std::set<uint32_t> pids;
    if (o.pid) pids.insert(*o.pid);
    const std::string wanted = lowerBasename(o.image);
    if (!wanted.empty()) {
      for (const auto& [pid, name] : s.processImage) {
        if (lowerBasename(name) == wanted) pids.insert(pid);
      }
    }
    r.filterPids.assign(pids.begin(), pids.end());
    for (const auto& [tid, count] : s.samplesPerThread) {
      auto it = s.threadToProcess.find(tid);
      if (it == s.threadToProcess.end() || !pids.count(it->second)) continue;
      r.processSamples += count;
      r.topThreads.emplace_back(tid, safeDiv(static_cast<double>(count), r.sampleSeconds));
    }
    std::sort(r.topThreads.begin(), r.topThreads.end(), [](const auto& a, const auto& b) {
      return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (r.topThreads.size() > 8) r.topThreads.resize(8);
    r.processSamplesPerSecond = safeDiv(static_cast<double>(r.processSamples), r.sampleSeconds);
  }

  r.rateAcceptance = r.busyCpuCount > 0 && r.minBusyRate >= o.minRatePerBusyCore;
  r.matchAcceptance = r.matchedFraction > o.minMatchedFraction;
  return r;
}

std::string formatReportText(const StatsSnapshot& s, const TraceFileInfo& info, const Report& r,
                             const ReportOptions& o) {
  std::string out;
#define LINE(...) appendLine(out, __VA_ARGS__)
  LINE("== etlstat report ==");
  if (info.valid) {
    LINE("logfile: perfFreq={} Hz, processors={}, buffersWritten={}", info.perfFreq, info.numberOfProcessors,
         info.buffersWritten);
    LINE("EventsLost={} BuffersLost={}", info.eventsLost, info.buffersLost);
  } else {
    LINE("logfile header: n/a (replay); assuming perfFreq={} Hz", info.perfFreq);
  }
  LINE("events: {} total, {} malformed payloads; span {:.3f} s", s.totalEvents, s.malformedEvents,
       r.traceSeconds);

  LINE("");
  LINE("-- events per (provider, opcode, version) --");
  LINE("{:<12} {:>6} {:<16} {:>3} {:>10} {:>8} {:>8}", "provider", "opcode", "name", "ver", "count",
       "minSize", "maxSize");
  for (const auto& [key, ks] : s.keys) {
    const std::string_view name = opcodeName(key.provider, key.opcode);
    LINE("{:<12} {:>6} {:<16} {:>3} {:>10} {:>8} {:>8}", providerName(key.provider), key.opcode,
         name.empty() ? "-" : std::string(name), key.version, ks.count,
         ks.minSize == 0xFFFFFFFFu ? 0u : ks.minSize, ks.maxSize);
  }

  LINE("");
  LINE("-- stack matching (key: EventTimeStamp, StackThread) --");
  LINE("StackWalk/Stack events: {}  matched: {}  unmatched: {}", s.stackEvents, s.stackMatched,
       s.stackUnmatched);
  LINE("max stack depth: {} frames", s.maxStackDepth);
  for (size_t k = 0; k < 3; ++k) {
    const ParentStats& p = s.parents[k];
    LINE("{:<14} total={} withStack={} withoutStack={} maxDepth={} meanDepth={:.1f}",
         parentKindName(static_cast<ParentKind>(k)), p.total, p.withStack, p.withoutStack, p.maxDepth,
         safeDiv(static_cast<double>(p.depthSum), static_cast<double>(p.withStack)));
  }
  for (size_t k = 0; k < 3; ++k) {
    LINE("matched via {}: {}", matchRoleName(static_cast<MatchRole>(k)), s.stackRoleCount[k]);
  }

  LINE("");
  LINE("-- sampling --");
  LINE("SampledProfile: {} total, {} non-idle over {:.3f} s (expected {} Hz per CPU)", s.sampledProfile,
       s.sampledNonIdle, r.sampleSeconds, o.expectedHz);
  LINE("samples with matched stack: {:.2f}% (non-idle only: {:.2f}%)", r.matchedFraction * 100.0,
       r.matchedFractionNonIdle * 100.0);
  LINE("{:>4} {:>14} {:>9} {}", "cpu", "samples/s", "non-idle", "busy");
  for (const CpuRate& c : r.cpus) {
    LINE("{:>4} {:>14.1f} {:>8.1f}% {}", c.cpu, c.samplesPerSecond, c.nonIdleFraction * 100.0,
         c.busy ? "yes" : "");
  }
  LINE("busy CPUs: {}  min busy rate: {:.1f}/s  mean busy rate: {:.1f}/s  all-CPU total: {:.1f}/s",
       r.busyCpuCount, r.minBusyRate, r.meanBusyRate, r.totalSamplesPerSecond);

  if (r.hasProcessFilter) {
    LINE("");
    LINE("-- process filter --");
    std::string pids;
    for (uint32_t pid : r.filterPids) pids += (pids.empty() ? "" : ",") + std::to_string(pid);
    LINE("pids: {}",
         pids.empty() ? "(none resolved; thread->process map comes from Thread Start/DCStart)" : pids);
    LINE("samples: {} ({:.1f}/s)", r.processSamples, r.processSamplesPerSecond);
    for (const auto& [tid, rate] : r.topThreads) LINE("  thread {:>7}: {:.1f}/s", tid, rate);
  }

  LINE("");
  LINE("-- thread names --");
  LINE("Thread/SetName (opcode 72) events: {} -> {}", s.setNameEvents,
       s.setNameEvents > 0 ? "PRESENT" : "ABSENT");
  for (const ThreadNameEvent& n : s.threadNames) LINE("  pid {} tid {}: {}", n.processId, n.threadId, n.name);

  LINE("");
  LINE("-- acceptance (informational) --");
  LINE("rate per busy core >= {:.0f}/s: {}", o.minRatePerBusyCore, r.rateAcceptance ? "PASS" : "FAIL");
  LINE("matched stacks > {:.0f}%: {}", o.minMatchedFraction * 100.0, r.matchAcceptance ? "PASS" : "FAIL");
  LINE("lost events: {}",
       info.valid ? (info.eventsLost == 0 && info.buffersLost == 0 ? "none" : "SOME") : "n/a");
  return out;
#undef LINE
}

std::string formatReportJson(const StatsSnapshot& s, const TraceFileInfo& info, const Report& r,
                             const ReportOptions& o) {
  using nlohmann::json;
  json j;
  j["logfile"] = {{"valid", info.valid},
                  {"perfFreq", info.perfFreq},
                  {"eventsLost", info.eventsLost},
                  {"buffersLost", info.buffersLost},
                  {"buffersWritten", info.buffersWritten},
                  {"numberOfProcessors", info.numberOfProcessors},
                  {"pointerSize", info.pointerSize}};
  j["totalEvents"] = s.totalEvents;
  j["malformedEvents"] = s.malformedEvents;
  j["traceSeconds"] = r.traceSeconds;
  json keys = json::array();
  for (const auto& [key, ks] : s.keys) {
    keys.push_back({{"provider", guidToString(key.provider)},
                    {"providerName", providerName(key.provider)},
                    {"opcode", key.opcode},
                    {"opcodeName", std::string(opcodeName(key.provider, key.opcode))},
                    {"version", key.version},
                    {"count", ks.count},
                    {"minSize", ks.minSize == 0xFFFFFFFFu ? 0u : ks.minSize},
                    {"maxSize", ks.maxSize}});
  }
  j["events"] = keys;
  j["stacks"] = {{"events", s.stackEvents},
                 {"matched", s.stackMatched},
                 {"unmatched", s.stackUnmatched},
                 {"maxDepth", s.maxStackDepth},
                 {"matchedViaHeaderThread", s.stackRoleCount[0]},
                 {"matchedViaPayloadThread", s.stackRoleCount[1]},
                 {"matchedViaPayloadOldThread", s.stackRoleCount[2]}};
  json parents = json::object();
  for (size_t k = 0; k < 3; ++k) {
    const ParentStats& p = s.parents[k];
    parents[parentKindName(static_cast<ParentKind>(k))] = {{"total", p.total},
                                                           {"withStack", p.withStack},
                                                           {"withoutStack", p.withoutStack},
                                                           {"maxDepth", p.maxDepth}};
  }
  j["parents"] = parents;
  json cpus = json::array();
  for (const CpuRate& c : r.cpus) {
    cpus.push_back({{"cpu", c.cpu},
                    {"samplesPerSecond", c.samplesPerSecond},
                    {"nonIdleFraction", c.nonIdleFraction},
                    {"busy", c.busy}});
  }
  j["sampling"] = {{"expectedHz", o.expectedHz},
                   {"sampledProfile", s.sampledProfile},
                   {"sampledNonIdle", s.sampledNonIdle},
                   {"sampleSeconds", r.sampleSeconds},
                   {"cpus", cpus},
                   {"busyCpuCount", r.busyCpuCount},
                   {"minBusyRate", r.minBusyRate},
                   {"meanBusyRate", r.meanBusyRate},
                   {"totalSamplesPerSecond", r.totalSamplesPerSecond},
                   {"matchedFraction", r.matchedFraction},
                   {"matchedFractionNonIdle", r.matchedFractionNonIdle}};
  if (r.hasProcessFilter) {
    json threads = json::array();
    for (const auto& [tid, rate] : r.topThreads)
      threads.push_back({{"thread", tid}, {"samplesPerSecond", rate}});
    j["process"] = {{"pids", r.filterPids},
                    {"samples", r.processSamples},
                    {"samplesPerSecond", r.processSamplesPerSecond},
                    {"topThreads", threads}};
  }
  json names = json::array();
  for (const ThreadNameEvent& n : s.threadNames) {
    names.push_back({{"pid", n.processId}, {"tid", n.threadId}, {"name", n.name}});
  }
  j["threadNames"] = {
      {"setNameEvents", s.setNameEvents}, {"present", s.setNameEvents > 0}, {"examples", names}};
  j["acceptance"] = {{"ratePerBusyCore", r.rateAcceptance}, {"matchedStacks", r.matchAcceptance}};
  return j.dump(2);
}

}  // namespace tachyon::etlstat
