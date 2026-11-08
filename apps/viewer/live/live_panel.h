// The "Live" panel (WP-65b): the controls and numbers of the running live capture: Pause / Resume, Follow,
// Save last 30 s, Stop, window span, latency, dropped events and gaps.
#pragma once

#include "viewer/panel.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace tachyon::viewer {

// Everything the panel and the status bar show, computed by App each time it is asked (main thread).
struct LiveUiState {
  enum class Phase : uint8_t {
    None,     // no live capture
    Waiting,  // started, no data yet
    Running,  // data is arriving and the document follows it
    Ended,    // the capture is over; the document is a static snapshot
    Failed    // the live feed was given up (the capture may still run)
  } phase = Phase::None;
  bool paused = false;
  bool following = false;
  bool followAvailable = false;    // a Live document is open in the timeline
  bool canStop = false;            // a capture is running and can be stopped
  bool canSave = false;            // circular capture running: Save last 30 s works
  std::string saveHint;            // why Save is disabled
  double windowSeconds = 0.0;      // span of the shown snapshot
  std::optional<double> latencyS;  // age of the newest shown sample (local agent only)
  uint64_t samples = 0;            // samples in the shown snapshot
  uint32_t threads = 0;
  uint64_t droppedLive = 0;  // live events dropped by the agent or the decoder (not trace data)
  uint32_t gaps = 0;         // live_gap events
  uint32_t resubscribes = 0;
  uint32_t corruptBatches = 0;
  uint64_t shownGeneration = 0;     // LiveModel generation of the shown snapshot
  uint64_t receivedGeneration = 0;  // LiveModel generation now
  uint64_t swaps = 0;               // snapshots swapped in so far
  double buildMs = 0.0;             // last snapshot + states + index + symbols time on the worker
  uint32_t symbolEpochs = 0;
  uint32_t symbolModulesLoaded = 0;
  std::string feedState;  // "streaming", "resyncing", ...
  std::string error;      // Failed: why
};

struct LivePanelHost {
  std::function<LiveUiState()> state;
  std::function<void(bool)> setPaused;
  std::function<void(bool)> setFollow;
  std::function<void()> saveLast;
  std::function<void()> stop;
};

// Thread-safe: pure function. The status bar text: "LIVE  30.0 s  latency 0.4 s  dropped 0  gaps 0", with
// "(paused)" / "ENDED" / "FAILED" in place of LIVE.
std::string liveStatusText(const LiveUiState& state);

// Thread-safe: factory.
std::unique_ptr<IPanel> makeLivePanel(LivePanelHost host);

inline constexpr const char* kLivePanelName = "Live";

}  // namespace tachyon::viewer
