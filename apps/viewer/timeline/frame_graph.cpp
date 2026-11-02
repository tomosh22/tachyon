#include "viewer/timeline/frame_graph.h"

#include "viewer/analysis_context.h"
#include "viewer/common/query_filter.h"
#include "viewer/common/rect_batch.h"
#include "viewer/document.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tachyon::viewer::timeline {

// ---------------------------------------------------------------------------------------------------------
// Pure helpers

FrameBars barsFrom(const std::vector<analysis::FrameSpan>& spans, const analysis::FrameSeries& series) {
  FrameBars bars;
  bars.series = series;
  bars.start.reserve(spans.size());
  bars.end.reserve(spans.size());
  bars.number.reserve(spans.size());
  for (const analysis::FrameSpan& s : spans) {
    bars.start.push_back(s.start);
    bars.end.push_back(s.end);
    bars.number.push_back(s.number);
    bars.maxDuration = std::max(bars.maxDuration, s.end - s.start);
  }
  return bars;
}

double FrameBudgets::msFor(const analysis::FrameSeries& series) const {
  for (const Entry& e : m_overrides) {
    if (e.thread == series.thread.get() && e.name == series.name.get()) return e.ms;
  }
  return m_default;
}

bool FrameBudgets::overridden(const analysis::FrameSeries& series) const {
  for (const Entry& e : m_overrides) {
    if (e.thread == series.thread.get() && e.name == series.name.get()) return true;
  }
  return false;
}

void FrameBudgets::set(const analysis::FrameSeries& series, double ms) {
  for (size_t i = 0; i < m_overrides.size(); ++i) {
    Entry& e = m_overrides[i];
    if (e.thread != series.thread.get() || e.name != series.name.get()) continue;
    if (ms > 0.0) {
      e.ms = ms;
    } else {
      m_overrides.erase(m_overrides.begin() + static_cast<ptrdiff_t>(i));
    }
    return;
  }
  if (ms > 0.0) m_overrides.push_back({series.thread.get(), series.name.get(), ms});
}

uint64_t msToTicks(double ms, double clockHz) {
  const double ticks = ms * 1e-3 * clockHz;
  if (!(ticks > 0.0)) return 0;
  return static_cast<uint64_t>(std::min(ticks, 9.0e18));
}

void layoutFrameBars(const FrameBars& bars, const PlotArea& area, uint64_t budgetTicks,
                     std::vector<FrameBarItem>& out) {
  out.clear();
  const size_t n = bars.size();
  const double scale = area.view.span > 0.0 ? static_cast<double>(area.width) / area.view.span : 0.0;
  if (n == 0 || !(scale > 0.0)) return;
  const double base = static_cast<double>(area.limits.traceStart);
  const auto xOf = [&](uint64_t t) {
    return static_cast<double>(area.x0) + ((static_cast<double>(t) - base) - area.view.start) * scale;
  };
  const double xLeft = area.x0, xRight = static_cast<double>(area.x0) + area.width;
  const double tA = base + area.view.start, tB = base + area.view.end();
  const uint64_t tAFloor = tA > 0.0 ? static_cast<uint64_t>(std::floor(tA)) : 0;
  size_t pos = static_cast<size_t>(std::upper_bound(bars.end.begin(), bars.end.end(), tAFloor) -
                                   bars.end.begin());  // first frame ending after the view start
  while (pos < n && static_cast<double>(bars.start[pos]) < tB) {
    const double xa = xOf(bars.start[pos]), xb = xOf(bars.end[pos]);
    FrameBarItem it;
    if (xb - xa >= 1.0) {
      it.x0 = static_cast<float>(std::max(xa, xLeft));
      it.x1 = static_cast<float>(std::min(xb, xRight));
      it.first = it.tallest = static_cast<uint32_t>(pos);
      it.count = 1;
      it.duration = bars.duration(pos);
      ++pos;
    } else {
      const double px = std::floor(xa);
      const double edgeTick = tA + (px + 1.0 - area.x0) / scale;
      const uint64_t edge = edgeTick > 0.0 ? static_cast<uint64_t>(std::ceil(edgeTick)) : 0;
      size_t q = static_cast<size_t>(
          std::lower_bound(bars.start.begin() + static_cast<ptrdiff_t>(pos), bars.start.end(), edge) -
          bars.start.begin());
      q = std::max(q, pos + 1);
      if (q - pos > 1) {
        // Only the last frame of the pixel can reach beyond it (frames are contiguous).
        const size_t last = q - 1;
        if (xOf(bars.end[last]) - xOf(bars.start[last]) >= 1.0) q = last;
      }
      size_t tallest = pos;
      for (size_t i = pos + 1; i < q; ++i) {
        if (bars.duration(i) > bars.duration(tallest)) tallest = i;
      }
      it.x0 = static_cast<float>(std::max(px, xLeft));
      it.x1 = static_cast<float>(std::min(px + 1.0, xRight));
      if (it.x1 <= it.x0) it.x1 = it.x0 + 1.0f;
      it.first = static_cast<uint32_t>(pos);
      it.count = static_cast<uint32_t>(q - pos);
      it.tallest = static_cast<uint32_t>(tallest);
      it.duration = bars.duration(tallest);
      pos = q;
    }
    it.overBudget = budgetTicks > 0 && it.duration > budgetTicks;
    out.push_back(it);
  }
}

uint64_t yAxisMax(uint64_t maxVisibleDuration, uint64_t budgetTicks) {
  return std::max<uint64_t>({maxVisibleDuration, budgetTicks + budgetTicks / 4, 1});
}

std::optional<size_t> barAt(const std::vector<FrameBarItem>& items, float x) {
  // Items are sorted by x0 and do not overlap.
  const auto it = std::upper_bound(items.begin(), items.end(), x,
                                   [](float v, const FrameBarItem& b) { return v < b.x0; });
  if (it == items.begin()) return std::nullopt;
  const FrameBarItem& b = *(it - 1);
  if (x < b.x0 || x >= b.x1) return std::nullopt;
  return static_cast<size_t>((it - 1) - items.begin());
}

std::vector<std::string> frameBarTooltipLines(const FrameBars& bars, const FrameBarItem& item, double clockHz,
                                              uint64_t budgetTicks, const std::string& seriesName) {
  std::vector<std::string> lines;
  if (item.tallest >= bars.size()) return lines;
  char buf[200];
  const auto fmt = [&](uint64_t ticks) { return formatDurationTicks(ticks, clockHz); };
  if (item.count > 1) {
    std::snprintf(buf, sizeof buf, "%u frames in this pixel (zoom in to separate them)", item.count);
    lines.push_back(buf);
    std::snprintf(buf, sizeof buf, "slowest: frame %llu, %s",
                  static_cast<unsigned long long>(bars.number[item.tallest]), fmt(item.duration).c_str());
  } else {
    std::snprintf(buf, sizeof buf, "%s frame %llu: %s", seriesName.c_str(),
                  static_cast<unsigned long long>(bars.number[item.tallest]), fmt(item.duration).c_str());
  }
  lines.push_back(buf);
  if (budgetTicks > 0) {
    if (item.duration > budgetTicks) {
      std::snprintf(buf, sizeof buf, "over budget by %s", fmt(item.duration - budgetTicks).c_str());
    } else {
      std::snprintf(buf, sizeof buf, "within budget (%s)", fmt(budgetTicks).c_str());
    }
    lines.push_back(buf);
  }
  lines.push_back("Click: zoom to the frame");
  return lines;
}

FrameJump jumpTargetOf(const analysis::FrameSummary& frame) {
  return FrameJump{frame.markThread, frame.start, frame.end};
}

FrameJump jumpTargetOf(const FrameBars& bars, size_t frame) {
  if (frame >= bars.size()) return {};
  return FrameJump{bars.series.thread, bars.start[frame], bars.end[frame]};
}

std::string seriesLabel(const model::TraceModel& model, const analysis::FrameSeries& series) {
  std::string name(model.strings.get(series.name));
  if (name.empty()) name = "Frame";
  if (series.thread.valid() && series.thread.get() < model.threads.size()) {
    const std::string& t = model.threads[series.thread.get()].name;
    return name + " (" + (t.empty() ? "thread" : t) + ")";
  }
  return name + " (global)";
}

// ---------------------------------------------------------------------------------------------------------
// The view

void FrameGraphView::reset(double defaultBudgetMs) {
  m_query.cancel();
  m_slowQuery.cancel();
  m_data.reset();
  m_wanted.reset();
  m_haveRequested = false;
  m_requestedDocument = 0;
  m_requestedSeries.reset();
  m_error.clear();
  m_budgets = FrameBudgets(defaultBudgetMs);
  m_items.clear();
  m_drawn.clear();
  m_tooltip.clear();
  m_canvas = {};
  m_slowestButton = {};
  m_overBudget = 0;
  m_axisMax = 0;
}

void FrameGraphView::inject(std::shared_ptr<const FrameGraphData> data) {
  m_data = std::move(data);
}

void FrameGraphView::selectSeries(size_t index) {
  if (!m_data || index >= m_data->series.size()) return;
  m_wanted = m_data->series[index];
}

void FrameGraphView::issue(const PanelContext& context) {
  const Document& doc = *context.document;
  const analysis::AggregationOptions options;
  std::shared_ptr<const AnalysisSnapshot> snapshot =
      doc.analysis().snapshot(options, context.symbolGeneration, context.queryExecutor);
  if (!snapshot) {
    m_error = "Analysis is not available for this document.";
    return;
  }
  m_haveRequested = true;
  m_requestedDocument = context.documentGeneration;
  m_requestedSeries = m_wanted;
  const std::optional<analysis::FrameSeries> wanted = m_wanted;
  analysis::QueryExecutor& executor = *context.queryExecutor;
  using Value = std::shared_ptr<const FrameGraphData>;
  m_query.issue(executor, [&](const analysis::SubmitOptions& so) {
    return doc.analysis().submit<Value>(
        executor, so, snapshot,
        [wanted](const analysis::QueryContext& qc, analysis::Cancel) -> Result<Value> {
          auto data = std::make_shared<FrameGraphData>();
          data->series = analysis::frameSeries(qc);
          if (data->series.empty()) return Value(std::move(data));
          if (wanted) {
            for (size_t i = 0; i < data->series.size(); ++i) {
              if (data->series[i].sameSeries(*wanted)) data->selected = i;
            }
          }
          const analysis::FrameSeries& chosen = data->series[data->selected];
          data->bars = barsFrom(analysis::frameSpans(qc.model(), chosen), chosen);
          return Value(std::move(data));
        });
  });
}

void FrameGraphView::update(const PanelContext& context) {
  if (!context.document || !context.queryExecutor) return;
  if (auto result = m_query.poll()) {
    if (result->ok()) {
      m_data = result->value();
      m_error.clear();
    } else {
      m_error = result->error().toString();
    }
    context.requestRedraw();
  }
  if (auto slow = m_slowQuery.poll()) {
    if (slow->ok() && !slow->value()->empty() && context.selection) {
      const FrameJump j = jumpTargetOf(slow->value()->front());
      context.selection->requestNavigation(j.thread, j.t0, j.t1);
    }
    context.requestRedraw();
  }
  const bool sameSeries = (!m_wanted && !m_requestedSeries) ||
                          (m_wanted && m_requestedSeries && m_wanted->sameSeries(*m_requestedSeries));
  if (m_haveRequested && m_requestedDocument == context.documentGeneration && sameSeries) {
    if (m_query.pending() || m_slowQuery.pending()) context.requestRedraw();
    return;
  }
  issue(context);
}

void FrameGraphView::jumpToSlowest(const PanelContext& context) {
  if (!context.document || !context.queryExecutor || !context.selection || !m_data || m_data->series.empty())
    return;
  const Document& doc = *context.document;
  const analysis::AggregationOptions options;
  std::shared_ptr<const AnalysisSnapshot> snapshot =
      doc.analysis().snapshot(options, context.symbolGeneration, context.queryExecutor);
  if (!snapshot) return;
  analysis::Filter filter = common::toQueryFilter(context.selection->state().timeFilter);
  filter.subtreeRoot.reset();  // the Call Graph's subtree does not choose which frame is the slowest
  analysis::FrameOptions fo;
  fo.series = m_data->series[m_data->selected];
  fo.topFunctions = 0;
  analysis::QueryExecutor& executor = *context.queryExecutor;
  using Value = std::shared_ptr<const std::vector<analysis::FrameSummary>>;
  m_slowQuery.issue(executor, [&](const analysis::SubmitOptions& so) {
    return doc.analysis().submit<Value>(
        executor, so, snapshot,
        [filter, fo](const analysis::QueryContext& qc, analysis::Cancel cancel) -> Result<Value> {
          return analysis::slowestFramesShared(qc, filter, 1, fo, cancel);
        });
  });
  context.requestRedraw();
}

void FrameGraphView::gotoFrame(const PanelContext& context, size_t frame) {
  if (!m_data || !context.selection) return;
  const FrameJump j = jumpTargetOf(m_data->bars, frame);
  if (j.t1 > j.t0) context.selection->requestNavigation(j.thread, j.t0, j.t1);
}

void FrameGraphView::draw(const PanelContext& context, const model::TraceModel& model, const PlotArea& area,
                          float gutterX0, float height) {
  m_drawn.clear();
  m_tooltip.clear();
  m_overBudget = 0;
  m_slowestButton = {};
  m_clockHz = area.clockHz;
  const float em = ImGui::GetFontSize();

  // Controls line: series, budget, jump button, summary.
  ImGui::PushID("framegraph");
  const analysis::FrameSeries* series =
      m_data && !m_data->series.empty() ? &m_data->series[m_data->selected] : nullptr;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Frames");
  ImGui::SameLine();
  if (series) {
    ImGui::SetNextItemWidth(em * 11.0f);
    if (ImGui::BeginCombo("##series", seriesLabel(model, *series).c_str())) {
      for (size_t i = 0; i < m_data->series.size(); ++i) {
        const std::string label = seriesLabel(model, m_data->series[i]) + " - " +
                                  std::to_string(m_data->series[i].markCount) + " marks";
        if (ImGui::Selectable(label.c_str(), i == m_data->selected)) selectSeries(i);
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    float ms = static_cast<float>(m_budgets.msFor(*series));
    ImGui::SetNextItemWidth(em * 6.0f);
    if (ImGui::DragFloat("Budget ms", &ms, 0.1f, 0.1f, 1000.0f, "%.2f")) m_budgets.set(*series, ms);
    if (m_budgets.overridden(*series)) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Default")) m_budgets.set(*series, 0.0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Slowest frame in filter")) jumpToSlowest(context);
    const ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
    m_slowestButton = {bmin.x, bmin.y, bmax.x, bmax.y, true};
    ImGui::SameLine();
    ImGui::TextDisabled("%zu frames, longest %s", m_data->bars.size(),
                        formatDurationTicks(m_data->bars.maxDuration, m_clockHz).c_str());
  } else if (!m_error.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_error.c_str());
  } else if (m_data) {
    ImGui::TextDisabled("(no frame marks in this trace)");
  } else {
    ImGui::TextDisabled("(loading frames...)");
  }

  // Canvas.
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float fullWidth = area.x0 - gutterX0 + area.width;
  ImGui::InvisibleButton("##canvas", ImVec2(fullWidth, height));
  const bool hovered = ImGui::IsItemHovered();
  const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
  m_canvas = {p0.x, p0.y, p0.x + fullWidth, p0.y + height, true};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float x0 = area.x0, x1 = area.x0 + area.width;
  const float top = p0.y + 2.0f, bottom = p0.y + height - 2.0f;
  dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p0.y + height), IM_COL32(22, 22, 27, 255));
  dl->AddText(ImVec2(gutterX0 + 4.0f, p0.y + 2.0f), IM_COL32(170, 170, 175, 255), "Frame times");

  if (series && m_data->bars.size() > 0) {
    const uint64_t budgetTicks = msToTicks(m_budgets.msFor(*series), m_clockHz);
    layoutFrameBars(m_data->bars, area, budgetTicks, m_items);
    uint64_t maxVisible = 0;
    for (const FrameBarItem& it : m_items) maxVisible = std::max(maxVisible, it.duration);
    m_axisMax = yAxisMax(maxVisible, budgetTicks);
    const float usable = bottom - top;
    const auto heightOf = [&](uint64_t d) {
      return std::max(1.0f,
                      usable * static_cast<float>(static_cast<double>(d) / static_cast<double>(m_axisMax)));
    };
    const std::optional<size_t> hover =
        hovered ? barAt(m_items, ImGui::GetIO().MousePos.x) : std::optional<size_t>();
    {
      common::RectBatch batch(dl, m_items.size());
      for (size_t i = 0; i < m_items.size(); ++i) {
        const FrameBarItem& it = m_items[i];
        const float h = heightOf(it.duration);
        const bool over = it.overBudget;
        ImU32 color = over ? IM_COL32(225, 80, 65, 255) : IM_COL32(70, 130, 200, 255);
        if (hover && *hover == i) color = over ? IM_COL32(255, 130, 110, 255) : IM_COL32(120, 175, 240, 255);
        const float gap = it.x1 - it.x0 > 4.0f ? 1.0f : 0.0f;
        batch.add(it.x0, bottom - h, it.x1 - gap, bottom, color);
        if (over) ++m_overBudget;
        DrawnFrameBar d;
        d.x0 = it.x0;
        d.x1 = it.x1 - gap;
        d.y0 = bottom - h;
        d.y1 = bottom;
        d.first = it.first;
        d.count = it.count;
        d.tallest = it.tallest;
        d.overBudget = over;
        m_drawn.push_back(d);
      }
    }
    // Budget line.
    if (budgetTicks > 0) {
      m_budgetY = bottom - heightOf(budgetTicks);
      dl->AddLine(ImVec2(x0, m_budgetY), ImVec2(x1, m_budgetY), IM_COL32(240, 200, 60, 230), 1.0f);
      char text[48];
      std::snprintf(text, sizeof text, "%.2f ms", m_budgets.msFor(*series));
      dl->AddText(ImVec2(x1 - ImGui::CalcTextSize(text).x - 4.0f, m_budgetY - em),
                  IM_COL32(240, 200, 60, 255), text);
    }
    if (hover) {
      m_tooltip = frameBarTooltipLines(m_data->bars, m_items[*hover], m_clockHz, budgetTicks,
                                       std::string(model.strings.get(series->name)));
      ImGui::BeginTooltip();
      for (const std::string& line : m_tooltip) ImGui::TextUnformatted(line.c_str());
      ImGui::EndTooltip();
      if (clicked) gotoFrame(context, m_items[*hover].tallest);
    }
  }
  // The visible window of the timeline is the rows' business; the strip shows the same time span as they do.
  dl->AddLine(ImVec2(gutterX0, p0.y + height), ImVec2(x1, p0.y + height), IM_COL32(70, 70, 76, 255));
  ImGui::PopID();
}

}  // namespace tachyon::viewer::timeline
