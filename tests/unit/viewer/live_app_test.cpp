// WP-65b: the viewer's Live mode inside a headless App against the real agent code (an in-process Agent and
// Server with the mock backend's live source): the Live dialog, a Live document that appears and refreshes,
// Follow and what turns it off, Pause, Stop (the final trace opens), live_end, a gap and a corrupt batch
// (resubscribe), symbols resolved once per module, and the destruction of the App during a live capture
// (the ASan build runs this too).
#include "live_test_util.h"

#include "symbols/symbol_service.h"
#include "viewer/callgraph/callgraph_panel.h"
#include "viewer/functions/function_list_panel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include "control/tcp.h"

#include <atomic>
#include <fstream>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::live_test;

namespace {

bool toastContains(App& app, const std::string& text) {
  for (const Toast& t : app.toasts().toasts())
    if (t.text.find(text) != std::string::npos) return true;
  return false;
}

}  // namespace

namespace {

const std::string kLiveToken = "viewer-live-token-0123456789-abcdefghij";

// The "remote machine": an Agent behind a TCP listener on loopback, replaying a fixture as a live capture;
// its traces go to <dir>/remote-out.
struct RemoteLiveAgent {
  RemoteLiveAgent(const fs::path& dir, const fs::path& fixtureTrace, double speed)
      : outDir(dir / "remote-out"), localAddress(newAddress("liveremote")) {
    fs::create_directories(outDir);
    capture::MockBackendConfig cfg;
    cfg.fixture = fixtureTrace;
    cfg.live.speed = speed;
    agent::AgentConfig ac;
    ac.backendName = "mock";
    ac.agentAddress = localAddress;
    ac.remoteOutDir = outDir;
    ac.verifyInstPeerPid = false;
    core = std::make_unique<agent::Agent>(std::make_unique<capture::MockBackend>(cfg), ac);
    auto local = control::listenOn(localAddress);
    EXPECT_TRUE(local.ok());
    server = std::make_unique<control::Server>(std::move(*local), *core);
    auto tcp = control::listenTcp(control::TcpEndpoint{"127.0.0.1", 0});
    EXPECT_TRUE(tcp.ok());
    hostPort = (*tcp)->address();
    control::AuthPolicy policy;
    policy.token = kLiveToken;
    server->addRemoteListener(std::move(*tcp), policy);
    core->setEventSink([this](const control::Event& e) { server->broadcast(e); });
    server->start();
    tokenFile = dir / "agent.token";
    std::ofstream(tokenFile, std::ios::binary) << kLiveToken << "\n";
#if !defined(_WIN32)
    ::chmod(tokenFile.string().c_str(), 0600);
#endif
  }
  ~RemoteLiveAgent() {
    server->stop();
    core->shutdown();
    std::error_code ec;
    fs::remove(localAddress, ec);
  }
  fs::path outDir;
  std::string localAddress;
  std::string hostPort;
  fs::path tokenFile;
  std::unique_ptr<agent::Agent> core;
  std::unique_ptr<control::Server> server;
};

}  // namespace

TEST(LiveApp, ARemoteAgentStreamsAndStopFetchesTheFinalTrace) {
  TempDir dir("live_remote");
  RemoteLiveAgent remote(dir.path(), fixture("frames.tachyon"), 2.0);
  Harness h(liveOptions());
  h.app.settings().capture.capturesDir = pathToUtf8(dir.path() / "captures");
  h.app.settings().capture.autoConnect = false;
  h.app.settings().capture.autoOpen = false;
  h.app.settings().capture.agentAddress = newAddress("liveremote-unused");
  CaptureController& cap = h.app.capture();
  ASSERT_TRUE(cap.connectRemote(remote.hostPort, pathToUtf8(remote.tokenFile)));
  ASSERT_TRUE(h.until([&] {
    return cap.session().snapshot().connection == ConnectionState::Connected &&
           cap.session().snapshot().phase == AgentPhase::Idle;
  }));
  ASSERT_TRUE(h.app.commands().run("capture.live"));
  h.frames(2);
  cap.form().mode = capture::CaptureMode::System;
  ASSERT_TRUE(cap.submitDialog());
  ASSERT_TRUE(
      h.until([&] { return h.app.document() && h.app.document()->isLive() && h.app.liveSwaps() >= 2; }));
  EXPECT_FALSE(h.app.liveUiState().latencyS.has_value()) << "no latency across machines: the clocks differ";
  EXPECT_NE(h.app.liveStatusLine().find("LIVE"), std::string::npos);

  h.app.liveStop();  // opens the trace even though autoOpen is off, once it has been fetched
  ASSERT_TRUE(h.until([&] { return !h.app.loading() && h.app.document() && !h.app.document()->isLive(); }));
  EXPECT_EQ(h.app.document()->path().parent_path(), dir.path() / "captures") << "fetched from the agent";
  EXPECT_FALSE(cap.transfers().empty());
  EXPECT_EQ(cap.transfers().front().state, RemoteTransfer::State::Done);
  EXPECT_EQ(h.app.liveUiState().phase, LiveUiState::Phase::None);
}

TEST(LiveApp, TheLiveDialogStartsALiveCaptureAndALiveDocumentAppears) {
  LiveRig rig("live_dialog", "frames.tachyon", 1.0, liveOptions());
  ASSERT_TRUE(rig.waitConnected());
  // The menu command opens the Live dialog: the same fields as Run / Attach / System plus the mode choice.
  ASSERT_TRUE(rig.app().commands().run("capture.live"));
  rig.h.frames(2);
  EXPECT_TRUE(rig.cap().dialogOpen());
  EXPECT_TRUE(rig.cap().form().live);
  rig.cap().form().mode = capture::CaptureMode::System;
  rig.cap().form().circularMb = 64;
  ASSERT_TRUE(rig.cap().submitDialog());
  EXPECT_TRUE(rig.cap().lastRequest().live);
  EXPECT_TRUE(rig.cap().session().lastStartRequest().live);
  EXPECT_EQ(rig.cap().lastRequest().circular_mb, 64u);

  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument(); }));
  const std::shared_ptr<Document> first = rig.app().document();
  EXPECT_TRUE(first->isLive());
  EXPECT_NE(first->streamId(), 0u);
  EXPECT_TRUE(first->isPreview()) << "a snapshot is partial by nature";
  EXPECT_TRUE(first->symbols() != nullptr);
  EXPECT_NE(first->prebuiltLod(), nullptr) << "the timeline index is built off the UI thread";
  EXPECT_TRUE(rig.app().panels().find(kLivePanelName)->open) << "the Live panel opens with the capture";

  // It refreshes: newer generations replace it, the document stays the same stream.
  const uint64_t gen0 = first->live().generation;
  ASSERT_TRUE(rig.h.until([&] { return rig.shownGeneration() > gen0 + 1; }));
  EXPECT_NE(rig.app().document(), first);
  EXPECT_EQ(rig.app().document()->streamId(), first->streamId());
  EXPECT_GE(rig.app().liveSwaps(), 2u);
  EXPECT_GT(rig.app().document()->totalSamples(), 0u);
  EXPECT_TRUE(rig.h.until([&] { return rig.widget().lodFinished(); }));

  // The status bar says LIVE and the Live panel numbers add up.
  rig.h.frames(2);
  EXPECT_NE(rig.app().liveStatusLine().find("LIVE"), std::string::npos) << rig.app().liveStatusLine();
  const LiveUiState ui = rig.app().liveUiState();
  EXPECT_EQ(ui.phase, LiveUiState::Phase::Running);
  EXPECT_TRUE(ui.canStop);
  EXPECT_TRUE(ui.canSave) << "a circular live capture can save its last window";
  EXPECT_GT(ui.threads, 0u);
  EXPECT_GT(ui.samples, 0u);
  EXPECT_EQ(rig.app().document()->path().filename(), "Live capture");

  // One snapshot per refresh, none kept: the documents, their timeline indices and the symbol services of the
  // past snapshots are released (the reference holders are the current document, the one being built and,
  // briefly, the one just replaced).
  ASSERT_TRUE(rig.waitSwaps(8));
  rig.h.frames(3);
  EXPECT_LE(Document::aliveCount(), 3);
  EXPECT_LE(timeline::LodShared::alive().load(), 4);
  EXPECT_LE(rig.app().live().stats().symbols.epochsAlive, 3u);
}

TEST(LiveApp, SaveLast30SecondsNeedsACircularCaptureAndFlushesIt) {
  LiveRig rig("live_save", "frames.tachyon", 1.0, liveOptions());
  ASSERT_TRUE(rig.startLive(/*circularMb=*/0));
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument(); }));
  LiveUiState ui = rig.app().liveUiState();
  EXPECT_FALSE(ui.canSave);
  EXPECT_FALSE(ui.saveHint.empty()) << "disabled with a hint";
  rig.app().commands().run("capture.stop");
  ASSERT_TRUE(rig.h.until([&] { return rig.cap().session().snapshot().phase == AgentPhase::Idle; }));
  // (The finished trace opens, as autoOpen says; wait for it so it does not end the next live view.)
  ASSERT_TRUE(rig.h.until(
      [&] { return !rig.app().loading() && rig.app().document() && !rig.app().document()->isLive(); }));

  // Again, circular: the flush is accepted by the agent (the mock writes <name>.tachyon) and announced.
  ASSERT_TRUE(rig.startLive(/*circularMb=*/64));
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument() && rig.app().liveSwaps() >= 2; }));
  ui = rig.app().liveUiState();
  EXPECT_TRUE(ui.canSave);
  rig.app().liveSaveLast();
  ASSERT_TRUE(rig.h.until([&] { return !rig.cap().flushedCaptures().empty(); }));
  EXPECT_FALSE(rig.cap().flushedCaptures().front().name.empty());
}

TEST(LiveApp, FollowScrollsTheWindowAndPanningOrZoomingTurnsItOff) {
  // A 2 s window over a replay that goes on past it: the window slides with "now".
  LiveRig rig("live_follow", "frames.tachyon", 1.0, liveOptions(100, 1));
  ASSERT_TRUE(rig.startLive());
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument() && rig.widget().hasDocument(); }));
  rig.h.frames(2);
  EXPECT_TRUE(rig.widget().following());
  EXPECT_TRUE(rig.widget().isLive());

  // Following: every swap shows the whole (new) window; the window start moves on.
  const uint64_t start0 = rig.app().document()->model().header.start_ts;
  ASSERT_TRUE(rig.h.until([&] {
    return rig.app().document()->model().header.start_ts > start0 + 1'000'000 && rig.widget().hasDocument();
  }));
  rig.h.frames(2);
  EXPECT_TRUE(rig.widget().following());
  EXPECT_EQ(rig.widget().view(), timeline::fitView(rig.widget().limits()));
  EXPECT_NE(rig.app().document()->live().generation, 0u);

  // A zoom (the W key, with the timeline focused) is the user's: Follow turns off.
  ImGui::SetWindowFocus(ThreadsTimelinePanel::kName);
  rig.h.frames(2);
  rig.h.key(ImGuiKey_W);
  EXPECT_FALSE(rig.widget().following());
  const timeline::TimeView zoomed = rig.widget().view();
  const uint64_t absStart = rig.widget().limits().traceStart + static_cast<uint64_t>(zoomed.start);
  EXPECT_LT(zoomed.span, rig.widget().limits().total());
  // Later snapshots keep the place the user chose (in absolute time) while it is inside the window.
  const uint64_t gen = rig.shownGeneration();
  ASSERT_TRUE(rig.h.until([&] { return rig.shownGeneration() > gen; }));
  rig.h.frames(2);
  EXPECT_FALSE(rig.widget().following());
  const timeline::TimeView later = rig.widget().view();
  EXPECT_NEAR(zoomed.span, later.span, 1.0);
  const uint64_t windowStart = rig.widget().limits().traceStart;
  const uint64_t absLater = windowStart + static_cast<uint64_t>(later.start);
  // The view keeps its place in absolute time; once the window's left edge passes it, it stays at the edge.
  // It never jumps forward to follow the data.
  EXPECT_EQ(absLater, std::max(absStart, windowStart));
  EXPECT_NE(later, timeline::fitView(rig.widget().limits()));

  // The Follow button turns it back on: the whole newest window again, and it scrolls from there.
  rig.app().setLiveFollow(true);
  EXPECT_TRUE(rig.widget().following());
  rig.h.frames(2);
  EXPECT_EQ(rig.widget().view(), timeline::fitView(rig.widget().limits()));

  // Panning (the A key) turns it off too, an explicit view as well.
  ImGui::SetWindowFocus(ThreadsTimelinePanel::kName);
  rig.h.frames(2);
  rig.h.key(ImGuiKey_W);  // zoom first: panning inside a fully fit view is a no-op
  rig.app().setLiveFollow(true);
  rig.h.frames(1);
  rig.widget().setView({0.0, rig.widget().limits().total() / 2});
  EXPECT_FALSE(rig.widget().following());
}

TEST(LiveApp, RowFlagsAndTheSelectionSurviveTheSwap) {
  LiveRig rig("live_rows", "frames.tachyon", 1.0, liveOptions());
  ASSERT_TRUE(rig.startLive());
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument() && rig.widget().rowOrder().size() >= 2; }));
  rig.h.frames(2);
  const ThreadIdx t = rig.widget().rowOrder().front();
  rig.widget().setPinned(t, true);
  rig.widget().setExpanded(t, true);
  rig.app().selection().setSelectedThread(t);
  const uint64_t gen = rig.shownGeneration();
  ASSERT_TRUE(rig.h.until([&] { return rig.shownGeneration() >= gen + 2; }));
  rig.h.frames(2);
  EXPECT_TRUE(rig.widget().flagsOf(t).pinned);
  EXPECT_TRUE(rig.widget().flagsOf(t).expanded);
  EXPECT_EQ(rig.app().selection().state().selectedThread, t);
}

TEST(LiveApp, TheCallGraphAndTheFunctionListKeepTheirSelectionAcrossSnapshots) {
  LiveRig rig("live_panels", "frames.tachyon", 1.0, liveOptions());
  for (PanelEntry& e : rig.app().panels().entries()) {
    const std::string name = e.panel->name();
    e.open = name == "Threads Timeline" || name == "Call Graph" || name == "Function List";
  }
  auto* graph = dynamic_cast<CallGraphPanel*>(rig.app().panels().find("Call Graph")->panel.get());
  auto* list = dynamic_cast<FunctionListPanel*>(rig.app().panels().find("Function List")->panel.get());
  ASSERT_NE(graph, nullptr);
  ASSERT_NE(list, nullptr);
  graph->disableThrottle();
  list->disableThrottle();
  ASSERT_TRUE(rig.startLive());
  std::optional<symbols::FunctionKey> chosen;
  ASSERT_TRUE(rig.h.until([&] {
    if (!rig.liveDocument() || !graph->tree() || !list->table()) return false;
    for (const functions::Entry& e : list->table()->entries)
      if (!e.module.empty() && !e.scope && !chosen) chosen = e.key;
    return chosen.has_value();
  }));
  // Select a function of the table from outside (the bus), as a click in another panel would.
  const symbols::FunctionKey key = *chosen;
  rig.app().selection().setSelectedFunction(key);
  ASSERT_TRUE(rig.h.until([&] { return graph->selectedKey() == key && list->selectedKey() == key; }));
  graph->collapseAllRows();
  const uint64_t trees = graph->deliveredTrees(), tables = list->deliveredTables();
  const callgraph::ExpandSet expanded = graph->expanded();

  // More snapshots arrive; both panels re-query on each and deliver, but keep the selection (and the tree its
  // expansion): a file document would have started them from scratch.
  ASSERT_TRUE(rig.h.until(
      [&] { return graph->deliveredTrees() >= trees + 3 && list->deliveredTables() >= tables + 3; }));
  EXPECT_EQ(graph->selectedKey(), key);
  EXPECT_EQ(list->selectedKey(), key);
  EXPECT_EQ(graph->expanded(), expanded);
  EXPECT_TRUE(graph->tree() != nullptr);
}

TEST(LiveApp, PauseFreezesTheDocumentAndResumeCatchesUp) {
  LiveRig rig("live_pause", "frames.tachyon", 1.0, liveOptions());
  ASSERT_TRUE(rig.startLive());
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument() && rig.app().liveSwaps() >= 2; }));
  rig.app().setLivePaused(true);
  rig.h.frames(2);
  const std::shared_ptr<Document> frozen = rig.app().document();
  const uint64_t shown = frozen->live().generation;
  const uint64_t swaps = rig.app().liveSwaps();
  EXPECT_NE(rig.app().liveStatusLine().find("paused"), std::string::npos) << rig.app().liveStatusLine();
  // The model keeps receiving while the view stands still.
  ASSERT_TRUE(rig.h.until([&] { return rig.app().liveUiState().receivedGeneration > shown + 1; }));
  rig.run(600);
  EXPECT_EQ(rig.app().document(), frozen) << "the document on screen did not change";
  EXPECT_EQ(rig.shownGeneration(), shown);
  EXPECT_EQ(rig.app().liveSwaps(), swaps);
  EXPECT_TRUE(rig.app().liveUiState().paused);

  rig.app().setLivePaused(false);
  ASSERT_TRUE(rig.h.until([&] { return rig.shownGeneration() > shown; }));
  EXPECT_FALSE(rig.app().liveUiState().paused);
}

TEST(LiveApp, StopStopsTheCaptureAndOpensTheFinalTrace) {
  LiveRig rig("live_stop", "frames.tachyon", 2.0, liveOptions());
  rig.app().settings().capture.autoOpen = false;  // Stop opens the trace whatever autoOpen says
  ASSERT_TRUE(rig.startLive());
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument() && rig.app().liveSwaps() >= 2; }));
  const std::string out = rig.cap().lastRequest().out_path;
  rig.app().liveStop();
  ASSERT_TRUE(rig.h.until(
      [&] { return !rig.app().loading() && rig.app().document() && !rig.app().document()->isLive(); }));
  EXPECT_EQ(rig.app().document()->path(), fs::path(out));
  EXPECT_EQ(rig.app().liveUiState().phase, LiveUiState::Phase::None) << "live mode ended";
  EXPECT_FALSE(rig.app().live().active());
  EXPECT_TRUE(rig.app().liveStatusLine().empty() || rig.h.until([&] {
    rig.h.frames(1);
    return rig.app().liveStatusLine().empty();
  }));
  EXPECT_GT(rig.app().document()->totalSamples(), 0u);
}

TEST(LiveApp, LiveEndTurnsTheDocumentIntoAStaticSnapshot) {
  LiveRig rig("live_end", "frames.tachyon", 2.0, liveOptions());
  rig.app().settings().capture.autoOpen = false;
  ASSERT_TRUE(rig.startLive());
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument() && rig.app().liveSwaps() >= 2; }));
  rig.cap().stopCapture();  // the capture ends from outside the Live window
  ASSERT_TRUE(rig.h.until([&] { return rig.app().liveUiState().phase == LiveUiState::Phase::Ended; }));
  // The last snapshot arrives and marks the end; then nothing changes any more.
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument() && rig.app().document()->live().ended; }));
  const std::shared_ptr<Document> last = rig.app().document();
  const uint64_t swaps = rig.app().liveSwaps();
  rig.run(500);
  EXPECT_EQ(rig.app().document(), last);
  EXPECT_EQ(rig.app().liveSwaps(), swaps);
  EXPECT_TRUE(last->isLive()) << "still the live snapshot, now static";
  EXPECT_FALSE(rig.app().live().active()) << "the worker and the live model are released";
  EXPECT_NE(rig.app().liveStatusLine().find("ENDED"), std::string::npos) << rig.app().liveStatusLine();
  EXPECT_EQ(rig.cap().session().liveStatus().state, LiveFeedStatus::State::Ended);
  EXPECT_FALSE(rig.app().liveUiState().canStop);
}

TEST(LiveApp, AGapResubscribesAndShowsAToast) {
  LiveRig rig("live_gap", "frames.tachyon", 1.0, liveOptions());
  ASSERT_TRUE(rig.startLive());
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument() && rig.app().liveSwaps() >= 2; }));
  EXPECT_EQ(rig.cap().session().liveStatus().gaps, 0u);
  // The agent tells this client that it fell behind (the real overflow needs a stalled socket).
  rig.agent.server->broadcast(control::LiveGapEvent{rig.cap().session().liveStatus().seq + 1});
  ASSERT_TRUE(rig.h.until([&] { return rig.cap().session().liveStatus().gaps == 1; }));
  ASSERT_TRUE(rig.h.until([&] { return rig.cap().session().liveStatus().resubscribes == 1; }));
  EXPECT_TRUE(rig.h.until([&] { return toastContains(rig.app(), "fell behind"); }));
  // The new subscription starts with a keyframe and the document goes on refreshing.
  const uint64_t gen = rig.shownGeneration();
  ASSERT_TRUE(rig.h.until([&] { return rig.shownGeneration() > gen; }));
  const LiveFeedStatus feed = rig.cap().session().liveStatus();
  EXPECT_EQ(feed.state, LiveFeedStatus::State::Streaming);
  EXPECT_EQ(feed.corruptBatches, 0u);
  EXPECT_EQ(rig.app().liveUiState().phase, LiveUiState::Phase::Running);
  EXPECT_EQ(rig.app().liveUiState().gaps, 1u);
}

TEST(LiveApp, ACorruptBatchResubscribesOnceThenShowsAnError) {
  LiveRig rig("live_corrupt", "frames.tachyon", 1.0, liveOptions());
  ASSERT_TRUE(rig.startLive());
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument() && rig.app().liveSwaps() >= 2; }));
  // A record whose length runs past the end of the batch.
  std::string garbage(8, '\0');
  garbage[0] = static_cast<char>(model::live_wire::RecordType::Samples);
  garbage[4] = '\xff';
  garbage[5] = '\xff';
  garbage[6] = '\xff';
  garbage[7] = '\x7f';
  const uint64_t seq = rig.cap().session().liveStatus().seq;
  rig.agent.server->broadcast(control::LiveEvent{seq + 1000, false, garbage});
  ASSERT_TRUE(rig.h.until([&] { return rig.cap().session().liveStatus().corruptBatches == 1; }));
  ASSERT_TRUE(rig.h.until([&] { return rig.cap().session().liveStatus().resubscribes == 1; }));
  const uint64_t gen = rig.shownGeneration();
  ASSERT_TRUE(rig.h.until([&] { return rig.shownGeneration() > gen; })) << "recovered through the keyframe";
  EXPECT_EQ(rig.app().liveUiState().phase, LiveUiState::Phase::Running);
  EXPECT_TRUE(rig.app().liveUiState().error.empty());

  // A second one: no more resubscribing, a visible error, the capture itself goes on.
  rig.agent.server->broadcast(control::LiveEvent{seq + 2000, false, garbage});
  ASSERT_TRUE(rig.h.until([&] { return rig.app().liveUiState().phase == LiveUiState::Phase::Failed; }));
  EXPECT_FALSE(rig.app().liveUiState().error.empty());
  EXPECT_TRUE(toastContains(rig.app(), "Live view stopped"));
  EXPECT_EQ(rig.cap().session().liveStatus().state, LiveFeedStatus::State::Failed);
  EXPECT_EQ(rig.cap().session().snapshot().phase, AgentPhase::Capturing);
  EXPECT_NE(rig.app().liveStatusLine().find("STOPPED"), std::string::npos) << rig.app().liveStatusLine();
}

TEST(LiveApp, ASecondLiveCaptureStartsFresh) {
  LiveRig rig("live_twice", "frames.tachyon", 4.0, liveOptions());
  rig.app().settings().capture.autoOpen = false;
  ASSERT_TRUE(rig.startLive());
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument() && rig.app().liveSwaps() >= 2; }));
  const uint64_t stream1 = rig.app().document()->streamId();
  rig.cap().stopCapture();
  ASSERT_TRUE(rig.h.until([&] { return rig.app().liveUiState().phase == LiveUiState::Phase::Ended; }));
  ASSERT_TRUE(rig.h.until([&] { return rig.cap().session().snapshot().phase == AgentPhase::Idle; }));
  ASSERT_TRUE(rig.startLive());
  ASSERT_TRUE(rig.h.until([&] {
    return rig.liveDocument() && rig.app().document()->streamId() != stream1 &&
           rig.app().liveUiState().phase == LiveUiState::Phase::Running;
  }));
  EXPECT_EQ(rig.app().liveSwaps() >= 1, true);
  EXPECT_FALSE(rig.app().document()->live().ended);
}

TEST(LiveApp, SymbolsAreLoadedOncePerModuleAcrossSnapshots) {
  // Every provider load is counted; the fixture's symbols resolve through its .symbols.json.
  auto loads = std::make_shared<std::atomic<int>>(0);
  AppOptions options = liveOptions();
  options.configureSymbols = [loads](symbols::SymbolService& service) {
    ASSERT_TRUE(service.addSymbolFileOverride(fixture("frames.symbols.json")).ok());
    symbols::SymbolService* svc = &service;
    service.setProviderFactory(
        [svc, loads](const model::Module&,
                     const fs::path& file) -> std::unique_ptr<symbols::ISymbolProvider> {
          ++*loads;
          if (file.extension() == ".json")
            return std::make_unique<symbols::JsonSymbolProvider>(svc->stringSink());
          return nullptr;
        });
  };
  LiveRig rig("live_symbols", "frames.tachyon", 1.0, std::move(options));
  ASSERT_TRUE(rig.startLive());
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument() && rig.app().liveSwaps() >= 8; }));
  ASSERT_TRUE(rig.h.until([&] { return rig.h.symbolsDone(); }));
  const LiveSessionStats stats = rig.app().live().stats();
  EXPECT_EQ(loads->load(), 1) << "one module in the fixture: its provider was loaded once, in "
                              << stats.symbols.epochs << " symbol epochs and " << rig.app().liveSwaps()
                              << " snapshots";
  EXPECT_EQ(stats.symbols.modulesLoaded, 1u);
  EXPECT_GE(stats.symbols.epochs, 2u) << "the tables grew while the replay went on";
  EXPECT_GE(stats.symbols.modulesAdopted, 1u) << "later epochs adopted the finished module";

  // The functions are named in the document on screen.
  const Document& doc = *rig.app().document();
  const symbols::SymbolService* svc = doc.symbols().get();
  ASSERT_NE(svc, nullptr);
  size_t resolved = 0;
  for (size_t f = 0; f < doc.model().frames.size(); ++f)
    if (svc->lookup(FrameIdx(static_cast<uint32_t>(f))).resolved) ++resolved;
  EXPECT_GT(resolved, 0u);
  EXPECT_EQ(rig.app().symbolGeneration() > 0, true);
}

TEST(LiveApp, DestroyingTheAppDuringALiveCaptureIsClean) {
  // The App is destroyed with the snapshot worker busy, symbol workers running and the agent streaming.
  for (int round = 0; round < 3; ++round) {
    LiveRig rig("live_destroy", "frames.tachyon", 2.0, liveOptions(50));
    ASSERT_TRUE(rig.startLive());
    ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument(); }));
    if (round > 0) ASSERT_TRUE(rig.waitSwaps(3));
    rig.h.frames(3);
  }
}

TEST(LiveApp, OpeningAnotherTraceLeavesLiveMode) {
  LiveRig rig("live_leave", "frames.tachyon", 1.0, liveOptions());
  ASSERT_TRUE(rig.startLive());
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument() && rig.app().liveSwaps() >= 2; }));
  ASSERT_TRUE(rig.h.open(fixture("tiny.tachyon")));
  EXPECT_FALSE(rig.app().document()->isLive());
  EXPECT_EQ(rig.app().liveUiState().phase, LiveUiState::Phase::None);
  EXPECT_FALSE(rig.app().live().active());
  // The capture itself is still running on the agent: no snapshot comes back.
  rig.run(500);
  EXPECT_FALSE(rig.app().document()->isLive());
  EXPECT_EQ(rig.cap().session().snapshot().phase, AgentPhase::Capturing);
}
