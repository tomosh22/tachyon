#include "model/model.h"

#include <cctype>

namespace tachyon::model {

namespace {

bool containsNoCase(std::string_view hay, std::string_view needle) {
  if (needle.size() > hay.size()) return false;
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    size_t j = 0;
    while (j < needle.size() &&
           std::tolower(static_cast<unsigned char>(hay[i + j])) == static_cast<unsigned char>(needle[j]))
      ++j;
    if (j == needle.size()) return true;
  }
  return false;
}

template <typename T>
size_t vecBytes(const std::vector<T>& v) {
  return v.capacity() * sizeof(T);
}

// Heap bytes owned by a std::string (the object itself is counted by its container).
size_t heapBytes(const std::string& s) {
  return s.capacity() > 15 ? s.capacity() + 1 : 0;
}

}  // namespace

ThreadRole classifyThreadRole(std::string_view name) {
  if (name.empty()) return ThreadRole::Unknown;
  if (containsNoCase(name, "main")) return ThreadRole::Main;
  if (containsNoCase(name, "render")) return ThreadRole::Render;
  if (containsNoCase(name, "worker") || containsNoCase(name, "job")) return ThreadRole::Worker;
  return ThreadRole::Other;
}

size_t TraceModel::residentBytes() const {
  size_t total = 0;
  total += vecBytes(processes);
  for (const Process& p : processes) total += heapBytes(p.name) + heapBytes(p.command_line);
  total += vecBytes(threads);
  for (const Thread& t : threads) total += heapBytes(t.name);
  total += vecBytes(modules);
  for (const Module& m : modules) total += heapBytes(m.path);
  total += vecBytes(frames) + vecBytes(nodes);
  // StringTable: bytes + 8 byte offset per string + open-addressing table (estimated at 8 bytes per string).
  size_t strData = 0;
  const size_t n = strings.size();
  for (size_t i = 0; i < n; ++i) strData += strings.get(StringIdx(static_cast<uint32_t>(i))).size();
  total += strData + (n + 1) * sizeof(uint64_t) + n * 2 * sizeof(uint32_t);
  // unordered_map: one node (next pointer, key, value) per element plus the bucket array.
  total += tidToThread.size() * (sizeof(void*) + sizeof(uint32_t) + sizeof(ThreadIdx)) +
           tidToThread.bucket_count() * sizeof(void*);
  for (const SampleColumns& s : samplesByThread)
    total += vecBytes(s.ts) + vecBytes(s.callstack) + vecBytes(s.cpu) + vecBytes(s.flags);
  total += vecBytes(samplesByThread);
  for (const auto& v : statesByThread) total += vecBytes(v);
  total += vecBytes(statesByThread);
  for (const auto& v : scopesByThread) total += vecBytes(v);
  total += vecBytes(scopesByThread);
  total += vecBytes(fiberSwitches) + vecBytes(frameMarks) + vecBytes(markers);
  for (const PlotSeries& p : plots) total += vecBytes(p.ts) + vecBytes(p.value);
  total += vecBytes(plots);
  total +=
      vecBytes(contextSwitches) + vecBytes(readyEvents) + vecBytes(scopeEvents) + vecBytes(clockCalibrations);
  total += heapBytes(captureInfo.raw_json);
  return total;
}

}  // namespace tachyon::model
