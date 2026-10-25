#include "viewer/stats/fuzzy_matcher.h"

#include <algorithm>
#include <cstring>

namespace tachyon::viewer::stats {

namespace {

constexpr int32_t kBase = kTierSpan;

bool isAlnum(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
}
bool isUpper(unsigned char c) {
  return c >= 'A' && c <= 'Z';
}
bool isLowerCase(unsigned char c) {
  return c >= 'a' && c <= 'z';
}

// A word starts at the beginning, after a non-alphanumeric character, and at a lower-to-upper case change.
bool isWordStart(std::string_view text, size_t p) {
  if (p == 0) return true;
  const unsigned char prev = static_cast<unsigned char>(text[p - 1]);
  const unsigned char cur = static_cast<unsigned char>(text[p]);
  if (!isAlnum(prev)) return true;
  return isLowerCase(prev) && isUpper(cur);
}

int32_t caseBonus(const FuzzyQuery& q, std::string_view text, size_t at) {
  int32_t n = 0;
  for (size_t i = 0; i < q.typed.size() && at + i < text.size(); ++i)
    if (isUpper(static_cast<unsigned char>(q.typed[i])) && text[at + i] == q.typed[i]) ++n;
  return std::min<int32_t>(n * 4, 2000);
}

int32_t clampIntra(int64_t v) {
  return static_cast<int32_t>(std::clamp<int64_t>(v, 0, kTierSpan - 1));
}

FuzzyMatch make(MatchTier tier, int64_t intra) {
  return FuzzyMatch{tier, static_cast<int32_t>(tier) * kBase + clampIntra(intra)};
}

// Greedy forward match, tightened backwards: positions of the query characters in `lower` or empty.
bool subsequencePositions(std::string_view q, std::string_view lower, uint32_t* pos) {
  size_t at = 0, last = 0;
  for (char c : q) {
    const void* hit = at < lower.size() ? std::memchr(lower.data() + at, c, lower.size() - at) : nullptr;
    if (!hit) return false;
    last = static_cast<size_t>(static_cast<const char*>(hit) - lower.data());
    at = last + 1;
  }
  // Tightest window ending at `last`: walk backwards.
  size_t k = q.size();
  size_t start = last;
  for (size_t i = last + 1; i-- > 0;) {
    if (lower[i] == q[k - 1]) {
      if (--k == 0) {
        start = i;
        break;
      }
    }
  }
  size_t cur = start, i = 0;
  for (char c : q) {
    while (lower[cur] != c) ++cur;
    pos[i++] = static_cast<uint32_t>(cur);
    ++cur;
  }
  return true;
}

FuzzyMatch subsequenceMatch(const FuzzyQuery& q, std::string_view text, std::string_view lower) {
  uint32_t stackPos[64];
  std::vector<uint32_t> heapPos;  // only for queries longer than 64 characters
  uint32_t* pos = stackPos;
  if (q.lower.size() > 64) {
    heapPos.resize(q.lower.size());
    pos = heapPos.data();
  }
  if (!subsequencePositions(q.lower, lower, pos)) return {};
  const size_t count = q.lower.size();
  int64_t s = 0;
  int run = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t p = pos[i];
    s += 16;
    if (i > 0 && p == pos[i - 1] + 1) {
      ++run;
      s += 12 * std::min(run, 4);
    } else {
      run = 0;
      if (i > 0) s -= 3 + std::min<int64_t>(p - pos[i - 1] - 1, 10);
    }
    if (isWordStart(text, p)) s += 24;
    if (isUpper(static_cast<unsigned char>(q.typed[i])) && text[p] == q.typed[i]) s += 4;
  }
  s -= std::min<int64_t>(pos[0], 20);
  return make(MatchTier::Subsequence, s);
}

// `found` is lower.find(q.lower) or npos.
FuzzyMatch matchWithFind(const FuzzyQuery& q, std::string_view text, std::string_view lower, size_t found) {
  const size_t n = text.size();
  if (found == std::string_view::npos) return subsequenceMatch(q, text, lower);
  const int64_t length = static_cast<int64_t>(std::min<size_t>(n, 20000));
  if (found == 0) {
    if (n == q.lower.size()) return make(MatchTier::Exact, 50000 + caseBonus(q, text, 0));
    return make(MatchTier::Prefix, 50000 - 2 * length + caseBonus(q, text, 0));
  }
  size_t p = found;
  while (p != std::string_view::npos) {
    if (isWordStart(text, p))
      return make(
          MatchTier::WordStart,
          50000 - 2 * length - 10 * static_cast<int64_t>(std::min<size_t>(p, 500)) + caseBonus(q, text, p));
    p = lower.find(q.lower, p + 1);
  }
  return make(MatchTier::Substring, 50000 - 2 * length -
                                        10 * static_cast<int64_t>(std::min<size_t>(found, 500)) +
                                        caseBonus(q, text, found));
}

}  // namespace

FuzzyQuery FuzzyQuery::make(std::string_view text) {
  FuzzyQuery q;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
    q.typed.push_back(c);
    q.lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return q;
}

FuzzyMatch fuzzyMatch(const FuzzyQuery& query, std::string_view text, std::string_view lower) {
  if (query.empty()) return FuzzyMatch{MatchTier::Prefix, kBase * static_cast<int32_t>(MatchTier::Prefix)};
  if (query.lower.size() > text.size()) return {};
  return matchWithFind(query, text, lower, lower.find(query.lower));
}

FuzzyMatch fuzzyMatch(std::string_view query, std::string_view text) {
  const FuzzyQuery q = FuzzyQuery::make(query);
  std::string lower(text);
  for (char& c : lower)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return fuzzyMatch(q, text, lower);
}

uint32_t FuzzyIndex::add(std::string_view text) {
  if (m_offsets.empty()) m_offsets.push_back(0);
  const uint32_t index = static_cast<uint32_t>(m_offsets.size() - 1);
  m_text.append(text);
  for (char c : text) m_lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  m_offsets.push_back(static_cast<uint32_t>(m_text.size()));
  return index;
}

std::string_view FuzzyIndex::text(uint32_t index) const {
  if (index >= size()) return {};
  return std::string_view(m_text).substr(m_offsets[index], m_offsets[index + 1] - m_offsets[index]);
}

std::vector<FuzzyResult> FuzzyIndex::search(std::string_view query, size_t limit) const {
  return search(FuzzyQuery::make(query), limit);
}

std::vector<FuzzyResult> FuzzyIndex::search(const FuzzyQuery& query, size_t limit) const {
  std::vector<FuzzyResult> out;
  const size_t count = size();
  if (limit == 0 || count == 0) return out;
  if (query.empty()) {
    const FuzzyMatch all{MatchTier::Prefix, kBase * static_cast<int32_t>(MatchTier::Prefix)};
    for (uint32_t i = 0; i < count && out.size() < limit; ++i) out.push_back({i, all});
    return out;
  }
  // Total order: score descending, then the shorter name, then the lower index.
  const auto length = [&](uint32_t i) { return m_offsets[i + 1] - m_offsets[i]; };
  const auto better = [&](const FuzzyResult& a, const FuzzyResult& b) {
    if (a.match.score != b.match.score) return a.match.score > b.match.score;
    const uint32_t la = length(a.index), lb = length(b.index);
    if (la != lb) return la < lb;
    return a.index < b.index;
  };
  // Max-heap by "worse": the top is the worst of the kept results.
  const size_t qn = query.lower.size();
  const std::string_view q = query.lower;
  const std::string_view allText = m_text, allLower = m_lower;
  int64_t maxCase = 0;
  for (char c : query.typed) maxCase += isUpper(static_cast<unsigned char>(c)) ? 4 : 0;
  maxCase = std::min<int64_t>(maxCase, 2000);
  out.reserve(limit + 1);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t off = m_offsets[i], len = m_offsets[i + 1] - m_offsets[i];
    if (len < qn) continue;
    const std::string_view lower = allLower.substr(off, len);
    const size_t found = lower.find(q);
    const bool full = out.size() == limit;
    if (full) {
      const int32_t worst = out.front().match.score;
      if (found == std::string_view::npos) {
        if (worst >= 2 * kBase) continue;  // a subsequence match cannot beat the kept results
      } else {
        // Upper bound of the contiguous tiers (the first occurrence is the best position): skip what cannot
        // beat the worst kept result. Equal scores may still win on length, so only a strictly lower bound
        // prunes.
        const int32_t tier = found == 0 ? (len == qn ? static_cast<int32_t>(MatchTier::Exact)
                                                     : static_cast<int32_t>(MatchTier::Prefix))
                                        : static_cast<int32_t>(MatchTier::WordStart);
        const int64_t bound = static_cast<int64_t>(tier) * kBase + 50000 -
                              2 * static_cast<int64_t>(std::min<uint32_t>(len, 20000)) -
                              10 * static_cast<int64_t>(std::min<size_t>(found, 500)) + maxCase;
        if (bound < worst) continue;
      }
    }
    const FuzzyMatch m = matchWithFind(query, allText.substr(off, len), lower, found);
    if (!m.matched()) continue;
    const FuzzyResult r{i, m};
    if (!full) {
      out.push_back(r);
      std::push_heap(out.begin(), out.end(), better);
    } else if (better(r, out.front())) {
      std::pop_heap(out.begin(), out.end(), better);
      out.back() = r;
      std::push_heap(out.begin(), out.end(), better);
    }
  }
  std::sort(out.begin(), out.end(), better);
  return out;
}

}  // namespace tachyon::viewer::stats
