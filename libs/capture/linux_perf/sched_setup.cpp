#include "capture/linux_perf/sched_setup.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

namespace tachyon::capture::perf {

namespace fs = std::filesystem;

namespace {

struct Loaded {
  std::string text;
  TracepointFormat format;
};

// Reads and parses events/sched/<event>/format. On failure `why` says what is wrong.
bool loadFormat(const fs::path& root, const std::string& event, Loaded& out, std::string& why) {
  const fs::path path = root / "events" / "sched" / event / "format";
  std::ifstream in(path);
  if (!in) {
    const int err = errno;
    why = path.string() +
          (err == ENOENT ? " does not exist" : std::string(" cannot be read: ") + std::strerror(err));
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  out.text = ss.str();
  auto parsed = parseTracepointFormat(out.text);
  if (!parsed.ok()) {
    why = path.string() + ": " + parsed.error().message;
    return false;
  }
  out.format = std::move(*parsed);
  return true;
}

std::string fallbackWarning(const std::string& reason, bool wantSwitch, bool wantWake) {
  std::string w = "scheduler tracepoints are unavailable (" + reason + "); ";
  if (wantSwitch)
    w += "context switches are recorded from PERF_RECORD_SWITCH instead: wait reasons, blocking stacks and "
         "waker (ready-thread) events are not available in this capture";
  else
    w += "no scheduler events are recorded";
  (void)wantWake;
  return w;
}

}  // namespace

SchedPlan fallbackSchedPlan(bool wantSwitch, bool wantWake, const std::string& reason) {
  SchedPlan plan;
  plan.info.reason = reason;
  if (wantSwitch) {
    plan.info.mode = "context_switch";
    plan.info.switches = true;
  } else {
    plan.info.mode = "none";
  }
  plan.warnings.push_back(fallbackWarning(reason, wantSwitch, wantWake));
  return plan;
}

SchedPlan planSched(const fs::path& tracefsRoot, bool wantSwitch, bool wantWake) {
  SchedPlan plan;
  if (!wantSwitch && !wantWake) return plan;
  if (tracefsRoot.empty())
    return fallbackSchedPlan(wantSwitch, wantWake,
                             "tracefs was not found under /sys/kernel/tracing or /sys/kernel/debug/tracing, "
                             "or is not accessible");

  Loaded sw, wake;
  std::string why;
  std::string wakeEvent;
  if (wantSwitch) {
    if (!loadFormat(tracefsRoot, "sched_switch", sw, why))
      return fallbackSchedPlan(wantSwitch, wantWake, why);
    auto dec = SchedSwitchDecoder::create(sw.format);
    if (!dec.ok()) return fallbackSchedPlan(wantSwitch, wantWake, dec.error().message);
  }
  std::string wakeProblem;
  if (wantWake) {
    for (const char* name : {"sched_waking", "sched_wakeup"}) {
      std::string problem;
      Loaded candidate;
      if (!loadFormat(tracefsRoot, name, candidate, problem)) {
        wakeProblem += (wakeProblem.empty() ? "" : "; ") + problem;
        continue;
      }
      auto dec = SchedWakeDecoder::create(candidate.format);
      if (!dec.ok()) {
        wakeProblem += (wakeProblem.empty() ? "" : "; ") + std::string(name) + ": " + dec.error().message;
        continue;
      }
      wake = std::move(candidate);
      wakeEvent = name;
      break;
    }
    if (wakeEvent.empty() && !wantSwitch) return fallbackSchedPlan(wantSwitch, wantWake, wakeProblem);
  }

  plan.info.mode = "tracepoint";
  if (wantSwitch) {
    plan.info.switches = true;
    plan.info.switchFormat = sw.text;
    plan.switchId = sw.format.id;
  }
  if (!wakeEvent.empty()) {
    plan.info.wakeups = true;
    plan.info.wakeFormat = wake.text;
    plan.info.wakeEvent = wakeEvent;
    plan.wakeId = wake.format.id;
    if (wakeEvent == "sched_wakeup")
      plan.warnings.push_back(
          "sched:sched_waking is unavailable here; sched_wakeup is used for ready-thread events, and for a "
          "wakeup delivered by an inter-processor interrupt it is recorded on the woken task's cpu, so that "
          "waker attribution is approximate");
  } else if (wantWake) {
    plan.warnings.push_back("wakeup tracepoints are unavailable (" + wakeProblem +
                            "): ready-thread events and unblocking stacks are not recorded");
  }
  return plan;
}

}  // namespace tachyon::capture::perf
