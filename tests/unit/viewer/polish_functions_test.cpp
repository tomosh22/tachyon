// WP-67 item 5: selecting "<no stack>" (kRootKey) in the Function List gives the Butterfly the row's totals
// with an explanatory note (no callers or callees), instead of an empty panel.
#include "trace/trace_writer.h"
#include "viewer/functions/butterfly_panel.h"
#include "viewer/functions/function_list_panel.h"

#include "status_test_util.h"

#include <span>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::status_test;

namespace {

// 6 samples of one stack and 4 without any frame, on one recorded thread.
void writeFramelessTrace(const fs::path& path) {
  using namespace trace;
  FileHeader h;
  h.clock_frequency_hz = 1'000'000'000;
  h.start_ts = 1000;
  h.end_ts = 100'000;
  h.platform = kPlatformLinux;
  auto w = TraceWriter::open(path, h);
  ASSERT_TRUE(w.ok());
  ProcessRec proc{};
  proc.pid = 7;
  proc.start_ts = h.start_ts;
  proc.end_ts = h.end_ts;
  proc.name = w->internString("app");
  w->append(std::span<const ProcessRec>(&proc, 1));
  ThreadRec th{};
  th.tid = 70;
  th.pid = 7;
  th.start_ts = h.start_ts;
  th.end_ts = h.end_ts;
  th.name = w->internString("main");
  th.flags = kThreadFlagTargetProcess;
  w->append(std::span<const ThreadRec>(&th, 1));
  // The thread runs for the whole trace (samples weigh nothing outside a Running interval).
  ContextSwitchRec in{};
  in.ts = h.start_ts;
  in.new_tid = 70;
  w->append(std::span<const ContextSwitchRec>(&in, 1));
  const FrameIdx f = w->internFrame(ModuleIdx(), 0x1000);
  const FrameIdx stack[1] = {f};
  const CallstackIdx withStack = w->internCallstack(std::span<const FrameIdx>(stack, 1));
  SampleBatch batch;
  for (uint64_t i = 0; i < 10; ++i)
    batch.push(h.start_ts + i * 100, 70, i < 6 ? withStack : CallstackIdx(0), 0);
  w->append(batch);
  ASSERT_TRUE(w->finish().ok());
}

template <typename T>
T* panelOf(App& app, const char* name) {
  PanelEntry* e = app.panels().find(name);
  return e ? dynamic_cast<T*>(e->panel.get()) : nullptr;
}

}  // namespace

TEST(PolishButterfly, TheNoStackRowGivesATotalsOnlyButterfly) {
  TempDir dir("polish_nostack");
  const fs::path trace = dir.path() / "frameless.tachyon";
  writeFramelessTrace(trace);
  Harness h;
  ASSERT_TRUE(h.open(trace));
  ASSERT_TRUE(h.until([&] { return h.symbolsDone() || h.app.document()->symbols()->progress().total == 0; }));
  auto* list = panelOf<FunctionListPanel>(h.app, FunctionListPanel::kName);
  auto* butterfly = panelOf<ButterflyPanel>(h.app, ButterflyPanel::kName);
  ASSERT_NE(list, nullptr);
  ASSERT_NE(butterfly, nullptr);
  h.app.panels().find(FunctionListPanel::kName)->open = true;
  h.app.panels().find(ButterflyPanel::kName)->open = true;
  ASSERT_TRUE(h.until([&] { return list->table() != nullptr; }));

  const auto& entries = list->table()->entries;
  const auto row = std::find_if(entries.begin(), entries.end(),
                                [](const functions::Entry& e) { return e.key == analysis::kRootKey; });
  ASSERT_NE(row, entries.end()) << "the Function List has the <no stack> row";
  EXPECT_EQ(row->function, "<no stack>");
  EXPECT_GT(row->inclusiveTicks, 0u);

  h.app.selection().setSelectedFunction(analysis::kRootKey);
  ASSERT_TRUE(h.until([&] { return butterfly->data() != nullptr; }));
  h.frames(3);
  ASSERT_TRUE(butterfly->showsNoStackRow());
  const functions::ButterflyView& view = butterfly->data()->view;
  EXPECT_EQ(view.self.key, analysis::kRootKey);
  EXPECT_EQ(view.self.name.function, "<no stack>");
  EXPECT_EQ(view.self.inclusiveTicks, row->inclusiveTicks) << "the row's totals";
  EXPECT_EQ(view.self.exclusiveTicks, row->exclusiveTicks);
  EXPECT_EQ(view.self.instances, row->instances);
  EXPECT_TRUE(view.callers.empty());
  EXPECT_TRUE(view.callees.empty());
  EXPECT_FALSE(butterfly->inlineForced());
  EXPECT_FALSE(butterfly->kernelForced());
  EXPECT_NE(std::string(ButterflyPanel::kNoStackNote).find("no callers or callees"), std::string::npos);

  // A real function afterwards: no no-stack note.
  const auto fn = std::find_if(entries.begin(), entries.end(),
                               [](const functions::Entry& e) { return !(e.key == analysis::kRootKey); });
  ASSERT_NE(fn, entries.end());
  h.app.selection().setSelectedFunction(fn->key);
  ASSERT_TRUE(h.until([&] { return butterfly->data() && !butterfly->showsNoStackRow(); }));
}
