#include <gtest/gtest.h>

#include "etw_payload.h"
#include "test_util.h"

using namespace tachyon::etlstat;
using namespace tachyon::etlstat::testing;

namespace {
Bytes view(const ByteVec& v) {
  return Bytes(v.data(), v.size());
}
}  // namespace

TEST(Payload, DecodesSampledProfile) {
  const ByteVec b = sampledProfilePayload(0x7ff612345678ull, 4242);
  ASSERT_EQ(b.size(), kSampledProfileSize);
  const auto p = decodeSampledProfile(view(b));
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->instructionPointer, 0x7ff612345678ull);
  EXPECT_EQ(p->threadId, 4242u);
  EXPECT_EQ(p->count, 1);
}

TEST(Payload, SampledProfileRejectsTruncatedAndToleratesTrailingBytes) {
  ByteVec b = sampledProfilePayload(1, 2);
  b.pop_back();
  EXPECT_FALSE(decodeSampledProfile(view(b)).has_value());
  ByteVec longer = sampledProfilePayload(1, 2);
  put<uint64_t>(longer, 0xdeadbeef);
  EXPECT_TRUE(decodeSampledProfile(view(longer)).has_value());
}

TEST(Payload, DecodesCSwitch) {
  const ByteVec b = cswitchPayload(11, 22, 15);
  ASSERT_EQ(b.size(), kCSwitchSize);
  const auto p = decodeCSwitch(view(b));
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->newThreadId, 11u);
  EXPECT_EQ(p->oldThreadId, 22u);
  EXPECT_EQ(p->newThreadPriority, 8);
  EXPECT_EQ(p->oldThreadPriority, 9);
  EXPECT_EQ(p->previousCState, 1);
  EXPECT_EQ(p->oldThreadWaitReason, 15);
  EXPECT_EQ(p->oldThreadWaitMode, 1);
  EXPECT_EQ(p->oldThreadState, 5);
  EXPECT_EQ(p->oldThreadWaitIdealProcessor, 3);
  EXPECT_EQ(p->newThreadWaitTime, 77u);
  EXPECT_FALSE(decodeCSwitch(view(ByteVec(b.begin(), b.end() - 1))).has_value());
}

TEST(Payload, DecodesReadyThread) {
  const ByteVec b = readyThreadPayload(99);
  ASSERT_EQ(b.size(), kReadyThreadSize);
  const auto p = decodeReadyThread(view(b));
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->threadId, 99u);
  EXPECT_EQ(p->adjustReason, 1);
  EXPECT_EQ(p->adjustIncrement, 2);
  EXPECT_EQ(p->flag, 3);
  EXPECT_FALSE(decodeReadyThread(view(ByteVec(b.begin(), b.begin() + 7))).has_value());
}

TEST(Payload, DecodesStackWalk) {
  const ByteVec b = stackWalkPayload(123456, 10, 20, {0x1111, 0x2222, 0x3333});
  const auto p = decodeStackWalk(view(b));
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->eventTimeStamp, 123456u);
  EXPECT_EQ(p->stackProcess, 10u);
  EXPECT_EQ(p->stackThread, 20u);
  ASSERT_EQ(p->depth, 3u);
  EXPECT_EQ(stackFrame(*p, 0), 0x1111u);
  EXPECT_EQ(stackFrame(*p, 2), 0x3333u);
  EXPECT_EQ(stackFrame(*p, 3), 0u);
}

TEST(Payload, StackWalkEdgeCases) {
  const auto empty = decodeStackWalk(view(stackWalkPayload(1, 2, 3, {})));
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(empty->depth, 0u);
  EXPECT_FALSE(decodeStackWalk(view(ByteVec(15))).has_value());
  // Frame bytes not a multiple of the pointer size are rejected as malformed.
  ByteVec odd = stackWalkPayload(1, 2, 3, {0x10});
  odd.push_back(std::byte{0});
  EXPECT_FALSE(decodeStackWalk(view(odd)).has_value());
}

TEST(Payload, DecodesImage) {
  ByteVec b;
  put<uint64_t>(b, 0x7ff600000000ull);
  put<uint64_t>(b, 0x20000);
  put<uint32_t>(b, 100);
  put<uint32_t>(b, 0xAABB);
  put<uint32_t>(b, 0x5f000000);
  put<uint32_t>(b, 0);
  put<uint64_t>(b, 0x140000000ull);
  for (int i = 0; i < 4; ++i) put<uint32_t>(b, 0);
  putWide(b, u"\\Device\\game.exe");
  const auto p = decodeImage(view(b));
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->imageBase, 0x7ff600000000ull);
  EXPECT_EQ(p->imageSize, 0x20000u);
  EXPECT_EQ(p->processId, 100u);
  EXPECT_EQ(p->imageChecksum, 0xAABBu);
  EXPECT_EQ(p->timeDateStamp, 0x5f000000u);
  EXPECT_EQ(p->defaultBase, 0x140000000ull);
  EXPECT_EQ(p->fileName, "\\Device\\game.exe");
  EXPECT_FALSE(decodeImage(view(ByteVec(b.begin(), b.begin() + 55))).has_value());
}

TEST(Payload, DecodesThreadLifetimeAndSetName) {
  ByteVec life;
  put<uint32_t>(life, 7);
  put<uint32_t>(life, 8);
  const auto l = decodeThreadLifetime(view(life));
  ASSERT_TRUE(l.has_value());
  EXPECT_EQ(l->processId, 7u);
  EXPECT_EQ(l->threadId, 8u);

  ByteVec name;
  put<uint32_t>(name, 7);
  put<uint32_t>(name, 8);
  putWide(name, u"Worker é中");
  const auto n = decodeThreadSetName(view(name));
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(n->name, "Worker \xc3\xa9\xe4\xb8\xad");
  EXPECT_FALSE(decodeThreadSetName(view(ByteVec(7))).has_value());
}

TEST(Payload, Utf16HandlesSurrogatesAndMissingTerminator) {
  ByteVec b;
  put<uint16_t>(b, 0xD83D);  // U+1F600
  put<uint16_t>(b, 0xDE00);
  put<uint16_t>(b, 'x');
  EXPECT_EQ(utf16LeToUtf8(view(b)), "\xf0\x9f\x98\x80x");
  ByteVec lone;
  put<uint16_t>(lone, 0xD800);
  EXPECT_EQ(utf16LeToUtf8(view(lone)), "\xef\xbf\xbd");
}

TEST(Payload, DecodesProcessV4WithZeroSid) {
  ByteVec b;
  put<uint64_t>(b, 0xffff800000001000ull);
  put<uint32_t>(b, 100);
  put<uint32_t>(b, 4);
  put<uint32_t>(b, 1);
  put<int32_t>(b, 0);
  put<uint64_t>(b, 0x1000);
  put<uint32_t>(b, 0);  // v4 flags
  put<uint64_t>(b, 0);  // no SID
  for (char c : std::string("game.exe")) put<uint8_t>(b, static_cast<uint8_t>(c));
  put<uint8_t>(b, 0);
  putWide(b, u"game.exe --flag");
  const auto p = decodeProcess(view(b), 4);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->processId, 100u);
  EXPECT_EQ(p->parentId, 4u);
  EXPECT_EQ(p->imageFileName, "game.exe");
}

TEST(Payload, DecodesProcessWithRealSid) {
  ByteVec b;
  put<uint64_t>(b, 0xffff800000001000ull);
  put<uint32_t>(b, 200);
  put<uint32_t>(b, 4);
  put<uint32_t>(b, 1);
  put<int32_t>(b, 0);
  put<uint64_t>(b, 0x1000);
  put<uint32_t>(b, 0);
  // TOKEN_USER-like header (pointer + attributes) then SID with 2 sub-authorities.
  put<uint64_t>(b, 0x1234);
  put<uint64_t>(b, 0);
  put<uint8_t>(b, 1);  // revision
  put<uint8_t>(b, 2);  // sub-authority count
  for (int i = 0; i < 6; ++i) put<uint8_t>(b, 0);
  put<uint32_t>(b, 21);
  put<uint32_t>(b, 1000);
  for (char c : std::string("tool.exe")) put<uint8_t>(b, static_cast<uint8_t>(c));
  put<uint8_t>(b, 0);
  const auto p = decodeProcess(view(b), 4);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->imageFileName, "tool.exe");
  EXPECT_FALSE(decodeProcess(view(ByteVec(10)), 4).has_value());
}
