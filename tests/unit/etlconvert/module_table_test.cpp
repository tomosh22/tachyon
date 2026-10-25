#include "etlconvert/module_table.h"

#include "test_util.h"

namespace tachyon::etlconvert {
namespace {

using namespace testing;

PeLookupFn lookupFrom(std::map<std::string, PeSpec> specs) {
  return [specs = std::move(specs)](const std::string& path) -> Result<PeInfo> {
    auto it = specs.find(path);
    if (it == specs.end()) return makeError(ErrorCode::NotFound, path);
    return parsePe(buildPe(it->second));
  };
}

TEST(ModuleBucket, FindsByAddressAndTime) {
  ModuleBucket b;
  b.insert({0x1000, 0x1000, 0, kOpenEnded, 0});
  b.insert({0x3000, 0x1000, 0, 500, 1});
  b.insert({0x3000, 0x1000, 600, kOpenEnded, 2});  // same base reloaded later
  size_t hint = ~size_t{0};
  ASSERT_NE(b.find(0x1800, 10, hint), nullptr);
  EXPECT_EQ(b.find(0x1800, 10, hint)->moduleIdx, 0u);
  EXPECT_EQ(b.find(0x3010, 100, hint)->moduleIdx, 1u);
  EXPECT_EQ(b.find(0x3010, 700, hint)->moduleIdx, 2u);
  EXPECT_EQ(b.find(0x3010, 550, hint), nullptr);  // between the two loads
  EXPECT_EQ(b.find(0x2800, 10, hint), nullptr);   // gap
  EXPECT_EQ(b.find(0x500, 10, hint), nullptr);    // below everything
  EXPECT_EQ(b.find(0x2000, 10, hint), nullptr);   // one past the end of module 0
  EXPECT_NE(b.find(0x1FFF, 10, hint), nullptr);
}

TEST(ModuleBucket, UnloadClosesTheOpenInterval) {
  ModuleBucket b;
  b.insert({0x1000, 0x1000, 0, kOpenEnded, 0});
  EXPECT_TRUE(b.unload(0x1000, 300));
  size_t hint = ~size_t{0};
  EXPECT_NE(b.find(0x1100, 299, hint), nullptr);
  EXPECT_EQ(b.find(0x1100, 300, hint), nullptr);
  EXPECT_FALSE(b.unload(0x1000, 400));  // already closed
}

TEST(ModuleBucket, HintIsOnlyACache) {
  ModuleBucket b;
  b.insert({0x1000, 0x100, 0, kOpenEnded, 0});
  b.insert({0x2000, 0x100, 0, kOpenEnded, 1});
  size_t hint = 0;
  EXPECT_EQ(b.find(0x2010, 1, hint)->moduleIdx, 1u);
  EXPECT_EQ(hint, 1u);
  EXPECT_EQ(b.find(0x1010, 1, hint)->moduleIdx, 0u);
  hint = 999;
  EXPECT_EQ(b.find(0x2010, 1, hint)->moduleIdx, 1u);
}

TEST(ModuleTable, IdentifiesRsdsFromPeFile) {
  PeSpec game;
  game.timeDateStamp = 0x5F001234;
  game.sizeOfImage = 0x10000;
  ModuleTable t(lookupFrom({{"C:\\game\\game.exe", game}}), [] {
    DevicePathMapper m;
    m.addDevice("\\Device\\HarddiskVolume3", "C:");
    return m;
  }());
  const uint32_t idx =
      t.load(100, 0x7ff600000000ull, 0x10000, "\\Device\\HarddiskVolume3\\game\\game.exe", 0, 0x5F001234, 0);
  ASSERT_EQ(idx, 0u);
  const LoadedModule& m = t.modules()[0];
  EXPECT_EQ(m.path, "C:\\game\\game.exe");
  EXPECT_EQ(m.rec.id_kind, trace::kModuleIdPdb);
  EXPECT_EQ(m.rec.id_len, 16);
  EXPECT_EQ(std::memcmp(m.rec.id.data(), game.guid, 16), 0);
  EXPECT_EQ(m.rec.age, 3u);
  EXPECT_EQ(m.rec.pid, 100u);
  EXPECT_EQ(t.counters().rsds, 1u);
  EXPECT_EQ(t.counters().fallback, 0u);
}

TEST(ModuleTable, FallbackWhenFileMissing) {
  ModuleTable t(lookupFrom({}), DevicePathMapper{});
  t.load(100, 0x400000, 0x20000, "C:\\gone.dll", 0, 0xAABBCCDD, 0);
  const trace::ModuleInfo& r = t.modules()[0].rec;
  EXPECT_EQ(r.id_kind, trace::kModuleIdPeTimestampSize);
  EXPECT_EQ(r.id_len, 8);
  uint32_t stamp = 0;
  uint32_t size = 0;
  std::memcpy(&stamp, r.id.data(), 4);
  std::memcpy(&size, r.id.data() + 4, 4);
  EXPECT_EQ(stamp, 0xAABBCCDDu);
  EXPECT_EQ(size, 0x20000u);
  EXPECT_EQ(t.counters().fallback, 1u);
  EXPECT_EQ(t.counters().fileMissing, 1u);
}

TEST(ModuleTable, FallbackWhenFileDiffersFromLoadedImage) {
  PeSpec disk;
  disk.timeDateStamp = 0x11111111;  // a different build than the one loaded
  ModuleTable t(lookupFrom({{"C:\\a.dll", disk}}), DevicePathMapper{});
  t.load(1, 0x400000, 0x20000, "C:\\a.dll", 0, 0x22222222, 0);
  EXPECT_EQ(t.modules()[0].rec.id_kind, trace::kModuleIdPeTimestampSize);
  EXPECT_EQ(t.counters().mismatch, 1u);
  // The size can differ too.
  PeSpec sized;
  sized.sizeOfImage = 0x9000;
  ModuleTable t2(lookupFrom({{"C:\\b.dll", sized}}), DevicePathMapper{});
  t2.load(1, 0x400000, 0x20000, "C:\\b.dll", 0, sized.timeDateStamp, 0);
  EXPECT_EQ(t2.counters().mismatch, 1u);
}

TEST(ModuleTable, FallbackWhenNoCodeView) {
  PeSpec spec;
  spec.codeView = false;
  ModuleTable t(lookupFrom({{"C:\\n.dll", spec}}), DevicePathMapper{});
  t.load(1, 0x400000, spec.sizeOfImage, "C:\\n.dll", 0, spec.timeDateStamp, 0);
  EXPECT_EQ(t.modules()[0].rec.id_kind, trace::kModuleIdPeTimestampSize);
  EXPECT_EQ(t.counters().noCodeView, 1u);
}

TEST(ModuleTable, ZeroEventStampIsNotAMismatch) {
  PeSpec spec;
  ModuleTable t(lookupFrom({{"C:\\z.dll", spec}}), DevicePathMapper{});
  t.load(1, 0x400000, 0, "C:\\z.dll", 0, 0, 0);
  EXPECT_EQ(t.modules()[0].rec.id_kind, trace::kModuleIdPdb);
}

TEST(ModuleTable, RundownDuplicateOfLoadedModuleIsIgnored) {
  ModuleTable t(lookupFrom({}), DevicePathMapper{});
  const uint32_t a = t.load(1, 0x400000, 0x1000, "C:\\a.dll", 0, 1, /*loadTs=*/500);
  const uint32_t b = t.load(1, 0x400000, 0x1000, "C:\\a.dll", 0, 1, /*loadTs=*/0);  // DCEnd rundown
  EXPECT_EQ(a, b);
  EXPECT_EQ(t.modules().size(), 1u);
  EXPECT_EQ(t.modules()[0].rec.load_ts, 500u);
}

TEST(ModuleTable, SameBaseInTwoProcessesAreTwoModules) {
  ModuleTable t(lookupFrom({}), DevicePathMapper{});
  const uint32_t a = t.load(100, 0x7ffb00000000ull, 0x1000, "C:\\ntdll.dll", 0, 1, 0);
  const uint32_t b = t.load(200, 0x7ffb00000000ull, 0x1000, "C:\\ntdll.dll", 0, 1, 0);
  EXPECT_NE(a, b);
  size_t hint = ~size_t{0};
  EXPECT_EQ(t.bucketFor(100)->find(0x7ffb00000010ull, 5, hint)->moduleIdx, a);
  EXPECT_EQ(t.bucketFor(200)->find(0x7ffb00000010ull, 5, hint)->moduleIdx, b);
  EXPECT_EQ(t.bucketFor(300), nullptr);
}

TEST(ModuleTable, KernelImagesShareOneAddressSpace) {
  ModuleTable t(lookupFrom({}), DevicePathMapper{});
  const uint32_t k =
      t.load(0xFFFFFFFFu, 0xFFFFF80000000000ull, 0x100000, "\\SystemRoot\\system32\\ntoskrnl.exe", 0, 1, 0);
  size_t hint = ~size_t{0};
  const ModuleInterval* hit = t.kernelBucket().find(0xFFFFF80000001234ull, 10, hint);
  ASSERT_NE(hit, nullptr);
  EXPECT_EQ(hit->moduleIdx, k);
  EXPECT_EQ(t.modules()[k].rec.pid, 0u);
  EXPECT_EQ(t.bucketFor(0xFFFFFFFFu), nullptr);
}

TEST(ModuleTable, UnloadRecordsTimestampOnTheModule) {
  ModuleTable t(lookupFrom({}), DevicePathMapper{});
  t.load(1, 0x400000, 0x1000, "C:\\a.dll", 0, 1, 10);
  t.unload(1, 0x400000, 90);
  EXPECT_EQ(t.modules()[0].rec.unload_ts, 90u);
  t.unload(1, 0x999000, 95);  // unknown: ignored
  t.unload(7, 0x400000, 95);  // unknown process: ignored
  size_t hint = ~size_t{0};
  EXPECT_EQ(t.bucketFor(1)->find(0x400010, 90, hint), nullptr);
}

}  // namespace
}  // namespace tachyon::etlconvert
