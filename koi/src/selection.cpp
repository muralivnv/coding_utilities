#include "selection.h"

#include <algorithm>
#include <limits>

#include "unicode.h"

namespace koi {
namespace {

Selection Snap(const PieceTable& table, Selection s) {
  const Index doc_len = DocLength(table);
  s.anchor = SnapToGraphemeBoundary(table, std::clamp<Index>(s.anchor, 0, doc_len));
  s.head = SnapToGraphemeBoundary(table, std::clamp<Index>(s.head, 0, doc_len));
  return s;
}

}  // namespace

Index CursorOf(const PieceTable& table, const Selection& s) {
  // Forward selections keep the head one past the last selected grapheme, so
  // the cursor is the grapheme behind it. Backward ones already point at it.
  if (s.head > s.anchor) return PrevGraphemeBoundary(table, s.head);
  return s.head;
}

Selection MinWidth1(const PieceTable& table, Selection s) {
  if (!s.IsEmpty()) return s;
  const Index next = NextGraphemeBoundary(table, s.head);
  if (next > s.head) s.head = next;
  return s;
}

Selection PutCursor(const PieceTable& table, Selection s, Index pos, bool extend) {
  if (!extend) {
    s.anchor = pos;
    s.head = NextGraphemeBoundary(table, pos);
    return s;
  }
  // Crossing the anchor: a forward selection holds its anchor grapheme at
  // [anchor, anchor+1); once the cursor moves behind it, that grapheme must be
  // held from the other side or it silently drops out on direction flip.
  //
  // Only when there is a grapheme under the anchor to re-hold. A zero-width
  // caret -- insert mode, the one place the model allows one (selection.h) --
  // holds nothing, and `head >= anchor` was true of it by equality, so
  // extending backwards off a caret annexed the grapheme in *front* of it:
  // `i` then A-I selected one too many and the following `d` ate it.
  if (!s.IsEmpty()) {
    if ((s.head > s.anchor) && (pos < s.anchor)) {
      s.anchor = NextGraphemeBoundary(table, s.anchor);
    } else if ((s.head < s.anchor) && (pos >= s.anchor)) {
      s.anchor = PrevGraphemeBoundary(table, s.anchor);
    }
  }
  // `pos` is where the cursor goes, never the head: forwards the head is one
  // grapheme past it, backwards the head *is* the cursor.
  s.head = (s.anchor <= pos) ? NextGraphemeBoundary(table, pos) : pos;
  return s;
}

Selection CoveringSelection(const PieceTable& table, Index from, Index to) {
  const Index doc_len = DocLength(table);
  Index lo = std::clamp<Index>(from, 0, doc_len);
  // The end rounds forwards, which is what SnapToGraphemeBoundary already does;
  // the start has to round the other way or the cluster the match starts inside
  // drops out of the selection along with the match.
  const Index hi = SnapToGraphemeBoundary(table, std::clamp<Index>(to, lo, doc_len));
  if (!IsGraphemeBoundary(table, lo)) lo = PrevGraphemeBoundary(table, lo);
  return Selection{lo, hi, -1};
}

Index MapPosition(Index pos, const Edit& edit) {
  if (pos <= edit.start_byte) return pos;
  if (pos >= edit.old_end_byte) return pos + edit.Delta();
  // Inside the changed region: keep the offset into it where the replacement
  // is long enough, otherwise land at its end.
  return std::min(pos, edit.new_end_byte);
}

Index MapPositionAfter(Index pos, const Edit& edit) {
  // Checked before the "at or before the start stays put" rule, which is the
  // whole difference: for a pure insertion start_byte == old_end_byte, and a
  // selection that begins exactly where its neighbour ended must move past the
  // bytes just laid down there rather than swallow them.
  if (pos >= edit.old_end_byte) return pos + edit.Delta();
  return MapPosition(pos, edit);
}

void SelectionSet::SetPrimary(std::size_t i) {
  if (i < ranges_.size()) primary_ = i;
}

void SelectionSet::Set(Selection s) {
  ranges_.assign(1, s);
  primary_ = 0;
}

void SelectionSet::Add(const PieceTable& table, Selection s) {
  ranges_.push_back(s);
  primary_ = ranges_.size() - 1;
  Normalize(table);
}

void SelectionSet::Replace(const PieceTable& table, std::vector<Selection> s) {
  if (s.empty()) {
    Set(Selection{});
    return;
  }
  ranges_ = std::move(s);
  if (primary_ >= ranges_.size()) primary_ = ranges_.size() - 1;
  Normalize(table);
}

void SelectionSet::KeepPrimaryOnly() {
  const Selection keep = ranges_[primary_];
  ranges_.assign(1, keep);
  primary_ = 0;
}

void SelectionSet::EnsureBlockCursors(const PieceTable& table) {
  bool widened = false;
  for (Selection& s : ranges_) {
    if (!s.IsEmpty()) continue;
    const Selection before = s;
    s = MinWidth1(table, s);
    widened = widened || !(s == before);
  }
  // Widening can make two cursors touch, so the merge has to run again -- but
  // only when something actually changed, since this is called on every key.
  if (widened) Normalize(table);
}

void SelectionSet::RotatePrimary(int delta) {
  const auto n = static_cast<std::ptrdiff_t>(ranges_.size());
  if (n <= 1) return;
  auto next = (static_cast<std::ptrdiff_t>(primary_) + delta) % n;
  if (next < 0) next += n;
  primary_ = static_cast<std::size_t>(next);
}

namespace {

// One edit of a batch restated so the whole batch composes into a single
// piecewise map: `start` and `old_end` in the coordinates the document had
// *before* the batch, `new_end` in the coordinates it has after all of it, and
// `delta_before` the length change of every edit that sits lower down. Entries
// are ascending in position whichever order the batch was applied in.
struct Rebased {
  Index start{0};
  Index old_end{0};
  Index new_end{0};
  Index delta_before{0};

  // At or above this, a position is past the edit outright and only shifts.
  // Not `old_end`: a pure insertion sitting exactly on a position leaves it
  // alone -- MapPosition tests `pos <= start` first, and for an insertion that
  // is the same byte as `old_end` -- so its threshold is one byte higher.
  Index Threshold() const { return std::max(old_end, start + 1); }
};

// A batch applied front to back, each edit in the coordinates the ones before
// it left behind: what a single Apply call emits, ascending and disjoint in
// that evolving document. Subtracting the running delta puts each edit back
// onto pre-batch coordinates; its own `new_end` is already final, since
// everything after it lies further along.
bool RebaseAscending(const std::vector<Edit>& edits, std::vector<Rebased>& out, Index& total_delta) {
  Index delta = 0;
  Index prev_new_end = std::numeric_limits<Index>::min();
  for (const Edit& e : edits) {
    if ((e.start_byte < prev_new_end) || (e.old_end_byte < e.start_byte) ||
        (e.new_end_byte < e.start_byte)) {
      return false;
    }
    out.push_back(Rebased{e.start_byte - delta, e.old_end_byte - delta, e.new_end_byte, delta});
    prev_new_end = e.new_end_byte;
    delta += e.Delta();
  }
  total_delta = delta;
  return true;
}

// A batch applied back to front -- one edit per cursor, highest first, which is
// what every line-wise command emits. Every edit is already in pre-batch
// coordinates there, because the ones applied before it all sat above it.
// Walked in reverse the list is ascending in position, and the running delta is
// what the lower, later-applied edits will add.
bool RebaseDescending(const std::vector<Edit>& edits, std::vector<Rebased>& out,
                      Index& total_delta) {
  Index delta = 0;
  Index prev_old_end = std::numeric_limits<Index>::min();
  bool prev_pure_insert = false;
  for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
    const Edit& e = *it;
    if ((e.start_byte < prev_old_end) || (e.old_end_byte < e.start_byte) ||
        (e.new_end_byte < e.start_byte)) {
      return false;
    }
    // A pure insertion touching the edit above it is the one shape where the
    // fold and the composed map part company: folding top down, a position that
    // the upper edit clamps or shifts onto exactly that byte then meets the
    // insertion's `start` and stops there, while the composed map has already
    // carried it past. Rare enough to hand back to the fold.
    if (prev_pure_insert && (e.start_byte == prev_old_end)) return false;
    out.push_back(Rebased{e.start_byte, e.old_end_byte, e.new_end_byte + delta, delta});
    prev_old_end = e.old_end_byte;
    prev_pure_insert = (e.start_byte == e.old_end_byte);
    delta += e.Delta();
  }
  total_delta = delta;
  return true;
}

// Identical, position for position, to folding MapPosition over the batch.
// Everything below the first edit the position does not clear is untouched by
// the rest, so one search settles it: before that edit's start it only picks up
// the shift from below, inside it collapses into the replacement.
Index MapRebased(Index pos, const std::vector<Rebased>& rebased, Index total_delta) {
  const auto it = std::ranges::partition_point(
      rebased, [pos](const Rebased& r) { return pos >= r.Threshold(); });
  if (it == rebased.end()) return pos + total_delta;
  if (pos <= it->start) return pos + it->delta_before;
  return std::min(pos + it->delta_before, it->new_end);
}

Index g_map_fallbacks = 0;

}  // namespace

Index MapThroughEditsFallbacks() { return g_map_fallbacks; }

// Normalized once at the end, not per edit: MapPosition is monotonic, so
// ranges that would have merged early still merge at the end -- one sort
// rather than N for a grouped undo's N edits.
//
// Folding edit by edit is O(edits x selections), and neither factor is bounded:
// `%` then `s <pattern>` leaves one cursor per line, and one edit per line is
// what a line-wise command over those cursors emits -- 32k of each froze the UI
// thread for three seconds. An ordered, disjoint batch composes into one
// monotone piecewise map, so restate it once and binary search that instead,
// keeping the fold for batches that do not compose.
void SelectionSet::MapThroughEdits(const PieceTable& table, const std::vector<Edit>& edits) {
  if (edits.empty()) {
    Normalize(table);
    return;
  }

  // thread_local and reused: this runs on every keystroke, and the batch is
  // bounded by the cursor count.
  static thread_local std::vector<Rebased> rebased;
  rebased.clear();
  rebased.reserve(edits.size());
  Index total_delta = 0;
  bool composed = RebaseAscending(edits, rebased, total_delta);
  if (!composed) {
    rebased.clear();
    composed = RebaseDescending(edits, rebased, total_delta);
  }

  if (composed) {
    for (Selection& s : ranges_) {
      s.anchor = MapRebased(s.anchor, rebased, total_delta);
      s.head = MapRebased(s.head, rebased, total_delta);
      s.goal_column = -1;
    }
  } else {
    ++g_map_fallbacks;
    for (const Edit& e : edits) {
      for (Selection& s : ranges_) {
        s.anchor = MapPosition(s.anchor, e);
        s.head = MapPosition(s.head, e);
        s.goal_column = -1;
      }
    }
  }
  Normalize(table);
}

void SelectionSet::Normalize(const PieceTable& table) {
  if (ranges_.empty()) {
    Set(Selection{});
    return;
  }
  // Track the primary by identity across the sort, not by index.
  const Selection primary = ranges_[std::min(primary_, ranges_.size() - 1)];

  for (Selection& s : ranges_) s = Snap(table, s);
  std::ranges::sort(ranges_, [](const Selection& a, const Selection& b) {
    return (a.From() != b.From()) ? (a.From() < b.From()) : (a.To() < b.To());
  });

  // Compacted in place: runs on every command, and the write index can never
  // outrun the read index, so no scratch allocation.
  std::size_t kept = 0;
  for (std::size_t i = 0; i < ranges_.size(); ++i) {
    const Selection s = ranges_[i];
    if (kept == 0) {
      ranges_[kept++] = s;
      continue;
    }
    Selection& last = ranges_[kept - 1];
    // Touching counts as overlapping for carets, so two cursors landing on the
    // same offset collapse rather than sitting invisibly on top of each other.
    const bool overlaps =
        (s.From() < last.To()) || ((s.From() == last.To()) && (s.IsEmpty() || last.IsEmpty()));
    if (!overlaps) {
      ranges_[kept++] = s;
      continue;
    }
    const Index lo = std::min(last.From(), s.From());
    const Index hi = std::max(last.To(), s.To());
    // Keep the later range's direction: it is the one the user just moved.
    const bool backwards = (s.head < s.anchor);
    last.anchor = backwards ? hi : lo;
    last.head = backwards ? lo : hi;
    last.goal_column = s.goal_column;
  }
  ranges_.resize(kept);
  primary_ = 0;
  for (std::size_t i = 0; i < ranges_.size(); ++i) {
    if ((ranges_[i].From() <= primary.From()) && (primary.To() <= ranges_[i].To())) {
      primary_ = i;
      break;
    }
  }
}

// -- movement ---------------------------------------------------------------

namespace {

Index LineContentEnd(const PieceTable& table, Index line) {
  const Interval content = LineContentRange(table, line);
  return content.empty() ? LineStart(table, line) : content.back() + 1;
}

// Word motions classify by grapheme cluster, not byte; everything non-ASCII
// is deliberately a word character. Line endings are their own category,
// exactly as in helix's categorize_char -- collapsing them into whitespace
// was measurably wrong.
enum class Cat : std::uint8_t { kSpace, kEol, kWord, kPunct };

// The first byte settles the category, which is what lets callers read one
// byte instead of gathering the cluster.
Cat CatOf(char first) {
  const auto c = static_cast<unsigned char>(first);
  if ((c == '\n') || (c == '\r')) return Cat::kEol;
  if (c >= 0x80) return Cat::kWord;
  if ((c == ' ') || (c == '\t') || (c == '\v') || (c == '\f')) return Cat::kSpace;
  if ((c == '_') || ((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'z')) ||
      ((c >= 'A') && (c <= 'Z'))) {
    return Cat::kWord;
  }
  return Cat::kPunct;
}

// A cluster the scan looks at: its category, and whether there was one at all.
// `exists == false` is helix's iterator returning None -- the document edge.
struct Cluster {
  Cat cat{Cat::kSpace};
  bool exists{false};
};

// Run once per grapheme travelled, per cursor. Gathering the cluster meant a
// string per grapheme; reading one byte is a memoised lookup.
Cluster ClusterAt(const PieceTable& table, Index pos) {
  const Index next = NextGraphemeBoundary(table, pos);
  if (next <= pos) return {};
  char first = 0;
  if (!ByteAt(table, pos, first)) return {};
  return {CatOf(first), true};
}

Cluster ClusterBefore(const PieceTable& table, Index pos) {
  const Index prev = PrevGraphemeBoundary(table, pos);
  if (prev >= pos) return {};
  char first = 0;
  if (!ByteAt(table, prev, first)) return {};
  return {CatOf(first), true};
}

Index FirstNonBlank(const PieceTable& table, Index line) {
  const Interval content = LineContentRange(table, line);
  if (content.empty()) return LineStart(table, line);
  // Byte at a time rather than copying the whole line for a handful of bytes.
  const Index stop = content.back() + 1;
  Index i = 0;
  char ch = 0;
  while (((content.front() + i) < stop) && ByteAt(table, content.front() + i, ch) &&
         ((ch == ' ') || (ch == '\t'))) {
    ++i;
  }
  return content.front() + i;
}

bool IsWordMotion(Motion motion) {
  switch (motion) {
    case Motion::kWordNext:
    case Motion::kWordPrev:
    case Motion::kWordEnd:
    case Motion::kWordPrevEnd:
    case Motion::kLongWordNext:
    case Motion::kLongWordPrev:
    case Motion::kLongWordEnd:
    case Motion::kLongWordPrevEnd:
      return true;
    default:
      return false;
  }
}

// Where a vertical motion lands: `target_row`, at the column the cursor is
// aiming for, with the row's start already in hand from the caller's descent.
//
// The goal column is read off the starting position when it is not already
// set, and survives even when the move is refused at the document's edge, so
// paging into the top of the file and back out keeps the column.
Index RowJump(const PieceTable& table, Index from, Index row_start, Index target_row,
              Index tab_width, Index& goal_column) {
  if (goal_column < 0) goal_column = ColumnForByteFrom(table, row_start, from, tab_width);
  if ((target_row < 0) || (target_row > LineCount(table) - 1)) return from;
  return ByteForColumn(table, target_row, goal_column, tab_width);
}

// One step of a non-word motion, measured from and returning a cursor position.
Index StepOnce(const PieceTable& table, Index from, Motion motion, Index tab_width,
               Index& goal_column) {
  const Index doc_len = DocLength(table);

  switch (motion) {
    case Motion::kLeft:
      return PrevGraphemeBoundary(table, from);
    case Motion::kRight:
      return NextGraphemeBoundary(table, from);
    case Motion::kUp:
    case Motion::kDown: {
      // Row and line start together: one descent for what took three walks.
      Index row = 0;
      Index row_start = 0;
      LineAtAndStart(table, from, row, row_start);
      return RowJump(table, from, row_start, (motion == Motion::kUp) ? row - 1 : row + 1, tab_width,
                     goal_column);
    }
    case Motion::kLineStart: {
      Index row = 0;
      Index row_start = 0;
      LineAtAndStart(table, from, row, row_start);
      return row_start;
    }
    case Motion::kLineFirstNonBlank:
      return FirstNonBlank(table, LineAt(table, from));
    case Motion::kLineEnd: {
      // The last character of the line, not the newline after it; `,` is
      // goto_line_end + move_char_right because both must be reachable.
      const Index line = LineAt(table, from);
      const Index start = LineStart(table, line);
      return std::max(start, PrevGraphemeBoundary(table, LineContentEnd(table, line)));
    }
    case Motion::kDocStart:
      return 0;
    case Motion::kDocEnd:
      return doc_len;
    case Motion::kLastLine: {
      // The start of the last line, not the end of the document: `gb` lands
      // on text you can see, and a trailing newline's empty line loses.
      Index line = std::max<Index>(0, LineCount(table) - 1);
      if ((line > 0) && LineRange(table, line).empty()) --line;
      return LineStart(table, line);
    }
    default:
      break;
  }
  return from;
}

// Faithful port of helix 25.07.1's word_move + range_to_target (movement.rs),
// verified against a live hx. Hand-derived rules diverged on non-word-aligned
// selections (repeated `b` grew instead of moving) -- do not re-derive.

bool IsBackwardWordMotion(Motion motion) {
  return (motion == Motion::kWordPrev) || (motion == Motion::kLongWordPrev) ||
         (motion == Motion::kWordPrevEnd) || (motion == Motion::kLongWordPrevEnd);
}

bool IsLongWordMotion(Motion motion) {
  return (motion == Motion::kLongWordNext) || (motion == Motion::kLongWordPrev) ||
         (motion == Motion::kLongWordEnd) || (motion == Motion::kLongWordPrevEnd);
}

// helix's is_word_boundary / is_long_word_boundary: any category change is a
// boundary, except that a WORD (long word) does not break between word and
// punctuation.
bool IsBoundary(Cat a, Cat b, bool long_word) {
  if (long_word && (((a == Cat::kWord) && (b == Cat::kPunct)) ||
                    ((a == Cat::kPunct) && (b == Cat::kWord)))) {
    return false;
  }
  return a != b;
}

// helix's reached_target. `prev` is the cluster the scan came from, `next` the
// one it is about to consume; going backward those roles are mirrored by the
// caller, exactly as helix mirrors its iterator.
bool ReachedTarget(Motion motion, Cat prev, Cat next) {
  const bool boundary = IsBoundary(prev, next, IsLongWordMotion(motion));
  const bool starts = (motion == Motion::kWordNext) || (motion == Motion::kLongWordNext) ||
                      (motion == Motion::kWordPrevEnd) || (motion == Motion::kLongWordPrevEnd);
  const bool next_ws = (next == Cat::kSpace) || (next == Cat::kEol);
  const bool prev_ws = (prev == Cat::kSpace) || (prev == Cat::kEol);
  if (starts) {
    // NextWordStart | PrevWordEnd: land where something readable begins.
    return boundary && ((next == Cat::kEol) || !next_ws);
  }
  // NextWordEnd | PrevWordStart: land where something readable ends.
  return boundary && (!prev_ws || (next == Cat::kEol));
}

struct WordRange {
  Index anchor;
  Index head;
};

// One helix range_to_target pass. Reaching the boundary without having moved
// re-anchors and keeps going -- the rule that makes `b` from a word's first
// byte select the *previous* word, and a second `b` move rather than grow.
WordRange RangeToTarget(const PieceTable& table, WordRange origin, Motion motion) {
  const bool backward = IsBackwardWordMotion(motion);
  Index anchor = origin.anchor;
  Index head = origin.head;

  const auto step = [&](Index p) {
    return backward ? PrevGraphemeBoundary(table, p) : NextGraphemeBoundary(table, p);
  };
  const auto ahead = [&](Index p) {
    return backward ? ClusterBefore(table, p) : ClusterAt(table, p);
  };

  // The cluster the scan starts on: behind the head for a forward scan, at it
  // for a backward one -- helix's `prev_ch`.
  Cluster prev = backward ? ClusterAt(table, head) : ClusterBefore(table, head);

  // Skip any initial line endings.
  while (true) {
    const Cluster next = ahead(head);
    if (!next.exists || (next.cat != Cat::kEol)) break;
    prev = next;
    head = step(head);
  }
  if (prev.exists && (prev.cat == Cat::kEol)) anchor = head;

  const Index head_start = head;
  while (true) {
    const Cluster next = ahead(head);
    if (!next.exists) break;
    if (!prev.exists || ReachedTarget(motion, prev.cat, next.cat)) {
      if (head == head_start) {
        anchor = head;
      } else {
        break;
      }
    }
    prev = next;
    head = step(head);
  }
  return {anchor, head};
}

// helix's word_move: the old anchor is discarded; the result spans what one
// range_to_target pass travelled. With a count, each pass starts where the
// last ended -- `3l` is the third word alone, not all three.
Selection WordMove(const PieceTable& table, Selection s, Motion motion, bool extend, Index count) {
  const bool backward = IsBackwardWordMotion(motion);
  const Index doc_len = DocLength(table);

  // helix's early-out is on the raw head, not the cursor: at the document's
  // edge the motion leaves the selection exactly as it was.
  if ((backward && (s.head <= 0)) || (!backward && (s.head >= doc_len))) return s;

  const Index cursor = CursorOf(table, s);
  WordRange r = backward ? WordRange{NextGraphemeBoundary(table, cursor), cursor}
                         : WordRange{cursor, NextGraphemeBoundary(table, cursor)};

  for (Index i = 0; i < count; ++i) {
    const WordRange next = RangeToTarget(table, r, motion);
    if ((next.anchor == r.anchor) && (next.head == r.head)) break;
    r = next;
  }

  if (extend) {
    // Only the landing spot matters when extending; helix takes the cursor of
    // the travelled range and puts it, keeping the anchor.
    const Index pos = (r.head > r.anchor) ? PrevGraphemeBoundary(table, r.head) : r.head;
    return PutCursor(table, s, pos, true);
  }
  s.anchor = r.anchor;
  s.head = r.head;
  return s;
}

}  // namespace

CharClass CharClassAt(const PieceTable& table, Index pos, bool long_word) {
  // The public three-way view: textobjects and friends treat a line ending as
  // whitespace; only the word motions' reached-target rules tell them apart.
  const Cluster cluster = ClusterAt(table, pos);
  switch (cluster.exists ? cluster.cat : Cat::kSpace) {
    case Cat::kWord:
      return CharClass::kWord;
    case Cat::kPunct:
      return long_word ? CharClass::kWord : CharClass::kPunct;
    default:
      return CharClass::kSpace;
  }
}

void Move(const PieceTable& table, SelectionSet& sel, Motion motion, bool extend, Index tab_width,
          Index count) {
  count = std::max<Index>(1, count);
  const Index doc_len = DocLength(table);
  const bool vertical = (motion == Motion::kUp) || (motion == Motion::kDown);

  for (Selection& s : sel.MutableRanges()) {
    if (IsWordMotion(motion)) {
      s = WordMove(table, s, motion, extend, count);
      s.goal_column = -1;
      continue;
    }

    Index goal = vertical ? s.goal_column : -1;
    Index pos = CursorOf(table, s);
    for (Index i = 0; i < count; ++i) {
      const Index next = StepOnce(table, pos, motion, tab_width, goal);
      if (next == pos) break;
      pos = next;
    }
    s = PutCursor(table, s, std::clamp<Index>(pos, 0, doc_len), extend);
    s.goal_column = goal;
  }
  sel.Normalize(table);
}

bool ClampCursorsToLines(const PieceTable& table, SelectionSet& sel, Index first, Index last,
                         Index tab_width) {
  const Index lo = std::min(first, last);
  const Index hi = std::max(first, last);
  const Index doc_len = DocLength(table);
  bool moved = false;

  for (Selection& s : sel.MutableRanges()) {
    const Index cursor = CursorOf(table, s);
    Index row = 0;
    Index row_start = 0;
    LineAtAndStart(table, cursor, row, row_start);
    const Index target = std::clamp(row, lo, hi);
    if (target == row) continue;

    // One hop however far away the row is: the arithmetic a counted `j` ends
    // up doing anyway, without walking the lines in between.
    Index goal = s.goal_column;
    const Index pos = RowJump(table, cursor, row_start, target, tab_width, goal);
    s = PutCursor(table, s, std::clamp<Index>(pos, 0, doc_len), false);
    s.goal_column = goal;
    moved = true;
  }

  // Normalized once, after every cursor has landed. Two that clamp onto the
  // same edge merge only because they now really are on the same grapheme --
  // per-cursor normalization would also merge ones that merely passed over
  // each other -- and a cursor that stayed put is left exactly as it was.
  if (moved) sel.Normalize(table);
  return moved;
}

// -- editing through the cursors --------------------------------------------

namespace {

// Runs `edit_one` at every selection in document order, in one undo group.
// Normalize guarantees sorted, disjoint, cluster-aligned ranges, so a running
// delta replaces per-edit tail mapping -- O(n) not O(n^2); measured at 512
// cursors the mapping alone was 0.6 ms of a 5.3 ms keypress.
CursorState SnapshotCursors(const SelectionSet& sel) {
  CursorState out;
  out.spans.reserve(sel.Size());
  for (const Selection& s : sel.Ranges()) out.spans.push_back(CursorSpan{s.anchor, s.head});
  out.primary = static_cast<std::uint32_t>(sel.PrimaryIndex());
  return out;
}

// Every cursor's edit as one transaction. `describe_one` says what a cursor
// wants changed against the document as it stands; the running total below is
// the only one in the system. Measured at 512 cursors: one transaction vs 512
// took a keystroke from 4.6 ms to 1.2 ms.
template <typename F>
ErrorCtx EditThroughCursors(PieceTable& table, SelectionSet& sel, F&& describe_one) {
  std::vector<Selection> ranges = sel.Ranges();
  std::vector<Change> changes;
  std::vector<Index> change_of(ranges.size(), -1);
  changes.reserve(ranges.size());

  Index last_to = 0;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    Change c{};
    bool changed = false;
    if (ErrorCtx err = describe_one(ranges[i], c, changed); err) return err;
    if (!changed) continue;
    // What cursors ask to delete may overlap (backspace at two cursors one
    // grapheme apart); the transaction wants disjoint, so trim, not refuse.
    if (c.from < last_to) c.from = last_to;
    if ((c.to < c.from) || ((c.from == c.to) && c.text.empty())) continue;
    change_of[i] = std::ssize(changes);
    changes.push_back(c);
    last_to = c.to;
  }
  if (changes.empty()) return Success();

  const CursorState before = SnapshotCursors(sel);
  NoteCursorsBefore(table, before);
  if (ErrorCtx err = Apply(table, changes, before, CursorState{}); err) return err;

  // A cursor that edited goes to the end of what it put there -- mapping
  // alone cannot place a cursor sitting exactly at its own insertion.
  Index shift = 0;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    if (change_of[i] < 0) {
      ranges[i].anchor += shift;
      ranges[i].head += shift;
    } else {
      const Change& c = changes[static_cast<std::size_t>(change_of[i])];
      const Index new_end = c.from + shift + std::ssize(c.text);
      shift += std::ssize(c.text) - (c.to - c.from);
      ranges[i].anchor = new_end;
      ranges[i].head = new_end;
    }
    // An edit invalidates every goal column, including those of the cursors it
    // only shifted -- the text under them is not what the column was aimed at.
    ranges[i].goal_column = -1;
  }

  sel.Replace(table, std::move(ranges));
  NoteCursorsAfter(table, SnapshotCursors(sel));
  return Success();
}

}  // namespace

ErrorCtx InsertAtCursors(std::string_view text, PieceTable& table, SelectionSet& sel) {
  if (text.empty()) return Success();
  return EditThroughCursors(table, sel, [&](const Selection& s, Change& c, bool& changed) {
    changed = true;
    c = s.IsEmpty() ? Change{s.head, s.head, text} : Change{s.From(), s.To(), text};
    return Success();
  });
}

ErrorCtx InsertAtCursorsKeeping(std::string_view text, PieceTable& table, SelectionSet& sel) {
  if (text.empty()) return Success();
  return EditThroughCursors(table, sel, [&](const Selection& s, Change& c, bool& changed) {
    changed = true;
    const Index at = CursorOf(table, s);
    c = Change{at, at, text};
    return Success();
  });
}

ErrorCtx DeleteSelections(PieceTable& table, SelectionSet& sel) {
  return EditThroughCursors(table, sel, [&](const Selection& s, Change& c, bool& changed) {
    if (s.IsEmpty()) return Success();
    changed = true;
    c = Change{s.From(), s.To(), std::string_view{}};
    return Success();
  });
}

ErrorCtx DeleteBackwardAtCursors(PieceTable& table, SelectionSet& sel) {
  return EditThroughCursors(table, sel, [&](const Selection& s, Change& c, bool& changed) {
    const Index from = s.From();
    const Index to = s.To();

    // A selection wider than exactly one grapheme is a real selection, and
    // backspace deletes it.
    if (to > NextGraphemeBoundary(table, from)) {
      changed = true;
      c = Change{from, to, std::string_view{}};
      return Success();
    }

    // Otherwise it is an insert-mode caret (from == to) or a normal-mode block
    // cursor (to == the next grapheme), and backspace deletes the character
    // *before* it in both cases.
    const Index prev = PrevGraphemeBoundary(table, from);
    if (prev >= from) return Success();  // start of document
    changed = true;
    c = Change{prev, from, std::string_view{}};
    return Success();
  });
}

ErrorCtx DeleteForwardAtCursors(PieceTable& table, SelectionSet& sel) {
  return EditThroughCursors(table, sel, [&](const Selection& s, Change& c, bool& changed) {
    if (!s.IsEmpty()) {
      changed = true;
      c = Change{s.From(), s.To(), std::string_view{}};
      return Success();
    }
    const Index next = NextGraphemeBoundary(table, s.head);
    if (next <= s.head) return Success();  // end of document
    changed = true;
    c = Change{s.head, next, std::string_view{}};
    return Success();
  });
}

}  // namespace koi
