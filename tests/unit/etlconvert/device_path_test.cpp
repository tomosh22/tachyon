#include "etlconvert/device_path.h"

#include "core/platform.h"

#include <gtest/gtest.h>

namespace tachyon::etlconvert {
namespace {

DevicePathMapper makeMapper() {
  DevicePathMapper m;
  m.addDevice("\\Device\\HarddiskVolume3", "C:");
  m.addDevice("\\Device\\HarddiskVolume30", "D:");
  m.setSystemRoot("C:\\Windows");
  return m;
}

TEST(DevicePath, MapsDeviceVolumeToDrive) {
  const auto m = makeMapper();
  EXPECT_EQ(m.toDosPath("\\Device\\HarddiskVolume3\\game\\game.exe"), "C:\\game\\game.exe");
  EXPECT_EQ(m.toDosPath("\\device\\harddiskvolume3\\x.dll"), "C:\\x.dll");
}

TEST(DevicePath, RequiresSeparatorBoundary) {
  const auto m = makeMapper();
  EXPECT_EQ(m.toDosPath("\\Device\\HarddiskVolume30\\a.dll"), "D:\\a.dll");
  EXPECT_EQ(m.toDosPath("\\Device\\HarddiskVolume300\\a.dll"), "\\Device\\HarddiskVolume300\\a.dll");
}

TEST(DevicePath, SystemRootAndNtPrefix) {
  const auto m = makeMapper();
  EXPECT_EQ(m.toDosPath("\\SystemRoot\\system32\\ntoskrnl.exe"), "C:\\Windows\\system32\\ntoskrnl.exe");
  EXPECT_EQ(m.toDosPath("\\??\\C:\\x\\y.dll"), "C:\\x\\y.dll");
}

TEST(DevicePath, UnknownAndEmptyMapperPassThrough) {
  DevicePathMapper empty;
  EXPECT_EQ(empty.toDosPath("/usr/lib/libc.so"), "/usr/lib/libc.so");
  EXPECT_EQ(empty.toDosPath("\\SystemRoot\\x"), "\\SystemRoot\\x");
  EXPECT_EQ(makeMapper().toDosPath("\\Device\\Mup\\server\\share\\a.dll"),
            "\\Device\\Mup\\server\\share\\a.dll");
  EXPECT_EQ(makeMapper().toDosPath(""), "");
}

TEST(DevicePath, FromSystemHasNoRulesOnLinux) {
#if !TACHYON_PLATFORM_WINDOWS
  EXPECT_EQ(DevicePathMapper::fromSystem().toDosPath("\\Device\\HarddiskVolume3\\a"),
            "\\Device\\HarddiskVolume3\\a");
#else
  GTEST_SKIP() << "Windows: rules come from QueryDosDevice";
#endif
}

}  // namespace
}  // namespace tachyon::etlconvert
