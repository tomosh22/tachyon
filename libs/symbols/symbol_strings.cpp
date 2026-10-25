#include "symbols/symbol_strings.h"

#include <cstring>

namespace tachyon::symbols {

SymbolStringTable::SymbolStringTable() : m_pages(new std::atomic<Entry*>[kMaxPages]) {
  for (uint32_t i = 0; i < kMaxPages; ++i) m_pages[i].store(nullptr, std::memory_order_relaxed);
  (void)intern("");
}

SymbolStringTable::~SymbolStringTable() = default;

const char* SymbolStringTable::store(std::string_view s) {
  if (s.empty()) return "";
  if (s.size() > kBlockSize / 4) {
    m_blocks.push_back(std::make_unique<char[]>(s.size()));
    std::memcpy(m_blocks.back().get(), s.data(), s.size());
    return m_blocks.back().get();
  }
  if (m_blockUsed + s.size() > kBlockSize) {
    m_blocks.push_back(std::make_unique<char[]>(kBlockSize));
    m_blockUsed = 0;
  }
  char* dst = m_blocks.back().get() + m_blockUsed;
  std::memcpy(dst, s.data(), s.size());
  m_blockUsed += s.size();
  return dst;
}

StringIdx SymbolStringTable::intern(std::string_view s) {
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_index.find(s);
  if (it != m_index.end()) return StringIdx(it->second);

  const uint32_t n = m_count.load(std::memory_order_relaxed);
  const uint32_t page = n >> kPageShift;
  if (page >= kMaxPages || s.size() > 0xFFFFFFFFu) return StringIdx::invalid();
  Entry* entries = m_pages[page].load(std::memory_order_relaxed);
  if (entries == nullptr) {
    m_pageStorage.push_back(std::make_unique<Entry[]>(kPageSize));
    entries = m_pageStorage.back().get();
    m_pages[page].store(entries, std::memory_order_release);
  }
  const char* data = store(s);
  entries[n & (kPageSize - 1)] = Entry{data, static_cast<uint32_t>(s.size())};
  m_index.emplace(std::string_view(data, s.size()), n);
  m_count.store(n + 1, std::memory_order_release);
  return StringIdx(n);
}

StringIdx SymbolStringTable::find(std::string_view s) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_index.find(s);
  return it == m_index.end() ? StringIdx::invalid() : StringIdx(it->second);
}

std::string_view SymbolStringTable::get(StringIdx idx) const {
  if (!idx.valid() || idx.get() >= m_count.load(std::memory_order_acquire)) return {};
  const Entry* entries = m_pages[idx.get() >> kPageShift].load(std::memory_order_acquire);
  const Entry& e = entries[idx.get() & (kPageSize - 1)];
  return std::string_view(e.data, e.length);
}

}  // namespace tachyon::symbols
