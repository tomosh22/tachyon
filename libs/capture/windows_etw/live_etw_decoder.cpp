#include "capture/windows_etw/live_etw_decoder.h"

#include "trace/trace_format.h"

#include <algorithm>
#include <cstring>

namespace tachyon::capture::etw {

namespace {

using etlconvert::EtwEvent;
using etlconvert::MatchRole;
using etlconvert::ParentKind;
using etlconvert::PendingParent;

constexpr size_t kMaxThreadEntries = 200'000;
constexpr uint32_t kInvalidPid = 0xFFFFFFFFu;
constexpr uint64_t kStillRunning = ~uint64_t{0};

uint32_t cleanPid(uint32_t pid) {
  return pid == kInvalidPid ? 0 : pid;
}

}  // namespace

LiveEtwDecoder::LiveEtwDecoder(LiveEtwDecoderConfig config)
    : m_cfg(std::move(config)),
      m_sink(*m_cfg.sink),
      m_roots(m_cfg.rootPids),
      m_tracker(m_cfg.rootPids, m_cfg.followChildren, m_cfg.childFilter),
      m_modules(m_cfg.peLookup, m_cfg.deviceMapper) {
  for (uint32_t pid : m_roots) m_targets[pid] = kStillRunning;
  const uint64_t windowTicks = m_cfg.clockFrequencyHz * m_cfg.stackWindowMs / 1000;
  m_matcher = std::make_unique<etlconvert::StackMatcher>(windowTicks,
                                                         static_cast<etlconvert::ParentListener&>(*this));
}

LiveEtwDecoder::~LiveEtwDecoder() = default;

uint32_t LiveEtwDecoder::pidOfThread(uint32_t tid, uint32_t fallbackPid) const {
  if (tid == 0) return 0;
  const auto it = m_threads.find(tid);
  if (it != m_threads.end() && it->second.pid != 0) return it->second.pid;
  return cleanPid(fallbackPid);
}

bool LiveEtwDecoder::keepThread(uint32_t tid, uint32_t fallbackPid, uint64_t ts) const {
  if (tid == 0) return false;
  const uint32_t pid = pidOfThread(tid, fallbackPid);
  if (systemMode()) return true;  // unknown pid or not: system mode keeps everything
  return isTarget(pid, ts);
}

// ---------------------------------------------------------------------------------------------------------
// Event dispatch

void LiveEtwDecoder::onEvent(const EtwEvent& e) {
  ++m_stats.events;
  m_stats.maxTs = std::max(m_stats.maxTs, e.timestamp);
  namespace guids = etlconvert::guids;
  namespace op = etlconvert::opcodes;
  if (e.provider == guids::kPerfInfo) {
    if (e.opcode == op::kPerfInfoSampledProfile) onSample(e);
  } else if (e.provider == guids::kStackWalk) {
    if (e.opcode == op::kStackWalkStack)
      onStack(e);
    else if (e.opcode >= op::kStackWalkKeyKernel && e.opcode <= op::kStackWalkKeyDelete)
      ++m_stats.stackKeyEvents;
  } else if (e.provider == guids::kThread) {
    if (e.opcode == op::kThreadCSwitch)
      onCSwitch(e);
    else if (e.opcode == op::kThreadReadyThread)
      onReady(e);
    else if (e.opcode == op::kThreadSetName)
      onSetName(e);
    else if (e.opcode >= op::kThreadStart && e.opcode <= op::kThreadDcEnd)
      onThreadEvent(e);
  } else if (e.provider == guids::kProcess) {
    onProcessEvent(e);
  } else if (e.provider == guids::kImage) {
    onImageEvent(e);
  }
  m_newestTs = std::max(m_newestTs, e.timestamp);
  m_matcher->advance(m_newestTs);
  if (!m_ended.empty()) reapEnded(false);
}

void LiveEtwDecoder::flush() {
  m_matcher->flush();
  reapEnded(true);
}

void LiveEtwDecoder::reapEnded(bool all) {
  const uint64_t grace = kEndedGraceSeconds * m_cfg.clockFrequencyHz;
  std::vector<std::pair<uint32_t, uint64_t>> due;
  for (const auto& [pid, endTs] : m_ended)
    if (all || m_newestTs > endTs + grace) due.emplace_back(pid, endTs);
  std::sort(due.begin(), due.end());
  for (const auto& [pid, endTs] : due) reapProcess(pid, endTs);
}

void LiveEtwDecoder::reapProcess(uint32_t pid, uint64_t endTs) {
  unloadAllModules(pid, endTs);
  m_targets.erase(pid);
  m_procs.erase(pid);
  m_ended.erase(pid);
}

void LiveEtwDecoder::onProcessEvent(const EtwEvent& e) {
  namespace op = etlconvert::opcodes;
  if (e.opcode < op::kProcessStart || e.opcode > op::kProcessDcEnd) return;
  auto p = etlconvert::decodeProcess(e.payload, e.version);
  if (!p) {
    ++m_stats.malformed;
    return;
  }
  const uint32_t pid = p->processId;
  if (pid == 0) return;
  // A new process with the pid of one that ended: forget the old one first.
  if (e.opcode == op::kProcessStart)
    if (const auto old = m_ended.find(pid); old != m_ended.end()) reapProcess(pid, old->second);
  if (m_tracker.onEvent(e)) m_targets[pid] = kStillRunning;  // a followed child (Start) or an existing one
  auto it = m_procs.find(pid);
  switch (e.opcode) {
    case op::kProcessStart: {
      Proc proc;
      proc.ppid = p->parentId;
      proc.name = std::move(p->imageFileName);
      proc.commandLine = std::move(p->commandLine);
      proc.startTs = e.timestamp;
      m_procs[pid] = std::move(proc);
      if (keepPid(pid, e.timestamp)) emitProcess(pid);
      break;
    }
    case op::kProcessDcStart:
    case op::kProcessDcEnd: {
      if (it == m_procs.end()) {
        Proc proc;
        proc.ppid = p->parentId;
        proc.name = std::move(p->imageFileName);
        proc.commandLine = std::move(p->commandLine);
        it = m_procs.emplace(pid, std::move(proc)).first;
      } else if (it->second.name.empty()) {
        it->second.name = std::move(p->imageFileName);
        it->second.commandLine = std::move(p->commandLine);
      }
      // DCEnd is the end-of-session rundown: it only completes what is already reported.
      if (keepPid(pid, e.timestamp) && (e.opcode == op::kProcessDcStart || it->second.emitted))
        emitProcess(pid);
      break;
    }
    case op::kProcessEnd: {
      if (it != m_procs.end()) {
        it->second.endTs = e.timestamp;
        if (it->second.emitted || keepPid(pid, e.timestamp)) emitProcess(pid);
      }
      // The process is forgotten later: what it did just before it ended may still be on its way here.
      if (const auto t = m_targets.find(pid); t != m_targets.end()) t->second = e.timestamp;
      m_ended[pid] = e.timestamp;
      break;
    }
    default: break;
  }
}

void LiveEtwDecoder::onThreadEvent(const EtwEvent& e) {
  namespace op = etlconvert::opcodes;
  auto p = etlconvert::decodeThreadLifetime(e.payload);
  if (!p) {
    ++m_stats.malformed;
    return;
  }
  const uint32_t tid = p->threadId;
  if (tid == 0) return;
  const uint32_t pid = p->processId;
  auto it = m_threads.find(tid);
  switch (e.opcode) {
    case op::kThreadStart:
    case op::kThreadDcStart:
    case op::kThreadDcEnd: {
      const bool fresh = it == m_threads.end() || it->second.pid != pid ||
                         (e.opcode == op::kThreadStart && it->second.endTs != 0);
      if (fresh) {
        Thr t;
        t.pid = pid;
        t.startTs = e.opcode == op::kThreadStart ? e.timestamp : 0;
        it = m_threads.insert_or_assign(tid, std::move(t)).first;
      }
      // DCEnd (end-of-session rundown) only completes what is known.
      if (keepPid(pid, e.timestamp) && (e.opcode != op::kThreadDcEnd || it->second.emitted)) emitThread(tid);
      break;
    }
    case op::kThreadEnd: {
      if (it == m_threads.end() || it->second.pid != pid) {
        Thr t;
        t.pid = pid;
        it = m_threads.insert_or_assign(tid, std::move(t)).first;
      }
      it->second.endTs = e.timestamp;
      if (it->second.emitted) emitThread(tid);
      break;
    }
    default: break;
  }
  if (m_threads.size() > kMaxThreadEntries) pruneThreads();
}

void LiveEtwDecoder::onSetName(const EtwEvent& e) {
  auto p = etlconvert::decodeThreadSetName(e.payload);
  if (!p) {
    ++m_stats.malformed;
    return;
  }
  if (p->threadId == 0) return;
  auto it = m_threads.find(p->threadId);
  if (it == m_threads.end()) {
    Thr t;
    t.pid = p->processId;
    it = m_threads.emplace(p->threadId, std::move(t)).first;
  }
  it->second.name = std::move(p->name);
  if (keepPid(it->second.pid, e.timestamp)) emitThread(p->threadId);
}

void LiveEtwDecoder::onImageEvent(const EtwEvent& e) {
  namespace op = etlconvert::opcodes;
  if (e.opcode != op::kImageLoad && e.opcode != op::kImageDcStart && e.opcode != op::kImageDcEnd &&
      e.opcode != op::kImageUnload)
    return;
  auto img = etlconvert::decodeImage(e.payload);
  if (!img) {
    ++m_stats.malformed;
    return;
  }
  if (img->imageBase >= etlconvert::kKernelAddressBase) return;  // kernel frames stay absolute addresses
  const uint32_t pid = img->processId;
  if (!keepPid(pid, e.timestamp)) return;
  if (e.opcode == op::kImageUnload) {
    unloadModule(pid, img->imageBase, e.timestamp);
  } else {
    loadModule(pid, img->imageBase, img->imageSize, img->fileName, img->imageChecksum, img->timeDateStamp,
               e.opcode == op::kImageLoad ? e.timestamp : 0);
  }
}

void LiveEtwDecoder::onSample(const EtwEvent& e) {
  auto s = etlconvert::decodeSampledProfile(e.payload);
  if (!s) {
    ++m_stats.malformed;
    return;
  }
  const uint32_t fallbackPid = e.threadId == s->threadId ? e.processId : 0;
  if (s->threadId == 0 || !keepThread(s->threadId, fallbackPid, e.timestamp)) {
    ++m_stats.droppedNotTarget;
    return;
  }
  const PendingParent::Candidate cands[2] = {{s->threadId, MatchRole::PayloadThread},
                                             {e.threadId, MatchRole::HeaderThread}};
  PendingParent& p = m_matcher->add(ParentKind::Sample, e.timestamp, cands, 2);
  p.cpu = e.cpu;
  p.headerTid = e.threadId;
  p.headerPid = fallbackPid;
  p.tidA = s->threadId;
  p.ip = s->instructionPointer;
}

void LiveEtwDecoder::onCSwitch(const EtwEvent& e) {
  auto c = etlconvert::decodeCSwitch(e.payload);
  if (!c) {
    ++m_stats.malformed;
    return;
  }
  if (!keepThread(c->newThreadId, 0, e.timestamp) && !keepThread(c->oldThreadId, 0, e.timestamp)) {
    ++m_stats.droppedNotTarget;
    return;
  }
  const PendingParent::Candidate cands[3] = {{e.threadId, MatchRole::HeaderThread},
                                             {c->newThreadId, MatchRole::PayloadThread},
                                             {c->oldThreadId, MatchRole::PayloadOldThread}};
  PendingParent& p = m_matcher->add(ParentKind::CSwitch, e.timestamp, cands, 3);
  p.cpu = e.cpu;
  p.headerTid = e.threadId;
  p.headerPid = cleanPid(e.processId);
  p.tidA = c->newThreadId;
  p.tidB = c->oldThreadId;
  p.bytes[0] = static_cast<uint8_t>(c->oldThreadState);
  p.bytes[1] = static_cast<uint8_t>(c->oldThreadWaitReason);
  p.bytes[2] = static_cast<uint8_t>(c->oldThreadWaitMode);
  p.bytes[3] = static_cast<uint8_t>(c->newThreadPriority);
}

void LiveEtwDecoder::onReady(const EtwEvent& e) {
  auto r = etlconvert::decodeReadyThread(e.payload);
  if (!r) {
    ++m_stats.malformed;
    return;
  }
  // The readying thread is the header thread; its stack is kept even when it belongs to another process, as
  // long as the readied thread is a target.
  if (!keepThread(r->threadId, 0, e.timestamp) && !keepThread(e.threadId, e.processId, e.timestamp)) {
    ++m_stats.droppedNotTarget;
    return;
  }
  const PendingParent::Candidate cands[2] = {{e.threadId, MatchRole::HeaderThread},
                                             {r->threadId, MatchRole::PayloadThread}};
  PendingParent& p = m_matcher->add(ParentKind::Ready, e.timestamp, cands, 2);
  p.cpu = e.cpu;
  p.headerTid = e.threadId;
  p.headerPid = cleanPid(e.processId);
  p.tidA = r->threadId;
  p.tidB = e.threadId;
  p.bytes[0] = static_cast<uint8_t>(r->adjustReason);
  p.bytes[1] = static_cast<uint8_t>(r->flag);
}

void LiveEtwDecoder::onStack(const EtwEvent& e) {
  auto s = etlconvert::decodeStackWalk(e.payload);
  if (!s) {
    ++m_stats.malformed;
    return;
  }
  const etlconvert::StackMatchResult r = m_matcher->onStack(*s);
  if (r.kind == etlconvert::StackMatchResult::Kind::Unmatched)
    ++m_stats.stacksUnmatched;
  else
    ++m_stats.stacksMatched;
}

// ---------------------------------------------------------------------------------------------------------
// Finished parents -> sink

void LiveEtwDecoder::onParentDone(PendingParent& p) {
  if (!p.hasStack()) ++m_stats.parentsWithoutStack;
  switch (p.kind) {
    case ParentKind::Sample: {
      const uint32_t tid = p.tidA;
      const uint32_t pid = pidOfThread(tid, p.headerPid);
      ensureThread(tid, pid);
      m_ips.clear();
      if (p.hasStack() && !p.frames.empty())
        m_ips.assign(p.frames.begin(), p.frames.end());
      else
        m_ips.push_back(p.ip);
      const uint16_t flags = m_ips[0] >= etlconvert::kKernelAddressBase ? trace::kSampleFlagKernelLeaf : 0;
      m_sink.onSample(p.ts, pid, tid, p.cpu, flags, m_ips);
      ++m_stats.samples;
      break;
    }
    case ParentKind::CSwitch: {
      LiveContextSwitch cs;
      cs.ts = p.ts;
      cs.cpu = p.cpu;
      cs.newTid = p.tidA;
      cs.oldTid = p.tidB;
      cs.newPid = pidOfThread(cs.newTid, 0);
      cs.oldPid = pidOfThread(cs.oldTid, 0);
      cs.oldState = p.bytes[0];
      cs.oldWaitReason = p.bytes[1];
      cs.oldWaitMode = p.bytes[2];
      cs.newPriority = p.bytes[3];
      cs.flags = p.hasStack() && p.stackThread != p.tidB ? trace::kSwitchFlagStackFromOtherThread : 0u;
      if (p.hasStack()) cs.oldStack = p.frames;
      if (keepPid(cs.newPid, p.ts)) ensureThread(cs.newTid, cs.newPid);
      if (keepPid(cs.oldPid, p.ts)) ensureThread(cs.oldTid, cs.oldPid);
      m_sink.onContextSwitch(cs);
      ++m_stats.switches;
      break;
    }
    case ParentKind::Ready: {
      LiveReady rd;
      rd.ts = p.ts;
      rd.cpu = p.cpu;
      rd.readiedTid = p.tidA;
      rd.readyingTid = p.tidB;
      rd.readiedPid = pidOfThread(rd.readiedTid, 0);
      rd.readyingPid = pidOfThread(rd.readyingTid, p.headerPid);
      rd.adjustReason = p.bytes[0];
      rd.flags = p.bytes[1];
      if (p.hasStack()) rd.readyingStack = p.frames;
      if (keepPid(rd.readiedPid, p.ts)) ensureThread(rd.readiedTid, rd.readiedPid);
      m_sink.onReady(rd);
      ++m_stats.readies;
      break;
    }
    case ParentKind::kCount: break;
  }
}

// ---------------------------------------------------------------------------------------------------------
// Tables -> sink

void LiveEtwDecoder::emitProcess(uint32_t pid) {
  auto it = m_procs.find(pid);
  if (it == m_procs.end()) return;
  Proc& p = it->second;
  LiveProcess lp;
  lp.pid = pid;
  lp.ppid = p.ppid;
  lp.startTs = p.startTs != 0 ? p.startTs : m_cfg.startTs;
  lp.endTs = p.endTs;
  lp.name = p.name;
  lp.commandLine = p.commandLine;
  p.emitted = true;
  m_sink.onProcess(lp);
}

void LiveEtwDecoder::ensureProcess(uint32_t pid) {
  if (pid == 0) return;
  auto it = m_procs.find(pid);
  if (it == m_procs.end()) it = m_procs.emplace(pid, Proc{}).first;
  if (!it->second.emitted) emitProcess(pid);
}

void LiveEtwDecoder::emitThread(uint32_t tid) {
  auto it = m_threads.find(tid);
  if (it == m_threads.end()) return;
  Thr& t = it->second;
  ensureProcess(t.pid);
  LiveThread lt;
  lt.tid = tid;
  lt.pid = t.pid;
  lt.startTs = t.startTs != 0 ? t.startTs : m_cfg.startTs;
  lt.endTs = t.endTs;
  lt.name = t.name;
  lt.isTarget = isTarget(t.pid);  // at some time
  t.emitted = true;
  m_sink.onThread(lt);
}

void LiveEtwDecoder::ensureThread(uint32_t tid, uint32_t pid) {
  if (tid == 0) return;
  auto it = m_threads.find(tid);
  if (it == m_threads.end()) {
    Thr t;
    t.pid = pid;
    it = m_threads.emplace(tid, std::move(t)).first;
  }
  if (!it->second.emitted) emitThread(tid);
}

void LiveEtwDecoder::pruneThreads() {
  const uint64_t keepAfter =
      m_newestTs > 60 * m_cfg.clockFrequencyHz ? m_newestTs - 60 * m_cfg.clockFrequencyHz : 0;
  for (auto it = m_threads.begin(); it != m_threads.end();) {
    if (it->second.endTs != 0 && it->second.endTs < keepAfter)
      it = m_threads.erase(it);
    else
      ++it;
  }
}

void LiveEtwDecoder::loadModule(uint32_t pid, uint64_t base, uint64_t size, const std::string& path,
                                uint32_t checksum, uint32_t timeDateStamp, uint64_t loadTs) {
  auto& perPid = m_loaded[pid];
  const auto known = perPid.find(base);
  if (known != perPid.end()) {
    if (known->second.size == size) return;  // the rundown or a repeated event: already reported
    unloadModule(pid, base, loadTs != 0 ? loadTs : m_cfg.startTs);
  }
  const uint32_t idx = m_modules.load(pid, base, size, path, checksum, timeDateStamp, loadTs);
  const etlconvert::LoadedModule& lm = m_modules.modules()[idx];
  LiveModule m;
  m.pid = pid;
  m.base = base;
  m.size = size;
  m.path = lm.path;
  m.idKind = lm.rec.id_kind;
  m.idLen = lm.rec.id_len;
  m.id = lm.rec.id;
  m.age = lm.rec.age;
  m.loadTs = loadTs != 0 ? loadTs : std::max<uint64_t>(m_cfg.startTs, 1);
  m.unloadTs = 0;
  ensureProcess(pid);
  m_sink.onModule(m);
  m_loaded[pid][base] = std::move(m);
  ++m_stats.modulesLoaded;
}

void LiveEtwDecoder::unloadModule(uint32_t pid, uint64_t base, uint64_t ts) {
  const auto pit = m_loaded.find(pid);
  if (pit == m_loaded.end()) return;
  const auto it = pit->second.find(base);
  if (it == pit->second.end()) return;
  LiveModule m = std::move(it->second);
  pit->second.erase(it);
  m.unloadTs = std::max<uint64_t>(ts, std::max<uint64_t>(m.loadTs, 1));
  m_modules.unload(pid, base, ts);
  m_sink.onModule(m);
  ++m_stats.modulesUnloaded;
}

void LiveEtwDecoder::unloadAllModules(uint32_t pid, uint64_t ts) {
  const auto pit = m_loaded.find(pid);
  if (pit == m_loaded.end()) return;
  std::vector<uint64_t> bases;
  bases.reserve(pit->second.size());
  for (const auto& [base, m] : pit->second) bases.push_back(base);
  std::sort(bases.begin(), bases.end());
  for (uint64_t base : bases) unloadModule(pid, base, ts);
  m_loaded.erase(pid);
}

void LiveEtwDecoder::applyRundown(const RundownSnapshot& snap) {
  for (const RundownProcess& rp : snap.processes) {
    if (rp.pid == 0) continue;
    auto it = m_procs.find(rp.pid);
    if (it == m_procs.end()) {
      Proc p;
      p.ppid = rp.ppid;
      p.name = rp.name;
      it = m_procs.emplace(rp.pid, std::move(p)).first;
    } else if (it->second.name.empty()) {
      it->second.name = rp.name;
      it->second.ppid = rp.ppid;
    }
    if (keepPid(rp.pid)) emitProcess(rp.pid);
  }
  for (const RundownThread& rt : snap.threads) {
    if (rt.tid == 0) continue;
    auto it = m_threads.find(rt.tid);
    if (it == m_threads.end()) {
      Thr t;
      t.pid = rt.pid;
      t.name = rt.name;
      it = m_threads.emplace(rt.tid, std::move(t)).first;
    } else if (it->second.name.empty()) {
      it->second.name = rt.name;
    }
    if (keepPid(it->second.pid)) emitThread(rt.tid);
  }
  for (const RundownModule& rm : snap.modules) {
    if (!keepPid(rm.pid) || rm.base >= etlconvert::kKernelAddressBase) continue;
    loadModule(rm.pid, rm.base, rm.size, rm.path, 0, 0, 0);
  }
}

}  // namespace tachyon::capture::etw
