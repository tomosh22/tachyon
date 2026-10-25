// Fuzzy name matching for the Function Statistics search box (WP-47). Pure, no ImGui.
//
// Matching. A query matches a name when its characters appear in the name in order (a subsequence),
// case-insensitively; whitespace in the query is ignored. Matches are ranked by tier first, then by a score
// inside the tier:
//   Exact       the name equals the query
//   Prefix      the name starts with the query
//   WordStart   the query occurs contiguously starting at a word start ("Engine::update" for "update";
//               word starts follow a non-alphanumeric character or a lower-to-upper case change)
//   Substring   the query occurs contiguously somewhere else
//   Subsequence the characters occur in order with gaps; scored by contiguous runs, word-start hits and
//               case matches, minus gap penalties
// Inside the contiguous tiers a shorter name and an earlier position score higher; a query character typed in
// upper case that meets an upper-case name character earns a small bonus. Ties are broken by the shorter
// name, then by the entry's position in the index, so the order is total and deterministic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon::viewer::stats {

enum class MatchTier : uint8_t { None = 0, Subsequence, Substring, WordStart, Prefix, Exact };

// A normalized query: whitespace removed, a lower-case copy for matching and the typed case for the bonus.
struct FuzzyQuery {
  std::string typed;  // whitespace removed, case kept
  std::string lower;  // ASCII lower case of `typed`

  // Thread-safe: pure function.
  static FuzzyQuery make(std::string_view text);
  bool empty() const { return lower.empty(); }
};

struct FuzzyMatch {
  MatchTier tier = MatchTier::None;
  int32_t score = 0;  // total ordering key: tier * kTierSpan + intra-tier score (higher is better)
  bool matched() const { return tier != MatchTier::None; }
};

inline constexpr int32_t kTierSpan = 100000;

// Thread-safe: pure function. Matches `query` against one name (`lower` is the ASCII lower-case copy of
// `text`). An empty query matches everything with tier Prefix and score 0.
FuzzyMatch fuzzyMatch(const FuzzyQuery& query, std::string_view text, std::string_view lower);
// Thread-safe: pure function. Convenience for tests: builds the query and the lower-case copy.
FuzzyMatch fuzzyMatch(std::string_view query, std::string_view text);

struct FuzzyResult {
  uint32_t index = 0;  // position in the index
  FuzzyMatch match;
};

// A set of names searched together. Names are stored back to back (original and lower case) so a scan over
// hundreds of thousands of names stays within a few milliseconds.
class FuzzyIndex {
 public:
  // Main-thread or build-thread only (not thread-safe against readers). Appends a name; returns its index.
  uint32_t add(std::string_view text);
  // Thread-safe (const).
  size_t size() const { return m_offsets.empty() ? 0 : m_offsets.size() - 1; }
  std::string_view text(uint32_t index) const;

  // Thread-safe (const). The best `limit` matches, best first. An empty query returns the first `limit`
  // names in index order. O(total name bytes) per call.
  std::vector<FuzzyResult> search(std::string_view query, size_t limit) const;
  std::vector<FuzzyResult> search(const FuzzyQuery& query, size_t limit) const;

 private:
  std::string m_text, m_lower;
  std::vector<uint32_t> m_offsets;  // size()+1 entries once non-empty
};

}  // namespace tachyon::viewer::stats
