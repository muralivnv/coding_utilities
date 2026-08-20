#ifndef KOI_SELECTION_H_
#define KOI_SELECTION_H_

#include <cstddef>
#include <string_view>
#include <vector>

#include "piece_doc.h"

namespace koi {

// The selection-first model helix and kakoune use: no separate cursor, only a
// range and a direction, both offsets on grapheme cluster boundaries. The
// head is *exclusive on the side it moved towards*: forward covers
// [anchor, head) with the block cursor on the grapheme before head; backward
// covers [head, anchor) with the cursor at head. In normal mode a selection
// is never narrower than one grapheme (that invariant is what lets `d` work
// with no operator); insert mode is the one place zero-width is legal.
struct Selection {
  Index anchor{0};
  Index head{0};

  // Display column to aim for when moving vertically, or -1: a short line
  // clamps the column on the way through, and coming back must undo that.
  Index goal_column{-1};

  Index From() const { return (anchor < head) ? anchor : head; }
  Index To() const { return (anchor < head) ? head : anchor; }
  bool IsEmpty() const { return anchor == head; }
  bool Backward() const { return head < anchor; }
  Interval Range() const { return Interval(From(), To()); }

  // Deliberately ignores goal_column: it is view state, not identity.
  friend bool operator==(const Selection& a, const Selection& b) {
    return (a.anchor == b.anchor) && (a.head == b.head);
  }
};

// Where the block cursor is drawn, and the position every motion measures from.
// Not the same as `head`: for a forward selection head is one past the cursor.
Index CursorOf(const PieceTable& table, const Selection& s);

// Widens a zero-width selection to one grapheme. At the document's end it is
// left empty rather than reaching backwards -- pulling the cursor back would
// make the end of the file unreachable.
Selection MinWidth1(const PieceTable& table, Selection s);

// Puts the cursor on the grapheme at `pos`. Without `extend`, the
// one-grapheme selection over it; with, the anchor is kept -- except on
// direction reversal, where it steps over its own grapheme to keep holding it.
Selection PutCursor(const PieceTable& table, Selection s, Index pos, bool extend);

// A selection over the byte range [from, to), widened to whole clusters. Regex
// matches arrive on *code point* boundaries, which inside a combining sequence,
// a ZWJ run or a regional indicator pair is not a *grapheme* boundary -- and
// Apply refuses every edit whose ends are not on one, so a match handed
// straight to the selection set is highlighted and then uneditable. Widened
// outwards rather than snapped: SelectionSet's Snap rounds both ends forwards,
// which walks the start past the match and can leave nothing selected at all.
Selection CoveringSelection(const PieceTable& table, Index from, Index to);

// Maps one offset through an edit. Positions before the change stay put,
// positions after it shift by the delta, and positions inside the replaced
// region collapse into what replaced it.
Index MapPosition(Index pos, const Edit& edit);

// The same, for a position known to lie *after* the edit -- a selection the
// caller has not reached yet while editing its siblings back to front. The two
// differ only where a pure insertion sits exactly on `pos`: MapPosition leaves
// it alone (the insert belongs to the selection being edited, which claims it
// explicitly), this moves it off (the insert is behind this selection).
Index MapPositionAfter(Index pos, const Edit& edit);

// How many MapThroughEdits calls have taken the per-edit fold rather than the
// binary-searched sweep. Both compute the same answer, so this counter is the
// only way a test can tell which one ran.
Index MapThroughEditsFallbacks();

// Every cursor in a buffer: sorted by start, non-overlapping, with one marked
// primary (the one the viewport follows).
class SelectionSet {
 public:
  SelectionSet() : ranges_{Selection{}} {}
  explicit SelectionSet(Selection s) : ranges_{s} {}

  const std::vector<Selection>& Ranges() const { return ranges_; }
  std::vector<Selection>& MutableRanges() { return ranges_; }
  std::size_t Size() const { return ranges_.size(); }
  const Selection& Primary() const { return ranges_[primary_]; }
  std::size_t PrimaryIndex() const { return primary_; }
  void SetPrimary(std::size_t i);

  void Set(Selection s);
  void Add(const PieceTable& table, Selection s);
  void Replace(const PieceTable& table, std::vector<Selection> s);
  void KeepPrimaryOnly();

  // Applies the never-empty invariant; called once per normal-mode keystroke.
  void EnsureBlockCursors(const PieceTable& table);

  // Steps which cursor is primary (the one the viewport follows).
  void RotatePrimary(int delta);

  // Shifts every range through a batch of edits, then snaps back onto cluster
  // boundaries -- the arithmetic can land mid-character when a cursor sat
  // inside the region that changed.
  void MapThroughEdits(const PieceTable& table, const std::vector<Edit>& edits);

  // Sorts, clamps, snaps to cluster boundaries and merges overlaps, keeping
  // the primary pointing at whatever absorbed it.
  void Normalize(const PieceTable& table);

 private:
  std::vector<Selection> ranges_;
  std::size_t primary_{0};
};

// How the word motions classify a grapheme. Exposed because the `mi w` / `ma w`
// text objects have to agree with `l` and `j` about where a word ends.
enum class CharClass { kSpace, kWord, kPunct };

// `long_word` folds punctuation into word, which is what makes a WORD stop only
// at whitespace.
CharClass CharClassAt(const PieceTable& table, Index pos, bool long_word = false);

// -- movement ---------------------------------------------------------------

enum class Motion {
  kLeft,
  kRight,
  kUp,
  kDown,
  kLineStart,       // first byte of the line
  kLineFirstNonBlank,
  kLineEnd,         // end of the line's content, before any terminator
  kDocStart,
  kDocEnd,
  kLastLine,        // start of the last line, not the end of the document
  kWordPrev,        // start of the word before the cursor
  kWordNext,        // start of the word after the cursor
  kWordEnd,         // end of the word the cursor is in or before
  kWordPrevEnd,     // end of the word before the cursor
  kLongWordPrev,    // helix's WORD: delimited by whitespace only
  kLongWordNext,
  kLongWordEnd,
  kLongWordPrevEnd,
};

// Moves every cursor `count` times; word motions select what they travelled.
// The count is applied inside the motion, not by repeating the command: `5l`
// must be one step for undo and the goal column, and a motion stopping at a
// boundary must stop once, not five times.
void Move(const PieceTable& table, SelectionSet& sel, Motion motion, bool extend, Index tab_width,
          Index count = 1);

// Pulls every cursor onto the closed line range [first, last] -- each one that
// is outside it to the nearer edge, each one already inside left alone -- and
// says whether anything moved. Vertical like `j` and `k`, so a cursor keeps
// its goal column and a selection it has to move collapses onto the grapheme
// it lands on. Scrolling is the caller: the viewport is what defines the
// range, and every cursor owes it a position, not just the primary.
bool ClampCursorsToLines(const PieceTable& table, SelectionSet& sel, Index first, Index last,
                         Index tab_width);

// -- editing through the cursors --------------------------------------------
// Each is one undo step however many cursors there are. The acting cursor is
// placed explicitly afterwards: mapping alone cannot say whether a cursor at
// an insertion point belongs before or after the inserted text.

// Replaces any non-empty selection, inserts at any caret.
ErrorCtx InsertAtCursors(std::string_view text, PieceTable& table, SelectionSet& sel);

// Inserts at each block cursor and leaves the selected text alone -- what
// typing in insert mode does, and `ret`/`tab` in normal mode.
ErrorCtx InsertAtCursorsKeeping(std::string_view text, PieceTable& table, SelectionSet& sel);

// Deletes every non-empty selection. Carets are left alone.
ErrorCtx DeleteSelections(PieceTable& table, SelectionSet& sel);

// Backspace: deletes the selection where there is a real one (wider than one
// grapheme -- the one-grapheme case IS the cursor, which backspaces behind
// itself; treating it as a selection made backspace act like delete).
ErrorCtx DeleteBackwardAtCursors(PieceTable& table, SelectionSet& sel);

// Delete-forward: the selection, otherwise the cluster after the caret.
ErrorCtx DeleteForwardAtCursors(PieceTable& table, SelectionSet& sel);

}  // namespace koi

#endif  // KOI_SELECTION_H_
