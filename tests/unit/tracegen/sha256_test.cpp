#include "sha256.h"
#include "prng.h"
#include "test_util.h"

#include <gtest/gtest.h>

namespace tachyon::tracegen {
namespace {

std::string hashOf(const std::string& s) {
  Sha256 h;
  h.update(s.data(), s.size());
  return h.finishHex();
}

TEST(Sha256, KnownVectors) {
  EXPECT_EQ(hashOf(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(hashOf("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(hashOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, MillionAsAndChunkedUpdatesAgree) {
  const std::string million(1'000'000, 'a');
  EXPECT_EQ(hashOf(million), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  Sha256 h;
  for (size_t i = 0; i < million.size(); i += 777)
    h.update(million.data() + i, std::min<size_t>(777, million.size() - i));
  EXPECT_EQ(h.finishHex(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, FileHash) {
  testing_util::TempDir dir;
  const auto p = dir.path() / "f.bin";
  {
    std::ofstream f(p, std::ios::binary);
    f << "abc";
  }
  EXPECT_EQ(testing_util::sha(p), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_FALSE(sha256File(dir.path() / "missing").ok());
}

// The PRNG is the platform-independence anchor of the generator: these values must never change.
TEST(Prng, PinnedOutputs) {
  uint64_t s = 0;
  EXPECT_EQ(splitmix64(s), 0xe220a8397b1dcdafull);
  EXPECT_EQ(splitmix64(s), 0x6e789e6aa1b965f4ull);
  EXPECT_EQ(splitmix64(s), 0x06c45d188009454full);
  Prng a(42), b(42), c(43);
  for (int i = 0; i < 100; ++i) EXPECT_EQ(a.next(), b.next());
  EXPECT_NE(Prng(42).next(), c.next());
  EXPECT_EQ(fnv1a("tachyon"), fnv1a("tachyon"));
  EXPECT_NE(streamSeed(1, "a"), streamSeed(1, "b"));
  EXPECT_NE(streamSeed(1, "a", 1), streamSeed(1, "a", 2));
  Prng r(7);
  for (int i = 0; i < 1000; ++i) {
    const uint64_t v = r.range(10, 20);
    EXPECT_GE(v, 10u);
    EXPECT_LE(v, 20u);
  }
  EXPECT_EQ(r.below(0), 0u);
}

}  // namespace
}  // namespace tachyon::tracegen
