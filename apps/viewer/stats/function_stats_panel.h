// The "Function Statistics" panel (WP-47): a fuzzy search over all functions and instrumentation scope ids,
// and for the chosen entry the per-instance duration statistics (count, min, p25, median, p75, max, mean,
// total), a log2 histogram and a box plot. Every instance of the entry is highlighted in the timeline
// (overlay) and can be stepped through with Previous / Next (F3 / Shift+F3).
//
// Opening. Ctrl+F (App shortcut "stats.search", ignored while a text field has the keyboard) and any
// PanelRequest for "Function Statistics" open the panel, focus its window and put the keyboard in the search
// box.
//
// Data flow. Two queries run on the app's QueryExecutor through Document::analysis(), each guarded by its own
// common::LatestQuery (newest wins, superseded results are dropped):
//   * the search index: bottomUpFunctions of the whole trace (names, modules, sample counts) plus the scope
//     ids of the trace; rebuilt when the document or the symbol generation changes (FunctionKeys change when
//     modules resolve; during a resolution burst re-queries are throttled to one per 200 ms) and the
//     selection is found again by key, else by name;
//   * the statistics of the selected entry: functionStats / scopeStats and the instance list
//     (instances.h), under the bus time range and thread selection (the Call Graph subtree is ignored),
//     re-run when that filter, the selection or the symbol generation changes.
// Searching itself is synchronous on the main thread (a subsequence scan over the index, top 200).
//
// Bus. Choosing an entry publishes it as the bus's selected function (scopes as {kScopeModule, StringIdx});
// the delivered instance list is published as the bus's instance highlight (key + generation), which the
// TimelineWidget draws; stepping publishes the current instance, selects its thread and requests a
// navigation that centres it. "Populate from selection" takes the bus's selected function into the panel.
#pragma once

#include "analysis/instances.h"
#include "analysis/query.h"
#include "symbols/function_key.h"
#include "viewer/analysis_context.h"
#include "viewer/common/latest_query.h"
#include "viewer/panel.h"
#include "viewer/stats/stats_model.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer {

// What the statistics query delivers for the selected entry.
struct StatsResult {
  symbols::FunctionKey key;
  std::shared_ptr<const analysis::Stats> stats;
  std::shared_ptr<const std::vector<analysis::Instance>> instances;
};

class FunctionStatsPanel final : public IPanel {
 public:
  static constexpr const char* kName = "Function Statistics";

  // The effective statistics query; the panel re-queries when it differs from the last one requested.
  struct StatsSpec {
    symbols::FunctionKey key;
    analysis::Filter filter;
    analysis::AggregationOptions options;
    uint64_t documentGeneration = 0;
    uint64_t symbolGeneration = 0;
    bool operator==(const StatsSpec& o) const {
      return key == o.key && filter == o.filter && options.expandInline == o.options.expandInline &&
             options.kernelFrames == o.options.kernelFrames && documentGeneration == o.documentGeneration &&
             symbolGeneration == o.symbolGeneration;
    }
  };

  FunctionStatsPanel();
  ~FunctionStatsPanel() override;

  // Main-thread only.
  const char* name() const override { return kName; }
  void draw(const PanelContext& context, bool* open) override;
  void onSelectionChanged(const PanelContext& context, const SelectionState& state,
                          SelectionField field) override;
  void onDocumentChanged(const Document* document) override;

  // Main-thread only. Delivers finished queries and starts the ones the state requires. draw() calls it
  // first; headless tests call it directly.
  void update(const PanelContext& context);

  // ---- actions (UI and tests); main-thread only ----
  // Sets the search text as typed and re-runs the search (synchronously).
  void setQueryText(const std::string& text);
  // Main-thread only. Text that setQueryText() set and the search field has not shown yet (used by the
  // field's callback); consumed by the call.
  std::optional<std::string> takeEditorText();
  // Moves the result cursor by `delta` rows (clamped).
  void moveCursor(int delta);
  // Chooses result row `row` (Enter on the cursor row, click): selects its entry.
  void chooseResult(const PanelContext& context, size_t row);
  void chooseCursor(const PanelContext& context);
  // Selects an entry of the current index: shows its statistics and publishes it on the bus.
  void selectEntry(const PanelContext& context, uint32_t entry);
  // Takes the bus's selected function into the panel. Returns false if nothing is selected / no analysis.
  bool populateFromSelection(const PanelContext& context);
  // Steps to the next / previous instance (wraps), selecting its thread and navigating the timeline.
  // Return false when there is no instance list.
  bool nextInstance(const PanelContext& context);
  bool previousInstance(const PanelContext& context);
  // Goes to instance `index` of the delivered list.
  bool goToInstance(const PanelContext& context, uint32_t index);

  // ---- state (tests); main-thread only ----
  const std::shared_ptr<const stats::SearchIndex>& index() const { return m_index; }
  const std::string& queryText() const { return m_queryText; }
  const std::vector<stats::SearchHit>& hits() const { return m_hits; }
  int cursor() const { return m_cursor; }
  const std::optional<stats::SearchEntry>& selected() const { return m_selected; }
  // The delivered result of the selected entry (null while it is computed / nothing selected).
  const std::shared_ptr<const StatsResult>& result() const { return m_result; }
  const std::optional<uint32_t>& currentInstance() const { return m_current; }
  bool indexPending() const { return m_indexQuery.pending(); }
  bool statsPending() const { return m_statsQuery.pending(); }
  uint64_t issuedIndexQueries() const { return m_indexQuery.issued(); }
  uint64_t issuedStatsQueries() const { return m_statsQuery.issued(); }
  uint64_t deliveredResults() const { return m_deliveredResults; }
  const StatsSpec& requestedStatsSpec() const { return m_requestedStats; }
  // The spec of the delivered result.
  const StatsSpec& shownStatsSpec() const { return m_shownStats; }
  uint64_t requestedIndexSymbolGeneration() const { return m_requestedIndexSymbols; }
  uint64_t indexSymbolGeneration() const { return m_shownIndexSymbols; }
  // Number of PanelRequests for this panel seen / of times the search box was asked to take the keyboard.
  uint64_t focusRequests() const { return m_focusRequests; }
  // True while the search box was given the keyboard by the last frames (tests).
  bool searchActive() const { return m_searchActive; }
  double lastSearchMicros() const { return m_searchMicros; }
  const std::string& message() const { return m_message; }
  // Geometry drawn in the last frame (tests).
  const stats::HistogramLayout& lastHistogram() const { return m_histogram; }
  const stats::BoxPlotLayout& lastBoxPlot() const { return m_boxPlot; }
  struct DrawnRow {
    size_t hit;
    float x0, y0, x1, y1;
  };
  const std::vector<DrawnRow>& drawnRows() const { return m_drawnRows; }
  // Screen position of the Previous / Next buttons drawn in the last frame (tests click them).
  struct ButtonRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool drawn = false;
  };
  const ButtonRect& previousButton() const { return m_prevButton; }
  const ButtonRect& nextButton() const { return m_nextButton; }
  const ButtonRect& populateButton() const { return m_populateButton; }
  // Tests: forget the symbol-burst throttle.
  void disableThrottle() { m_throttle = false; }

 private:
  using Clock = std::chrono::steady_clock;

  StatsSpec statsSpecFor(const PanelContext& context, const stats::SearchEntry& entry) const;
  void issueIndex(const PanelContext& context);
  void issueStats(const PanelContext& context, const StatsSpec& spec);
  void applyIndex(const PanelContext& context, std::shared_ptr<const stats::SearchIndex> index);
  void applyResult(const PanelContext& context, std::shared_ptr<const StatsResult> result);
  void runSearch();
  void applyQueryText(const std::string& text);
  void resetDocumentState();
  void useEntry(const PanelContext& context, stats::SearchEntry entry, bool publishSelection);
  bool symbolBurst(const PanelContext& context) const;

  void drawSearch(const PanelContext& context);
  void drawResults(const PanelContext& context, float height);
  void drawStats(const PanelContext& context);
  void drawHistogram(double clockHz);
  void drawBoxPlot(double clockHz);
  void drawInstanceNav(const PanelContext& context);

  common::LatestQuery<stats::SearchIndex> m_indexQuery;
  common::LatestQuery<StatsResult> m_statsQuery;
  bool m_haveDocument = false;
  uint64_t m_documentGeneration = 0;
  uint64_t m_requestedIndexSymbols = 0;
  uint64_t m_shownIndexSymbols = 0;
  bool m_haveRequestedIndex = false;
  uint64_t m_requestedIndexDocument = 0;
  Clock::time_point m_lastIssue{};
  bool m_throttle = true;
  double m_clockHz = 1e9;
  uint64_t m_sampleInterval = 1;
  std::string m_message;

  std::shared_ptr<const stats::SearchIndex> m_index;
  std::string m_queryText;
  std::optional<std::string> m_editorText;  // see takeEditorText()
  std::vector<stats::SearchHit> m_hits;
  int m_cursor = -1;
  double m_searchMicros = 0;

  std::optional<stats::SearchEntry> m_selected;
  StatsSpec m_requestedStats, m_shownStats;
  bool m_haveRequestedStats = false;
  std::shared_ptr<const StatsResult> m_result;
  uint64_t m_deliveredResults = 0;
  std::optional<uint32_t> m_current;
  bool m_publishedHighlight = false;  // the bus holds the highlight of m_result

  bool m_focusWindow = false;
  bool m_focusSearch = false;
  bool m_searchActive = false;
  uint64_t m_focusRequests = 0;
  bool m_scrollToCursor = false;

  stats::HistogramLayout m_histogram;
  stats::BoxPlotLayout m_boxPlot;
  std::vector<DrawnRow> m_drawnRows;
  ButtonRect m_prevButton, m_nextButton, m_populateButton;
};

}  // namespace tachyon::viewer
