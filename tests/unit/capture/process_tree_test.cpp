// Child-process following rules (WP-13): the tracker over hand-built Process events plus the filter helpers.
#include "capture/windows_etw/process_tree.h"

#include "../etlconvert/test_util.h"

#include <gtest/gtest.h>

namespace tachyon::capture::etw {
namespace {

using etlconvert::opcodes::kProcessDcEnd;
using etlconvert::opcodes::kProcessEnd;
using etlconvert::opcodes::kProcessStart;
namespace tu = etlconvert::testing;

// Feeds one Process event through the tracker the way the ETL scan does.
bool feed(ProcessTreeTracker& t, uint8_t opcode, uint32_t pid, uint32_t ppid, const std::string& name,
          const std::string& cmd = {}, uint64_t ts = 1000) {
  const std::u16string wide(cmd.begin(), cmd.end());
  const tu::ByteVec payload = tu::processPayload(pid, ppid, name, wide);
  const etlconvert::EtwEvent e = tu::makeEvent(etlconvert::guids::kProcess, opcode, 3, ts, 4, 4, 0, payload);
  return t.onEvent(e);
}

TEST(ExecutablePathOf, QuotedUnquotedAndEmpty) {
  EXPECT_EQ(executablePathOf("\"C:\\Program Files\\game.exe\" --x"), "C:\\Program Files\\game.exe");
  EXPECT_EQ(executablePathOf("C:\\bin\\game.exe --x"), "C:\\bin\\game.exe");
  EXPECT_EQ(executablePathOf("   game.exe"), "game.exe");
  EXPECT_EQ(executablePathOf(""), "");
  EXPECT_EQ(executablePathOf("\"unterminated path"), "unterminated path");
}

TEST(BaseNameOf, BothSeparators) {
  EXPECT_EQ(baseNameOf("C:\\a\\b.exe"), "b.exe");
  EXPECT_EQ(baseNameOf("/usr/bin/b"), "b");
  EXPECT_EQ(baseNameOf("b.exe"), "b.exe");
  EXPECT_EQ(baseNameOf("dir\\"), "");
}

TEST(MatchesChildFilter, ImageNameCommandLineAndPaths) {
  EXPECT_TRUE(matchesChildFilter("", "anything.exe", "", true)) << "empty filter accepts everything";
  EXPECT_TRUE(matchesChildFilter("*.exe", "worker.exe", "", true));
  EXPECT_FALSE(matchesChildFilter("*.dll", "worker.exe", "", true));
  EXPECT_TRUE(matchesChildFilter("WORKER.EXE", "worker.exe", "", true));
  EXPECT_FALSE(matchesChildFilter("WORKER.EXE", "worker.exe", "", false));
  // The image name may be missing or truncated; the command line's executable is the second chance.
  EXPECT_TRUE(matchesChildFilter("helper-tool.exe", "helper-tool.e", "\"C:\\x\\helper-tool.exe\" -a", true));
  EXPECT_TRUE(matchesChildFilter("helper*", "", "C:\\x\\helper-tool.exe", true));
  // A filter with a separator matches the full path of the command line.
  EXPECT_TRUE(matchesChildFilter("C:\\x\\*.exe", "a.exe", "C:\\x\\a.exe", true));
  EXPECT_FALSE(matchesChildFilter("C:\\y\\*.exe", "a.exe", "C:\\x\\a.exe", true));
  EXPECT_FALSE(matchesChildFilter("C:\\x\\*.exe", "a.exe", "", true));
}

TEST(ProcessTree, RootsAreTargetsWithoutFollowing) {
  ProcessTreeTracker t({100}, false, "");
  EXPECT_FALSE(feed(t, kProcessStart, 101, 100, "child.exe"));
  EXPECT_EQ(t.targets(), (std::vector<uint32_t>{100}));
}

TEST(ProcessTree, ChildOfTargetIsFollowedWhenFilterMatches) {
  ProcessTreeTracker t({100}, true, "*.exe");
  EXPECT_TRUE(feed(t, kProcessStart, 101, 100, "child.exe", "\"child.exe\""));
  EXPECT_FALSE(feed(t, kProcessStart, 102, 100, "data.bin"));   // rejected by the filter
  EXPECT_FALSE(feed(t, kProcessStart, 103, 999, "other.exe"));  // parent is not a target
  EXPECT_EQ(t.targets(), (std::vector<uint32_t>{100, 101}));
  ASSERT_EQ(t.children().size(), 1u);
  EXPECT_EQ(t.children()[0].parentPid, 100u);
  EXPECT_EQ(t.children()[0].imageName, "child.exe");
  EXPECT_EQ(t.rejectedChildren(), 1u);
}

TEST(ProcessTree, EmptyFilterFollowsEveryChild) {
  ProcessTreeTracker t({100}, true, "");
  EXPECT_TRUE(feed(t, kProcessStart, 101, 100, "a.exe"));
  EXPECT_TRUE(feed(t, kProcessStart, 102, 100, "b.bin"));
  EXPECT_EQ(t.targets().size(), 3u);
}

TEST(ProcessTree, GrandchildrenFollowAcceptedChildrenOnly) {
  ProcessTreeTracker t({100}, true, "*.exe");
  EXPECT_TRUE(feed(t, kProcessStart, 101, 100, "child.exe"));
  EXPECT_TRUE(feed(t, kProcessStart, 102, 101, "grandchild.exe"));
  EXPECT_FALSE(feed(t, kProcessStart, 103, 100, "skipped.tmp"));
  EXPECT_FALSE(feed(t, kProcessStart, 104, 103, "under-skipped.exe")) << "a rejected child is not traversed";
  EXPECT_EQ(t.targets(), (std::vector<uint32_t>{100, 101, 102}));
}

// True when `pid` is a target at `ts` according to the intervals (the converter's rule).
bool targetAt(const ProcessTreeTracker& t, uint32_t pid, uint64_t ts) {
  for (const auto& iv : t.intervals())
    if (iv.pid == pid && ts >= iv.start && ts <= iv.end) return true;
  return false;
}

TEST(ProcessTree, EndedParentStopsBeingAParentSoReusedPidsAreFresh) {
  ProcessTreeTracker t({100}, true, "*");
  EXPECT_TRUE(feed(t, kProcessStart, 101, 100, "child.exe", {}, 2000));
  feed(t, kProcessEnd, 101, 100, "child.exe", {}, 3000);
  // pid 101 is reused by an unrelated process, which then spawns a child: not ours.
  EXPECT_FALSE(feed(t, kProcessStart, 101, 5, "unrelated.exe", {}, 4000));
  EXPECT_FALSE(feed(t, kProcessStart, 150, 101, "its-child.exe", {}, 4500));
  EXPECT_EQ(t.targets(), (std::vector<uint32_t>{100, 101}));
  // The samples of the reused pid are excluded: only the first lifetime of 101 is a target interval.
  EXPECT_FALSE(targetAt(t, 101, 1999));
  EXPECT_TRUE(targetAt(t, 101, 2000));
  EXPECT_TRUE(targetAt(t, 101, 3000));
  EXPECT_FALSE(targetAt(t, 101, 4001)) << "a sample of the unrelated process that reuses pid 101";
  EXPECT_FALSE(targetAt(t, 150, 4600));
}

TEST(ProcessTree, RootIsATargetFromTheStartUntilItEnds) {
  ProcessTreeTracker t({100}, false, "");
  EXPECT_TRUE(targetAt(t, 100, 0));
  EXPECT_TRUE(targetAt(t, 100, 123456));
  feed(t, kProcessEnd, 100, 1, "root.exe", {}, 5000);
  EXPECT_TRUE(targetAt(t, 100, 5000));
  EXPECT_FALSE(targetAt(t, 100, 5001)) << "an unrelated process reusing the root's pid";
  EXPECT_FALSE(feed(t, kProcessStart, 100, 9, "reuse.exe", {}, 6000)) << "and it does not reopen the root";
  EXPECT_FALSE(targetAt(t, 100, 6500));
}

TEST(ProcessTree, ReusedPidAcceptedTwiceHasTwoIntervals) {
  ProcessTreeTracker t({100}, true, "*");
  EXPECT_TRUE(feed(t, kProcessStart, 101, 100, "a.exe", {}, 1000));
  feed(t, kProcessEnd, 101, 100, "a.exe", {}, 2000);
  EXPECT_TRUE(feed(t, kProcessStart, 101, 100, "b.exe", {}, 3000));
  EXPECT_TRUE(targetAt(t, 101, 1500));
  EXPECT_FALSE(targetAt(t, 101, 2500));
  EXPECT_TRUE(targetAt(t, 101, 3500));
  EXPECT_EQ(t.targets(), (std::vector<uint32_t>{100, 101}));
}

TEST(ProcessTree, MissedEndIsClosedWhenTheSamePidStartsAgain) {
  ProcessTreeTracker t({100}, true, "*.exe");
  EXPECT_TRUE(feed(t, kProcessStart, 101, 100, "a.exe", {}, 1000));
  EXPECT_FALSE(feed(t, kProcessStart, 101, 7, "x.exe", {}, 2000)) << "unrelated start, End was never seen";
  EXPECT_TRUE(targetAt(t, 101, 1500));
  EXPECT_FALSE(targetAt(t, 101, 2500));
}

TEST(ProcessTree, CircularSegmentsKeepChildrenAcrossFlushes) {
  // One tracker sees every segment. Segment 1 starts the child; segment 2 (a fresh session) only has the
  // DCStart rundown of the still-running processes.
  ProcessTreeTracker t({100}, true, "*");
  EXPECT_TRUE(feed(t, kProcessStart, 101, 100, "child.exe", {}, 1000));
  EXPECT_FALSE(feed(t, etlconvert::opcodes::kProcessDcStart, 101, 100, "child.exe", {}, 9000))
      << "already tracked: not accepted twice";
  EXPECT_TRUE(feed(t, etlconvert::opcodes::kProcessDcStart, 102, 101, "grand.exe", {}, 9000));
  EXPECT_TRUE(feed(t, kProcessStart, 103, 101, "later.exe", {}, 9500)) << "the parent survived the flush";
  EXPECT_EQ(t.targets(), (std::vector<uint32_t>{100, 101, 102, 103}));
  EXPECT_TRUE(targetAt(t, 102, 0)) << "a rundown record was already running when the segment began";
  EXPECT_FALSE(feed(t, etlconvert::opcodes::kProcessDcStart, 200, 999, "stranger.exe", {}, 9000));
}

TEST(ProcessTree, DcStartRespectsTheFilterAndTheLiveParent) {
  ProcessTreeTracker t({100}, true, "keep*");
  EXPECT_FALSE(feed(t, etlconvert::opcodes::kProcessDcStart, 101, 100, "drop.exe"));
  EXPECT_TRUE(feed(t, etlconvert::opcodes::kProcessDcStart, 102, 100, "keep.exe"));
  EXPECT_FALSE(feed(t, etlconvert::opcodes::kProcessDcStart, 103, 101, "keep2.exe")) << "parent was rejected";
}

TEST(ProcessTree, RootsOwnStartEventDoesNotUnseatIt) {
  // Launch mode: the session is running when the root is created, so its Start event is in the trace.
  ProcessTreeTracker t({100}, true, "*");
  EXPECT_FALSE(feed(t, kProcessStart, 100, 7, "root.exe"));
  EXPECT_TRUE(feed(t, kProcessStart, 101, 100, "child.exe"));
  EXPECT_EQ(t.targets(), (std::vector<uint32_t>{100, 101}));
}

TEST(ProcessTree, EndRundownIsIgnored) {
  ProcessTreeTracker t({100}, true, "*");
  EXPECT_FALSE(feed(t, kProcessDcEnd, 101, 100, "old-child.exe"));
  EXPECT_EQ(t.targets(), (std::vector<uint32_t>{100}));
}

TEST(ProcessTree, NonProcessEventsAndMalformedPayloadsAreIgnored) {
  ProcessTreeTracker t({100}, true, "*");
  const tu::ByteVec junk = {std::byte{1}, std::byte{2}};
  EXPECT_FALSE(t.onEvent(tu::makeEvent(etlconvert::guids::kProcess, kProcessStart, 3, 1, 4, 4, 0, junk)));
  EXPECT_FALSE(t.onEvent(tu::makeEvent(etlconvert::guids::kThread, kProcessStart, 3, 1, 4, 4, 0,
                                       tu::processPayload(101, 100, "x.exe"))));
  EXPECT_EQ(t.targets(), (std::vector<uint32_t>{100}));
}

TEST(ProcessTree, DuplicateRootsAndZeroAreDropped) {
  ProcessTreeTracker t({100, 100, 0, 200}, false, "");
  EXPECT_EQ(t.targets(), (std::vector<uint32_t>{100, 200}));
}

TEST(ProcessTree, CaseSensitiveModeForPosix) {
  ProcessTreeTracker t({100}, true, "Child", false);
  EXPECT_FALSE(feed(t, kProcessStart, 101, 100, "child"));
  EXPECT_TRUE(feed(t, kProcessStart, 102, 100, "Child"));
}

}  // namespace
}  // namespace tachyon::capture::etw
