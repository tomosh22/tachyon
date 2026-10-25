// The "Call Graph" panel (WP-43): the merged top-down call tree of the current filter as a virtualized tree
// table (Function, Module, Inclusive, Incl %, Exclusive, Excl %, Samples), a pie chart of the selected node's
// children, expand hot path / expand all / collapse all, and a context menu (filter to subtree, show in
// Function List, show source, copy name).
//
// Data flow. The panel follows the SelectionBus time filter (time range, threads, subtree root). Whenever the
// effective query (filter, inline / kernel options, document and symbol generation) changes it asks
// Document::analysis() for the matching QueryContext and submits topDownTreeShared() to the app's
// QueryExecutor (Foreground). Each request supersedes the previous one (callgraph::LatestQuery: cancel plus
// GenerationToken); results of superseded requests are dropped. While a request runs the previous tree stays
// on screen, greyed, with a spinner. Resolving symbols changes FunctionKeys, so every symbol generation
// change re-queries on a fresh QueryContext (AnalysisContext keeps one per (options, generation)); during
// the resolution burst re-queries are throttled to one per 200 ms.
#pragma once

#include "analysis/query.h"
#include "symbols/function_key.h"
#include "viewer/analysis_context.h"
#include "viewer/callgraph/callgraph_model.h"
#include "viewer/common/function_names.h"
#include "viewer/panel.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tachyon::viewer {

class CallGraphPanel final : public IPanel {
 public:
  static constexpr const char* kName = "Call Graph";
  static constexpr const char* kFunctionListPanel = "Function List";
  static constexpr const char* kSourcePanel = "Source";

  // What a row action works on, captured when the menu opens so that a re-query cannot change the target.
  struct ActionTarget {
    std::vector<symbols::FunctionKey> path;  // keys below the tree root
    symbols::FunctionKey key;
    std::string fullName;  // module!function, as copied
    bool isRoot = true;
  };

  // The effective query; the panel re-queries when it differs from the last one requested.
  struct QuerySpec {
    analysis::Filter filter;
    bool expandInline = false;
    analysis::KernelFrames kernelFrames = analysis::KernelFrames::Show;
    uint64_t documentGeneration = 0;
    uint64_t symbolGeneration = 0;
    bool operator==(const QuerySpec& o) const {
      return filter == o.filter && expandInline == o.expandInline && kernelFrames == o.kernelFrames &&
             documentGeneration == o.documentGeneration && symbolGeneration == o.symbolGeneration;
    }
  };

  using NameParts = common::NameParts;

  CallGraphPanel();
  ~CallGraphPanel() override;

  // Main-thread only.
  const char* name() const override { return kName; }
  void draw(const PanelContext& context, bool* open) override;
  void onSelectionChanged(const PanelContext& context, const SelectionState& state,
                          SelectionField field) override;
  void onDocumentChanged(const Document* document) override;

  // Main-thread only. Starts a query if the effective one changed and delivers a finished one. draw() calls
  // it first; headless tests call it directly.
  void update(const PanelContext& context);

  // ---- actions (menu, toolbar and tests); main-thread only ----
  void expandHotPath();  // from the selected row (the root without a selection)
  bool expandAllRows();  // false when the 100 k row cap cut the expansion
  void collapseAllRows();
  void toggleExpanded(size_t row);
  // Selects the row, publishes its function on the bus and shows its children in the pie chart.
  void selectRow(const PanelContext& context, size_t row);
  // The target of a row (path, key, full name); nullopt for an invalid row.
  std::optional<ActionTarget> targetOfRow(size_t row) const;
  // Sets the bus filter's subtree root to the callstack node behind `target`. False (with message()) when it
  // cannot be resolved or the target is the root.
  bool filterToSubtree(const PanelContext& context, const ActionTarget& target);
  // Removes the subtree restriction from the bus filter.
  void clearSubtreeFilter(const PanelContext& context);
  // Select the function on the bus and ask for the "Function List" panel.
  void showInFunctionList(const PanelContext& context, const ActionTarget& target);
  // Select the function on the bus and ask for the "Source" panel (a no-op if that panel does not exist).
  void showSource(const PanelContext& context, const ActionTarget& target);
  // Puts the full name on the ImGui clipboard.
  void copyName(const ActionTarget& target);
  void setInlineExpansion(bool on);
  void setKernelFrames(analysis::KernelFrames mode);
  void setPercentMode(callgraph::PercentMode mode) { m_percentMode = mode; }

  // ---- state (tests); main-thread only ----
  const std::shared_ptr<const analysis::NodeAgg>& tree() const { return m_tree; }
  const std::vector<callgraph::Row>& rows() const { return m_rows; }
  const std::vector<callgraph::PieSlice>& pie() const { return m_pie; }
  bool pending() const { return m_query.pending(); }
  uint64_t issuedQueries() const { return m_query.issued(); }
  uint64_t deliveredTrees() const { return m_delivered; }
  const QuerySpec& requestedSpec() const { return m_requested; }
  const QuerySpec& shownSpec() const { return m_shown; }
  const std::optional<callgraph::NodeId>& selectedId() const { return m_selectedId; }
  const std::optional<symbols::FunctionKey>& selectedKey() const { return m_selectedKey; }
  const callgraph::ExpandSet& expanded() const { return m_expanded; }
  callgraph::PercentMode percentMode() const { return m_percentMode; }
  bool inlineExpansion() const { return m_inline; }
  analysis::KernelFrames kernelFrames() const { return m_kernel; }
  const std::string& message() const { return m_message; }
  bool rowCapReached() const { return m_truncated; }
  // Number of panel focus requests (PanelRequest for this panel).
  uint64_t focusRequests() const { return m_focusRequests; }
  // Name parts of a key as the table shows them.
  const NameParts& nameOf(const symbols::FunctionKey& key);
  // Row index of the node with `id` among the visible rows, or SIZE_MAX.
  size_t rowOfId(callgraph::NodeId id) const;
  // Screen rectangles of the rows drawn in the last frame: {row, x0, y0, x1, y1}.
  struct DrawnRow {
    size_t row;
    float x0, y0, x1, y1;
    float arrowX1;  // right edge of the expand arrow hit area
  };
  const std::vector<DrawnRow>& drawnRows() const { return m_drawnRows; }
  // The menu target while the context menu is open.
  const std::optional<ActionTarget>& menuTarget() const { return m_menuTarget; }
  // Tests: replace the displayed tree as if a query had delivered it. `snapshot` provides the names.
  void injectTree(std::shared_ptr<const analysis::NodeAgg> tree,
                  std::shared_ptr<const AnalysisSnapshot> snapshot, double clockHz,
                  bool resetExpansion = true);
  // Tests: forget the throttle so the next update() re-queries immediately.
  void disableThrottle() { m_throttle = false; }

 private:
  using Clock = std::chrono::steady_clock;

  QuerySpec currentSpec(const PanelContext& context) const;
  void issue(const PanelContext& context, const QuerySpec& spec);
  void applyTree(std::shared_ptr<const analysis::NodeAgg> tree,
                 std::shared_ptr<const AnalysisSnapshot> snapshot, double clockHz, bool resetExpansion);
  void rebuildRows();
  void rebuildPie();
  void resolveExternalSelection();
  void resetDocumentState();
  void selectLocated(const callgraph::Located& at);

  void drawToolbar(const PanelContext& context);
  void drawTable(const PanelContext& context, float height);
  void drawPie(const PanelContext& context);
  void drawContextMenu(const PanelContext& context);

  callgraph::LatestQuery<analysis::NodeAgg> m_query;
  std::shared_ptr<const AnalysisSnapshot> m_pendingSnapshot, m_shownSnapshot;
  QuerySpec m_requested, m_shown;
  bool m_haveRequested = false;
  Clock::time_point m_lastIssue{};
  bool m_throttle = true;
  uint64_t m_delivered = 0;
  uint64_t m_documentGeneration = 0;
  bool m_haveDocument = false;
  double m_pendingClockHz = 1e9;
  std::string m_message;

  std::shared_ptr<const analysis::NodeAgg> m_tree;
  double m_clockHz = 1e9;
  callgraph::ExpandSet m_expanded;
  std::vector<callgraph::Row> m_rows;
  bool m_truncated = false;
  bool m_firstTree = true;

  std::optional<callgraph::NodeId> m_selectedId;
  std::vector<symbols::FunctionKey> m_selectedPath;
  std::optional<symbols::FunctionKey> m_selectedKey;
  std::optional<callgraph::NodeId> m_scrollTo;
  std::optional<symbols::FunctionKey> m_externalKey;  // bus selection waiting for a tree to be found in

  bool m_inline = false;
  analysis::KernelFrames m_kernel = analysis::KernelFrames::Show;
  callgraph::PercentMode m_percentMode = callgraph::PercentMode::OfRoot;

  std::unordered_map<symbols::FunctionKey, NameParts, symbols::FunctionKeyHash> m_names;
  std::vector<callgraph::PieSlice> m_pie;
  std::string m_pieTitle;

  std::optional<ActionTarget> m_menuTarget;
  bool m_openMenu = false;
  std::string m_subtreeLabel;
  CallstackIdx m_subtreeLabelNode;

  std::vector<DrawnRow> m_drawnRows;
  bool m_focusPending = false;
  uint64_t m_focusRequests = 0;
};

}  // namespace tachyon::viewer
