#include "viewer/app.h"

#include "analysis/query_executor.h"
#include "analysis/thread_states.h"
#include "core/log.h"
#include "core/platform.h"
#include "symbols/dia/dia_provider.h"
#include "symbols/dwarf/dwarf_provider.h"
#include "viewer/builtin_panels.h"
#include "viewer/capture/capture_controller.h"
#include "viewer/capture/session_explorer_panel.h"
#include "viewer/paths.h"
#include "viewer/source/source_panel.h"
#include "viewer/status/symbol_paths.h"
#include "viewer/status/symbols_panel.h"
#include "viewer/theme.h"
#include "viewer/timeline/timeline_panel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <ostream>
#include <thread>

namespace tachyon::viewer {

namespace {

using Clock = std::chrono::steady_clock;

void drawSpinner(float radius, float thickness, ImU32 color) {
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const ImVec2 centre(pos.x + radius, pos.y + radius + 1.0f);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float start = static_cast<float>(ImGui::GetTime()) * 6.0f;
  draw->PathClear();
  constexpr int kSegments = 24;
  for (int i = 0; i < kSegments; ++i) {
    const float a = start + (static_cast<float>(i) / kSegments) * 4.5f;
    draw->PathLineTo(ImVec2(centre.x + std::cos(a) * radius, centre.y + std::sin(a) * radius));
  }
  draw->PathStroke(color, 0, thickness);
  ImGui::Dummy(ImVec2(radius * 2.0f, radius * 2.0f + 2.0f));
}

}  // namespace

App::App(std::unique_ptr<IRenderer> renderer, AppOptions options)
    : m_renderer(std::move(renderer)),
      m_options(std::move(options)),
      m_queryExecutor(std::make_unique<analysis::QueryExecutor>(analysis::QueryExecutor::Options{2, 16})),
      m_toasts(m_options.toastClock) {}

App::~App() {
  m_jobs.cancelAll();
  m_capture.reset();   // joins the agent worker (it wakes the renderer) before the renderer is shut down
  m_document.reset();  // joins the symbol workers (they wake the renderer) before the renderer is shut down
  if (m_selectionSub != SelectionBus::kNoSubscription) m_selection.unsubscribe(m_selectionSub);
  if (m_imgui) {
    // Persist layout/settings while the context is still alive.
    if (m_options.persist) saveSettings();
  }
  m_panels.entries().clear();  // panels may hold ImGui state; destroy them before the context
  if (m_rendererUp) m_renderer->shutdown();
  if (m_imgui) ImGui::DestroyContext(m_imgui);
}

bool App::init(std::string& error) {
  if (m_options.configPath.empty()) {
    const std::filesystem::path dir = defaultConfigDir();
    if (!dir.empty()) m_settingsPath = dir / "settings.json";
  } else {
    m_settingsPath = m_options.configPath;
  }
  if (!m_settingsPath.empty()) m_settings = Settings::load(m_settingsPath);

  IMGUI_CHECKVERSION();
  m_imgui = ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;
  if (m_options.persist && !m_settingsPath.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(m_settingsPath.parent_path(), ec);
    m_iniPath = pathToUtf8(m_settingsPath.parent_path() / "imgui.ini");
    io.IniFilename = m_iniPath.c_str();  // layout persists next to settings.json
  }

  if (!m_renderer->init(error)) {
    return false;
  }
  m_rendererUp = true;

  m_panels.add(makeTraceInfoPanel());
  m_panels.add(makeThreadsTimelinePanel());
  m_panels.add(makeThreadInteractionPanel());
  m_panels.add(makeCallGraphPanel());
  m_panels.add(makeFunctionListPanel());
  m_panels.add(makeButterflyPanel());
  m_panels.add(makeFunctionStatsPanel(), false);  // opened by Ctrl+F or from the View menu
  {
    SymbolsPanelHost host;
    host.addSymbolPath = [this] { m_commands.run("symbols.addPath"); };
    host.retry = [this] { rerunSymbolResolution(); };
    host.toast = [this](ToastLevel level, std::string text) { toast(level, std::move(text)); };
    m_panels.add(makeSymbolsPanel(std::move(host)), false);  // opened from the status bar or the View menu
  }
  {
    // WP-45: the Source panel; its host persists the path mappings that "Locate file..." derives.
    SourcePanel::Host host;
    host.addRemap = [this](const symbols::SourceRemapRule& rule) {
      m_settings.addSourcePathMapping({rule.from, rule.to});
      if (m_options.persist) saveSettings();
      requestRedraw();
    };
    host.addRoot = [this](const std::filesystem::path& root) {
      m_settings.addSourceRoot(pathToUtf8(root));
      if (m_options.persist) saveSettings();
      requestRedraw();
    };
    host.toast = [this](ToastLevel level, std::string text) { toast(level, std::move(text)); };
    m_panels.add(makeSourcePanel(std::move(host)), false);  // opened by "Show source" requests
  }
  {
    // WP-41: the agent connection / capture dialogs, the Capture panel and the Session Explorer.
    IRenderer* renderer = m_renderer.get();
    CaptureHost host;
    host.settings = &m_settings;
    host.jobs = &m_jobs;
    host.toast = [this](ToastLevel level, std::string text) { toast(level, std::move(text)); };
    host.openFile = [this](const std::filesystem::path& path) { openFile(path); };
    host.wake = [renderer] { renderer->wake(); };
    host.requestRedraw = [this] { requestRedraw(); };
    host.saveSettings = [this] {
      if (m_options.persist) saveSettings();
    };
    host.showPanel = [this](const char* name) {
      if (PanelEntry* p = m_panels.find(name)) p->open = true;
      requestRedraw();
    };
    host.capturesChanged = [this] {
      if (PanelEntry* p = m_panels.find(kSessionExplorerPanelName)) {
        if (auto* explorer = dynamic_cast<SessionExplorerPanel*>(p->panel.get())) explorer->refresh();
      }
    };
    m_capture = std::make_unique<CaptureController>(std::move(host), m_options.processLauncher);
    m_panels.add(makeCapturePanel(*m_capture), false);  // opened when a capture starts or from the menu
    SessionExplorerHost explorer;
    explorer.settings = &m_settings;
    explorer.saveSettings = [this] {
      if (m_options.persist) saveSettings();
    };
    explorer.openFile = [this](const std::filesystem::path& path) { openFile(path); };
    explorer.toast = [this](ToastLevel level, std::string text) { toast(level, std::move(text)); };
    explorer.wake = [renderer] { renderer->wake(); };
    explorer.launcher = m_capture->sharedLauncher();
    m_panels.add(makeSessionExplorerPanel(std::move(explorer)), false);  // opened from the View menu
  }
  registerBuiltinPanelShortcuts(m_shortcuts);
  m_shortcuts.add("Source", "Ctrl+F", "Search in the file (Source panel focused)");
  m_shortcuts.add("Source", "Enter / Shift+Enter, F3 / Shift+F3", "Next / previous match");
  m_shortcuts.add("Source", "Esc", "Close the search");
  m_shortcuts.add("Source", "Up / Down, PageUp / PageDown", "Move the selected row");
  m_shortcuts.add("Source", "Click on >", "Expand the instructions of a line (Mixed mode)");
  m_selection.setBlendScopes(m_settings.blendScopes);  // before the first subscriber: nobody to notify
  m_selectionSub = m_selection.subscribe([this](const SelectionState& state, SelectionField field) {
    // WP-54: the blend choice is a preference, whichever panel changed it.
    if (field == SelectionField::ViewOptions && m_settings.blendScopes != state.viewOptions.blendScopes) {
      m_settings.blendScopes = state.viewOptions.blendScopes;
      if (m_options.persist) saveSettings();
    }
    // WP-46: a panel asked for another panel to be shown (the panel itself takes the focus).
    if (field == SelectionField::PanelRequest) {
      if (PanelEntry* p = m_panels.find(state.panelRequest.name)) p->open = true;
    }
    const PanelContext context = panelContext();
    for (PanelEntry& e : m_panels.entries()) e.panel->onSelectionChanged(context, state, field);
  });
  registerCommands();
  applyPendingStyle();
  if (m_options.autoConnectAgent) m_capture->connectAtStartup();
  return true;
}

PanelContext App::panelContext() const {
  PanelContext c;
  c.document = m_document.get();
  c.selection = const_cast<SelectionBus*>(&m_selection);
  c.settings = &m_settings;
  c.jobs = const_cast<JobQueue*>(&m_jobs);
  c.queryExecutor = m_queryExecutor.get();
  c.documentGeneration = m_documentGeneration;
  c.symbols = m_document ? m_document->symbols().get() : nullptr;
  c.symbolGeneration = m_symbolGeneration;
  c.memoryCeilingBytes = memoryCeiling();
  c.redrawFn = [](void* app) { static_cast<App*>(app)->requestRedraw(); };
  c.redrawUser = const_cast<App*>(this);
  return c;
}

void App::registerCommands() {
  m_commands.add({"file.open", "File: Open Trace...", "Ctrl+O", [this] {
                    std::filesystem::path start;
                    if (m_document) start = m_document->path().parent_path();
                    if (start.empty() && !m_settings.recentFiles.empty()) {
                      start = pathFromUtf8(m_settings.recentFiles.front()).parent_path();
                    }
                    m_fileBrowser.open(start);
                  }});
  m_commands.add({"file.close", "File: Close Trace", "", [this] { closeDocument(); }});
  m_commands.add({"app.quit", "File: Quit", "Ctrl+Q", [this] { requestQuit(); }});
  m_commands.add({"view.theme.toggle", "View: Toggle Dark/Light Theme", "", [this] {
                    setTheme(m_settings.theme == ThemeKind::Dark ? ThemeKind::Light : ThemeKind::Dark);
                  }});
  m_commands.add(
      {"view.scale.up", "View: Increase UI Scale", "", [this] { setUserScale(m_settings.uiScale * 1.1f); }});
  m_commands.add({"view.scale.down", "View: Decrease UI Scale", "",
                  [this] { setUserScale(m_settings.uiScale / 1.1f); }});
  m_commands.add({"view.scale.reset", "View: Reset UI Scale", "", [this] { setUserScale(1.0f); }});
  m_commands.add({"stats.search", "Function Statistics: Search Functions", "Ctrl+F",
                  [this] { m_selection.requestPanel(kFunctionStatsPanelName); }});
  m_commands.add({"help.palette", "Help: Command Palette", "Ctrl+P", [this] { m_paletteRequested = true; }});
  m_commands.add({"help.shortcuts", "Help: Keyboard Shortcuts", "?", [this] { m_shortcutSheet.open(); }});
  m_commands.add({"capture.run", "Capture: Run Application...", "",
                  [this] { m_capture->openDialog(capture::CaptureMode::Launch); }});
  m_commands.add({"capture.attach", "Capture: Attach to Process...", "",
                  [this] { m_capture->openDialog(capture::CaptureMode::Attach); }});
  m_commands.add({"capture.system", "Capture: System Capture...", "",
                  [this] { m_capture->openDialog(capture::CaptureMode::System); }});
  m_commands.add({"capture.stop", "Capture: Stop", "", [this] { m_capture->stopCapture(); }});
  m_commands.add({"capture.cancel", "Capture: Cancel", "", [this] { m_capture->cancelCapture(); }});
  m_commands.add(
      {"capture.resume", "Capture: Resume Parked Launch", "", [this] { m_capture->resumeCapture(); }});
  m_commands.add({"capture.agent.start", "Capture: Start Agent", "",
                  [this] { m_capture->startAgent(m_capture->agentNeedsElevation()); }});
  m_commands.add(
      {"capture.agent.reconnect", "Capture: Reconnect to Agent", "", [this] { m_capture->reconnect(); }});
  m_commands.add({"capture.window", "Capture: Show Capture Window", "", [this] {
                    if (PanelEntry* p = m_panels.find(kCapturePanelName)) p->open = true;
                  }});
  m_commands.add({"capture.explorer", "Capture: Show Session Explorer", "", [this] {
                    if (PanelEntry* p = m_panels.find(kSessionExplorerPanelName)) p->open = true;
                  }});
  m_commands.add({"symbols.addPath", "Symbols: Add Symbol Path...", "", [this] {
                    std::filesystem::path start;
                    if (!m_settings.symbolSearchPaths.empty())
                      start = pathFromUtf8(m_settings.symbolSearchPaths.back()).parent_path();
                    if (start.empty() && m_document) start = m_document->path().parent_path();
                    m_pathPicker.open(start);
                  }});
  m_commands.add({"symbols.retry", "Symbols: Retry Resolution", "", [this] { rerunSymbolResolution(); }});
}

void App::setTheme(ThemeKind theme) {
  m_settings.theme = theme;
  m_styleDirty = true;
}

void App::setUserScale(float scale) {
  m_settings.uiScale = std::clamp(scale, Settings::kMinUiScale, Settings::kMaxUiScale);
  m_styleDirty = true;
}

void App::applyPendingStyle() {
  if (!m_styleDirty) return;
  m_styleDirty = false;
  applyTheme(m_settings.theme, effectiveUiScale(m_renderer->dpiScale(), m_settings.uiScale));
}

void App::saveSettings() {
  if (m_settingsPath.empty()) return;
  const Result<void> r = m_settings.save(m_settingsPath);
  if (!r.ok()) TACHYON_LOG_WARN("settings: save failed: {}", r.error().toString());
}

void App::notifyDocumentChanged() {
  for (PanelEntry& e : m_panels.entries()) e.panel->onDocumentChanged(m_document.get());
  m_renderer->setWindowTitle(m_document ? "Tachyon - " + pathToUtf8(m_document->path().filename())
                                        : std::string("Tachyon"));
}

bool App::pumpSymbolEvents() {
  if (!m_symbolMailbox || !m_symbolMailbox->dirty.exchange(false)) return false;
  size_t delivered = 0;
  {
    const std::lock_guard<std::mutex> lock(m_symbolMailbox->mutex);
    delivered = m_symbolMailbox->events.size();
    m_symbolMailbox->events.clear();
  }
  if (delivered == 0) return false;
  ++m_symbolGeneration;
  requestRedraw();
  return true;
}

void App::startSymbolResolution(Document& document) {
  std::filesystem::path cacheDir = pathFromUtf8(m_settings.cacheDir);
  if (cacheDir.empty()) cacheDir = defaultSymbolCacheDir();
  auto service = std::make_shared<symbols::SymbolService>(m_settings.symbolServers, cacheDir);
#if TACHYON_PLATFORM_WINDOWS && TACHYON_HAVE_DIA
  symbols::registerDiaProvider(*service);  // PDBs through DIA; .symbols.json sidecars keep working
#endif
#if TACHYON_PLATFORM_LINUX && TACHYON_HAVE_LIBDW
  symbols::registerDwarfProvider(
      *service);  // ELF debug files through libdw; .symbols.json sidecars keep working
#endif
  // Developer convenience (and the test fixtures): a "<trace>.symbols.json" next to the trace is registered
  // as the symbol file of the modules it describes, ahead of the locator.
  std::filesystem::path sidecar = document.path();
  sidecar.replace_extension(".symbols.json");
  std::error_code ec;
  if (std::filesystem::is_regular_file(sidecar, ec)) {
    const Result<void> added = service->addSymbolFileOverride(sidecar);
    if (!added.ok()) TACHYON_LOG_WARN("symbols: {}: {}", pathToUtf8(sidecar), added.error().toString());
  }
  // WP-48: explicit symbol search paths from the Symbols panel / settings.
  const SymbolPathReport paths =
      applySymbolSearchPaths(*service, m_settings.symbolSearchPaths, m_settings.symbolServers, cacheDir);
  for (const std::string& problem : paths.problems) TACHYON_LOG_WARN("symbols: search path: {}", problem);
  if (m_options.configureSymbols) m_options.configureSymbols(*service);
  m_symbolMailbox = std::make_shared<SymbolMailbox>();
  IRenderer* renderer = m_renderer.get();
  m_symbolMailbox->wake = [renderer] { renderer->wake(); };
  document.setSymbols(service);
  auto mailbox = m_symbolMailbox;
  // The const overload never writes the model (Module::state is written only by the non-const overload's
  // waitAll()); module states are read with SymbolService::moduleState().
  service->resolveAllAsync(document.model(), [mailbox](ModuleIdx module, symbols::SymbolState state) {
    const std::lock_guard<std::mutex> lock(mailbox->mutex);
    mailbox->events.emplace_back(module, state);
    mailbox->dirty.store(true);
    if (mailbox->wake) mailbox->wake();
  });
}

// ---- WP-64: memory ceiling ----
uint64_t App::memoryCeiling() const {
  if (m_options.memoryCeilingBytes != 0) return m_options.memoryCeilingBytes;
  return memoryCeilingBytes(m_settings.memoryCeilingPercent, physicalMemoryBytes());
}

void App::setMemoryCeilingPercent(uint32_t percent) {
  m_settings.memoryCeilingPercent =
      percent == 0 ? 0u : std::clamp<uint32_t>(percent, Settings::kMinMemoryCeilingPercent, 100u);
  if (m_options.persist) saveSettings();
  checkMemory();
}

void App::checkMemory() {
  m_lastMemoryCheck = Clock::now();
  if (!m_document) return;
  if (m_memoryGeneration != m_documentGeneration) {  // a new document: new numbers, nothing degraded yet
    m_memoryGeneration = m_documentGeneration;
    m_memoryModelBytes = m_document->model().residentBytes();
    m_memory.reset();
  }
  timeline::TimelineWidget* widget = nullptr;
  if (PanelEntry* entry = m_panels.find(ThreadsTimelinePanel::kName)) {
    if (auto* panel = dynamic_cast<ThreadsTimelinePanel*>(entry->panel.get())) widget = &panel->widget();
  }
  const Document& document = *m_document;
  MemorySources sources;
  sources.modelBytes = [this] { return m_memoryModelBytes; };
  sources.lodBytes = [widget] { return widget ? static_cast<uint64_t>(widget->lodBytes()) : uint64_t{0}; };
  sources.cacheBytes = [&document] { return static_cast<uint64_t>(document.analysis().cacheBytes()); };
  sources.trimLod = [widget](uint64_t bytes) {
    return widget ? static_cast<uint64_t>(widget->trimLod(static_cast<size_t>(bytes))) : uint64_t{0};
  };
  sources.limitCaches = [&document](uint64_t budget) {
    document.analysis().setCacheBudget(static_cast<size_t>(budget));
  };
  if (m_memory.update(sources, memoryCeiling())) {
    const MemoryStatus& status = m_memory.status();
    TACHYON_LOG_WARN("memory: {}", status.message);
    toast(ToastLevel::Warning, status.message);
    m_banner.addNotice(CaptureWarningKind::MemoryDegraded, status.message);
  }
}

void App::toast(ToastLevel level, std::string text) {
  m_toasts.push(level, std::move(text));
  requestRedraw();
}

void App::rerunSymbolResolution() {
  if (!m_document) return;
  // Stop the old run first: snapshots and in-flight queries may keep the old service alive, and its workers
  // would otherwise keep loading providers alongside the new run. cancel() keeps lookups usable.
  if (const std::shared_ptr<symbols::SymbolService>& old = m_document->symbols()) old->cancel();
  startSymbolResolution(*m_document);  // fresh service; the old one is released once nobody holds it
  ++m_symbolGeneration;
  requestRedraw();
}

bool App::addSymbolPath(const std::filesystem::path& path, std::string& error) {
  const Result<void> valid = validateSymbolPath(path);
  if (!valid.ok()) {
    error = valid.error().toString();
    toast(ToastLevel::Error, "Cannot add symbol path: " + error);
    return false;
  }
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  m_settings.addSymbolSearchPath(pathToUtf8(ec ? path : absolute));
  if (m_options.persist) saveSettings();
  toast(ToastLevel::Info,
        "Added symbol path " + pathToUtf8(path) + (m_document ? "; resolving symbols again" : ""));
  rerunSymbolResolution();
  return true;
}

void App::onDocumentLoaded(std::shared_ptr<Document> document) {
  // The complete document of a stream replaces its preview: the user's selection stays.
  const bool continuation =
      m_document && document->streamId() != 0 && m_document->streamId() == document->streamId();
  m_symbolMailbox.reset();
  m_document = std::move(document);
  ++m_documentGeneration;
  ++m_symbolGeneration;
  startSymbolResolution(*m_document);
  m_lastError.clear();
  if (!continuation) m_selection.clear();
  m_settings.addRecentFile(pathToUtf8(std::filesystem::absolute(m_document->path())));
  if (m_options.persist) saveSettings();
  notifyDocumentChanged();
}

void App::closeDocument() {
  if (!m_document) return;
  // Closing the preview of a trace that is still loading abandons the load.
  if (m_openJob != 0 && m_document->streamId() != 0 && m_document->streamId() == m_openStream)
    m_jobs.cancel(m_openJob);
  m_document.reset();  // joins the symbol workers before the model can go away
  m_symbolMailbox.reset();
  ++m_documentGeneration;
  ++m_symbolGeneration;
  m_selection.clear();
  notifyDocumentChanged();
}

// WP-64: the previews of a streaming load, handed from the loading job to the main thread.
struct StreamMailbox {
  std::mutex mutex;
  std::shared_ptr<const model::TraceModel> preview;  // the newest preview the main thread has not taken yet
  uint64_t stream = 0;
  std::filesystem::path path;
};

void App::openFile(const std::filesystem::path& path) {
  if (m_openJob != 0) m_jobs.cancel(m_openJob);  // a new open supersedes the load in flight
  using Loaded = std::optional<Result<model::TraceModel>>;
  auto loaded = std::make_shared<Loaded>();
  m_lastError.clear();
  m_redrawFrames = std::max(m_redrawFrames, 3);
  // Big files open in stages (WP-64): a preview of the first part of the capture is shown while the rest loads.
  std::error_code sizeError;
  const uint64_t fileBytes = std::filesystem::file_size(path, sizeError);
  const bool streaming = m_options.streamingLoad && !sizeError && fileBytes >= m_options.streamMinFileBytes &&
                         !m_options.streamStages.empty();
  auto mailbox = std::make_shared<StreamMailbox>();
  mailbox->stream = ++m_streamCounter;
  mailbox->path = path;
  m_stream = mailbox;
  m_openStream = mailbox->stream;
  const std::vector<double> stages = m_options.streamStages;
  const std::function<void()> gate = m_options.streamGate;
  m_openJob = m_jobs.submit(
      "Loading " + pathToUtf8(path.filename()),
      [path, loaded, mailbox, streaming, stages, gate](JobContext& ctx) {
        model::LoadOptions options;
        options.cancel = ctx.cancelFlag();
        auto progress = [&ctx](float f) { ctx.setProgress(f); };
        // The timeline and the analysis queries need the thread state intervals (WP-31); build them while the
        // model is still mutable, before it is shared as const.
        auto buildStates = [&ctx](model::TraceModel& m) {
          if (ctx.cancelled()) return;
          const Result<void> states = analysis::buildThreadStates(m);
          if (!states.ok()) TACHYON_LOG_WARN("thread states: {}", states.error().toString());
        };
        if (streaming) {
          loaded->emplace(model::TraceModel::loadProgressive(
              path, options, stages,
              [&](model::TraceModel&& preview) {
                buildStates(preview);
                if (ctx.cancelled()) return;
                auto shared = std::make_shared<const model::TraceModel>(std::move(preview));
                {
                  const std::lock_guard<std::mutex> lock(mailbox->mutex);
                  mailbox->preview = std::move(shared);
                }
                if (gate) gate();
              },
              progress));
        } else {
          loaded->emplace(model::TraceModel::load(path, options, progress));
        }
        if (loaded->has_value() && (*loaded)->ok()) buildStates((*loaded)->value());
      },
      [this, path, loaded, mailbox](JobQueue::JobId id, JobOutcome outcome) {
        if (id != m_openJob) return;  // superseded by a newer open
        m_openJob = 0;
        if (m_stream == mailbox) m_stream.reset();
        if (outcome == JobOutcome::Cancelled || !loaded->has_value()) return;
        Result<model::TraceModel>& r = **loaded;
        if (!r.ok()) {
          m_lastError = "Cannot open " + pathToUtf8(path) + ": " + r.error().toString();
          TACHYON_LOG_ERROR("{}", m_lastError);
          if (m_document && m_document->streamId() == mailbox->stream) closeDocument();  // the preview of it
          toast(ToastLevel::Error, m_lastError);
          return;
        }
        auto model = std::make_shared<const model::TraceModel>(std::move(r).value());
        auto document = std::make_shared<Document>(path, std::move(model));
        document->setStreamId(mailbox->stream);
        onDocumentLoaded(std::move(document));
      });
}

// Takes the newest preview of the loading job, if any, and shows it (the complete document replaces it later).
void App::pumpStream() {
  if (!m_stream || m_openJob == 0) return;
  std::shared_ptr<const model::TraceModel> preview;
  {
    const std::lock_guard<std::mutex> lock(m_stream->mutex);
    preview = std::move(m_stream->preview);
  }
  if (!preview) return;
  auto document = std::make_shared<Document>(m_stream->path, preview);
  document->setStreamId(m_stream->stream);
  onDocumentLoaded(std::move(document));
  const model::LoadStatus& status = preview->loadStatus;
  const double hz = static_cast<double>(std::max<uint64_t>(preview->header.clock_frequency_hz, 1));
  char text[160];
  std::snprintf(text, sizeof text, "Showing the first %.1f s of %.1f s while the rest of the trace loads (%.0f%%)",
                static_cast<double>(preview->header.end_ts - preview->header.start_ts) / hz,
                static_cast<double>(status.fullEndTs - preview->header.start_ts) / hz, status.fraction() * 100.0);
  toast(ToastLevel::Info, text);
}

bool App::frame(int waitMs) {
  const PlatformEvents events = m_renderer->pollEvents(waitMs);
  if (events.quit) m_quit = true;
  if (events.hadInput) m_redrawFrames = std::max(m_redrawFrames, 3);
  for (const std::string& dropped : events.droppedFiles) openFile(pathFromUtf8(dropped));
  m_jobs.pump();
  pumpStream();
  pumpSymbolEvents();
  m_capture->pump();
  if (m_document && (m_options.memoryCheckMs <= 0 || m_memoryGeneration != m_documentGeneration ||
                     Clock::now() - m_lastMemoryCheck >= std::chrono::milliseconds(m_options.memoryCheckMs)))
    checkMemory();

  applyPendingStyle();
  m_renderer->beginFrame();
  drawUi();
  m_renderer->endFrame();
  ++m_frameCount;

  const ImGuiIO& io = ImGui::GetIO();
  if (m_redrawFrames > 0) --m_redrawFrames;
  if (io.WantTextInput || m_paletteOpen || m_fileBrowser.isOpen() || m_pathPicker.isOpen() ||
      m_shortcutSheet.isOpen() || !m_toasts.empty())
    m_redrawFrames = std::max(m_redrawFrames, 1);
  return !m_quit;
}

int App::run() {
  const auto framePeriod = std::chrono::microseconds(1000000 / std::max(1, m_options.targetFps));
  while (!m_quit) {
    const auto start = Clock::now();
    const bool busy = m_jobs.activeCount() > 0 || m_redrawFrames > 0 || m_capture->hasPending();
    if (busy || !m_options.pacing) {
      frame(0);
    } else {
      // Idle: block on events instead of rendering continuously.
      const PlatformEvents events = m_renderer->pollEvents(m_options.idleTimeoutMs);
      if (events.quit) m_quit = true;
      // The agent's events wake the loop; a running capture redraws its timer every idle timeout.
      const bool captureWork = m_capture->hasPending() || m_capture->active();
      if (pumpSymbolEvents() || captureWork) frame(0);
      if (events.hadInput || !events.droppedFiles.empty()) {
        m_redrawFrames = 3;
        for (const std::string& dropped : events.droppedFiles) openFile(pathFromUtf8(dropped));
        frame(0);
      }
    }
    if (m_options.pacing) {
      const auto end = start + framePeriod;
      if (Clock::now() < end) std::this_thread::sleep_until(end);
    }
  }
  return 0;
}

int App::runSelfTest(const std::filesystem::path& path, int frames, std::ostream& out) {
  openFile(path);
  while (m_openJob != 0) {
    frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!m_document) {
    out << "selftest: FAILED " << (m_lastError.empty() ? "trace did not load" : m_lastError) << '\n';
    return 1;
  }
  for (int i = 0; i < frames; ++i) frame(0);
  const model::TraceModel& m = m_document->model();
  out << "selftest: OK file=" << pathToUtf8(path.filename()) << " threads=" << m.threads.size()
      << " samples=" << m_document->totalSamples() << " modules=" << m.modules.size() << " frames=" << frames
      << '\n';
  return 0;
}

void App::drawUi() {
  const ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P)) m_paletteRequested = true;
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) m_commands.run("file.open");
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) requestQuit();
  // Ctrl+F opens the Function Statistics search, but never steals the keys from a text field.
  // A focused Source panel keeps the key for its own search (WP-45).
  bool sourceOwnsFind = false;
  for (const PanelEntry& e : m_panels.entries()) {
    if (const auto* source = dynamic_cast<const SourcePanel*>(e.panel.get()))
      sourceOwnsFind = e.open && source->wantsFindShortcut();
  }
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F) && !io.WantTextInput && !m_paletteOpen &&
      m_document && !sourceOwnsFind) {
    m_commands.run("stats.search");
  }

  drawMenuBar();
  m_banner.update(m_document.get(), m_documentGeneration);
  m_banner.draw();
  drawStatusBar();
  ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

  const PanelContext context = panelContext();
  for (PanelEntry& e : m_panels.entries()) {
    if (e.open) e.panel->draw(context, &e.open);
  }

  // Tell panels that were opened or closed since the last frame (window X, View menu, commands).
  for (PanelEntry& e : m_panels.entries()) {
    if (e.open != e.notifiedOpen) {
      e.notifiedOpen = e.open;
      e.panel->onVisibilityChanged(e.open);
    }
  }

  m_capture->draw();
  if (auto chosen = m_fileBrowser.draw()) openFile(*chosen);
  if (auto chosen = m_pathPicker.draw()) {
    std::string error;
    addSymbolPath(*chosen, error);
  }
  drawCommandPalette();
  m_shortcutSheet.draw(m_commands, m_shortcuts,
                       io.WantTextInput || m_paletteOpen || m_fileBrowser.isOpen() || m_pathPicker.isOpen());
  m_toasts.draw();
}

void App::drawMenuBar() {
  if (!ImGui::BeginMainMenuBar()) return;
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Open...", "Ctrl+O")) m_commands.run("file.open");
    if (ImGui::BeginMenu("Open Recent", !m_settings.recentFiles.empty())) {
      const std::vector<std::string> recent = m_settings.recentFiles;  // openFile may reorder the list later
      for (const std::string& r : recent) {
        if (ImGui::MenuItem(r.c_str())) openFile(pathFromUtf8(r));
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Close", nullptr, false, m_document != nullptr)) closeDocument();
    ImGui::Separator();
    if (ImGui::MenuItem("Quit", "Ctrl+Q")) requestQuit();
    ImGui::EndMenu();
  }
  m_capture->drawMenu();
  if (ImGui::BeginMenu("View")) {
    for (PanelEntry& e : m_panels.entries()) ImGui::MenuItem(e.panel->name(), nullptr, &e.open);
    ImGui::Separator();
    {
      // WP-54: analysis options that every panel follows (through the SelectionBus).
      const bool hasScopes = m_document && !m_document->model().scopeEvents.empty();
      const bool hasFibers = m_document && !m_document->model().fiberSwitches.empty();
      bool blend = m_selection.state().viewOptions.blendScopes;
      if (ImGui::MenuItem("Blend instrumentation scopes into sampled stacks", nullptr, &blend, hasScopes)) {
        m_selection.setBlendScopes(blend);  // the subscription below keeps the setting (and saves it)
      }
      bool group = m_selection.state().viewOptions.groupByFiber;
      if (ImGui::MenuItem("Group by fiber", nullptr, &group, hasFibers)) m_selection.setGroupByFiber(group);
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Theme")) {
      if (ImGui::MenuItem("Dark", nullptr, m_settings.theme == ThemeKind::Dark)) setTheme(ThemeKind::Dark);
      if (ImGui::MenuItem("Light", nullptr, m_settings.theme == ThemeKind::Light)) setTheme(ThemeKind::Light);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Memory limit")) {
      // WP-64: above the limit the viewer frees timeline detail levels and stops caching analysis results.
      const uint64_t physical = physicalMemoryBytes();
      struct Choice {
        uint32_t percent;
        const char* name;
      };
      for (const Choice c : {Choice{50, "50%"}, Choice{60, "60%"}, Choice{75, "75% (default)"},
                             Choice{90, "90%"}, Choice{0, "No limit"}}) {
        std::string label = c.name;
        if (c.percent != 0 && physical != 0) label += "  (" + formatBytes(memoryCeilingBytes(c.percent, physical)) + ")";
        if (ImGui::MenuItem(label.c_str(), nullptr, m_settings.memoryCeilingPercent == c.percent))
          setMemoryCeilingPercent(c.percent);
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("UI Scale")) {
      for (const float s : {0.75f, 1.0f, 1.25f, 1.5f, 2.0f}) {
        const std::string label = std::to_string(static_cast<int>(std::lround(s * 100.0f))) + "%";
        if (ImGui::MenuItem(label.c_str(), nullptr, std::fabs(m_settings.uiScale - s) < 0.01f))
          setUserScale(s);
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Help")) {
    if (ImGui::MenuItem("Command Palette...", "Ctrl+P")) m_paletteRequested = true;
    if (ImGui::MenuItem("Keyboard Shortcuts...", "?")) m_shortcutSheet.open();
    ImGui::EndMenu();
  }
  ImGui::EndMainMenuBar();
}

void App::drawStatusBar() {
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
  if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down,
                                  ImGui::GetFrameHeight(), flags)) {
    if (ImGui::BeginMenuBar()) {
      m_statusBar.update(m_document.get(), m_documentGeneration);
      m_statusBar.draw(m_document.get(), m_selection.state(), m_lastError);
      if (m_statusBar.symbolsClicked()) m_selection.requestPanel(SymbolsPanel::kName);
      m_capture->drawStatusSegment();
      const std::vector<JobQueue::Info> jobs = m_jobs.active();
      if (!jobs.empty()) {
        const JobQueue::Info& job = jobs.front();
        const float barWidth = ImGui::GetFontSize() * 10.0f;
        const float width = ImGui::CalcTextSize(job.label.c_str()).x + barWidth + ImGui::GetFontSize() * 6.0f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - width));
        drawSpinner(ImGui::GetFontSize() * 0.4f, 2.0f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
        ImGui::SameLine();
        ImGui::TextUnformatted(job.label.c_str());
        ImGui::SameLine();
        ImGui::ProgressBar(job.progress, ImVec2(barWidth, 0.0f));
        ImGui::SameLine();
        ImGui::PushID(static_cast<int>(job.id));
        if (ImGui::SmallButton("Cancel")) m_jobs.cancel(job.id);
        ImGui::PopID();
      }
      ImGui::EndMenuBar();
    }
  }
  ImGui::End();
}

void App::drawCommandPalette() {
  if (m_paletteRequested) {
    m_paletteRequested = false;
    // Panel toggles are registered lazily so panels added after init() show up.
    for (PanelEntry& e : m_panels.entries()) {
      const std::string name = e.panel->name();
      m_commands.add({"view.panel." + name, "View: Toggle " + name + " Panel", "", [this, name] {
                        if (PanelEntry* p = m_panels.find(name)) p->open = !p->open;
                      }});
    }
    m_paletteOpen = true;
    m_paletteQuery = {};
    m_paletteSelected = 0;
    ImGui::OpenPopup("##CommandPalette");
  }
  if (!m_paletteOpen) return;

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->Pos.y + vp->Size.y * 0.2f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, 0.0f));
  if (!ImGui::BeginPopup("##CommandPalette", ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings |
                                                 ImGuiWindowFlags_AlwaysAutoResize)) {
    m_paletteOpen = false;
    return;
  }
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(-FLT_MIN);
  const bool edited =
      ImGui::InputTextWithHint("##query", "Type a command...", m_paletteQuery.data(), m_paletteQuery.size());
  const std::vector<const Command*> matches = m_commands.match(m_paletteQuery.data());
  if (edited) m_paletteSelected = 0;
  const int count = static_cast<int>(matches.size());
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) m_paletteSelected = std::min(m_paletteSelected + 1, count - 1);
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) m_paletteSelected = std::max(m_paletteSelected - 1, 0);
  m_paletteSelected = std::clamp(m_paletteSelected, 0, std::max(count - 1, 0));

  const Command* toRun = nullptr;
  for (int i = 0; i < count; ++i) {
    const Command& c = *matches[static_cast<size_t>(i)];
    if (ImGui::Selectable(c.title.c_str(), i == m_paletteSelected)) toRun = &c;
    if (!c.shortcut.empty()) {
      ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(c.shortcut.c_str()).x -
                      ImGui::GetStyle().WindowPadding.x);
      ImGui::TextDisabled("%s", c.shortcut.c_str());
    }
  }
  if (count == 0) ImGui::TextDisabled("No matching commands");
  if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && count > 0) {
    toRun = matches[static_cast<size_t>(m_paletteSelected)];
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
    m_paletteOpen = false;
  }
  if (toRun) {
    const std::string id = toRun->id;
    ImGui::CloseCurrentPopup();
    m_paletteOpen = false;
    ImGui::EndPopup();
    m_commands.run(id);  // after the popup closes: the action may open another popup
    return;
  }
  ImGui::EndPopup();
}

}  // namespace tachyon::viewer
