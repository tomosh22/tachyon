// WP-48 viewer follow-ups: (a) the Butterfly says when the selected key forces inline frames on / kernel
// frames to be grouped, (c) closing the Function Statistics panel by any route (View menu, command) takes the
// timeline instance highlight off, through the generic IPanel::onVisibilityChanged hook.
#include "analysis/query.h"
#include "viewer/functions/butterfly_panel.h"
#include "viewer/stats/function_stats_panel.h"

#include "status_test_util.h"

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::status_test;
using symbols::FunctionKey;

namespace {

template <typename T>
T* panelOf(App& app, const char* name) {
  PanelEntry* e = app.panels().find(name);
  return e ? dynamic_cast<T*>(e->panel.get()) : nullptr;
}

}  // namespace

TEST(StatusFollowups, ButterflyShowsWhenInlineIsForced) {
  Harness h;
  ASSERT_TRUE(h.open(fixture("tiny.tachyon")));
  ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
  auto* butterfly = panelOf<ButterflyPanel>(h.app, ButterflyPanel::kName);
  ASSERT_NE(butterfly, nullptr);
  h.app.panels().find(ButterflyPanel::kName)->open = true;
  h.frames(3);
  EXPECT_FALSE(butterfly->inlineForced());
  EXPECT_FALSE(butterfly->kernelForced());

  // An inline key with the "Inline frames" toggle off: the panel forces the expansion on and says so.
  ASSERT_FALSE(butterfly->inlineExpansion());
  const FunctionKey inlineKey{ModuleIdx(0), (uint64_t{1} << 63) | 5u};
  ASSERT_TRUE(inlineKey.isInline());
  h.app.selection().setSelectedFunction(inlineKey);
  h.frames(3);
  EXPECT_TRUE(butterfly->inlineForced());
  EXPECT_FALSE(butterfly->kernelForced());
  EXPECT_FALSE(butterfly->inlineExpansion());  // the toggle keeps the user's own setting
  EXPECT_TRUE(butterfly->requestedSpec().base.expandInline);
  EXPECT_STREQ(ButterflyPanel::kInlineForcedHint, "inline frames on for this selection");

  // Switching the toggle on makes the hint unnecessary.
  butterfly->setInlineExpansion(true);
  h.frames(3);
  EXPECT_FALSE(butterfly->inlineForced());

  // The merged kernel key with kernel frames shown: grouped for this selection.
  butterfly->setInlineExpansion(false);
  h.app.selection().setSelectedFunction(analysis::kKernelKey);
  h.frames(3);
  EXPECT_TRUE(butterfly->kernelForced());
  EXPECT_FALSE(butterfly->inlineForced());
  EXPECT_EQ(butterfly->kernelFrames(), analysis::KernelFrames::Show);
  EXPECT_EQ(butterfly->requestedSpec().base.kernelFrames, analysis::KernelFrames::Group);
  EXPECT_STREQ(ButterflyPanel::kKernelForcedHint, "kernel grouped for this selection");

  // A plain function: no hint.
  h.app.selection().setSelectedFunction(FunctionKey{ModuleIdx(0), 0x10});
  h.frames(3);
  EXPECT_FALSE(butterfly->inlineForced());
  EXPECT_FALSE(butterfly->kernelForced());
  // Clearing the selection clears the hints too.
  h.app.selection().setSelectedFunction(analysis::kKernelKey);
  h.frames(3);
  ASSERT_TRUE(butterfly->kernelForced());
  h.app.selection().setSelectedFunction(std::nullopt);
  h.frames(3);
  EXPECT_FALSE(butterfly->kernelForced());
}

TEST(StatusFollowups, ClosingStatsThroughTheViewMenuClearsTheHighlight) {
  Harness h;
  ASSERT_TRUE(h.open(fixture("scopes.tachyon")));
  ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
  auto* stats = panelOf<FunctionStatsPanel>(h.app, FunctionStatsPanel::kName);
  ASSERT_NE(stats, nullptr);
  PanelEntry* entry = h.app.panels().find(FunctionStatsPanel::kName);
  entry->open = true;  // what Ctrl+F / the View menu do
  ASSERT_TRUE(h.until([&] { return stats->index() && !stats->indexPending(); }));

  stats->setQueryText("Frame");
  PanelContext context;
  context.document = h.app.document().get();
  context.selection = &h.app.selection();
  context.symbols = h.app.document()->symbols().get();
  context.queryExecutor = &h.app.queryExecutor();
  context.documentGeneration = h.app.documentGeneration();
  context.symbolGeneration = h.app.symbolGeneration();
  ASSERT_FALSE(stats->hits().empty());
  stats->chooseResult(context, 0);
  ASSERT_TRUE(h.until([&] { return stats->result() != nullptr; }));
  h.frames(3);
  ASSERT_TRUE(h.app.selection().state().instanceHighlight.active());

  // The View menu item toggles PanelEntry::open directly; the next frame must clear the highlight.
  entry->open = false;
  h.frames(2);
  EXPECT_FALSE(h.app.selection().state().instanceHighlight.active());
  EXPECT_EQ(stats->result(), nullptr);

  // Reopening shows it again.
  entry->open = true;
  ASSERT_TRUE(h.until([&] { return stats->result() != nullptr; }));
  h.frames(3);
  EXPECT_TRUE(h.app.selection().state().instanceHighlight.active());
}

namespace {
class VisibilityPanel final : public IPanel {
 public:
  const char* name() const override { return "Visibility Probe"; }
  void draw(const PanelContext&, bool* open) override {
    if (ImGui::Begin(name(), open)) ImGui::TextUnformatted("probe");
    ImGui::End();
  }
  void onVisibilityChanged(bool visible) override { events.push_back(visible); }
  std::vector<bool> events;
};
}  // namespace

TEST(StatusFollowups, AppReportsPanelVisibilityChanges) {
  Harness h;
  auto probe = std::make_unique<VisibilityPanel>();
  VisibilityPanel* p = probe.get();
  ASSERT_TRUE(h.app.panels().add(std::move(probe)));
  h.frames(2);
  ASSERT_EQ(p->events.size(), 1u);  // told once that it is open
  EXPECT_TRUE(p->events[0]);
  h.frames(3);
  EXPECT_EQ(p->events.size(), 1u);                        // not repeated
  h.app.panels().find("Visibility Probe")->open = false;  // View menu
  h.frames(2);
  ASSERT_EQ(p->events.size(), 2u);
  EXPECT_FALSE(p->events[1]);
  h.app.panels().find("Visibility Probe")->open = true;
  h.frames(2);
  ASSERT_EQ(p->events.size(), 3u);
  EXPECT_TRUE(p->events[2]);
}
