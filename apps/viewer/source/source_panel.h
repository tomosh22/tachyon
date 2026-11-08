// The "Source" panel (WP-45): source code, mixed source/assembly and pure disassembly of the function that is
// selected on the SelectionBus, with the time the sampled threads spent on every line and instruction.
//
// Data flow. The panel follows the bus's selected function and time filter. Whenever the effective request
// (function, filter, document / symbol generation, source path configuration) changes it submits ONE task to
// the app's QueryExecutor (common::LatestQuery, newest wins): buildSourceView() (source/source_model.h) runs
// the line and instruction queries, locates and reads the file (16 MiB cap, binary check) and disassembles
// the function. The main thread only draws the finished SourceView, so file IO, queries and disassembly never
// block a frame. A superseded or stale result (other selection, document or symbol generation) is dropped;
// the previous view stays on screen, dimmed, with a spinner while a request runs.
//
// Modes. Source: the file with a line gutter, a time bar (share of the function's total), a stacked
// running / ready / waiting mini-bar, a log-scaled heat background and percentage / ms columns. Mixed: Source
// plus a per-line expander that lists the instructions mapped to that line with their own times; samples of a
// caller frame sit on the instruction AFTER the call and carry an "after call" tag. Disassembly: the
// function's instructions (the fallback when the file cannot be found; "Locate file..." derives a path remap
// rule from the chosen file, persists it through the host and re-resolves).
//
// Keys. Ctrl+F inside the focused panel opens the search bar (Enter / Shift+Enter or F3 / Shift+F3 next /
// previous, Esc closes). When the panel is not focused Ctrl+F keeps opening Function Statistics:
// wantsFindShortcut() lets the application decide.
#pragma once

#include "symbols/function_key.h"
#include "symbols/source_locator.h"
#include "viewer/analysis_context.h"
#include "viewer/common/latest_query.h"
#include "viewer/common/live_stream.h"
#include "viewer/panel.h"
#include "viewer/source/source_model.h"
#include "viewer/status/path_picker.h"
#include "viewer/status/toast.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer {

class SourcePanel final : public IPanel {
 public:
  static constexpr const char* kName = "Source";
  static constexpr size_t kMaxSearchMatches = 100000;

  enum class Mode : uint8_t { Source, Mixed, Disassembly };

  // What the panel asks of the application (all main-thread only; empty functions are ignored).
  struct Host {
    // Persist a path remap rule / a search root for source files (Settings::sourcePathMappings /
    // sourceRoots).
    std::function<void(const symbols::SourceRemapRule&)> addRemap;
    std::function<void(const std::filesystem::path&)> addRoot;
    std::function<void(ToastLevel, std::string)> toast;
  };

  // One displayed row: a source line (instr < 0), an instruction below its line (Mixed) or an instruction of
  // the pure disassembly (line = 0).
  struct Row {
    uint32_t line = 0;
    int32_t instr = -1;
  };

  // The effective request.
  struct Spec {
    std::optional<symbols::FunctionKey> key;
    std::optional<ViewFilter> filter;
    uint64_t documentGeneration = 0;
    uint64_t symbolGeneration = 0;
    uint64_t sourceGeneration = 0;  // bumped when the path configuration changes or "Reload" is pressed
    bool operator==(const Spec&) const = default;
  };

  explicit SourcePanel(Host host = {});
  ~SourcePanel() override;

  // Main-thread only.
  const char* name() const override { return kName; }
  void draw(const PanelContext& context, bool* open) override;
  void onSelectionChanged(const PanelContext& context, const SelectionState& state,
                          SelectionField field) override;
  void onDocumentChanged(const Document* document) override;

  // Main-thread only. Delivers a finished view and starts a request when the effective one changed. draw()
  // calls it first; headless tests call it directly.
  void update(const PanelContext& context);

  // ---- actions (UI and tests); main-thread only ----
  void setMode(Mode mode);
  // Expands / collapses the instructions of source line `line` (Mixed mode).
  void toggleLine(uint32_t line);
  void expandAll();
  void collapseAll();
  // Scrolls to and selects the hottest source line (Source / Mixed) or instruction (Disassembly).
  void jumpToHottest();
  // Selects display row `row`.
  void selectRow(size_t row);
  void openSearch();
  void closeSearch();
  // Sets the search text, finds all matches and jumps to the first one at or after the selected row.
  void setSearchText(const std::string& text);
  // Jump to the next / previous match after / before the selected row (wrapping around).
  void searchNext();
  void searchPrevious();
  // Forgets the located-file cache and reads the file again.
  void reload();
  // Opens the file picker of "Locate file...".
  void beginLocate(const PanelContext& context);
  // Applies a file (or directory) chosen with "Locate file...": derives a remap rule (or a search root) and
  // hands it to the host. The settings change is noticed by the next update().
  void locateFile(const std::filesystem::path& chosen, bool chosenIsDirectory);
  // Tests: forget the symbol-resolution throttle.
  void disableThrottle() { m_throttle = false; }
  // Tests: show `view` as if a request had delivered it.
  void injectView(std::shared_ptr<const source::SourceView> view);

  // ---- state (tests); main-thread only ----
  const std::shared_ptr<const source::SourceView>& view() const { return m_view; }
  Mode mode() const { return m_mode; }
  // The mode actually shown (Mixed falls back to Source without instructions, anything to Disassembly
  // without a file).
  Mode effectiveMode() const;
  const std::vector<Row>& rows() const { return m_rows; }
  bool lineExpanded(uint32_t line) const { return line < m_expanded.size() && m_expanded[line] != 0; }
  // Selected display row (SIZE_MAX = none) and the source line / instruction index it stands for.
  size_t selectedRow() const { return m_selectedRow; }
  uint32_t selectedLine() const { return m_selectedRow < m_rows.size() ? m_rows[m_selectedRow].line : 0; }
  // Display rows the last frame drew (first, last inclusive; SIZE_MAX / SIZE_MAX before the first draw).
  size_t firstVisibleRow() const { return m_firstVisible; }
  size_t lastVisibleRow() const { return m_lastVisible; }
  bool searchOpen() const { return m_searchOpen; }
  const std::string& searchText() const { return m_searchText; }
  // Source line numbers (Source / Mixed) or instruction indices (Disassembly) that contain the search text.
  const std::vector<uint32_t>& searchMatches() const { return m_matches; }
  bool pending() const { return m_query.pending(); }
  uint64_t issuedQueries() const { return m_query.issued(); }
  uint64_t deliveredViews() const { return m_delivered; }
  const Spec& requestedSpec() const { return m_requested; }
  const Spec& shownSpec() const { return m_shown; }
  const std::string& message() const { return m_message; }
  uint64_t focusRequests() const { return m_focusRequests; }
  // True when the panel had the keyboard focus in the last drawn frame: Ctrl+F then belongs to it.
  bool wantsFindShortcut() const { return m_hadFocus; }
  symbols::SourceLocator& locator() { return *m_locator; }
  PathPicker& picker() { return m_picker; }
  // Screen rectangles of the rows drawn in the last frame. `fixedX` is the left edge of the fixed columns.
  struct DrawnRow {
    size_t row;
    float x0, y0, x1, y1;
    float fixedX;
    float expanderX1;  // right edge of the expander cell
  };
  const std::vector<DrawnRow>& drawnRows() const { return m_drawn; }
  float charWidth() const { return m_charW; }
  // Screen rectangle of the "Locate file..." button (zero when not drawn).
  struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  };
  const Rect& locateButton() const { return m_locateButton; }

 private:
  using Clock = std::chrono::steady_clock;

  struct Layout;
  struct RowContext;

  Spec currentSpec(const PanelContext& context) const;
  void issue(const PanelContext& context, const Spec& spec);
  void applyView(std::shared_ptr<const source::SourceView> view);
  void resetDocumentState();
  void syncLocatorConfig(const PanelContext& context);
  void rebuildRows();
  void rebuildMatches();
  void scrollToRow(size_t row);
  size_t rowOfLine(uint32_t line) const;
  bool rowMatchesSearch(size_t row) const;
  void jumpToMatch(size_t index);

  void drawToolbar(const PanelContext& context);
  void drawHeaderText(const PanelContext& context);
  void drawCode(const PanelContext& context);
  void drawRow(const RowContext& c, size_t rowIndex);
  void drawTimeCells(const RowContext& c, float x, const source::LineTime& t, uint64_t denominator) const;

  Host m_host;
  std::shared_ptr<symbols::SourceLocator> m_locator;
  std::vector<SourcePathMapping> m_appliedRemaps;
  std::vector<std::string> m_appliedRoots;
  bool m_haveConfig = false;
  uint64_t m_sourceGeneration = 0;

  common::LatestQuery<source::SourceView> m_query;
  Spec m_requested, m_shown;
  bool m_haveRequested = false;
  Clock::time_point m_lastIssue{};
  bool m_throttle = true;
  uint64_t m_delivered = 0;
  uint64_t m_documentGeneration = 0;
  bool m_haveDocument = false;
  common::LiveStreamTracker m_live;  // WP-65c
  double m_clockHz = 1e9;
  std::string m_message;

  std::shared_ptr<const source::SourceView> m_view;
  Mode m_mode = Mode::Source;
  std::vector<Row> m_rows;
  std::vector<uint32_t> m_lineRow;  // Source / Mixed: display row of every source line (index = line)
  std::vector<uint8_t> m_expanded;  // index = source line
  size_t m_selectedRow = SIZE_MAX;
  int64_t m_scrollRow = -1;  // pending scroll request
  size_t m_firstVisible = SIZE_MAX, m_lastVisible = SIZE_MAX;

  bool m_searchOpen = false;
  bool m_searchFocus = false;
  std::string m_searchText;
  std::vector<uint32_t> m_matches;

  bool m_focusPending = false;
  uint64_t m_focusRequests = 0;
  bool m_hadFocus = false;
  PathPicker m_picker;
  std::string m_pickerDebugPath;  // the debug path the open picker was started for

  std::vector<DrawnRow> m_drawn;
  float m_charW = 7.0f;
  Rect m_locateButton;
};

// Thread-safe: factory.
std::unique_ptr<IPanel> makeSourcePanel(SourcePanel::Host host = {});

}  // namespace tachyon::viewer
