// IRenderer: window + input + ImGui backend abstraction. Backends: SDL2+OpenGL3 (interactive) and Null
// (headless: no SDL video, no GL; ImGui still runs NewFrame/Render).
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace tachyon::viewer {

struct PlatformEvents {
  bool quit = false;
  bool hadInput = false;                  // anything that should cause a redraw
  std::vector<std::string> droppedFiles;  // UTF-8 paths
};

class IRenderer {
 public:
  virtual ~IRenderer() = default;

  // Main-thread only. Creates the window/GL context and the ImGui backends. An ImGui context must already
  // exist. Returns false (with `error` filled) on failure.
  virtual bool init(std::string& error) = 0;
  // Main-thread only. Destroys backends and window; the ImGui context stays alive.
  virtual void shutdown() = 0;

  // Main-thread only. Processes pending platform events; if `timeoutMs` > 0 and nothing is pending, blocks up
  // to that long for the first event (idle throttling). Events are also forwarded to ImGui.
  virtual PlatformEvents pollEvents(int timeoutMs) = 0;
  // Main-thread only. Backend NewFrame calls (display size, delta time) followed by ImGui::NewFrame().
  virtual void beginFrame() = 0;
  // Main-thread only. ImGui::Render() followed by drawing and presenting (a no-op draw for Null).
  virtual void endFrame() = 0;

  // Main-thread only. Display DPI divided by 96, 1.0 if unknown.
  virtual float dpiScale() const = 0;
  // Main-thread only.
  virtual void setWindowTitle(const std::string& title) = 0;
  // Main-thread only. True when frame pacing is done by the backend (vsync); App still caps at 60 fps.
  virtual bool isHeadless() const = 0;
};

// Thread-safe: factories.
std::unique_ptr<IRenderer> makeNullRenderer();
std::unique_ptr<IRenderer> makeSdlGlRenderer();

}  // namespace tachyon::viewer
