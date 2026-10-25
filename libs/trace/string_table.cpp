#include "trace/string_table.h"

#include "core/hash.h"
#include "core/span_util.h"

#include <cstring>

namespace tachyon::trace {

StringTable::StringTable() {
  m_offsets.push_back(0);
  m_table.assign(256, kInvalidIdx);
  intern("");
}

StringIdx StringTable::find(std::string_view s) const {
  const size_t mask = m_table.size() - 1;
  for (size_t slot = core::hashString(s) & mask;; slot = (slot + 1) & mask) {
    const uint32_t idx = m_table[slot];
    if (idx == kInvalidIdx) return StringIdx::invalid();
    if (get(StringIdx(idx)) == s) return StringIdx(idx);
  }
}

void StringTable::insertIndex(uint32_t idx) {
  if ((size() + 1) * 10 > m_table.size() * 7) grow();
  const size_t mask = m_table.size() - 1;
  const std::string_view s = get(StringIdx(idx));
  size_t slot = core::hashString(s) & mask;
  while (m_table[slot] != kInvalidIdx) {
    if (get(StringIdx(m_table[slot])) == s) return;  // duplicate ordinal: first wins
    slot = (slot + 1) & mask;
  }
  m_table[slot] = idx;
}

void StringTable::grow() {
  std::vector<uint32_t> old;
  old.swap(m_table);
  m_table.assign(old.size() * 2, kInvalidIdx);
  const size_t mask = m_table.size() - 1;
  for (uint32_t idx : old) {
    if (idx == kInvalidIdx) continue;
    size_t slot = core::hashString(get(StringIdx(idx))) & mask;
    while (m_table[slot] != kInvalidIdx) slot = (slot + 1) & mask;
    m_table[slot] = idx;
  }
}

StringIdx StringTable::intern(std::string_view s) {
  const StringIdx found = find(s);
  if (found.valid()) return found;
  const uint32_t idx = static_cast<uint32_t>(size());
  m_data.append(s);
  m_offsets.push_back(m_data.size());
  insertIndex(idx);
  return StringIdx(idx);
}

std::string_view StringTable::get(StringIdx idx) const {
  if (!idx.valid() || idx.get() >= size()) return {};
  const uint64_t b = m_offsets[idx.get()];
  return std::string_view(m_data).substr(b, m_offsets[idx.get() + 1] - b);
}

void StringTable::serialize(std::vector<std::byte>& out, size_t first, size_t count) const {
  for (size_t i = first; i < first + count && i < size(); ++i) {
    const std::string_view s = get(StringIdx(static_cast<uint32_t>(i)));
    core::appendPod(out, static_cast<uint32_t>(s.size()));
    core::appendBytes(out, std::as_bytes(std::span<const char>(s.data(), s.size())));
  }
}

Result<void> StringTable::deserializeAppend(std::span<const std::byte> payload, uint64_t recordCount) {
  core::ByteReader r(payload);
  for (uint64_t i = 0; i < recordCount; ++i) {
    auto len = r.read<uint32_t>();
    if (!len) return makeError(ErrorCode::CorruptData, "Strings chunk truncated (length)");
    auto bytes = r.readBytes(*len);
    if (!bytes) return makeError(ErrorCode::CorruptData, "Strings chunk truncated (bytes)");
    const uint32_t idx = static_cast<uint32_t>(size());
    m_data.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    m_offsets.push_back(m_data.size());
    insertIndex(idx);
  }
  if (!r.atEnd()) return makeError(ErrorCode::CorruptData, "Strings chunk has trailing bytes");
  return ok();
}

}  // namespace tachyon::trace
