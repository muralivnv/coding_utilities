#include "piece_doc.h"

#include <algorithm>
#include <atomic>
#include <chrono>

// piece_doc.h deliberately does not include this -- unicode.h includes
// piece_doc.h, and the cycle would be the wrong way round. The .cpp may.
#include "unicode.h"

namespace koi {
namespace {

// Relaxed: the only thing anyone asks of these is which of two is larger, and
// nothing is published through them. Atomic at all because a document may be
// edited off the main thread, and two threads sharing a counter that is not is
// a data race whatever the reads are for.
std::uint64_t NextEditSeq() {
  static std::atomic<std::uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t NowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// A point in a tree that is not necessarily the live one.
Point PointIn(const pt::Tree& tree, const pt::TextSource& src, Index at) {
  Point p;
  pt::PointAt(tree, at, src, p.row, p.column);
  return p;
}

// Where `p` ends up after `text` is inserted there. Pure arithmetic: no tree
// lookup on the insert-only path multi-cursor typing takes.
Point Advance(Point p, std::string_view text) {
  const std::size_t last = text.rfind('\n');
  if (last == std::string_view::npos) return Point{p.row, p.column + std::ssize(text)};
  return Point{p.row + pt::CountNewlines(text),
               std::ssize(text) - static_cast<Index>(last) - 1};
}

// Where `base` ends up after the whole of `t` has been passed over, from the
// tree's own aggregates.
Point Extend(Point base, const pt::Tree& t, const pt::TextSource& src) {
  const Index rows = t.Newlines();
  if (rows == 0) return Point{base.row, base.column + t.Bytes()};
  return Point{base.row + rows, t.Bytes() - pt::LineStart(t, rows, src)};
}

Point EndPointOf(const pt::Tree& t, const pt::TextSource& src) {
  return Extend(Point{}, t, src);
}

// One revision per *edit*, not per transaction: consumers replay
// `journal[r - journal_base]` per revision they are behind, which only works
// while the counters move together -- a multi-cursor keystroke is one
// transaction carrying N edits. `revision` is otherwise only compared for
// equality (it is the memo key), so counting edits costs the memos nothing.
void PushJournal(PieceTable& table, std::span<const Edit> edits) {
  table.revision += std::ssize(edits);
  for (const Edit& e : edits) table.journal.push_back(e);
  TrimJournal(table);
}

// An edit's inverse is the same edit seen from the other document: the old
// and new fields swap.
Edit Inverted(const Edit& e) {
  Edit inv;
  inv.start_byte = e.start_byte;
  inv.old_end_byte = e.new_end_byte;
  inv.new_end_byte = e.old_end_byte;
  inv.start_point = e.start_point;
  inv.old_end_point = e.new_end_point;
  inv.new_end_point = e.old_end_point;
  return inv;
}

// A run of typing at one place stays one undo step. Every guard below traces
// to a specific defect or measurement; change them against the tests.
bool ShouldCoalesce(const PieceTable& table, std::span<const Change> changes) {
  if (!table.allow_coalesce) return false;
  // group_started, not group_depth: every keystroke arrives wrapped in its
  // own group, and testing depth meant typing never coalesced at all.
  if (table.group_started) return false;
  // `<= 0` on purpose, unlike the guards that only ask whether `current` names
  // a revision: the base is parentless, so folding into it would bake the new
  // text into a step nothing can undo out of.
  if (table.current <= 0) return false;
  const Revision& top = table.revisions[static_cast<std::size_t>(table.current)];
  // Never fold across the save point: "buffer matches disk" compares serials,
  // and a merged step would blur the one it compares against.
  if (top.serial == table.saved_serial) return false;
  if (top.forward.empty()) return false;
  if ((NowMs() - top.stamp_ms) > 700) return false;

  // Typing: every cursor inserting where it left off, matched cursor for
  // cursor against the previous round -- the last `n` edits of the step.
  const std::size_t n = changes.size();
  if (n <= top.forward.size()) {
    const Edit* prev_round = top.forward.data() + (top.forward.size() - n);
    bool typing = true;
    for (std::size_t i = 0; (i < n) && typing; ++i) {
      const Change& c = changes[i];
      const Edit& p = prev_round[i];
      typing = (c.from == c.to) && !c.text.empty() &&      // this round inserts
               (p.old_end_byte == p.start_byte) &&          // so did the last
               (p.new_end_byte == c.from);                  // right where it ended
    }
    if (typing) return true;
  }

  // Deleting: a run eating into the same place. Kept to one cursor on
  // purpose -- several deletions merging is harder to be sure of.
  if (n == 1) {
    const Change& c = changes[0];
    const Edit& prev = top.forward.back();
    if (!c.text.empty() || (c.from == c.to)) return false;
    if (prev.new_end_byte != prev.start_byte) return false;
    return (c.to == prev.start_byte) || (c.from == prev.start_byte);
  }
  return false;
}

}  // namespace

void PieceTable::Clear() {
  owned_nodes = 0;
  nodes_at_load = 0;
  tree = pt::Tree{};
  original.clear();
  modified.clear();
  original_owner.reset();
  original_mapped = nullptr;
  original_mapped_size = 0;
  ResetHistory(*this);
  partial_group_edits = 0;
  ++revision;
  journal.clear();
  journal_base = revision;
}

std::string_view OriginalText(const PieceTable& table) {
  if (table.original_mapped != nullptr) {
    return std::string_view{table.original_mapped,
                            static_cast<std::size_t>(table.original_mapped_size)};
  }
  return table.original;
}

pt::TextSource SourceOf(const PieceTable& table) {
  return pt::TextSource{OriginalText(table), table.modified};
}

// -- loading ------------------------------------------------------------------

namespace {

void ResetCommon(PieceTable& table) {
  const Index pool_before = pt::LiveNodeCount();
  table.modified.clear();
  // Cleared before the new tree is built so the pool can hand the old
  // document's nodes straight back.
  table.revisions.clear();
  table.tree = pt::BuildFromText(OriginalText(table), true);
  ResetHistory(table);
  // A different document: anything derived from the old text must be rebuilt,
  // not patched, so the journal restarts.
  ++table.revision;
  table.journal.clear();
  table.journal_base = table.revision;
  table.owned_nodes += pt::LiveNodeCount() - pool_before;
  table.nodes_at_load = table.owned_nodes;
}

}  // namespace

void ResetToOriginal(PieceTable& table, std::string text) {
  table.original_owner.reset();
  table.original_mapped = nullptr;
  table.original_mapped_size = 0;
  table.original = std::move(text);
  ResetCommon(table);
}

void ResetToMapped(PieceTable& table, std::string_view text, std::shared_ptr<void> owner) {
  table.original.clear();
  table.original_owner = std::move(owner);
  table.original_mapped = text.data();
  table.original_mapped_size = std::ssize(text);
  ResetCommon(table);
}

void MaterializeOriginal(PieceTable& table) {
  if (table.original_mapped == nullptr) return;
  // Byte-for-byte identical, so pieces and line offsets all still hold: they
  // address bytes by offset, not by address. `read_memo.text` is the one
  // exception -- a raw pointer into the bytes that are about to move -- and
  // dropping the mapping below frees the page it points at. Surviving that is
  // the same use-after-free DocumentMemos already guards on copy and move,
  // except here `revision` cannot catch it: materializing happens before the
  // mutation that would bump it, so a stale memo still looks live.
  table.original.assign(table.original_mapped,
                        static_cast<std::size_t>(table.original_mapped_size));
  table.read_memo = PieceTable::ReadMemo{};
  table.original_mapped = nullptr;
  table.original_mapped_size = 0;
  table.original_owner.reset();
}

// -- editing ------------------------------------------------------------------

ErrorCtx Apply(PieceTable& table, std::span<const Change> changes,
               const CursorState& cursors_before, const CursorState& cursors_after,
               std::vector<Edit>* out_edits) {
  if (out_edits != nullptr) out_edits->clear();
  if (changes.empty()) return Success();

  MaterializeOriginal(table);

  // Every check before every mutation: a refused change must leave the
  // document exactly as it was. This is also the one place that holds the
  // line on character integrity -- every split the tree performs is driven by
  // a position validated here.
  const Index doc_len = DocLength(table);
  Index last = 0;
  for (const Change& c : changes) {
    if ((c.from < last) || (c.to < c.from) || (c.to > doc_len) || (c.from < 0)) {
      return MakeErrorCtx(PieceTableErrorCode::kOutOfBoundsDocPos);
    }
    if (!c.text.empty() && !IsWellFormedUtf8(c.text)) {
      return MakeErrorCtx(PieceTableErrorCode::kMalformedUtf8Input);
    }
    // Only positions strictly inside the document can land mid-cluster; the
    // ends are boundaries by definition.
    for (const Index at : {c.from, c.to}) {
      if ((at > 0) && (at < doc_len) && !IsGraphemeBoundary(table, at)) {
        return MakeErrorCtx(PieceTableErrorCode::kDocPosNotOnGraphemeBoundary);
      }
    }
    last = c.to;
  }

  // Past validation: nothing below returns early, so the charge at the end is
  // reached on every path that can change the tree.
  const Index pool_before = pt::LiveNodeCount();

  const pt::Tree before_tree = table.tree;
  const bool coalescing = ShouldCoalesce(table, changes);

  std::vector<Edit> forward;
  forward.reserve(changes.size());

  // All insertion text is appended before the sweep: growing `modified` moves
  // it, so a TextSource taken before an append would dangle by the next one.
  // thread_local and reused: runs on every keystroke, bounded by cursor count.
  static thread_local std::vector<Index> src_starts;
  src_starts.clear();
  src_starts.reserve(changes.size());
  for (const Change& c : changes) {
    src_starts.push_back(std::ssize(table.modified));
    table.modified.append(c.text);
  }
  const pt::TextSource src = SourceOf(table);

  // The sweep consumes the tree exactly once, left to right (one Replace per
  // change would re-descend from the root N times). It also makes the edit
  // descriptors fall out instead of being looked up: `result` *is* the
  // document each change sees, and what a change removed is `gone`, in hand.
  pt::Tree result;
  pt::Tree rest = table.tree;
  Index consumed = 0;
  static thread_local std::vector<pt::Piece> pieces;
  for (std::size_t i = 0; i < changes.size(); ++i) {
    const Change& c = changes[i];

    pt::Tree head;
    pt::Tree tail;
    pt::Split(rest, c.from - consumed, src, head, tail);
    pt::Tree gone;
    pt::Tree after;
    pt::Split(tail, c.to - c.from, src, gone, after);

    result = pt::Concat(result, head);

    Edit fwd;
    fwd.start_byte = result.Bytes();
    fwd.start_point = EndPointOf(result, src);
    fwd.old_end_byte = fwd.start_byte + gone.Bytes();
    fwd.old_end_point = Extend(fwd.start_point, gone, src);
    fwd.new_end_byte = fwd.start_byte + std::ssize(c.text);
    fwd.new_end_point = Advance(fwd.start_point, c.text);
    forward.push_back(fwd);

    pt::PiecesFor(src_starts[i], c.text, false, pieces);
    result = pt::Concat(result, pt::Build(pieces));
    rest = after;
    consumed = c.to;
  }
  const pt::Tree tree = pt::Concat(result, rest);

  // Both reported before either branch below takes ownership of the list --
  // doing this inside the new-revision branch left a folded step never
  // writing the caller's edit at all, handing it a stale one.
  if (out_edits != nullptr) *out_edits = forward;

  table.tree = tree;
  PushJournal(table, forward);

  // `current > 0` for the same reason ShouldCoalesce refuses it: the base is
  // parentless and its `forward` is cleared, so an edit folded there is one
  // undo steps straight over -- baked into the document with no way back. The
  // group gate needed the rule too. A trim that fires mid-group re-roots the
  // chain and can leave `current` standing on the base (it may drop everything
  // below the step we are on, and that step then *becomes* the base); the next
  // transaction in the group would fold into it. Refusing here starts a new
  // revision parented on the base instead, which is undoable -- the group is
  // split, but the half the trim already baked in was unrecoverable anyway.
  const bool extending_group =
      (table.group_depth > 0) && table.group_started && (table.current > 0);

  // The note describes the cursors as they were before *this* transaction, so
  // this transaction spends it -- whether it fills a new revision below or
  // folds into an existing one, which already carries the note taken for it.
  // Left set (ResetHistory was the only thing that ever cleared it) it was
  // re-read by every later edit that passes no cursors of its own -- the
  // Insert/Delete/Replace wrappers -- so those revisions were stamped with a
  // snapshot from some earlier, unrelated command, and undo restored a
  // selection the user was never at, possibly off the end of a document that
  // had since shrunk.
  CursorState noted = std::move(table.pending_before);
  table.pending_before = CursorState{};

  if (extending_group || coalescing) {
    // Fold into the newest step. Folding is *appending*, not rewriting into a
    // union: the step restores its document by holding a tree, and for the
    // journal a sequence of edits is exactly as replayable as a merged one.
    Revision& top = table.revisions[static_cast<std::size_t>(table.current)];
    top.tree = tree;
    top.cursors_after = cursors_after;
    top.forward.insert(top.forward.end(), forward.begin(), forward.end());
    top.stamp_ms = NowMs();
    top.stamp_seq = NextEditSeq();
    if (table.group_depth > 0) table.group_started = true;
  } else {
    Revision rev;
    rev.tree = tree;
    // A caller that noted the cursors separately wins over the argument,
    // which for multi-call edits is whatever the last call happened to see.
    if (cursors_before.spans.empty()) {
      rev.cursors_before = std::move(noted);
    } else {
      rev.cursors_before = cursors_before;
    }
    rev.cursors_after = cursors_after;
    rev.forward = std::move(forward);
    rev.parent = table.current;
    rev.serial = table.serial_next++;
    rev.stamp_ms = NowMs();
    rev.stamp_seq = NextEditSeq();

    table.revisions.push_back(std::move(rev));
    const Index at = std::ssize(table.revisions) - 1;
    // A new edit after an undo starts a branch; the old one is kept rather
    // than discarded, so nothing is ever lost by editing after undoing.
    table.revisions[static_cast<std::size_t>(table.current)].last_child = at;
    table.current = at;
    table.allow_coalesce = true;
    if (table.group_depth > 0) table.group_started = true;
  }

  // Charged before trimming, which does its own accounting: the two would
  // otherwise both cover the same frees and count them twice.
  table.owned_nodes += pt::LiveNodeCount() - pool_before;
  TrimHistory(table);
  return Success();
}

ErrorCtx Insert(std::string_view s, Index at, PieceTable& table, Edit* out_edit) {
  if (s.empty()) return MakeErrorCtx(PieceTableErrorCode::kEmptyInputString);
  const Change c{at, at, s};
  std::vector<Edit> edits;
  const ErrorCtx err = Apply(table, std::span{&c, 1}, CursorState{}, CursorState{}, &edits);
  if (!err && (out_edit != nullptr) && !edits.empty()) *out_edit = edits[0];
  return err;
}

ErrorCtx Delete(Index from, Index to, PieceTable& table, Edit* out_edit) {
  const Change c{from, to, std::string_view{}};
  std::vector<Edit> edits;
  const ErrorCtx err = Apply(table, std::span{&c, 1}, CursorState{}, CursorState{}, &edits);
  if (!err && (out_edit != nullptr) && !edits.empty()) *out_edit = edits[0];
  return err;
}

ErrorCtx Replace(std::string_view s, Index from, Index to, PieceTable& table, Edit* out_edit) {
  const Change c{from, to, s};
  std::vector<Edit> edits;
  const ErrorCtx err = Apply(table, std::span{&c, 1}, CursorState{}, CursorState{}, &edits);
  if (!err && (out_edit != nullptr) && !edits.empty()) *out_edit = edits[0];
  return err;
}

ErrorCtx Delete(Interval range, PieceTable& table, Edit* out_edit) {
  if (range.empty()) return Success();
  return Delete(range.front(), range.back() + 1, table, out_edit);
}

ErrorCtx Replace(std::string_view s, Interval range, PieceTable& table, Edit* out_edit) {
  // Degenerate cases stay out: empty replacement is Delete's job, empty range
  // is Insert's.
  if (s.empty()) return MakeErrorCtx(PieceTableErrorCode::kEmptyInputString);
  if (range.empty()) return MakeErrorCtx(PieceTableErrorCode::kMismatchInputStringAndDocRange);
  const Index from = range.front();
  const Index doc_len = DocLength(table);
  if ((from < 0) || ((doc_len > 0) && (from >= doc_len))) {
    return MakeErrorCtx(PieceTableErrorCode::kOutOfBoundsDocPos);
  }
  // A range running off the end replaces to the end rather than being refused
  // -- `s/.../.../` on the last line wants that.
  return Replace(s, from, std::min(range.back() + 1, doc_len), table, out_edit);
}

void NoteCursorsBefore(PieceTable& table, CursorState cursors) {
  // Only the outermost note of a step counts: the inner calls of a grouped
  // edit see cursors that have already moved.
  if ((table.group_depth > 0) && table.group_started) return;
  table.pending_before = std::move(cursors);
}

// A note is taken speculatively -- before running something that may or may not
// edit -- so whoever took it has to take it back when nothing did. Left behind,
// the next edit from any path that passes no cursors of its own inherits it and
// undo restores a position the user only passed through.
void DropCursorNote(PieceTable& table) { table.pending_before = CursorState{}; }

// "No revision to stamp" is the out-of-range test CurrentUndoSerial and
// CanRedo use, not `current <= 0`: the store is empty only before the first
// load, and there `current` is 0 with nothing to index, which the upper bound
// already catches. Index 0 is a real revision the user can be standing on --
// after TrimHistory re-roots the chain it is some edit far from the load --
// and treating it as nothing dropped the stamp meant for it.
void NoteCursorsAfter(PieceTable& table, CursorState cursors) {
  if ((table.current < 0) || (table.current >= std::ssize(table.revisions))) return;
  table.revisions[static_cast<std::size_t>(table.current)].cursors_after = std::move(cursors);
}

bool CanUndo(const PieceTable& table) {
  return (table.current > 0) && (table.current < std::ssize(table.revisions));
}

bool CanRedo(const PieceTable& table) {
  if ((table.current < 0) || (table.current >= std::ssize(table.revisions))) return false;
  return table.revisions[static_cast<std::size_t>(table.current)].last_child >= 0;
}

ErrorCtx Undo(PieceTable& table, CursorState* out_cursors, std::vector<Edit>* out_edits) {
  if (out_edits != nullptr) out_edits->clear();
  if (!CanUndo(table)) return Success();
  MaterializeOriginal(table);

  const Revision& leaving = table.revisions[static_cast<std::size_t>(table.current)];
  const Index parent = leaving.parent;

  // Restoring a document is assigning a root; nothing replays, so nothing can
  // replay wrongly.
  table.tree = table.revisions[static_cast<std::size_t>(parent)].tree;
  if (out_cursors != nullptr) *out_cursors = leaving.cursors_before;
  // Undoing runs against the document being left from the top down, so the
  // inverses come out newest-first.
  std::vector<Edit> inverse;
  inverse.reserve(leaving.forward.size());
  for (auto it = leaving.forward.rbegin(); it != leaving.forward.rend(); ++it) {
    inverse.push_back(Inverted(*it));
  }
  PushJournal(table, inverse);
  if (out_edits != nullptr) *out_edits = std::move(inverse);

  table.current = parent;
  // Typing after an undo must not fold into the step just undone.
  table.allow_coalesce = false;
  return Success();
}

ErrorCtx Redo(PieceTable& table, CursorState* out_cursors, std::vector<Edit>* out_edits) {
  if (out_edits != nullptr) out_edits->clear();
  if (!CanRedo(table)) return Success();
  MaterializeOriginal(table);

  const Index child = table.revisions[static_cast<std::size_t>(table.current)].last_child;
  const Revision& entering = table.revisions[static_cast<std::size_t>(child)];

  table.tree = entering.tree;
  if (out_cursors != nullptr) *out_cursors = entering.cursors_after;
  if (out_edits != nullptr) *out_edits = entering.forward;
  PushJournal(table, entering.forward);

  table.current = child;
  table.allow_coalesce = false;
  return Success();
}

// `current` indexes the store; only a *serial* says which document state we
// are on. revisions[0].serial is 0 for a freshly-loaded document, but after
// TrimHistory re-roots the chain the base carries the serial of the edit that
// made it -- answering 0 there says "matches disk" about a state that does not.
Index CurrentUndoSerial(const PieceTable& table) {
  if ((table.current < 0) || (table.current >= std::ssize(table.revisions))) return 0;
  return table.revisions[static_cast<std::size_t>(table.current)].serial;
}

void MarkUndoSavePoint(PieceTable& table) {
  table.saved_serial = CurrentUndoSerial(table);
  table.allow_coalesce = false;
}

// Serials grow child-ward, so the walk stops the moment it steps below the
// target. Running out *above* it means history trimming re-rooted the chain
// past the revision: its edit is baked into everything still reachable.
bool SerialApplied(const PieceTable& table, Index serial) {
  Index at = table.current;
  while ((at >= 0) && (at < std::ssize(table.revisions))) {
    const Revision& rev = table.revisions[static_cast<std::size_t>(at)];
    if (rev.serial == serial) return true;
    if (rev.serial < serial) return false;
    at = rev.parent;
  }
  return true;
}

void BreakUndoCoalescing(PieceTable& table) { table.allow_coalesce = false; }

// -- reading ------------------------------------------------------------------

Index DocLength(const PieceTable& table) { return table.tree.Bytes(); }

Index LineCount(const PieceTable& table) { return table.tree.Newlines() + 1; }

Index LineAt(const PieceTable& table, Index at) {
  return pt::LineAt(table.tree, at, SourceOf(table));
}

Index LineStart(const PieceTable& table, Index line) {
  // Clamp before anything else, not on the way out: pt::LineStart clamps its
  // own answer, but the memo below stores whatever `line` says, and a negative
  // one poisons it -- the next in-range query walks line-(-n) lines forward
  // from the memo and confidently answers with another line's offset.
  line = std::max<Index>(0, line);
  // Both budgets were four times larger until measured: a walk that gives up
  // still gets paid for before the descent does, so the walk must *obviously*
  // win. Tightening both to 8 took unindent at 512 cursors down 12% and
  // indent down 8-11%, with consecutive-line walks (what the memo is really
  // for) untouched. Do not raise these without re-measuring scattered-cursor
  // benchmarks.
  constexpr Index kMaxWalkLines = 8;
  // The walk also loses when lines are near but shredded into fragments;
  // AdvanceLines returns -1 rather than crawling, and the descent answers.
  constexpr Index kMaxWalkPieces = 8;

  PieceTable::LineMemo& memo = table.line_memo;
  Index at = -1;
  if ((memo.revision == table.revision) && (line >= memo.line) &&
      ((line - memo.line) <= kMaxWalkLines)) {
    at = (line == memo.line) ? memo.offset
                             : pt::AdvanceLines(table.tree, memo.offset, line - memo.line,
                                                SourceOf(table), kMaxWalkPieces);
  }
  if (at < 0) at = pt::LineStart(table.tree, line, SourceOf(table));
  memo = PieceTable::LineMemo{table.revision, line, at};
  return at;
}

void LineAtAndStart(const PieceTable& table, Index at, Index& line, Index& line_start) {
  Index column = 0;
  pt::PointAt(table.tree, at, SourceOf(table), line, column);
  line_start = at - column;
  // Leave the memo here: the next question is almost always the line after,
  // which becomes a one-line walk instead of a descent.
  table.line_memo = PieceTable::LineMemo{table.revision, line, line_start};
}

Point PointAt(const PieceTable& table, Index at) {
  return PointIn(table.tree, SourceOf(table), at);
}

Interval LineRange(const PieceTable& table, Index line) {
  if ((line < 0) || (line >= LineCount(table))) return Interval(0, 0);
  const Index start = LineStart(table, line);
  const Index stop = (line + 1 < LineCount(table)) ? LineStart(table, line + 1) : DocLength(table);
  return Interval(start, stop);
}

Index GetNlinesInDocRange(Interval range, const PieceTable& table) {
  if (range.empty()) return 0;
  const Index lo = std::max<Index>(0, range.front());
  const Index hi = std::min<Index>(DocLength(table), range.back() + 1);
  if (lo >= hi) return 0;
  return LineAt(table, hi) - LineAt(table, lo);
}

namespace {

// Fills the memo with the piece holding `at`. Every read goes through here,
// so there is one place that descends.
bool LocateFor(const PieceTable& table, Index at) {
  const PieceTable::ReadMemo& memo = table.read_memo;
  if ((memo.revision == table.revision) && (at >= memo.start) && (at < memo.end)) return true;
  Index start = 0;
  const pt::Piece* p = pt::FindPiece(table.tree, at, start);
  if (p == nullptr) return false;
  const std::string_view text = SourceOf(table).Of(*p);
  table.read_memo = PieceTable::ReadMemo{table.revision, start, start + std::ssize(text),
                                         text.data()};
  return true;
}

}  // namespace

bool ByteAt(const PieceTable& table, Index at, char& out) {
  if ((at < 0) || (at >= DocLength(table)) || !LocateFor(table, at)) return false;
  out = table.read_memo.text[at - table.read_memo.start];
  return true;
}

bool BytePairAt(const PieceTable& table, Index at, char& before, char& here) {
  if ((at - 1 < 0) || (at >= DocLength(table)) || !LocateFor(table, at)) return false;
  const PieceTable::ReadMemo& memo = table.read_memo;
  here = memo.text[at - memo.start];
  if (at - 1 >= memo.start) {
    before = memo.text[at - 1 - memo.start];
    return true;
  }
  // Piece boundary. Deliberately does not overwrite the memo with the earlier
  // piece: the next question will be about `at` again, not what precedes it.
  char c = 0;
  if (!pt::ByteAt(table.tree, at - 1, SourceOf(table), c)) return false;
  before = c;
  return true;
}


ErrorCtx Undo(PieceTable& table, std::vector<Edit>* out_edits) {
  return Undo(table, nullptr, out_edits);
}

ErrorCtx Redo(PieceTable& table, std::vector<Edit>* out_edits) {
  return Redo(table, nullptr, out_edits);
}

Index HistoryBytes(const PieceTable& table) {
  const Index added = table.owned_nodes - table.nodes_at_load;
  return (added > 0) ? (added * pt::ApproxNodeBytes()) : 0;
}

void TrimJournal(PieceTable& table) {
  if (table.journal.size() <= kMaxJournalEntries) return;
  const auto drop = table.journal.size() - kMaxJournalEntries;
  table.journal.erase(table.journal.begin(),
                      table.journal.begin() + static_cast<std::ptrdiff_t>(drop));
  table.journal_base += static_cast<Index>(drop);
}

void TrimHistory(PieceTable& table) {
  if (HistoryBytes(table) <= table.history_budget_bytes) return;
  if (std::ssize(table.revisions) <= kMinRevisionsKept) return;
  const Index pool_before = pt::LiveNodeCount();
  const auto charge = [&table, pool_before] {
    table.owned_nodes += pt::LiveNodeCount() - pool_before;
  };

  // The chain worth keeping, oldest first: ancestors, here, redo branch.
  // Anything off that path was abandoned by editing after an undo and is
  // already unreachable by pressing undo and redo.
  std::vector<Index> chain;
  for (Index at = table.current; at >= 0;) {
    chain.push_back(at);
    at = table.revisions[static_cast<std::size_t>(at)].parent;
  }
  std::ranges::reverse(chain);
  const std::size_t here = chain.size() - 1;
  for (Index at = table.revisions[static_cast<std::size_t>(table.current)].last_child; at >= 0;) {
    chain.push_back(at);
    at = table.revisions[static_cast<std::size_t>(at)].last_child;
  }

  // A quarter at a time, not exactly-enough-to-fit: structural sharing makes
  // any one step's real cost unknowable, and a fixed fraction converges
  // geometrically instead of re-trimming on the next keystroke.
  std::size_t drop = std::max<std::size_t>(1, chain.size() / 4);
  drop = std::min(drop, here);  // never drop the step we are standing on
  const Index floor_excess = std::ssize(chain) - kMinRevisionsKept;
  drop = (floor_excess > 0) ? std::min(drop, static_cast<std::size_t>(floor_excess))
                            : std::size_t{0};
  // The floor forbids dropping a *step*; it never forbids compacting the
  // *store*. A branch abandoned by editing after an undo is unreachable by any
  // key, and the rebuild below is the only thing that frees one -- bailing here
  // on a short chain let an undo-then-retype rhythm grow `revisions` without
  // bound while the budget stayed blown. Bail only when the store already holds
  // nothing but the chain.
  if ((drop == 0) && (chain.size() == table.revisions.size())) {
    charge();
    return;
  }

  // Rebuild, renumbering; the oldest kept step becomes the parentless base.
  std::vector<Revision> kept;
  kept.reserve(chain.size() - drop);
  for (std::size_t i = drop; i < chain.size(); ++i) {
    Revision r = std::move(table.revisions[static_cast<std::size_t>(chain[i])]);
    r.parent = (i == drop) ? -1 : static_cast<Index>(i - drop - 1);
    r.last_child = ((i + 1) < chain.size()) ? static_cast<Index>(i - drop + 1) : -1;
    kept.push_back(std::move(r));
  }
  // The base is a state, not a step: nothing may undo *into* the edit that
  // made it, so the edits describing that edit go with its parent. With
  // `drop == 0` the kept base *is* the old base, whose forward is already
  // empty -- clearing it there would throw away nothing, but it would also be
  // claiming a re-rooting that did not happen.
  if (drop > 0) kept.front().forward.clear();

  table.revisions = std::move(kept);
  table.current = static_cast<Index>(here - drop);
  // A pure compaction drops only what no key can reach, so it must be
  // otherwise invisible: breaking coalescing there would turn every abandoned
  // branch into an extra step, which is the growth this function is fighting.
  if (drop > 0) table.allow_coalesce = false;
  charge();
}

Index UndoDepth(const PieceTable& table) {
  Index depth = 0;
  Index at = table.current;
  while ((at > 0) && (at < std::ssize(table.revisions))) {
    at = table.revisions[static_cast<std::size_t>(at)].parent;
    ++depth;
  }
  return depth;
}

void ResetHistory(PieceTable& table) {
  Revision root;
  root.tree = table.tree;
  table.revisions.clear();
  table.revisions.push_back(std::move(root));
  table.current = 0;
  table.serial_next = 1;
  table.saved_serial = 0;
  table.allow_coalesce = true;
  table.group_started = false;
  table.pending_before = CursorState{};
}

Index PieceCount(const PieceTable& table) {
  Index n = 0;
  for (pt::Cursor cur{table.tree, 0}; cur.Valid(); ) {
    ++n;
    if (!cur.Next()) break;
  }
  return n;
}

}  // namespace koi
