// Shared helpers of the WP-41 tests (capture dialogs, agent session, Session Explorer).
#pragma once

#include "agent/agent.h"
#include "agent/fake_launch_backend.h"
#include "agent/process_util.h"
#include "capture/mock/mock_backend.h"
#include "control/client.h"
#include "control/server.h"
#include "control/test_util.h"
#include "status_test_util.h"
#include "viewer/capture/capture_controller.h"
#include "viewer/capture/process_launcher.h"
#include "viewer/capture/session_explorer_panel.h"
#include "viewer/paths.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tachyon::viewer::capture_test {

namespace fs = std::filesystem;
using status_test::fixture;
using status_test::Harness;
using status_test::headlessOptions;
using status_test::TempDir;

inline std::string newAddress(const std::string& tag) {
  return control::testing_util::testAddress(tag);
}

// Polls `pred` every millisecond until it holds or `seconds` passed.
inline bool waitFor(const std::function<bool()>& pred, int seconds = 30) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

// A launcher that records what it was asked to start and, unless told otherwise, really starts it (but never
// elevates: pkexec / UAC are never run by the tests).
class TestLauncher final : public IProcessLauncher {
 public:
  Result<LaunchedProcess> launch(const LaunchSpec& spec) override {
    std::lock_guard lock(m_mutex);
    specs.push_back(spec);
    if (!failWith.empty()) return makeError(ErrorCode::IoError, failWith);
    if (!spawn) return LaunchedProcess{4242};
    agent::testing_util::Process p = agent::testing_util::Process::spawn(pathToUtf8(spec.exe), spec.args);
    if (!p.valid()) return makeError(ErrorCode::NotFound, "cannot spawn " + pathToUtf8(spec.exe));
    m_processes.push_back(std::move(p));
    return LaunchedProcess{static_cast<uint32_t>(m_processes.size())};
  }
  size_t launches() const {
    std::lock_guard lock(m_mutex);
    return specs.size();
  }
  LaunchSpec last() const {
    std::lock_guard lock(m_mutex);
    return specs.empty() ? LaunchSpec() : specs.back();
  }
  // Kills every process started so far (the agent "crashing").
  void killAll() {
    std::lock_guard lock(m_mutex);
    for (auto& p : m_processes) p.kill();
    m_processes.clear();
  }

  std::vector<LaunchSpec> specs;
  bool spawn = true;
  std::string failWith;

 private:
  mutable std::mutex m_mutex;
  std::vector<agent::testing_util::Process> m_processes;
};

// An agent process (the real tachyon-agent, mock backend) on a private address.
class AgentProcess {
 public:
  explicit AgentProcess(const std::string& address, int mockDurationMs = 0,
                        const fs::path& mockFixture = fixture("tiny.tachyon"),
                        std::vector<std::string> extraArgs = {})
      : m_address(address) {
    std::vector<std::string> args = {"--backend",          "mock",
                                     "--socket",           address,
                                     "--mock-fixture",     pathToUtf8(mockFixture),
                                     "--mock-duration-ms", std::to_string(mockDurationMs),
                                     "--log-level",        "error"};
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    m_process = agent::testing_util::Process::spawn(TACHYON_AGENT_EXE, args);
    EXPECT_TRUE(m_process.valid());
    // Ready when it accepts a connection.
    auto c = control::Client::connectWithRetry(address, 10000);
    EXPECT_TRUE(c.ok());
  }
  ~AgentProcess() {
    m_process.kill();
    std::error_code ec;
    fs::remove(m_address, ec);
  }
  void kill() { m_process.kill(); }
  const std::string& address() const { return m_address; }

 private:
  std::string m_address;
  agent::testing_util::Process m_process;
};

// Settings for a viewer talking to an agent on `address` with the mock backend; captures go to `dir`.
inline void configureCapture(Settings& settings, const std::string& address, const fs::path& dir) {
  settings.capture.agentAddress = address;
  settings.capture.agentExe = TACHYON_AGENT_EXE;
  settings.capture.agentBackend = "mock";
  settings.capture.agentArgs = {"--mock-fixture", pathToUtf8(fixture("tiny.tachyon")), "--log-level",
                                "error"};
  settings.capture.capturesDir = pathToUtf8(dir);
  settings.capture.autoConnect = false;
}

// An in-process agent (Agent + Server) with the scriptable launch backend, so parking works on every
// platform.
struct InProcessAgent {
  explicit InProcessAgent(const std::string& addr, const fs::path& fixtureTrace) : address(addr) {
    auto fake = std::make_unique<agent::testing_util::FakeLaunchBackend>(fixtureTrace);
    backend = fake.get();
    agent::AgentConfig config;
    config.backendName = "fake";
    config.agentAddress = address;
    config.elevated = false;
    core = std::make_unique<agent::Agent>(std::move(fake), config);
    auto listener = control::listenOn(address);
    EXPECT_TRUE(listener.ok());
    server = std::make_unique<control::Server>(std::move(*listener), *core);
    core->setEventSink([this](const control::Event& e) { server->broadcast(e); });
    server->start();
  }
  ~InProcessAgent() {
    server->stop();
    core->shutdown();
  }
  std::string address;
  agent::testing_util::FakeLaunchBackend* backend = nullptr;
  std::unique_ptr<agent::Agent> core;
  std::unique_ptr<control::Server> server;
};

// A server that accepts connections and reads requests but never answers: a hung agent.
class BlackholeServer {
 public:
  explicit BlackholeServer(const std::string& address) : m_address(address) {
    auto listener = control::listenOn(address);
    EXPECT_TRUE(listener.ok());
    m_listener = std::move(*listener);
    m_thread = std::thread([this] {
      for (;;) {
        auto conn = m_listener->accept();
        if (!conn.ok()) return;
        std::shared_ptr<control::IConnection> shared = std::move(*conn);
        {
          std::lock_guard lock(m_mutex);
          m_connections.push_back(shared);
        }
        m_readers.emplace_back([shared] {
          std::string line;
          for (;;) {
            auto r = shared->readLine(line, -1);
            if (!r.ok() || !*r) return;
          }
        });
      }
    });
  }
  ~BlackholeServer() {
    m_listener->close();
    {
      std::lock_guard lock(m_mutex);
      for (auto& c : m_connections) c->close();
    }
    if (m_thread.joinable()) m_thread.join();
    for (auto& t : m_readers) t.join();
    std::error_code ec;
    fs::remove(m_address, ec);
  }

 private:
  std::string m_address;
  std::unique_ptr<control::IListener> m_listener;
  std::thread m_thread;
  std::mutex m_mutex;
  std::vector<std::shared_ptr<control::IConnection>> m_connections;
  std::vector<std::thread> m_readers;
};

}  // namespace tachyon::viewer::capture_test
