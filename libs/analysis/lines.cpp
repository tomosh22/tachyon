// linesForFunction (WP-34) and the matcher shared with instructionsForFunction.
#include "analysis/lines.h"

#include "analysis/query_executor.h"

#include <algorithm>
#include <unordered_map>

namespace tachyon::analysis {

namespace detail {

// ---------------------------------------------------------------------------------------------------------
// FunctionMatcher

FunctionMatcher::FunctionMatcher(const QueryContext& ctx, FunctionKey target, CallstackIdx subtree)
    : m_ctx(ctx), m_model(ctx.model()), m_target(target) {
  if (subtree.valid() && subtree.get() != 0) m_subtree = subtree.get();
  if (target.module == kScopeModule || target == kRootKey || target == kKernelKey) m_matchable = false;
  m_inline = target.isInline();
  if (m_inline) m_inlineName = static_cast<uint32_t>(target.value & ~kInlineKeyBit);
}

bool FunctionMatcher::frameMatches(FrameIdx fi, bool caller) const {
  if (!fi.valid() || fi.get() >= m_model.frames.size()) return false;
  const model::Frame& f = m_model.frames[fi.get()];
  if (!m_target.module.valid()) return !f.module.valid() && f.addr_or_rva == m_target.value;
  if (f.module != m_target.module) return false;
  const symbols::ResolvedAddress& ra = caller ? m_ctx.symbols().lookupCaller(fi) : m_ctx.symbols().lookup(fi);
  if (m_inline) {
    if (!ra.resolved) return false;
    for (const symbols::InlineFrame& in : ra.inlines)
      if (in.function.get() == m_inlineName) return true;
    return false;
  }
  if (ra.resolved) return ra.functionStartRva == m_target.value;
  return f.addr_or_rva == m_target.value;  // unresolved key: grouped per {module, rva}
}

int64_t FunctionMatcher::outermostCaller(uint32_t node) {
  const size_t limit = m_model.nodes.size();
  if (m_memo.size() != limit) m_memo.assign(limit, kUnknown);
  m_work.clear();
  int64_t base = kNone;
  uint32_t cur = node;
  while (m_work.size() <= limit) {
    if (cur == 0 || cur >= limit) {
      base = kNone;
      break;
    }
    if (m_memo[cur] != kUnknown) {
      base = m_memo[cur];
      break;
    }
    m_work.push_back(cur);
    if (cur == m_subtree) {
      base = kNone;
      break;
    }
    cur = m_model.nodes[cur].parent.get();
  }
  for (size_t i = m_work.size(); i-- > 0;) {
    const uint32_t x = m_work[i];
    int64_t r = base;
    if (r == kNone) {
      const FrameIdx fi = m_model.nodes[x].frame;
      if (fi.valid() && frameMatches(fi, true)) r = fi.get();
    }
    m_memo[x] = r;
    base = r;
  }
  return base;
}

std::optional<FunctionHit> FunctionMatcher::matchWalk(uint32_t leaf, bool kernelLeaf, uint32_t cut) {
  // Rare paths (kernel-leaf samples, subtree paths): walk the stack, leaf first, up to the cut.
  std::vector<uint32_t> chain;
  const size_t limit = m_model.nodes.size();
  for (uint32_t n = leaf; n != 0 && n < limit && chain.size() <= limit; n = m_model.nodes[n].parent.get()) {
    chain.push_back(n);
    if (n == cut) break;
  }
  size_t exactAt = 0;
  while (kernelLeaf && exactAt < chain.size()) {
    const FrameIdx fi = m_model.nodes[chain[exactAt]].frame;
    if (!(fi.valid() && fi.get() < m_model.frames.size() && isKernelFrame(m_model, m_model.frames[fi.get()])))
      break;
    ++exactAt;
  }
  for (size_t i = chain.size(); i-- > 0;) {  // outermost first
    const FrameIdx fi = m_model.nodes[chain[i]].frame;
    const bool caller = i != 0 && i != exactAt;
    if (frameMatches(fi, caller)) return FunctionHit{fi, caller};
  }
  return std::nullopt;
}

std::optional<FunctionHit> FunctionMatcher::match(CallstackIdx leaf, bool kernelLeaf) {
  if (!m_matchable || !leaf.valid()) return std::nullopt;
  const uint32_t n = leaf.get();
  if (n == 0 || n >= m_model.nodes.size()) return std::nullopt;
  if (kernelLeaf) return matchWalk(n, true, m_subtree);
  if (n != m_subtree) {
    const int64_t up = outermostCaller(m_model.nodes[n].parent.get());
    if (up >= 0) return FunctionHit{FrameIdx(static_cast<uint32_t>(up)), true};
  }
  const FrameIdx fi = m_model.nodes[n].frame;
  if (frameMatches(fi, false)) return FunctionHit{fi, false};
  return std::nullopt;
}

std::optional<FunctionHit> FunctionMatcher::matchWithin(CallstackIdx leaf, bool kernelLeaf, uint32_t cut) {
  if (!m_matchable || !leaf.valid() || cut == 0) return std::nullopt;
  const uint32_t n = leaf.get();
  if (n == 0 || n >= m_model.nodes.size()) return std::nullopt;
  return matchWalk(n, kernelLeaf, cut);
}

uint64_t FunctionMatcher::addressOf(const FunctionHit& hit) const {
  return m_model.frames[hit.frame.get()].addr_or_rva;
}

void FunctionMatcher::ensureOwnLines() {
  if (m_ownLoaded) return;
  m_ownLoaded = true;
  if (m_inline || !m_target.module.valid() || m_target.module.get() >= m_model.modules.size()) return;
  symbols::ISymbolProvider* p = m_ctx.symbols().provider(m_target.module);
  if (p == nullptr) return;
  const symbols::ResolvedAddress start = p->resolve(m_target.value);
  if (!start.resolved || start.functionStartRva != m_target.value) return;
  m_file = start.file;
  m_haveFile = true;
  auto lines = p->linesForFunction(m_target.value);
  if (!lines) return;
  for (const symbols::LineRange& l : *lines)
    if (l.file == m_file && l.line != 0) m_ownLines.push_back(l);
  std::stable_sort(m_ownLines.begin(), m_ownLines.end(),
                   [](const symbols::LineRange& a, const symbols::LineRange& b) { return a.rva < b.rva; });
}

uint32_t FunctionMatcher::lineOf(const FunctionHit& hit) {
  const symbols::ResolvedAddress& ra =
      hit.caller ? m_ctx.symbols().lookupCaller(hit.frame) : m_ctx.symbols().lookup(hit.frame);
  if (!ra.resolved) return 0;
  if (m_inline) {
    for (size_t i = ra.inlines.size(); i-- > 0;)  // outermost occurrence of the inlined function
      if (ra.inlines[i].function.get() == m_inlineName) return ra.inlines[i].line;
    return 0;
  }
  if (!ra.inlines.empty() && ra.callSiteLine != 0 && ra.callSiteFile.valid()) {
    ensureOwnLines();
    if (!m_haveFile || ra.callSiteFile == m_file) return ra.callSiteLine;
  }
  if (!ra.file.valid()) return 0;  // no line row at all: report "unknown" instead of approximating
  ensureOwnLines();
  if (!m_haveFile || ra.file == m_file) return ra.line;
  // The line table points into another file (an inlined callee defined elsewhere): use the call site, which
  // we approximate with the nearest preceding line row of the function's own file.
  if (m_ownLines.empty()) return 0;
  const uint64_t rva = addressOf(hit) - (hit.caller ? 1 : 0);
  auto it = std::upper_bound(m_ownLines.begin(), m_ownLines.end(), rva,
                             [](uint64_t v, const symbols::LineRange& l) { return v < l.rva; });
  if (it != m_ownLines.begin()) --it;
  return it->line;
}

// ---------------------------------------------------------------------------------------------------------
// StackPass

StackPass::StackPass(const QueryContext& ctx, const ResolvedFilter& f)
    : m_trivial(!f.subtree.valid() && !f.function && !f.path),
      m_hasPath(f.path.has_value()),
      m_subtree(f.subtree),
      m_flat(ctx, m_table) {
  if (f.function) m_required = m_table.intern(*f.function);
  m_flat.setPath(f.path ? &*f.path : nullptr);
  if (!m_trivial) {
    m_memo[0].assign(ctx.model().nodes.size(), 0);
    m_memo[1].assign(ctx.model().nodes.size(), 0);
    if (m_hasPath) {
      m_cutMemo[0].assign(ctx.model().nodes.size(), 0);
      m_cutMemo[1].assign(ctx.model().nodes.size(), 0);
    }
  }
}

bool StackPass::pass(CallstackIdx node, bool kernelLeaf, Cancel cancel) {
  m_cost = 0;
  if (m_trivial) return true;
  const uint32_t n = node.valid() ? node.get() : 0;
  const size_t slot = n < m_memo[0].size() ? n : 0;
  uint8_t& memo = m_memo[kernelLeaf ? 1 : 0][slot];
  if (memo != 0) {
    if (m_hasPath && memo == 1) m_lastCut = m_cutMemo[kernelLeaf ? 1 : 0][slot];
    return memo == 1;
  }
  m_flat.setCancel(cancel);
  const bool ok = m_flat.flatten(node, m_subtree, kernelLeaf, m_ids, m_required);
  m_cost = m_flat.lastCost() + m_ids.size();
  if (m_flat.aborted()) {
    m_aborted = true;
    return false;
  }
  memo = ok ? 1 : 2;
  if (ok && m_hasPath) m_lastCut = m_cutMemo[kernelLeaf ? 1 : 0][slot] = m_flat.pathCutNode();
  return ok;
}

// ---------------------------------------------------------------------------------------------------------
// Weights

Result<std::shared_ptr<const std::vector<NodeWeight>>> nodeWeightsFor(const QueryContext& ctx,
                                                                      const ResolvedFilter& f,
                                                                      Cancel cancel) {
  using Ptr = std::shared_ptr<const std::vector<NodeWeight>>;
  if (f.t0 >= f.t1) return Ptr(std::make_shared<const std::vector<NodeWeight>>());
  // Same key as the aggregation queries' weight table (aggregation.cpp weightsKey): a table cached by them is
  // reused, and ours serves them. Only valid without scope blending, whose table is keyed by scope stack too.
  const bool shareable = !ctx.options().blendScopes;
  QueryKey key;
  key.kind = QueryKind::Weights;
  key.filter.t0 = f.canonical.t0;
  key.filter.t1 = f.canonical.t1;
  key.filter.threads = f.canonical.threads;
  key.options = f.options;
  if (shareable) {
    if (auto hit = ctx.cache().find<std::vector<NodeWeight>>(key)) return Ptr(hit);
  }
  auto acc = accumulateNodeWeights(ctx.model(), f.t0, f.t1, f.threads, ctx.options().threads, cancel,
                                   ctx.executor());
  if (!acc) return acc.error();
  auto value = std::make_shared<const std::vector<NodeWeight>>(std::move(*acc));
  if (shareable) {
    const size_t bytes = sizeof(std::vector<NodeWeight>) + 2 * kAllocOverhead + 32 + vectorBytes(*value);
    ctx.cache().insert(key, value, bytes);
  }
  return Ptr(value);
}

}  // namespace detail

namespace {

using namespace detail;

size_t linesBytes(const std::vector<LineAgg>& v) {
  return sizeof(std::vector<LineAgg>) + 2 * kAllocOverhead + 32 + vectorBytes(v);
}

}  // namespace

Result<std::shared_ptr<const std::vector<LineAgg>>> linesForFunctionShared(const QueryContext& ctx,
                                                                           const Filter& filter,
                                                                           FunctionKey key, Cancel cancel) {
  auto rf = resolveFilter(ctx, filter);
  if (!rf) return rf.error();
  return cachedOrCompute<std::vector<LineAgg>>(
      ctx, resultKey(*rf, QueryKind::Lines, key),
      [&]() -> Result<std::vector<LineAgg>> {
        FunctionMatcher matcher(ctx, key, rf->subtree);
        if (!matcher.matchable() || rf->t0 >= rf->t1) return std::vector<LineAgg>{};
        auto w = nodeWeightsFor(ctx, *rf, cancel);
        if (!w) return w.error();

        StackPass stackPass(ctx, *rf);
        CancelMeter meter{cancel};
        std::unordered_map<uint32_t, LineAgg> lines;
        auto slot = [&](uint32_t line) -> LineAgg& {
          LineAgg& l = lines[line];
          l.line = line;
          return l;
        };

        // Running samples.
        for (const NodeWeight& nw : **w) {
          if (meter.due()) return cancelledError();
          if (nw.ticks == 0) continue;
          const bool kept = stackPass.pass(nw.node, nw.kernelLeaf, cancel);
          if (stackPass.aborted()) return cancelledError();
          meter.add(stackPass.lastCost());
          if (!kept) continue;
          const std::optional<FunctionHit> hit =
              stackPass.hasPath() ? matcher.matchWithin(nw.node, nw.kernelLeaf, stackPass.cutNode())
                                  : matcher.match(nw.node, nw.kernelLeaf);
          if (!hit) continue;
          slot(matcher.lineOf(*hit)).runningTicks += nw.ticks;
        }

        // Waiting / Ready time through the blocking stacks.
        const model::TraceModel& m = ctx.model();
        for (ThreadIdx t : rf->threads) {
          const std::vector<model::ThreadStateInterval>& ivs = m.statesByThread[t.get()];
          size_t i = static_cast<size_t>(
              std::partition_point(ivs.begin(), ivs.end(),
                                   [&](const model::ThreadStateInterval& iv) { return iv.end <= rf->t0; }) -
              ivs.begin());
          for (; i < ivs.size() && ivs[i].start < rf->t1; ++i) {
            meter.add(1);
            if (meter.due()) return cancelledError();
            const model::ThreadStateInterval& iv = ivs[i];
            const bool waiting = iv.state == model::ThreadState::Waiting;
            if (!waiting && iv.state != model::ThreadState::Ready) continue;
            if (!iv.blocking_stack.valid() || iv.blocking_stack.get() == 0) continue;
            const uint64_t lo = std::max(iv.start, rf->t0), hi = std::min(iv.end, rf->t1);
            if (hi <= lo) continue;
            const bool kept = stackPass.pass(iv.blocking_stack, false, cancel);
            if (stackPass.aborted()) return cancelledError();
            meter.add(stackPass.lastCost());
            if (!kept) continue;
            const std::optional<FunctionHit> hit =
                stackPass.hasPath() ? matcher.matchWithin(iv.blocking_stack, false, stackPass.cutNode())
                                    : matcher.match(iv.blocking_stack, false);
            if (!hit) continue;
            LineAgg& l = slot(matcher.lineOf(*hit));
            (waiting ? l.waitingTicks : l.readyTicks) += hi - lo;
          }
        }

        std::vector<LineAgg> out;
        out.reserve(lines.size());
        for (auto& [line, l] : lines) {
          l.ticks = l.runningTicks + l.readyTicks + l.waitingTicks;
          if (l.ticks != 0) out.push_back(l);
        }
        std::sort(out.begin(), out.end(), [](const LineAgg& a, const LineAgg& b) { return a.line < b.line; });
        return out;
      },
      linesBytes);
}

Result<std::vector<LineAgg>> linesForFunction(const QueryContext& ctx, const Filter& filter, FunctionKey key,
                                              Cancel cancel) {
  auto r = linesForFunctionShared(ctx, filter, key, cancel);
  if (!r) return r.error();
  return std::vector<LineAgg>(**r);
}

}  // namespace tachyon::analysis
