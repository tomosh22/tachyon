// Toasts (WP-48): short messages in the bottom-right corner of the main window. They stack (at most
// kMaxVisible; a newer toast pushes the oldest out), dismiss themselves after kLifetimeSeconds and can be
// dismissed with a click. Main-thread only; the clock is injectable so tests control expiry.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tachyon::viewer {

enum class ToastLevel : uint8_t { Info, Warning, Error };

struct Toast {
  uint64_t id = 0;
  ToastLevel level = ToastLevel::Info;
  std::string text;
  double createdAt = 0.0;  // seconds on the queue's clock
};

class ToastQueue {
 public:
  static constexpr size_t kMaxVisible = 5;
  static constexpr double kLifetimeSeconds = 5.0;
  using ClockFn = std::function<double()>;  // seconds, monotonic

  // Main-thread only. An empty `clock` uses std::chrono::steady_clock.
  explicit ToastQueue(ClockFn clock = {});

  // Main-thread only. Adds a toast; beyond kMaxVisible the oldest ones are dropped. Returns its id.
  uint64_t push(ToastLevel level, std::string text);
  // Main-thread only. Removes the toasts older than kLifetimeSeconds. Returns true if any was removed.
  bool expire();
  // Main-thread only.
  void dismiss(uint64_t id);
  // Main-thread only.
  void clear() { m_toasts.clear(); }
  // Main-thread only. Oldest first.
  const std::vector<Toast>& toasts() const { return m_toasts; }
  // Main-thread only.
  bool empty() const { return m_toasts.empty(); }
  // Main-thread only. Seconds on the queue's clock.
  double now() const;

  // Main-thread only. Expires, then draws the stack above the bottom edge of the main viewport's work area.
  void draw();

 private:
  ClockFn m_clock;
  std::vector<Toast> m_toasts;
  uint64_t m_nextId = 1;
  std::unordered_map<uint64_t, float> m_heights;  // last drawn window height per toast (stacking)
};

}  // namespace tachyon::viewer
