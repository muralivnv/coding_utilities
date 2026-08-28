#include "fuzzy.h"

#include <array>
#include <cstring>
#include <utility>

namespace koi {
namespace {

// Saturating, everywhere. A score that could go negative would let a long enough
// gap turn into a bonus the moment anything added to it again, and u16 keeps the
// two rolling rows in cache.
constexpr std::uint16_t Sat(int v) {
  return (v <= 0) ? std::uint16_t{0}
                  : static_cast<std::uint16_t>((v > 0xFFFF) ? 0xFFFF : v);
}

// ASCII only, and on purpose. The corpus is code: paths, identifiers and source
// lines. A byte >= 0x80 is left exactly as it is, so a UTF-8 candidate still
// matches byte-for-byte -- it just does not case-fold, which is the slow path
// this design says will barely fire.
constexpr char Fold(char c) {
  return ((c >= 'A') && (c <= 'Z')) ? static_cast<char>(c - 'A' + 'a') : c;
}

// Fold maps A-Z to a-z and nothing else, so a folded byte in a-z is matched by
// exactly two candidate bytes and any other folded byte by exactly one. That
// makes every case-folded scan one or two memchr calls instead of a byte loop.
const char* FindFolded(const char* data, std::size_t size, char want) {
  if (size == 0) return nullptr;
  const auto* const lower =
      static_cast<const char*>(std::memchr(data, static_cast<unsigned char>(want), size));
  if ((want < 'a') || (want > 'z')) return lower;
  const auto upper = static_cast<unsigned char>(want - 'a' + 'A');
  const auto* const hit = static_cast<const char*>(std::memchr(data, upper, size));
  if (lower == nullptr) return hit;
  if (hit == nullptr) return lower;
  return (lower < hit) ? lower : hit;
}

const char* RFindFolded(const char* data, std::size_t size, char want) {
  if (size == 0) return nullptr;
  const auto* const lower =
      static_cast<const char*>(memrchr(data, static_cast<unsigned char>(want), size));
  if ((want < 'a') || (want > 'z')) return lower;
  const auto upper = static_cast<unsigned char>(want - 'a' + 'A');
  const auto* const hit = static_cast<const char*>(memrchr(data, upper, size));
  if (lower == nullptr) return hit;
  if (hit == nullptr) return lower;
  return (lower > hit) ? lower : hit;
}

FuzzyClass ClassOf(char c, std::string_view delimiters) {
  if (static_cast<unsigned char>(c) >= 0x80) return FuzzyClass::kLetter;
  if ((c >= 'a') && (c <= 'z')) return FuzzyClass::kLower;
  if ((c >= 'A') && (c <= 'Z')) return FuzzyClass::kUpper;
  if ((c >= '0') && (c <= '9')) return FuzzyClass::kNumber;
  if ((c == ' ') || (c == '\t') || (c == '\n') || (c == '\v') || (c == '\f') || (c == '\r')) {
    return FuzzyClass::kWhite;
  }
  if (delimiters.find(c) != std::string_view::npos) return FuzzyClass::kDelimiter;
  return FuzzyClass::kNonWord;
}

// fzf's table, read off the transition rather than off the character: what a
// position is worth depends on what precedes it.
int BonusFor(FuzzyClass prev, FuzzyClass cur, const FuzzyConfig& config) {
  if (cur > FuzzyClass::kNonWord) {
    switch (prev) {
      case FuzzyClass::kNonWord:
        return kFuzzyBoundary;
      case FuzzyClass::kDelimiter:
        return kFuzzyBoundaryDelimiter;
      case FuzzyClass::kWhite:
        return config.boundary_white;
      default:
        break;
    }
  }
  if (((prev == FuzzyClass::kLower) && (cur == FuzzyClass::kUpper)) ||
      ((prev != FuzzyClass::kNumber) && (cur == FuzzyClass::kNumber))) {
    return kFuzzyCamel;
  }
  switch (cur) {
    case FuzzyClass::kNonWord:
    case FuzzyClass::kDelimiter:
      return kFuzzyNonWord;
    case FuzzyClass::kWhite:
      return config.boundary_white;
    default:
      break;
  }
  return 0;
}

// The class chain and the bonus chain, flattened into two indexed loads. Built
// by calling ClassOf and BonusFor, so a table cannot disagree with them.
constexpr std::size_t kFuzzyClasses = 7;
static_assert((static_cast<std::size_t>(FuzzyClass::kNumber) + 1) == kFuzzyClasses,
              "the bonus table is indexed by FuzzyClass");

struct ConfigTables {
  std::array<std::uint8_t, 256> klass;
  std::array<std::array<std::uint8_t, kFuzzyClasses>, kFuzzyClasses> bonus;
};

ConfigTables BuildTables(const FuzzyConfig& config) {
  ConfigTables tables{};
  for (std::size_t c = 0; c < tables.klass.size(); ++c) {
    tables.klass[c] =
        static_cast<std::uint8_t>(ClassOf(static_cast<char>(c), config.delimiters));
  }
  for (std::size_t prev = 0; prev < kFuzzyClasses; ++prev) {
    for (std::size_t cur = 0; cur < kFuzzyClasses; ++cur) {
      tables.bonus[prev][cur] = static_cast<std::uint8_t>(
          BonusFor(static_cast<FuzzyClass>(prev), static_cast<FuzzyClass>(cur), config));
    }
  }
  return tables;
}

// Namespace scope rather than function-local: a local static is read through a
// guard variable on every call, and these are read once per scored candidate.
const ConfigTables kDefaultTables = BuildTables(FuzzyConfig::Default());
const ConfigTables kPathTables = BuildTables(FuzzyConfig::Path());

// `delimiters` and `boundary_white` are the only fields ClassOf and BonusFor
// read, so two configs that agree on them share a table. Two configs ship;
// anything else is built into the caller's scratch rather than cached, which
// keeps the scorer a pure function of its arguments.
const ConfigTables& TablesFor(const FuzzyConfig& config, ConfigTables& scratch) {
  const auto same = [&config](const FuzzyConfig& other) {
    return (config.boundary_white == other.boundary_white) &&
           (config.delimiters == other.delimiters);
  };
  if (same(FuzzyConfig::Default())) return kDefaultTables;
  if (same(FuzzyConfig::Path())) return kPathTables;
  scratch = BuildTables(config);
  return scratch;
}

int PrefixBonus(const FuzzyConfig& config, std::size_t offset) {
  if (!config.prefer_prefix) return 0;
  const int decayed = kFuzzyPreferPrefix - static_cast<int>(offset) * kFuzzyGapStart;
  return (decayed > 0) ? decayed : 0;
}

// The greedy in-order gate. It answers "is the needle a subsequence at all",
// which is what kills most candidates for free, and it bounds the DP: nothing
// left of lo[0] and nothing right of `last` can be part of any alignment, and
// row i starts at lo[i] because that is the earliest column needle[i] can reach.
//
// The right-hand bound is global rather than per row, which is fzf's shape and
// not an approximation of it: row i has to carry its gap chain as far right as
// row i+1 will read a diagonal from, and that is the end of the span.
struct Gate {
  std::array<std::size_t, kFuzzyMaxNeedle> lo{};
  std::size_t last{0};
  std::size_t n{0};
  bool ok{false};
};

Gate RunGate(std::string_view needle, std::string_view candidate) {
  Gate gate;
  gate.n = needle.size();
  // A subsequence cannot outrun its haystack, and this is also what keeps the
  // scans below off an empty candidate's null data pointer.
  if (gate.n > candidate.size()) return gate;
  const char* const data = candidate.data();
  const std::size_t size = candidate.size();
  std::size_t at = 0;
  for (std::size_t i = 0; i < gate.n; ++i) {
    const char* const hit = FindFolded(data + at, size - at, Fold(needle[i]));
    if (hit == nullptr) return gate;
    at = static_cast<std::size_t>(hit - data);
    gate.lo[i] = at;
    ++at;
  }
  // The last place the last needle character occurs: everything past it is dead
  // span, and on a source line that is most of the line.
  const char* const back = RFindFolded(data, size, Fold(needle[gate.n - 1]));
  gate.last = static_cast<std::size_t>(back - data);
  gate.ok = true;
  return gate;
}

// score and, for the run it ends, the bonus that run inherited and where it
// began. `start` is what makes the first-match tie-break exact without a
// backtrace. `consec` holds a bonus, which never exceeds kFuzzyBoundaryWhite,
// and `start` holds a column, which never exceeds kFuzzyMaxSpan - 1; both fit a
// byte, which keeps the two rolling rows at 4 KB instead of 6 KB.
//
// No default member initialisers: they would make an array of Cell
// non-trivially-default-constructible, and the rolling rows below would then be
// zeroed on every call whether or not they are declared with `{}`.
struct Cell {
  std::uint16_t score;
  std::uint8_t consec;
  std::uint8_t start;
};

static_assert(sizeof(Cell) == 4, "the rolling rows are sized on a packed cell");
static_assert(kFuzzyMaxSpan <= 256, "a column index must fit the cell's start byte");
static_assert(kFuzzyBoundaryWhite <= 255 && kFuzzyBoundaryDelimiter <= 255 &&
                  kFuzzyBoundary <= 255 && kFuzzyNonWord <= 255 && kFuzzyCamel <= 255,
              "a carried bonus must fit the cell's consec byte");

// Where the backtrace needs a decision recorded. Null for the scoring path,
// which is every candidate; only the displayed row pays for these.
struct Trace {
  std::uint8_t* m_from_gap{nullptr};
  std::uint8_t* p_from_match{nullptr};
};

struct DpOut {
  int score{0};
  std::size_t first{0};
  std::size_t end{0};
  bool matched{false};
};

// Two rolling rows, fixed buffers, no allocation. `m` is the best alignment
// ending in a match at this column, `p` the best ending in a gap; the affine
// penalty is the difference between opening one and extending one.
DpOut RunDp(std::string_view needle, std::string_view candidate, const FuzzyConfig& config,
            const ConfigTables& tables, const Gate& gate, const Trace& trace) {
  const std::size_t base = gate.lo[0];
  const std::size_t width = gate.last - base + 1;

  // Uninitialised on purpose: the loop below writes [0, width) and nothing
  // outside that range is ever read.
  std::array<std::uint8_t, kFuzzyMaxSpan> bonus;
  {
    std::uint8_t prev = (base == 0)
                            ? static_cast<std::uint8_t>(config.start_class)
                            : tables.klass[static_cast<unsigned char>(candidate[base - 1])];
    for (std::size_t x = 0; x < width; ++x) {
      const std::uint8_t cur = tables.klass[static_cast<unsigned char>(candidate[base + x])];
      bonus[x] = tables.bonus[prev][cur];
      prev = cur;
    }
  }

  // Also uninitialised: every row clears [row_lo, row_hi] as it computes it, and
  // gate.lo is strictly increasing while row_hi grows by one per row, so row i's
  // reads of row i-1 all land inside the range row i-1 wrote.
  std::array<std::array<Cell, kFuzzyMaxSpan>, 2> m_rows;
  std::array<std::array<Cell, kFuzzyMaxSpan>, 2> p_rows;
  Cell* m_prev = m_rows[0].data();
  Cell* p_prev = p_rows[0].data();
  Cell* m_cur = m_rows[1].data();
  Cell* p_cur = p_rows[1].data();

  std::size_t prev_lo = 0;
  for (std::size_t i = 0; i < gate.n; ++i) {
    const std::size_t row_lo = gate.lo[i] - base;
    // nucleo's parallelogram. Row i only has to reach as far right as row i+1
    // reads a diagonal from, and the last row stops at the last column, so row i
    // stops (n-1-i) columns short. Every cell dropped is one no alignment can
    // pass through. row_lo <= row_hi holds because gate.lo is strictly
    // increasing and gate.lo[n-1] <= gate.last.
    const std::size_t row_hi = width - 1 - (gate.n - 1 - i);
    const char raw = needle[i];
    const char want = Fold(raw);
    for (std::size_t x = row_lo; x <= row_hi; ++x) {
      // The clear, fused into the compute loop: the gap branch writes every
      // p cell but the first, and the match branch may leave an m cell unwritten.
      m_cur[x] = Cell{};
      if (x == row_lo) p_cur[x] = Cell{};

      // The gap cell first: it reads this row's previous column, which the last
      // turn of this loop wrote.
      if (x > row_lo) {
        const std::uint16_t opened = Sat(static_cast<int>(m_cur[x - 1].score) - kFuzzyGapStart);
        const bool can_extend = ((x - 1) > row_lo);
        const std::uint16_t extended =
            can_extend ? Sat(static_cast<int>(p_cur[x - 1].score) - kFuzzyGapExtension) : 0;
        // Both scores saturate to 0 often enough that the tie alone would credit
        // a gap-open to a column holding no match cell, and the backtrace would
        // leave the chain there. The scores agree either way; the trace does
        // not, so the open needs a real match cell behind it.
        const bool m_valid = (m_cur[x - 1].score > 0);
        const bool from_match = m_valid && (!can_extend || (opened >= extended));
        p_cur[x] = Cell{from_match ? opened : extended, 0,
                        from_match ? m_cur[x - 1].start : p_cur[x - 1].start};
        if (trace.p_from_match != nullptr) {
          trace.p_from_match[(i * kFuzzyMaxSpan) + x] = from_match ? 1 : 0;
        }
      }

      const char c = candidate[base + x];
      if (Fold(c) != want) continue;
      const int penalty = (c != raw) ? kFuzzyCaseMismatch : 0;
      const int b = bonus[x];

      if (i == 0) {
        // The first needle character carries more weight than the rest, so its
        // bonus -- and only its bonus, never the match score -- is doubled.
        const int score = kFuzzyMatch + (b * kFuzzyFirstCharMultiplier) - penalty +
                          PrefixBonus(config, base + x);
        m_cur[x].score = Sat(score);
        // The run starts here, so this is what the next character inherits.
        m_cur[x].consec = static_cast<std::uint8_t>(b);
        m_cur[x].start = static_cast<std::uint8_t>(x);
        if (trace.m_from_gap != nullptr) trace.m_from_gap[(i * kFuzzyMaxSpan) + x] = 0;
        continue;
      }

      const Cell& m_diag = m_prev[x - 1];
      const Cell& p_diag = p_prev[x - 1];
      // A valid match cell always scores at least kFuzzyMatch - kFuzzyCaseMismatch,
      // so zero is unreachable rather than cheap. A gap cell can legitimately be
      // zero after enough extensions, which is why its validity is positional.
      const bool m_ok = (m_diag.score > 0);
      const bool p_ok = ((x - 1) > prev_lo);
      if (!m_ok && !p_ok) continue;

      // The carry, per cell -- the part fzf derives from side arrays and nucleo
      // fixes. A run inherits the bonus of whatever started it (at least
      // kFuzzyConsecutive), and only a bonus at word-boundary strength that
      // beats the carry may break the run and become the new one.
      int carry = std::max<int>(m_diag.consec, kFuzzyConsecutive);
      if ((b >= kFuzzyBoundary) && (b > carry)) carry = b;
      const int from_m = m_ok ? (static_cast<int>(m_diag.score) + std::max(carry, b)) : -1;
      const int from_p = p_ok ? (static_cast<int>(p_diag.score) + b) : -1;
      const bool took_gap = (from_p > from_m);
      const int best = took_gap ? from_p : from_m;
      m_cur[x].score = Sat(best + kFuzzyMatch - penalty);
      m_cur[x].consec = static_cast<std::uint8_t>(took_gap ? b : carry);
      m_cur[x].start = took_gap ? p_diag.start : m_diag.start;
      if (trace.m_from_gap != nullptr) {
        trace.m_from_gap[(i * kFuzzyMaxSpan) + x] = took_gap ? 1 : 0;
      }
    }
    std::swap(m_prev, m_cur);
    std::swap(p_prev, p_cur);
    prev_lo = row_lo;
  }

  DpOut out;
  const std::size_t last_lo = gate.lo[gate.n - 1] - base;
  for (std::size_t x = last_lo; x < width; ++x) {
    if (m_prev[x].score <= out.score) continue;
    out.score = m_prev[x].score;
    out.first = base + m_prev[x].start;
    out.end = x;
    out.matched = true;
  }
  return out;
}

// A one-character needle has no gaps, no runs and no second row, so the DP
// collapses to the leftmost best-scoring occurrence -- which is exactly what row
// 0 computes and exactly the tie-break the final scan applies.
FuzzyResult ScoreOne(std::string_view needle, std::string_view candidate,
                     const FuzzyConfig& config, const ConfigTables& tables) {
  FuzzyResult out;
  const char raw = needle[0];
  const char want = Fold(raw);
  const char* const data = candidate.data();
  const std::size_t size = candidate.size();
  std::size_t at = 0;
  while (at < size) {
    const char* const hit = FindFolded(data + at, size - at, want);
    if (hit == nullptr) break;
    const auto p = static_cast<std::size_t>(hit - data);
    // The DP's preamble starts its class chain one byte before the first
    // occurrence, so every occurrence reads the transition into itself.
    const std::uint8_t prev = (p == 0) ? static_cast<std::uint8_t>(config.start_class)
                                       : tables.klass[static_cast<unsigned char>(data[p - 1])];
    const int b = tables.bonus[prev][tables.klass[static_cast<unsigned char>(data[p])]];
    const int penalty = (data[p] != raw) ? kFuzzyCaseMismatch : 0;
    const int score = Sat(kFuzzyMatch + (b * kFuzzyFirstCharMultiplier) - penalty +
                          PrefixBonus(config, p));
    if (score > out.score) {
      out.score = score;
      out.first = static_cast<int>(p);
      out.matched = true;
    }
    at = p + 1;
  }
  return out;
}

// Cap and clamp, never allocate: a needle past 32 characters is scored on its
// first 32, a candidate past 256 bytes on its first 256.
std::string_view CapNeedle(std::string_view needle) {
  return needle.substr(0, std::min(needle.size(), kFuzzyMaxNeedle));
}

std::string_view CapCandidate(std::string_view candidate) {
  return candidate.substr(0, std::min(candidate.size(), kFuzzyMaxSpan));
}

bool IEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (Fold(a[i]) != Fold(b[i])) return false;
  }
  return true;
}

bool IStartsWith(std::string_view hay, std::string_view prefix) {
  return (hay.size() >= prefix.size()) && IEquals(hay.substr(0, prefix.size()), prefix);
}

// Case-insensitive contiguous containment, the band-2 test.
bool IContains(std::string_view haystack, std::string_view needle) {
  if (needle.size() > haystack.size()) return false;
  for (std::size_t at = 0; at + needle.size() <= haystack.size(); ++at) {
    if (IEquals(haystack.substr(at, needle.size()), needle)) return true;
  }
  return false;
}

bool IsSubsequence(std::string_view needle, std::string_view hay) {
  // Same guard as RunGate's: a length reject, and an empty hay never reaches the
  // scan.
  if (needle.size() > hay.size()) return false;
  const char* const data = hay.data();
  const std::size_t size = hay.size();
  std::size_t at = 0;
  for (const char want : needle) {
    const char* const hit = FindFolded(data + at, size - at, Fold(want));
    if (hit == nullptr) return false;
    at = static_cast<std::size_t>(hit - data) + 1;
  }
  return true;
}

}  // namespace

FuzzyResult FuzzyScore(std::string_view needle, std::string_view candidate,
                       const FuzzyConfig& config) {
  FuzzyResult out;
  needle = CapNeedle(needle);
  const std::string_view hay = CapCandidate(candidate);
  if (needle.empty()) {
    out.matched = true;
    out.first = 0;
    return out;
  }
  ConfigTables scratch;
  const ConfigTables& tables = TablesFor(config, scratch);
  if (needle.size() == 1) return ScoreOne(needle, hay, config, tables);
  const Gate gate = RunGate(needle, hay);
  if (!gate.ok) return out;
  const DpOut dp = RunDp(needle, hay, config, tables, gate, Trace{});
  if (!dp.matched) return out;
  out.matched = true;
  out.score = dp.score;
  out.first = static_cast<int>(dp.first);
  return out;
}

std::vector<FuzzySpan> FuzzyPositions(std::string_view needle, std::string_view candidate,
                                      const FuzzyConfig& config) {
  std::vector<FuzzySpan> spans;
  needle = CapNeedle(needle);
  const std::string_view hay = CapCandidate(candidate);
  if (needle.empty()) return spans;
  const Gate gate = RunGate(needle, hay);
  if (!gate.ok) return spans;

  // ~16k of stack, paid once for the row that gets highlighted rather than once
  // per candidate. Everything else goes through FuzzyScore, which traces
  // nothing.
  static_assert((kFuzzyMaxNeedle * kFuzzyMaxSpan) == 8192, "trace buffers are sized to the caps");
  std::array<std::uint8_t, kFuzzyMaxNeedle * kFuzzyMaxSpan> m_from_gap{};
  std::array<std::uint8_t, kFuzzyMaxNeedle * kFuzzyMaxSpan> p_from_match{};
  const Trace trace{m_from_gap.data(), p_from_match.data()};
  ConfigTables scratch;
  const ConfigTables& tables = TablesFor(config, scratch);
  const DpOut dp = RunDp(needle, hay, config, tables, gate, trace);
  if (!dp.matched) return spans;

  const std::size_t base = gate.lo[0];
  std::vector<std::size_t> columns;
  columns.reserve(needle.size());
  std::size_t x = dp.end;
  for (std::size_t i = gate.n; i-- > 0;) {
    columns.push_back(base + x);
    if (i == 0) break;
    if (m_from_gap[(i * kFuzzyMaxSpan) + x] == 0) {
      --x;
      continue;
    }
    // This match started a run, so the previous needle character is at the far
    // end of a gap chain in the row above. Walk the chain back to the cell that
    // opened it.
    std::size_t k = x - 1;
    while ((k > 0) && (p_from_match[((i - 1) * kFuzzyMaxSpan) + k] == 0)) --k;
    x = (k > 0) ? (k - 1) : 0;
  }

  for (std::size_t i = columns.size(); i-- > 0;) {
    const int at = static_cast<int>(columns[i]);
    if (!spans.empty() && (spans.back().end == at)) {
      spans.back().end = at + 1;
    } else {
      spans.push_back(FuzzySpan{at, at + 1});
    }
  }
  return spans;
}

int FuzzySelfScore(std::string_view needle, const FuzzyConfig& config) {
  needle = needle.substr(0, std::min(needle.size(), kFuzzyMaxNeedle));
  if (needle.empty()) return 0;
  // Closed form, and equal to FuzzyScore(needle, needle, config) by
  // construction: the run starts at the config's start-of-string class, which is
  // the strongest bonus either config hands out, so the carry swallows every
  // bonus inside the needle and every character past the first is worth
  // kFuzzyMatch + that same start bonus.
  const int start = BonusFor(config.start_class, ClassOf(needle[0], config.delimiters), config);
  const int rest = static_cast<int>(needle.size()) - 1;
  return kFuzzyMatch + (start * kFuzzyFirstCharMultiplier) + (rest * (kFuzzyMatch + start)) +
         PrefixBonus(config, 0);
}

double NormalizedScore(int score, std::string_view needle, const FuzzyConfig& config) {
  const int self = FuzzySelfScore(needle, config);
  if (self <= 0) return 0.0;
  return std::clamp(static_cast<double>(score) / static_cast<double>(self), 0.0, 1.0);
}

int MatchBand(std::string_view needle, std::string_view candidate, std::size_t name_offset) {
  if (needle.empty()) return 0;
  if (IEquals(needle, candidate)) return 4;
  const std::string_view name = candidate.substr(std::min(name_offset, candidate.size()));
  // Subsequence first: bands 3 and 2 both imply it, so a row headed for band 0
  // never pays for the quadratic IContains.
  if (!IsSubsequence(needle, name)) return 0;
  if (IStartsWith(name, needle)) return 3;
  // Contiguous beats scattered by a whole band, not by bonus arithmetic:
  // `node` inside KeyNode is a statement about the name, `n..o..d..e` strewn
  // through InsertNewlineAutoIndent is a guess about it.
  if (IContains(name, needle)) return 2;
  return 1;
}

FuzzyMatch ScoreBanded(std::string_view needle, std::string_view candidate,
                       std::size_t name_offset, const FuzzyConfig& config) {
  FuzzyMatch out;
  if (needle.empty()) {
    out.matched = true;
    out.first = 0;
    return out;
  }
  // One cap, applied before anything reads either string, so the band and the
  // score are statements about the same bytes. Without it a candidate whose only
  // match sits past byte 256 is classified into a band and then fails to score,
  // and the row disappears; capped in one place it is honestly unmatched.
  needle = CapNeedle(needle);
  candidate = CapCandidate(candidate);
  name_offset = std::min(name_offset, candidate.size());
  const int band = MatchBand(needle, candidate, name_offset);
  // With no name offset the name is the whole candidate, so band 0 means the
  // needle is not a subsequence of the candidate and the DP below can only fail.
  if ((band == 0) && (name_offset == 0)) return out;
  // Bands 1 to 3 are statements about the name, so the name is what gets
  // scored -- and the prefix bonus is measured from the name's first byte, which
  // is what makes `key` -> `koi/src/keymap.cpp` earn the whole of it. Band 4 is
  // a statement about the whole candidate and band 0 is what is left.
  const std::size_t offset = ((band >= 1) && (band <= 3)) ? name_offset : 0;
  const FuzzyResult scored = FuzzyScore(needle, candidate.substr(offset), config);
  if (!scored.matched) return out;
  out.matched = true;
  out.band = band;
  out.score = scored.score;
  out.first = scored.first + static_cast<int>(offset);
  out.normalized = NormalizedScore(scored.score, needle, config);
  out.banded = static_cast<double>(band) + out.normalized;
  return out;
}

bool RanksBefore(const RankKey& a, const RankKey& b) {
  // Two comparisons rather than one inequality: these are doubles, and
  // -Wfloat-equal is right that == on a pair of them is nearly always a mistake.
  if (a.score > b.score) return true;
  if (b.score > a.score) return false;
  if (a.length != b.length) return a.length < b.length;
  if (a.first != b.first) return a.first < b.first;
  return a.order < b.order;
}

}  // namespace koi
