// The "Butterfly" panel (WP-44): for the SelectionBus's selected function, its callers above and its callees
// below the function's own row, with inclusive / exclusive ms and percentages (relative to the function's
// inclusive time), heaviest first. Clicking a caller or callee selects it (the bus's selected function), so
// the Butterfly and the Function List follow. A function at the base of a stack has the caller "<root>".
// Callers and callees are taken relative to the outermost occurrence of the function (recursion counted once,
// see docs/specs/analysis-query.md).
//
// Data flow as in the Function List: the bus filter and the selected function form the effective query;
// butterflyShared() runs on the app's QueryExecutor through Document::analysis(); the newest request wins
// (common::LatestQuery), the previous view stays on screen, greyed, while a request runs, and a symbol
// generation change re-queries (the selected key is looked up again by the query itself). A key of an inline
// frame or the merged kernel key forces the matching aggregation option on (functions::effectiveOptions).
#pragma once

#include "analysis/query.h"
#include "symbols/function_key.h"
#include "viewer/analysis_context.h"
#include "viewer/common/latest_query.h"
#include "viewer/functions/butterfly_model.h"
#include "viewer/functions/function_list_model.h"
#include "viewer/panel.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer {

class ButterflyPanel final : public IPanel {
 public:
  static constexpr const char* kName = "Butterfly";
  static constexpr const char* kCallGraphPanel = "Call Graph";
  static constexpr const char* kSourcePanel = "Source";

  // The result a query delivers: the display lines plus the clock for the ms columns.
  struct Data {
    functions::ButterflyView view;
    double clockHz = 1e9;
  };

  // The effective query: the function list's query spec plus the function.
  struct QuerySpec {
    functions::QuerySpec base;
    std::optional<symbols::FunctionKey> key;
    bool operator==(const QuerySpec&) const = default;
  };

  // One line of the table.
  struct Item {
    enum class Kind : uint8_t { CallersHeader, Caller, Self, CalleesHeader, Callee };
    Kind kind;
    uint32_t index = 0;  // into callers / callees
  };

  ButterflyPanel();
  ~ButterflyPanel() override;

  // Main-thread only.
  const char* name() const override { return kName; }
  void draw(const PanelContext& context, bool* open) override;
  void onSelectionChanged(const PanelContext& context, const SelectionState& state,
                          SelectionField field) override;
  void onDocumentChanged(const Document* document) override;

  // Main-thread only. Delivers a finished query and starts one if the effective query changed.
  void update(const PanelContext& context);

  // ---- actions (UI and tests); main-thread only ----
  void setRestrictToSubtree(bool on) { m_restrictSubtree = on; }
  void setInlineExpansion(bool on) { m_inline = on; }
  void setKernelFrames(analysis::KernelFrames mode) { m_kernel = mode; }
  // Click on a caller or callee line: selects its function on the bus. False for the <root> caller.
  bool navigate(const PanelContext& context, const functions::Line& line);
  void showInCallGraph(const PanelContext& context, const functions::Line& line);
  void showSource(const PanelContext& context, const functions::Line& line);
  void copyName(const functions::Line& line);

  // ---- state (tests); main-thread only ----
  const std::shared_ptr<const Data>& data() const { return m_data; }
  const std::vector<Item>& items() const { return m_items; }
  bool restrictToSubtree() const { return m_restrictSubtree; }
  bool inlineExpansion() const { return m_inline; }
  analysis::KernelFrames kernelFrames() const { return m_kernel; }
  bool pending() const { return m_query.pending(); }
  uint64_t issuedQueries() const { return m_query.issued(); }
  uint64_t deliveredResults() const { return m_delivered; }
  const QuerySpec& requestedSpec() const { return m_requested; }
  const QuerySpec& shownSpec() const { return m_shown; }
  const std::string& message() const { return m_message; }
  uint64_t focusRequests() const { return m_focusRequests; }
  const std::optional<functions::Line>& menuLine() const { return m_menuLine; }
  struct DrawnRow {
    size_t item;
    float x0, y0, x1, y1;
  };
  const std::vector<DrawnRow>& drawnRows() const { return m_drawnRows; }
  // Tests: replace the shown result as if a query had delivered it.
  void injectData(std::shared_ptr<const Data> result);
  // Tests: forget the throttle so the next update() re-queries immediately.
  void disableThrottle() { m_throttle = false; }

 private:
  using Clock = std::chrono::steady_clock;

  QuerySpec currentSpec(const PanelContext& context) const;
  void issue(const PanelContext& context, const QuerySpec& spec);
  void applyData(std::shared_ptr<const Data> result);
  void resetDocumentState();
  const functions::Line* lineOf(const Item& item) const;

  void drawToolbar(const PanelContext& context);
  void drawTable(const PanelContext& context, float height);
  void drawContextMenu(const PanelContext& context);

  common::LatestQuery<Data> m_query;
  QuerySpec m_requested, m_shown;
  bool m_haveRequested = false;
  Clock::time_point m_lastIssue{};
  bool m_throttle = true;
  uint64_t m_delivered = 0;
  uint64_t m_documentGeneration = 0;
  bool m_haveDocument = false;
  std::string m_message;

  std::shared_ptr<const Data> m_data;
  std::vector<Item> m_items;

  bool m_restrictSubtree = true;
  bool m_inline = false;
  analysis::KernelFrames m_kernel = analysis::KernelFrames::Show;

  std::optional<functions::Line> m_menuLine;
  bool m_openMenu = false;
  std::vector<DrawnRow> m_drawnRows;
  bool m_focusPending = false;
  uint64_t m_focusRequests = 0;
};

}  // namespace tachyon::viewer
