#include "inst/frame_stream.h"

#include "wire_builder.h"

#include <gtest/gtest.h>

#include <algorithm>

#include <cstring>

namespace tachyon::inst {
namespace {

using testing_util::StreamBuilder;

StreamBuilder sampleStream() {
  StreamBuilder b;
  b.header();
  const uint64_t key = b.string("scope");
  b.scopeBegin(10, 1, key, "data");
  b.scopeEnd(20, 1);
  b.plot(30, key, 1.5);
  return b;
}

TEST(FrameAssembler, SplitsWholeStreamInOneChunk) {
  const StreamBuilder b = sampleStream();
  FrameAssembler a;
  size_t frames = 0;
  EXPECT_TRUE(a.feed(b.bytes().data(), b.bytes().size(), [&](const uint8_t*, size_t) { ++frames; }));
  EXPECT_EQ(frames, b.frames());
  EXPECT_EQ(a.pendingBytes(), 0u);
}

TEST(FrameAssembler, EverySplitPointYieldsTheSameFrames) {
  const StreamBuilder b = sampleStream();
  const auto& bytes = b.bytes();
  for (size_t cut = 0; cut <= bytes.size(); ++cut) {
    FrameAssembler a;
    std::vector<std::vector<uint8_t>> frames;
    auto sink = [&](const uint8_t* f, size_t n) { frames.emplace_back(f, f + n); };
    EXPECT_TRUE(a.feed(bytes.data(), cut, sink));
    EXPECT_TRUE(a.feed(bytes.data() + cut, bytes.size() - cut, sink));
    ASSERT_EQ(frames.size(), b.frames()) << "cut " << cut;
    EXPECT_EQ(a.pendingBytes(), 0u);
    std::vector<uint8_t> joined;
    for (const auto& f : frames) joined.insert(joined.end(), f.begin(), f.end());
    EXPECT_EQ(joined, bytes) << "cut " << cut;
  }
}

TEST(FrameAssembler, ByteAtATime) {
  const StreamBuilder b = sampleStream();
  FrameAssembler a;
  size_t frames = 0;
  for (uint8_t byte : b.bytes()) EXPECT_TRUE(a.feed(&byte, 1, [&](const uint8_t*, size_t) { ++frames; }));
  EXPECT_EQ(frames, b.frames());
  EXPECT_EQ(a.pendingBytes(), 0u);
}

TEST(FrameAssembler, ReportsTruncatedTail) {
  const StreamBuilder b = sampleStream();
  FrameAssembler a;
  size_t frames = 0;
  const size_t cut = b.bytes().size() - 3;
  EXPECT_TRUE(a.feed(b.bytes().data(), cut, [&](const uint8_t*, size_t) { ++frames; }));
  EXPECT_EQ(frames, b.frames() - 1);
  EXPECT_GT(a.pendingBytes(), 0u);
}

TEST(FrameAssembler, ZeroLengthPayloadFrame) {
  StreamBuilder b;
  b.frameRaw(99, nullptr, 0);
  FrameAssembler a;
  size_t frames = 0;
  for (uint8_t byte : b.bytes())
    EXPECT_TRUE(a.feed(&byte, 1, [&](const uint8_t*, size_t n) {
      EXPECT_EQ(n, sizeof(FrameHeader));
      ++frames;
    }));
  EXPECT_EQ(frames, 1u);
}

TEST(FrameAssembler, ImplausibleLengthMakesTheStreamCorrupt) {
  FrameHeader bad{3, kMaxSkippablePayloadBytes + 1};
  std::vector<uint8_t> bytes(sizeof(bad) + 100, 0xAA);
  std::memcpy(bytes.data(), &bad, sizeof(bad));
  FrameAssembler a;
  size_t frames = 0;
  EXPECT_FALSE(a.feed(bytes.data(), bytes.size(), [&](const uint8_t*, size_t) { ++frames; }));
  EXPECT_TRUE(a.corrupt());
  EXPECT_EQ(frames, 0u);
  EXPECT_EQ(a.discardedBytes(), bytes.size());
  // Later input is discarded too.
  EXPECT_FALSE(a.feed(bytes.data(), 10, [&](const uint8_t*, size_t) { ++frames; }));
  EXPECT_EQ(a.discardedBytes(), bytes.size() + 10);
}

TEST(FrameAssembler, OversizedButPlausibleFrameIsSkippedByItsLength) {
  StreamBuilder b;
  b.header();
  b.scopeEnd(1, 1);
  const std::vector<uint8_t> big(2u << 20, 0x5A);  // 2 MiB: above what is kept, below what is believable
  b.frameRaw(77, big.data(), big.size());
  b.scopeEnd(2, 1);
  for (size_t chunk : {size_t{1}, size_t{7}, size_t{8}, size_t{4096}, size_t{1} << 30}) {
    FrameAssembler a;
    size_t frames = 0;
    const auto& bytes = b.bytes();
    for (size_t off = 0; off < bytes.size(); off += chunk) {
      const size_t n = std::min(chunk, bytes.size() - off);
      EXPECT_TRUE(a.feed(bytes.data() + off, n, [&](const uint8_t*, size_t) { ++frames; }))
          << "chunk " << chunk;
    }
    EXPECT_EQ(frames, 3u) << "chunk " << chunk;  // header + the two ScopeEnds
    EXPECT_EQ(a.skippedFrames(), 1u);
    EXPECT_FALSE(a.corrupt());
    EXPECT_EQ(a.pendingBytes(), 0u);
    EXPECT_EQ(a.skipRemaining(), 0u);
  }
}

TEST(FrameAssembler, StreamEndingInsideAnOversizedFrameReportsTheMissingBytes) {
  StreamBuilder b;
  const std::vector<uint8_t> big(2u << 20, 0);
  b.frameRaw(77, big.data(), big.size());
  FrameAssembler a;
  EXPECT_TRUE(a.feed(b.bytes().data(), 1000, [](const uint8_t*, size_t) {}));
  EXPECT_EQ(a.skipRemaining(), big.size() - (1000 - sizeof(FrameHeader)));
}

TEST(FrameAssembler, CorruptLengthSplitAcrossChunks) {
  FrameHeader bad{3, 0xFFFFFFFFu};
  std::vector<uint8_t> bytes(sizeof(bad));
  std::memcpy(bytes.data(), &bad, sizeof(bad));
  FrameAssembler a;
  EXPECT_TRUE(a.feed(bytes.data(), 5, [](const uint8_t*, size_t) {}));
  EXPECT_FALSE(a.feed(bytes.data() + 5, 3, [](const uint8_t*, size_t) {}));
  EXPECT_TRUE(a.corrupt());
}

TEST(PeekFrame, ReadsTimestampThreadAndDroppedCount) {
  StreamBuilder b;
  b.header();
  b.scopeBegin(111, 7, 0x42);
  b.dropped(222, 8, 9);
  b.plot(333, 1, 0.5);
  std::vector<FramePeek> peeks;
  FrameAssembler a;
  a.feed(b.bytes().data(), b.bytes().size(),
         [&](const uint8_t* f, size_t n) { peeks.push_back(peekFrame(f, n)); });
  ASSERT_EQ(peeks.size(), 4u);
  EXPECT_FALSE(peeks[0].isRecord);
  EXPECT_TRUE(peeks[1].isRecord);
  EXPECT_EQ(peeks[1].ts, 111u);
  EXPECT_EQ(peeks[1].tid, 7u);
  EXPECT_EQ(peeks[2].dropped, 9u);
  EXPECT_EQ(peeks[3].ts, 333u);
  EXPECT_EQ(peeks[3].tid, 0u);
}

}  // namespace
}  // namespace tachyon::inst
