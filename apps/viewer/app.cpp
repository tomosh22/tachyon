#include "viewer/app.h"

#include "analysis/query_executor.h"
#include "analysis/thread_states.h"
#include "core/log.h"
#include "core/platform.h"
#include "symbols/dia/dia_provider.h"
#include "symbols/dwarf/dwarf_provider.h"
#include "viewer/builtin_panels.h"
#include "viewer/paths.h"
#include "viewer/theme.h"

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
      m_queryExecutor(std::make_unique<analysis::QueryExecutor>(analysis::QueryExecutor::Options{2, 16})) {}

App::~App() {
  m_jobs.cancelAll();
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
  m_selectionSub = m_selection.subscribe([this](const SelectionState& state, SelectionField field) {
    // WP-46: a panel asked for another panel to be shown (the panel itself takes the focus).
    if (field == SelectionField::PanelRequest) {
      if (PanelEntry* p = m_panels.find(state.panelRequest.name)) p->open = true;
    }
    const PanelContext context = panelContext();
    for (PanelEntry& e : m_panels.entries()) e.panel->onSelectionChanged(context, state, field);
  });
  registerCommands();
  applyPendingStyle();
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
  m_commands.add({"help.palette", "Help: Command Palette", "Ctrl+P", [this] { m_paletteRequested = true; }});
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

void App::onDocumentLoaded(std::shared_ptr<Document> document) {
  m_symbolMailbox.reset();
  m_document = std::move(document);
  ++m_documentGeneration;
  ++m_symbolGeneration;
  startSymbolResolution(*m_document);
  m_lastError.clear();
  m_selection.clear();
  m_settings.addRecentFile(pathToUtf8(std::filesystem::absolute(m_document->path())));
  if (m_options.persist) saveSettings();
  notifyDocumentChanged();
}

void App::closeDocument() {
  if (!m_document) return;
  m_document.reset();  // joins the symbol workers before the model can go away
  m_symbolMailbox.reset();
  ++m_documentGeneration;
  ++m_symbolGeneration;
  m_selection.clear();
  notifyDocumentChanged();
}

void App::openFile(const std::filesystem::path& path) {
  if (m_openJob != 0) m_jobs.cancel(m_openJob);  // a new open supersedes the load in flight
  using Loaded = std::optional<Result<model::TraceModel>>;
  auto loaded = std::make_shared<Loaded>();
  m_lastError.clear();
  m_redrawFrames = std::max(m_redrawFrames, 3);
  m_openJob = m_jobs.submit(
      "Loading " + pathToUtf8(path.filename()),
      [path, loaded](JobContext& ctx) {
        model::LoadOptions options;
        options.cancel = ctx.cancelFlag();
        loaded->emplace(model::TraceModel::load(path, options, [&ctx](float f) { ctx.setProgress(f); }));
        // The timeline and the analysis queries need the thread state intervals (WP-31); build them while the
        // model is still mutable, before it is shared as const.
        if (loaded->has_value() && (*loaded)->ok() && !ctx.cancelled()) {
          const Result<void> states = analysis::buildThreadStates((*loaded)->value());
          if (!states.ok()) TACHYON_LOG_WARN("thread states: {}", states.error().toString());
        }
      },
      [this, path, loaded](JobQueue::JobId id, JobOutcome outcome) {
        if (id != m_openJob) return;  // superseded by a newer open
        m_openJob = 0;
        if (outcome == JobOutcome::Cancelled || !loaded->has_value()) return;
        Result<model::TraceModel>& r = **loaded;
        if (!r.ok()) {
          m_lastError = "Cannot open " + pathToUtf8(path) + ": " + r.error().toString();
          TACHYON_LOG_ERROR("{}", m_lastError);
          return;
        }
        auto model = std::make_shared<const model::TraceModel>(std::move(r).value());
        onDocumentLoaded(std::make_shared<Document>(path, std::move(model)));
      });
}

bool App::frame(int waitMs) {
  const PlatformEvents events = m_renderer->pollEvents(waitMs);
  if (events.quit) m_quit = true;
  if (events.hadInput) m_redrawFrames = std::max(m_redrawFrames, 3);
  for (const std::string& dropped : events.droppedFiles) openFile(pathFromUtf8(dropped));
  m_jobs.pump();
  pumpSymbolEvents();

  applyPendingStyle();
  m_renderer->beginFrame();
  drawUi();
  m_renderer->endFrame();
  ++m_frameCount;

  const ImGuiIO& io = ImGui::GetIO();
  if (m_redrawFrames > 0) --m_redrawFrames;
  if (io.WantTextInput || m_paletteOpen || m_fileBrowser.isOpen())
    m_redrawFrames = std::max(m_redrawFrames, 1);
  return !m_quit;
}

int App::run() {
  const auto framePeriod = std::chrono::microseconds(1000000 / std::max(1, m_options.targetFps));
  while (!m_quit) {
    const auto start = Clock::now();
    const bool busy = m_jobs.activeCount() > 0 || m_redrawFrames > 0;
    if (busy || !m_options.pacing) {
      frame(0);
    } else {
      // Idle: block on events instead of rendering continuously.
      const PlatformEvents events = m_renderer->pollEvents(m_options.idleTimeoutMs);
      if (events.quit) m_quit = true;
      if (pumpSymbolEvents()) frame(0);
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
  (void)io;

  drawMenuBar();
  drawStatusBar();
  ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

  const PanelContext context = panelContext();
  for (PanelEntry& e : m_panels.entries()) {
    if (e.open) e.panel->draw(context, &e.open);
  }

  if (auto chosen = m_fileBrowser.draw()) openFile(*chosen);
  drawCommandPalette();
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
  if (ImGui::BeginMenu("View")) {
    for (PanelEntry& e : m_panels.entries()) ImGui::MenuItem(e.panel->name(), nullptr, &e.open);
    ImGui::Separator();
    if (ImGui::BeginMenu("Theme")) {
      if (ImGui::MenuItem("Dark", nullptr, m_settings.theme == ThemeKind::Dark)) setTheme(ThemeKind::Dark);
      if (ImGui::MenuItem("Light", nullptr, m_settings.theme == ThemeKind::Light)) setTheme(ThemeKind::Light);
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
      if (!m_lastError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_lastError.c_str());
      } else if (m_document) {
        const model::TraceModel& m = m_document->model();
        ImGui::Text("%s  |  %zu threads  |  %llu samples", pathToUtf8(m_document->path().filename()).c_str(),
                    m.threads.size(), static_cast<unsigned long long>(m_document->totalSamples()));
      } else {
        ImGui::TextDisabled("Ready");
      }
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
