// Trace exporters (WP-63): speedscope JSON and Chrome trace event JSON (Perfetto, chrome://tracing).
//
// The library lives in libs/export; its namespace is tachyon::exporters because `export` is a C++20 keyword.
// Both exporters stream their JSON (no document tree), are deterministic (the same model, symbols and filter
// give byte-identical output) and need TraceModel::statesByThread built (analysis::buildThreadStates).
//
// Filter semantics (both exporters): `t0`/`t1` are the half-open range [t0, t1) on event timestamps (a scope
// is selected by its begin), `threads` restricts the per-thread data (nullopt = all threads). `subtreeRoot`
// and `function` are not supported and make the exporters fail with ErrorCode::Unsupported.
#pragma once

#include "analysis/query.h"
#include "core/result.h"
#include "model/model.h"
#include "symbols/symbol_service.h"

#include <cstdint>
#include <iosfwd>

namespace tachyon::exporters {

struct ExportStats {
  uint64_t threads = 0;  // speedscope: profiles written; chrome: threads with metadata
  uint64_t samples = 0;  // speedscope: exported (non-zero-weight) samples
  uint64_t frames = 0;   // speedscope: entries of the shared frame table
  uint64_t events = 0;   // chrome: entries of traceEvents (metadata included)
};

// Thread-safe for concurrent const access to the context and distinct output streams.
//
// speedscope file format (https://www.speedscope.app/file-format-schema.json): one "sampled" profile per
// selected thread that has at least one sample with a non-zero rule-1 weight (3.8) in the range; the profile
// is named "<thread name> (<tid>)". Samples are root-to-leaf lists of indices into the shared frame table,
// weights are the rule-1 sample weights in nanoseconds (zero-weight samples are omitted), startValue and
// endValue are nanoseconds since the trace start (the filter window clamped to the trace; endValue is raised
// to startValue plus the weight sum when the weights exceed the window). A frame is a physical function
// (FunctionKey) named by its symbol, with the file and first line of the function when known, and
// `module!0x<rva>` (mapped) or `0x<address>` (unmapped) otherwise. Frame keys follow the return-address rule:
// the leaf frame, and the interrupted frame of a kernel-leaf sample, are looked up at their exact address,
// every other frame at its return address minus one. The context's inline, kernel-frame and scope-blending
// options are not applied (physical frames only).
// Fails with InvalidArgument for a model whose thread states are not built, an out-of-range thread index or a
// zero clock frequency, and with IoError when the stream fails.
Result<ExportStats> exportSpeedscope(const analysis::QueryContext& ctx, const analysis::Filter& filter,
                                     std::ostream& out);

// Thread-safe for concurrent const access to the model and distinct output streams.
//
// Chrome trace event JSON: {"traceEvents":[...],"displayTimeUnit":"ns"}, timestamps in microseconds since
// the trace start (ns resolution). Contents, in this order:
//  * ph "M" metadata: process_name / thread_name of the selected threads, and a pseudo-process "CPU" with one
//    thread "CPU n" per CPU that ran a selected thread;
//  * per selected thread (ascending ThreadIdx), sorted by timestamp: scopes as ph "X" (paired by
//    analysis::pairScopes; an unterminated scope ends at the trace end and carries args.unterminated=true),
//    frame marks and markers as thread-scoped instants (ph "i", s "t");
//  * the CPU tracks: one ph "X" slice per Running interval of a selected thread, named after the thread,
//    on pid = the CPU pseudo-process, tid = cpu, clipped to the range;
//  * markers without a thread as global instants (s "g") and plot series as counters (ph "C", args.value).
// Global markers and plots are not restricted by `threads`. `symbols` is accepted for symmetry with the
// speedscope exporter and is currently unused.
// Fails with InvalidArgument for a model whose thread states are not built, an out-of-range thread index or a
// zero clock frequency, and with IoError when the stream fails.
Result<ExportStats> exportChromeTrace(const model::TraceModel& model, const symbols::SymbolService& symbols,
                                      const analysis::Filter& filter, std::ostream& out);

}  // namespace tachyon::exporters
