#include "viewer/capture/agent_session.h"

#include "control/auth.h"
#include "control/tcp.h"
#include "core/log.h"

#include <algorithm>
#include <atomic>

namespace tachyon::viewer {

namespace {
using Clock = std::chrono::steady_clock;
}  // namespace

const char* connectionStateName(ConnectionState state) {
  switch (state) {
    case ConnectionState::Disconnected: return "Disconnected";
    case ConnectionState::Connecting: return "Connecting";
    case ConnectionState::Connected: return "Connected";
    case ConnectionState::Lost: return "Lost";
  }
  return "?";
}

const char* agentPhaseName(AgentPhase phase) {
  switch (phase) {
    case AgentPhase::Unknown: return "unknown";
    case AgentPhase::Idle: return "idle";
    case AgentPhase::Starting: return "starting";
    case AgentPhase::Armed: return "armed";
    case AgentPhase::Capturing: return "capturing";
    case AgentPhase::Stopping: return "stopping";
  }
  return "?";
}

AgentSession::AgentSession(Options options) : m_options(std::move(options)) {}

AgentSession::~AgentSession() {
  {
    std::lock_guard lock(m_mutex);
    m_quit = true;
    m_commands.clear();
  }
  m_quitFlag.store(true);
  m_cv.notify_all();
  {
    std::lock_guard link(m_linkMutex);
    if (m_activeClient) m_activeClient->close();  // unblocks a request in flight
  }
  if (m_worker.joinable()) m_worker.join();
}

bool AgentSession::commandPending() const {
  std::lock_guard lock(m_mutex);
  return !m_commands.empty();
}

void AgentSession::notify() {
  if (m_options.wake) m_options.wake();
}

void AgentSession::post(Command command) {
  {
    std::lock_guard lock(m_mutex);
    if (m_quit) return;
    if (!m_workerStarted) {
      m_workerStarted = true;
      m_worker = std::thread([this] { workerMain(); });
    }
    m_commands.push_back(std::move(command));
  }
  m_cv.notify_all();
}

void AgentSession::connect(std::string address, int retryMs, bool userInitiated, std::string tokenFile) {
  Command c;
  c.kind = Command::Kind::Connect;
  c.address = std::move(address);
  c.tokenFile = std::move(tokenFile);
  c.retryMs = std::max(0, retryMs);
  c.userInitiated = userInitiated;
  {
    // Visible at once, before the worker gets to it.
    std::lock_guard lock(m_mutex);
    m_snapshot.connection = ConnectionState::Connecting;
    m_snapshot.address = c.address;
    m_snapshot.remote = control::isTcpAddress(c.address);
  }
  post(std::move(c));
}

void AgentSession::disconnect() {
  Command c;
  c.kind = Command::Kind::Disconnect;
  post(std::move(c));
}

void AgentSession::start(control::StartRequest request, std::string targetLabel) {
  Command c;
  c.kind = Command::Kind::Start;
  c.start = request;
  c.label = std::move(targetLabel);
  {
    std::lock_guard lock(m_mutex);
    m_lastStart = std::move(request);
  }
  post(std::move(c));
}

void AgentSession::resume() {
  Command c;
  c.kind = Command::Kind::Resume;
  post(std::move(c));
}

void AgentSession::stop() {
  Command c;
  c.kind = Command::Kind::Stop;
  post(std::move(c));
}

void AgentSession::cancel() {
  Command c;
  c.kind = Command::Kind::Cancel;
  post(std::move(c));
}

void AgentSession::flush(std::string name, uint32_t seconds, std::string reason) {
  Command c;
  c.kind = Command::Kind::Flush;
  c.flush.name = std::move(name);
  c.flush.seconds = seconds;
  c.flush.reason = std::move(reason);
  post(std::move(c));
}

void AgentSession::setLiveModel(std::shared_ptr<model::LiveModel> model) {
  std::lock_guard lock(m_mutex);
  m_liveModel = std::move(model);
}

void AgentSession::setLiveNotify(std::function<void()> notify) {
  std::lock_guard lock(m_mutex);
  m_liveNotify = std::move(notify);
}

LiveFeedStatus AgentSession::liveStatus() const {
  std::lock_guard lock(m_mutex);
  return m_liveStatus;
}

void AgentSession::refreshStatus() {
  Command c;
  c.kind = Command::Kind::Refresh;
  post(std::move(c));
}

void AgentSession::listProcesses() {
  Command c;
  c.kind = Command::Kind::Processes;
  post(std::move(c));
}

void AgentSession::stopLiveFeed() {
  Command c;
  c.kind = Command::Kind::LiveOff;
  post(std::move(c));
}

AgentSnapshot AgentSession::snapshot() const {
  std::lock_guard lock(m_mutex);
  AgentSnapshot s = m_snapshot;
  if (s.phase == AgentPhase::Capturing)
    s.elapsedSeconds = std::chrono::duration<double>(Clock::now() - m_captureStart).count();
  return s;
}

std::vector<AgentEvent> AgentSession::drainEvents() {
  std::lock_guard lock(m_mutex);
  std::vector<AgentEvent> out;
  out.swap(m_events);
  return out;
}

bool AgentSession::hasEvents() const {
  std::lock_guard lock(m_mutex);
  return !m_events.empty();
}

control::StartRequest AgentSession::lastStartRequest() const {
  std::lock_guard lock(m_mutex);
  return m_lastStart;
}

void AgentSession::push(AgentEvent event) {
  {
    std::lock_guard lock(m_mutex);
    m_events.push_back(std::move(event));
  }
  notify();
}

void AgentSession::setPhase(AgentPhase phase) {
  bool changed = false;
  {
    std::lock_guard lock(m_mutex);
    if (m_snapshot.phase != phase) {
      if (phase == AgentPhase::Capturing) m_captureStart = Clock::now();
      if (phase == AgentPhase::Idle || phase == AgentPhase::Unknown) {
        m_snapshot.target.clear();
        m_snapshot.circularMb = 0;
      }
      if (phase != AgentPhase::Stopping) m_snapshot.progress = 0.0;
      m_snapshot.phase = phase;
      changed = true;
    }
  }
  if (changed) {
    AgentEvent e;
    e.kind = AgentEventKind::StateChanged;
    e.phase = phase;
    push(std::move(e));
  }
}

void AgentSession::applyPhase(capture::CaptureState state) {
  AgentPhase phase = AgentPhase::Unknown;
  {
    std::lock_guard lock(m_mutex);
    switch (state) {
      case capture::CaptureState::Idle:
        phase = AgentPhase::Idle;
        m_expectParked = false;
        break;
      case capture::CaptureState::Starting:
        phase = m_expectParked ? AgentPhase::Armed : AgentPhase::Starting;
        break;
      case capture::CaptureState::Capturing:
        phase = AgentPhase::Capturing;
        m_expectParked = false;
        break;
      case capture::CaptureState::Converting:
      case capture::CaptureState::Done: phase = AgentPhase::Stopping; break;
    }
  }
  setPhase(phase);
}

void AgentSession::applyStatus(const control::StatusBody& status) {
  {
    std::lock_guard lock(m_mutex);
    m_snapshot.backend = status.backend;
    m_snapshot.elevated = status.elevated;
    m_snapshot.progress = status.progress;
  }
  applyPhase(status.state);
  // WP-65b: a live start whose capture never got going (the agent went back to idle) will never accept
  // live_subscribe: stop trying.
  if (m_liveWanted && !m_liveSubscribed) {
    if (status.state == capture::CaptureState::Idle) {
      if (++m_liveIdlePolls >= 3) failLive("the live capture did not start");
    } else {
      m_liveIdlePolls = 0;
    }
  }
  notify();
}

void AgentSession::applyEvent(const control::Event& event) {
  if (const auto* state = std::get_if<control::StateEvent>(&event)) {
    {
      std::lock_guard lock(m_mutex);
      m_snapshot.progress = state->progress;
    }
    applyPhase(state->state);
    notify();
  } else if (const auto* done = std::get_if<control::CaptureDoneEvent>(&event)) {
    AgentEvent e;
    e.kind = AgentEventKind::CaptureDone;
    e.summary = done->summary;
    push(std::move(e));
  } else if (const auto* flushed = std::get_if<control::FlushedEvent>(&event)) {
    AgentEvent e;
    e.kind = AgentEventKind::Flushed;
    e.flushed = *flushed;
    push(std::move(e));
  } else if (const auto* error = std::get_if<control::ErrorEvent>(&event)) {
    {
      std::lock_guard lock(m_mutex);
      m_snapshot.lastError = error->message;
    }
    AgentEvent e;
    e.kind = AgentEventKind::AgentError;
    e.message = error->message;
    push(std::move(e));
  }
}

void AgentSession::dropClient() {
  {
    std::lock_guard link(m_linkMutex);
    m_activeClient = nullptr;
  }
  m_client.reset();
}

void AgentSession::lose(const std::string& why) {
  dropClient();
  endLiveFeed(why);
  {
    std::lock_guard lock(m_mutex);
    m_snapshot.connection = ConnectionState::Lost;
    m_snapshot.phase = AgentPhase::Unknown;
    m_snapshot.progress = 0.0;
    m_snapshot.commandInFlight = false;
    m_snapshot.target.clear();
    m_snapshot.lastError = why;
    m_expectParked = false;
  }
  AgentEvent e;
  e.kind = AgentEventKind::ConnectionLost;
  e.message = why;
  push(std::move(e));
}

void AgentSession::pollStatus() {
  if (!m_client) return;
  m_lastPoll = Clock::now();
  auto r = m_client->call(control::StatusRequest{}, m_options.callTimeoutMs);
  if (!r.ok()) {
    if (!m_quitFlag.load()) lose("lost the connection to the agent: " + r.error().message);
    return;
  }
  if (!r->ok) {
    std::lock_guard lock(m_mutex);
    m_snapshot.lastError = r->error;
    return;
  }
  if (const auto* body = std::get_if<control::StatusBody>(&r->body)) applyStatus(*body);
}

void AgentSession::doConnect(const Command& command) {
  dropClient();
  {
    std::lock_guard lock(m_mutex);
    m_snapshot.connection = ConnectionState::Connecting;
    m_snapshot.address = command.address;
    m_snapshot.remote = control::isTcpAddress(command.address);
    m_snapshot.phase = AgentPhase::Unknown;
    m_snapshot.lastError.clear();
    m_expectParked = false;
  }
  notify();
  const auto deadline = Clock::now() + std::chrono::milliseconds(command.retryMs);
  std::string lastError;
  std::string token;
  if (control::isTcpAddress(command.address)) {
    // The token file is read here, off the main thread, and only its path is ever stored.
    auto loaded = control::loadTokenFile(command.tokenFile, 1);
    if (!loaded.ok()) {
      lastError = command.tokenFile.empty()
                      ? "a remote agent needs a token file (Capture > Connect to remote "
                        "agent...)"
                      : loaded.error().message;
      {
        std::lock_guard lock(m_mutex);
        m_snapshot.connection = ConnectionState::Disconnected;
        m_snapshot.lastError = lastError;
      }
      AgentEvent e;
      e.kind = AgentEventKind::ConnectFailed;
      e.message = lastError;
      e.userInitiated = command.userInitiated;
      push(std::move(e));
      return;
    }
    token = std::move(*loaded);
  }
  for (;;) {
    auto c = control::Client::connect(command.address, token);
    if (c.ok()) {
      m_client.emplace(std::move(*c));
      m_client->setLiveHandler([this](const control::Event& e) { onLiveEvent(e); });
      {
        std::lock_guard link(m_linkMutex);
        m_activeClient = &*m_client;
        if (m_quitFlag.load()) m_client->close();
      }
      {
        std::lock_guard lock(m_mutex);
        m_snapshot.connection = ConnectionState::Connected;
      }
      AgentEvent e;
      e.kind = AgentEventKind::Connected;
      e.userInitiated = command.userInitiated;
      push(std::move(e));
      pollStatus();
      return;
    }
    lastError = c.error().message;
    // A refused token or a malformed address will not get better by retrying.
    if (c.error().code == ErrorCode::NoPrivilege || c.error().code == ErrorCode::InvalidArgument) break;
    if (m_quitFlag.load() || Clock::now() >= deadline || commandPending()) break;  // a newer command wins
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  {
    std::lock_guard lock(m_mutex);
    m_snapshot.connection = ConnectionState::Disconnected;
    m_snapshot.lastError = "cannot connect to the agent at " + command.address + ": " + lastError;
  }
  AgentEvent e;
  e.kind = AgentEventKind::ConnectFailed;
  e.message = "cannot connect to the agent at " + command.address + ": " + lastError;
  e.userInitiated = command.userInitiated;
  push(std::move(e));
}

bool AgentSession::doCall(const char* name, control::RequestBody body) {
  auto r = m_client->call(std::move(body), m_options.callTimeoutMs);
  if (!r.ok()) {
    if (!m_quitFlag.load())
      lose(std::string("lost the connection to the agent during ") + name + ": " + r.error().message);
    return false;
  }
  if (!r->ok) {
    {
      std::lock_guard lock(m_mutex);
      m_snapshot.lastError = r->error;
    }
    AgentEvent e;
    e.kind = AgentEventKind::CommandFailed;
    e.command = name;
    e.code = std::string(control::protocolCode(r->code));
    e.message = r->error;
    push(std::move(e));
    return false;
  }
  return true;
}

void AgentSession::handleCommand(const Command& command) {
  using Kind = Command::Kind;
  {
    std::lock_guard lock(m_mutex);
    m_snapshot.commandInFlight = true;
  }
  notify();
  if (command.kind == Kind::Connect) {
    doConnect(command);
  } else if (command.kind == Kind::Disconnect) {
    dropClient();
    endLiveFeed("disconnected from the agent");
    std::lock_guard lock(m_mutex);
    m_snapshot.connection = ConnectionState::Disconnected;
    m_snapshot.phase = AgentPhase::Unknown;
    m_snapshot.target.clear();
    m_snapshot.progress = 0.0;
    m_expectParked = false;
  } else if (!m_client && command.kind == Kind::LiveOff) {
    // Closing a live view after the agent was lost: nothing to unsubscribe from, and nothing to report.
  } else if (!m_client) {
    AgentEvent e;
    e.kind = AgentEventKind::CommandFailed;
    e.command = command.kind == Kind::Start ? "start" : command.kind == Kind::Stop ? "stop" : "command";
    e.code = "ENOTCONNECTED";
    e.message = "not connected to the agent";
    push(std::move(e));
  } else {
    switch (command.kind) {
      case Kind::Start: {
        {
          std::lock_guard lock(m_mutex);
          m_expectParked =
              command.start.mode == capture::CaptureMode::Launch && !command.start.start_immediately;
          m_snapshot.target = command.label;
          m_snapshot.circularMb = command.start.circular_mb;
        }
        // A new capture: whatever the previous live feed was is over.
        m_liveWanted = false;
        m_liveSubscribed = false;
        m_liveResync = false;
        m_liveIgnore = false;
        {
          std::lock_guard lock(m_mutex);
          m_liveStatus = {};
        }
        if (doCall("start", command.start) && command.start.live) beginLiveFeed();
        break;
      }
      case Kind::Resume: {
        control::StartRequest trigger;
        trigger.mode = capture::CaptureMode::Launch;
        trigger.exe = "-";  // the agent's "resume" form (agent.h): never a program to launch
        trigger.start_immediately = true;
        {
          std::lock_guard lock(m_mutex);
          m_expectParked = false;
        }
        doCall("resume", trigger);
        break;
      }
      case Kind::Stop: doCall("stop", control::StopRequest{}); break;
      case Kind::Cancel: doCall("cancel", control::CancelRequest{}); break;
      case Kind::LiveOff: {
        const bool wasLive = m_liveWanted || m_liveSubscribed;
        m_liveWanted = false;
        m_liveSubscribed = false;
        m_liveResync = false;
        m_liveIgnore = true;
        if (wasLive) doCall("live_unsubscribe", control::LiveUnsubscribeRequest{});
        std::lock_guard lock(m_mutex);
        m_liveStatus.state = LiveFeedStatus::State::Off;
        break;
      }
      case Kind::Flush: doCall("flush", command.flush); break;
      case Kind::Processes: {
        AgentEvent e;
        e.kind = AgentEventKind::ProcessList;
        auto list = control::fetchProcessList(*m_client, m_options.callTimeoutMs);
        if (list.ok()) {
          e.processes = std::move(*list);
        } else if (list.error().code == ErrorCode::IoError || list.error().code == ErrorCode::Timeout) {
          lose("lost the connection to the agent during processes: " + list.error().message);
          break;
        } else {
          e.message = list.error().message;
        }
        push(std::move(e));
        break;
      }
      default: break;
    }
    if (m_client) pollStatus();
    if (m_client) {
      // A start that failed leaves the agent idle; do not keep claiming a target.
      std::lock_guard lock(m_mutex);
      if (m_snapshot.phase == AgentPhase::Idle) {
        m_snapshot.target.clear();
        m_snapshot.circularMb = 0;
      }
    }
  }
  {
    std::lock_guard lock(m_mutex);
    m_snapshot.commandInFlight = false;
  }
  notify();
}

// ---- WP-65b: live feed (worker thread) ----

void AgentSession::beginLiveFeed() {
  m_liveWanted = true;
  m_liveSubscribed = false;
  m_liveIgnore = false;
  m_liveResync = false;
  m_liveIdlePolls = 0;
  m_lastCorruptAt = {};
  {
    std::lock_guard lock(m_mutex);
    m_liveStatus = {};
    m_liveStatus.state = LiveFeedStatus::State::Subscribing;
  }
  tryLiveSubscribe();
}

void AgentSession::tryLiveSubscribe() {
  if (!m_client) return;
  m_lastLiveTry = Clock::now();
  m_liveIgnore = true;  // events of an earlier subscription may still be in flight: drop them
  auto r = m_client->call(control::LiveSubscribeRequest{}, m_options.callTimeoutMs);
  m_liveIgnore = false;
  if (!r.ok()) {
    if (!m_quitFlag.load())
      lose("lost the connection to the agent during live_subscribe: " + r.error().message);
    return;
  }
  if (r->ok) {
    m_liveSubscribed = true;
    std::lock_guard lock(m_mutex);
    if (m_liveStatus.state != LiveFeedStatus::State::Resyncing)
      m_liveStatus.state = LiveFeedStatus::State::Streaming;
    return;
  }
  if (r->code == ErrorCode::NotFound) {
    // Not live yet (parked launch, still starting), or the capture has just ended.
    bool resyncing = false;
    {
      std::lock_guard lock(m_mutex);
      resyncing = m_liveStatus.state == LiveFeedStatus::State::Resyncing;
    }
    if (resyncing) endLiveFeed("the live capture ended");
    return;
  }
  failLive("the agent refused live_subscribe (" + std::string(control::protocolCode(r->code)) +
           "): " + r->error);
}

void AgentSession::resubscribeLive(const char* why) {
  m_liveSubscribed = false;
  {
    std::lock_guard lock(m_mutex);
    ++m_liveStatus.resubscribes;
    m_liveStatus.state = LiveFeedStatus::State::Resyncing;
  }
  TACHYON_LOG_INFO("live: resubscribing ({})", why);
  tryLiveSubscribe();
}

void AgentSession::pollLive() {
  if (!m_client || !m_liveWanted) return;
  if (m_liveResync) {
    m_liveResync = false;
    resubscribeLive(m_liveResyncWhy.c_str());
    return;
  }
  if (!m_liveSubscribed && Clock::now() - m_lastLiveTry >= std::chrono::milliseconds(100)) tryLiveSubscribe();
}

void AgentSession::failLive(const std::string& message) {
  const bool wasSubscribed = m_liveSubscribed;
  m_liveWanted = false;
  m_liveSubscribed = false;
  m_liveResync = false;
  m_liveIgnore = true;
  {
    std::lock_guard lock(m_mutex);
    m_liveStatus.state = LiveFeedStatus::State::Failed;
    m_liveStatus.error = message;
  }
  if (wasSubscribed && m_client) {
    // Stop the stream so the agent does not keep queueing for a client that ignores it.
    (void)m_client->call(control::LiveUnsubscribeRequest{}, m_options.callTimeoutMs);
  }
  AgentEvent e;
  e.kind = AgentEventKind::LiveFailed;
  e.message = message;
  push(std::move(e));
}

void AgentSession::endLiveFeed(const std::string& why) {
  bool live = m_liveWanted || m_liveSubscribed;
  {
    std::lock_guard lock(m_mutex);
    live = live || m_liveStatus.state == LiveFeedStatus::State::Subscribing ||
           m_liveStatus.state == LiveFeedStatus::State::Streaming ||
           m_liveStatus.state == LiveFeedStatus::State::Resyncing;
    if (live) m_liveStatus.state = LiveFeedStatus::State::Ended;
  }
  m_liveWanted = false;
  m_liveSubscribed = false;
  m_liveResync = false;
  if (!live) return;
  AgentEvent e;
  e.kind = AgentEventKind::LiveEnded;
  e.message = why;
  push(std::move(e));
}

void AgentSession::onLiveEvent(const control::Event& event) {
  if (m_liveIgnore || !m_liveWanted) return;
  if (const auto* batch = std::get_if<control::LiveEvent>(&event)) {
    std::shared_ptr<model::LiveModel> model;
    {
      std::lock_guard lock(m_mutex);
      model = m_liveModel;
    }
    if (!model) return;
    const Result<void> applied =
        model->apply(std::span<const std::byte>(reinterpret_cast<const std::byte*>(batch->data.data()),
                                                batch->data.size()),
                     batch->keyframe);
    std::function<void()> notifyLive;
    {
      std::lock_guard lock(m_mutex);
      m_liveStatus.seq = batch->seq;
      ++m_liveStatus.events;
      m_liveStatus.bytes += batch->data.size();
      m_liveStatus.lastEventAt = Clock::now();
      if (applied.ok() && m_liveStatus.state == LiveFeedStatus::State::Resyncing)
        m_liveStatus.state = LiveFeedStatus::State::Streaming;
      if (!applied.ok()) ++m_liveStatus.corruptBatches;
      notifyLive = m_liveNotify;
    }
    if (applied.ok()) {
      if (notifyLive) notifyLive();
      return;
    }
    // A malformed batch: everything after it is useless until a keyframe. Resubscribe once; a repeat within
    // 30 s means the agent (or the stream) is broken.
    const auto now = Clock::now();
    const bool repeated =
        m_lastCorruptAt != Clock::time_point{} && now - m_lastCorruptAt < std::chrono::seconds(30);
    m_lastCorruptAt = now;
    TACHYON_LOG_WARN("live: malformed batch {}: {}", batch->seq, applied.error().message);
    if (repeated) {
      failLive("the agent sent malformed live data twice: " + applied.error().message);
      return;
    }
    m_liveIgnore = true;
    m_liveResync = true;
    m_liveResyncWhy = "malformed batch: " + applied.error().message;
    return;
  }
  if (const auto* gap = std::get_if<control::LiveGapEvent>(&event)) {
    {
      std::lock_guard lock(m_mutex);
      ++m_liveStatus.gaps;
    }
    m_liveIgnore = true;
    m_liveResync = true;
    m_liveResyncWhy = "live_gap at " + std::to_string(gap->seq);
    AgentEvent e;
    e.kind = AgentEventKind::LiveGap;
    e.message = "the live view fell behind (batch " + std::to_string(gap->seq) + "); resynchronising";
    push(std::move(e));
    return;
  }
  if (std::holds_alternative<control::LiveEndEvent>(event)) {
    endLiveFeed("the live capture ended");
    std::function<void()> notifyLive;
    {
      std::lock_guard lock(m_mutex);
      notifyLive = m_liveNotify;
    }
    if (notifyLive) notifyLive();
  }
}

void AgentSession::workerMain() {
  for (;;) {
    Command command;
    bool have = false;
    {
      std::unique_lock lock(m_mutex);
      if (!m_client) m_cv.wait(lock, [this] { return m_quit || !m_commands.empty(); });
      if (m_quit) break;
      if (!m_commands.empty()) {
        command = std::move(m_commands.front());
        m_commands.pop_front();
        have = true;
      }
    }
    if (have) {
      handleCommand(command);
      continue;
    }
    if (!m_client) continue;
    pollLive();
    if (!m_client) continue;
    auto event = m_client->nextEvent(m_options.eventWaitMs);
    if (event.ok()) {
      applyEvent(*event);
    } else if (event.error().code != ErrorCode::Timeout) {
      if (m_quitFlag.load()) break;
      lose("lost the connection to the agent: " + event.error().message);
      continue;
    }
    if (m_client && Clock::now() - m_lastPoll >= std::chrono::milliseconds(m_options.pollIntervalMs))
      pollStatus();
  }
  dropClient();
}

}  // namespace tachyon::viewer
