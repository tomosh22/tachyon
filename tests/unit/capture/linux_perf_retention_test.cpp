// WP-66: the retention rule of a circular perf capture (libs/capture/linux_perf/retention.h).
#include "capture/linux_perf/retention.h"

#include <gtest/gtest.h>

#include <string>

namespace tachyon::capture::perf {
namespace {

constexpr uint64_t kSec = 1'000'000'000ull;

SealedSegment seg(const std::string& name, uint64_t firstTs, uint64_t lastTs) {
  SealedSegment s;
  s.path = name;
  s.firstTs = firstTs;
  s.lastTs = lastTs;
  s.bytes = 100;
  return s;
}

std::vector<std::string> names(const std::vector<SealedSegment>& v) {
  std::vector<std::string> out;
  for (const auto& s : v) out.push_back(s.path.string());
  return out;
}

using Names = std::vector<std::string>;

TEST(Retention, TimedSegmentsAreReleasedOnlyWhenOlderThanTheHorizon) {
  const std::vector<SealedSegment> sealed = {seg("a", 10 * kSec, 20 * kSec), seg("b", 20 * kSec, 40 * kSec),
                                             seg("c", 40 * kSec, 58 * kSec)};
  // flush at t=60 s with a 15 s horizon: cutoff 45 s -> a (20 s) and b (40 s) are older, c is not.
  EXPECT_EQ(names(selectReleasable(sealed, 60 * kSec, 15 * kSec)), (Names{"a", "b"}));
  // Exactly at the cutoff is not older.
  EXPECT_EQ(names(selectReleasable(sealed, 60 * kSec, 20 * kSec)), (Names{"a"}))
      << "b ends at 40 s == cutoff";
  EXPECT_TRUE(selectReleasable(sealed, 60 * kSec, 0).empty()) << "a window of 0 keeps everything";
  EXPECT_TRUE(selectReleasable(sealed, 5 * kSec, 10 * kSec).empty()) << "the horizon reaches before time 0";
}

TEST(Retention, SegmentsWithoutTimestampsAreReleasedBySequence) {
  // z1 and z2 have no timestamps and precede segment b, which is itself past the horizon: they were never
  // released before (lastTs == 0 was "unknown, keep").
  const std::vector<SealedSegment> sealed = {seg("a", 1 * kSec, 5 * kSec), seg("z1", 0, 0),
                                             seg("b", 6 * kSec, 9 * kSec), seg("z2", 0, 0),
                                             seg("c", 50 * kSec, 58 * kSec)};
  EXPECT_EQ(names(selectReleasable(sealed, 60 * kSec, 20 * kSec)), (Names{"a", "z1", "b"}))
      << "z1 is older than b by sequence; z2 lies after b and stays until a later segment is released";
  // An untimed segment after the newest one past the horizon stays.
  const std::vector<SealedSegment> tail = {seg("a", 1 * kSec, 5 * kSec), seg("z", 0, 0),
                                           seg("c", 50 * kSec, 58 * kSec)};
  EXPECT_EQ(names(selectReleasable(tail, 60 * kSec, 20 * kSec)), (Names{"a"}));
  // Nothing past the horizon at all: untimed segments before timed ones are kept (they may be inside it).
  EXPECT_TRUE(selectReleasable(tail, 60 * kSec, 59 * kSec).empty());
}

TEST(Retention, WhenNothingHasATimestampOnlyTheNewestSegmentIsKept) {
  const std::vector<SealedSegment> sealed = {seg("z1", 0, 0), seg("z2", 0, 0), seg("z3", 0, 0)};
  EXPECT_EQ(names(selectReleasable(sealed, 60 * kSec, 10 * kSec)), (Names{"z1", "z2"}));
  EXPECT_TRUE(selectReleasable({seg("only", 0, 0)}, 60 * kSec, 10 * kSec).empty());
  EXPECT_TRUE(selectReleasable({}, 60 * kSec, 10 * kSec).empty());
  EXPECT_TRUE(selectReleasable(sealed, 60 * kSec, 0).empty());
}

TEST(Retention, HorizonIsTheLargestRecentWindow) {
  RetentionHorizon h(300 * kSec);
  EXPECT_EQ(h.note(100 * kSec, 10), 10 * kSec);
  EXPECT_EQ(h.note(110 * kSec, 60), 60 * kSec) << "a longer window raises it";
  EXPECT_EQ(h.note(120 * kSec, 10), 60 * kSec) << "and a shorter one keeps it for the next flush";
  EXPECT_EQ(h.note(130 * kSec, 0), 0u) << "seconds == 0 releases nothing";
  EXPECT_EQ(h.note(140 * kSec, 5), 60 * kSec) << "and did not disturb the horizon";
}

TEST(Retention, OneHugeWindowDoesNotRaiseTheHorizonForever) {
  RetentionHorizon h(300 * kSec);
  EXPECT_EQ(h.note(100 * kSec, 3600), 3600 * kSec);
  EXPECT_EQ(h.note(200 * kSec, 10), 3600 * kSec) << "still remembered 100 s later";
  EXPECT_EQ(h.note(400 * kSec, 10), 3600 * kSec) << "exactly the memory: still held";
  // 301 s after the huge flush the memory has run out.
  EXPECT_EQ(h.note(401 * kSec, 10), 10 * kSec) << "the horizon came back down";
  EXPECT_EQ(h.note(402 * kSec, 20), 20 * kSec);
  h.reset();
  EXPECT_EQ(h.note(500 * kSec, 7), 7 * kSec) << "a new capture starts afresh";
  RetentionHorizon backwards(300 * kSec);
  EXPECT_EQ(backwards.note(1000 * kSec, 50), 50 * kSec);
  EXPECT_EQ(backwards.note(10 * kSec, 5), 5 * kSec) << "a clock that jumped back forgets, never overflows";
}

}  // namespace
}  // namespace tachyon::capture::perf
