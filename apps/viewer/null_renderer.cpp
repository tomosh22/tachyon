// Headless backend: ImGui runs a full NewFrame/Render cycle with a manually configured display and font
// atlas.
#include "viewer/renderer.h"

#include <imgui.h>

#include <cstdint>

namespace tachyon::viewer {

namespace {

class NullRenderer final : public IRenderer {
 public:
  bool init(std::string&) override {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    // Build the font atlas without a GPU: ImGui only needs the pixel data to exist and a texture id.
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)1);  // NOLINT: type is void* or an integer by version
    return pixels != nullptr;
  }
  void shutdown() override {}

  PlatformEvents pollEvents(int) override { return {}; }
  void beginFrame() override {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
  }
  void endFrame() override { ImGui::Render(); }

  float dpiScale() const override { return 1.0f; }
  void setWindowTitle(const std::string&) override {}
  bool isHeadless() const override { return true; }
};

}  // namespace

std::unique_ptr<IRenderer> makeNullRenderer() {
  return std::make_unique<NullRenderer>();
}

}  // namespace tachyon::viewer
