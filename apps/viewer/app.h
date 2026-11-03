// App: owns the window backend, ImGui context, settings, selection bus, jobs, panels and the open Document.
#pragma once

#include "viewer/command_registry.h"
#include "viewer/document.h"
#include "viewer/file_browser.h"
#include "viewer/job_queue.h"
#include "viewer/memory_governor.h"
#include "viewer/panel.h"
#include "viewer/renderer.h"
#include "viewer/selection_bus.h"
#include "viewer/settings.h"
#include "viewer/status/path_picker.h"
#include "viewer/status/shortcut_sheet.h"
#include "viewer/status/status_bar.h"
#include "viewer/status/toast.h"
#include "viewer/status/warnings_banner.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct ImGuiContext;

namespace tachyon::analysis {
class QueryExecutor;
}  // namespace tachyon::analysis

namespace tachyon::viewer {

struct StreamMailbox;     // app.cpp (WP-64)
struct SymbolSetupRun;    // app.cpp (WP-67)
class CaptureController;  // capture/capture_controller.h (WP-41)
class IProcessLauncher;   // capture/process_launcher.h (WP-41)

// Module state changes reported by the symbol workers, handed to the main thread.
struct SymbolMailbox {
  std::mutex mutex;
  std::vector<std::pair<ModuleIdx, symbols::SymbolState>> events;
  std::atomic<bool> dirty{false};
  // Called (from the worker) after each push; set before the workers start and never changed afterwards.
  std::function<void()> wake;
};

struct AppOptions {
  // settings.json to use; empty means <user config dir>/settings.json (no persistence if that is unknown).
  std::filesystem::path configPath;
  // Persist settings and imgui.ini (layout) next to the settings file. Off for tests and --selftest.
  bool persist = true;
  // Cap at 60 fps and block in the event wait when idle. Off for headless runs.
  bool pacing = true;
  int idleTimeoutMs = 250;
  int targetFps = 60;
  // WP-48 (tests): clock of the toast queue, in seconds; empty = steady_clock.
  ToastQueue::ClockFn toastClock;
  // WP-48 (tests): called with every document's new SymbolService after the viewer configured it and before
  // resolution starts (e.g. to install a slow fake provider factory).
  std::function<void(symbols::SymbolService&)> configureSymbols;
  // WP-41: starts the capture agent and the file manager ("Reveal"); null = the real launcher (posix_spawn /
  // ShellExecuteEx, pkexec / UAC for an elevated agent). Tests inject a recording or de-elevating launcher.
  std::shared_ptr<IProcessLauncher> processLauncher;
  // WP-41: try to connect to a running agent at startup (silently; also needs Settings::capture.autoConnect).
  bool autoConnectAgent = true;
  // WP-64: open big traces in stages. A file of at least `streamMinFileBytes` is loaded as previews of the
  // fractions in `streamStages` (each shown as soon as it is ready), then in full; the complete document
  // replaces the last preview and the timeline keeps its view. Off for small files and when `streamingLoad`
  // is false.
  bool streamingLoad = true;
  uint64_t streamMinFileBytes = 64ull << 20;
  std::vector<double> streamStages = {0.05};
  // WP-64 (tests): called on the loading thread after each preview was published, before the next stage
  // starts; a test blocks here to look at the preview.
  std::function<void()> streamGate;
  // WP-64: memory ceiling. 0 = Settings::memoryCeilingPercent of the physical RAM; tests pass a byte count.
  uint64_t memoryCeilingBytes = 0;
  // How often the ceiling is checked (the first check of a document is immediate); 0 = every frame.
  int memoryCheckMs = 1000;
};

// Thread-safe: pure function. WP-67: the options of the headless entry points (--selftest and --bench): no
// persistence, no pacing, and no connection attempt to a capture agent (there is none to talk to, and a
// failing attempt would only cost time and log noise).
inline AppOptions makeHeadlessOptions(AppOptions options) {
  options.persist = false;
  options.pacing = false;
  options.autoConnectAgent = false;
  return options;
}

// Thread-safe: pure function. WP-67: the old model's ThreadIdx -> the new model's, by (pid, tid) (the first
// thread of a pair wins, like TraceModel::tidToThread). A thread the new model lacks maps to an invalid
// index. Returns null when every old index maps to itself (nothing to remap). Used when the complete model of
// a streaming load replaces its preview: threads synthesised from samples (no ThreadRec) are numbered in
// ascending tid order of what each model saw, so the same thread can move.
std::function<ThreadIdx(ThreadIdx)> threadMapBetween(const model::TraceModel& from,
                                                     const model::TraceModel& to);

class App {
 public:
  // Main-thread only.
  App(std::unique_ptr<IRenderer> renderer, AppOptions options);
  // Main-thread only. Cancels jobs, saves settings (if persisting) and destroys the ImGui context.
  ~App();
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // Main-thread only. Creates the ImGui context, loads settings, initialises the renderer, registers the
  // built-in panels and commands. Must succeed before anything else is called.
  bool init(std::string& error);

  // Main-thread only. Starts loading `path` on a worker thread; cancels a load still in flight.
  void openFile(const std::filesystem::path& path);
  // Main-thread only. Interactive loop (idle-throttled, capped at options.targetFps). Returns the exit code.
  int run();
  // Main-thread only. Runs one iteration of the loop: events (waiting up to `waitMs`), job completions, one
  // ImGui frame. Returns false once quit was requested.
  bool frame(int waitMs = 0);
  // Main-thread only. Headless check used by --selftest: opens `path`, waits for the load, runs `frames`
  // frames, prints a summary line to `out`. Returns 0 on success, non-zero if the trace did not load.
  int runSelfTest(const std::filesystem::path& path, int frames, std::ostream& out);
  // Main-thread only.
  void requestQuit() { m_quit = true; }

  // Main-thread only accessors.
  Settings& settings() { return m_settings; }
  SelectionBus& selection() { return m_selection; }
  JobQueue& jobs() { return m_jobs; }
  // WP-43: the analysis query executor shared by all panels (2 worker threads).
  analysis::QueryExecutor& queryExecutor() { return *m_queryExecutor; }
  PanelRegistry& panels() { return m_panels; }
  CommandRegistry& commands() { return m_commands; }
  const std::shared_ptr<Document>& document() const { return m_document; }
  uint64_t frameCount() const { return m_frameCount; }
  bool loading() const { return m_openJob != 0; }
  const std::string& lastError() const { return m_lastError; }
  const std::filesystem::path& settingsPath() const { return m_settingsPath; }
  // Main-thread only. Draw a few more frames even without input (PanelContext::requestRedraw).
  void requestRedraw() { m_redrawFrames = std::max(m_redrawFrames, 3); }
  uint64_t documentGeneration() const { return m_documentGeneration; }
  // Main-thread only. Counts module-resolution events delivered so far (also bumped per document); panels see
  // it as PanelContext::symbolGeneration.
  uint64_t symbolGeneration() const { return m_symbolGeneration; }
  // Main-thread only. Delivers pending symbol events from the workers: bumps the symbol generation and asks
  // for a redraw. Called by frame() and by the idle path of run(). Returns true if anything was delivered.
  bool pumpSymbolEvents();
  void setTheme(ThemeKind theme);
  void setUserScale(float scale);

  // ---- WP-41: capture ----
  // Main-thread only. The agent connection, capture dialogs and live capture state.
  CaptureController& capture() { return *m_capture; }

  // ---- WP-48: status, warnings, symbol paths, toasts, shortcut sheet ----
  // Main-thread only. Shows a message in the bottom-right corner (5 s, at most 5 stacked).
  void toast(ToastLevel level, std::string text);
  ToastQueue& toasts() { return m_toasts; }
  StatusBar& statusBar() { return m_statusBar; }
  WarningsBanner& warningsBanner() { return m_banner; }
  ShortcutRegistry& shortcuts() { return m_shortcuts; }
  ShortcutSheet& shortcutSheet() { return m_shortcutSheet; }
  PathPicker& symbolPathPicker() { return m_pathPicker; }
  // ---- WP-64: memory ceiling ----
  // Main-thread only. What the last check of the memory ceiling found and degraded (Normal when nothing).
  const MemoryStatus& memoryStatus() const { return m_memory.status(); }
  // Main-thread only. The ceiling in bytes now (0 = none).
  uint64_t memoryCeiling() const;
  // Main-thread only. Sets Settings::memoryCeilingPercent (0 = none), saves it and checks at once.
  void setMemoryCeilingPercent(uint32_t percent);
  // Main-thread only. Runs the check now (frame() runs it every options.memoryCheckMs).
  void checkMemory();
  // Main-thread only (tests). Replaces AppOptions::memoryCeilingBytes; checks at once.
  void setMemoryCeilingOverride(uint64_t bytes) {
    m_options.memoryCeilingBytes = bytes;
    checkMemory();
  }

  // Main-thread only. Adds `path` (a directory or a symbol file) to Settings::symbolSearchPaths (persisted
  // unless persist is off), then re-runs the symbol resolution of the open document. Returns false and sets
  // `error` (also toasted) if the path is unusable. Cost: resolution runs again for ALL modules.
  bool addSymbolPath(const std::filesystem::path& path, std::string& error);
  // Main-thread only. Starts the symbol resolution of the open document again with a fresh SymbolService
  // (current settings and search paths); no-op without a document. Same cost note as addSymbolPath().
  void rerunSymbolResolution();
  // Main-thread only (WP-67). What the symbol search paths of the current run reported once their worker
  // finished (directory scans stopped at the entry cap, unreadable directories, rejected files); empty
  // until then and for a run without search paths.
  const std::vector<std::string>& symbolPathProblems() const { return m_symbolPathProblems; }
  // Main-thread only (WP-67). True while the worker that scans the search paths is still running (the
  // resolution of the modules starts when it is done).
  bool symbolPathScanRunning() const { return m_symbolSetup != nullptr; }

 private:
  PanelContext panelContext() const;
  void registerCommands();
  void onDocumentLoaded(std::shared_ptr<Document> document);
  void closeDocument();
  void pumpStream();
  // Main-thread only. Installs a SymbolService on `document` and starts resolving; the workers report through
  // m_symbolMailbox (thread-safe), which wakes the event loop.
  void startSymbolResolution(Document& document);
  void beginSymbolResolve(Document& document);  // resolveAllAsync of the document's service
  void finishSymbolSetup();                     // the search-path worker is done
  void notifyDocumentChanged();
  void applyPendingStyle();
  void drawUi();
  void drawMenuBar();
  void drawStatusBar();
  void drawCommandPalette();
  void saveSettings();

  std::unique_ptr<IRenderer> m_renderer;
  AppOptions m_options;
  ImGuiContext* m_imgui = nullptr;
  bool m_rendererUp = false;

  Settings m_settings;
  std::filesystem::path m_settingsPath;
  std::string m_iniPath;  // ImGui keeps the pointer; must outlive the context
  SelectionBus m_selection;
  SelectionBus::SubscriptionId m_selectionSub = SelectionBus::kNoSubscription;
  JobQueue m_jobs;
  std::unique_ptr<analysis::QueryExecutor> m_queryExecutor;  // declared before m_document: outlives it
  PanelRegistry m_panels;
  CommandRegistry m_commands;
  FileBrowser m_fileBrowser;
  std::unique_ptr<CaptureController>
      m_capture;  // after m_jobs / m_settings (it uses them); reset first in ~App
  std::shared_ptr<Document> m_document;
  uint64_t m_documentGeneration = 0;
  uint64_t m_symbolGeneration = 0;
  std::shared_ptr<SymbolMailbox> m_symbolMailbox;
  std::unique_ptr<SymbolSetupRun> m_symbolSetup;  // WP-67: after m_document, m_symbolMailbox; destroyed first
  std::vector<std::string> m_symbolPathProblems;

  MemoryGovernor m_memory;
  uint64_t m_memoryModelBytes = 0;  // TraceModel::residentBytes() of the document of m_memoryGeneration
  uint64_t m_memoryGeneration = ~0ull;
  std::chrono::steady_clock::time_point m_lastMemoryCheck{};
  JobQueue::JobId m_openJob = 0;
  uint64_t m_streamCounter = 0;  // WP-64: numbers the opens; documents of one open share the id
  uint64_t m_openStream = 0;     // the stream of m_openJob
  std::shared_ptr<StreamMailbox> m_stream;
  std::string m_lastError;
  bool m_styleDirty = true;
  bool m_quit = false;
  uint64_t m_frameCount = 0;
  int m_redrawFrames = 3;  // frames still to draw after the last input (ImGui needs a few to settle)

  ToastQueue m_toasts;
  StatusBar m_statusBar;
  WarningsBanner m_banner;
  ShortcutRegistry m_shortcuts;
  ShortcutSheet m_shortcutSheet;
  PathPicker m_pathPicker;

  bool m_paletteRequested = false;
  bool m_paletteOpen = false;
  std::array<char, 256> m_paletteQuery{};
  int m_paletteSelected = 0;
};

}  // namespace tachyon::viewer
