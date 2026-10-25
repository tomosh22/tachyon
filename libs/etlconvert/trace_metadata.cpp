#include "etlconvert/trace_metadata.h"

namespace tachyon::etlconvert {

bool TraceMetadata::onProcess(const EtwEvent& event) {
  auto p = decodeProcess(event.payload, event.version);
  if (!p) return false;
  auto it = m_processes.find(p->processId);
  switch (event.opcode) {
    case opcodes::kProcessStart: {
      ProcessInfo info;
      info.pid = p->processId;
      info.parentPid = p->parentId;
      info.name = std::move(p->imageFileName);
      info.commandLine = std::move(p->commandLine);
      info.start = event.timestamp;
      m_processes[p->processId] = std::move(info);
      break;
    }
    case opcodes::kProcessDcStart:
    case opcodes::kProcessDcEnd: {
      if (it == m_processes.end()) {
        ProcessInfo info;
        info.pid = p->processId;
        info.parentPid = p->parentId;
        info.name = std::move(p->imageFileName);
        info.commandLine = std::move(p->commandLine);
        m_processes[p->processId] = std::move(info);
      } else if (it->second.name.empty()) {
        it->second.name = std::move(p->imageFileName);
        it->second.commandLine = std::move(p->commandLine);
      }
      break;
    }
    case opcodes::kProcessEnd: {
      if (it == m_processes.end()) {
        ProcessInfo info;
        info.pid = p->processId;
        info.parentPid = p->parentId;
        info.name = std::move(p->imageFileName);
        info.commandLine = std::move(p->commandLine);
        it = m_processes.emplace(p->processId, std::move(info)).first;
      }
      it->second.end = event.timestamp;
      break;
    }
    default: break;
  }
  return true;
}

bool TraceMetadata::onThread(const EtwEvent& event) {
  auto p = decodeThreadLifetime(event.payload);
  if (!p) return false;
  std::vector<uint32_t>& lives = m_byTid[p->threadId];
  // The active (not yet ended) life of this tid, if any.
  ThreadLife* active = nullptr;
  for (uint32_t idx : lives) {
    if (m_threads[idx].end == kOpenEnded) active = &m_threads[idx];
  }
  auto begin = [&](uint64_t start) {
    ThreadLife life;
    life.tid = p->threadId;
    life.pid = p->processId;
    life.start = start;
    lives.push_back(static_cast<uint32_t>(m_threads.size()));
    m_threads.push_back(std::move(life));
  };
  switch (event.opcode) {
    case opcodes::kThreadStart: begin(event.timestamp); break;
    case opcodes::kThreadDcStart:
    case opcodes::kThreadDcEnd:
      if (active == nullptr || active->pid != p->processId) begin(0);
      break;
    case opcodes::kThreadEnd:
      if (active == nullptr || active->pid != p->processId) {
        begin(0);
        m_threads.back().end = event.timestamp;
      } else {
        active->end = event.timestamp;
      }
      break;
    default: break;
  }
  return true;
}

bool TraceMetadata::onSetName(const EtwEvent& event) {
  auto p = decodeThreadSetName(event.payload);
  if (!p) return false;
  m_names.push_back(ThreadNameEvent{event.timestamp, p->threadId, p->name});
  auto it = m_byTid.find(p->threadId);
  if (it != m_byTid.end() && !it->second.empty()) {
    if (const ThreadLife* life = lifeOf(p->threadId, event.timestamp))
      m_threads[static_cast<size_t>(life - m_threads.data())].name = std::move(p->name);
  }
  return true;
}

const ThreadLife* TraceMetadata::lifeOf(uint32_t tid, uint64_t ts) const {
  auto it = m_byTid.find(tid);
  if (it == m_byTid.end() || it->second.empty()) return nullptr;
  const std::vector<uint32_t>& lives = it->second;
  if (lives.size() == 1) return &m_threads[lives[0]];
  const ThreadLife* best = nullptr;
  for (uint32_t idx : lives) {
    const ThreadLife& life = m_threads[idx];
    if (life.start <= ts && (best == nullptr || life.start >= best->start)) best = &life;
  }
  return best != nullptr ? best : &m_threads[lives[0]];
}

}  // namespace tachyon::etlconvert
