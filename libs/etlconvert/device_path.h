// Converts NT device paths as logged by the kernel image events ("\Device\HarddiskVolume3\Windows\x.dll",
// "\SystemRoot\system32\ntoskrnl.exe", "\??\C:\x") into DOS paths the converter can open.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tachyon::etlconvert {

class DevicePathMapper {
 public:
  // Not thread-safe. Maps a device prefix ("\Device\HarddiskVolume3") to a DOS prefix ("C:").
  void addDevice(std::string devicePrefix, std::string dosPrefix);
  // Not thread-safe. Maps the "\SystemRoot" alias (e.g. to "C:\Windows").
  void setSystemRoot(std::string dosPath);

  // Thread-safe for const access. Returns the DOS path, or the input unchanged when no rule applies
  // (always the case with an empty mapper, i.e. on Linux). Device-name matching is ASCII case-insensitive and
  // requires a path-separator boundary after the prefix.
  std::string toDosPath(std::string_view path) const;

  // Thread-safe. Windows: queries QueryDosDevice for every drive letter and the SystemRoot environment.
  // Other platforms: an empty mapper (paths are used as-is).
  static DevicePathMapper fromSystem();

 private:
  std::vector<std::pair<std::string, std::string>> m_devices;
  std::string m_systemRoot;
};

}  // namespace tachyon::etlconvert
