// Interactive backend: SDL2 window, OpenGL 3.3 core context, Dear ImGui SDL2 + OpenGL3 backends.
#include "viewer/renderer.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <algorithm>

namespace tachyon::viewer {

namespace {

class SdlGlRenderer final : public IRenderer {
 public:
  ~SdlGlRenderer() override { shutdown(); }

  bool init(std::string& error) override {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
      error = std::string("SDL_Init failed: ") + SDL_GetError();
      return false;
    }
    m_sdlUp = true;
    m_wakeEvent = SDL_RegisterEvents(1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    m_window = SDL_CreateWindow("Tachyon", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 900,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!m_window) {
      error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
      return false;
    }
    m_context = SDL_GL_CreateContext(m_window);
    if (!m_context) {
      error = std::string("OpenGL 3.3 context creation failed: ") + SDL_GetError();
      return false;
    }
    SDL_GL_MakeCurrent(m_window, m_context);
    SDL_GL_SetSwapInterval(1);

    if (!ImGui_ImplSDL2_InitForOpenGL(m_window, m_context)) {
      error = "ImGui_ImplSDL2_InitForOpenGL failed";
      return false;
    }
    m_platformUp = true;
    if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
      error = "ImGui_ImplOpenGL3_Init failed";
      return false;
    }
    m_glUp = true;
    return true;
  }

  void shutdown() override {
    if (m_glUp) ImGui_ImplOpenGL3_Shutdown();
    if (m_platformUp) ImGui_ImplSDL2_Shutdown();
    m_glUp = m_platformUp = false;
    if (m_context) SDL_GL_DeleteContext(m_context);
    if (m_window) SDL_DestroyWindow(m_window);
    m_context = nullptr;
    m_window = nullptr;
    if (m_sdlUp) SDL_Quit();
    m_sdlUp = false;
  }

  PlatformEvents pollEvents(int timeoutMs) override {
    PlatformEvents out;
    SDL_Event e;
    bool got = false;
    if (timeoutMs > 0) {
      got = SDL_WaitEventTimeout(&e, timeoutMs) != 0;
    } else {
      got = SDL_PollEvent(&e) != 0;
    }
    while (got) {
      handle(e, out);
      got = SDL_PollEvent(&e) != 0;
    }
    return out;
  }

  void wake() override {
    // SDL_PushEvent is thread-safe. A failed push (queue full) is harmless: the idle timeout still fires.
    const Uint32 type = m_wakeEvent;
    if (type == 0 || type == static_cast<Uint32>(-1)) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    SDL_PushEvent(&e);
  }

  void beginFrame() override {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
  }

  void endFrame() override {
    ImGui::Render();
    const ImGuiIO& io = ImGui::GetIO();
    glViewport(0, 0, static_cast<GLsizei>(io.DisplaySize.x * io.DisplayFramebufferScale.x),
               static_cast<GLsizei>(io.DisplaySize.y * io.DisplayFramebufferScale.y));
    glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(m_window);
  }

  float dpiScale() const override {
    if (!m_window) return 1.0f;
    float ddpi = 0.0f;
    const int display = SDL_GetWindowDisplayIndex(m_window);
    if (display < 0 || SDL_GetDisplayDPI(display, &ddpi, nullptr, nullptr) != 0 || ddpi <= 0.0f) return 1.0f;
    return std::clamp(ddpi / 96.0f, 0.5f, 4.0f);
  }

  void setWindowTitle(const std::string& title) override {
    if (m_window) SDL_SetWindowTitle(m_window, title.c_str());
  }
  bool isHeadless() const override { return false; }

 private:
  void handle(const SDL_Event& e, PlatformEvents& out) {
    if (e.type == m_wakeEvent && m_wakeEvent != 0) return;  // wake(): not input
    ImGui_ImplSDL2_ProcessEvent(&e);
    out.hadInput = true;
    switch (e.type) {
      case SDL_QUIT: out.quit = true; break;
      case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_CLOSE && e.window.windowID == SDL_GetWindowID(m_window)) {
          out.quit = true;
        }
        break;
      case SDL_DROPFILE:
        if (e.drop.file) {
          out.droppedFiles.emplace_back(e.drop.file);
          SDL_free(e.drop.file);
        }
        break;
      default: break;
    }
  }

  SDL_Window* m_window = nullptr;
  SDL_GLContext m_context = nullptr;
  Uint32 m_wakeEvent = 0;  // set once in init(), read by wake() from worker threads afterwards
  bool m_sdlUp = false, m_platformUp = false, m_glUp = false;
};

}  // namespace

std::unique_ptr<IRenderer> makeSdlGlRenderer() {
  return std::make_unique<SdlGlRenderer>();
}

}  // namespace tachyon::viewer
