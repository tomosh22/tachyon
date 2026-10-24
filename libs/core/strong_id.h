// Strong-typedef'd 32-bit indices (Part 2.4). Layout-identical to uint32_t so they can sit in packed on-disk
// structs.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace tachyon {

inline constexpr uint32_t kInvalidIdx = 0xFFFFFFFFu;

template <typename Tag>
struct StrongId {
  uint32_t value = kInvalidIdx;

  constexpr StrongId() = default;
  constexpr explicit StrongId(uint32_t v) : value(v) {}

  // All members: thread-safe (value type).
  constexpr bool valid() const { return value != kInvalidIdx; }
  constexpr uint32_t get() const { return value; }
  constexpr explicit operator uint32_t() const { return value; }
  constexpr auto operator<=>(const StrongId&) const = default;

  static constexpr StrongId invalid() { return StrongId(kInvalidIdx); }
};

using ThreadIdx = StrongId<struct ThreadIdxTag>;
using ProcessIdx = StrongId<struct ProcessIdxTag>;
using ModuleIdx = StrongId<struct ModuleIdxTag>;
using FrameIdx = StrongId<struct FrameIdxTag>;
using CallstackIdx = StrongId<struct CallstackIdxTag>;
using StringIdx = StrongId<struct StringIdxTag>;
using FunctionIdx = StrongId<struct FunctionIdxTag>;

static_assert(sizeof(ThreadIdx) == 4 && std::is_trivially_copyable_v<ThreadIdx> &&
              std::is_standard_layout_v<ThreadIdx>);

}  // namespace tachyon

template <typename Tag>
struct std::hash<tachyon::StrongId<Tag>> {
  size_t operator()(const tachyon::StrongId<Tag>& id) const noexcept {
    return std::hash<uint32_t>{}(id.value);
  }
};
