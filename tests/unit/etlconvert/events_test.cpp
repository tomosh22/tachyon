#include "test_util.h"

namespace tachyon::etlconvert {
namespace {

using namespace testing;

TEST(EtlEvents, SampledProfile) {
  const ByteVec b = sampledProfilePayload(0xFFFFF80012345678ull, 4242);
  auto p = decodeSampledProfile(b);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->instructionPointer, 0xFFFFF80012345678ull);
  EXPECT_EQ(p->threadId, 4242u);
  EXPECT_EQ(p->count, 1);
  EXPECT_FALSE(decodeSampledProfile(Bytes(b.data(), 15)).has_value());
}

TEST(EtlEvents, SampledProfileIgnoresTrailingBytes) {
  ByteVec b = sampledProfilePayload(1, 2);
  put<uint64_t>(b, 99);
  EXPECT_TRUE(decodeSampledProfile(b).has_value());
}

TEST(EtlEvents, CSwitch) {
  const ByteVec b = cswitchPayload(11, 22, /*waitReason=*/16, /*oldState=*/5, /*newPriority=*/9);
  auto p = decodeCSwitch(b);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->newThreadId, 11u);
  EXPECT_EQ(p->oldThreadId, 22u);
  EXPECT_EQ(p->newThreadPriority, 9);
  EXPECT_EQ(p->oldThreadWaitReason, 16);
  EXPECT_EQ(p->oldThreadWaitMode, 1);
  EXPECT_EQ(p->oldThreadState, 5);
  EXPECT_EQ(p->newThreadWaitTime, 77u);
  EXPECT_FALSE(decodeCSwitch(Bytes(b.data(), 23)).has_value());
}

TEST(EtlEvents, ReadyThread) {
  const ByteVec b = readyThreadPayload(77, 4, 2);
  auto p = decodeReadyThread(b);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->threadId, 77u);
  EXPECT_EQ(p->adjustReason, 4);
  EXPECT_EQ(p->flag, 2);
  EXPECT_FALSE(decodeReadyThread(Bytes(b.data(), 7)).has_value());
}

TEST(EtlEvents, StackWalk) {
  const ByteVec b = stackWalkPayload(5000, 100, 101, {0x10, 0x20, 0xFFFFF80000000030ull});
  auto p = decodeStackWalk(b);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->eventTimeStamp, 5000u);
  EXPECT_EQ(p->stackProcess, 100u);
  EXPECT_EQ(p->stackThread, 101u);
  ASSERT_EQ(p->depth, 3u);
  EXPECT_EQ(stackFrame(*p, 0), 0x10u);
  EXPECT_EQ(stackFrame(*p, 2), 0xFFFFF80000000030ull);
  EXPECT_EQ(stackFrame(*p, 3), 0u);
  // Empty stack is valid; a payload that is not a multiple of 8 after the header is not.
  EXPECT_EQ(decodeStackWalk(stackWalkPayload(1, 2, 3, {}))->depth, 0u);
  EXPECT_FALSE(decodeStackWalk(Bytes(b.data(), 20)).has_value());
  EXPECT_FALSE(decodeStackWalk(Bytes(b.data(), 8)).has_value());
}

TEST(EtlEvents, Image) {
  const ByteVec b =
      imagePayload(0x7ff600000000ull, 0x10000, 100, 0x5F000000, u"\\Device\\HarddiskVolume3\\game\\game.exe");
  auto p = decodeImage(b);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->imageBase, 0x7ff600000000ull);
  EXPECT_EQ(p->imageSize, 0x10000u);
  EXPECT_EQ(p->processId, 100u);
  EXPECT_EQ(p->timeDateStamp, 0x5F000000u);
  EXPECT_EQ(p->imageChecksum, 0x1234u);
  EXPECT_EQ(p->fileName, "\\Device\\HarddiskVolume3\\game\\game.exe");
  EXPECT_FALSE(decodeImage(Bytes(b.data(), 55)).has_value());
}

TEST(EtlEvents, ImageWithoutNameIsEmptyString) {
  const ByteVec b = imagePayload(1, 2, 3, 4, u"");
  auto p = decodeImage(b);
  ASSERT_TRUE(p.has_value());
  EXPECT_TRUE(p->fileName.empty());
}

TEST(EtlEvents, ThreadLifetimeAndName) {
  auto life = decodeThreadLifetime(threadPayload(100, 101));
  ASSERT_TRUE(life.has_value());
  EXPECT_EQ(life->processId, 100u);
  EXPECT_EQ(life->threadId, 101u);
  EXPECT_FALSE(decodeThreadLifetime(Bytes{}).has_value());

  auto name = decodeThreadSetName(threadNamePayload(100, 101, u"Renderé"));
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(name->threadId, 101u);
  EXPECT_EQ(name->name, "Render\xC3\xA9");
}

TEST(EtlEvents, ProcessV3AndV4) {
  auto p = decodeProcess(processPayload(100, 4, "game.exe", u"game.exe --run"), 3);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->processId, 100u);
  EXPECT_EQ(p->parentId, 4u);
  EXPECT_EQ(p->imageFileName, "game.exe");
  EXPECT_EQ(p->commandLine, "game.exe --run");

  // v4 has a Flags field before the SID.
  ByteVec v4;
  put<uint64_t>(v4, 1);
  put<uint32_t>(v4, 200);
  put<uint32_t>(v4, 100);
  put<uint32_t>(v4, 1);
  put<int32_t>(v4, 0);
  put<uint64_t>(v4, 0x1000);
  put<uint32_t>(v4, 0);  // flags
  put<uint64_t>(v4, 0);  // no SID
  putAnsi(v4, "other.exe");
  putWide(v4, u"other.exe");
  auto q = decodeProcess(v4, 4);
  ASSERT_TRUE(q.has_value());
  EXPECT_EQ(q->processId, 200u);
  EXPECT_EQ(q->imageFileName, "other.exe");
  EXPECT_FALSE(decodeProcess(Bytes(v4.data(), 10), 4).has_value());
}

TEST(EtlEvents, Utf16SurrogatesAndLoneSurrogates) {
  ByteVec b;
  putWide(b, u"a\U0001F600b");
  EXPECT_EQ(utf16LeToUtf8(b),
            "a\xF0\x9F\x98\x80"
            "b");
  ByteVec lone;
  put<uint16_t>(lone, 0xD800);
  put<uint16_t>(lone, 'x');
  EXPECT_EQ(utf16LeToUtf8(lone),
            "\xEF\xBF\xBD"
            "x");
}

}  // namespace
}  // namespace tachyon::etlconvert
