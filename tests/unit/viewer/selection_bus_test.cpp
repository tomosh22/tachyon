#include "viewer/selection_bus.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace tachyon;
using namespace tachyon::viewer;

TEST(SelectionBus, PublishesToSubscribersInOrder) {
  SelectionBus bus;
  std::vector<std::string> log;
  bus.subscribe([&](const SelectionState&, SelectionField) { log.push_back("a"); });
  bus.subscribe([&](const SelectionState&, SelectionField) { log.push_back("b"); });
  bus.subscribe([&](const SelectionState&, SelectionField) { log.push_back("c"); });
  bus.setSelectedThread(ThreadIdx(3));
  EXPECT_EQ(log, (std::vector<std::string>{"a", "b", "c"}));
}

TEST(SelectionBus, CarriesStateAndField) {
  SelectionBus bus;
  SelectionField seenField = SelectionField::All;
  ThreadIdx seenThread;
  bus.subscribe([&](const SelectionState& s, SelectionField f) {
    seenField = f;
    seenThread = s.selectedThread;
  });
  bus.setSelectedThread(ThreadIdx(7));
  EXPECT_EQ(seenField, SelectionField::SelectedThread);
  EXPECT_EQ(seenThread, ThreadIdx(7));

  ViewFilter filter;
  filter.t0 = 10;
  filter.t1 = 20;
  filter.threads = std::vector<ThreadIdx>{ThreadIdx(1)};
  bus.setTimeFilter(filter);
  EXPECT_EQ(seenField, SelectionField::TimeFilter);
  ASSERT_TRUE(bus.state().timeFilter.has_value());
  EXPECT_EQ(*bus.state().timeFilter, filter);

  bus.setSelectedFunction(FunctionKey{ModuleIdx(2), 0x1000});
  EXPECT_EQ(seenField, SelectionField::SelectedFunction);
  EXPECT_FALSE(bus.state().selectedFunction->isScope());
  bus.setSelectedFunction(FunctionKey{ModuleIdx(FunctionKey::kScopeModule), 5});
  EXPECT_TRUE(bus.state().selectedFunction->isScope());

  HoveredItem hover;
  hover.kind = HoveredItem::Kind::Thread;
  hover.thread = ThreadIdx(4);
  bus.setHovered(hover);
  EXPECT_EQ(seenField, SelectionField::Hovered);
}

TEST(SelectionBus, UnchangedValueDoesNotPublish) {
  SelectionBus bus;
  int calls = 0;
  bus.subscribe([&](const SelectionState&, SelectionField) { ++calls; });
  bus.setSelectedThread(ThreadIdx(1));
  bus.setSelectedThread(ThreadIdx(1));
  EXPECT_EQ(calls, 1);
}

TEST(SelectionBus, UnsubscribeStopsDelivery) {
  SelectionBus bus;
  int a = 0, b = 0;
  const auto idA = bus.subscribe([&](const SelectionState&, SelectionField) { ++a; });
  bus.subscribe([&](const SelectionState&, SelectionField) { ++b; });
  bus.setSelectedThread(ThreadIdx(1));
  bus.unsubscribe(idA);
  bus.unsubscribe(idA);  // unknown ids are ignored
  bus.setSelectedThread(ThreadIdx(2));
  EXPECT_EQ(a, 1);
  EXPECT_EQ(b, 2);
  EXPECT_EQ(bus.subscriberCount(), 1u);
}

TEST(SelectionBus, UnsubscribeSelfDuringDispatch) {
  SelectionBus bus;
  int self = 0, other = 0;
  SelectionBus::SubscriptionId selfId = 0;
  selfId = bus.subscribe([&](const SelectionState&, SelectionField) {
    ++self;
    bus.unsubscribe(selfId);
  });
  bus.subscribe([&](const SelectionState&, SelectionField) { ++other; });
  bus.setSelectedThread(ThreadIdx(1));
  bus.setSelectedThread(ThreadIdx(2));
  EXPECT_EQ(self, 1);
  EXPECT_EQ(other, 2);
}

TEST(SelectionBus, UnsubscribeLaterSubscriberDuringDispatchSkipsIt) {
  SelectionBus bus;
  int victim = 0, last = 0;
  SelectionBus::SubscriptionId victimId = 0;
  bus.subscribe([&](const SelectionState&, SelectionField) { bus.unsubscribe(victimId); });
  victimId = bus.subscribe([&](const SelectionState&, SelectionField) { ++victim; });
  bus.subscribe([&](const SelectionState&, SelectionField) { ++last; });
  bus.setSelectedThread(ThreadIdx(1));
  EXPECT_EQ(victim, 0);
  EXPECT_EQ(last, 1);
}

TEST(SelectionBus, SubscribeDuringDispatchHearsNextPublishOnly) {
  SelectionBus bus;
  int added = 0;
  bool done = false;
  bus.subscribe([&](const SelectionState&, SelectionField) {
    if (done) return;
    done = true;
    bus.subscribe([&](const SelectionState&, SelectionField) { ++added; });
  });
  bus.setSelectedThread(ThreadIdx(1));
  EXPECT_EQ(added, 0);
  bus.setSelectedThread(ThreadIdx(2));
  EXPECT_EQ(added, 1);
}

TEST(SelectionBus, ClearResetsAndPublishesOnce) {
  SelectionBus bus;
  bus.setSelectedThread(ThreadIdx(1));
  int calls = 0;
  SelectionField field = SelectionField::Hovered;
  bus.subscribe([&](const SelectionState&, SelectionField f) {
    ++calls;
    field = f;
  });
  bus.clear();
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(field, SelectionField::All);
  EXPECT_FALSE(bus.state().selectedThread.valid());
}
