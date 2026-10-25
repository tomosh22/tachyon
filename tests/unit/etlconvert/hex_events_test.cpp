#include "test_util.h"

namespace tachyon::etlconvert {
namespace {

using namespace testing;

TEST(HexEvents, GuidRoundTrip) {
  auto g = parseGuid("3d6fa8d0-fe05-11d0-9dda-00c04fd7ba7c");
  ASSERT_TRUE(g.has_value());
  EXPECT_EQ(*g, guids::kProcess);
  EXPECT_EQ(guidToString(*g), "3d6fa8d0-fe05-11d0-9dda-00c04fd7ba7c");
  EXPECT_EQ(parseGuid("{3D6FA8D1-FE05-11D0-9DDA-00C04FD7BA7C}"), guids::kThread);
  EXPECT_FALSE(parseGuid("3d6fa8d0-fe05-11d0-9dda").has_value());
  EXPECT_FALSE(parseGuid("zz6fa8d0-fe05-11d0-9dda-00c04fd7ba7c").has_value());
}

TEST(HexEvents, ParseLineAndFormatRoundTrip) {
  const std::string line =
      "event provider=ce1dbfb4-137e-4da6-87b0-3f59aa102cbc opcode=46 version=2 ts=10000 tid=101 pid=100 "
      "cpu=3 "
      "data=00100000f67f00006500000001000000";
  auto h = parseHexEventLine(line);
  ASSERT_TRUE(h.ok()) << h.error().toString();
  EXPECT_EQ(h->event.provider, guids::kPerfInfo);
  EXPECT_EQ(h->event.opcode, 46);
  EXPECT_EQ(h->event.timestamp, 10000u);
  EXPECT_EQ(h->event.cpu, 3);
  ASSERT_EQ(h->data.size(), 16u);
  auto s = decodeSampledProfile(h->view().payload);
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s->threadId, 101u);
  EXPECT_EQ(formatHexEventLine(h->view()), line);
}

TEST(HexEvents, RejectsMalformedLines) {
  EXPECT_FALSE(parseHexEventLine("garbage").ok());
  EXPECT_FALSE(parseHexEventLine("event opcode=1").ok());  // no provider
  EXPECT_FALSE(parseHexEventLine("event provider=ce1dbfb4-137e-4da6-87b0-3f59aa102cbc data=abc").ok());
  EXPECT_FALSE(parseHexEventLine("event provider=ce1dbfb4-137e-4da6-87b0-3f59aa102cbc opcode=999").ok());
  EXPECT_FALSE(parseHexEventLine("event provider=ce1dbfb4-137e-4da6-87b0-3f59aa102cbc bogus=1").ok());
  auto bad = parseHexEvents("# c\n\nevent nope\n");
  ASSERT_FALSE(bad.ok());
  EXPECT_NE(bad.error().message.find("line 3"), std::string::npos);
}

TEST(HexEvents, LoadsWp01SyntheticFixture) {
  auto events = loadHexEventFile(std::string(TACHYON_FIXTURE_DIR) + "/etw/synthetic_basic.hexevents");
  ASSERT_TRUE(events.ok()) << events.error().toString();
  EXPECT_EQ(events->size(), 17u);
  EXPECT_EQ(events->front().event.provider, guids::kProcess);
  EXPECT_FALSE(loadHexEventFile("/nonexistent/x.hexevents").ok());
}

}  // namespace
}  // namespace tachyon::etlconvert
