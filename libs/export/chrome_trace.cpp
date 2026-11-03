// Chrome trace event exporter (WP-63). See export.h for the output contract.
#include "analysis/scope_pairing.h"
#include "export/export.h"
#include "export/export_common.h"
#include "export/json_writer.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace tachyon::exporters {

namespace {

constexpr uint32_t kCpuPidBase = 0x40000000u;

struct CpuSlice {
  uint32_t cpu = 0;
  uint32_t thread = 0;
  uint64_t start = 0, end = 0;
};

std::string colorHex(uint32_t rgba) {
  char buf[12];
  std::snprintf(buf, sizeof buf, "#%08X", rgba);
  return buf;
}

// One output stream plus the conversions every event needs.
class Emitter {
 public:
  Emitter(JsonWriter& w, uint64_t origin, uint64_t hz) : m_w(w), m_origin(origin), m_hz(hz) {}

  // Starts an event object with the common fields; the caller adds optional fields and ends it.
  void begin(std::string_view name, std::string_view cat, std::string_view ph, uint64_t ts, uint32_t pid,
             uint32_t tid) {
    m_w.beginObject();
    m_w.key("name");
    m_w.value(name);
    if (!cat.empty()) {
      m_w.key("cat");
      m_w.value(cat);
    }
    m_w.key("ph");
    m_w.value(ph);
    m_w.key("ts");
    m_w.microsFromNanos(detail::signedNanos(ts, m_origin, m_hz));
    m_w.key("pid");
    m_w.value(pid);
    m_w.key("tid");
    m_w.value(tid);
  }

  void duration(uint64_t ticks) {
    m_w.key("dur");
    m_w.microsFromNanos(static_cast<int64_t>(detail::ticksToNanos(ticks, m_hz)));
  }

  void end() {
    m_w.endObject();
    ++m_events;
  }

  void metadata(std::string_view what, uint32_t pid, uint32_t tid, std::string_view name) {
    m_w.beginObject();
    m_w.key("name");
    m_w.value(what);
    m_w.key("ph");
    m_w.value("M");
    m_w.key("pid");
    m_w.value(pid);
    m_w.key("tid");
    m_w.value(tid);
    m_w.key("args");
    m_w.beginObject();
    m_w.key("name");
    m_w.value(name);
    m_w.endObject();
    end();
  }

  JsonWriter& writer() { return m_w; }
  uint64_t events() const { return m_events; }

 private:
  JsonWriter& m_w;
  uint64_t m_origin, m_hz;
  uint64_t m_events = 0;
};

}  // namespace

Result<ExportStats> exportChromeTrace(const model::TraceModel& m, const symbols::SymbolService& /*symbols*/,
                                      const analysis::Filter& filter, std::ostream& out) {
  auto selected = detail::selectThreads(m, filter);
  if (!selected.ok()) return selected.error();
  auto paired = analysis::pairScopes(m);
  if (!paired.ok()) return paired.error();
  const uint64_t hz = m.header.clock_frequency_hz;
  const uint64_t t0 = filter.t0, t1 = filter.t1;
  auto inRange = [&](uint64_t ts) { return ts >= t0 && ts < t1; };

  std::vector<bool> isSelected(m.threads.size(), false);
  for (const ThreadIdx t : *selected) isSelected[t.get()] = true;

  // Frame marks and markers per selected thread (time order), global markers apart.
  std::vector<std::vector<uint32_t>> marksBy(m.threads.size()), markersBy(m.threads.size());
  std::vector<uint32_t> globalMarkers;
  for (uint32_t i = 0; i < m.frameMarks.size(); ++i) {
    const model::FrameMark& fm = m.frameMarks[i];
    if (fm.thread.valid() && fm.thread.get() < m.threads.size() && isSelected[fm.thread.get()] &&
        inRange(fm.ts))
      marksBy[fm.thread.get()].push_back(i);
  }
  for (uint32_t i = 0; i < m.markers.size(); ++i) {
    const model::Marker& mk = m.markers[i];
    if (!inRange(mk.ts)) continue;
    if (!mk.thread.valid() || mk.thread.get() >= m.threads.size())
      globalMarkers.push_back(i);
    else if (isSelected[mk.thread.get()])
      markersBy[mk.thread.get()].push_back(i);
  }
  auto sortByTs = [](std::vector<uint32_t>& v, const auto& events) {
    std::stable_sort(v.begin(), v.end(), [&](uint32_t a, uint32_t b) { return events[a].ts < events[b].ts; });
  };
  for (const ThreadIdx t : *selected) {
    sortByTs(marksBy[t.get()], m.frameMarks);
    sortByTs(markersBy[t.get()], m.markers);
  }
  sortByTs(globalMarkers, m.markers);

  // CPU tracks: the Running intervals of the selected threads, clipped to the range.
  std::vector<CpuSlice> slices;
  for (const ThreadIdx t : *selected) {
    for (const model::ThreadStateInterval& iv : m.statesByThread[t.get()]) {
      if (iv.end <= t0) continue;
      if (iv.start >= t1) break;
      if (iv.state != model::ThreadState::Running) continue;
      const uint64_t s = std::max(iv.start, t0), e = std::min(iv.end, t1);
      if (s < e) slices.push_back({iv.cpu, t.get(), s, e});
    }
  }
  std::sort(slices.begin(), slices.end(), [](const CpuSlice& a, const CpuSlice& b) {
    if (a.cpu != b.cpu) return a.cpu < b.cpu;
    if (a.start != b.start) return a.start < b.start;
    return a.thread < b.thread;
  });

  // Pseudo-process id for the CPU tracks: unused by every real process and thread.
  uint32_t cpuPid = kCpuPidBase;
  for (bool clash = true; clash;) {
    clash = false;
    for (const model::Thread& th : m.threads)
      if (th.pid == cpuPid) clash = true;
    for (const model::Process& p : m.processes)
      if (p.pid == cpuPid) clash = true;
    if (clash) ++cpuPid;
  }
  uint32_t primaryPid = 0;
  for (const model::Thread& th : m.threads) {
    if (th.is_target) {
      primaryPid = th.pid;
      break;
    }
  }
  if (primaryPid == 0 && !m.threads.empty()) primaryPid = m.threads.front().pid;

  JsonWriter w(out);
  Emitter e(w, m.header.start_ts, hz);
  w.beginObject();
  w.key("displayTimeUnit");
  w.value("ns");
  w.key("traceEvents");
  w.beginArray();

  // Metadata.
  std::vector<uint32_t> pids;
  for (const ThreadIdx t : *selected) pids.push_back(m.threads[t.get()].pid);
  std::sort(pids.begin(), pids.end());
  pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
  for (const uint32_t pid : pids) {
    std::string name = "pid " + std::to_string(pid);
    for (const model::Process& p : m.processes) {
      if (p.pid == pid && !p.name.empty()) {
        name = p.name + " (" + std::to_string(pid) + ")";
        break;
      }
    }
    e.metadata("process_name", pid, 0, name);
  }
  for (const ThreadIdx t : *selected) {
    const model::Thread& th = m.threads[t.get()];
    e.metadata("thread_name", th.pid, th.tid, th.name.empty() ? "thread " + std::to_string(th.tid) : th.name);
  }
  if (!slices.empty()) {
    e.metadata("process_name", cpuPid, 0, "CPU");
    uint32_t last = kInvalidIdx;
    for (const CpuSlice& s : slices) {
      if (s.cpu == last) continue;
      last = s.cpu;
      e.metadata("thread_name", cpuPid, s.cpu, "CPU " + std::to_string(s.cpu));
    }
  }

  auto threadLabel = [&](const model::Thread& th) {
    return th.name.empty() ? "thread " + std::to_string(th.tid) : th.name;
  };

  // Per thread: scopes, frame marks and markers merged by timestamp (ties: scopes, frame marks, markers).
  for (const ThreadIdx t : *selected) {
    const model::Thread& th = m.threads[t.get()];
    const std::vector<analysis::ScopeInstance>& scopes = paired->byThread[t.get()];
    size_t si = static_cast<size_t>(
        std::partition_point(scopes.begin(), scopes.end(),
                             [&](const analysis::ScopeInstance& s) { return s.begin < t0; }) -
        scopes.begin());
    const std::vector<uint32_t>& marks = marksBy[t.get()];
    const std::vector<uint32_t>& markers = markersBy[t.get()];
    size_t mi = 0, ki = 0;
    for (;;) {
      const bool haveScope = si < scopes.size() && scopes[si].begin < t1;
      const bool haveMark = mi < marks.size();
      const bool haveMarker = ki < markers.size();
      if (!haveScope && !haveMark && !haveMarker) break;
      constexpr uint64_t kNone = UINT64_MAX;
      const uint64_t sTs = haveScope ? scopes[si].begin : kNone;
      const uint64_t fTs = haveMark ? m.frameMarks[marks[mi]].ts : kNone;
      const uint64_t kTs = haveMarker ? m.markers[markers[ki]].ts : kNone;
      if (haveScope && sTs <= fTs && sTs <= kTs) {
        const analysis::ScopeInstance& sc = scopes[si++];
        std::string_view name = m.strings.get(sc.id);
        e.begin(name.empty() ? std::string_view("(scope)") : name, "scope", "X", sc.begin, th.pid, th.tid);
        e.duration(sc.end >= sc.begin ? sc.end - sc.begin : 0);
        const std::string_view data = m.strings.get(sc.data);
        if (!data.empty() || sc.unterminated) {
          w.key("args");
          w.beginObject();
          if (!data.empty()) {
            w.key("data");
            w.value(data);
          }
          if (sc.unterminated) {
            w.key("unterminated");
            w.value(true);
          }
          w.endObject();
        }
        e.end();
      } else if (haveMark && fTs <= kTs) {
        const model::FrameMark& fm = m.frameMarks[marks[mi++]];
        std::string_view name = m.strings.get(fm.name);
        e.begin(name.empty() ? std::string_view("Frame") : name, "framemark", "i", fm.ts, th.pid, th.tid);
        w.key("s");
        w.value("t");
        w.key("args");
        w.beginObject();
        w.key("frame_number");
        w.value(fm.frame_number);
        w.endObject();
        e.end();
      } else {
        const model::Marker& mk = m.markers[markers[ki++]];
        std::string_view name = m.strings.get(mk.name);
        e.begin(name.empty() ? std::string_view("Marker") : name, "marker", "i", mk.ts, th.pid, th.tid);
        w.key("s");
        w.value("t");
        const std::string_view data = m.strings.get(mk.data);
        if (!data.empty() || mk.color != 0) {
          w.key("args");
          w.beginObject();
          if (!data.empty()) {
            w.key("data");
            w.value(data);
          }
          if (mk.color != 0) {
            w.key("color");
            w.value(colorHex(mk.color));
          }
          w.endObject();
        }
        e.end();
      }
    }
  }

  // CPU tracks.
  for (const CpuSlice& s : slices) {
    const model::Thread& th = m.threads[s.thread];
    e.begin(threadLabel(th), "cpu", "X", s.start, cpuPid, s.cpu);
    e.duration(s.end - s.start);
    w.key("args");
    w.beginObject();
    w.key("tid");
    w.value(th.tid);
    w.key("pid");
    w.value(th.pid);
    w.endObject();
    e.end();
  }

  // Global markers and counters share the process track (pid, tid 0), so they are emitted merged by time.
  struct GlobalItem {
    uint64_t ts;
    uint32_t series;  // kMarker: marker index; otherwise the plot series
    uint32_t point;
  };
  constexpr uint32_t kMarker = kInvalidIdx;
  std::vector<GlobalItem> globals;
  for (const uint32_t i : globalMarkers) globals.push_back({m.markers[i].ts, kMarker, i});
  for (uint32_t s = 0; s < m.plots.size(); ++s) {
    const model::PlotSeries& series = m.plots[s];
    const size_t n = std::min(series.ts.size(), series.value.size());
    for (size_t i = static_cast<size_t>(
             std::partition_point(series.ts.begin(), series.ts.begin() + static_cast<std::ptrdiff_t>(n),
                                  [&](uint64_t ts) { return ts < t0; }) -
             series.ts.begin());
         i < n && series.ts[i] < t1; ++i)
      globals.push_back({series.ts[i], s, static_cast<uint32_t>(i)});
  }
  std::stable_sort(globals.begin(), globals.end(),
                   [](const GlobalItem& a, const GlobalItem& b) { return a.ts < b.ts; });
  for (const GlobalItem& g : globals) {
    if (g.series == kMarker) {
      const model::Marker& mk = m.markers[g.point];
      std::string_view name = m.strings.get(mk.name);
      e.begin(name.empty() ? std::string_view("Marker") : name, "marker", "i", mk.ts, primaryPid, 0);
      w.key("s");
      w.value("g");
      const std::string_view data = m.strings.get(mk.data);
      if (!data.empty() || mk.color != 0) {
        w.key("args");
        w.beginObject();
        if (!data.empty()) {
          w.key("data");
          w.value(data);
        }
        if (mk.color != 0) {
          w.key("color");
          w.value(colorHex(mk.color));
        }
        w.endObject();
      }
      e.end();
    } else {
      const model::PlotSeries& series = m.plots[g.series];
      std::string_view name = m.strings.get(series.name);
      if (name.empty()) name = "Plot";
      e.begin(name, "plot", "C", series.ts[g.point], primaryPid, 0);
      w.key("args");
      w.beginObject();
      w.key("value");
      w.value(series.value[g.point]);
      w.endObject();
      e.end();
    }
  }

  w.endArray();
  w.endObject();
  if (!w.flush()) return makeError(ErrorCode::IoError, "writing the Chrome trace output failed");
  ExportStats stats;
  stats.threads = selected->size();
  stats.events = e.events();
  return stats;
}

}  // namespace tachyon::exporters
