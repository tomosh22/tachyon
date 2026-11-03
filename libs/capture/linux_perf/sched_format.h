// Scheduler tracepoints of the perf backend (WP-71): parsing of tracefs `format` files, decoding of the raw
// sched_switch / sched_waking payloads by field name, and the mapping of the kernel's task state to the
// ContextSwitchRec encoding.
//
// Nothing here touches the kernel: the format text and the raw bytes come from tracefs files, the spool or
// byte fixtures, so the same code runs in unit tests on every kernel generation. Field offsets and widths are
// read from the format (they differ between kernels: `prev_state` is a `long` on some and an `unsigned int`
// on others), never hard-coded.
#pragma once

#include "core/result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon::capture::perf {

struct TracepointField {
  std::string name;  // "prev_pid"; for arrays without the "[16]"
  std::string type;  // the declaration without the name, e.g. "char", "unsigned long", "__data_loc char[]"
  uint32_t offset = 0;
  uint32_t size = 0;  // bytes (the whole array for arrays)
  bool isSigned = false;
  bool isArray = false;
  bool isDataLoc = false;  // a variable-length field: the payload holds an (offset, length) descriptor
};

struct TracepointFormat {
  std::string name;  // "sched_switch"
  uint32_t id = 0;   // the PERF_TYPE_TRACEPOINT config value
  std::vector<TracepointField> fields;

  // Thread-safe: pure function. The field called `fieldName`, or nullptr.
  const TracepointField* find(std::string_view fieldName) const;
};

// Thread-safe: pure function. Parses the text of events/<system>/<event>/format ("name:", "ID:", then one
// "field:<decl>; offset:N; size:N; signed:N;" line per field, a blank line, "print fmt:"). CorruptData when
// the name or ID is missing, a field line is malformed, or a field runs past the 64 KiB payload limit.
Result<TracepointFormat> parseTracepointFormat(std::string_view text);

// The values of a sched_switch record that the converter needs.
struct SchedSwitchEvent {
  uint32_t prevPid = 0;
  int32_t prevPrio = 0;
  uint64_t prevState = 0;  // the kernel's reported task state bits (see encodeSwitchState)
  uint32_t nextPid = 0;
  int32_t nextPrio = 0;
};

// The values of a sched_waking / sched_wakeup record.
struct SchedWakeEvent {
  uint32_t pid = 0;  // the task that is woken
  int32_t prio = 0;
  int32_t targetCpu = -1;
  bool success = true;  // false for a sched_wakeup of kernels that report failed wakeups (`success` field)
};

// Reads the raw payload of one tracepoint by the field table of its format. Immutable after create().
class SchedSwitchDecoder {
 public:
  // Thread-safe: pure function. InvalidArgument when prev_pid, prev_state, next_pid or next_prio is missing
  // or has a width other than 1, 2, 4 or 8 bytes; prev_prio is optional.
  static Result<SchedSwitchDecoder> create(const TracepointFormat& format);
  // Thread-safe. False when the payload is too short for the fields.
  bool decode(std::span<const uint8_t> raw, SchedSwitchEvent& out) const;
  uint32_t id() const { return m_id; }

 private:
  struct Field {
    uint32_t offset = 0, size = 0;
    bool isSigned = false, present = false;
  };
  uint32_t m_id = 0;
  Field m_prevPid, m_prevPrio, m_prevState, m_nextPid, m_nextPrio;
  uint32_t m_minSize = 0;
};

class SchedWakeDecoder {
 public:
  // Thread-safe: pure function. InvalidArgument when `pid` is missing or malformed; prio, target_cpu and
  // success are optional.
  static Result<SchedWakeDecoder> create(const TracepointFormat& format);
  // Thread-safe. False when the payload is too short for the fields.
  bool decode(std::span<const uint8_t> raw, SchedWakeEvent& out) const;
  uint32_t id() const { return m_id; }

 private:
  struct Field {
    uint32_t offset = 0, size = 0;
    bool isSigned = false, present = false;
  };
  uint32_t m_id = 0;
  Field m_pid, m_prio, m_targetCpu, m_success;
  uint32_t m_minSize = 0;
};

// Thread-safe: pure function. The common_type (event id) at the start of every tracepoint payload, or 0 when
// the payload is shorter than 2 bytes.
uint32_t tracepointCommonType(std::span<const uint8_t> raw);

// ContextSwitchRec fields derived from sched_switch's prev_state.
struct SwitchStateEncoding {
  uint8_t oldState = 5;       // KTHREAD_STATE numbering: 1 ready (preempted), 4 terminated, 5 waiting
  uint8_t oldWaitReason = 0;  // prev_state & 0xFF
};

// Thread-safe: pure function. The binding WP-71 encoding (docs/specs/attribution-rules.md): prev_state
// TASK_RUNNING (low byte 0: runnable, including the TASK_REPORT_MAX "preempted" flag 0x100 and older kernels'
// TASK_STATE_MAX) -> old_state 1; EXIT_DEAD / EXIT_ZOMBIE (0x10 / 0x20) -> 4; else 5. old_wait_reason is the
// low byte of prev_state.
SwitchStateEncoding encodeSwitchState(uint64_t prevState);

}  // namespace tachyon::capture::perf
