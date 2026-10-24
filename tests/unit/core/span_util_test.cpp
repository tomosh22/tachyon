#include "core/span_util.h"

#include <gtest/gtest.h>

namespace tachyon::core {
namespace {

#pragma pack(push, 1)
struct Rec {
  uint64_t ts;
  uint32_t tid;
  uint8_t kind;
};
#pragma pack(pop)
static_assert(sizeof(Rec) == 13);

TEST(SpanUtil, RoundTripPackedRecord) {
  std::vector<std::byte> buf;
  appendPod(buf, Rec{0x1122334455667788ull, 7, 3});
  appendPod(buf, uint16_t{0xBEEF});
  ASSERT_EQ(buf.size(), 15u);
  ByteReader reader(buf);
  auto rec = reader.read<Rec>();
  ASSERT_TRUE(rec);
  EXPECT_EQ(rec->ts, 0x1122334455667788ull);
  EXPECT_EQ(rec->tid, 7u);
  EXPECT_EQ(*reader.read<uint16_t>(), 0xBEEF);
  EXPECT_TRUE(reader.atEnd());
  EXPECT_FALSE(reader.read<uint8_t>());
}

TEST(SpanUtil, LittleEndianFromByteArray) {
  const uint8_t raw[] = {0x01, 0x00, 0x00, 0x00, 0xFF};
  auto bytes = bytesOf(raw, sizeof raw);
  EXPECT_EQ(*readPod<uint32_t>(bytes), 1u);
  EXPECT_FALSE(readPod<uint32_t>(bytes, 2));
  EXPECT_FALSE(subspanChecked(bytes, 4, 2));
  EXPECT_EQ(subspanChecked(bytes, 4, 1)->size(), 1u);
}

}  // namespace
}  // namespace tachyon::core
