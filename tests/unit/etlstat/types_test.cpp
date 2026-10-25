#include <gtest/gtest.h>

#include "etw_types.h"
#include "wait_reason.h"

using namespace tachyon::etlstat;

TEST(EtwTypes, ParsesAndFormatsGuidRoundTrip) {
  const auto g = parseGuid("def2fe46-7bd6-4b80-bd94-f57fe20d0ce3");
  ASSERT_TRUE(g.has_value());
  EXPECT_EQ(*g, guids::kStackWalk);
  EXPECT_EQ(guidToString(*g), "def2fe46-7bd6-4b80-bd94-f57fe20d0ce3");
  EXPECT_EQ(parseGuid("{DEF2FE46-7BD6-4B80-BD94-F57FE20D0CE3}"), g);
}

TEST(EtwTypes, RejectsMalformedGuids) {
  EXPECT_FALSE(parseGuid("").has_value());
  EXPECT_FALSE(parseGuid("def2fe467bd64b80bd94f57fe20d0ce3").has_value());
  EXPECT_FALSE(parseGuid("zzf2fe46-7bd6-4b80-bd94-f57fe20d0ce3").has_value());
  EXPECT_FALSE(parseGuid("def2fe46-7bd6-4b80-bd94-f57fe20d0ce").has_value());
}

TEST(EtwTypes, KernelGuidsMatchAppendixB) {
  EXPECT_EQ(guidToString(guids::kProcess), "3d6fa8d0-fe05-11d0-9dda-00c04fd7ba7c");
  EXPECT_EQ(guidToString(guids::kThread), "3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7c");
  EXPECT_EQ(guidToString(guids::kImage), "2cb15d1d-5fc1-11d2-abe1-00a0c911f518");
  EXPECT_EQ(guidToString(guids::kPerfInfo), "ce1dbfb4-137e-4da6-87b0-3f59aa102cbc");
  EXPECT_EQ(guidToString(guids::kStackWalk), "def2fe46-7bd6-4b80-bd94-f57fe20d0ce3");
  EXPECT_EQ(guidToString(guids::kEventTrace), "68fdd900-4a3e-11d1-84f4-0000f80464e3");
}

TEST(EtwTypes, NamesProvidersAndOpcodes) {
  EXPECT_EQ(providerName(guids::kPerfInfo), "PerfInfo");
  EXPECT_EQ(providerName(Guid{}), "00000000-0000-0000-0000-000000000000");
  EXPECT_EQ(opcodeName(guids::kPerfInfo, 46), "SampledProfile");
  EXPECT_EQ(opcodeName(guids::kThread, 36), "CSwitch");
  EXPECT_EQ(opcodeName(guids::kThread, 50), "ReadyThread");
  EXPECT_EQ(opcodeName(guids::kThread, 72), "SetName");
  EXPECT_EQ(opcodeName(guids::kImage, 10), "Load");
  EXPECT_EQ(opcodeName(guids::kImage, 2), "Unload");
  EXPECT_EQ(opcodeName(guids::kStackWalk, 32), "Stack");
  EXPECT_EQ(opcodeName(guids::kStackWalk, 37), "StackKeyDelete");
  EXPECT_TRUE(opcodeName(guids::kPerfInfo, 1).empty());
}

TEST(WaitReason, NamesAndCategories) {
  EXPECT_EQ(waitReasonName(0), "Executive");
  EXPECT_EQ(waitReasonName(15), "WrQueue");
  EXPECT_EQ(waitReasonName(16), "WrLpcReceive");
  EXPECT_EQ(waitReasonName(32), "WrPreempted");
  EXPECT_EQ(waitReasonName(100), "Unknown");
  EXPECT_EQ(waitReasonName(-1), "Unknown");
  EXPECT_EQ(waitCategory(0), WaitCategory::Synchronization);
  EXPECT_EQ(waitCategory(15), WaitCategory::Synchronization);
  EXPECT_EQ(waitCategory(16), WaitCategory::Io);
  EXPECT_EQ(waitCategory(2), WaitCategory::Memory);
  EXPECT_EQ(waitCategory(18), WaitCategory::Memory);
  EXPECT_EQ(waitCategory(32), WaitCategory::Preempted);
  EXPECT_EQ(waitCategory(4), WaitCategory::Other);
  EXPECT_EQ(waitCategoryName(WaitCategory::Io), "I/O");
}
