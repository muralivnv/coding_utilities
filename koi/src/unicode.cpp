#include "unicode.h"

#include <unigbrk.h>
#include <unistr.h>
#include <uniwidth.h>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace koi {
namespace {

constexpr Index kGraphemeContextBytes = 128;

constexpr Index kMaxGraphemeContextBytes = 1 << 16;

const std::uint8_t* AsU8(const std::string& s) {
  return reinterpret_cast<const std::uint8_t*>(s.data());
}

bool IsContinuationByte(char c) {
  return (static_cast<std::uint8_t>(c) & 0xC0) == 0x80;
}

size_t FirstCodePointStart(const std::string& window) {
  size_t i = 0;
  while (i < window.size() && IsContinuationByte(window[i])) ++i;
  return i;
}

std::optional<char> ByteAt(const PieceTable& table, Index doc_pos) {
  char c = 0;
  if (!koi::ByteAt(table, doc_pos, c)) return std::nullopt;
  return c;
}

enum class AsciiBreak {
  kUnknown,
  kBoundary,
  kInsideCrLf,
};

bool AsciiSingleColumn(std::string_view text, size_t i) {
  const auto c = static_cast<unsigned char>(text[i]);
  if ((c < 0x20) || (c >= 0x7F)) return false;
  return ((i + 1) >= text.size()) || (static_cast<unsigned char>(text[i + 1]) < 0x80);
}

AsciiBreak AsciiBreakAt(const PieceTable& table, Index at) {
  char before = 0;
  char here = 0;
  if (!koi::BytePairAt(table, at, before, here)) return AsciiBreak::kUnknown;
  if ((static_cast<unsigned char>(before) >= 0x80) || (static_cast<unsigned char>(here) >= 0x80)) {
    return AsciiBreak::kUnknown;
  }
  return ((before == '\r') && (here == '\n')) ? AsciiBreak::kInsideCrLf : AsciiBreak::kBoundary;
}

bool OpensInsideCluster(const std::string& window, size_t resync) {
  if (resync >= window.size()) return false;
  ucs4_t uc = 0;
  const int len = u8_mbtouc(&uc, AsU8(window) + resync, window.size() - resync);
  if (len <= 0) return false;
  switch (uc_graphemeclusterbreak_property(uc)) {
    case GBP_EXTEND:
    case GBP_ZWJ:
    case GBP_SPACINGMARK:
    case GBP_RI:
    case GBP_L:
    case GBP_V:
    case GBP_T:
    case GBP_LV:
    case GBP_LVT:
      return true;
    default:
      return false;
  }
}

// True when the segmenter had no whole code point at `at` to look at: either it
// ran out of window, or the window's last bytes are the head of a multi-byte
// sequence the read cut in half. u8_grapheme_next stops at both -- returning
// `end` for the first and the last complete code point start for the second --
// and neither is distinguishable from a real cluster end by the return value
// alone.
bool WindowEndsInsideACodePoint(const std::string& window, size_t at) {
  if (at >= window.size()) return true;
  const auto lead = static_cast<std::uint8_t>(window[at]);
  size_t need = 1;
  if ((lead & 0xE0) == 0xC0) {
    need = 2;
  } else if ((lead & 0xF0) == 0xE0) {
    need = 3;
  } else if ((lead & 0xF8) == 0xF0) {
    need = 4;
  }
  return (window.size() - at) < need;
}

struct Segmentation {
  Index prev{0};
  Index next{0};
  bool on_boundary{true};
};

// Residual (audited): once `context` reaches kMaxGraphemeContextBytes the
// window stops growing and the answer below is a guess again -- a cluster that
// big still gets reported as ending at the window edge, and the four public
// boundary functions can disagree about it because IsGraphemeBoundary re-centres
// its own window on a different position. A cap-sized cluster is out of scope;
// the cap is what bounds the work per query.
Segmentation SegmentAround(const PieceTable& table, Index doc_pos) {
  const Index doc_len = DocLength(table);
  const Index pos = std::clamp<Index>(doc_pos, 0, doc_len);
  Segmentation out{pos, pos, true};
  if (doc_len == 0) return out;

  for (Index context = kGraphemeContextBytes;; context *= 2) {
    const Index win_lo = std::max<Index>(0, pos - context);
    const Index win_hi = std::min<Index>(doc_len, pos + context);
    const std::string window = ReadDocRange(table, {win_lo, win_hi});
    if (window.empty()) return out;
    const size_t resync = FirstCodePointStart(window);
    const bool at_cap = (context >= kMaxGraphemeContextBytes);

    // The left edge may sit inside a cluster, which desynchronises the walk.
    if (!at_cap && (win_lo > 0) && OpensInsideCluster(window, resync)) continue;

    const auto target = static_cast<size_t>(pos - win_lo);
    const std::uint8_t* base = AsU8(window);
    const std::uint8_t* end = base + window.size();

    size_t i = resync;
    size_t prev = i;
    while (i < target) {
      const std::uint8_t* nx = u8_grapheme_next(base + i, end);
      size_t step = nx ? static_cast<size_t>(nx - base) : window.size();
      if (step <= i) step = i + 1;
      prev = i;
      i = step;
    }
    out.on_boundary = (i == target);

    size_t after = i;
    if (out.on_boundary) {
      const std::uint8_t* nx =
          (target < window.size()) ? u8_grapheme_next(base + target, end) : nullptr;
      after = std::max(nx ? static_cast<size_t>(nx - base) : window.size(), target);
    }

    // ...and so may the right one. u8_grapheme_next stops where it runs out of
    // bytes exactly as it stops at a cluster end, so a cluster that continues
    // past the window was reported as ending at the edge -- and the two are
    // only distinguishable by whether the window held a whole code point for it
    // to look at. Answering with the window edge hands back a position that
    // IsGraphemeBoundary -- which re-centres and sees the whole cluster --
    // then rejects, and Apply refuses every change at it.
    if (!at_cap && (win_hi < doc_len) && WindowEndsInsideACodePoint(window, after)) continue;

    out.prev = std::clamp<Index>(win_lo + static_cast<Index>(prev), 0, doc_len);
    out.next = std::clamp<Index>(win_lo + static_cast<Index>(after), 0, doc_len);
    return out;
  }
}

std::pair<Index, Index> ClampToDoc(const PieceTable& table, Interval range) {
  const Index doc_len = DocLength(table);
  const Index lo = std::max<Index>(0, range.empty() ? 0 : range.front());
  const Index hi = std::min<Index>(doc_len, range.empty() ? 0 : range.back() + 1);
  return {lo, hi};
}

}

std::string ReadDocRange(const PieceTable& table, Interval range) {
  const auto [lo, hi] = ClampToDoc(table, range);
  std::string out;
  if (lo >= hi) return out;
  out.reserve(static_cast<size_t>(hi - lo));

  pt::ReadInto(table.tree, lo, hi, SourceOf(table), out);
  return out;
}

bool IsGraphemeBoundary(const PieceTable& table, Index doc_pos) {
  const Index doc_len = DocLength(table);
  if (doc_pos <= 0) return doc_pos == 0;
  if (doc_pos >= doc_len) return doc_pos == doc_len;
  switch (AsciiBreakAt(table, doc_pos)) {
    case AsciiBreak::kBoundary: return true;
    case AsciiBreak::kInsideCrLf: return false;
    case AsciiBreak::kUnknown: break;
  }
  return SegmentAround(table, doc_pos).on_boundary;
}

Index NextGraphemeBoundary(const PieceTable& table, Index doc_pos) {
  const Index doc_len = DocLength(table);
  if (doc_pos >= doc_len) return doc_len;
  if (doc_pos < 0) return 0;
  if ((doc_pos + 1) >= doc_len) return doc_len;
  switch (AsciiBreakAt(table, doc_pos + 1)) {
    case AsciiBreak::kBoundary: return doc_pos + 1;
    case AsciiBreak::kInsideCrLf: return doc_pos + 2;
    case AsciiBreak::kUnknown: break;
  }
  return SegmentAround(table, doc_pos).next;
}

Index PrevGraphemeBoundary(const PieceTable& table, Index doc_pos) {
  const Index doc_len = DocLength(table);
  if (doc_pos <= 0) return 0;
  if (doc_pos == 1) return 0;
  switch (AsciiBreakAt(table, doc_pos - 1)) {
    case AsciiBreak::kBoundary: return doc_pos - 1;
    case AsciiBreak::kInsideCrLf: return doc_pos - 2;
    case AsciiBreak::kUnknown: break;
  }
  return SegmentAround(table, std::min(doc_pos, doc_len)).prev;
}

Index SnapToGraphemeBoundary(const PieceTable& table, Index doc_pos) {
  const Index doc_len = DocLength(table);
  if (doc_pos <= 0) return 0;
  if (doc_pos >= doc_len) return doc_len;
  switch (AsciiBreakAt(table, doc_pos)) {
    case AsciiBreak::kBoundary: return doc_pos;
    case AsciiBreak::kInsideCrLf: return doc_pos + 1;
    case AsciiBreak::kUnknown: break;
  }
  const Segmentation seg = SegmentAround(table, doc_pos);
  return seg.on_boundary ? doc_pos : seg.next;
}

Index CountGraphemes(const PieceTable& table, Interval range) {
  const std::string text = ReadDocRange(table, range);
  if (text.empty()) return 0;
  const std::uint8_t* base = AsU8(text);
  const std::uint8_t* end = base + text.size();
  Index count = 0;
  for (const std::uint8_t* p = base; p < end;) {
    const std::uint8_t* next = u8_grapheme_next(p, end);
    if (next == nullptr || next <= p) break;
    ++count;
    p = next;
  }
  return count;
}

void ReadDocRangeInto(const PieceTable& table, Interval range, std::string& out) {
  out.clear();
  const auto [lo, hi] = ClampToDoc(table, range);
  if (lo >= hi) return;

  pt::ReadInto(table.tree, lo, hi, SourceOf(table), out);
}

std::size_t NextGraphemeInString(std::string_view s, std::size_t pos) {
  if (pos >= s.size()) return s.size();
  const auto* base = reinterpret_cast<const std::uint8_t*>(s.data());
  const std::uint8_t* next = u8_grapheme_next(base + pos, base + s.size());
  const std::size_t stop = next ? static_cast<std::size_t>(next - base) : s.size();
  return (stop > pos) ? stop : (pos + 1);
}

std::size_t PrevGraphemeInString(std::string_view s, std::size_t pos) {
  if ((pos == 0) || s.empty()) return 0;
  const auto* base = reinterpret_cast<const std::uint8_t*>(s.data());
  const std::uint8_t* prev = u8_grapheme_prev(base + std::min(pos, s.size()), base);
  return prev ? static_cast<std::size_t>(prev - base) : 0;
}

int GraphemeWidth(std::string_view cluster) {
  if (cluster.empty()) return 0;
  const auto* p = reinterpret_cast<const std::uint8_t*>(cluster.data());
  ucs4_t first = 0;
  const int len = u8_mbtouc(&first, p, cluster.size());
  if (len <= 0) return 0;

  if (uc_graphemeclusterbreak_property(first) == GBP_RI) {
    if (static_cast<size_t>(len) < cluster.size()) {
      ucs4_t second = 0;
      const int len2 = u8_mbtouc(&second, p + len, cluster.size() - len);
      if ((len2 > 0) && (uc_graphemeclusterbreak_property(second) == GBP_RI)) return 2;
    }
    return 1;
  }

  // Tab keeps its zero: its width is positional, not intrinsic, and every
  // caller that lays text out already computes it from the column instead of
  // asking here (DisplayWidth, AdvanceOf, DrawLine).
  if (first == '\t') return 0;

  const int w = uc_width(static_cast<ucs4_t>(first), "UTF-8");
  // uc_width says -1 for every other control character. Zero was the old
  // answer, which made an ESC in a document occupy no column and leave nothing
  // on screen to say it was there -- and disagreed with the render path, which
  // has always clamped to one. PrintableCluster now draws a control picture, so
  // one column is both what is drawn and what the arithmetic should count.
  if (w < 0) return 1;
  return (w == 0) ? 1 : w;
}

Index DisplayWidth(std::string_view text, Index tab_width, Index start_column) {
  if (tab_width <= 0) tab_width = 8;
  Index column = start_column;
  const auto* base = reinterpret_cast<const std::uint8_t*>(text.data());
  const std::uint8_t* end = base + text.size();
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '\t') {
      column += tab_width - (column % tab_width);
      ++i;
      continue;
    }
    if (AsciiSingleColumn(text, i)) {
      ++column;
      ++i;
      continue;
    }
    const std::uint8_t* next = u8_grapheme_next(base + i, end);
    const size_t stop = next ? static_cast<size_t>(next - base) : text.size();
    const size_t take = (stop > i) ? (stop - i) : 1;
    column += GraphemeWidth(text.substr(i, take));
    i += take;
  }
  return column - start_column;
}

Interval LineContentRange(const PieceTable& table, Index line) {
  const Interval full = LineRange(table, line);
  Index lo = full.empty() ? 0 : full.front();
  Index hi = full.empty() ? 0 : full.back() + 1;
  if (hi > lo) {
    if (const std::optional<char> last = ByteAt(table, hi - 1); last && (*last == '\n')) {
      --hi;
      if (hi > lo) {
        if (const std::optional<char> cr = ByteAt(table, hi - 1); cr && (*cr == '\r')) --hi;
      }
    }
  }
  return Interval(lo, std::max(lo, hi));
}

Index ColumnForByte(const PieceTable& table, Index doc_pos, Index tab_width) {
  const Index doc_len = DocLength(table);
  const Index pos = std::clamp<Index>(doc_pos, 0, doc_len);
  Index line = 0;
  Index line_start = 0;
  LineAtAndStart(table, pos, line, line_start);
  return ColumnForByteFrom(table, line_start, pos, tab_width);
}

Index ColumnForByteFrom(const PieceTable& table, Index line_start, Index pos, Index tab_width) {
  if (pos <= line_start) return 0;
  static thread_local std::string prefix;
  ReadDocRangeInto(table, {line_start, pos}, prefix);
  return DisplayWidth(prefix, tab_width, 0);
}

Index ByteForColumn(const PieceTable& table, Index line, Index column, Index tab_width) {
  if (tab_width <= 0) tab_width = 8;
  const Interval content = LineContentRange(table, line);
  const Index lo = content.empty() ? LineStart(table, line) : content.front();
  const Index hi = content.empty() ? lo : content.back() + 1;
  if (column <= 0 || hi <= lo) return lo;

  static thread_local std::string scratch;
  ReadDocRangeInto(table, {lo, hi}, scratch);
  const std::string_view text{scratch};
  const auto* base = reinterpret_cast<const std::uint8_t*>(text.data());
  const std::uint8_t* end = base + text.size();
  Index col = 0;
  size_t i = 0;
  while (i < text.size()) {
    Index advance = 0;
    size_t take = 1;
    if (text[i] == '\t') {
      advance = tab_width - (col % tab_width);
    } else if (AsciiSingleColumn(text, i)) {
      advance = 1;
    } else {
      const std::uint8_t* next = u8_grapheme_next(base + i, end);
      const size_t stop = next ? static_cast<size_t>(next - base) : text.size();
      take = (stop > i) ? (stop - i) : 1;
      advance = GraphemeWidth(text.substr(i, take));
    }
    if (col + advance > column) break;
    col += advance;
    i += take;
  }
  return lo + static_cast<Index>(i);
}

bool IsWellFormedUtf8(std::string_view s) {
  if (s.empty()) return true;
  return u8_check(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()) == nullptr;
}

namespace {

// U+2400..U+241F then U+2421, three bytes each. All 33 share the lead `E2 90`,
// so the table is really the third byte -- but keeping it whole means the
// lookups below hand back a view into static storage rather than build one.
constexpr std::size_t kDelSlot = 32;
constexpr auto kControlPictures = [] {
  std::array<char, (kDelSlot + 1) * 3> table{};
  for (std::size_t c = 0; c <= kDelSlot; ++c) {
    table[(c * 3) + 0] = static_cast<char>(0xE2);
    table[(c * 3) + 1] = static_cast<char>(0x90);
    // U+2400 + c for the C0 pictures; DEL's picture is U+2421, one past the
    // last of them.
    table[(c * 3) + 2] = static_cast<char>(0x80 + ((c == kDelSlot) ? 0x21 : c));
  }
  return table;
}();

constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD

std::string_view PictureFor(unsigned char c) {
  const std::size_t slot = (c == 0x7F) ? kDelSlot : static_cast<std::size_t>(c);
  return std::string_view{kControlPictures.data() + (slot * 3), 3};
}

// True when `text[i]` begins a C1 control, i.e. the two-byte U+0080..U+009F.
// 0x9B is an eight-bit CSI on a terminal in 8-bit mode, so these matter as much
// as the C0 set does.
bool StartsC1(std::string_view text, std::size_t i) {
  if (static_cast<unsigned char>(text[i]) != 0xC2) return false;
  if ((i + 1) >= text.size()) return false;
  const auto second = static_cast<unsigned char>(text[i + 1]);
  return (second >= 0x80) && (second <= 0x9F);
}

// How many bytes the UTF-8 sequence led by `c` occupies, or 0 when `c` cannot
// lead one at all: a continuation byte, an overlong two-byte lead, or a lead
// past the top of the encoding.
std::size_t SequenceLength(unsigned char c) {
  if (c < 0x80) return 1;
  if (c < 0xC2) return 0;
  if (c < 0xE0) return 2;
  if (c < 0xF0) return 3;
  if (c < 0xF5) return 4;
  return 0;
}

// How many of a `want`-byte sequence starting at `text[i]` are actually present:
// `want` when the whole thing is there, fewer when the text runs out or a byte
// that is not a continuation cuts it short.
std::size_t SequencePresent(std::string_view text, std::size_t i, std::size_t want) {
  std::size_t have = 1;
  while ((have < want) && ((i + have) < text.size()) &&
         ((static_cast<unsigned char>(text[i + have]) & 0xC0) == 0x80)) {
    ++have;
  }
  return have;
}

}  // namespace

std::string_view PrintableCluster(std::string_view cluster) {
  if (cluster.empty()) return cluster;
  // The lead byte decides: a continuation byte is never below 0x20, so a byte
  // in the C0 range is always a control standing on its own -- including the
  // CR of a CR LF cluster, which collapses to one picture here because a cell
  // holds one.
  const auto lead = static_cast<unsigned char>(cluster.front());
  if ((lead < 0x20) || (lead == 0x7F)) return PictureFor(lead);
  if (StartsC1(cluster, 0)) return kReplacement;
  return cluster;
}

std::size_t AppendPrintable(std::string& out, std::string_view text, bool more) {
  std::size_t run = 0;   // start of the stretch that passes through untouched
  std::size_t i = 0;
  const auto substitute = [&](std::size_t at, std::size_t len) {
    out.append(text, run, at - run);
    out.append(kReplacement);
    run = at + len;
  };

  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      // Tab survives: see the header. Everything else in C0 and DEL does not --
      // a bare CR is as much an injection as an ESC, because it returns the
      // cursor to column 0 and lets later bytes overwrite what was drawn.
      if (((c < 0x20) && (c != '\t')) || (c == 0x7F)) {
        out.append(text, run, i - run);
        out.append(PictureFor(c));
        run = i + 1;
      }
      ++i;
      continue;
    }

    // Non-ASCII is taken a whole sequence at a time, so that a continuation
    // byte inside one -- 0x82 of the euro sign, say -- is never mistaken for
    // the loose half of a C1 below.
    const std::size_t want = SequenceLength(c);
    if (want == 0) {
      // Not a lead byte. 0x80..0x9F standing on its own is the second half of a
      // C1 whose 0xC2 went somewhere else, and a terminal in 8-bit mode reads
      // that byte as the control itself -- 0x9B is CSI -- so it gets the same
      // U+FFFD the pair would have. Other ill-formed bytes are not controls and
      // are left as they are.
      if (c <= 0x9F) substitute(i, 1);
      ++i;
      continue;
    }

    const std::size_t have = SequencePresent(text, i, want);
    if (have < want) {
      if (more && ((i + have) == text.size())) {
        // Cut by the chunk boundary rather than by the text: hand the bytes
        // back so the caller can put them in front of the next chunk. Without
        // this a 0xC2 ending one chunk and a 0x9B starting the next are a C1
        // that neither call can see, and it reaches the terminal intact.
        out.append(text, run, i - run);
        return text.size() - i;
      }
      // Ill-formed, and this is all of the text there is: one picture for the
      // stump. It is not a codepoint, so it is not a C1 either, but dropping it
      // silently is not what this function does with anything else.
      substitute(i, have);
      i += have;
      continue;
    }

    if (StartsC1(text, i)) substitute(i, want);
    i += want;
  }

  out.append(text, run, text.size() - run);
  return 0;
}

}
