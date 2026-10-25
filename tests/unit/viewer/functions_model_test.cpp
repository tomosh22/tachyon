// WP-44: the pure models of the Function List and the Butterfly: table build, sorting of every column
// (directions, ties), the text filter, module grouping sums, display items, the filter mapping, option
// forcing, butterfly percentages / ordering / navigation.
#include "analysis/query.h"
#include "viewer/functions/butterfly_model.h"
#include "viewer/functions/function_list_model.h"

#include <gtest/gtest.h>

#include <map>
#include <numeric>

using namespace tachyon;
using namespace tachyon::viewer;
namespace fl = tachyon::viewer::functions;
using analysis::FunctionRow;
using symbols::FunctionKey;

namespace {

FunctionKey key(uint32_t module, uint64_t value) {
  return FunctionKey{ModuleIdx(module), value};
}

FunctionRow row(FunctionKey k, uint64_t incl, uint64_t excl, uint32_t instances) {
  FunctionRow r;
  r.key = k;
  r.inclusiveTicks = incl;
  r.exclusiveTicks = excl;
  r.instances = instances;
  return r;
}

// Names: module 0 = "Game.exe", 1 = "ntdll.dll", 2 = "render.dll"; function "fn<rva>" except named ones.
common::NameParts nameOf(const FunctionKey& k) {
  static const std::map<FunctionKey, std::string> kNamed = {
      {key(0, 0x10), "Engine::Update"}, {key(0, 0x20), "main"},          {key(1, 0x30), "RtlUserThreadStart"},
      {key(2, 0x40), "Render::Draw"},   {key(2, 0x50), "render::flush"}, {key(0, 0x60), "Zeta"}};
  common::NameParts p;
  if (k == analysis::kRootKey) {
    p.full = p.function = "<root>";
    return p;
  }
  if (k.isScope()) {
    p.full = p.function = "scope:" + std::to_string(k.value);
    return p;
  }
  static const char* const kModules[] = {"Game.exe", "ntdll.dll", "render.dll"};
  p.module = kModules[k.module.get()];
  const auto it = kNamed.find(k);
  p.function = it != kNamed.end() ? it->second : "fn" + std::to_string(k.value);
  p.full = p.module + "!" + p.function;
  return p;
}

std::shared_ptr<const fl::Table> sampleTable() {
  const std::vector<FunctionRow> rows = {
      row(key(1, 0x30), 1000, 0, 100),  // RtlUserThreadStart
      row(key(0, 0x20), 900, 50, 90),   // main
      row(key(0, 0x10), 600, 300, 60),  // Engine::Update
      row(key(2, 0x40), 300, 200, 30),  // Render::Draw
      row(key(2, 0x50), 100, 200, 30),  // render::flush: ties Render::Draw on exclusive and instances
      row(key(0, 0x60), 600, 150, 60),  // Zeta: ties Engine::Update on inclusive and instances
      row(FunctionKey::scope(StringIdx(7)), 40, 40, 4),
  };
  return fl::buildTable(rows, nameOf, 1e9);
}

std::vector<std::string> names(const fl::Table& t, const std::vector<uint32_t>& view) {
  std::vector<std::string> out;
  for (const uint32_t i : view) out.push_back(t.entries[i].function);
  return out;
}

}  // namespace

TEST(FunctionsTable, BuildCopiesRowsAndComputesTheTotal) {
  const auto t = sampleTable();
  ASSERT_EQ(t->entries.size(), 7u);
  EXPECT_EQ(t->totalTicks, 0u + 50 + 300 + 200 + 200 + 150 + 40);
  const fl::Entry& e = t->entries[2];
  EXPECT_EQ(e.key, key(0, 0x10));
  EXPECT_EQ(e.function, "Engine::Update");
  EXPECT_EQ(e.module, "Game.exe");
  EXPECT_EQ(e.full, "Game.exe!Engine::Update");
  EXPECT_EQ(fl::haystackOf(*t, e), "game.exe!engine::update");
  EXPECT_DOUBLE_EQ(fl::percentOfTotal(e.exclusiveTicks, t->totalTicks), 100.0 * 300 / t->totalTicks);
  EXPECT_DOUBLE_EQ(fl::percentOfTotal(5, 0), 0.0);
  EXPECT_EQ(fl::entryOfKey(*t, key(2, 0x40)), 3u);
  EXPECT_EQ(fl::entryOfKey(*t, key(2, 0x99)), SIZE_MAX);
}

TEST(FunctionsTable, EmptyInputGivesAnEmptyTable) {
  const auto t = fl::buildTable({}, nameOf, 1e9);
  EXPECT_TRUE(t->entries.empty());
  EXPECT_EQ(t->totalTicks, 0u);
  EXPECT_TRUE(fl::buildView(*t, "x", fl::SortSpec{}).empty());
  EXPECT_TRUE(fl::groupByModule(*t, {}, fl::SortSpec{}).empty());
}

TEST(FunctionsView, SortsEveryNumericColumnInBothDirectionsWithKeyTieBreak) {
  const auto t = sampleTable();
  // Inclusive descending: the two 600s tie, Engine::Update (key 0/0x10) before Zeta (0/0x60).
  EXPECT_EQ(names(*t, fl::buildView(*t, "", {fl::SortColumn::Inclusive, false})),
            (std::vector<std::string>{"RtlUserThreadStart", "main", "Engine::Update", "Zeta", "Render::Draw",
                                      "render::flush", "scope:7"}));
  // Ascending: the tie keeps the key order (direction does not flip the tie-break).
  EXPECT_EQ(names(*t, fl::buildView(*t, "", {fl::SortColumn::Inclusive, true})),
            (std::vector<std::string>{"scope:7", "render::flush", "Render::Draw", "Engine::Update", "Zeta",
                                      "main", "RtlUserThreadStart"}));
  // Exclusive descending: 300, 200 (two: Render::Draw module 2/0x40 before render::flush 2/0x50), 150, 50,
  // 40, 0.
  EXPECT_EQ(names(*t, fl::buildView(*t, "", {fl::SortColumn::Exclusive, false})),
            (std::vector<std::string>{"Engine::Update", "Render::Draw", "render::flush", "Zeta", "main",
                                      "scope:7", "RtlUserThreadStart"}));
  EXPECT_EQ(names(*t, fl::buildView(*t, "", {fl::SortColumn::Exclusive, true})),
            (std::vector<std::string>{"RtlUserThreadStart", "scope:7", "main", "Zeta", "Render::Draw",
                                      "render::flush", "Engine::Update"}));
  // Instances descending: 100, 90, 60 (Engine::Update, Zeta), 30 (Draw, flush), 4.
  EXPECT_EQ(names(*t, fl::buildView(*t, "", {fl::SortColumn::Instances, false})),
            (std::vector<std::string>{"RtlUserThreadStart", "main", "Engine::Update", "Zeta", "Render::Draw",
                                      "render::flush", "scope:7"}));
  EXPECT_EQ(names(*t, fl::buildView(*t, "", {fl::SortColumn::Instances, true})),
            (std::vector<std::string>{"scope:7", "Render::Draw", "render::flush", "Engine::Update", "Zeta",
                                      "main", "RtlUserThreadStart"}));
}

TEST(FunctionsView, SortsNamesCaseInsensitivelyAndModulesByName) {
  const auto t = sampleTable();
  // Case-insensitive: "Engine::Update" < "main" < "Render::Draw" < "render::flush" < "RtlUserThreadStart" <
  // "scope:7" < "Zeta" ("render::draw" < "render::flush").
  EXPECT_EQ(names(*t, fl::buildView(*t, "", {fl::SortColumn::Function, true})),
            (std::vector<std::string>{"Engine::Update", "main", "Render::Draw", "render::flush",
                                      "RtlUserThreadStart", "scope:7", "Zeta"}));
  EXPECT_EQ(names(*t, fl::buildView(*t, "", {fl::SortColumn::Function, false})),
            (std::vector<std::string>{"Zeta", "scope:7", "RtlUserThreadStart", "render::flush",
                                      "Render::Draw", "main", "Engine::Update"}));
  // Module ascending: rows without a module (the scope) first, then game.exe (key order), ntdll, render.
  EXPECT_EQ(names(*t, fl::buildView(*t, "", {fl::SortColumn::Module, true})),
            (std::vector<std::string>{"scope:7", "Engine::Update", "main", "Zeta", "RtlUserThreadStart",
                                      "Render::Draw", "render::flush"}));
  EXPECT_EQ(names(*t, fl::buildView(*t, "", {fl::SortColumn::Module, false})),
            (std::vector<std::string>{"Render::Draw", "render::flush", "RtlUserThreadStart", "Engine::Update",
                                      "main", "Zeta", "scope:7"}));
}

TEST(FunctionsView, DefaultDirectionsAndDeterminismForEqualKeysOfRows) {
  EXPECT_FALSE(fl::defaultAscending(fl::SortColumn::Inclusive));
  EXPECT_FALSE(fl::defaultAscending(fl::SortColumn::Exclusive));
  EXPECT_FALSE(fl::defaultAscending(fl::SortColumn::Instances));
  EXPECT_TRUE(fl::defaultAscending(fl::SortColumn::Function));
  EXPECT_TRUE(fl::defaultAscending(fl::SortColumn::Module));
  // Many rows with identical numbers: the order is the key order and does not depend on the input order.
  std::vector<FunctionRow> rows;
  for (uint64_t i = 0; i < 50; ++i) rows.push_back(row(key(0, 100 + i), 10, 10, 1));
  std::vector<FunctionRow> reversed(rows.rbegin(), rows.rend());
  const auto a = fl::buildTable(rows, nameOf, 1e9);
  const auto b = fl::buildTable(reversed, nameOf, 1e9);
  for (const auto column :
       {fl::SortColumn::Inclusive, fl::SortColumn::Exclusive, fl::SortColumn::Instances}) {
    for (const bool asc : {false, true}) {
      const auto va = fl::buildView(*a, "", {column, asc});
      const auto vb = fl::buildView(*b, "", {column, asc});
      ASSERT_EQ(va.size(), 50u);
      for (size_t i = 0; i < 50; ++i) {
        EXPECT_EQ(a->entries[va[i]].key, key(0, 100 + i));
        EXPECT_EQ(b->entries[vb[i]].key, key(0, 100 + i));
      }
    }
  }
}

TEST(FunctionsView, TextFilterIsACaseInsensitiveSubstringOverNameAndModule) {
  const auto t = sampleTable();
  const fl::SortSpec byName{fl::SortColumn::Function, true};
  const auto filtered = [&](std::string_view text) {
    return names(*t, fl::buildView(*t, fl::normalizeFilterText(text), byName));
  };
  EXPECT_EQ(filtered("render"), (std::vector<std::string>{"Render::Draw", "render::flush"}));
  EXPECT_EQ(filtered("RENDER::D"), (std::vector<std::string>{"Render::Draw"}));
  EXPECT_EQ(filtered("  update  "), (std::vector<std::string>{"Engine::Update"}));  // trimmed
  EXPECT_EQ(filtered("ntdll"), (std::vector<std::string>{"RtlUserThreadStart"}));   // by module
  EXPECT_EQ(filtered("game.exe!"), (std::vector<std::string>{"Engine::Update", "main", "Zeta"}));
  EXPECT_EQ(filtered("scope"), (std::vector<std::string>{"scope:7"}));
  EXPECT_TRUE(filtered("no such function").empty());
  EXPECT_EQ(filtered("").size(), 7u);
  EXPECT_EQ(filtered("   ").size(), 7u);
  EXPECT_EQ(fl::normalizeFilterText(" \tAbC "), "abc");
  // The filter and the sort combine: filtered rows are sorted by the chosen column.
  EXPECT_EQ(names(*t, fl::buildView(*t, "render", {fl::SortColumn::Exclusive, true})).size(), 2u);
}

TEST(FunctionsGroups, SumsAreOverTheVisibleMembersAndSortedLikeTheRows) {
  const auto t = sampleTable();
  const fl::SortSpec sort{fl::SortColumn::Exclusive, false};
  const auto view = fl::buildView(*t, "", sort);
  const auto groups = fl::groupByModule(*t, view, sort);
  ASSERT_EQ(groups.size(), 4u);
  // Exclusive sums: game.exe 50 + 300 + 150 = 500, render.dll 400, (no module) 40, ntdll 0.
  EXPECT_EQ(groups[0].module, "Game.exe");
  EXPECT_EQ(groups[0].exclusiveSum, 500u);
  EXPECT_EQ(groups[0].inclusiveSum, 900u + 600 + 600);
  EXPECT_EQ(groups[0].instancesSum, 90u + 60 + 60);
  EXPECT_EQ(groups[1].module, "render.dll");
  EXPECT_EQ(groups[1].exclusiveSum, 400u);
  EXPECT_EQ(groups[2].module, "(no module)");
  EXPECT_EQ(groups[2].exclusiveSum, 40u);
  EXPECT_EQ(groups[3].module, "ntdll.dll");
  EXPECT_EQ(groups[3].exclusiveSum, 0u);
  // Members keep the order of the view.
  EXPECT_EQ(names(*t, groups[0].members), (std::vector<std::string>{"Engine::Update", "Zeta", "main"}));
  // Every row of the view is in exactly one group.
  size_t members = 0;
  uint64_t exclusive = 0;
  for (const fl::Group& g : groups) {
    members += g.members.size();
    exclusive += g.exclusiveSum;
  }
  EXPECT_EQ(members, view.size());
  EXPECT_EQ(exclusive, t->totalTicks);

  // Ascending inclusive: smallest sum first.
  const auto byIncl = fl::groupByModule(*t, fl::buildView(*t, "", {fl::SortColumn::Inclusive, true}),
                                        {fl::SortColumn::Inclusive, true});
  EXPECT_EQ(byIncl.front().module, "(no module)");  // 40
  EXPECT_EQ(byIncl.back().module, "Game.exe");      // 2100
  // By name: module order, descending flips it.
  const auto byName = fl::groupByModule(*t, fl::buildView(*t, "", {fl::SortColumn::Module, true}),
                                        {fl::SortColumn::Module, true});
  ASSERT_EQ(byName.size(), 4u);
  EXPECT_EQ(byName[0].module, "(no module)");
  EXPECT_EQ(byName[1].module, "Game.exe");
  EXPECT_EQ(byName[2].module, "ntdll.dll");
  EXPECT_EQ(byName[3].module, "render.dll");
  const auto byNameDesc = fl::groupByModule(*t, fl::buildView(*t, "", {fl::SortColumn::Function, false}),
                                            {fl::SortColumn::Function, false});
  EXPECT_EQ(byNameDesc.front().module, "render.dll");
  // With a text filter only the survivors are summed.
  const auto filteredView = fl::buildView(*t, "render::", sort);
  const auto filteredGroups = fl::groupByModule(*t, filteredView, sort);
  ASSERT_EQ(filteredGroups.size(), 1u);
  EXPECT_EQ(filteredGroups[0].members.size(), 2u);
  EXPECT_EQ(filteredGroups[0].exclusiveSum, 400u);
}

TEST(FunctionsGroups, ItemsFlatGroupedAndCollapsed) {
  const auto t = sampleTable();
  const fl::SortSpec sort{fl::SortColumn::Exclusive, false};
  const auto view = fl::buildView(*t, "", sort);
  const auto flat = fl::flatItems(view);
  ASSERT_EQ(flat.size(), view.size());
  for (size_t i = 0; i < flat.size(); ++i) {
    EXPECT_FALSE(flat[i].header);
    EXPECT_EQ(flat[i].entry, view[i]);
  }
  const auto groups = fl::groupByModule(*t, view, sort);
  auto items = fl::groupedItems(groups, {});
  EXPECT_EQ(items.size(), groups.size() + view.size());
  EXPECT_TRUE(items[0].header);
  EXPECT_EQ(items[0].group, 0u);
  EXPECT_FALSE(items[1].header);
  EXPECT_EQ(items[1].entry, groups[0].members[0]);
  // Collapsing a module hides its members but keeps the header.
  items = fl::groupedItems(groups, {"Game.exe"});
  EXPECT_EQ(items.size(), groups.size() + view.size() - 3);
  EXPECT_TRUE(items[0].header);
  EXPECT_TRUE(items[1].header);
  EXPECT_EQ(fl::itemOfEntry(items, groups[0].members[0]), SIZE_MAX);
  EXPECT_NE(fl::itemOfEntry(items, groups[1].members[0]), SIZE_MAX);
}

TEST(FunctionsFilter, SubtreeToggleMapsTheBusFilter) {
  ViewFilter bus;
  bus.t0 = 100;
  bus.t1 = 900;
  bus.threads = std::vector<ThreadIdx>{ThreadIdx(2), ThreadIdx(5)};
  bus.subtreeRoot = CallstackIdx(77);

  analysis::Filter on = fl::makeFilter(bus, true);
  EXPECT_EQ(on.t0, 100u);
  EXPECT_EQ(on.t1, 900u);
  ASSERT_TRUE(on.threads);
  EXPECT_EQ(on.threads->size(), 2u);
  ASSERT_TRUE(on.subtreeRoot);
  EXPECT_EQ(on.subtreeRoot->get(), 77u);
  EXPECT_FALSE(on.function);

  analysis::Filter off = fl::makeFilter(bus, false);
  EXPECT_EQ(off.t0, 100u);
  EXPECT_EQ(off.t1, 900u);
  EXPECT_EQ(off.threads, on.threads);
  EXPECT_FALSE(off.subtreeRoot);  // cleared: the whole call graph
  EXPECT_NE(on, off);

  // No bus filter: the whole trace either way.
  EXPECT_EQ(fl::makeFilter(std::nullopt, true), analysis::Filter{});
  EXPECT_EQ(fl::makeFilter(std::nullopt, false), analysis::Filter{});
  // A bus filter without a subtree maps the same with both toggle positions.
  bus.subtreeRoot.reset();
  EXPECT_EQ(fl::makeFilter(bus, true), fl::makeFilter(bus, false));
}

TEST(FunctionsFilter, SelectionForcesTheOptionsItsKeyNeeds) {
  const auto plain = fl::effectiveOptions(false, analysis::KernelFrames::Show, std::nullopt);
  EXPECT_FALSE(plain.expandInline);
  EXPECT_EQ(plain.kernelFrames, analysis::KernelFrames::Show);
  const FunctionKey inl{ModuleIdx(0), symbols::kInlineKeyBit | 5};
  EXPECT_TRUE(fl::effectiveOptions(false, analysis::KernelFrames::Show, inl).expandInline);
  EXPECT_FALSE(fl::effectiveOptions(false, analysis::KernelFrames::Show, key(0, 0x10)).expandInline);
  EXPECT_EQ(fl::effectiveOptions(false, analysis::KernelFrames::Show, analysis::kKernelKey).kernelFrames,
            analysis::KernelFrames::Group);
  EXPECT_EQ(fl::effectiveOptions(false, analysis::KernelFrames::Hide, analysis::kKernelKey).kernelFrames,
            analysis::KernelFrames::Hide);
  EXPECT_TRUE(fl::effectiveOptions(true, analysis::KernelFrames::Show, key(0, 1)).expandInline);
}

TEST(FunctionsButterfly, PercentagesAreRelativeToTheFunctionsInclusiveAndSortedByInclusive) {
  analysis::Butterfly bf;
  bf.self = row(key(0, 0x20), 800, 100, 80);
  // Unsorted on purpose; two callers tie (400): key order decides.
  bf.callers = {row(key(0, 0x60), 100, 10, 10), row(key(1, 0x30), 400, 50, 40),
                row(key(0, 0x10), 400, 40, 30), row(analysis::kRootKey, 0, 0, 0)};
  bf.callees = {row(key(2, 0x50), 100, 100, 10), row(key(2, 0x40), 600, 200, 60)};
  const fl::ButterflyView v = fl::buildButterflyView(bf, nameOf);

  EXPECT_EQ(v.self.name.full, "Game.exe!main");
  EXPECT_DOUBLE_EQ(v.self.inclusivePercent, 100.0);
  EXPECT_DOUBLE_EQ(v.self.exclusivePercent, 12.5);
  ASSERT_EQ(v.callers.size(), 4u);
  EXPECT_EQ(v.callers[0].key, key(0, 0x10));  // 400, key 0/0x10 before 1/0x30
  EXPECT_EQ(v.callers[1].key, key(1, 0x30));
  EXPECT_EQ(v.callers[2].key, key(0, 0x60));
  EXPECT_EQ(v.callers[3].key, analysis::kRootKey);
  EXPECT_DOUBLE_EQ(v.callers[0].inclusivePercent, 50.0);
  EXPECT_DOUBLE_EQ(v.callers[0].exclusivePercent, 5.0);
  EXPECT_DOUBLE_EQ(v.callers[2].inclusivePercent, 12.5);
  ASSERT_EQ(v.callees.size(), 2u);
  EXPECT_EQ(v.callees[0].key, key(2, 0x40));
  EXPECT_DOUBLE_EQ(v.callees[0].inclusivePercent, 75.0);
  EXPECT_DOUBLE_EQ(v.callees[0].exclusivePercent, 25.0);
  EXPECT_DOUBLE_EQ(v.callees[1].inclusivePercent, 12.5);
  EXPECT_EQ(v.callees[1].name.function, "render::flush");
  EXPECT_EQ(v.callees[1].name.module, "render.dll");
  EXPECT_FALSE(v.empty());
}

TEST(FunctionsButterfly, RootCallerIsNotNavigableAndEmptyViewsAreRecognised) {
  analysis::Butterfly bf;
  bf.self = row(key(0, 0x20), 100, 100, 10);
  bf.callers = {row(analysis::kRootKey, 100, 100, 10)};
  const fl::ButterflyView v = fl::buildButterflyView(bf, nameOf);
  ASSERT_EQ(v.callers.size(), 1u);
  EXPECT_EQ(v.callers[0].name.function, "<root>");
  EXPECT_FALSE(v.callers[0].navigable);
  EXPECT_FALSE(fl::navigationTarget(analysis::kRootKey));
  EXPECT_EQ(fl::navigationTarget(key(0, 0x20)), key(0, 0x20));
  EXPECT_TRUE(v.self.navigable);
  EXPECT_TRUE(v.callees.empty());
  EXPECT_DOUBLE_EQ(v.callers[0].inclusivePercent, 100.0);

  const fl::ButterflyView none = fl::buildButterflyView(analysis::Butterfly{}, nameOf);
  EXPECT_TRUE(none.empty());
  EXPECT_DOUBLE_EQ(none.self.inclusivePercent, 0.0);  // no division by zero
}
