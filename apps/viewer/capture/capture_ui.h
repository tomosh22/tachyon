// Small ImGui helpers of the capture UI (WP-41).
#pragma once

#include <imgui.h>

#include <string>

namespace tachyon::viewer::capture_ui {

inline int resizeCallback(ImGuiInputTextCallbackData* data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* str = static_cast<std::string*>(data->UserData);
    str->resize(static_cast<size_t>(data->BufTextLen));
    data->Buf = str->data();
  }
  return 0;
}

// Main-thread only. A single-line text field editing a std::string (grows as needed). Returns true on edit.
inline bool inputString(const char* label, std::string& value, const char* hint = nullptr,
                        ImGuiInputTextFlags extra = 0) {
  const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize | extra;
  if (hint)
    return ImGui::InputTextWithHint(label, hint, value.data(), value.capacity() + 1, flags, resizeCallback,
                                    &value);
  return ImGui::InputText(label, value.data(), value.capacity() + 1, flags, resizeCallback, &value);
}

}  // namespace tachyon::viewer::capture_ui
