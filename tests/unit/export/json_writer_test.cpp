#include "export/json_writer.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <functional>
#include <sstream>

namespace tachyon::exporters {
namespace {

std::string write(const std::function<void(JsonWriter&)>& fn) {
  std::ostringstream os;
  JsonWriter w(os);
  fn(w);
  EXPECT_TRUE(w.flush());
  return os.str();
}

TEST(JsonWriter, NestedStructureAndCommas) {
  const std::string s = write([](JsonWriter& w) {
    w.beginObject();
    w.key("a");
    w.value(1);
    w.key("b");
    w.beginArray();
    w.value("x");
    w.beginObject();
    w.endObject();
    w.beginArray();
    w.endArray();
    w.value(true);
    w.null();
    w.endArray();
    w.key("c");
    w.beginObject();
    w.key("d");
    w.value(uint64_t{18446744073709551615ull});
    w.key("e");
    w.value(int64_t{-5});
    w.endObject();
    w.endObject();
  });
  EXPECT_EQ(s, R"({"a":1,"b":["x",{},[],true,null],"c":{"d":18446744073709551615,"e":-5}})");
}

TEST(JsonWriter, EscapesAndRepairsUtf8) {
  const std::string s = write([](JsonWriter& w) {
    w.beginArray();
    w.value(std::string_view("q\"b\\n\n\t\x01\x1f/"));
    w.value(std::string_view("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"));  // valid 2/3/4-byte sequences
    w.value(
        std::string_view("bad\xFF"
                         "x\xC3"
                         "y\xE2\x82"
                         "z\xC0\xAF\xED\xA0\x80"));  // invalid bytes
    w.endArray();
  });
  const auto j = nlohmann::json::parse(s);  // must be valid JSON
  ASSERT_EQ(j.size(), 3u);
  EXPECT_EQ(j[0].get<std::string>(), std::string("q\"b\\n\n\t\x01\x1f/"));
  EXPECT_EQ(j[1].get<std::string>(), "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
  const std::string repaired = j[2].get<std::string>();
  EXPECT_NE(repaired.find("\xEF\xBF\xBD"), std::string::npos);
  EXPECT_EQ(repaired.find('\xFF'), std::string::npos);
}

TEST(JsonWriter, Doubles) {
  const std::string s = write([](JsonWriter& w) {
    w.beginArray();
    w.value(0.5);
    w.value(-1234.25);
    w.value(1e21);
    w.value(std::nan(""));
    w.value(INFINITY);
    w.endArray();
  });
  const auto j = nlohmann::json::parse(s);
  EXPECT_DOUBLE_EQ(j[0].get<double>(), 0.5);
  EXPECT_DOUBLE_EQ(j[1].get<double>(), -1234.25);
  EXPECT_DOUBLE_EQ(j[2].get<double>(), 1e21);
  EXPECT_TRUE(j[3].is_null());
  EXPECT_TRUE(j[4].is_null());
}

TEST(JsonWriter, MicrosFromNanosIsExact) {
  const std::string s = write([](JsonWriter& w) {
    w.beginArray();
    for (const int64_t ns : {int64_t{0}, int64_t{1}, int64_t{10}, int64_t{100}, int64_t{1000}, int64_t{1500},
                             int64_t{123456789}, int64_t{-1}, int64_t{-2500}, int64_t{-1000}})
      w.microsFromNanos(ns);
    w.endArray();
  });
  EXPECT_EQ(s, "[0,0.001,0.01,0.1,1,1.5,123456.789,-0.001,-2.5,-1]");
}

TEST(JsonWriter, FlushesLargeOutputInBlocks) {
  std::ostringstream os;
  JsonWriter w(os);
  w.beginArray();
  for (int i = 0; i < 100000; ++i) w.value(uint64_t{static_cast<uint64_t>(i)});
  w.endArray();
  ASSERT_TRUE(w.flush());
  const auto j = nlohmann::json::parse(os.str());
  ASSERT_EQ(j.size(), 100000u);
  EXPECT_EQ(j[99999].get<int>(), 99999);
}

TEST(JsonWriter, ReportsStreamFailure) {
  std::ostringstream os;
  os.setstate(std::ios::badbit);
  JsonWriter w(os);
  w.beginArray();
  w.value("x");
  w.endArray();
  EXPECT_FALSE(w.flush());
  EXPECT_FALSE(w.good());
}

}  // namespace
}  // namespace tachyon::exporters
