// The "Function List" panel (WP-44): the bottom-up function table of the current filter (Function, Module,
// Inclusive ms / %, Exclusive ms / %, Instances), sortable by every column, with a case-insensitive text
// filter, a "restrict to call graph subtree" toggle and an optional grouping by module.
//
// Data flow. The panel follows the SelectionBus time filter. Whenever the effective query (filter, inline /
// kernel options, document and symbol generation) changes it asks Document::analysis() for the matching
// QueryContext and submits bottomUpFunctionsShared() to the app's QueryExecutor; the worker also builds the
// display Table (names, ranks, haystacks), so the main thread only sorts and filters. Each request
// supersedes the previous one (common::LatestQuery); results of superseded requests are dropped, and the
// previous table stays on screen, greyed, with a spinner while a request runs. Text filter and sort do not
// query: they re-derive the view from the cached table. A symbol generation change re-queries on a fresh
// QueryContext (FunctionKeys change when modules resolve) and finds the selection again by key; during the
// resolution burst re-queries are throttled to one per 200 ms.
//
// Selection. Clicking a row publishes it as the bus's selected function; a selection that comes from the bus
// scrolls to and highlights the row (clearing the text filter when it hides the row, expanding a collapsed
// module group).
#pragma once

#include "analysis/query.h"
#include "symbols/function_key.h"
#include "viewer/analysis_context.h"
#include "viewer/common/latest_query.h"
#include "viewer/functions/function_list_model.h"
#include "viewer/panel.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace tachyon::viewer {

class FunctionListPanel final : public IPanel {
 public:
  static constexpr const char* kName = "Function List";
  static constexpr const char* kCallGraphPanel = "Call Graph";
  static constexpr const char* kSourcePanel = "Source";

  using QuerySpec = functions::QuerySpec;
  using SortColumn = functions::SortColumn;
  using SortSpec = functions::SortSpec;

  FunctionListPanel();
  ~FunctionListPanel() override;

  // Main-thread only.
  const char* name() const override { return kName; }
  void draw(const PanelContext& context, bool* open) override;
  void onSelectionChanged(const PanelContext& context, const SelectionState& state,
                          SelectionField field) override;
  void onDocumentChanged(const Document* document) override;

  // Main-thread only. Delivers a finished query and starts one if the effective query changed. draw() calls
  // it first; headless tests call it directly.
  void update(const PanelContext& context);

  // ---- actions (UI and tests); main-thread only ----
  // Sets the text filter (the raw text as typed) and re-derives the view; no query.
  void setFilterText(const std::string& text);
  // Sets the sort; the ImGui table adopts it on the next frame.
  void setSort(SortSpec sort);
  void setRestrictToSubtree(bool on) { m_restrictSubtree = on; }
  void setGroupByModule(bool on);
  void setInlineExpansion(bool on) { m_inline = on; }
  void setKernelFrames(analysis::KernelFrames mode) { m_kernel = mode; }
  void toggleGroupCollapsed(size_t group);
  // Selects the table row `entry`, publishes its function on the bus.
  void selectEntry(const PanelContext& context, uint32_t entry);
  // Context-menu actions on a table row (also used by tests). Show* select the function on the bus first.
  void showInCallGraph(const PanelContext& context, uint32_t entry);
  void showSource(const PanelContext& context, uint32_t entry);
  void copyName(uint32_t entry);

  // ---- state (tests); main-thread only ----
  const std::shared_ptr<const functions::Table>& table() const { return m_table; }
  // Entry indices passing the text filter, in sort order.
  const std::vector<uint32_t>& view() const { return m_view; }
  const std::vector<functions::Group>& groups() const { return m_groups; }
  const std::vector<functions::Item>& items() const { return m_items; }
  const std::unordered_set<std::string>& collapsedModules() const { return m_collapsed; }
  const std::string& filterText() const { return m_filterText; }
  SortSpec sort() const { return m_sort; }
  bool restrictToSubtree() const { return m_restrictSubtree; }
  bool groupByModule() const { return m_grouped; }
  bool inlineExpansion() const { return m_inline; }
  analysis::KernelFrames kernelFrames() const { return m_kernel; }
  bool pending() const { return m_query.pending(); }
  uint64_t issuedQueries() const { return m_query.issued(); }
  uint64_t deliveredTables() const { return m_delivered; }
  const QuerySpec& requestedSpec() const { return m_requested; }
  const QuerySpec& shownSpec() const { return m_shown; }
  const std::optional<symbols::FunctionKey>& selectedKey() const { return m_selectedKey; }
  // Table row of the highlighted function; nullopt when it is not in the table.
  const std::optional<uint32_t>& selectedEntry() const { return m_selectedEntry; }
  const std::string& message() const { return m_message; }
  uint64_t focusRequests() const { return m_focusRequests; }
  // The row the context menu works on while it is open.
  const std::optional<uint32_t>& menuEntry() const { return m_menuEntry; }
  // Screen rectangles of the item rows drawn in the last frame.
  struct DrawnRow {
    size_t item;
    float x0, y0, x1, y1;
  };
  const std::vector<DrawnRow>& drawnRows() const { return m_drawnRows; }
  // Tests: replace the table as if a query had delivered it.
  void injectTable(std::shared_ptr<const functions::Table> table);
  // Tests: forget the throttle so the next update() re-queries immediately.
  void disableThrottle() { m_throttle = false; }

 private:
  using Clock = std::chrono::steady_clock;

  QuerySpec currentSpec(const PanelContext& context) const;
  void issue(const PanelContext& context, const QuerySpec& spec);
  void applyTable(std::shared_ptr<const functions::Table> table);
  void rebuildView();
  void rebuildItems();
  void resolveExternalSelection();
  void resetDocumentState();

  void drawToolbar(const PanelContext& context);
  void drawTable(const PanelContext& context, float height);
  void drawContextMenu(const PanelContext& context);

  common::LatestQuery<functions::Table> m_query;
  QuerySpec m_requested, m_shown;
  bool m_haveRequested = false;
  Clock::time_point m_lastIssue{};
  bool m_throttle = true;
  uint64_t m_delivered = 0;
  uint64_t m_documentGeneration = 0;
  bool m_haveDocument = false;
  double m_pendingClockHz = 1e9;
  std::string m_message;

  std::shared_ptr<const functions::Table> m_table;
  std::vector<uint32_t> m_view;
  std::vector<functions::Group> m_groups;
  std::vector<functions::Item> m_items;
  std::unordered_set<std::string> m_collapsed;

  std::string m_filterText;  // as typed
  std::string m_filterNorm;  // normalized
  SortSpec m_sort;
  bool m_pushSort = true;  // the ImGui table has to adopt m_sort
  bool m_restrictSubtree = true;
  bool m_grouped = false;
  bool m_inline = false;
  analysis::KernelFrames m_kernel = analysis::KernelFrames::Show;

  std::optional<symbols::FunctionKey> m_selectedKey;
  std::optional<uint32_t> m_selectedEntry;
  std::optional<symbols::FunctionKey> m_externalKey;  // bus selection waiting for a table to be found in
  bool m_scrollPending = false;

  std::optional<uint32_t> m_menuEntry;
  bool m_openMenu = false;
  std::vector<DrawnRow> m_drawnRows;
  bool m_focusPending = false;
  uint64_t m_focusRequests = 0;
};

}  // namespace tachyon::viewer
