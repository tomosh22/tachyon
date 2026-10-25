#include "tally.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace tachyon::tracegen {

using json = nlohmann::json;

void Tally::addStack(uint32_t tid, const std::vector<FnKey>& rootToLeaf, bool useBlended, uint64_t ticks,
                     uint64_t samples) {
  if (rootToLeaf.empty()) return;
  ThreadTally& t = threads[tid];
  std::map<FnKey, FnStat>& overall = useBlended ? blended : fns;
  std::map<FnKey, FnStat>& perThread = useBlended ? t.blended : t.fns;
  for (size_t i = 0; i < rootToLeaf.size(); ++i) {
    bool seenAbove = false;  // the key already appeared closer to the root: count inclusive once
    for (size_t j = 0; j < i; ++j)
      if (rootToLeaf[j] == rootToLeaf[i]) {
        seenAbove = true;
        break;
      }
    if (seenAbove) continue;
    for (auto* m : {&overall, &perThread}) {
      FnStat& s = (*m)[rootToLeaf[i]];
      s.inclTicks += ticks;
      s.inclSamples += samples;
    }
  }
  for (auto* m : {&overall, &perThread}) {
    FnStat& s = (*m)[rootToLeaf.back()];
    s.exclTicks += ticks;
    s.exclSamples += samples;
  }
}

namespace {

const char* stateName(uint8_t s) {
  switch (s) {
    case kStateRunning: return "running";
    case kStateReady: return "ready";
    case kStateWaiting: return "waiting";
    default: return "unknown";
  }
}

json statJson(const FnStat& s) {
  return json{{"inclusive_ticks", s.inclTicks},
              {"exclusive_ticks", s.exclTicks},
              {"inclusive_samples", s.inclSamples},
              {"exclusive_samples", s.exclSamples}};
}

json keyJson(const FnKey& k, const std::map<FnKey, std::string>& names) {
  json j;
  switch (k.kind) {
    case 0: j["kind"] = "function"; break;
    case 1: j["kind"] = "unmapped"; break;
    default: j["kind"] = "scope"; break;
  }
  if (k.kind == 2) {
    j["id"] = k.scope;
  } else {
    j["module"] = k.kind == 1 ? 0xFFFFFFFFull : static_cast<uint64_t>(k.module);
    j["start_rva"] = k.startRva;
  }
  const auto it = names.find(k);
  j["name"] = it != names.end() ? it->second : (k.kind == 2 ? "scope:" + k.scope : std::string());
  return j;
}

json functionsJson(const std::map<FnKey, FnStat>& overall, const std::map<uint32_t, ThreadTally>& threads,
                   bool blended, const std::map<FnKey, std::string>& names) {
  json out = json::array();
  for (const auto& [key, stat] : overall) {
    json j = keyJson(key, names);
    j.update(statJson(stat));
    json per = json::array();
    for (const auto& [tid, t] : threads) {
      const auto& m = blended ? t.blended : t.fns;
      const auto it = m.find(key);
      if (it == m.end()) continue;
      json p = statJson(it->second);
      p["tid"] = tid;
      per.push_back(std::move(p));
    }
    j["per_thread"] = std::move(per);
    out.push_back(std::move(j));
  }
  return out;
}

json namesJson(const std::vector<std::string>& v) {
  json a = json::array();
  for (const auto& s : v) a.push_back(s);
  return a;
}

}  // namespace

json Tally::toJson() const {
  json j;
  j["schema"] = "tachyon-expected-1";
  j["scenario"] = scenario;
  j["seed"] = seed;
  j["generator"] = generator;
  j["clock_frequency_hz"] = clockHz;
  j["sampling_hz"] = samplingHz;
  j["sample_interval_ticks"] = sampleInterval;
  j["trace"] = json{{"start_ts", startTs}, {"end_ts", endTs}, {"partial", partial}};
  j["counts"] =
      json{{"samples", nSamples},          {"context_switches", nSwitches}, {"ready_events", nReady},
           {"scope_events", nScopeEvents}, {"fiber_events", nFiberEvents},  {"frame_marks", nFrameMarks},
           {"markers", nMarkers},          {"plot_samples", nPlots},        {"thread_names", nThreadNames}};
  json mods = json::array();
  for (size_t i = 0; i < modules.size(); ++i)
    mods.push_back(json{{"index", i}, {"name", modules[i].name}, {"base", modules[i].base}});
  j["modules"] = std::move(mods);
  j["functions"] = functionsJson(fns, threads, false, names);

  json ths = json::array();
  for (const auto& [tid, t] : threads) {
    json tj;
    tj["tid"] = tid;
    tj["name"] = t.name;
    tj["samples"] = t.samples;
    tj["zero_weight_samples"] = t.zeroWeight;
    tj["clipped_samples"] = t.clipped;
    tj["sample_ticks"] = t.sampleTicks;
    tj["state_ticks"] = json{{"running", t.stateTicks[kStateRunning]},
                             {"ready", t.stateTicks[kStateReady]},
                             {"waiting", t.stateTicks[kStateWaiting]},
                             {"unknown", t.stateTicks[kStateUnknown]}};
    tj["context_switches_out"] = t.switchesOut;
    tj["context_switches_in"] = t.switchesIn;
    tj["ready_events"] = t.readyEvents;
    if (detail) {
      json ivs = json::array();
      for (const IntervalLog& iv : t.intervals) {
        json ij{{"state", stateName(iv.state)}, {"start", iv.start}, {"end", iv.end}};
        if (iv.state == kStateWaiting || iv.state == kStateReady) {
          ij["blocking_stack"] = namesJson(iv.blocking);
          ij["unblocking_stack"] = namesJson(iv.unblocking);
          ij["woke_by_tid"] = iv.wokeBy;
        }
        ivs.push_back(std::move(ij));
      }
      tj["intervals"] = std::move(ivs);
    }
    ths.push_back(std::move(tj));
  }
  j["threads"] = std::move(ths);

  json scopes = json::array();
  for (const auto& [id, durs] : scopeDurations) {
    std::vector<uint64_t> d = durs;
    std::sort(d.begin(), d.end());
    uint64_t total = 0;
    for (uint64_t v : d) total += v;
    scopes.push_back(json{{"id", id},
                          {"instances", d.size()},
                          {"min_ticks", d.empty() ? 0 : d.front()},
                          {"median_ticks", d.empty() ? 0 : d[(d.size() - 1) / 2]},
                          {"max_ticks", d.empty() ? 0 : d.back()},
                          {"total_ticks", total}});
  }
  j["scopes"] = std::move(scopes);

  if (blend) {
    json b;
    b["functions"] = functionsJson(blended, threads, true, names);
    json pl = json::array();
    for (const auto& [key, p] : placements) {
      pl.push_back(json{{"scope", key.scope},
                        {"matched", key.matched},
                        {"parent", keyJson(key.parent, names)},
                        {"samples", p.samples},
                        {"ticks", p.ticks}});
    }
    b["placements"] = std::move(pl);
    j["blended"] = std::move(b);
  }

  json frs = json::array();
  std::vector<const FrameTally*> orderedFrames;
  for (const FrameTally& f : frames) orderedFrames.push_back(&f);
  std::sort(orderedFrames.begin(), orderedFrames.end(), [](const FrameTally* a, const FrameTally* b) {
    return std::tie(a->tid, a->name, a->number) < std::tie(b->tid, b->name, b->number);
  });
  for (const FrameTally* fp : orderedFrames) {
    const FrameTally& f = *fp;
    json th = json::array();
    for (const auto& [tid, ft] : f.threads)
      th.push_back(json{{"tid", tid}, {"running_ticks", ft.runningTicks}, {"sample_ticks", ft.sampleTicks}});
    frs.push_back(json{{"mark_tid", f.tid},
                       {"name", f.name},
                       {"number", f.number},
                       {"start", f.start},
                       {"end", f.end},
                       {"threads", std::move(th)}});
  }
  j["frames"] = std::move(frs);

  json fibs = json::array();
  for (const auto& [fid, segsIn] : fibers) {
    std::vector<FiberSegment> segs = segsIn;
    std::sort(segs.begin(), segs.end(), [](const FiberSegment& a, const FiberSegment& b) {
      return std::tie(a.begin, a.tid, a.depth, a.scope, a.end, a.scopeBegin) <
             std::tie(b.begin, b.tid, b.depth, b.scope, b.end, b.scopeBegin);
    });
    json sj = json::array();
    std::map<std::string, uint64_t> ticks;
    for (const FiberSegment& s : segs) {
      sj.push_back(json{{"tid", s.tid},
                        {"scope", s.scope},
                        {"depth", s.depth},
                        {"scope_begin", s.scopeBegin},
                        {"begin", s.begin},
                        {"end", s.end}});
      ticks[s.scope] += s.end - s.begin;
    }
    json tj = json::array();
    for (const auto& [id, t] : ticks) tj.push_back(json{{"scope", id}, {"ticks", t}});
    fibs.push_back(json{{"fiber_id", fid}, {"segments", std::move(sj)}, {"scope_ticks", std::move(tj)}});
  }
  j["fibers"] = std::move(fibs);

  json pls = json::array();
  for (const auto& [name, p] : plots)
    pls.push_back(json{{"name", name},
                       {"count", p.count},
                       {"min", p.min},
                       {"max", p.max},
                       {"first_ts", p.firstTs},
                       {"last_ts", p.lastTs}});
  j["plots"] = std::move(pls);
  return j;
}

namespace {

std::string cmp(const json& a, const json& b, const std::string& path) {
  if (a.is_number() && b.is_number()) {
    if (a.is_number_float() || b.is_number_float()) {
      const double x = a.get<double>(), y = b.get<double>();
      const double scale = std::max(std::fabs(x), std::fabs(y));
      if (std::fabs(x - y) <= 1e-9 * scale) return {};
    } else if (a == b) {
      return {};
    }
    return path + ": " + a.dump() + " != " + b.dump();
  }
  if (a.type() != b.type())
    return path + ": type mismatch (" + a.dump().substr(0, 60) + " vs " + b.dump().substr(0, 60) + ")";
  if (a.is_object()) {
    for (auto it = a.begin(); it != a.end(); ++it) {
      const auto jt = b.find(it.key());
      if (jt == b.end()) return path + "/" + it.key() + ": missing on the right";
      std::string r = cmp(it.value(), *jt, path + "/" + it.key());
      if (!r.empty()) return r;
    }
    for (auto it = b.begin(); it != b.end(); ++it)
      if (a.find(it.key()) == a.end()) return path + "/" + it.key() + ": missing on the left";
    return {};
  }
  if (a.is_array()) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
      std::string r = cmp(a[i], b[i], path + "[" + std::to_string(i) + "]");
      if (!r.empty()) return r;
    }
    if (a.size() != b.size())
      return path + ": array length " + std::to_string(a.size()) + " != " + std::to_string(b.size());
    return {};
  }
  return a == b ? std::string() : path + ": " + a.dump() + " != " + b.dump();
}

}  // namespace

std::string compareJson(const json& a, const json& b) {
  return cmp(a, b, "");
}

}  // namespace tachyon::tracegen
