#ifndef KOI_PIECE_DOC_H_
#define KOI_PIECE_DOC_H_

#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "error.h"
#include "piece_tree.h"

namespace koi {

using pt::Index;

using Interval = std::ranges::iota_view<Index, Index>;

// What one mutation did, in the shape ts_tree_edit expects.
struct Point {
  Index row{0};
  Index column{0};
  bool operator==(const Point&) const = default;
};

struct Edit {
  Index start_byte{0};
  Index old_end_byte{0};
  Index new_end_byte{0};
  Point start_point{};
  Point old_end_point{};
  Point new_end_point{};

  Index Delta() const { return new_end_byte - old_end_byte; }
  bool IsEmpty() const { return (start_byte == old_end_byte) && (old_end_byte == new_end_byte); }
};

// Cursors as history sees them: plain offsets, deliberately not SelectionSet,
// so piece_doc.h never has to know selection.h exists.
struct CursorSpan {
  Index anchor{0};
  Index head{0};
};

struct CursorState {
  std::vector<CursorSpan> spans;
  std::uint32_t primary{0};
};

// One edit to the document, however many places it touches. `from`/`to` are
// in the coordinates of the document *as it stands before the transaction*,
// and the changes must be sorted and disjoint -- callers never do position
// arithmetic; the single sweep inside Apply is the only code that shifts.
struct Change {
  Index from{0};
  Index to{0};  // exclusive; from == to is an insertion
  std::string_view text;
};

// One entry in the history tree. Holds a whole document, not a delta: the
// tree is persistent, so a revision costs only the root-to-leaf path its edit
// copied (~1.5 KB) and restoring one cannot drift.
struct Revision {
  pt::Tree tree;
  CursorState cursors_before;  // where the cursors were before the edit that made this
  CursorState cursors_after;
  // Only the tree-sitter journal consumes these. Inverses are not stored: an
  // edit's inverse is its old/new fields swapped, so Undo derives them.
  std::vector<Edit> forward;
  Index parent{-1};
  Index last_child{-1};  // the redo branch
  Index serial{0};
  std::uint64_t stamp_ms{0};
};

// The two read caches, in a base of their own so copying or moving a table
// drops them. `read_memo.text` can point into the table's own SSO string
// bytes, so a memo surviving a move is a genuine use-after-free (confirmed
// under ASan when the buffers vector reallocated). Handling it here keeps
// PieceTable's copy/move defaulted and unable to get it wrong.
struct DocumentMemos {
  // The piece the last byte read came from. Reads are overwhelmingly local,
  // so this turns most descents into a bounds check; `revision` bumps on
  // every mutation, so a stale memo can never be mistaken for a live one.
  struct ReadMemo {
    Index revision{-1};
    Index start{0};
    Index end{0};
    const char* text{nullptr};
  };
  mutable ReadMemo read_memo{};

  // Where the last "which offset does line N start at?" landed. Line lookups
  // come in runs; resuming makes the next line a memchr from the last answer.
  struct LineMemo {
    Index revision{-1};
    Index line{0};
    Index offset{0};
  };
  mutable LineMemo line_memo{};

  DocumentMemos() = default;
  DocumentMemos(const DocumentMemos&) noexcept {}
  DocumentMemos(DocumentMemos&&) noexcept {}
  DocumentMemos& operator=(const DocumentMemos&) noexcept { return Drop(); }
  DocumentMemos& operator=(DocumentMemos&&) noexcept { return Drop(); }

 private:
  DocumentMemos& Drop() noexcept {
    read_memo = ReadMemo{};
    line_memo = LineMemo{};
    return *this;
  }
};

struct PieceTable : DocumentMemos {
  pt::Tree tree;

  // `modified` is append-only and never compacted: that is what keeps every
  // historical tree readable.
  std::string original;
  std::string modified;

  // The original may instead be a read-only mapping -- see
  // MaterializeOriginal, which every mutator calls first, so a mapping and an
  // unsaved edit never coexist.
  std::shared_ptr<void> original_owner;
  const char* original_mapped{nullptr};
  Index original_mapped_size{0};

  // -- history: revisions[0] is the document as loaded and has no parent.
  std::vector<Revision> revisions;
  Index current{0};
  Index serial_next{1};
  Index saved_serial{0};
  // A transaction opened while this is set coalesces into the newest revision
  // instead of adding one, when the two are adjacent and close in time.
  bool allow_coalesce{true};

  // There is deliberately no freeze flag here. An excerpt view was once
  // read-only at this layer; it is now editable by design, writing back
  // through a transaction, and a gate no path ever closed only made the header
  // promise something the code did not do. Read-only-ness that a *user* can
  // see lives on Document::read_only.

  // Open UndoGroup scopes, and whether one has already started a revision
  // that later transactions in the same scope should extend.
  Index group_depth{0};
  bool group_started{false};
  // Times an edit was refused after an earlier edit in the same undo group had
  // already been written. That is always a bug in the caller, never a state the
  // editor should reach: the group is half applied, and whatever the command
  // meant to do to the cursors afterwards did not happen. Counted rather than
  // asserted so every test and fuzz that checks EditorInvariants catches the
  // whole class, not just the three loops that were known to have it.
  Index partial_group_edits{0};
  // Cursors before the edit now being assembled; consumed by the next
  // transaction Apply writes -- which either stamps a new revision with it or,
  // if it folds into an existing revision, drops it. A note is never carried
  // past the edit it was taken for: the edit after it records its own cursors,
  // or none, and undo derives those from the edits.
  CursorState pending_before;

  // History budget. Unbounded history is a session-long climb -- measured:
  // 2000 keystrokes at 64 cursors retained 222 MB, at 500 cursors 1.8 GB.
  // Oldest steps are dropped until the estimate fits. Per file, not per
  // editor: opening a second buffer must not shorten this one's undo.
  Index history_budget_bytes{64 * 1024 * 1024};

  // Nodes this document's operations created, net of freed. The pool is
  // global and shared structurally, so ownership has no answer -- but the
  // *delta* across one table's operation is unambiguously that table's.
  Index owned_nodes{0};
  // owned_nodes after load: the text's own cost, never charged as history.
  Index nodes_at_load{0};

  // Edit journal, for consumers that patch derived state instead of
  // rebuilding -- tree-sitter, measured 0.35 ms incremental vs 21 ms fresh.
  Index revision{0};
  Index journal_base{0};
  std::vector<Edit> journal;

  void Clear();
};

// The two buffers, as the tree wants them. Never hold one across an append:
// growing `modified` moves it, and a view taken beforehand dangles.
pt::TextSource SourceOf(const PieceTable& table);

std::string_view OriginalText(const PieceTable& table);

// -- loading ------------------------------------------------------------------
void ResetToOriginal(PieceTable& table, std::string text);
void ResetToMapped(PieceTable& table, std::string_view text, std::shared_ptr<void> owner);
void MaterializeOriginal(PieceTable& table);

// -- editing ------------------------------------------------------------------

// Applies one transaction as a single undoable step. `cursors_before` is what
// undo restores, `cursors_after` what redo restores; neither is interpreted.
ErrorCtx Apply(PieceTable& table, std::span<const Change> changes,
               const CursorState& cursors_before, const CursorState& cursors_after,
               std::vector<Edit>* out_edits = nullptr);

// Convenience wrappers, each a one-change transaction.
ErrorCtx Insert(std::string_view s, Index at, PieceTable& table, Edit* out_edit = nullptr);
ErrorCtx Delete(Index from, Index to, PieceTable& table, Edit* out_edit = nullptr);
ErrorCtx Replace(std::string_view s, Index from, Index to, PieceTable& table,
                 Edit* out_edit = nullptr);
ErrorCtx Delete(Interval range, PieceTable& table, Edit* out_edit = nullptr);
ErrorCtx Replace(std::string_view s, Interval range, PieceTable& table, Edit* out_edit = nullptr);

// Collects everything recorded during its lifetime into one undo step: the
// first transaction in the scope creates the revision, the rest extend it.
class UndoGroup {
 public:
  explicit UndoGroup(PieceTable& table) : table_{table} {
    if (table_.group_depth == 0) table_.group_started = false;
    ++table_.group_depth;
  }
  ~UndoGroup() {
    if (--table_.group_depth == 0) table_.group_started = false;
  }
  UndoGroup(const UndoGroup&) = delete;
  UndoGroup& operator=(const UndoGroup&) = delete;

 private:
  PieceTable& table_;
};

// Kept separate from Apply so callers that edit through several calls record
// the ends of the whole step, not of whichever call happened to be last.
void NoteCursorsBefore(PieceTable& table, CursorState cursors);
void NoteCursorsAfter(PieceTable& table, CursorState cursors);

// Withdraws a note nothing spent. A caller that notes before running something
// that may not edit -- a command batch, a pending-char resolution -- must drop
// the note when the run is over, or the next edit anywhere inherits it.
void DropCursorNote(PieceTable& table);

// Moves to the parent revision (Undo) or down the redo branch (Redo). Both
// restore recorded cursors rather than reconstructing them from the edits.
// `out_edits` is for the journal only.
ErrorCtx Undo(PieceTable& table, CursorState* out_cursors = nullptr,
              std::vector<Edit>* out_edits = nullptr);
ErrorCtx Redo(PieceTable& table, CursorState* out_cursors = nullptr,
              std::vector<Edit>* out_edits = nullptr);

// Compatibility with callers that only want the journal edits.
ErrorCtx Undo(PieceTable& table, std::vector<Edit>* out_edits);
ErrorCtx Redo(PieceTable& table, std::vector<Edit>* out_edits);

bool CanUndo(const PieceTable& table);
bool CanRedo(const PieceTable& table);

// Depth of *this* branch (walks parents), not the size of the revision store.
Index UndoDepth(const PieceTable& table);

// Roughly what the undo history is holding, in bytes.
Index HistoryBytes(const PieceTable& table);

// Drops the oldest steps until the history fits its budget. Called after
// every edit; exposed so tests can drive it directly.
void TrimHistory(PieceTable& table);

// Never trimmed below this: one edit big enough to blow the budget on its own
// must still be undoable.
inline constexpr Index kMinRevisionsKept = 16;

// The journal is what consumers replay to patch derived state instead of
// rebuilding it (tree-sitter, stashed window selections). Left unbounded it is
// a session-long climb of its own -- 72 bytes per Edit, one Edit per cursor per
// keystroke, so 20k keystrokes at 64 cursors is 88 MB. Past this many entries
// the oldest are dropped and the consumers that fall behind take the rebuild
// path they already have for a base that has moved past them.
inline constexpr std::size_t kMaxJournalEntries = 1 << 16;  // ~4.7 MB

// Drops the oldest journal entries, moving `journal_base` up to match. Called
// after every append; exposed so tests can drive it directly.
// Keeps `journal_base + std::ssize(journal) == revision`.
void TrimJournal(PieceTable& table);

// Drops all history, keeping the document. The single place the history
// fields reset -- Clear and the reload path call it rather than drifting.
void ResetHistory(PieceTable& table);

// Diagnostic only; nothing about behaviour depends on it.
Index PieceCount(const PieceTable& table);

// The serial of the boundary the document sits on; 0 is "as loaded". A saver
// remembers it to answer "is the buffer back at its saved state".
Index CurrentUndoSerial(const PieceTable& table);
void MarkUndoSavePoint(PieceTable& table);

// Whether the revision bearing `serial` is part of the document as it stands
// -- on the path from the root to the current revision. Undoing across it
// makes this false; redoing back makes it true again; a branch abandoned by
// editing after an undo can never make it true again. Consumers that version
// state alongside the history (the excerpt model) key on it.
bool SerialApplied(const PieceTable& table, Index serial);

// Ends the current coalescing run. Mode changes and cursor moves call it.
void BreakUndoCoalescing(PieceTable& table);

// -- reading ------------------------------------------------------------------
Index DocLength(const PieceTable& table);
Index LineCount(const PieceTable& table);
Index LineAt(const PieceTable& table, Index at);
Index LineStart(const PieceTable& table, Index line);

// The line containing `at` and where it begins, in one descent instead of two.
void LineAtAndStart(const PieceTable& table, Index at, Index& line, Index& line_start);
Point PointAt(const PieceTable& table, Index at);

// Byte range of a 0-based line, including its terminating newline. Empty when
// out of range and for a trailing empty final line -- anything wanting a
// line's *position* must ask LineStart instead.
Interval LineRange(const PieceTable& table, Index line);

Index GetNlinesInDocRange(Interval range, const PieceTable& table);

// Byte reads are spelled ReadDocRange / ReadDocRangeInto (unicode.h); those
// clamp to the document. There is deliberately no unclamped pair beside them.
bool ByteAt(const PieceTable& table, Index at, char& out);

// The bytes at `at - 1` and `at`: the grapheme fast path's question, and the
// hottest read in the editor.
bool BytePairAt(const PieceTable& table, Index at, char& before, char& here);

}  // namespace koi

#endif  // KOI_PIECE_DOC_H_
