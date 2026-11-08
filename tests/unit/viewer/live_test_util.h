// Shared helpers of the WP-65b tests (viewer Live mode): an in-process Agent with the mock backend's live
// source, a viewer wired to it, and helpers to drive a Live capture through the real UI paths.
#pragma once

#include "capture_test_util.h"
#include "symbols/json_symbol_provider.h"
#include "viewer/timeline/timeline_panel.h"

namespace tachyon::viewer::live_test {

using namespace capture_test;

// An in-process Agent (real agent code, real Server) whose mock backend replays `fixtureTrace` as a live
// capture, the trace clock running `speed` times as fast as the wall clock (the agent binary's mock backend
// is manual: it has no replay speed switch).
struct LiveAgent {
  LiveAgent(const std::string& addr, const fs::path& fixtureTrace, double speed, const fs::path& outDir,
            agent::AgentConfig base = {})
      : address(addr) {
    capture::MockBackendConfig cfg;
    cfg.fixture = fixtureTrace;
    cfg.live.speed = speed;
    auto backend = std::make_unique<capture::MockBackend>(cfg);
    mock = backend.get();
    base.backendName = "mock";
    base.agentAddress = address;
    base.verifyInstPeerPid = false;
    base.remoteOutDir = outDir;
    core = std::make_unique<agent::Agent>(std::move(backend), base);
    auto listener = control::listenOn(address);
    EXPECT_TRUE(listener.ok());
    server = std::make_unique<control::Server>(std::move(*listener), *core);
    core->setEventSink([this](const control::Event& e) { server->broadcast(e); });
    server->start();
  }
  ~LiveAgent() {
    server->stop();
    core->shutdown();
    std::error_code ec;
    fs::remove(address, ec);
  }
  std::string address;
  capture::MockBackend* mock = nullptr;
  std::unique_ptr<agent::Agent> core;
  std::unique_ptr<control::Server> server;
};

inline AppOptions liveOptions(uint32_t refreshMs = 100, uint32_t windowSeconds = 30) {
  AppOptions o = headlessOptions("live");
  o.autoConnectAgent = false;
  o.liveRefreshMs = refreshMs;
  o.liveWindowSeconds = windowSeconds;
  return o;
}

// A viewer connected to a LiveAgent. The agent is declared first: the viewer is destroyed (and its worker
// threads joined) before the agent goes away.
struct LiveRig {
  LiveRig(const std::string& tag, const std::string& fixtureName, double speed, AppOptions options)
      : dir(tag),
        addr(newAddress(tag)),
        agent(addr, fixtureName.empty() ? fixture("tiny.tachyon") : fixture(fixtureName.c_str()), speed,
              dir.path()),
        h(std::move(options)) {
    configureCapture(h.app.settings(), addr, dir.path() / "captures");
    h.app.settings().capture.autoOpen = true;
    // Only the panels the tests look at: the timeline keeps the keyboard focus.
    for (PanelEntry& e : h.app.panels().entries()) {
      e.open = e.panel->name() == std::string(ThreadsTimelinePanel::kName);
    }
    timeline =
        dynamic_cast<ThreadsTimelinePanel*>(h.app.panels().find(ThreadsTimelinePanel::kName)->panel.get());
    h.app.capture().reconnect();
  }

  CaptureController& cap() { return h.app.capture(); }
  App& app() { return h.app; }
  timeline::TimelineWidget& widget() { return timeline->widget(); }
  bool connected() { return cap().session().snapshot().connection == ConnectionState::Connected; }
  bool waitConnected() {
    return h.until([&] { return connected() && cap().session().snapshot().phase == AgentPhase::Idle; });
  }
  // Starts a live system capture through the Live dialog (the real UI path).
  bool startLive(int circularMb = 0) {
    EXPECT_TRUE(waitConnected());
    EXPECT_TRUE(app().commands().run("capture.live"));
    h.frames(2);
    EXPECT_TRUE(cap().dialogOpen());
    cap().form().mode = capture::CaptureMode::System;
    cap().form().circularMb = circularMb;
    return cap().submitDialog();
  }
  bool liveDocument() {
    const std::shared_ptr<Document>& d = app().document();
    return d && d->isLive();
  }
  uint64_t shownGeneration() { return app().document() ? app().document()->live().generation : 0; }
  bool waitSwaps(uint64_t swaps, int seconds = 30) {
    return h.until([&] { return app().liveSwaps() >= swaps; }, seconds);
  }
  // Runs frames for `ms` milliseconds.
  void run(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      h.app.frame(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

  TempDir dir;
  std::string addr;
  LiveAgent agent;
  Harness h;
  ThreadsTimelinePanel* timeline = nullptr;
};

}  // namespace tachyon::viewer::live_test
