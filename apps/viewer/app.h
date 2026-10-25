// App: owns the window backend, ImGui context, settings, selection bus, jobs, panels and the open Document.
#pragma once

#include "viewer/command_registry.h"
#include "viewer/document.h"
#include "viewer/file_browser.h"
#include "viewer/job_queue.h"
#include "viewer/panel.h"
#include "viewer/renderer.h"
#include "viewer/selection_bus.h"
#include "viewer/settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct ImGuiContext;

namespace tachyon::viewer {

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
};

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

 private:
  PanelContext panelContext() const;
  void registerCommands();
  void onDocumentLoaded(std::shared_ptr<Document> document);
  void closeDocument();
  // Main-thread only. Installs a SymbolService on `document` and starts resolving; the workers report through
  // m_symbolMailbox (thread-safe), which wakes the event loop.
  void startSymbolResolution(Document& document);
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
  PanelRegistry m_panels;
  CommandRegistry m_commands;
  FileBrowser m_fileBrowser;
  std::shared_ptr<Document> m_document;
  uint64_t m_documentGeneration = 0;
  uint64_t m_symbolGeneration = 0;
  std::shared_ptr<SymbolMailbox> m_symbolMailbox;

  JobQueue::JobId m_openJob = 0;
  std::string m_lastError;
  bool m_styleDirty = true;
  bool m_quit = false;
  uint64_t m_frameCount = 0;
  int m_redrawFrames = 3;  // frames still to draw after the last input (ImGui needs a few to settle)

  bool m_paletteRequested = false;
  bool m_paletteOpen = false;
  std::array<char, 256> m_paletteQuery{};
  int m_paletteSelected = 0;
};

}  // namespace tachyon::viewer
