// WP-48: toasts. The queue with an injected clock (stacking limit, 5 s expiry, dismissal), then in a headless
// App: a failed open and a bad symbol path raise error toasts that are drawn, stack bottom-right and expire.
#include "viewer/status/toast.h"

#include "status_test_util.h"

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::status_test;

TEST(StatusToast, StacksUpToFiveAndDropsTheOldest) {
  double now = 100.0;
  ToastQueue q([&] { return now; });
  for (int i = 0; i < 7; ++i) q.push(ToastLevel::Error, "e" + std::to_string(i));
  ASSERT_EQ(q.toasts().size(), ToastQueue::kMaxVisible);
  EXPECT_EQ(q.toasts().front().text, "e2");
  EXPECT_EQ(q.toasts().back().text, "e6");
}

TEST(StatusToast, ExpiresAfterFiveSeconds) {
  double now = 10.0;
  ToastQueue q([&] { return now; });
  q.push(ToastLevel::Info, "first");
  now = 12.0;
  q.push(ToastLevel::Warning, "second");
  now = 14.99;
  EXPECT_FALSE(q.expire());
  EXPECT_EQ(q.toasts().size(), 2u);
  now = 15.0;
  EXPECT_TRUE(q.expire());
  ASSERT_EQ(q.toasts().size(), 1u);
  EXPECT_EQ(q.toasts()[0].text, "second");
  now = 17.0;
  q.expire();
  EXPECT_TRUE(q.empty());
}

TEST(StatusToast, DismissById) {
  double now = 0.0;
  ToastQueue q([&] { return now; });
  const uint64_t a = q.push(ToastLevel::Info, "a");
  const uint64_t b = q.push(ToastLevel::Info, "b");
  EXPECT_NE(a, b);
  q.dismiss(a);
  ASSERT_EQ(q.toasts().size(), 1u);
  EXPECT_EQ(q.toasts()[0].id, b);
  q.dismiss(12345);
  EXPECT_EQ(q.toasts().size(), 1u);
}

TEST(StatusToast, AppToastsAreDrawnStackedAndExpire) {
  double now = 1.0;
  AppOptions options = headlessOptions("toast");
  options.toastClock = [&] { return now; };
  Harness h(std::move(options));
  h.app.toast(ToastLevel::Error, "first problem");
  h.app.toast(ToastLevel::Warning, "second problem");
  h.frames(3);
  ASSERT_EQ(h.app.toasts().toasts().size(), 2u);
  const ImGuiWindow* w1 =
      ImGui::FindWindowByName(("##toast" + std::to_string(h.app.toasts().toasts()[0].id)).c_str());
  const ImGuiWindow* w2 =
      ImGui::FindWindowByName(("##toast" + std::to_string(h.app.toasts().toasts()[1].id)).c_str());
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);
  EXPECT_TRUE(w1->Active);
  EXPECT_TRUE(w2->Active);
  // Newest at the bottom, the older one above it; both on the right half of the window.
  EXPECT_GT(w2->Pos.y, w1->Pos.y);
  EXPECT_GT(w1->Pos.x, ImGui::GetIO().DisplaySize.x * 0.5f);
  EXPECT_GT(w2->Pos.x, ImGui::GetIO().DisplaySize.x * 0.5f);
  EXPECT_LE(w2->Pos.y + w2->Size.y, ImGui::GetIO().DisplaySize.y);

  now = 6.5;
  h.frames(2);
  EXPECT_TRUE(h.app.toasts().empty());
}

TEST(StatusToast, FailedOpenRaisesAnErrorToast) {
  Harness h;
  h.app.openFile(fixture("does-not-exist.tachyon"));
  ASSERT_TRUE(h.until([&] { return !h.app.loading(); }));
  ASSERT_EQ(h.app.toasts().toasts().size(), 1u);
  EXPECT_EQ(h.app.toasts().toasts()[0].level, ToastLevel::Error);
  EXPECT_NE(h.app.toasts().toasts()[0].text.find("Cannot open"), std::string::npos);
  EXPECT_NE(h.app.toasts().toasts()[0].text.find("does-not-exist"), std::string::npos);
}

TEST(StatusToast, ClickDismisses) {
  Harness h;
  h.app.toast(ToastLevel::Info, "click me");
  h.frames(3);
  const ImGuiWindow* w =
      ImGui::FindWindowByName(("##toast" + std::to_string(h.app.toasts().toasts()[0].id)).c_str());
  ASSERT_NE(w, nullptr);
  h.click(w->Pos.x + w->Size.x * 0.5f, w->Pos.y + w->Size.y * 0.5f);
  EXPECT_TRUE(h.app.toasts().empty());
}
