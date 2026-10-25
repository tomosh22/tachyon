// Naming of `.tachyon-inst` sidecar files, including the numbered re-open files the SDK writes when it had to
// open the sidecar again in the same process (inst_wire.md, "Transports").
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace tachyon::inst {

// Thread-safe: pure function. The n-th re-open (n >= 1) of `base`: `<stem>.<n><ext>`, i.e. ".<n>" inserted
// before the last extension of the file name (appended when there is none), exactly as the SDK does.
std::filesystem::path numberedSidecarPath(const std::filesystem::path& base, uint32_t n);

// Thread-safe: reads the directory. `base` followed by its existing numbered siblings in numeric order.
// Gaps in the numbering are fine. `base` itself need not exist (it is returned regardless).
std::vector<std::filesystem::path> expandSidecarSiblings(const std::filesystem::path& base);

}  // namespace tachyon::inst
