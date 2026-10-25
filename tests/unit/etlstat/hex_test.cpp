#include <gtest/gtest.h>

#include "etw_hex.h"
#include "test_util.h"

using namespace tachyon::etlstat;
using namespace tachyon::etlstat::testing;

TEST(HexEvents, FormatParseRoundTrip) {
  const ByteVec payload = stackWalkPayload(555, 10, 20, {0xAABBCCDD11223344ull, 2});
  const EtwEvent e = makeEvent(guids::kStackWalk, 32, 2, 123456789, 20, 3, payload);
  const std::string line = formatHexEventLine(e);
  const auto parsed = parseHexEventLine(line);
  ASSERT_TRUE(parsed.ok()) << parsed.error().toString();
  const EtwEvent back = parsed->view();
  EXPECT_EQ(back.provider, guids::kStackWalk);
  EXPECT_EQ(back.opcode, 32);
  EXPECT_EQ(back.version, 2);
  EXPECT_EQ(back.timestamp, 123456789u);
  EXPECT_EQ(back.threadId, 20u);
  EXPECT_EQ(back.processId, 100u);
  EXPECT_EQ(back.cpu, 3);
  ASSERT_EQ(back.payload.size(), payload.size());
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(), back.payload.begin()));
}

TEST(HexEvents, ToHexIsLowerCaseWithoutSeparators) {
  const ByteVec b = {std::byte{0x00}, std::byte{0xAB}, std::byte{0x7F}};
  EXPECT_EQ(toHex(std::span<const std::byte>(b.data(), b.size())), "00ab7f");
}

TEST(HexEvents, ParsesMultiLineTextWithCommentsAndBlankLines) {
  const std::string text =
      "# comment\n\n"
      "event provider=ce1dbfb4-137e-4da6-87b0-3f59aa102cbc opcode=46 version=2 ts=5 tid=1 pid=2 cpu=0 data=\n"
      "  event provider=ce1dbfb4-137e-4da6-87b0-3f59aa102cbc opcode=46 version=2 ts=6 tid=1 pid=2 cpu=0 "
      "data=0a0B\r\n";
  const auto events = parseHexEvents(text);
  ASSERT_TRUE(events.ok()) << events.error().toString();
  ASSERT_EQ(events->size(), 2u);
  EXPECT_TRUE((*events)[0].data.empty());
  ASSERT_EQ((*events)[1].data.size(), 2u);
  EXPECT_EQ(static_cast<uint8_t>((*events)[1].data[1]), 0x0B);
}

TEST(HexEvents, ReportsLineNumberOnError) {
  const auto bad = parseHexEvents("# ok\nevent provider=nope opcode=1\n");
  ASSERT_FALSE(bad.ok());
  EXPECT_NE(bad.error().message.find("line 2"), std::string::npos);
}

TEST(HexEvents, RejectsBadInputs) {
  EXPECT_FALSE(parseHexEventLine("evnt provider=x").ok());
  EXPECT_FALSE(parseHexEventLine("event opcode=1").ok());  // missing provider
  const std::string prov = "event provider=ce1dbfb4-137e-4da6-87b0-3f59aa102cbc ";
  EXPECT_FALSE(parseHexEventLine(prov + "opcode=300").ok());  // does not fit u8
  EXPECT_FALSE(parseHexEventLine(prov + "data=abc").ok());    // odd length
  EXPECT_FALSE(parseHexEventLine(prov + "data=zz").ok());
  EXPECT_FALSE(parseHexEventLine(prov + "bogus=1").ok());
  EXPECT_FALSE(parseHexEventLine(prov + "opcode").ok());
}

TEST(HexEvents, MissingFileIsNotFound) {
  const auto r = loadHexEventFile(std::string(TACHYON_FIXTURE_DIR) + "/etw/does_not_exist.hexevents");
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, tachyon::ErrorCode::NotFound);
}

TEST(HexEvents, SyntheticFixtureLoads) {
  const auto r = loadHexEventFile(std::string(TACHYON_FIXTURE_DIR) + "/etw/synthetic_basic.hexevents");
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r->size(), 17u);
}

TEST(HexEvents, DumpSelectorLimitsPerKey) {
  HexDumpSelector sel(2);
  const ByteVec p = sampledProfilePayload(1, 2);
  const EtwEvent a = makeEvent(guids::kPerfInfo, 46, 2, 1, 1, 0, p);
  const EtwEvent b = makeEvent(guids::kPerfInfo, 46, 3, 1, 1, 0, p);  // different version = different key
  EXPECT_TRUE(sel.select(a));
  EXPECT_TRUE(sel.select(a));
  EXPECT_FALSE(sel.select(a));
  EXPECT_TRUE(sel.select(b));
  HexDumpSelector none(0);
  EXPECT_FALSE(none.select(a));
}
