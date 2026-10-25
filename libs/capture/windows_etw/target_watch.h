// Optional capability of a capture backend: it knows the target process of the live capture (WP-13).
//
// The agent discovers it with dynamic_cast<const ITargetWatch*>(backend). Backends that do not implement it
// (the mock, the stub) are simply never auto-finished and never deferred.
#pragma once

#include <cstdint>

namespace tachyon::capture {

class ITargetWatch {
 public:
  virtual ~ITargetWatch() = default;

  // Thread-safe. A process was launched with start_immediately=false: it sits suspended, no kernel session is
  // running, and the next start request begins recording and resumes it.
  virtual bool launchPending() const = 0;
  // Thread-safe. The live capture's launched or attached target has exited. With follow_children and a job
  // object this means the whole process tree is gone. False when no capture is live.
  virtual bool targetExited() const = 0;
  // Thread-safe. Pid of the launched or attached root process; 0 when there is none.
  virtual uint32_t rootPid() const = 0;
};

}  // namespace tachyon::capture
