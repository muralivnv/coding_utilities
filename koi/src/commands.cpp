#include "commands.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <deque>
#include <limits>
#include <optional>
#include <span>

#include "indent.h"
#include "jumplist.h"
#include "keylog.h"
#include "keymap.h"
#include "navigate.h"
#include "render.h"
#include "search.h"
#include "syntax.h"
#include "textobject.h"
#include "theme.h"
#include "selection.h"
#include "shell.h"
#include "unicode.h"

namespace koi {
namespace {

void DoMove(Editor& ed, Motion motion, bool extend, Index count) {
  Move(ed.doc.table, ed.doc.selections, motion, extend, ed.doc.tab_width, count);

  if ((ed.mode == Mode::kInsert) && !extend) ed.collapse_insert_caret = true;
}

void DoMove(Editor& ed, Motion motion, bool extend) {
  DoMove(ed, motion, extend, CountOr(ed, 1));
}

// Runs one edit and reports whether it was accepted, marking the buffer dirty
// only when something actually changed.
//
// `op` runs *inside* rather than being handed its result, because "did
// anything land?" is a before/after comparison and the call site cannot take
// the snapshot itself: in `Edited(ed, op(...))` the argument is evaluated
// first. PieceTable::revision counts applied edits and Apply is the only thing
// that moves it, so an operation that returns success without moving it
// changed nothing -- every edit path has such a shape (backspace at offset 0,
// delete forward at the end of the file, `d` on an empty selection, anything
// at all on an empty document; EditThroughCursors returns Success early when
// no cursor described a change, and Delete(Interval) does the same for an
// empty range).
//
// Marking one of those modified stuck the flag on: no revision exists, so
// UndoOrRedo -- the only code that recomputes `modified` from the undo serial
// -- has nothing to undo and returns before it can clear it. One backspace in
// the empty scratch buffer stranded a `[no name]+` that :q refused forever.
template <typename F>
bool Edited(Editor& ed, F&& op) {
  const Index revision_before = ed.doc.table.revision;
  const ErrorCtx err = op();
  if (err) {
    // Refused after an earlier edit in this group already landed: the group is
    // half applied. See PieceTable::partial_group_edits.
    if (ed.doc.table.group_started) ++ed.doc.table.partial_group_edits;
    ed.status.Fail(FormatErrorCtx(err));
    return false;
  }
  // Accepted, but nothing to record: success, and the buffer stays as clean as
  // it was. The read-only warning belongs below this too -- a no-op is not an
  // edit anyone needs warning about.
  if (ed.doc.table.revision == revision_before) return true;
  if (ed.doc.read_only && !ed.doc.modified) {
    ed.status.Warn(DisplayPath(ed.doc.file) +
                   " is not writable -- edits stay here until :w <path>");
  }
  ed.doc.modified = true;
  if (IsExcerptView(ed.doc)) DropUnreachableEpochs(ed.doc);
  return true;
}

// Where the cursors stand right now, for whatever edit runs next -- the dozen
// commands that edit through the Insert/Delete/Replace wrappers pass no cursors
// of their own and undo restores this instead.
//
// Taken immediately before the thing that might edit, and dropped again the
// moment that thing is over: a note is one-shot, spent by the first transaction
// after it, and one taken for a run that turned out not to edit would be
// collected by some unrelated later edit.
void NoteCursorsForNextEdit(Editor& ed) {
  CursorState before;
  for (const Selection& s : ed.doc.selections.Ranges()) {
    before.spans.push_back(CursorSpan{s.anchor, s.head});
  }
  NoteCursorsBefore(ed.doc.table, std::move(before));
}

void SpanKeepingOrientation(Selection& s, Index lo, Index hi) {
  const bool backwards = s.Backward();
  s.anchor = backwards ? hi : lo;
  s.head = backwards ? lo : hi;
}

// Where a selection goes after the edit it made.
//
// The two modes want different answers and only one of them was ever written.
// Normal mode holds a block cursor, so selecting what was just pasted is the
// point of `p`. Insert mode holds an *empty* caret, and CursorOf reads a
// forward selection as the grapheme behind its head -- so leaving a span there
// draws the caret one grapheme short of the paste and puts the next typed
// character inside it. InsertAtCursorsKeeping already lands empty on
// `new_end_byte` for exactly this reason (selection.cpp, EditThroughCursors);
// this is the same rule for the paste commands.
void LandOnEdit(const Editor& ed, Selection& s, const Edit& edit) {
  s.goal_column = -1;
  if (ed.mode == Mode::kInsert) {
    s.anchor = edit.new_end_byte;
    s.head = edit.new_end_byte;
    return;
  }
  s.anchor = edit.start_byte;
  s.head = edit.new_end_byte;
}

void VerticalPage(Editor& ed, bool down, Index rows, bool extend) {
  const Index total = std::max<Index>(1, rows) * CountOr(ed, 1);
  Move(ed.doc.table, ed.doc.selections, down ? Motion::kDown : Motion::kUp, extend,
       ed.doc.tab_width, total);
}

void MapLaterRanges(std::vector<Selection>& ranges, size_t edited, const Edit& edit) {
  for (size_t j = edited + 1; j < ranges.size(); ++j) {
    ranges[j].anchor = MapPositionAfter(ranges[j].anchor, edit);
    ranges[j].head = MapPositionAfter(ranges[j].head, edit);
  }
}

// Applies one change per selection as a single transaction.
//
// Every "edit each selection in turn, back to front" loop shared one hole. Apply
// validates a change against the document in front of it, and the *previous*
// iteration is what can move the next position inside a grapheme: replacement
// text beginning with a combining mark does exactly that to a selection whose
// end touches its neighbour's start. The loop then returned on the refusal with
// the earlier edits already written, `modified` already true and the pre-edit
// selections still installed -- a state EditorInvariants rejects. Handing Apply
// the whole list removes the possibility instead of the symptom: it validates
// everything against the pre-edit document, so the command lands on every
// selection or on none, and no iteration can invalidate a position for the next.
//
// `changes` must be in increasing position order and non-overlapping, and
// `owner[k]` names the index in `ranges` that `changes[k]` came from. Note that
// Change::text is a string_view: callers own the text and must keep it alive
// and unmoved across this call.
bool ApplyPerSelection(Editor& ed, std::vector<Selection>& ranges, std::span<const Change> changes,
                       std::span<const std::size_t> owner, bool keep_orientation) {
  if (changes.empty()) return false;

  CursorState before;
  before.primary = static_cast<std::uint32_t>(ed.doc.selections.PrimaryIndex());
  for (const Selection& s : ranges) before.spans.push_back(CursorSpan{s.anchor, s.head});

  std::vector<Edit> edits;
  if (!Edited(ed, [&] { return Apply(ed.doc.table, changes, before, CursorState{}, &edits); })) {
    return false;
  }

  // A selection that was edited lands on its own edit; one that was skipped just
  // shifts by whatever the edits before it added or removed.
  Index shift = 0;
  std::size_t k = 0;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    if ((k < owner.size()) && (owner[k] == i)) {
      if (keep_orientation) {
        SpanKeepingOrientation(ranges[i], edits[k].start_byte, edits[k].new_end_byte);
      } else {
        ranges[i].anchor = edits[k].start_byte;
        ranges[i].head = edits[k].new_end_byte;
      }
      ranges[i].goal_column = -1;
      shift = edits[k].new_end_byte - changes[k].to;
      ++k;
    } else {
      ranges[i].anchor += shift;
      ranges[i].head += shift;
    }
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
  return true;
}

Index LineContentEnd(const PieceTable& table, Index line) {
  const Interval content = LineContentRange(table, line);
  return content.empty() ? LineStart(table, line) : content.back() + 1;
}

Index LineStartOrDocEnd(const PieceTable& table, Index line) {
  return (line >= LineCount(table)) ? DocLength(table) : LineStart(table, line);
}

std::pair<Index, Index> LineSpan(const PieceTable& table, const Selection& s) {
  const Index first = LineAt(table, s.From());
  const Index last = LineAt(table, (s.To() > s.From()) ? s.To() - 1 : s.From());
  return {first, last};
}

void SelectWholeLines(Editor& ed, bool grow) {
  const Index count = CountOr(ed, 1);
  auto ranges = ed.doc.selections.Ranges();
  for (Selection& s : ranges) {
    const auto [first, last] = LineSpan(ed.doc.table, s);
    const bool upward = s.Backward();

    Index start = 0;
    Index end = 0;
    if (upward) {
      end = LineStartOrDocEnd(ed.doc.table, last + 1);
      start = LineStart(ed.doc.table, std::max<Index>(0, first - (count - 1)));
      if (grow && (s.From() == start) && (s.To() == end)) {
        start = LineStart(ed.doc.table, std::max<Index>(0, first - count));
      }
    } else {
      start = LineStart(ed.doc.table, first);
      end = LineStartOrDocEnd(ed.doc.table, last + count);
      if (grow && (s.From() == start) && (s.To() == end)) {
        end = LineStartOrDocEnd(ed.doc.table, last + count + 1);
      }
    }

    s.anchor = upward ? end : start;
    s.head = upward ? start : end;
    s.goal_column = -1;
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
}

void CollapseToCursor(Editor& ed) {
  SetCursors(ed, [&](const Selection& s) { return CursorOf(ed.doc.table, s); });
}

void EnterInsertAt(Editor& ed, bool at_end) {
  SetCursors(ed, [&](const Selection& s) { return at_end ? s.To() : s.From(); });
  ed.mode = Mode::kInsert;
}

void LeaveInsertMode(Editor& ed) { ed.mode = Mode::kNormal; }

struct NewlineIndent {
  std::string indent;
  std::string closing_line;
};

bool ColonOpensBlock(std::string_view language) {
  return (language == "python") || (language == "yaml");
}

char CloserFor(char opener) {
  switch (opener) {
    case '{': return '}';
    case '[': return ']';
    case '(': return ')';
    default: return '\0';
  }
}

// The indent koi has always computed: the current line's leading whitespace,
// plus a unit when the part of the line left of the caret opened a bracket that
// the syntax tree agrees is a bracket and not text. It knows only about the line
// it is on, which is why the tree engine sits in front of it -- but it is also
// the answer for every language with no `indents.scm`, for a buffer with no
// syntax at all, and for a query that could not be run, so it stays exactly as
// it was.
NewlineIndent BracketIndentForNewline(Editor& ed, Index cursor) {
  const PieceTable& table = ed.doc.table;
  const Index line = LineAt(table, cursor);
  const Index start = LineStart(table, line);
  const Index prefix_end = std::max(start, std::min(cursor, LineContentEnd(table, line)));

  const std::string prefix = ReadDocRange(table, Interval(start, prefix_end));

  std::size_t lead = 0;
  while ((lead < prefix.size()) && ((prefix[lead] == ' ') || (prefix[lead] == '\t'))) ++lead;

  NewlineIndent out;
  out.indent.assign(prefix, 0, lead);

  std::string open_stack;
  char last_code = '\0';
  for (std::size_t i = lead; i < prefix.size(); ++i) {
    const char c = prefix[i];
    if ((c == ' ') || (c == '\t') || (c == '\r')) continue;
    const bool opens = (c == '{') || (c == '[') || (c == '(');
    const bool closes = (c == '}') || (c == ']') || (c == ')');
    if ((opens || closes) && (ed.doc.syntax != nullptr) &&
        ed.doc.syntax->InLiteralOrComment(start + static_cast<Index>(i))) {
      continue;
    }
    last_code = c;
    if (opens) {
      open_stack.push_back(c);
    } else if (closes && !open_stack.empty()) {
      open_stack.pop_back();
    }
  }

  const bool deeper = !open_stack.empty() ||
                      ((last_code == ':') && (ed.doc.syntax != nullptr) &&
                       ColonOpensBlock(ed.doc.syntax->Language()));
  if (!deeper) return out;

  const std::string base = out.indent;
  out.indent += ed.doc.insert_spaces ? std::string(static_cast<size_t>(ed.doc.tab_width), ' ')
                                     : std::string("\t");

  if (!open_stack.empty() && (cursor < DocLength(table))) {
    char next = 0;
    if (ByteAt(table, cursor, next) && (next == CloserFor(open_stack.back()))) {
      out.closing_line = "\n" + base;
    }
  }
  return out;
}

// What is left of a command's indent deadline, floored at zero and in the whole
// milliseconds TreeIndentFor* is paid in. Truncating rather than rounding costs
// one caret under a millisecond of its share and can only ever end the run
// early, which is the direction a bound is allowed to be wrong in. Measured from
// one absolute deadline per command, so it does not accumulate: each caret is
// short by less than a millisecond of the same fixed end, not of the last
// caret's.
std::chrono::milliseconds RemainingIndentBudget(std::chrono::steady_clock::time_point deadline) {
  const auto left = deadline - std::chrono::steady_clock::now();
  if (left <= std::chrono::steady_clock::duration::zero()) return std::chrono::milliseconds::zero();
  return std::chrono::duration_cast<std::chrono::milliseconds>(left);
}

// The tree's answer where there is one, today's heuristic where there is not.
//
// Only the two indent *strings* change hands: whether a closer belongs on a line
// of its own is still auto-pairs' question, answered by the bracket scan above,
// and the tree only says which column it lands in. The two cannot disagree about
// placement that way, and a language with an `indents.scm` that says nothing
// about the construct being typed degrades to the same shape it had before.
//
// Caret sitting between an auto-paired `{` and `}` is the one case where the
// tree's answer is the *closer's* line and not the caret's: the `}` has not
// moved yet, so it still begins the line the newline opens and its `@outdent`
// counts against it. That is the right column for the brace, and the caret goes
// one level inside it -- which is the same shape the bracket heuristic builds,
// and the same one helix builds for a pair it is splitting.
//
// Unless the answer is an `@align`, where it is a column and not a depth: the
// argument after `foo(a,` lines up under `a` whether or not the `)` came along
// for the ride, so pushing the caret a level past it would put it where nothing
// else in that list sits. The closer takes the same column -- which is what the
// tree answers for the `)` line on its own, and what keeps a later keystroke on
// that line from moving a brace this code just placed.
// `deadline` is the whole keystroke's, not this cursor's: what this cursor may
// spend is whatever is left of it when it is reached. A cursor that finds it
// spent keeps the bracket heuristic's answer, which is already computed on the
// line above -- so the fallback for a spent budget costs nothing and is the same
// answer a language without an `indents.scm` gets.
NewlineIndent IndentForNewline(Editor& ed, Index cursor, std::string& error,
                               std::chrono::steady_clock::time_point deadline) {
  NewlineIndent out = BracketIndentForNewline(ed, cursor);
  if (ed.doc.syntax == nullptr) return out;

  const std::chrono::milliseconds left = RemainingIndentBudget(deadline);
  if (left <= std::chrono::milliseconds::zero()) return out;

  const IndentStyle style{ed.doc.tab_width, ed.doc.insert_spaces};
  bool aligned = false;
  const std::optional<std::string> tree =
      TreeIndentForNewline(ed.doc.table, *ed.doc.syntax, cursor, style, error, left, &aligned);
  if (!tree.has_value()) return out;

  if (out.closing_line.empty()) {
    out.indent = *tree;
  } else {
    out.closing_line = "\n" + *tree;
    out.indent = aligned ? *tree : IndentedOnce(*tree, style);
  }
  return out;
}

// What one Enter has to say about the indent query, said at most once per
// buffer. See IndentWarning for why the repeat is dropped rather than shown:
// nothing but a broken `indents.scm` gets this far, and it breaks the same way
// on every keystroke.
void WarnAboutIndentOnce(Editor& ed, const std::string& error) {
  if (error.empty()) {
    ed.indent_warned = IndentWarning{};
    return;
  }
  if ((ed.indent_warned.document == ed.doc.id) && (ed.indent_warned.message == error)) return;
  ed.indent_warned = IndentWarning{ed.doc.id, error};
  ed.status.Warn(error);
}

void InsertNewlineAutoIndent(Editor& ed) {
  if (ed.doc.syntax != nullptr) ed.doc.syntax->Sync(ed.doc.table);

  UndoGroup group(ed.doc.table);
  auto ranges = ed.doc.selections.Ranges();

  // The first message any cursor produced, shown once. A query that will not
  // compile fails identically for every cursor -- the language and the query
  // file belong to the buffer, not to the caret -- and an Enter with sixty
  // carets in it must not be sixty status messages, nor sixty Enters be sixty of
  // them, which is what WarnAboutIndentOnce below is for.
  //
  // First and not last on purpose. Only indent.cpp's Prepare writes into the
  // string it is handed, and it never clears one, so keeping the last write
  // would come to the same thing today; it would stop doing so the moment a
  // second writer appeared or the compile started failing differently on the
  // second attempt, and which message a status line shows should not rest on a
  // promise made in another file.
  std::string error;
  std::string reported;
  // One deadline for the Enter and not one per cursor. A budget each cursor
  // mints fresh is no bound on the keystroke -- sixty carets was sixty times
  // 25 ms, with every millisecond of it between the key going down and the line
  // appearing -- and what a cursor that runs into the end of it loses is the
  // tree's answer for its own line, not the newline. See kIndentBudget.
  const auto deadline = std::chrono::steady_clock::now() + kIndentBudget;
  for (size_t i = ranges.size(); i-- > 0;) {
    const Index cursor = CursorOf(ed.doc.table, ranges[i]);
    reported.clear();
    const NewlineIndent indent = IndentForNewline(ed, cursor, reported, deadline);
    if (error.empty()) error = reported;
    const std::string to_insert = "\n" + indent.indent + indent.closing_line;

    Edit edit;
    if (!Edited(ed, [&] { return Insert(to_insert, cursor, ed.doc.table, &edit); })) return;

    const Index landing = edit.start_byte + 1 + std::ssize(indent.indent);
    ranges[i].anchor = landing;
    ranges[i].head = landing;
    ranges[i].goal_column = -1;
    MapLaterRanges(ranges, i, edit);
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
  WarnAboutIndentOnce(ed, error);
}

template <typename F>
void TransformSelections(Editor& ed, F&& transform) {
  UndoGroup group(ed.doc.table);
  auto ranges = ed.doc.selections.Ranges();
  for (size_t i = ranges.size(); i-- > 0;) {
    const Selection& s = ranges[i];
    if (s.IsEmpty()) continue;
    std::string text = ReadDocRange(ed.doc.table, s.Range());
    transform(text);
    if (text.empty()) continue;
    Edit edit;
    if (!Edited(ed, [&] { return Replace(text, s.Range(), ed.doc.table, &edit); })) return;
  }
  // No position mapping here, and that is only correct because every transform
  // this is instantiated with is an ASCII case change: same byte length, so no
  // selection moves. A transform that changed the length -- anything Unicode
  // aware, 'ß' to "SS" being the obvious one -- would need the MapLaterRanges
  // loop its neighbours have, and would corrupt every selection but the last
  // without it. Keep that in mind before adding one.
  ed.doc.selections.Normalize(ed.doc.table);
}

void AsciiUpper(std::string& s) {
  for (char& c : s) {
    if ((c >= 'a') && (c <= 'z')) c = static_cast<char>(c - 'a' + 'A');
  }
}
void AsciiLower(std::string& s) {
  for (char& c : s) {
    if ((c >= 'A') && (c <= 'Z')) c = static_cast<char>(c - 'A' + 'a');
  }
}

void IndentBy(Editor& ed, bool add) {
  UndoGroup group(ed.doc.table);
  const std::string unit = ed.doc.insert_spaces
                               ? std::string(static_cast<size_t>(ed.doc.tab_width), ' ')
                               : std::string("\t");

  std::vector<Index> lines;
  for (const Selection& s : ed.doc.selections.Ranges()) {
    const auto [first, last] = LineSpan(ed.doc.table, s);
    for (Index line = first; line <= last; ++line) lines.push_back(line);
  }
  std::ranges::sort(lines);
  lines.erase(std::ranges::unique(lines).begin(), lines.end());

  std::vector<Change> changes;
  changes.reserve(lines.size());
  for (const Index line : lines) {
    const Index start = LineStart(ed.doc.table, line);
    if (add) {
      if (LineContentRange(ed.doc.table, line).empty()) continue;
      changes.push_back(Change{start, start, unit});
      continue;
    }
    Index remove = 0;
    Index columns = 0;
    char ch = 0;
    while ((columns < ed.doc.tab_width) && ByteAt(ed.doc.table, start + remove, ch)) {
      if (ch == '\t') {
        ++remove;
        break;
      }
      if (ch != ' ') break;
      ++remove;
      ++columns;
    }
    if (remove > 0) changes.push_back(Change{start, start + remove, std::string_view{}});
  }
  if (changes.empty()) return;

  auto ranges = ed.doc.selections.Ranges();
  CursorState before;
  for (const Selection& s : ranges) before.spans.push_back(CursorSpan{s.anchor, s.head});
  if (!Edited(ed, [&] { return Apply(ed.doc.table, changes, before, CursorState{}); })) return;

  std::vector<Index> starts;
  std::vector<Index> lands;
  std::vector<Index> shifts;
  starts.reserve(changes.size());
  lands.reserve(changes.size());
  shifts.reserve(changes.size());
  Index shift = 0;
  for (const Change& c : changes) {
    starts.push_back(c.from);
    lands.push_back(c.from + shift + std::ssize(c.text));
    shift += std::ssize(c.text) - (c.to - c.from);
    shifts.push_back(shift);
  }
  const auto map = [&](Index p) {
    const auto it = std::upper_bound(starts.begin(), starts.end(), p);
    if (it == starts.begin()) return p;
    const auto j = static_cast<std::size_t>(it - starts.begin() - 1);
    return std::max(lands[j], p + shifts[j]);
  };
  for (Selection& s : ranges) {
    s.anchor = map(s.anchor);
    s.head = map(s.head);
    s.goal_column = -1;
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
  CursorState after;
  for (const Selection& s : ed.doc.selections.Ranges()) {
    after.spans.push_back(CursorSpan{s.anchor, s.head});
  }
  NoteCursorsAfter(ed.doc.table, std::move(after));
}

void OpenLine(Editor& ed, bool below) {
  UndoGroup group(ed.doc.table);
  auto ranges = ed.doc.selections.Ranges();
  for (size_t i = ranges.size(); i-- > 0;) {
    const Index line = LineAt(ed.doc.table, below ? ranges[i].To() : ranges[i].From());
    const Index at = below ? LineContentEnd(ed.doc.table, line) : LineStart(ed.doc.table, line);
    Edit edit;
    if (!Edited(ed, [&] { return Insert("\n", at, ed.doc.table, &edit); })) return;
    const Index caret = below ? edit.new_end_byte : edit.start_byte;
    ranges[i].anchor = caret;
    ranges[i].head = caret;
    ranges[i].goal_column = -1;
    // Each iteration inserts below everything already processed, so the carets
    // already parked in this vector all move forward by that newline.
    MapLaterRanges(ranges, i, edit);
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
  ed.mode = Mode::kInsert;
}

void YankSelections(Editor& ed) {
  ed.registers.clear();
  for (const Selection& s : ed.doc.selections.Ranges()) {
    ed.registers.push_back(s.IsEmpty() ? std::string{} : ReadDocRange(ed.doc.table, s.Range()));
  }
  ed.status = "yanked " + std::to_string(ed.registers.size()) + " selection(s)";
}

void Paste(Editor& ed, bool after) {
  if (ed.registers.empty()) {
    ed.status.Warn("nothing yanked");
    return;
  }
  UndoGroup group(ed.doc.table);
  auto ranges = ed.doc.selections.Ranges();
  for (size_t i = ranges.size(); i-- > 0;) {
    const std::string& text =
        (ed.registers.size() == ranges.size()) ? ed.registers[i] : ed.registers.front();
    if (text.empty()) continue;
    const Index at = after ? ranges[i].To() : ranges[i].From();
    Edit edit;
    if (!Edited(ed, [&] { return Insert(text, at, ed.doc.table, &edit); })) return;
    LandOnEdit(ed, ranges[i], edit);
    MapLaterRanges(ranges, i, edit);
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
}

void JoinOnce(Editor& ed, Index line) {
  if (line + 1 >= LineCount(ed.doc.table)) return;
  const Index end = LineContentEnd(ed.doc.table, line);
  Index next = LineStart(ed.doc.table, line + 1);
  const Interval content = LineContentRange(ed.doc.table, line + 1);
  if (!content.empty()) {
    const std::string text = ReadDocRange(ed.doc.table, content);
    Index skip = 0;
    while ((skip < std::ssize(text)) && ((text[skip] == ' ') || (text[skip] == '\t'))) ++skip;
    next += skip;
  }
  if (next <= end) return;
  Edit edit;
  Edited(ed, [&] { return Replace(" ", Interval(end, next), ed.doc.table, &edit); });
}

void JoinLines(Editor& ed) {
  UndoGroup group(ed.doc.table);
  auto ranges = ed.doc.selections.Ranges();
  for (size_t i = ranges.size(); i-- > 0;) {
    const Index first = LineAt(ed.doc.table, ranges[i].From());
    const Index last =
        LineAt(ed.doc.table, std::max(ranges[i].From(), ranges[i].To() - 1));
    const Index highest_join = (first == last) ? first : last - 1;
    for (Index line = highest_join; line >= first; --line) JoinOnce(ed, line);
  }
  ed.doc.selections.Normalize(ed.doc.table);
}

void ToggleComments(Editor& ed) {
  const std::string_view token = CommentTokenFor(LanguageForPath(ed.doc.file));
  if (token.empty()) {
    ed.status.Warn("no line comment for this file type");
    return;
  }
  PieceTable& table = ed.doc.table;

  std::vector<Index> lines;
  for (const Selection& s : ed.doc.selections.Ranges()) {
    const auto [first, last] = LineSpan(table, s);
    for (Index line = first; line <= last; ++line) lines.push_back(line);
  }
  std::ranges::sort(lines);
  lines.erase(std::ranges::unique(lines).begin(), lines.end());

  struct LineState {
    Index line;
    Index text_at;
    bool commented;
  };
  std::vector<LineState> states;
  Index min_indent = std::numeric_limits<Index>::max();
  bool all_commented = true;
  for (const Index line : lines) {
    const Interval content = LineContentRange(table, line);
    if (content.empty()) continue;
    const std::string text = ReadDocRange(table, content);
    const size_t first_ns = text.find_first_not_of(" \t");
    if (first_ns == std::string::npos) continue;
    const bool commented = std::string_view{text}.substr(first_ns).starts_with(token);
    states.push_back(LineState{line, content.front() + static_cast<Index>(first_ns), commented});
    min_indent = std::min(min_indent, static_cast<Index>(first_ns));
    all_commented = all_commented && commented;
  }
  if (states.empty()) {
    ed.status.Warn("nothing to comment");
    return;
  }

  UndoGroup group(table);
  std::vector<Edit> edits;
  const Index doc_len = DocLength(table);
  if (all_commented) {
    for (size_t i = states.size(); i-- > 0;) {
      const LineState& state = states[i];
      Index len = static_cast<Index>(token.size());
      if ((state.text_at + len < doc_len) &&
          (ReadDocRange(table, Interval(state.text_at + len, state.text_at + len + 1)) == " ")) {
        ++len;
      }
      Edit edit;
      Edited(ed, [&] { return Delete(Interval(state.text_at, state.text_at + len), table, &edit); });
      edits.push_back(edit);
    }
  } else {
    const std::string insert = std::string{token} + " ";
    for (size_t i = states.size(); i-- > 0;) {
      if (states[i].commented) continue;
      Edit edit;
      Edited(ed, [&] {
        return Insert(insert, LineStart(table, states[i].line) + min_indent, table, &edit);
      });
      edits.push_back(edit);
    }
  }
  ed.doc.selections.MapThroughEdits(table, edits);
}

void ScrollBy(Editor& ed, Index delta) {
  const Index line_count = LineCount(ed.doc.table);
  const Index rows = std::max<Index>(1, ed.doc.view.rows);
  const Index max_top = std::max<Index>(0, line_count - 1);
  ed.doc.view.top_line = std::clamp<Index>(ed.doc.view.top_line + delta, 0, max_top);

  const Index pad = std::clamp<Index>(ed.doc.view.scrolloff, 0, std::max<Index>(0, (rows - 1) / 2));
  const Index first = ed.doc.view.top_line + pad;
  const Index last = std::min(ed.doc.view.top_line + rows - 1 - pad, line_count - 1);

  ed.pending_count = 0;

  // The band belongs to the view, so every cursor is measured against it, not
  // just the primary. Clamping the primary alone and shifting the whole set by
  // its correction was wrong from both ends: a cursor already off-screen was
  // carried along still off-screen, and one sitting comfortably inside the
  // viewport was dragged by a delta computed for somebody else, out through
  // the far edge. Cursors inside the band stay where the text put them --
  // scrolling out from under a cursor is what C-e is for -- and the rest land
  // on the nearer edge. Two that land on the same grapheme merge, which is the
  // price of not leaving one of them outside the window.
  //
  // The landing is one hop per cursor whatever the count. top_line has already
  // jumped the whole way, so the distance to cover is bounded by the
  // *document*, not the viewport: 5000<C-e> on a 20k-line file used to walk it
  // a line at a time, 5000 Move+Normalize passes for one keypress, times every
  // cursor. That walk also normalized after every line, so two cursors that
  // only *transiently* shared a byte -- same starting line, different columns,
  // passing over a line too short for both -- merged and never came apart
  // again. Landing once keeps each cursor's goal column, which is what a
  // counted `j` has always done; the scroll agrees with it.
  if (ClampCursorsToLines(ed.doc.table, ed.doc.selections, first, last, ed.doc.tab_width) &&
      (ed.mode == Mode::kInsert)) {
    // Same rule DoMove applies to any unextended motion: the block cursors
    // this leaves behind are collapsed back to carets at the end of the
    // binding, or insert mode ends up holding a grapheme it never selected.
    ed.collapse_insert_caret = true;
  }
}

void FindCharOnLine(Editor& ed, std::string_view target, PendingChar kind, bool extend,
                    Index count) {
  if (target.empty()) return;
  const PieceTable& table = ed.doc.table;
  const bool forward = (kind == PendingChar::kFindNext) || (kind == PendingChar::kTillNext);
  const bool till = (kind == PendingChar::kTillNext) || (kind == PendingChar::kTillPrev);

  auto ranges = ed.doc.selections.Ranges();
  for (Selection& s : ranges) {
    const Index origin = CursorOf(table, s);
    const Index line = LineAt(table, origin);
    const Interval content = LineRange(table, line);
    if (content.empty()) continue;
    const Index lo = content.front();
    const Index hi = content.back() + 1;
    const std::string text = ReadDocRange(table, content);

    const auto matches = [&](Index at) {
      const Index next = NextGraphemeBoundary(table, at);
      if ((next <= at) || (next > hi)) return false;
      return std::string_view{text}.substr(static_cast<size_t>(at - lo),
                                           static_cast<size_t>(next - at)) == target;
    };

    std::optional<Index> hit;
    Index from = origin;
    for (Index n = 0; n < count; ++n) {
      std::optional<Index> found;
      if (forward) {
        for (Index at = NextGraphemeBoundary(table, from); at < hi;) {
          const Index next = NextGraphemeBoundary(table, at);
          if (next <= at) break;
          if (matches(at)) {
            found = at;
            break;
          }
          at = next;
        }
      } else {
        Index at = PrevGraphemeBoundary(table, from);
        while ((at < from) && (at >= lo)) {
          if (matches(at)) {
            found = at;
            break;
          }
          if (at == lo) break;
          at = PrevGraphemeBoundary(table, at);
        }
      }
      if (!found) break;
      hit = found;
      from = *found;
    }
    if (!hit) continue;

    Index landing = *hit;
    if (till) {
      landing = forward ? PrevGraphemeBoundary(table, *hit) : NextGraphemeBoundary(table, *hit);
      if ((landing < lo) || (landing >= hi)) landing = *hit;
    }
    s = PutCursor(table, s, landing, extend);
    s.goal_column = -1;
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
}

void ArmFindChar(Editor& ed, PendingChar kind, bool extend) {
  const bool till = (kind == PendingChar::kTillNext) || (kind == PendingChar::kTillPrev);
  ed.pending_char = kind;
  ed.pending_char_extend = extend;
  ed.status = till ? "till char..." : "find char...";
}

void ReplaceEachChar(Editor& ed, std::string_view grapheme) {
  if (grapheme.empty()) return;
  UndoGroup group(ed.doc.table);
  auto ranges = ed.doc.selections.Ranges();
  // deque, not vector: Change::text points into these and a vector that grew
  // would leave every earlier view dangling.
  std::deque<std::string> texts;
  std::vector<Change> changes;
  std::vector<std::size_t> owner;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    if (ranges[i].IsEmpty()) continue;
    const Index count = CountGraphemes(ed.doc.table, ranges[i].Range());
    if (count <= 0) continue;
    std::string& text = texts.emplace_back();
    text.reserve(static_cast<size_t>(count) * grapheme.size());
    for (Index n = 0; n < count; ++n) text += grapheme;
    changes.push_back(Change{ranges[i].From(), ranges[i].To(), text});
    owner.push_back(i);
  }
  ApplyPerSelection(ed, ranges, changes, owner, true);
}

void ReplaceWithYanked(Editor& ed) {
  if (ed.registers.empty()) {
    ed.status.Warn("nothing yanked");
    return;
  }
  UndoGroup group(ed.doc.table);
  auto ranges = ed.doc.selections.Ranges();
  // Insert, delete and replace are the same change here: an empty selection
  // makes from == to, and empty text makes it a deletion.
  std::vector<Change> changes;
  std::vector<std::size_t> owner;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    const std::string& text =
        (ed.registers.size() == ranges.size()) ? ed.registers[i] : ed.registers.front();
    if (ranges[i].IsEmpty() && text.empty()) continue;
    changes.push_back(Change{ranges[i].From(), ranges[i].To(), text});
    owner.push_back(i);
  }
  ApplyPerSelection(ed, ranges, changes, owner, false);
}

void AlignView(Editor& ed, int where) {
  const Index rows = std::max<Index>(1, ed.doc.view.rows);
  const Index line = LineAt(ed.doc.table, ed.doc.selections.Primary().head);
  const Index top = (where < 0) ? line : (where == 0) ? (line - rows / 2) : (line - rows + 1);
  ed.doc.view.top_line = std::max<Index>(0, top);
  ed.align_view_once = true;
}

void SwitchCase(std::string& s) {
  for (char& c : s) {
    if ((c >= 'a') && (c <= 'z')) c = static_cast<char>(c - 'a' + 'A');
    else if ((c >= 'A') && (c <= 'Z')) c = static_cast<char>(c - 'A' + 'a');
  }
}

void UndoOrRedo(Editor& ed, bool undo) {
  std::vector<Edit> edits;
  CursorState cursors;
  const ErrorCtx err =
      undo ? Undo(ed.doc.table, &cursors, &edits) : Redo(ed.doc.table, &cursors, &edits);
  if (err) {
    ed.status.Fail(FormatErrorCtx(err));
    return;
  }
  if (edits.empty()) {
    return;
  }
  std::vector<Selection> restored;
  // Both directions: Undo hands back cursors_before, Redo hands back
  // cursors_after (piece_doc.h, Undo/Redo in piece_doc.cpp). Taking it only for
  // undo threw away everything NoteCursorsAfter recorded and rebuilt the
  // selection from the edit list -- which, for a grouped revision, is one
  // selection per *change*, not per cursor. Redoing an indent over N lines
  // came back as N selections, and a redone insert-mode keystroke came back as
  // a forward span over the text it typed instead of the caret after it.
  restored.reserve(cursors.spans.size());
  for (const CursorSpan& s : cursors.spans) {
    restored.push_back(Selection{s.anchor, s.head, -1});
  }
  if (restored.empty()) {
    // A revision from before NoteCursorsAfter existed, or one whose command
    // never noted them: fall back to the edit spans.
    for (const Edit& e : edits) {
      restored.push_back(Selection{e.start_byte, std::max(e.start_byte, e.new_end_byte), -1});
    }
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(restored));
  ed.doc.modified = (CurrentUndoSerial(ed.doc.table) != ed.doc.saved_undo_serial);
  if (IsExcerptView(ed.doc)) AlignExcerptModel(ed.doc);
}

void SplitOnNewlines(Editor& ed) {
  const PieceTable& table = ed.doc.table;
  std::vector<Selection> out;
  for (const Selection& s : ed.doc.selections.Ranges()) {
    const auto [first, last] = LineSpan(table, s);
    if (first == last) {
      out.push_back(s);
      continue;
    }
    for (Index line = first; line <= last; ++line) {
      const Index lo = std::max(s.From(), LineStart(table, line));
      const Index hi = std::min(s.To(), LineContentEnd(table, line));
      out.push_back(Selection{lo, std::max(lo, hi), -1});
    }
  }
  ed.doc.selections.Replace(table, std::move(out));
}

void SplitOnChar(Editor& ed, std::string_view separator) {
  if (separator.empty()) return;
  const PieceTable& table = ed.doc.table;
  std::vector<Selection> out;
  for (const Selection& s : ed.doc.selections.Ranges()) {
    if (s.IsEmpty()) {
      out.push_back(s);
      continue;
    }
    const std::string text = ReadDocRange(table, s.Range());
    const Index base = s.From();
    Index piece_start = 0;
    bool split_any = false;
    for (size_t i = 0; i + separator.size() <= text.size();) {
      if (std::string_view{text}.substr(i, separator.size()) == separator) {
        out.push_back(Selection{base + static_cast<Index>(piece_start), base + static_cast<Index>(i), -1});
        i += separator.size();
        piece_start = static_cast<Index>(i);
        split_any = true;
        continue;
      }
      i = NextGraphemeInString(text, i);
    }
    if (!split_any) {
      out.push_back(s);
      continue;
    }
    out.push_back(Selection{base + piece_start, s.To(), -1});
  }
  std::erase_if(out, [](const Selection& s) { return s.IsEmpty(); });
  if (out.empty()) return;
  ed.doc.selections.Replace(table, std::move(out));
}

void RotateContents(Editor& ed, bool forward) {
  const auto& ranges = ed.doc.selections.Ranges();
  if (ranges.size() < 2) return;

  std::vector<std::string> texts;
  texts.reserve(ranges.size());
  for (const Selection& s : ranges) texts.push_back(ReadDocRange(ed.doc.table, s.Range()));

  std::vector<std::string> rotated(texts.size());
  for (size_t i = 0; i < texts.size(); ++i) {
    const size_t n = texts.size();
    rotated[i] = forward ? texts[(i + n - 1) % n] : texts[(i + 1) % n];
  }

  UndoGroup group(ed.doc.table);
  auto after = ranges;
  std::vector<Change> changes;
  std::vector<std::size_t> owner;
  for (std::size_t i = 0; i < after.size(); ++i) {
    if (rotated[i] == texts[i]) continue;
    changes.push_back(Change{after[i].From(), after[i].To(), rotated[i]});
    owner.push_back(i);
  }
  ApplyPerSelection(ed, after, changes, owner, true);
}

// One selection per line, which is both what another application expects to
// receive and what SplitClipboardLines can take apart again.
std::string JoinParts(std::span<const std::string> parts) {
  std::string joined;
  // Separator on every part but the first. Not "whenever `joined` is non-empty":
  // an empty selection comes through as an empty part, and {"", "b"} has to
  // join to "\nb" rather than "b" or the two no longer come apart again.
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) joined += '\n';
    joined += parts[i];
  }
  return joined;
}

// The inverse, for text we did not write: a copy from another application has
// no record of where its selections were, so its line breaks are the only
// division on offer. A trailing newline ends the last line rather than starting
// an empty one -- "a\nb\n" is the two lines a caller meant to copy.
std::vector<std::string> SplitClipboardLines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t nl = text.find('\n', at);
    const std::size_t end = (nl == std::string_view::npos) ? text.size() : nl;
    std::string_view line = text.substr(at, end - at);
    if (!line.empty() && (line.back() == '\r')) line.remove_suffix(1);
    lines.emplace_back(line);
    if (nl == std::string_view::npos) break;
    at = nl + 1;
  }
  if ((lines.size() > 1) && lines.back().empty()) lines.pop_back();
  return lines;
}

}

std::vector<std::string> ClipboardPieces(std::string_view text,
                                         std::span<const std::string> remembered,
                                         std::size_t cursors, bool spread) {
  if (cursors > 1) {
    // Our own multi-cursor copy, still sitting on the clipboard untouched: hand
    // each cursor back the selection it came from.
    if ((remembered.size() == cursors) && (JoinParts(remembered) == text)) {
      return {remembered.begin(), remembered.end()};
    }
    // Somebody else's copy, whose line count happens to match. Its line breaks
    // are the only division on offer, and VS Code takes them.
    if (spread) {
      std::vector<std::string> lines = SplitClipboardLines(text);
      if (lines.size() == cursors) return lines;
    }
  }
  return {std::string{text}};
}

namespace {

void YankToClipboard(Editor& ed, bool main_only) {
  std::vector<std::string> parts;
  if (main_only) {
    parts.push_back(ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range()));
  } else {
    for (const Selection& s : ed.doc.selections.Ranges()) {
      parts.push_back(s.IsEmpty() ? std::string{} : ReadDocRange(ed.doc.table, s.Range()));
    }
  }
  const std::size_t count = parts.size();
  const std::string text = JoinParts(parts);
  const bool copied = ClipboardCopy(text);
  ed.registers = parts;
  // Only a clipboard that really took the text can be recognised later.
  ed.clipboard_parts = copied ? std::move(parts) : std::vector<std::string>{};
  if (!copied) {
    ed.status.Warn(HasClipboard()
                       ? "clipboard command failed -- yanked to the internal register"
                       : "no clipboard tool found -- yanked to the internal register");
  } else if (count > 1) {
    ed.status = "yanked " + std::to_string(count) + " selections (" +
                std::to_string(text.size()) + " bytes) to the clipboard";
  } else {
    ed.status = "yanked " + std::to_string(text.size()) + " bytes to the clipboard";
  }
}

// `parts` is either one piece for every selection, in which case each selection
// gets its own, or a single piece that every selection gets a copy of.
void PasteParts(Editor& ed, std::span<const std::string> parts, bool after,
                bool replace_selection) {
  if (std::ranges::all_of(parts, [](const std::string& p) { return p.empty(); })) {
    ed.status.Warn("clipboard is empty");
    return;
  }
  UndoGroup group(ed.doc.table);
  auto ranges = ed.doc.selections.Ranges();
  const bool per_selection = (parts.size() == ranges.size());
  for (size_t i = ranges.size(); i-- > 0;) {
    const std::string& text = per_selection ? parts[i] : parts.front();
    // Nothing to paste here, but a replacing paste still clears the selection.
    if (text.empty() && !(replace_selection && !ranges[i].IsEmpty())) continue;
    Edit edit;
    if (!Edited(ed, [&] {
          if (replace_selection && !ranges[i].IsEmpty()) {
            return Replace(text, ranges[i].Range(), ed.doc.table, &edit);
          }
          return Insert(text, after ? ranges[i].To() : ranges[i].From(), ed.doc.table, &edit);
        })) {
      return;
    }
    LandOnEdit(ed, ranges[i], edit);
    MapLaterRanges(ranges, i, edit);
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
}

void PasteText(Editor& ed, std::string_view text, bool after, bool replace_selection) {
  const std::array<std::string, 1> one{std::string{text}};
  PasteParts(ed, one, after, replace_selection);
}

void PasteFromClipboard(Editor& ed, bool after, bool replace_selection) {
  std::string text;
  if (!ClipboardPaste(text)) {
    if (ed.registers.empty()) {
      ed.status = HasClipboard() ? "could not read the clipboard" : "no clipboard tool found";
      return;
    }
    // No clipboard tool, so the internal register stands in -- and it already
    // holds one entry per selection from the yank that filled it.
    PasteParts(ed, ed.registers, after, replace_selection);
    return;
  }
  const std::size_t cursors = ed.doc.selections.Ranges().size();
  const std::vector<std::string> parts =
      ClipboardPieces(text, ed.clipboard_parts, cursors, ed.settings.multi_cursor_paste_spread);
  PasteParts(ed, parts, after, replace_selection);

  // Spread is asked for by line count, so a copy one line off silently pastes
  // whole instead -- which looks like the setting is not working. Say what the
  // counts were. Only when a spread was plausibly wanted, and never over a
  // warning the paste itself raised.
  if (ed.settings.multi_cursor_paste_spread && (cursors > 1) && (parts.size() == 1) &&
      (ed.status.level() == StatusLevel::kInfo)) {
    const std::size_t lines = SplitClipboardLines(text).size();
    if (lines > 1) {
      ed.status = "pasted whole at each cursor: " + std::to_string(lines) + " lines, " +
                  std::to_string(cursors) + " cursors";
    }
  }
}

char ClosingFor(char open) {
  switch (open) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '<': return '>';
    default: return open;
  }
}

bool SurroundPair(std::string_view key, char& open, char& close) {
  if (key.size() != 1) return false;
  switch (key.front()) {
    case ')': case '(': open = '('; close = ')'; return true;
    case ']': case '[': open = '['; close = ']'; return true;
    case '}': case '{': open = '{'; close = '}'; return true;
    case '>': case '<': open = '<'; close = '>'; return true;
    default: break;
  }
  const char c = key.front();
  if (static_cast<unsigned char>(c) < 0x20) return false;
  open = c;
  close = ClosingFor(c);
  return true;
}

void SurroundAdd(Editor& ed, std::string_view key) {
  char open = 0;
  char close = 0;
  if (!SurroundPair(key, open, close)) {
    ed.status.Warn("not a delimiter");
    return;
  }
  UndoGroup group(ed.doc.table);
  auto ranges = ed.doc.selections.Ranges();
  for (size_t i = ranges.size(); i-- > 0;) {
    Edit close_edit;
    Edit open_edit;
    if (!Edited(ed, [&] {
          ErrorCtx err = Insert(std::string(1, close), ranges[i].To(), ed.doc.table, &close_edit);
          if (!err) {
            err = Insert(std::string(1, open), ranges[i].From(), ed.doc.table, &open_edit);
          }
          return err;
        })) {
      return;
    }
    SpanKeepingOrientation(ranges[i], ranges[i].From(), ranges[i].To() + 2);
    MapLaterRanges(ranges, i, close_edit);
    MapLaterRanges(ranges, i, open_edit);
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
}

bool FindEnclosingIn(const PieceTable& table, Index at, char open, char close, Index search_start,
                     Index search_end, Index& open_at, Index& close_at) {
  const bool symmetric = (open == close);
  const std::string text = ReadDocRange(table, Interval(search_start, search_end));
  const Index local_at = at - search_start;
  if ((local_at < 0) || (local_at > std::ssize(text))) return false;

  Index depth = 0;
  Index i = std::min<Index>(local_at, std::ssize(text) - 1);
  for (; i >= 0; --i) {
    if (!symmetric && (text[i] == close) && (i != local_at)) {
      ++depth;
      continue;
    }
    if (text[i] == open) {
      if (depth == 0) break;
      --depth;
    }
  }
  if (i < 0) return false;
  open_at = search_start + i;

  depth = 0;
  Index j = i + 1;
  for (; j < std::ssize(text); ++j) {
    if (!symmetric && (text[j] == open)) {
      ++depth;
      continue;
    }
    if (text[j] == close) {
      if (depth == 0) break;
      --depth;
    }
  }
  if (j >= std::ssize(text)) return false;
  close_at = search_start + j;
  return close_at >= at;
}

bool FindEnclosing(const PieceTable& table, Index at, char open, char close, Index& open_at,
                   Index& close_at) {
  const Index doc_len = DocLength(table);

  constexpr Index kFirstWindow = 8 * 1024;
  constexpr Index kMaxWindow = 4 << 20;

  for (Index window = kFirstWindow;; window *= 2) {
    const Index search_start = std::max<Index>(0, at - window);
    const Index search_end = std::min<Index>(doc_len, at + window);
    if (FindEnclosingIn(table, at, open, close, search_start, search_end, open_at, close_at)) {
      return true;
    }
    if (((search_start == 0) && (search_end == doc_len)) || (window >= kMaxWindow)) return false;
  }
}

void SurroundDelete(Editor& ed, std::string_view key, std::string_view replacement) {
  char open = 0;
  char close = 0;
  if (!SurroundPair(key, open, close)) {
    ed.status.Warn("not a delimiter");
    return;
  }
  char new_open = 0;
  char new_close = 0;
  const bool replacing = !replacement.empty();
  if (replacing && !SurroundPair(replacement, new_open, new_close)) {
    ed.status.Warn("not a delimiter");
    return;
  }

  UndoGroup group(ed.doc.table);
  std::vector<std::pair<Index, Index>> pairs;
  for (const Selection& s : ed.doc.selections.Ranges()) {
    Index open_at = 0;
    Index close_at = 0;
    if (!FindEnclosing(ed.doc.table, CursorOf(ed.doc.table, s), open, close, open_at, close_at)) {
      continue;
    }
    pairs.emplace_back(open_at, close_at);
  }
  if (pairs.empty()) {
    ed.status.Warn("no enclosing pair");
    return;
  }
  std::ranges::sort(pairs);
  pairs.erase(std::ranges::unique(pairs).begin(), pairs.end());

  std::vector<Edit> edits;
  for (size_t i = pairs.size(); i-- > 0;) {
    const auto [open_at, close_at] = pairs[i];
    Edit close_edit;
    Edit open_edit;
    if (!Edited(ed, [&] {
          ErrorCtx err =
              replacing ? Replace(std::string(1, new_close), Interval(close_at, close_at + 1),
                                  ed.doc.table, &close_edit)
                        : Delete(Interval(close_at, close_at + 1), ed.doc.table, &close_edit);
          if (!err) {
            err = replacing ? Replace(std::string(1, new_open), Interval(open_at, open_at + 1),
                                      ed.doc.table, &open_edit)
                            : Delete(Interval(open_at, open_at + 1), ed.doc.table, &open_edit);
          }
          return err;
        })) {
      return;
    }
    edits.push_back(close_edit);
    edits.push_back(open_edit);
  }
  ed.doc.selections.MapThroughEdits(ed.doc.table, edits);
  ed.status.clear();
}

}

Index MatchingBracket(const PieceTable& table, Index at, Index window) {
  static constexpr std::string_view kOpens = "([{<";
  static constexpr std::string_view kCloses = ")]}>";
  const Index doc_len = DocLength(table);
  if ((at < 0) || (at >= doc_len)) return -1;
  char c = 0;
  if (!ByteAt(table, at, c)) return -1;

  if (const size_t open_index = kOpens.find(c); open_index != std::string_view::npos) {
    const std::string text = ReadDocRange(table, Interval(at, std::min(doc_len, at + window)));
    Index depth = 0;
    for (Index j = 1; j < std::ssize(text); ++j) {
      if (text[j] == c) {
        ++depth;
      } else if (text[j] == kCloses[open_index]) {
        if (depth == 0) return at + j;
        --depth;
      }
    }
  } else if (const size_t close_index = kCloses.find(c); close_index != std::string_view::npos) {
    const Index from = std::max<Index>(0, at - window);
    const std::string text = ReadDocRange(table, Interval(from, at + 1));
    Index depth = 0;
    for (Index j = std::ssize(text) - 2; j >= 0; --j) {
      if (text[j] == c) {
        ++depth;
      } else if (text[j] == kOpens[close_index]) {
        if (depth == 0) return from + j;
        --depth;
      }
    }
  }
  return -1;
}

namespace {

std::string_view TreeSitterObjectFor(std::string_view key) {
  if (key == "f") return "function";
  if (key == "t") return "class";
  if (key == "a") return "parameter";
  if (key == "c") return "comment";
  if (key == "T") return "test";
  if (key == "e") return "entry";
  return {};
}

bool ParagraphAround(const PieceTable& table, Index at, bool around, Index& lo, Index& hi) {
  const Index line_count = LineCount(table);
  const auto is_empty = [&](Index l) {
    return (l >= 0) && (l < line_count) && LineContentRange(table, l).empty() &&
           !LineRange(table, l).empty();
  };
  const auto line_start = [&](Index l) {
    return (l < line_count) ? LineStart(table, l) : DocLength(table);
  };

  Index line = LineAt(table, at);
  const bool prev_line_empty = is_empty(std::max<Index>(0, line - 1));
  const bool curr_line_empty = is_empty(line);
  const bool next_line_empty = prev_line_empty;
  const bool last_char = PrevGraphemeBoundary(table, line_start(line + 1)) == at;
  const bool prev_empty_to_line = prev_line_empty && !curr_line_empty;
  const bool curr_empty_to_line = curr_line_empty && !next_line_empty;

  Index line_back = line;
  if (prev_empty_to_line || curr_empty_to_line) ++line_back;
  if (!(curr_empty_to_line && last_char)) {
    while ((line_back > 0) && is_empty(line_back - 1)) --line_back;
    while ((line_back > 0) && !is_empty(line_back - 1)) --line_back;
  }

  if (curr_empty_to_line && last_char) ++line;
  bool crossed = false;
  while ((line < line_count) && !is_empty(line)) {
    ++line;
    crossed = true;
  }
  while ((line < line_count) && is_empty(line)) ++line;

  if (!crossed && (line >= line_count)) {
    while ((line_back > 0) && is_empty(line_back - 1)) --line_back;
    while ((line_back > 0) && !is_empty(line_back - 1)) --line_back;
  }

  if (!around) {
    while ((line > 0) && is_empty(line - 1)) --line;
  }

  lo = line_start(line_back);
  hi = line_start(line);
  return hi > lo;
}

void SelectTextObject(Editor& ed, std::string_view key, bool around) {
  if (key.empty()) return;
  const PieceTable& table = ed.doc.table;

  if (const std::string_view object = TreeSitterObjectFor(key); !object.empty()) {
    static constexpr std::array<std::string_view, 1> kInside{"inside"};
    static constexpr std::array<std::string_view, 1> kAround{"around"};
    std::vector<ObjectRange> objects;
    std::string error;
    Index lo = DocLength(table);
    Index hi = 0;
    for (const Selection& s : ed.doc.selections.Ranges()) {
      const Index at = CursorOf(table, s);
      lo = std::min(lo, at);
      hi = std::max(hi, at + 1);
    }
    if (!TextObjectRanges(table, ed.doc.file, object,
                          around ? std::span<const std::string_view>{kAround}
                                 : std::span<const std::string_view>{kInside},
                          objects, error, ed.doc.syntax.get(),
                          (lo < hi) ? Interval(lo, hi) : Interval(0, 0))) {
      ed.status = error;
      return;
    }
    // Succeeded and still complained: the query ran out of budget or of match
    // slots, so `objects` is short. Say which, and keep the selection that the
    // objects it did find supports.
    if (!error.empty()) ed.status.Warn(error);
    auto ts_ranges = ed.doc.selections.Ranges();
    bool ts_any = false;
    for (Selection& s : ts_ranges) {
      const Index at = CursorOf(table, s);
      const ObjectRange* best = nullptr;
      for (const ObjectRange& o : objects) {
        if ((at < o.from) || (at >= o.to)) continue;
        if ((best == nullptr) || ((o.to - o.from) < (best->to - best->from))) best = &o;
      }
      if (best == nullptr) continue;
      s.anchor = best->from;
      s.head = best->to;
      s.goal_column = -1;
      ts_any = true;
    }
    if (!ts_any) {
      // Not "no function here" when the lookup came back short: that is a claim
      // about the file, and what happened is a claim about the budget.
      if (error.empty()) ed.status.Warn("no " + std::string{object} + " here");
      return;
    }
    ed.doc.selections.Replace(table, std::move(ts_ranges));
    return;
  }

  if (key == "g") {
    ed.status = "\"g\" needs a diff provider, which koi does not have";
    return;
  }

  if (key == "p") {
    auto ranges = ed.doc.selections.Ranges();
    bool any = false;
    for (Selection& s : ranges) {
      Index lo = 0;
      Index hi = 0;
      if (!ParagraphAround(table, CursorOf(table, s), around, lo, hi)) continue;
      s.anchor = lo;
      s.head = hi;
      s.goal_column = -1;
      any = true;
    }
    if (!any) {
      ed.status.Warn("no paragraph here");
      return;
    }
    ed.doc.selections.Replace(table, std::move(ranges));
    return;
  }

  const bool is_word = (key == "w") || (key == "W");
  const bool is_closest = (key == "m");
  char open = 0;
  char close = 0;
  static constexpr std::string_view kDelimiters = "()[]{}<>\"'`";
  const bool is_pair = (key.size() == 1) && (kDelimiters.find(key.front()) != std::string_view::npos);
  if (!is_word && !is_closest && (!is_pair || !SurroundPair(key, open, close))) {
    ed.status = "\"" + std::string{key} + "\" is not a textobject";
    return;
  }

  auto ranges = ed.doc.selections.Ranges();
  bool any = false;
  for (Selection& s : ranges) {
    const Index at = CursorOf(table, s);
    Index lo = 0;
    Index hi = 0;

    if (is_word) {
      const bool long_word = (key == "W");
      const Index line = LineAt(table, at);
      const Interval content = LineContentRange(table, line);
      if (content.empty()) continue;
      const Index line_lo = content.front();
      const Index line_hi = content.back() + 1;
      const auto same_class = [&](Index pos) {
        return (pos >= line_lo) && (pos < line_hi) &&
               (CharClassAt(table, pos, long_word) == CharClassAt(table, at, long_word));
      };
      if (CharClassAt(table, at, long_word) == CharClass::kSpace) continue;
      if (!same_class(at)) continue;
      lo = at;
      while ((lo > line_lo) && same_class(PrevGraphemeBoundary(table, lo))) {
        lo = PrevGraphemeBoundary(table, lo);
      }
      hi = at;
      while ((hi < line_hi) && same_class(hi)) hi = NextGraphemeBoundary(table, hi);
      if (around) {
        Index end = hi;
        while ((end < line_hi) && (CharClassAt(table, end) == CharClass::kSpace)) {
          end = NextGraphemeBoundary(table, end);
        }
        if (end > hi) {
          hi = end;
        } else {
          while ((lo > line_lo) &&
                 (CharClassAt(table, PrevGraphemeBoundary(table, lo)) == CharClass::kSpace)) {
            lo = PrevGraphemeBoundary(table, lo);
          }
        }
      }
    } else {
      Index open_at = 0;
      Index close_at = 0;
      if (is_closest) {
        static constexpr std::string_view kOpens = "([{<\"'`";
        static constexpr std::string_view kCloses = ")]}>\"'`";
        bool found = false;
        for (size_t p = 0; p < kOpens.size(); ++p) {
          Index o_at = 0;
          Index c_at = 0;
          if (!FindEnclosing(table, at, kOpens[p], kCloses[p], o_at, c_at)) continue;
          if (!found || (o_at > open_at)) {
            open_at = o_at;
            close_at = c_at;
            found = true;
          }
        }
        if (!found) continue;
      } else if (!FindEnclosing(table, at, open, close, open_at, close_at)) {
        continue;
      }
      lo = around ? open_at : open_at + 1;
      hi = around ? close_at + 1 : close_at;
      if (lo >= hi) continue;
    }

    s.anchor = lo;
    s.head = hi;
    s.goal_column = -1;
    any = true;
  }
  if (!any) {
    ed.status.Warn("no text object here");
    return;
  }
  ed.doc.selections.Replace(table, std::move(ranges));
}

void MatchBrackets(Editor& ed) {
  const PieceTable& table = ed.doc.table;
  auto ranges = ed.doc.selections.Ranges();
  bool moved = false;
  for (Selection& s : ranges) {
    const Index target = MatchingBracket(table, CursorOf(table, s));
    if (target < 0) continue;
    s = PutCursor(table, s, target, false);
    s.goal_column = -1;
    moved = true;
  }
  if (!moved) {
    ed.status.Warn("no matching bracket");
    return;
  }
  ed.doc.selections.Replace(table, std::move(ranges));
}

void GotoTextObject(Editor& ed, std::string_view object, bool forward) {
  static constexpr std::array<std::string_view, 3> kSuffixes{"movement", "around", "inside"};
  const PieceTable& table = ed.doc.table;
  std::vector<ObjectRange> objects;
  std::string error;
  if (!TextObjectRanges(table, ed.doc.file, object, kSuffixes, objects, error, ed.doc.syntax.get())) {
    ed.status = error;
    return;
  }
  // Short of objects rather than out of them: the jump below may land on the
  // wrong one or nowhere, and the reason is the budget, not the file.
  if (!error.empty()) ed.status.Warn(error);
  const Index count = CountOr(ed, 1);
  auto ranges = ed.doc.selections.Ranges();
  bool moved = false;
  for (Selection& s : ranges) {
    for (Index step = 0; step < count; ++step) {
      const Index at = CursorOf(table, s);
      const ObjectRange* pick = nullptr;
      for (const ObjectRange& o : objects) {
        if (forward) {
          if (o.from <= at) continue;
          if ((pick == nullptr) || (o.from < pick->from) ||
              ((o.from == pick->from) && (o.to > pick->to))) {
            pick = &o;
          }
        } else {
          if (o.to >= at) continue;
          if ((pick == nullptr) || (o.to > pick->to) ||
              ((o.to == pick->to) && (o.from < pick->from))) {
            pick = &o;
          }
        }
      }
      if (pick == nullptr) break;
      if ((s.anchor == pick->from) && (s.head == pick->to)) break;
      s.anchor = pick->from;
      s.head = pick->to;
      s.goal_column = -1;
      moved = true;
    }
  }
  if (!moved) {
    if (error.empty()) ed.status.clear();
    return;
  }
  ed.doc.selections.Replace(table, std::move(ranges));
}

void MoveParagraph(Editor& ed, bool forward) {
  const PieceTable& table = ed.doc.table;
  const Index line_count = LineCount(table);
  const auto is_empty = [&](Index l) {
    return LineContentRange(table, l).empty() && !LineRange(table, l).empty();
  };
  const auto line_start = [&](Index l) {
    return (l < line_count) ? LineStart(table, l) : DocLength(table);
  };
  const Index count = CountOr(ed, 1);

  auto ranges = ed.doc.selections.Ranges();
  for (Selection& s : ranges) {
    const Index cursor = CursorOf(table, s);
    Index line = LineAt(table, cursor);

    if (forward) {
      const bool last_char = PrevGraphemeBoundary(table, line_start(line + 1)) == cursor;
      const bool curr_empty = is_empty(line);
      const bool next_empty = is_empty(std::min(line_count - 1, line + 1));
      const bool curr_empty_to_line = curr_empty && !next_empty;
      if (curr_empty_to_line && last_char) ++line;
      Index last_line = line;
      for (Index i = 0; i < count; ++i) {
        while ((line < line_count) && !is_empty(line)) ++line;
        while ((line < line_count) && is_empty(line)) ++line;
        if (line == last_line) break;
        last_line = line;
      }
      const Index head = line_start(line);
      const Index anchor = (curr_empty_to_line && last_char) ? s.head : cursor;
      s.anchor = anchor;
      s.head = head;
    } else {
      const bool first_char = line_start(line) == cursor;
      const bool prev_empty = (line > 0) && is_empty(line - 1);
      const bool prev_empty_to_line = prev_empty && !is_empty(line);
      if (prev_empty_to_line && !first_char) ++line;
      Index last_line = line;
      for (Index i = 0; i < count; ++i) {
        while ((line > 0) && is_empty(line - 1)) --line;
        while ((line > 0) && !is_empty(line - 1)) --line;
        if (line == last_line) break;
        last_line = line;
      }
      const Index head = line_start(line);
      const Index anchor = (prev_empty_to_line && first_char) ? cursor : s.head;
      s.anchor = anchor;
      s.head = head;
    }
    s.goal_column = -1;
  }
  ed.doc.selections.Replace(table, std::move(ranges));
}

void AlignSelections(Editor& ed) {
  const PieceTable& table = ed.doc.table;
  const auto& ranges = ed.doc.selections.Ranges();
  if (ranges.size() < 2) return;

  Index target_column = 0;
  std::vector<Index> columns;
  columns.reserve(ranges.size());
  Index previous_line = -1;
  for (const Selection& s : ranges) {
    const Index line = LineAt(table, s.From());
    if (line == previous_line) {
      ed.status.Warn("align needs one selection per line");
      return;
    }
    previous_line = line;
    const Index column = ColumnForByte(table, s.From(), ed.doc.tab_width);
    columns.push_back(column);
    target_column = std::max(target_column, column);
  }

  UndoGroup group(ed.doc.table);
  auto after = ranges;
  for (size_t i = after.size(); i-- > 0;) {
    const Index pad = target_column - columns[i];
    if (pad <= 0) continue;
    Edit edit;
    if (!Edited(ed, [&] {
          return Insert(std::string(static_cast<size_t>(pad), ' '), after[i].From(), ed.doc.table,
                        &edit);
        })) {
      return;
    }
    after[i].anchor += pad;
    after[i].head += pad;
    MapLaterRanges(after, i, edit);
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(after));
}

void AddBlankLine(Editor& ed, bool below) {
  Index count = CountOr(ed, 1);
  UndoGroup group(ed.doc.table);
  std::vector<Index> at;
  for (const Selection& s : ed.doc.selections.Ranges()) {
    const auto [first, last] = LineSpan(ed.doc.table, s);
    at.push_back(below ? LineStartOrDocEnd(ed.doc.table, last + 1) : LineStart(ed.doc.table, first));
  }
  std::ranges::sort(at);
  at.erase(std::ranges::unique(at).begin(), at.end());
  if (at.empty()) return;

  // The count multiplies against the cursor count, and both reach five figures
  // on their own: `%` then `<A-s>` over a 20000-line file leaves one cursor per
  // line, and a slipped `999999` in front of `[` then asks for 20 GB of
  // newlines in a single keystroke -- an OOM kill that takes every unsaved
  // buffer with it. Bound the product the way ResizeFocusedPane bounds its
  // repeat: past this the count has stopped describing anything anyone wanted.
  constexpr Index kMaxInsertedBytes = 8 << 20;
  const Index max_each = std::max<Index>(1, kMaxInsertedBytes / std::ssize(at));
  if (count > max_each) {
    count = max_each;
    ed.status.Warn("too many blank lines for " + std::to_string(at.size()) + " cursors -- added " +
                   std::to_string(count) + " each");
  }

  const std::string newlines(static_cast<size_t>(count), '\n');
  std::vector<Edit> edits;
  edits.reserve(at.size());
  for (size_t i = at.size(); i-- > 0;) {
    Edit edit;
    const bool ok = Edited(ed, [&] { return Insert(newlines, at[i], ed.doc.table, &edit); });
    if (ok) edits.push_back(edit);
    if (!ok) break;
  }
  // One pass at the end rather than a remap interleaved with every insert:
  // same work today, but it is the shape MapThroughEdits can make cheaper for
  // every caller at once, and a refused insert now still leaves the cursors
  // mapped through the ones that did land.
  ed.doc.selections.MapThroughEdits(ed.doc.table, edits);
}

void ExpandSelection(Editor& ed) {
  const PieceTable& table = ed.doc.table;
  auto ranges = ed.doc.selections.Ranges();
  for (Selection& s : ranges) {
    const Index at = CursorOf(table, s);
    const Index line = LineAt(table, at);
    const Interval content = LineContentRange(table, line);
    const Index line_lo = LineStart(table, line);
    // A blank line has no *content*, but the cursor still sits on its
    // terminator. Measuring the line rung against the empty content range put
    // line_hi at line_lo, which makes the guard below unsatisfiable -- so one
    // press on a blank line skipped both the word and the line rung and handed
    // the next d/c/r/> the whole buffer.
    const Index line_hi = content.empty() ? LineStartOrDocEnd(table, line + 1) : content.back() + 1;

    const auto is_word = [&](Index pos) {
      if ((pos < line_lo) || (pos >= line_hi)) return false;
      const Index next = NextGraphemeBoundary(table, pos);
      if (next <= pos) return false;
      const std::string cluster = ReadDocRange(table, Interval(pos, next));
      const auto c = static_cast<unsigned char>(cluster.front());
      return (c >= 0x80) || (c == '_') || ((c >= '0') && (c <= '9')) ||
             ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z'));
    };

    if ((s.From() > line_lo) || (s.To() < line_hi)) {
      if (is_word(at)) {
        Index lo = at;
        while ((lo > line_lo) && is_word(PrevGraphemeBoundary(table, lo))) {
          lo = PrevGraphemeBoundary(table, lo);
        }
        Index hi = at;
        while ((hi < line_hi) && is_word(hi)) hi = NextGraphemeBoundary(table, hi);
        if ((lo <= s.From()) && (hi >= s.To()) && ((lo < s.From()) || (hi > s.To()))) {
          s.anchor = lo;
          s.head = hi;
          s.goal_column = -1;
          continue;
        }
      }
      s.anchor = line_lo;
      s.head = line_hi;
      s.goal_column = -1;
      continue;
    }
    s.anchor = 0;
    s.head = DocLength(table);
    s.goal_column = -1;
  }
  ed.doc.selections.Replace(table, std::move(ranges));
}

void ArmPendingChar(Editor& ed, PendingChar kind, bool extend, std::string_view prompt) {
  ed.pending_char = kind;
  ed.pending_char_extend = extend;
  ed.status = std::string{prompt};
}

void StepBuffer(Editor& ed, bool forward);
void CloseOtherWindows(Editor& ed);
void CloseOtherBuffers(Editor& ed, bool force);
void CloseBuffer(Editor& ed, bool force);
void ListBuffers(Editor& ed);

void ResizeFocusedPane(Editor& ed, ResizeAxis axis, bool grow) {
  const Index times = CountOr(ed, 1);
  ResizeResult last = ResizeResult::kNoNeighbour;
  for (Index i = 0; i < times; ++i) {
    last = ResizePane(ed, axis, grow, PaneArea(ed));
    if (last != ResizeResult::kMoved) break;
  }
  if (last == ResizeResult::kMoved) return;
  const bool width = (axis == ResizeAxis::kWidth);
  if (last == ResizeResult::kNoNeighbour) {
    ed.status.Warn(width ? "no window beside this one" : "no window above or below this one");
    return;
  }
  ed.status.Warn(std::string{"this window is as "} +
                 (width ? (grow ? "wide" : "narrow") : (grow ? "tall" : "short")) +
                 " as it goes");
}

void AdjustExcerptContext(Editor& ed, Index delta) {
  const Index lines = std::clamp<Index>(ed.settings.excerpt_context + delta, 0, 100);
  if (lines == ed.settings.excerpt_context) {
    ed.status.Warn(delta > 0 ? "excerpt-context is already 100 lines"
                             : "excerpt-context is already 0 lines");
    return;
  }
  ed.settings.excerpt_context = lines;
  if (!RebuildExcerptView(ed)) {
    ed.status = "excerpt-context: " + std::to_string(lines) + " lines";
  }
}

void FromCommandCancel(Editor& ed, std::string_view rest) {
  const std::string_view arg = Trim(rest, " \t");
  if (arg == "all") {
    const std::size_t count = ed.pending_commands.size();
    if (count == 0) {
      ed.status.Warn("no :from command is running");
      return;
    }
    KillAllCommandJobs(ed);
    ed.status = "cancelled " + std::to_string(count) + " :from command(s)";
    return;
  }
  if (!arg.empty()) {
    ed.status.Warn(":from-cancel takes nothing, or `all`");
    return;
  }
  if (!CancelCommandJob(ed)) ed.status.Warn("no :from command is running");
}

#define KOI_CMD(...) [](Editor& ed) __VA_ARGS__

constexpr std::array kCommands = std::to_array<CommandDef>({
    {"add_newline_above", KOI_CMD({ AddBlankLine(ed, false); }), "open a blank line above, staying put"},
    {"add_newline_below", KOI_CMD({ AddBlankLine(ed, true); }), "open a blank line below, staying put"},
    {"align_selections", KOI_CMD({ AlignSelections(ed); }), "pad so every selection starts in the same column"},
    {"align_view_bottom", KOI_CMD({ AlignView(ed, 1); }), "put the cursor line at the bottom"},
    {"align_view_center", KOI_CMD({ AlignView(ed, 0); }), "centre the cursor line"},
    {"align_view_top", KOI_CMD({ AlignView(ed, -1); }), "put the cursor line at the top"},
    {"append_mode", KOI_CMD({ EnterInsertAt(ed, true); }), "insert after the selection"},
    {"buffer_close", KOI_CMD({ CloseBuffer(ed, false); }), "close this buffer, keeping unsaved work"},
    {"buffer_list", KOI_CMD({ ListBuffers(ed); }), "list open buffers"},
    {"buffer_next", KOI_CMD({ StepBuffer(ed, true); }), "go to the next buffer"},
    {"buffer_picker", KOI_CMD({ BufferPicker(ed); }), "pick an open buffer"},
    {"buffer_previous", KOI_CMD({ StepBuffer(ed, false); }), "go to the previous buffer"},
    {"buffer_symbol_picker", KOI_CMD({ BufferSymbolPicker(ed); }), "definitions in this file, in order"},
    {"change_selection", KOI_CMD({ YankSelections(ed); ed.status.clear(); if (Edited(ed, [&] { return DeleteSelections(ed.doc.table, ed.doc.selections); })) ed.mode = Mode::kInsert; }), "delete the selection and insert"},
    {"clear_search_highlight", KOI_CMD({ ed.search_highlight = false; }), "stop painting search matches, keeping the pattern"},
    {"collapse_selection", KOI_CMD({ CollapseToCursor(ed); }), "drop the selection, keep the cursor"},
    {"command_mode", KOI_CMD({ PromptOpen(ed); }), "open the : command line"},
    {"content_picker", KOI_CMD({ ContentPicker(ed); }), "search every line in the project"},
    {"copy_selection_on_next_line", KOI_CMD({ for (Index i = 0; i < CountOr(ed, 1); ++i) { if (!AddCursorVertically(ed, true)) break; } }), "add a cursor below"},
    {"copy_selection_on_prev_line", KOI_CMD({ for (Index i = 0; i < CountOr(ed, 1); ++i) { if (!AddCursorVertically(ed, false)) break; } }), "add a cursor above"},
    {"decrement_excerpt_context", KOI_CMD({ AdjustExcerptContext(ed, -1); }), "a line less of context in the excerpt view"},
    {"delete_char_backward", KOI_CMD({ Edited(ed, [&] { return DeleteBackwardAtCursors(ed.doc.table, ed.doc.selections); }); }), "backspace"},
    {"delete_char_forward", KOI_CMD({ Edited(ed, [&] { return DeleteForwardAtCursors(ed.doc.table, ed.doc.selections); }); }), "delete forward"},
    {"delete_selection", KOI_CMD({ YankSelections(ed); ed.status.clear(); Edited(ed, [&] { return DeleteSelections(ed.doc.table, ed.doc.selections); }); }), "yank and delete the selection"},
    {"delete_selection_noyank", KOI_CMD({ Edited(ed, [&] { return DeleteSelections(ed.doc.table, ed.doc.selections); }); }), "delete without yanking"},
    {"excerpt_drop", KOI_CMD({ DropExcerptHunk(ed); }), "remove the excerpts under the cursors from the view, leaving their files alone"},
    {"expand_height", KOI_CMD({ ResizeFocusedPane(ed, ResizeAxis::kHeight, true); }), "make this window a tenth taller"},
    {"expand_selection", KOI_CMD({ ExpandSelection(ed); }), "grow to the word, then the line, then the file"},
    {"expand_width", KOI_CMD({ ResizeFocusedPane(ed, ResizeAxis::kWidth, true); }), "make this window a tenth wider"},
    {"extend_char_left", KOI_CMD({ DoMove(ed, Motion::kLeft, true); }), "extend left"},
    {"extend_char_right", KOI_CMD({ DoMove(ed, Motion::kRight, true); }), "extend right"},
    {"extend_line", KOI_CMD({ SelectWholeLines(ed, true); }), "select whole lines, extending on repeat"},
    {"extend_line_below", KOI_CMD({ SelectWholeLines(ed, true); }), "select whole lines, extending on repeat"},
    {"extend_line_down", KOI_CMD({ DoMove(ed, Motion::kDown, true); }), "extend down a line"},
    {"extend_line_up", KOI_CMD({ DoMove(ed, Motion::kUp, true); }), "extend up a line"},
    {"extend_next_char", KOI_CMD({ ArmFindChar(ed, PendingChar::kFindNext, true); }), "extend to the next occurrence of a character"},
    {"extend_next_long_word_end", KOI_CMD({ DoMove(ed, Motion::kLongWordEnd, true); }), "extend to the WORD end"},
    {"extend_next_long_word_start", KOI_CMD({ DoMove(ed, Motion::kLongWordNext, true); }), "extend to the next WORD"},
    {"extend_next_word_end", KOI_CMD({ DoMove(ed, Motion::kWordEnd, true); }), "extend to the word end"},
    {"extend_next_word_start", KOI_CMD({ DoMove(ed, Motion::kWordNext, true); }), "extend to the next word"},
    {"extend_page_cursor_half_down", KOI_CMD({ VerticalPage(ed, true, ed.doc.view.rows / 2, true); }), "extend half a page down"},
    {"extend_page_cursor_half_up", KOI_CMD({ VerticalPage(ed, false, ed.doc.view.rows / 2, true); }), "extend half a page up"},
    {"extend_page_down", KOI_CMD({ VerticalPage(ed, true, ed.doc.view.rows, true); }), "extend a page down"},
    {"extend_page_up", KOI_CMD({ VerticalPage(ed, false, ed.doc.view.rows, true); }), "extend a page up"},
    {"extend_prev_char", KOI_CMD({ ArmFindChar(ed, PendingChar::kFindPrev, true); }), "extend to the previous occurrence of a character"},
    {"extend_prev_long_word_end", KOI_CMD({ DoMove(ed, Motion::kLongWordPrevEnd, true); }), "extend to the previous WORD end"},
    {"extend_prev_long_word_start", KOI_CMD({ DoMove(ed, Motion::kLongWordPrev, true); }), "extend to the previous WORD"},
    {"extend_prev_word_end", KOI_CMD({ DoMove(ed, Motion::kWordPrevEnd, true); }), "extend to the previous word end"},
    {"extend_prev_word_start", KOI_CMD({ DoMove(ed, Motion::kWordPrev, true); }), "extend to the previous word"},
    {"extend_till_char", KOI_CMD({ ArmFindChar(ed, PendingChar::kTillNext, true); }), "extend up to the next occurrence"},
    {"extend_till_prev_char", KOI_CMD({ ArmFindChar(ed, PendingChar::kTillPrev, true); }), "extend back to after the previous occurrence"},
    {"extend_to_file_end", KOI_CMD({ DoMove(ed, Motion::kDocEnd, true); }), "extend to end of file"},
    {"extend_to_file_start", KOI_CMD({ DoMove(ed, Motion::kDocStart, true); }), "extend to start of file"},
    {"extend_to_line_bounds", KOI_CMD({ SelectWholeLines(ed, false); }), "grow the selection to whole lines"},
    {"extend_to_line_end", KOI_CMD({ DoMove(ed, Motion::kLineEnd, true); }), "extend to end of line"},
    {"extend_to_line_start", KOI_CMD({ DoMove(ed, Motion::kLineStart, true); }), "extend to start of line"},
    {"file_picker", KOI_CMD({ FilePicker(ed); }), "open a file, most-used first"},
    {"find_next_char", KOI_CMD({ ArmFindChar(ed, PendingChar::kFindNext, false); }), "move to the next occurrence of a character"},
    {"find_prev_char", KOI_CMD({ ArmFindChar(ed, PendingChar::kFindPrev, false); }), "move to the previous occurrence"},
    {"find_till_char", KOI_CMD({ ArmFindChar(ed, PendingChar::kTillNext, false); }), "move up to the next occurrence"},
    {"flip_selections", KOI_CMD({ for (Selection& s : ed.doc.selections.MutableRanges()) std::swap(s.anchor, s.head); }), "swap anchor and cursor"},
    {"goto_definition", KOI_CMD({ GotoDefinition(ed); }), "go to where the selection is defined"},
    {"goto_excerpt_source", KOI_CMD({ GotoExcerptSource(ed); }), "open the file this excerpt came from"},
    {"goto_file_start", KOI_CMD({ RecordJump(ed); DoMove(ed, Motion::kDocStart, false); }), "go to the start of the file"},
    {"goto_first_nonwhitespace", KOI_CMD({ DoMove(ed, Motion::kLineFirstNonBlank, false); }), "go to the first non-blank"},
    {"goto_last_edit", KOI_CMD({ GoToLastEdit(ed); }), "go to the most recent edit, in whichever file it was"},
    {"goto_last_line", KOI_CMD({ RecordJump(ed); DoMove(ed, Motion::kLastLine, false); }), "go to the last line"},
    {"goto_line_end", KOI_CMD({ DoMove(ed, Motion::kLineEnd, false); }), "go to the end of the line"},
    {"goto_line_start", KOI_CMD({ DoMove(ed, Motion::kLineStart, false); }), "go to the start of the line"},
    {"goto_next_buffer", KOI_CMD({ StepBuffer(ed, true); }), "go to the next buffer"},
    {"goto_next_class", KOI_CMD({ GotoTextObject(ed, "class", true); }), "select the next type definition"},
    {"goto_next_comment", KOI_CMD({ GotoTextObject(ed, "comment", true); }), "select the next comment"},
    {"goto_next_function", KOI_CMD({ GotoTextObject(ed, "function", true); }), "select the next function"},
    {"goto_next_paragraph", KOI_CMD({ MoveParagraph(ed, true); }), "move to the next paragraph"},
    {"goto_next_parameter", KOI_CMD({ GotoTextObject(ed, "parameter", true); }), "select the next argument"},
    {"goto_next_test", KOI_CMD({ GotoTextObject(ed, "test", true); }), "select the next test"},
    {"goto_prev_class", KOI_CMD({ GotoTextObject(ed, "class", false); }), "select the previous type definition"},
    {"goto_prev_comment", KOI_CMD({ GotoTextObject(ed, "comment", false); }), "select the previous comment"},
    {"goto_prev_function", KOI_CMD({ GotoTextObject(ed, "function", false); }), "select the previous function"},
    {"goto_prev_paragraph", KOI_CMD({ MoveParagraph(ed, false); }), "move to the previous paragraph"},
    {"goto_prev_parameter", KOI_CMD({ GotoTextObject(ed, "parameter", false); }), "select the previous argument"},
    {"goto_prev_test", KOI_CMD({ GotoTextObject(ed, "test", false); }), "select the previous test"},
    {"goto_previous_buffer", KOI_CMD({ StepBuffer(ed, false); }), "go to the previous buffer"},
    {"hsplit", KOI_CMD({ SplitWindow(ed, false); }), "split this window top and bottom"},
    {"increment_excerpt_context", KOI_CMD({ AdjustExcerptContext(ed, 1); }), "a line more of context in the excerpt view"},
    {"indent", KOI_CMD({ IndentBy(ed, true); }), "indent the selected lines"},
    {"insert_at_line_end", KOI_CMD({ DoMove(ed, Motion::kLineEnd, false); EnterInsertAt(ed, true); }), "insert at end of line"},
    {"insert_at_line_start", KOI_CMD({ DoMove(ed, Motion::kLineFirstNonBlank, false); EnterInsertAt(ed, false); }), "insert at first non-blank"},
    {"insert_mode", KOI_CMD({ EnterInsertAt(ed, false); }), "insert before the selection"},
    {"insert_newline", KOI_CMD({ InsertNewlineAutoIndent(ed); }), "insert a newline with auto-indent"},
    {"insert_tab", KOI_CMD({ const std::string t = ed.doc.insert_spaces ? std::string(static_cast<size_t>(ed.doc.tab_width), ' ') : std::string("\t"); Edited(ed, [&] { return InsertAtCursorsKeeping(t, ed.doc.table, ed.doc.selections); }); }), "insert a tab"},
    {"join_selections", KOI_CMD({ JoinLines(ed); }), "join the line below"},
    {"jump_backward", KOI_CMD({ StepJump(ed, false); }), "back to the previous position"},
    {"jump_forward", KOI_CMD({ StepJump(ed, true); }), "forward to the next position"},
    {"jump_view_down", KOI_CMD({ JumpWindow(ed, WindowDir::kDown); }), "focus the window below"},
    {"jump_view_left", KOI_CMD({ JumpWindow(ed, WindowDir::kLeft); }), "focus the window to the left"},
    {"jump_view_next", KOI_CMD({ FocusWindow(ed, true); }), "focus the next window"},
    {"jump_view_previous", KOI_CMD({ FocusWindow(ed, false); }), "focus the previous window"},
    {"jump_view_right", KOI_CMD({ JumpWindow(ed, WindowDir::kRight); }), "focus the window to the right"},
    {"jump_view_up", KOI_CMD({ JumpWindow(ed, WindowDir::kUp); }), "focus the window above"},
    {"keep_primary_selection", KOI_CMD({ ed.doc.selections.KeepPrimaryOnly(); }), "drop all but the primary cursor"},
    {"last_picker", KOI_CMD({ LastPicker(ed); }), "reopen the picker you were last in"},
    {"leap", KOI_CMD({ StartLeap(ed); }), "jump to a visible pair of characters"},
    {"match_brackets", KOI_CMD({ RecordJump(ed); MatchBrackets(ed); }), "jump to the matching bracket"},
    {"move_char_left", KOI_CMD({ DoMove(ed, Motion::kLeft, false); }), "move left one character"},
    {"move_char_right", KOI_CMD({ DoMove(ed, Motion::kRight, false); }), "move right one character"},
    {"move_line_down", KOI_CMD({ DoMove(ed, Motion::kDown, false); }), "move down a line"},
    {"move_line_up", KOI_CMD({ DoMove(ed, Motion::kUp, false); }), "move up a line"},
    {"move_next_long_word_end", KOI_CMD({ DoMove(ed, Motion::kLongWordEnd, false); }), "move to the WORD end"},
    {"move_next_long_word_start", KOI_CMD({ DoMove(ed, Motion::kLongWordNext, false); }), "move to the next WORD"},
    {"move_next_word_end", KOI_CMD({ DoMove(ed, Motion::kWordEnd, false); }), "move to the word end"},
    {"move_next_word_start", KOI_CMD({ DoMove(ed, Motion::kWordNext, false); }), "move to the next word"},
    {"move_prev_long_word_end", KOI_CMD({ DoMove(ed, Motion::kLongWordPrevEnd, false); }), "move to the previous WORD end"},
    {"move_prev_long_word_start", KOI_CMD({ DoMove(ed, Motion::kLongWordPrev, false); }), "move to the previous WORD"},
    {"move_prev_word_end", KOI_CMD({ DoMove(ed, Motion::kWordPrevEnd, false); }), "move to the previous word end"},
    {"move_prev_word_start", KOI_CMD({ DoMove(ed, Motion::kWordPrev, false); }), "move to the previous word"},
    {"move_visual_line_down", KOI_CMD({ DoMove(ed, Motion::kDown, false); }), "move down a line"},
    {"move_visual_line_up", KOI_CMD({ DoMove(ed, Motion::kUp, false); }), "move up a line"},
    {"no_op", KOI_CMD({ (void)ed; }), "do nothing"},
    {"normal_mode", KOI_CMD({ LeaveInsertMode(ed); }), "leave insert mode"},
    {"open_above", KOI_CMD({ OpenLine(ed, false); }), "open a line above and insert"},
    {"open_below", KOI_CMD({ OpenLine(ed, true); }), "open a line below and insert"},
    {"page_cursor_half_down", KOI_CMD({ VerticalPage(ed, true, ed.doc.view.rows / 2, false); }), "half a page down"},
    {"page_cursor_half_up", KOI_CMD({ VerticalPage(ed, false, ed.doc.view.rows / 2, false); }), "half a page up"},
    {"page_down", KOI_CMD({ VerticalPage(ed, true, ed.doc.view.rows, false); }), "a page down"},
    {"page_up", KOI_CMD({ VerticalPage(ed, false, ed.doc.view.rows, false); }), "a page up"},
    {"paste_after", KOI_CMD({ Paste(ed, true); }), "paste after the selection"},
    {"paste_before", KOI_CMD({ Paste(ed, false); }), "paste before the selection"},
    {"paste_clipboard_after", KOI_CMD({ PasteFromClipboard(ed, true, false); }), "paste the system clipboard after the selection"},
    {"paste_clipboard_before", KOI_CMD({ PasteFromClipboard(ed, false, false); }), "paste the system clipboard before the selection"},
    {"redo", KOI_CMD({ UndoOrRedo(ed, false); }), "redo"},
    {"remove_primary_selection", KOI_CMD({
       if (ed.doc.selections.Size() > 1) {
         auto ranges = ed.doc.selections.Ranges();
         ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(ed.doc.selections.PrimaryIndex()));
         ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
       }
     }), "drop the primary cursor"},
    {"replace", KOI_CMD({ ArmPendingChar(ed, PendingChar::kReplaceChar, false, "replace with..."); }), "overwrite the selection with a character"},
    {"replace_with_yanked", KOI_CMD({ ReplaceWithYanked(ed); }), "replace the selection with the yanked text"},
    {"rotate_selection_contents_backward", KOI_CMD({ RotateContents(ed, false); }), "move the selected text one selection back"},
    {"rotate_selection_contents_forward", KOI_CMD({ RotateContents(ed, true); }), "move the selected text one selection on"},
    {"rotate_selections_backward", KOI_CMD({ ed.doc.selections.RotatePrimary(-1); }), "make the previous cursor primary"},
    {"rotate_selections_forward", KOI_CMD({ ed.doc.selections.RotatePrimary(1); }), "make the next cursor primary"},
    {"save_selection", KOI_CMD({ RecordJump(ed); ed.status.clear(); }), "add this position to the jump list by hand"},
    {"scroll_down", KOI_CMD({ ScrollBy(ed, CountOr(ed, 1)); }), "scroll the view down a line"},
    {"scroll_up", KOI_CMD({ ScrollBy(ed, -CountOr(ed, 1)); }), "scroll the view up a line"},
    {"search", KOI_CMD({ PromptOpen(ed, PromptKind::kSearch); }), "search for a regex"},
    {"search_excerpts", KOI_CMD({ PromptOpen(ed, PromptKind::kSearchExcerpts); }), "search the project, as excerpts in one view"},
    {"search_next", KOI_CMD({ SearchStep(ed, true); }), "next match"},
    {"search_prev", KOI_CMD({ SearchStep(ed, false); }), "previous match"},
    {"select_all", KOI_CMD({ ed.doc.selections.Set(Selection{0, DocLength(ed.doc.table), -1}); }), "select the whole file"},
    {"select_regex", KOI_CMD({ PromptOpen(ed, PromptKind::kSelectRegex); }), "select every match inside the selections"},
    {"select_textobject_around", KOI_CMD({ ArmPendingChar(ed, PendingChar::kTextObjectAround, false, "select around..."); }), "select around a pair or word"},
    {"select_textobject_inner", KOI_CMD({ ArmPendingChar(ed, PendingChar::kTextObjectInner, false, "select inside..."); }), "select inside a pair or word"},
    {"show_definition_excerpts", KOI_CMD({ ShowDefinitionExcerpts(ed); }), "every definition of the selection, as excerpts in one view"},
    {"show_reference_excerpts", KOI_CMD({ ShowReferenceExcerpts(ed); }), "every use of the selection, as excerpts in one view"},
    {"show_references", KOI_CMD({ ShowReferences(ed); }), "every use of the selection"},
    {"shrink_height", KOI_CMD({ ResizeFocusedPane(ed, ResizeAxis::kHeight, false); }), "make this window a tenth shorter"},
    {"shrink_width", KOI_CMD({ ResizeFocusedPane(ed, ResizeAxis::kWidth, false); }), "make this window a tenth narrower"},
    {"split_selection", KOI_CMD({ ArmPendingChar(ed, PendingChar::kSplitOn, false, "split on char..."); }), "split every selection on a character"},
    {"split_selection_on_newline", KOI_CMD({ SplitOnNewlines(ed); }), "one cursor per selected line"},
    {"surround_add", KOI_CMD({ ArmPendingChar(ed, PendingChar::kSurroundAdd, false, "surround with..."); }), "wrap the selection in a pair"},
    {"surround_delete", KOI_CMD({ ArmPendingChar(ed, PendingChar::kSurroundDelete, false, "delete surrounding..."); }), "remove the enclosing pair"},
    {"surround_replace", KOI_CMD({ ArmPendingChar(ed, PendingChar::kSurroundReplaceFrom, false, "replace surrounding..."); }), "swap the enclosing pair for another"},
    {"swap_view_down", KOI_CMD({ SwapWindow(ed, WindowDir::kDown); }), "exchange this window with the one below"},
    {"swap_view_left", KOI_CMD({ SwapWindow(ed, WindowDir::kLeft); }), "exchange this window with the one to the left"},
    {"swap_view_right", KOI_CMD({ SwapWindow(ed, WindowDir::kRight); }), "exchange this window with the one to the right"},
    {"swap_view_up", KOI_CMD({ SwapWindow(ed, WindowDir::kUp); }), "exchange this window with the one above"},
    {"switch_case", KOI_CMD({ TransformSelections(ed, SwitchCase); }), "invert the case of the selection"},
    {"switch_to_lowercase", KOI_CMD({ TransformSelections(ed, AsciiLower); }), "lowercase the selection"},
    {"switch_to_uppercase", KOI_CMD({ TransformSelections(ed, AsciiUpper); }), "uppercase the selection"},
    {"symbol_picker", KOI_CMD({ SymbolPicker(ed); }), "every definition in the project"},
    {"till_prev_char", KOI_CMD({ ArmFindChar(ed, PendingChar::kTillPrev, false); }), "move back to after the previous occurrence"},
    {"toggle_comments", KOI_CMD({ ToggleComments(ed); }), "comment or uncomment the selected lines"},
    {"toggle_soft_wrap", KOI_CMD({
       ed.settings.soft_wrap = !ed.settings.soft_wrap;
       if (ed.settings.soft_wrap) ed.doc.view.left_column = 0;
       ed.status = ed.settings.soft_wrap ? "soft wrap on" : "soft wrap off";
     }), "turn soft wrap on or off"},
    {"transpose_view", KOI_CMD({ TransposeWindow(ed); }), "turn this split the other way"},
    {"trim_selections", KOI_CMD({
       auto ranges = ed.doc.selections.Ranges();
       for (Selection& s : ranges) {
         if (s.IsEmpty()) continue;
         const std::string text = ReadDocRange(ed.doc.table, s.Range());
         Index lead = 0, trail = 0;
         while ((lead < std::ssize(text)) && (std::isspace(static_cast<unsigned char>(text[lead])) != 0)) ++lead;
         while ((trail < std::ssize(text) - lead) && (std::isspace(static_cast<unsigned char>(text[std::ssize(text) - 1 - trail])) != 0)) ++trail;
         const Index lo = s.From() + lead, hi = s.To() - trail;
         if (lo >= hi) continue;
         const bool backwards = s.Backward();
         s.anchor = backwards ? hi : lo;
         s.head = backwards ? lo : hi;
       }
       ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
     }), "drop whitespace from the selection edges"},
    {"undo", KOI_CMD({ UndoOrRedo(ed, true); }), "undo"},
    {"unindent", KOI_CMD({ IndentBy(ed, false); }), "unindent the selected lines"},
    {"vsplit", KOI_CMD({ SplitWindow(ed, true); }), "split this window left and right"},
    {"wclose", KOI_CMD({ CloseWindow(ed); }), "close this window"},
    {"wonly", KOI_CMD({ CloseOtherWindows(ed); }), "close every window but this one"},
    {"yank", KOI_CMD({ YankSelections(ed); }), "yank the selection"},
    {"yank_main_selection_to_clipboard", KOI_CMD({ YankToClipboard(ed, true); }), "yank the primary selection to the system clipboard"},
    {"yank_to_clipboard", KOI_CMD({ YankToClipboard(ed, false); }), "yank every selection to the system clipboard, one per line"},
});

static_assert(std::ranges::is_sorted(kCommands, {}, &CommandDef::name),
              "kCommands must stay sorted by name");

constexpr std::array kUnimplemented = std::to_array<std::string_view>({
    "format_selections",
    "rsearch", "repeat_last_motion",
    "hover", "completion", "signature_help", "smart_tab",
    "increment", "decrement",
});

std::string DescribeSequence(const std::vector<Key>& keys) {
  std::string out;
  for (const Key& k : keys) {
    if (!out.empty()) out += ' ';
    out += KeyToString(k);
  }
  return out;
}

const KeyMap& MapFor(const KeyMaps& maps, Mode mode) {
  return (mode == Mode::kInsert) ? maps.insert : maps.normal;
}

bool IsCountDigit(const Editor& ed, const std::vector<Key>& pending, const Key& key) {
  if (ed.mode != Mode::kNormal) return false;
  if (!pending.empty()) return false;
  if ((key.named != NamedKey::kNone) || (key.mods != kModNone)) return false;
  if ((key.code < '0') || (key.code > '9')) return false;
  return (key.code > '0') || (ed.pending_count > 0);
}

}

bool IsSelfInsert(const Key& key) {
  return (key.named == NamedKey::kNone) && (key.code >= 0x20) && (key.code != 0x7F) &&
         ((key.mods & (kModCtrl | kModAlt)) == 0);
}

std::string KeyText(const Key& key) {
  std::string out;
  AppendUtf8(out, key.code);
  return out;
}

std::string_view AutoPairClose(std::string_view opener) {
  if (opener == "(") return ")";
  if (opener == "[") return "]";
  if (opener == "{") return "}";
  if (opener == "\"") return "\"";
  if (opener == "'") return "'";
  if (opener == "`") return "`";
  return {};
}

bool IsAutoPairQuote(std::string_view s) { return (s == "\"") || (s == "'") || (s == "`"); }

bool ClosesAutoPair(std::string_view s) {
  return (s == ")") || (s == "]") || (s == "}") || IsAutoPairQuote(s);
}

bool IsWordByte(char c) {
  const auto u = static_cast<unsigned char>(c);
  return (u >= 0x80) || (u == '_') || ((u >= '0') && (u <= '9')) || ((u >= 'a') && (u <= 'z')) ||
         ((u >= 'A') && (u <= 'Z'));
}

bool ShouldPair(const Editor& ed, std::string_view typed, Index cursor, std::string_view next) {
  if (!next.empty() && IsWordByte(next.front())) return false;

  if (IsAutoPairQuote(typed)) {
    const Index prev = PrevGraphemeBoundary(ed.doc.table, cursor);
    if (prev < cursor) {
      const std::string before = ReadDocRange(ed.doc.table, Interval(prev, cursor));
      if (!before.empty() && IsWordByte(before.front())) return false;
    }
  }
  return true;
}

namespace {

// -- re-indent on type --------------------------------------------------------
//
// Vim has always kept two mechanisms apart: computing a line's indent, and
// deciding which keystroke should recompute it (`indentkeys`). indent.cpp is the
// first; this is the second, and without it a typed `}` lands wherever the line
// above put it and nothing ever brings it back.
//
// The trigger is deliberately narrow, because it moves text the user did not ask
// to move. What fires it is the keystroke that *completes* the token, which is
// vim's `indentkeys` semantics: the line is a single run of non-blank bytes --
// whitespace, one token, then nothing but whitespace -- the keystroke put a
// non-blank byte on it, the caret is exactly at that token's end, and the tree
// says the token is what dedents the line. Middle-of-line typing is never
// touched, because a caret with code in front of it fails the single-run test;
// nor is a plain statement on a fresh line, because its first token carries no
// `@outdent` and the editor has no business having an opinion about where the
// user is putting it. Nor is a line the caret has already walked away from: a
// space typed after a `}` leaves the caret past the token and is the user saying
// they are done with it, not asking for the column back.
//
// The exception is a line this code has already moved: `else` dedents and
// `elsewhere` does not, so the `w` has to be able to put back what the `e` took.
// ReindentMemory (editor.h) is that and only that.

// The line's leading whitespace and the token that follows it, in offsets. This
// is the cheap half of the trigger, and it is all a keystroke that does not
// qualify ever pays: a scan of one line, no query, no tree.
struct LeadingToken {
  Index start{0};    // first byte of the line
  Index content{0};  // first non-blank byte, == start when there is no indent
  Index end{0};      // one past the last byte of the first non-blank run
  // The run is the only non-blank text on the line -- everything after it up to
  // the line's end is blank.
  bool sole{false};
};

bool IsBlank(char c) { return (c == ' ') || (c == '\t') || (c == '\r'); }

LeadingToken MeasureLeadingToken(const PieceTable& table, Index line) {
  LeadingToken out;
  out.start = LineStart(table, line);
  const Index line_end = LineContentEnd(table, line);

  char c = 0;
  out.content = out.start;
  while ((out.content < line_end) && ByteAt(table, out.content, c) && IsBlank(c)) ++out.content;
  out.end = out.content;
  while ((out.end < line_end) && ByteAt(table, out.end, c) && !IsBlank(c)) ++out.end;

  Index after = out.end;
  while ((after < line_end) && ByteAt(table, after, c) && IsBlank(c)) ++after;
  out.sole = (out.content < out.end) && (after == line_end);
  return out;
}

// Longer than any token a query outdents on -- `default:` is the longest in the
// vendored corpus at eight bytes -- and short enough that typing an identifier at
// the start of a line stops costing a query after a few keystrokes.
constexpr Index kMaxOutdentToken = 32;

bool TypedAtLeadingToken(const LeadingToken& token, Index cursor) {
  if (!token.sole) return false;
  // Exactly at the token's end: the caret is finishing the token, not sitting in
  // front of it and not standing off in the blanks behind it. Each grapheme of
  // `else` -- and of the `elsewhere` that has to undo it -- lands the caret at
  // the new end, so growing a token keeps firing; the auto-pairs skip-over lands
  // there too. Past the end is a line whose token is finished, and re-indenting
  // it is how a column somebody placed by hand gets taken away from them.
  if (cursor != token.end) return false;
  return (token.end - token.content) <= kMaxOutdentToken;
}

// Whether any caret is standing strictly inside the line's leading whitespace.
// Those bytes are that caret's property this keystroke -- typing at a caret in
// the indent is what put them there -- and replacing the run wholesale maps
// every position inside it onto the line's start, where Normalize merges the
// now-coincident carets and the input they just took goes with the whitespace.
// A position exactly at either edge survives the mapping intact and is fine;
// the caret that owns the re-indent is at or past the token's end, so it can
// never be the one that trips this.
bool CaretInsideIndent(std::span<const Selection> ranges, const LeadingToken& token) {
  const auto inside = [&token](Index pos) {
    return (pos > token.start) && (pos < token.content);
  };
  return std::ranges::any_of(
      ranges, [&inside](const Selection& s) { return inside(s.anchor) || inside(s.head); });
}

const ReindentedLine* RememberedLine(const ReindentMemory& memory, Index line,
                                     std::string_view leading) {
  for (const ReindentedLine& one : memory.lines) {
    // Still standing where this code left it. Anything else on that line is
    // somebody else's edit, and the memory of it is not ours to act on.
    if ((one.line == line) && (one.written == leading)) return &one;
  }
  return nullptr;
}

// Runs after the keystroke's own insertion and inside its UndoGroup, so a typed
// `}` and the dedent it causes are one undo step rather than two.
void ReindentTypedLines(Editor& ed, const ReindentMemory& memory) {
  if (ed.doc.syntax == nullptr) return;
  if (!HasIndentQuery(ed.doc.syntax->Language())) return;

  // The cheap half, run over the carets where they sit. Nearly every keystroke
  // lands with code in front of it and is ruled out here by a scan of one line --
  // no copy of the selections, no query, no tree. Everything below this line is
  // paid for only by a caret that is finishing the first token on its line.
  const auto candidate = [&ed](const Selection& s) {
    const Index cursor = CursorOf(ed.doc.table, s);
    const LeadingToken token = MeasureLeadingToken(ed.doc.table, LineAt(ed.doc.table, cursor));
    return TypedAtLeadingToken(token, cursor);
  };
  if (!std::ranges::any_of(ed.doc.selections.Ranges(), candidate)) return;

  const IndentStyle style{ed.doc.tab_width, ed.doc.insert_spaces};
  auto ranges = ed.doc.selections.Ranges();
  ReindentMemory next;
  next.document = ed.doc.id;
  bool moved = false;
  std::string error;
  // Before the clock is read, exactly as the newline path syncs before its loop:
  // the first query would otherwise run the parse of the edit that just landed
  // *inside* the first caret's share of the budget, and on a buffer whose
  // incremental parse is itself tens of milliseconds that caret would spend the
  // whole keystroke's budget on work every other caret then benefits from. The
  // parse has budgets of its own; this one is for queries. Free when a paint has
  // already synced this revision, which between keystrokes it has.
  ed.doc.syntax->Sync(ed.doc.table);
  // One deadline for the keystroke, as on the newline path: twenty carets on
  // twenty qualifying lines used to be twenty budgets, and a `}` typed at all of
  // them in a deeply nested buffer measured 261 ms of query for one keystroke.
  // See kIndentBudget.
  const auto deadline = std::chrono::steady_clock::now() + kIndentBudget;
  // Lines this keystroke has already had an answer for. Two carets can qualify
  // on one line -- both standing at the end of its leading token, which two
  // selections with different anchors can do without Normalize merging them --
  // and the second of them would run the whole query again to be told what the
  // first was told, then push a second memory entry for the same line. One
  // query per line per keystroke, and one entry.
  std::vector<Index> answered;

  // Back to front, like every other multi-cursor edit here: a line's indent
  // changing shifts the byte offsets of every caret below it, and MapLaterRanges
  // is what carries the ones already visited.
  for (std::size_t i = ranges.size(); i-- > 0;) {
    const Index cursor = CursorOf(ed.doc.table, ranges[i]);
    const Index line = LineAt(ed.doc.table, cursor);
    const LeadingToken token = MeasureLeadingToken(ed.doc.table, line);
    if (!TypedAtLeadingToken(token, cursor)) continue;
    if (std::ranges::find(answered, line) != answered.end()) continue;
    answered.push_back(line);

    const std::string leading = ReadDocRange(ed.doc.table, Interval(token.start, token.content));
    const ReindentedLine* was = RememberedLine(memory, line, leading);

    // Still a cheap precondition -- a scan of the selections, no query and no
    // tree. A line somebody else's caret is sitting in the indent of is refused
    // outright rather than remapped through: an indent is not worth a silently
    // merged cursor and the bytes it had just typed. As on the decline path, a
    // line already moved keeps its memory so a later keystroke can put it back.
    if (CaretInsideIndent(ranges, token)) {
      if (was != nullptr) next.lines.push_back(*was);
      continue;
    }

    // What is left of the keystroke's budget, and nothing at all once it is
    // spent: a caret past the deadline declines exactly as one whose query ran
    // out does, which is the branch below.
    const std::chrono::milliseconds left = RemainingIndentBudget(deadline);
    bool outdent_token = false;
    const std::optional<std::string> tree =
        (left > std::chrono::milliseconds::zero())
            ? TreeIndentForLine(ed.doc.table, *ed.doc.syntax, line, style, error, left,
                                &outdent_token)
            : std::nullopt;
    // Declined -- no `indents.scm`, an injected region, a query that will not
    // compile, a budget spent. Nothing is known about this line so nothing is
    // done to it, and the error is swallowed: the newline path is where a broken
    // query gets said out loud, and saying it once per keystroke is not a status
    // line, it is a stutter. A line already moved keeps its memory, so the
    // keystroke after this one can still put it back.
    if (!tree.has_value()) {
      if (was != nullptr) next.lines.push_back(*was);
      continue;
    }

    const std::string* want = nullptr;
    if (outdent_token) {
      want = &*tree;
    } else if (was != nullptr) {
      // The token grew out of being an outdent. Back to exactly what was there,
      // not to the tree's answer for it: this code moved the line because the
      // token dedented it, and with that gone it has no licence to hold an
      // opinion about a line somebody typed by hand.
      want = &was->original;
    }
    if (want == nullptr) continue;

    const std::string& original = (was != nullptr) ? was->original : leading;
    if (*want != leading) {
      Edit edit;
      const std::string text = *want;
      if (!Edited(ed, [&] {
            return Replace(text, token.start, token.content, ed.doc.table, &edit);
          })) {
        return;
      }
      // Every range and not just the later ones, which is where this differs
      // from its neighbours: their edit lands *at* the caret that owns it, so
      // only the ranges past it move. This one lands at the head of the line,
      // behind every caret on it -- a second caret further along the same line
      // is earlier in `ranges` and still sits after the bytes just replaced.
      // Mapping the whole list is safe for the rest: a position before the edit
      // maps to itself.
      for (Selection& s : ranges) {
        const Index anchor = MapPositionAfter(s.anchor, edit);
        const Index head = MapPositionAfter(s.head, edit);
        // A goal column is a column, and a caret the remap moved is in a new
        // one: the whole point of the edit is that the line's indent changed
        // width. Every caret on the moved line has to lose it, not just the one
        // that owns the keystroke -- a second caret further along the same line
        // kept the column it had before the dedent, and the next Up or Down
        // jumped it back there. Positions the mapping left alone -- everything
        // above the edit -- keep the goal they had; nothing moved them.
        if ((anchor != s.anchor) || (head != s.head)) s.goal_column = -1;
        s.anchor = anchor;
        s.head = head;
      }
      // The owning caret unconditionally, even where the replacement happened to
      // be the same length: its line is the one that changed shape.
      ranges[i].goal_column = -1;
      moved = true;
    }
    // Remembered exactly while the line stands somewhere this code put it. Back
    // at its original -- which is the common case of typing a `}` whose column
    // was already right -- there is nothing left to undo and nothing to keep.
    if (*want != original) next.lines.push_back(ReindentedLine{line, original, *want});
  }

  if (moved) ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
  next.revision = ed.doc.table.revision;
  if (!next.lines.empty()) ed.reindent = std::move(next);
}

}

void TypeOneKey(Editor& ed, const std::string& typed) {
  auto ranges = ed.doc.selections.Ranges();
  const std::string_view close = AutoPairClose(typed);

  for (size_t i = ranges.size(); i-- > 0;) {
    const Index cursor = CursorOf(ed.doc.table, ranges[i]);
    const Index after = NextGraphemeBoundary(ed.doc.table, cursor);
    const std::string next =
        (after > cursor) ? ReadDocRange(ed.doc.table, Interval(cursor, after)) : std::string{};

    if (ClosesAutoPair(typed) && (next == typed)) {
      ranges[i].anchor = after;
      ranges[i].head = after;
      ranges[i].goal_column = -1;
      continue;
    }

    std::string insert = typed;
    if (!close.empty() && ShouldPair(ed, typed, cursor, next)) insert += close;

    Edit edit;
    if (!Edited(ed, [&] { return Insert(insert, cursor, ed.doc.table, &edit); })) break;

    const Index landing = edit.start_byte + std::ssize(typed);
    ranges[i].anchor = landing;
    ranges[i].head = landing;
    ranges[i].goal_column = -1;
    MapLaterRanges(ranges, i, edit);
  }

  ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
}

void FlushPendingAsText(Editor& ed, std::vector<Key>& pending) {
  UndoGroup group(ed.doc.table);
  std::string text;

  // Read before a single byte lands, because the insertion below moves the very
  // revision the memory is keyed on: what has to be decided is whether the
  // keystroke that set it was this one's immediate predecessor.
  ReindentMemory memory;
  if ((ed.reindent.document == ed.doc.id) && (ed.reindent.revision == ed.doc.table.revision)) {
    memory = std::move(ed.reindent);
  }
  ed.reindent = {};

  const auto flush_plain = [&] {
    if (text.empty()) return;
    Edited(ed, [&] { return InsertAtCursorsKeeping(text, ed.doc.table, ed.doc.selections); });
    text.clear();
  };

  bool typed_non_blank = false;
  for (const Key& k : pending) {
    if (!IsSelfInsert(k)) continue;
    const std::string typed = KeyText(k);
    // Only a keystroke that puts a non-blank byte on the line can be the one
    // that completes a token, so this is the rule said out loud rather than left
    // to fall out of the caret test. It falls out of it as well -- the trigger
    // is judged after the insertion, and a blank pushes the caret past the end
    // it would have to be standing on -- which is why nothing observable rests
    // on this line; what it buys is the per-caret line scan a held-down space
    // would otherwise pay on every repeat.
    if (!std::ranges::all_of(typed, IsBlank)) typed_non_blank = true;
    if (ed.settings.auto_pairs && (!AutoPairClose(typed).empty() || ClosesAutoPair(typed))) {
      flush_plain();
      TypeOneKey(ed, typed);
      continue;
    }
    text += typed;
  }
  flush_plain();
  // The one chokepoint both typing paths pass through: the auto-pairs one above,
  // including the skip-over that only moves a caret, and the batched plain
  // inserts that a `}` typed with auto-pairs off arrives on.
  if (typed_non_blank) ReindentTypedLines(ed, memory);
  pending.clear();
}

namespace {

class Recording {
 public:
  Recording(Editor& ed, const Key& key, const std::vector<Key>& prefix)
      : recorder_{ed.recorder.get()} {
    if (recorder_ != nullptr) recorder_->Begin(ed, key, prefix);
  }
  ~Recording() { Resolve(KeyOutcome::kOther); }

  Recording(const Recording&) = delete;
  Recording& operator=(const Recording&) = delete;

  void Resolve(KeyOutcome outcome, const std::vector<std::string>* commands = nullptr) {
    if (recorder_ == nullptr) return;
    recorder_->Resolve(outcome, commands);
    recorder_ = nullptr;
  }

  void Abandon() {
    if (recorder_ == nullptr) return;
    recorder_->Abandon();
    recorder_ = nullptr;
  }

 private:
  KeyRecorder* recorder_;
};

}

void SearchPreviewJump(Editor& ed) {
  if (!ed.prompt_active || (ed.prompt_kind != PromptKind::kSearch)) return;
  PromptRestoreSearchOrigin(ed);
  if (ed.prompt_input.empty()) return;
  const Index cursor = CursorOf(ed.doc.table, ed.doc.selections.Primary());
  std::optional<Interval> landed;
  std::string error;
  if (!FindFirstInDocument(ed.doc.table, ed.prompt_input, cursor, landed, error) || !landed) {
    return;
  }
  ed.doc.selections.Set(CoveringSelection(ed.doc.table, landed->front(), landed->back() + 1));
}

void HandleKeyInput(Editor& ed, const KeyMaps& maps, const Key& key, std::vector<Key>& pending) {
  ed.status_overlay = false;

  Recording recording{ed, key, pending};

  if (ed.prompt_active) {
    recording.Resolve(KeyOutcome::kPrompt);
    pending.clear();
    if (key.mods == kModNone) {
      switch (key.named) {
        case NamedKey::kEsc: PromptCancel(ed); return;
        case NamedKey::kRet: PromptSubmit(ed); return;
        case NamedKey::kBackspace:
          if (ed.prompt_input.empty()) {
            PromptCancel(ed);
          } else {
            PromptBackspace(ed);
            SearchPreviewJump(ed);
          }
          return;
        case NamedKey::kDelete:
          PromptDeleteForward(ed);
          SearchPreviewJump(ed);
          return;
        case NamedKey::kLeft: PromptMoveLeft(ed); return;
        case NamedKey::kRight: PromptMoveRight(ed); return;
        case NamedKey::kUp:
          PromptHistory(ed, true);
          SearchPreviewJump(ed);
          return;
        case NamedKey::kDown:
          PromptHistory(ed, false);
          SearchPreviewJump(ed);
          return;
        case NamedKey::kHome: PromptHome(ed); return;
        case NamedKey::kEnd: PromptEnd(ed); return;
        default: break;
      }
    }
    if ((key.named == NamedKey::kTab) && (key.mods == kModNone)) {
      if (ed.prompt_kind == PromptKind::kCommand) {
        PromptComplete(ed);
      } else {
        PromptInsert(ed, "\t");
        SearchPreviewJump(ed);
      }
      return;
    }
    if ((key.mods == kModCtrl) && (key.code == 'u')) {
      ed.prompt_input.erase(0, ed.prompt_cursor);
      ed.prompt_cursor = 0;
      SearchPreviewJump(ed);
      return;
    }
    if (IsSelfInsert(key)) {
      PromptInsert(ed, KeyText(key));
      SearchPreviewJump(ed);
    }
    return;
  }

  if (ed.pending_char != PendingChar::kNone) {
    recording.Resolve(KeyOutcome::kPendingChar);
    pending.clear();
    if (key.named == NamedKey::kEsc) {
      ApplyPendingChar(ed, {});
      ed.status = "cancelled";
      return;
    }
    // Alt+letter at the label stage is "pick this one". It arrives as a
    // modifier chord rather than a character, so the key path spells it to
    // the step as the pick prefix plus the letter -- a control byte
    // IsSelfInsert refuses, so nothing typed can arrive looking like it.
    if ((ed.pending_char == PendingChar::kLeapLabel) && (key.mods == kModAlt) &&
        (key.named == NamedKey::kNone) && (key.code < 128)) {
      const char c = static_cast<char>(key.code);
      const char lower = ((c >= 'A') && (c <= 'Z')) ? static_cast<char>(c - 'A' + 'a') : c;
      if ((lower >= 'a') && (lower <= 'z')) {
        ApplyPendingChar(ed, std::string{kLeapPickPrefix} + lower);
        return;
      }
    }
    if (key.named == NamedKey::kRet) {
      ApplyPendingChar(ed, "\n");
      return;
    }
    if (key.named == NamedKey::kTab) {
      ApplyPendingChar(ed, "\t");
      return;
    }
    if (IsSelfInsert(key)) {
      ApplyPendingChar(ed, KeyText(key));
    } else {
      // A leap step has already said what happened, in the vocabulary of the
      // stage it ended: "not a character" answers a question nobody asked
      // about a left arrow at a stage whose alphabet is label keys.
      const bool leaping = IsLeapPending(ed.pending_char);
      ApplyPendingChar(ed, {});
      if (!leaping) ed.status = "not a character";
    }
    return;
  }

  if (IsCountDigit(ed, pending, key)) {
    recording.Resolve(KeyOutcome::kCount);
    if (ed.pending_count < 100000) {
      ed.pending_count = ed.pending_count * 10 + static_cast<Index>(key.code - '0');
    }
    return;
  }

  const Mode mode_before = ed.mode;
  pending.push_back(key);

  const std::vector<std::string>* commands = nullptr;
  switch (MapFor(maps, mode_before).Find(pending, &commands)) {
    case KeyMap::Lookup::kPending:
      recording.Resolve(KeyOutcome::kPending);
      ed.status = DescribeSequence(pending) + "...";
      return;
    case KeyMap::Lookup::kMatched: {
      const std::vector<std::string> to_run = *commands;
      recording.Resolve(KeyOutcome::kBinding, &to_run);
      pending.clear();
      RunCommands(ed, to_run);
      return;
    }
    case KeyMap::Lookup::kNoMatch:
      break;
  }

  if (mode_before == Mode::kInsert) {
    const bool all_text = std::ranges::all_of(pending, IsSelfInsert);
    if (all_text) {
      recording.Resolve(KeyOutcome::kInsertText);
      FlushPendingAsText(ed, pending);
      ed.pending_count = 0;
      return;
    }
    if (pending.size() > 1) {
      std::vector<Key> typed(pending.begin(), pending.end() - 1);
      FlushPendingAsText(ed, typed);
      const Key abandoning = pending.back();
      pending.clear();
      ed.pending_count = 0;

      recording.Abandon();
      HandleKeyInput(ed, maps, abandoning, pending);
      return;
    }
  }
  recording.Resolve(KeyOutcome::kUnbound);
  ed.status.Warn(DescribeSequence(pending) + " is not bound");
  pending.clear();
  ed.pending_count = 0;
}

const CommandDef* FindCommand(std::string_view name) {
  const auto it = std::ranges::lower_bound(kCommands, name, {}, &CommandDef::name);
  if ((it != kCommands.end()) && (it->name == name)) return &*it;
  return nullptr;
}

std::span<const CommandDef> AllCommands() { return kCommands; }

bool IsKnownUnimplemented(std::string_view name) {
  return std::ranges::find(kUnimplemented, name) != kUnimplemented.end();
}

// -- leap --------------------------------------------------------------------

namespace {

constexpr std::string_view kLeapArmHint = "leap: type two characters";

// The visible text of the focused pane, one range per document line, in
// document order and never crossing a line break -- which is also the rule
// that a pair may not straddle one.
//
// The metrics come from the renderer's own helper rather than being rebuilt
// here: they have to be the ones the focused pane is drawn with, down to how
// the wrap indicator is measured, or the row boundaries this walks are not the
// boundaries on the screen and the leap offers -- or refuses -- text by a
// layout nobody is looking at. Reading them off the viewport also means a leap
// armed in a test that set the viewport by hand sees the same pane.
void LeapVisibleRanges(const Editor& ed, std::vector<Interval>& out) {
  out.clear();
  const PieceTable& table = ed.doc.table;
  const Viewport& view = ed.doc.view;
  const Index lines = LineCount(table);
  if ((lines <= 0) || (view.rows <= 0) || (view.columns <= 0)) return;

  const WrapMetrics wrap = WrapForFocusedViewport(ed);

  std::vector<Index> row_starts;
  std::string scratch;
  Index row_y = wrap.enabled ? -std::max<Index>(0, view.top_row) : 0;
  for (Index line = std::max<Index>(0, view.top_line); (line < lines) && (row_y < view.rows);
       ++line) {
    LayoutLine(table, line, wrap, row_starts, scratch);
    const Index start = LineStart(table, line);
    const Interval content = LineContentRange(table, line);
    const Index end = content.empty() ? start : (content.back() + 1);

    // Which rows of this line the pane really shows. Under wrap the top line
    // can be scrolled part-way off and the bottom one runs off the end, and
    // the rows that survive are contiguous bytes -- so they stay one range,
    // and a pair broken across a wrapped row is still a pair.
    Index from = -1;
    Index to = -1;
    for (Index r = 0; r < std::ssize(row_starts); ++r) {
      if (((row_y + r) < 0) || ((row_y + r) >= view.rows)) continue;
      if (from < 0) from = row_starts[static_cast<std::size_t>(r)];
      to = ((r + 1) < std::ssize(row_starts)) ? row_starts[static_cast<std::size_t>(r) + 1] : end;
    }
    row_y += std::ssize(row_starts);
    if (from < 0) continue;

    // Without wrap the pane is a window on the columns too, and text scrolled
    // off either side is no more visible than a line scrolled off the top.
    if (!wrap.enabled) {
      const Index left = std::max<Index>(0, view.left_column);
      const Index limit = left + view.columns;
      from = std::max(from, ByteForColumnFrom(table, start, end, left, ed.doc.tab_width));
      Index right = ByteForColumnFrom(table, start, end, limit, ed.doc.tab_width);
      // ByteForColumnFrom stops at the first cluster that *reaches* the limit,
      // so the last one it steps over can straddle the edge. DrawLine refuses
      // to draw half a wide glyph and paints a blank in its place, and a blank
      // column is not text the leap may offer. A tab is the exception: its
      // first column is drawn whatever else falls off the pane.
      if ((right > from) &&
          (ColumnForByteFrom(table, start, right, ed.doc.tab_width) > limit)) {
        const Index prev = PrevGraphemeBoundary(table, right);
        char byte = 0;
        if (!ByteAt(table, prev, byte) || (byte != '\t')) right = prev;
      }
      to = std::min(to, right);
    }
    if (to > from) out.push_back(Interval(from, to));
  }
}

// Every position in `visible` where `first` sits, and -- when `second` is
// given -- is immediately followed by it. Overlapping pairs count at every
// start, so the walk steps one grapheme past a hit rather than past the pair.
void LeapScan(const Editor& ed, const std::vector<Interval>& visible, std::string_view first,
              std::string_view second, std::vector<Index>& hits, std::vector<Interval>& spans) {
  hits.clear();
  spans.clear();
  if (first.empty()) return;
  const PieceTable& table = ed.doc.table;
  std::string one;
  std::string two;
  for (const Interval& range : visible) {
    if (range.empty()) continue;
    const Index end = range.back() + 1;
    for (Index at = range.front(); at < end;) {
      const Index next = NextGraphemeBoundary(table, at);
      if ((next <= at) || (next > end)) break;
      ReadDocRangeInto(table, Interval(at, next), one);
      if (one != first) {
        at = next;
        continue;
      }
      Index stop = next;
      if (!second.empty()) {
        const Index after = NextGraphemeBoundary(table, next);
        if ((after <= next) || (after > end)) break;
        ReadDocRangeInto(table, Interval(next, after), two);
        if (two != second) {
          at = next;
          continue;
        }
        stop = after;
      }
      hits.push_back(at);
      if (!spans.empty() && ((spans.back().back() + 1) >= at)) {
        spans.back() = Interval(spans.back().front(), std::max(spans.back().back() + 1, stop));
      } else {
        spans.push_back(Interval(at, stop));
      }
      at = next;
    }
  }
}

// Byte distance from the caret, which is what "nearest" means here: a display
// distance would have to weigh a row against a column, and over the one pane
// this is measured in the two answers agree about everything except which of
// two matches on different lines is closer. Equal distances take the earlier
// position, so the order is total and a label always means the same match.
void LeapOrderByDistance(std::vector<Index>& matches, Index caret) {
  std::ranges::sort(matches, [caret](Index a, Index b) {
    const Index da = (a < caret) ? (caret - a) : (a - caret);
    const Index db = (b < caret) ? (caret - b) : (b - caret);
    return (da != db) ? (da < db) : (a < b);
  });
}

void LeapLabelPage(Editor& ed) {
  LeapState& leap = ed.leap;
  leap.labels.clear();
  const std::size_t from = leap.page * kLeapKeys.size();
  for (std::size_t i = 0; (i < kLeapKeys.size()) && ((from + i) < leap.matches.size()); ++i) {
    leap.labels.push_back(LeapLabel{leap.matches[from + i], kLeapKeys[i]});
  }
  std::ranges::sort(leap.labels, {}, &LeapLabel::at);
}

std::string LeapLabelHint(const Editor& ed) {
  std::string said;
  if (ed.leap.picked.empty()) {
    said = "leap: a label jumps, a capital selects to it, alt picks, * takes every match";
  } else {
    said = "leap: " + std::to_string(ed.leap.picked.size()) +
           " picked -- a label lands the last one, alt picks more, enter keeps these";
  }
  if (ed.leap.matches.size() > ed.leap.labels.size()) {
    said += ", space labels the other " +
            std::to_string(ed.leap.matches.size() - ed.leap.labels.size());
  }
  return said + ", esc stops";
}

// Arms the next keystroke of a leap and says what it is for, in the leap's own
// slot as well as in the status bar. The slot is what the bar actually draws
// (see LeapHint): a background job's warning arrives with no keystroke to
// catch it and takes ed.status, and a mode sitting on the screen has to go on
// saying what its keys mean. ed.status keeps the same words so that the log,
// and everything that reads the bar's text, are unchanged.
void ArmLeap(Editor& ed, PendingChar stage, std::string hint) {
  ArmPendingChar(ed, stage, false, hint);
  ed.leap.hint = std::move(hint);
}

// Ordinary motion semantics: the jump list first, so ctrl-o comes back here,
// and then one cursor where there may have been many -- the same collapse
// every other caret jump makes.
void LeapJumpTo(Editor& ed, Index at) {
  RecordJump(ed);
  // Belt and braces over the liveness gate: every position here was measured
  // in this document's text a keystroke ago, and a jump is not the place to
  // find out that something has moved underneath it.
  const Index last = std::max<Index>(0, DocLength(ed.doc.table) - 1);
  at = SnapToGraphemeBoundary(ed.doc.table, std::clamp<Index>(at, 0, last));
  ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
}

// The multi-cursor exits (`*`, the capital picks): one caret per position,
// `primary` the one the viewport follows. The same jumplist entry and the
// same clamp-and-snap as the single jump -- these positions are a keystroke
// old too -- and Replace's Normalize is what folds a match picked twice into
// one cursor.
void LeapCursorsTo(Editor& ed, const std::vector<Index>& at, Index primary) {
  RecordJump(ed);
  const Index last = std::max<Index>(0, DocLength(ed.doc.table) - 1);
  const auto place = [&ed, last](Index pos) {
    return SnapToGraphemeBoundary(ed.doc.table, std::clamp<Index>(pos, 0, last));
  };
  std::vector<Selection> cursors;
  cursors.reserve(at.size());
  for (const Index pos : at) {
    const Index p = place(pos);
    cursors.push_back(MinWidth1(ed.doc.table, Selection{p, p, -1}));
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(cursors));
  const Index want = place(primary);
  const auto& ranges = ed.doc.selections.Ranges();
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    if (ranges[i].From() == want) {
      ed.doc.selections.SetPrimary(i);
      break;
    }
  }
}

// The capital-label exit: select from here to the match. The primary keeps
// its anchor and its head lands on the match -- PutCursor's extend, so the
// direction flip and the anchor grapheme's re-holding follow the same rules
// every extend_* motion does. The other cursors collapse, like any jump.
void LeapSelectTo(Editor& ed, Index at) {
  RecordJump(ed);
  const Index last = std::max<Index>(0, DocLength(ed.doc.table) - 1);
  at = SnapToGraphemeBoundary(ed.doc.table, std::clamp<Index>(at, 0, last));
  Selection grown = PutCursor(ed.doc.table, ed.doc.selections.Primary(), at, true);
  grown.goal_column = -1;
  ed.doc.selections.Set(MinWidth1(ed.doc.table, grown));
}

}

void StartLeap(Editor& ed) {
  ed.leap = {};
  ArmLeap(ed, PendingChar::kLeapFirst, std::string{kLeapArmHint});
}

void HandleResize(Editor& ed) {
  // A resize replaces the pane the leap measured, and every position it is
  // offering was picked out of a viewport that is gone: the ones now off the
  // screen are drawn nowhere and their keys would still jump. Ending the mode
  // here also disarms the capture, so the next key is the command it usually
  // is rather than a label for a pane nobody can see -- the same reason the
  // mouse branch drops pending input before it looks at the event.
  if ((ed.leap.stage == LeapState::Stage::kOff) && !IsLeapPending(ed.pending_char)) return;
  ed.leap = {};
  ed.pending_char = PendingChar::kNone;
  ed.pending_char_extend = false;
  ed.pending_char_arg.clear();
  ed.pending_count = 0;
  ed.status.Warn("leap ended -- the window resized");
}

bool LeapIsLive(const Editor& ed) {
  return (ed.leap.stage != LeapState::Stage::kOff) && (ed.leap.doc_id == ed.doc.id) &&
         (ed.leap.revision == ed.doc.table.revision);
}

std::string_view LeapHint(const Editor& ed) {
  if (!IsLeapPending(ed.pending_char)) return {};
  // Before the first character nothing has been measured, so there is nothing
  // to have gone stale; after it the hint describes targets the renderer only
  // paints while the leap is live, and a hint for an overlay nobody can see is
  // worse than the warning it would be holding the line against.
  if ((ed.leap.stage != LeapState::Stage::kOff) && !LeapIsLive(ed)) return {};
  return ed.leap.hint;
}

char LeapLabelAt(const LeapState& leap, Index pos) {
  const auto it = std::ranges::lower_bound(leap.labels, pos, {}, &LeapLabel::at);
  return ((it != leap.labels.end()) && (it->at == pos)) ? it->key : char{0};
}

bool StepLeap(Editor& ed, PendingChar stage, std::string_view grapheme) {
  const auto done = [&ed] {
    ed.leap = {};
    return false;
  };
  // Esc, and every key that is not a character: backspace, the arrows, a ctrl
  // chord. Both end the leap, and this is the mode's own account of it -- the
  // pending-char router's "not a character" answers a question about an
  // alphabet the label stage does not use. Esc's branch overwrites this with
  // the editor-wide "cancelled" it says for every capture.
  if (grapheme.empty()) {
    ed.status = "leap: cancelled";
    return done();
  }

  // A pair never crosses a line break -- the visible ranges stop at the line's
  // content end -- so a newline as either half of one can only be a mistake,
  // and Enter is the key people press to back out of a mode. At the label
  // stage Enter means "take the nearest", which is why this stops short of it.
  if ((grapheme == "\n") && (stage != PendingChar::kLeapLabel)) {
    ed.status = "leap: cancelled";
    return done();
  }

  if (stage == PendingChar::kLeapFirst) {
    ed.leap = {};
    ed.leap.first = std::string{grapheme};
    ed.leap.doc_id = ed.doc.id;
    ed.leap.revision = ed.doc.table.revision;
    ed.leap.stage = LeapState::Stage::kSecond;
    std::vector<Interval> visible;
    LeapVisibleRanges(ed, visible);
    std::vector<Index> hits;
    LeapScan(ed, visible, ed.leap.first, {}, hits, ed.leap.spans);
    ArmLeap(ed, PendingChar::kLeapSecond,
            "leap: " + ed.leap.first + " -- and the character after it");
    return true;
  }

  // The positions below were measured in one document's text, and nothing
  // since has been a keystroke this mode could have caught.
  if (!LeapIsLive(ed)) {
    ed.status.Warn("leap ended -- the text changed");
    return done();
  }

  if (stage == PendingChar::kLeapSecond) {
    std::vector<Interval> visible;
    LeapVisibleRanges(ed, visible);
    LeapScan(ed, visible, ed.leap.first, grapheme, ed.leap.matches, ed.leap.spans);
    if (ed.leap.matches.empty()) {
      ed.status.Warn("leap: no match");
      return done();
    }
    LeapOrderByDistance(ed.leap.matches,
                        CursorOf(ed.doc.table, ed.doc.selections.Primary()));
    if (ed.leap.matches.size() == 1) {
      LeapJumpTo(ed, ed.leap.matches.front());
      return done();
    }
    ed.leap.stage = LeapState::Stage::kLabel;
    LeapLabelPage(ed);
    ArmLeap(ed, PendingChar::kLeapLabel, LeapLabelHint(ed));
    return true;
  }

  // Enter takes the nearest -- or the picks, once a capital has made any:
  // they were chosen one by one, and enter is "that's all of them".
  if (grapheme == "\n") {
    if (!ed.leap.picked.empty()) {
      LeapCursorsTo(ed, ed.leap.picked, ed.leap.picked.back());
    } else {
      LeapJumpTo(ed, ed.leap.matches.front());
    }
    return done();
  }
  if (grapheme == " ") {
    const std::size_t pages =
        (ed.leap.matches.size() + kLeapKeys.size() - 1) / kLeapKeys.size();
    ed.leap.page = (ed.leap.page + 1) % std::max<std::size_t>(1, pages);
    LeapLabelPage(ed);
    ArmLeap(ed, PendingChar::kLeapLabel, LeapLabelHint(ed));
    return true;
  }
  // Every visible match at once: the two characters were the pattern, the
  // cursors are the result -- select_regex without the regex, scoped to what
  // the pane shows. Nearest match is primary, the same one enter would take.
  if (grapheme == "*") {
    LeapCursorsTo(ed, ed.leap.matches, ed.leap.matches.front());
    return done();
  }
  // Alt on a label key picks that match as an extra cursor and keeps the
  // labels up for the next one; the cursors arrive when the mode ends.
  if ((grapheme.size() == 2) && (grapheme.front() == kLeapPickPrefix)) {
    if (const std::size_t key = kLeapKeys.find(grapheme[1]); key != std::string_view::npos) {
      if (const std::size_t which = (ed.leap.page * kLeapKeys.size()) + key;
          which < ed.leap.matches.size()) {
        ed.leap.picked.push_back(ed.leap.matches[which]);
      }
      // A dead alt-label on a short page is ignored the same way a dead
      // label is.
      ArmLeap(ed, PendingChar::kLeapLabel, LeapLabelHint(ed));
      return true;
    }
    ed.status = "leap: cancelled";
    return done();
  }
  if (grapheme.size() == 1) {
    const char pressed = grapheme.front();
    // A capital is "select to it": the primary keeps its anchor and its head
    // lands on that match, the same extension every extend_* motion makes.
    // kLeapKeys is lowercase letters only, so the fold is unambiguous.
    const bool extending = (pressed >= 'A') && (pressed <= 'Z');
    const char key_char = extending ? static_cast<char>(pressed - 'A' + 'a') : pressed;
    if (const std::size_t key = kLeapKeys.find(key_char); key != std::string_view::npos) {
      // Only the labels actually on the screen: the last group of a paged set
      // is short, and the keys past its end name nothing.
      if (const std::size_t which = (ed.leap.page * kLeapKeys.size()) + key;
          which < ed.leap.matches.size()) {
        if (extending) {
          // Ends the mode whole: a selection has one shape, so any picks so
          // far go the way esc would take them.
          LeapSelectTo(ed, ed.leap.matches[which]);
          return done();
        }
        if (!ed.leap.picked.empty()) {
          // A lowercase label with picks pending is the last pick and the
          // landing both: it joins the set and becomes primary.
          ed.leap.picked.push_back(ed.leap.matches[which]);
          LeapCursorsTo(ed, ed.leap.picked, ed.leap.matches[which]);
          return done();
        }
        LeapJumpTo(ed, ed.leap.matches[which]);
        return done();
      }
      // A label key past the end of a short page names nothing on the screen.
      // Being told nothing is not the same as being told to stop, and throwing
      // the leap away costs the two characters that set it up -- so the page
      // stays up, still saying what its keys mean.
      ArmLeap(ed, PendingChar::kLeapLabel, LeapLabelHint(ed));
      return true;
    }
  }
  ed.status = "leap: cancelled";
  return done();
}

void ApplyPendingChar(Editor& ed, std::string_view grapheme) {
  const PendingChar kind = ed.pending_char;
  const bool extend = ed.pending_char_extend;
  const std::string first = ed.pending_char_arg;
  ed.pending_char = PendingChar::kNone;
  ed.pending_char_extend = false;
  ed.pending_char_arg.clear();
  // Only a leap step gets to keep its overlay up. Any other capture resolving
  // here means the mode is over, however it got left behind.
  if (!IsLeapPending(kind)) ed.leap = {};
  if (kind == PendingChar::kNone) return;
  ed.status.clear();

  // The batch that armed the capture (`r`, `ms`, `md`) edited nothing and left
  // no note behind; the edit is here, a keystroke later, and the cursors it
  // wants are the ones in front of it now. The kinds that only move -- find,
  // split, text objects, leap -- drop it again below.
  NoteCursorsForNextEdit(ed);

  switch (kind) {
    case PendingChar::kFindNext:
    case PendingChar::kTillNext:
    case PendingChar::kFindPrev:
    case PendingChar::kTillPrev:
      FindCharOnLine(ed, grapheme, kind, extend, CountOr(ed, 1));
      break;
    case PendingChar::kSplitOn:
      SplitOnChar(ed, grapheme);
      break;
    case PendingChar::kReplaceChar:
      ReplaceEachChar(ed, grapheme);
      break;
    case PendingChar::kSurroundAdd:
      SurroundAdd(ed, grapheme);
      break;
    case PendingChar::kSurroundDelete:
      SurroundDelete(ed, grapheme, {});
      break;
    case PendingChar::kSurroundReplaceFrom:
      if (!grapheme.empty()) {
        ed.pending_char = PendingChar::kSurroundReplaceTo;
        ed.pending_char_arg = std::string{grapheme};
        ed.status = "replace " + std::string{grapheme} + " with...";
        DropCursorNote(ed.doc.table);
        return;
      }
      break;
    case PendingChar::kSurroundReplaceTo:
      SurroundDelete(ed, first, grapheme);
      break;
    case PendingChar::kTextObjectInner:
      SelectTextObject(ed, grapheme, false);
      break;
    case PendingChar::kTextObjectAround:
      SelectTextObject(ed, grapheme, true);
      break;
    case PendingChar::kLeapFirst:
    case PendingChar::kLeapSecond:
    case PendingChar::kLeapLabel:
      // Re-arms itself for the character or the label key after this one;
      // falls through to the tidy-up below only once the leap is over.
      if (StepLeap(ed, kind, grapheme)) {
        DropCursorNote(ed.doc.table);
        return;
      }
      break;
    case PendingChar::kNone:
      break;
  }
  DropCursorNote(ed.doc.table);
  ed.pending_count = 0;
  ApplyModeInvariants(ed);
}

bool OpenFile(Editor& ed, const std::filesystem::path& path, bool open_generated=false);

void RecordJump(Editor& ed) {
  if (!ed.jumps) return;
  const std::filesystem::path where =
      HasDiskFile(ed.doc) ? ed.doc.file : std::filesystem::path{ed.doc.view_name};
  if (where.empty()) return;
  const Index cursor = CursorOf(ed.doc.table, ed.doc.selections.Primary());
  const Index line = LineAt(ed.doc.table, cursor);
  ed.jumps->Record(where, line + 1, cursor - LineStart(ed.doc.table, line) + 1);
}

namespace {

bool JumpLandsSomewhere(const Editor& ed, const std::filesystem::path& path) {
  if (path.empty()) return false;
  if (FindFileBuffer(ed, path) < BufferCount(ed)) return true;
  if (FindViewBuffer(ed, path.native()) < BufferCount(ed)) return true;
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

}

void StepJump(Editor& ed, bool forward) {
  if (!ed.jumps) {
    ed.status.Warn("no jump store");
    return;
  }
  if (!forward && ed.jumps->AtNewest()) RecordJump(ed);

  Jump jump;
  bool landed = false;
  while (ed.jumps->Step(forward, jump)) {
    if (JumpLandsSomewhere(ed, std::filesystem::path{jump.path})) {
      landed = true;
      break;
    }
  }
  if (!landed) {
    ed.status = forward ? "no newer position" : "no older position";
    return;
  }

  Target target;
  target.path = jump.path;
  target.line = jump.line;
  target.has_line = true;
  target.column = jump.col;
  target.has_column = true;

  std::error_code ec;
  const std::filesystem::path here = std::filesystem::weakly_canonical(ed.doc.file, ec);
  const bool already_here = IsExcerptView(ed.doc)
                                ? (ed.doc.view_name == jump.path)
                                : (HasDiskFile(ed.doc) && ((ec ? ed.doc.file : here) == target.path));
  if (!already_here) {
    if (!OpenFile(ed, target.path, true )) return;
  }
  GoToTarget(ed.doc, target);
  ed.status.clear();
}

namespace {

const std::vector<Interval>* AllMatches(Editor& ed, std::string_view pattern) {
  if ((ed.doc.search_cache_revision != ed.doc.table.revision) ||
      (ed.doc.search_cache_pattern != pattern)) {
    ed.doc.search_cache.clear();
    std::string error;
    if (!FindInDocument(ed.doc.table, pattern, Interval(0, DocLength(ed.doc.table)),
                        ed.doc.search_cache, error)) {
      ed.status.Warn("bad pattern: " + error);
      ed.doc.search_cache_revision = -1;
      return nullptr;
    }
    ed.doc.search_cache_pattern = pattern;
    ed.doc.search_cache_revision = ed.doc.table.revision;
  }
  if (ed.doc.search_cache.empty()) {
    ed.status.Warn("no match: " + std::string{pattern});
    return nullptr;
  }
  return &ed.doc.search_cache;
}

void SelectMatch(Editor& ed, const Interval& match) {
  // Not the raw offsets: Set is the one mutator that neither clamps nor snaps,
  // and a match starting inside a grapheme cluster would install a selection
  // that Apply then refuses -- a highlighted match that `d` silently ignores.
  ed.doc.selections.Set(CoveringSelection(ed.doc.table, match.front(), match.back() + 1));
}

}

std::string_view ActiveSearchPattern(const Editor& ed) {
  const bool previews = (ed.prompt_kind == PromptKind::kSearch) ||
                        (ed.prompt_kind == PromptKind::kSelectRegex);
  if (ed.prompt_active && previews) return ed.prompt_input;
  return ed.search_highlight ? std::string_view{ed.search_pattern} : std::string_view{};
}

bool SearchIsConfinedToSelections(const Editor& ed) {
  return ed.prompt_active && (ed.prompt_kind == PromptKind::kSelectRegex);
}

namespace {

void ResolveCaptureStyles(const Theme& theme, Document& doc) {
  doc.capture_styles.clear();
  if (IsExcerptView(doc)) {
    doc.capture_styles.reserve(doc.excerpts.capture_names.size());
    for (const std::string& name : doc.excerpts.capture_names) {
      doc.capture_styles.push_back(theme.Get(name));
    }
    return;
  }
  if (doc.syntax == nullptr) return;
  const auto names = doc.syntax->CaptureNames();
  doc.capture_styles.reserve(names.size());
  for (const std::string& name : names) doc.capture_styles.push_back(theme.Get(name));
}

}

void RefreshCaptureStyles(Editor& ed) {
  ResolveCaptureStyles(ed.theme, ed.doc);
  for (std::size_t i = 0; i < ed.buffers.size(); ++i) {

    if (i == ed.active) continue;
    ResolveCaptureStyles(ed.theme, ed.buffers[i]);
  }
}

void AttachSyntax(Editor& ed) {
  if (!OwnsGrammar(ed.doc)) {
    ed.doc.syntax = nullptr;
    ResolveCaptureStyles(ed.theme, ed.doc);
    return;
  }
  std::string error;
  ed.doc.syntax = OpenSyntax(ed.doc.file, error);
  if ((ed.doc.syntax == nullptr) && !error.empty()) ed.status.Warn("no highlighting: " + error);
  ResolveCaptureStyles(ed.theme, ed.doc);
}

bool ApplyTheme(Editor& ed, std::string_view name) {
  Theme loaded;
  std::string error;
  if (!LoadTheme(name, loaded, error)) {
    ed.status.Warn(error);
    return false;
  }
  ed.theme = std::move(loaded);
  RefreshCaptureStyles(ed);
  ed.settings.theme = name;
  return true;
}

void RunSearch(Editor& ed, std::string_view pattern) {
  const std::vector<Interval>* found = AllMatches(ed, pattern);
  if (found == nullptr) return;
  const std::vector<Interval>& matches = *found;
  ed.search_pattern = pattern;
  ed.search_highlight = true;
  RecordJump(ed);

  const Index cursor = CursorOf(ed.doc.table, ed.doc.selections.Primary());
  for (std::size_t i = 0; i < matches.size(); ++i) {
    if (matches[i].front() >= cursor) {
      SelectMatch(ed, matches[i]);
      ed.status = "[" + std::to_string(i + 1) + "/" + std::to_string(matches.size()) + "]";
      return;
    }
  }
  SelectMatch(ed, matches.front());
  ed.status = "[1/" + std::to_string(matches.size()) + "] -- wrapped to the top";
}

void SearchStep(Editor& ed, bool forward) {
  if (ed.search_pattern.empty()) {
    ed.status.Warn("no search pattern -- / to search");
    return;
  }
  const std::vector<Interval>* found = AllMatches(ed, ed.search_pattern);
  if (found == nullptr) return;
  const std::vector<Interval>& matches = *found;
  ed.search_highlight = true;
  RecordJump(ed);

  const Index cursor = CursorOf(ed.doc.table, ed.doc.selections.Primary());
  const auto count = static_cast<std::ptrdiff_t>(matches.size());

  const auto it = std::upper_bound(matches.begin(), matches.end(), cursor,
                                   [](Index c, const Interval& m) { return c < m.front(); });
  const auto next = static_cast<std::ptrdiff_t>(it - matches.begin());
  std::ptrdiff_t on = -1;
  if (next > 0) {
    const Interval& prev = matches[static_cast<std::size_t>(next - 1)];
    if ((cursor >= prev.front()) && (cursor <= prev.back())) on = next - 1;
  }

  const std::ptrdiff_t base = (on >= 0) ? on : (forward ? (next - 1) : next);
  const std::ptrdiff_t steps = static_cast<std::ptrdiff_t>(CountOr(ed, 1));
  const std::ptrdiff_t target = base + (forward ? steps : -steps);

  const bool wrapped = (target < 0) || (target >= count);
  const std::ptrdiff_t landed = ((target % count) + count) % count;

  SelectMatch(ed, matches[static_cast<std::size_t>(landed)]);
  ed.status = "[" + std::to_string(landed + 1) + "/" + std::to_string(count) + "]" +
              (!wrapped   ? ""
               : forward  ? " -- wrapped to the top"
                          : " -- wrapped to the bottom");
}

void SelectRegex(Editor& ed, std::string_view pattern) {
  RecordJump(ed);
  std::vector<Selection> found;
  std::string error;
  for (const Selection& s : ed.doc.selections.Ranges()) {
    std::vector<Interval> hits;
    if (!FindInDocument(ed.doc.table, pattern, Interval(s.From(), s.To()), hits, error)) {
      ed.status.Warn("bad pattern: " + error);
      return;
    }
    for (const Interval& hit : hits) {
      found.push_back(CoveringSelection(ed.doc.table, hit.front(), hit.back() + 1));
    }
  }
  if (found.empty()) {
    ed.status.Warn("no match in selection: " + std::string{pattern});
    return;
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(found));
  ed.search_pattern = pattern;
  ed.search_highlight = true;
}

void PromptSubmit(Editor& ed) {
  if (!ed.prompt_active) return;
  const PromptKind kind = ed.prompt_kind;
  std::string line = ed.prompt_input;
  PromptCancel(ed);

  if (kind == PromptKind::kCommand) {
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) return;
    const size_t last = line.find_last_not_of(" \t");
    line = line.substr(first, last - first + 1);
  } else if (line.empty()) {
    return;
  }

  std::vector<std::string>& history = PromptHistoryOf(ed);
  if (history.empty() || (history.back() != line)) history.push_back(line);
  ed.prompt_history_index = history.size();

  switch (kind) {
    case PromptKind::kCommand: RunTypableCommand(ed, line); return;
    case PromptKind::kSearch: RunSearch(ed, line); return;
    case PromptKind::kSelectRegex: SelectRegex(ed, line); return;
    case PromptKind::kSearchExcerpts: SearchExcerpts(ed, line); return;
  }
}

bool OpenFile(Editor& ed, const std::filesystem::path& path, bool open_generated) {

  std::size_t existing = FindFileBuffer(ed, path);
  if ((existing >= BufferCount(ed)) && open_generated) {
    existing = FindViewBuffer(ed, path.native());
  }
  if (existing < BufferCount(ed)) {
    SwitchToBuffer(ed, existing);
    ed.mode = Mode::kNormal;
    ApplyModeInvariants(ed);
    ed.status.clear();
    return true;
  }

  Document fresh;
  fresh.tab_width = ed.doc.tab_width;
  fresh.insert_spaces = ed.doc.insert_spaces;
  fresh.view.rows = ed.doc.view.rows;
  fresh.view.columns = ed.doc.view.columns;
  if (const ErrorCtx err = LoadDocument(path, fresh); err) {
    ed.status.Fail("cannot open " + DisplayPath(path) + ": " + FormatErrorCtx(err));
    return false;
  }

  AddBuffer(ed, std::move(fresh));
  ed.mode = Mode::kNormal;
  ApplyModeInvariants(ed);
  AttachSyntax(ed);
  ed.status.clear();
  return true;
}

bool OpenTarget(Editor& ed, std::string_view spec) {
  const Target target = ParseTarget(spec);
  RecordJump(ed);
  RecordVisitHere(ed);
  if (!OpenFile(ed, target.path)) return false;
  GoToTarget(ed.doc, target);
  if (!target.has_line) RestoreLastPosition(ed);
  RecordJump(ed);
  return true;
}

namespace {

bool TrimTrailingWhitespace(Editor& ed) {
  PieceTable& table = ed.doc.table;

  std::vector<Interval> cuts;
  const Index lines = LineCount(table);
  for (Index line = 0; line < lines; ++line) {
    const Interval content = LineContentRange(table, line);
    if (content.empty()) continue;
    const Index begin = *content.begin();
    const Index end = begin + static_cast<Index>(content.size());
    const std::string text = ReadDocRange(table, content);
    Index keep = static_cast<Index>(text.size());
    while ((keep > 0) && ((text[keep - 1] == ' ') || (text[keep - 1] == '\t'))) --keep;
    if (keep == static_cast<Index>(text.size())) continue;
    cuts.emplace_back(begin + keep, end);
  }
  if (cuts.empty()) return false;

  UndoGroup group(table);
  std::vector<Edit> edits;
  edits.reserve(cuts.size());
  for (Index i = std::ssize(cuts); i-- > 0;) {
    Edit edit;
    if (const ErrorCtx err = Delete(cuts[static_cast<std::size_t>(i)], table, &edit); err) {
      ed.status.Fail(FormatErrorCtx(err));
      return false;
    }
    edits.push_back(edit);
  }

  ed.doc.selections.MapThroughEdits(table, edits);
  ed.doc.modified = true;
  return true;
}

bool SaveBuffer(Editor& ed, std::string_view rest, bool force, bool rerun_watched = true) {
  namespace fs = std::filesystem;
  if (IsExcerptView(ed.doc) && rest.empty()) return SaveExcerptView(ed);
  if (IsExcerptView(ed.doc) && !force) {
    ed.status.Warn("writing a view to a file converts it into that file -- :w! " +
                   std::string{rest} + " to do that");
    return false;
  }
  const fs::path target = rest.empty() ? ed.doc.file : fs::path{rest};
  if (target.empty()) {
    ed.status.Warn("no file name -- :w <path>");
    return false;
  }

  if (!force) {
    const auto canon = [](const fs::path& p) {
      std::error_code ec;
      const fs::path abs = fs::weakly_canonical(fs::absolute(p, ec), ec);
      return ec ? p : abs;
    };
    const bool same = HasDiskFile(ed.doc) && (canon(target) == canon(ed.doc.file));
    std::error_code ec;
    if (same) {
      if (ExternallyModified(ed.doc)) {
        ed.status.Warn(DisplayPath(ed.doc.file) +
                    " changed on disk -- :reload to take that, :w! to overwrite");
        return false;
      }

    } else if (FindFileBuffer(ed, target) < BufferCount(ed)) {
      ed.status.Warn(DisplayPath(target) +
                     " is open in another buffer -- :w! to write over it anyway");
      return false;
    } else if (fs::exists(target, ec)) {
      ed.status.Warn(DisplayPath(target) + " exists -- :w! to overwrite");
      return false;
    }
  }

  const std::string_view was = LanguageForPath(ed.doc.file);

  if (ed.settings.trim_trailing_whitespace_on_save) std::ignore = TrimTrailingWhitespace(ed);

  const bool was_view = IsExcerptView(ed.doc);
  const ErrorCtx err = SaveDocumentAs(ed.doc, target);
  if (err) {
    ed.status.Fail("cannot write " + DisplayPath(target) + ": " + FormatErrorCtx(err) +
                   " -- :w <path> to save elsewhere");
    return false;
  }
  if (was_view) ed.doc.excerpts = ExcerptView{};
  if (LanguageForPath(ed.doc.file) != was) AttachSyntax(ed);
  RecordEditHere(ed);
  if (rerun_watched) std::ignore = RerunWatchedViews(ed);
  return true;
}

bool SaveAllBuffers(Editor& ed, bool force, bool rerun_watched = true) {
  const std::size_t started_at = ed.active;
  bool all_written = true;
  int wrote = 0;
  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    SwitchToBuffer(ed, i);
    if (!ed.doc.modified) continue;
    if (SaveBuffer(ed, std::string_view{}, force, false)) {
      ++wrote;
    } else {
      all_written = false;
    }
  }
  SwitchToBuffer(ed, started_at);
  if (all_written) {
    ed.status = (wrote == 0) ? std::string{"nothing to write"}
                             : ("wrote " + std::to_string(wrote) + " buffer" +
                                ((wrote == 1) ? "" : "s"));
  }
  if ((wrote > 0) && rerun_watched) std::ignore = RerunWatchedViews(ed);
  return all_written;
}

std::string UnsavedSummary(const std::vector<std::string>& names) {
  if (names.size() == 1) return names.front() + " has unsaved changes";
  std::string out = std::to_string(names.size()) + " buffers have unsaved changes: ";
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i > 0) out += ", ";
    if (i == 3) {
      out += "and " + std::to_string(names.size() - 3) + " more";
      break;
    }
    out += names[i];
  }
  return out;
}

void StepBuffer(Editor& ed, bool forward) {
  const std::size_t count = BufferCount(ed);
  if (count <= 1) {
    ed.status.Warn("only one buffer open");
    return;
  }
  const std::size_t next = forward ? ((ed.active + 1) % count) : ((ed.active + count - 1) % count);
  SwitchToBuffer(ed, next);
  ed.mode = Mode::kNormal;
  ApplyModeInvariants(ed);
  ed.status = "[" + std::to_string(next + 1) + "/" + std::to_string(count) + "] " +
              (IsExcerptView(ed.doc) ? ed.doc.view_name
               : !HasDiskFile(ed.doc) ? std::string{"[no name]"}
                                      : DisplayPath(ed.doc.file));
}

void CloseBuffer(Editor& ed, bool force) {
  if (!force && ed.doc.modified) {
    ed.status.Warn((IsExcerptView(ed.doc) ? ed.doc.view_name
               : !HasDiskFile(ed.doc) ? std::string{"[no name]"}
                                      : DisplayPath(ed.doc.file)) +
                   " has unsaved changes -- :w to write, :bc! to discard");
    return;
  }
  CloseActiveBuffer(ed);
  ed.mode = Mode::kNormal;
  ApplyModeInvariants(ed);
  AttachSyntax(ed);
  ed.status = "[" + std::to_string(ed.active + 1) + "/" +
                          std::to_string(BufferCount(ed)) + "] " +
                          (IsExcerptView(ed.doc)  ? ed.doc.view_name
                           : !HasDiskFile(ed.doc) ? std::string{"[no name]"}
                                                  : DisplayPath(ed.doc.file));
}

void CloseOtherWindows(Editor& ed) {
  if (ed.windows.empty()) return;
  const std::size_t before = WindowCount(ed);
  if (before <= 1) return;
  KeepOnlyFocusedWindow(ed);
  const int closed = static_cast<int>(before - WindowCount(ed));
  if (closed > 0) ed.status = "closed " + std::to_string(closed) + " window(s)";
}

void CloseOtherBuffers(Editor& ed, bool force) {
  if (BufferCount(ed) <= 1) {
    ed.status.clear();
    return;
  }
  if (!force) {
    std::vector<std::string> unsaved;
    for (std::size_t i = 0; i < BufferCount(ed); ++i) {
      if (i == ed.active) continue;
      const Document& doc = BufferAt(ed, i);
      if (!doc.modified) continue;
      unsaved.push_back(IsExcerptView(doc)  ? doc.view_name
                        : !HasDiskFile(doc) ? std::string{"[no name]"}
                                            : DisplayPath(doc.file));
    }
    if (!unsaved.empty()) {
      std::string names = unsaved.front();
      if (unsaved.size() > 1) names += " and " + std::to_string(unsaved.size() - 1) + " more";
      ed.status.Warn(names + " has unsaved changes -- :wa to write, :bco! to discard");
      return;
    }
  }
  std::size_t keep = ed.active;
  int guard = 0;
  std::size_t closed = 0;
  while ((BufferCount(ed) > 1) && (++guard < 4096)) {
    const std::size_t victim = (keep == 0) ? 1 : 0;
    SwitchToBuffer(ed, victim);
    CloseActiveBuffer(ed);
    ++closed;
    if (victim < keep) --keep;
  }
  SwitchToBuffer(ed, std::min(keep, BufferCount(ed) - 1));
  ed.mode = Mode::kNormal;
  ApplyModeInvariants(ed);
  AttachSyntax(ed);
  ed.status = "closed " + std::to_string(closed) + " buffer(s)";
}

void ListBuffers(Editor& ed) {
  std::string out;
  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    const Document& doc = BufferAt(ed, i);
    if (i > 0) out += "  ";
    if (i == ed.active) out += "*";
    out += std::to_string(i + 1) + ":";
    out += IsExcerptView(doc)  ? doc.view_name
           : !HasDiskFile(doc) ? std::string{"[no name]"}
                               : DisplayPath(doc.file);
    if (doc.modified) out += "[+]";
  }
  ed.status = out;
}

void SetExcerptContext(Editor& ed, std::string_view rest) {
  const auto describe = [&ed] {
    return "excerpt-context: " + std::to_string(ed.settings.excerpt_context) + " lines";
  };
  if (rest.empty()) {
    ed.status = describe();
    return;
  }
  int lines = 0;
  const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), lines);
  if ((ec != std::errc{}) || (ptr != (rest.data() + rest.size())) || (lines < 0) ||
      (lines > 100)) {
    ed.status.Warn(":set-excerpt-context wants 0-100");
    return;
  }
  ed.settings.excerpt_context = lines;
  if (!RebuildExcerptView(ed)) ed.status = describe();
}

void SetIndent(Editor& ed, std::string_view rest) {
  const auto describe = [&ed] {
    return ed.doc.insert_spaces ? (std::to_string(ed.doc.tab_width) + " spaces") : std::string{"tab"};
  };
  if (rest.empty()) {
    ed.status = "indent: " + describe();
    return;
  }
  if (rest == "tab") {
    ed.doc.insert_spaces = false;
    ed.status = "indent: tab";
    return;
  }
  int width = 0;
  const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), width);
  if ((ec != std::errc{}) || (ptr != (rest.data() + rest.size())) || (width < 1) || (width > 16)) {
    ed.status.Warn(":set-indent wants a width of 1-16, or \"tab\"");
    return;
  }
  ed.doc.insert_spaces = true;
  ed.doc.tab_width = width;
  ed.status = "indent: " + describe();
}

void SetMulticursorPaste(Editor& ed, std::string_view rest) {
  const auto describe = [&ed] {
    return std::string{"multicursor-paste: "} +
           (ed.settings.multi_cursor_paste_spread ? "spread" : "full");
  };
  if (rest.empty()) {
    ed.status = describe();
    return;
  }
  if ((rest != "spread") && (rest != "full")) {
    ed.status.Warn(":set-multicursor-paste wants \"spread\" or \"full\"");
    return;
  }
  ed.settings.multi_cursor_paste_spread = (rest == "spread");
  ed.status = describe();
}

void SetLanguage(Editor& ed, std::string_view rest) {
  if (rest.empty()) {
    ed.status = "language: " + (ed.doc.syntax ? std::string{ed.doc.syntax->Language()} : std::string{"none"});
    return;
  }

  if (!OwnsGrammar(ed.doc)) {
    ed.status.Warn("an excerpt view keeps its own highlighting");
    return;
  }

  const std::span<const std::string_view> known = KnownLanguages();
  if (std::ranges::find(known, rest) == known.end()) {
    std::string list;
    for (const std::string_view one : known) {
      if (!list.empty()) list += ' ';
      list += one;
    }
    ed.status.Warn("no grammar for \"" + std::string{rest} + "\" -- have: " + list);
    return;
  }

  std::string error;
  std::shared_ptr<Syntax> syntax = OpenSyntaxForLanguage(rest, error);
  if (syntax == nullptr) {
    ed.status.Fail("cannot highlight as " + std::string{rest} +
                   (error.empty() ? std::string{} : (": " + error)));
    return;
  }

  ed.doc.syntax = std::move(syntax);
  ResolveCaptureStyles(ed.theme, ed.doc);
  ed.status = "language: " + std::string{ed.doc.syntax->Language()};
}

}

void ApplyPaste(Editor& ed, std::string_view raw) {
  if (raw.empty()) return;
  std::string text;
  text.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] != '\r') {
      text += raw[i];
    } else if ((i + 1 < raw.size()) && (raw[i + 1] == '\n')) {
      continue;
    } else {
      text += '\n';
    }
  }
  if (ed.prompt_active) {
    std::string flat{text};
    std::ranges::replace(flat, '\n', ' ');
    std::ranges::replace(flat, '\r', ' ');
    PromptInsert(ed, flat);
    return;
  }
  if (ed.mode == Mode::kInsert) {
    Edited(ed, [&] { return InsertAtCursorsKeeping(text, ed.doc.table, ed.doc.selections); });
    return;
  }
  PasteText(ed, text, false, false);
}

namespace {

ErrorCtx ReloadInto(Document& doc) {

  std::error_code exists_ec;
  if (!HasDiskFile(doc) || !std::filesystem::exists(doc.file, exists_ec)) {
    return MakeErrorCtx(std::errc::no_such_file_or_directory);
  }

  const Index cursor_line = LineAt(doc.table, doc.selections.Primary().head) + 1;
  Document fresh;
  fresh.tab_width = doc.tab_width;
  fresh.insert_spaces = doc.insert_spaces;
  if (const ErrorCtx err = LoadDocument(doc.file, fresh); err) return err;

  const std::string text = ReadDocRange(fresh.table, Interval(0, DocLength(fresh.table)));
  BreakUndoCoalescing(doc.table);
  CursorState before;
  for (const Selection& s : doc.selections.Ranges()) {
    before.spans.push_back(CursorSpan{s.anchor, s.head});
  }
  const Change whole{0, DocLength(doc.table), text};
  if (const ErrorCtx err = Apply(doc.table, std::span{&whole, 1}, before, CursorState{}); err) {
    return err;
  }

  doc.tab_width = fresh.tab_width;
  doc.insert_spaces = fresh.insert_spaces;
  doc.read_only = fresh.read_only;
  doc.disk_stamp = fresh.disk_stamp;
  MarkUndoSavePoint(doc.table);
  doc.saved_undo_serial = CurrentUndoSerial(doc.table);
  doc.modified = false;
  doc.selections.Normalize(doc.table);

  Target back;
  back.line = cursor_line;
  back.has_line = true;
  GoToTarget(doc, back);
  return {};
}

}

bool ReloadDocument(Editor& ed) {
  if (!HasDiskFile(ed.doc)) {
    ed.status.Warn("no file to reload");
    return false;
  }
  if (const ErrorCtx err = ReloadInto(ed.doc); err) {
    ed.status.Fail("cannot reload " + DisplayPath(ed.doc.file) + ": " + FormatErrorCtx(err));
    return false;
  }
  return true;
}

void ReloadEveryBuffer(Editor& ed, bool force) {
  int reloaded = 0;
  int kept = 0;
  int failed = 0;
  std::string first_error;

  const auto reload = [&](Document& doc) {
    if (const ErrorCtx err = ReloadInto(doc); err) {
      ++failed;
      if (first_error.empty()) first_error = FormatErrorCtx(err);
    } else {
      ++reloaded;
    }
  };

  int attempted = 0;
  if (HasDiskFile(ed.doc)) {
    ++attempted;
    if (!force && ed.doc.modified) {
      ++kept;
    } else {
      reload(ed.doc);
    }
  }
  for (std::size_t i = 0; i < ed.buffers.size(); ++i) {

    if (i == ed.active) continue;
    Document& doc = ed.buffers[i];
    if (!HasDiskFile(doc)) continue;
    ++attempted;
    if (!force && doc.modified) {
      ++kept;
      continue;
    }
    reload(doc);
  }

  if (attempted == 0) {
    ed.status.Warn("no file to reload");
    return;
  }

  std::string report = "reloaded " + std::to_string(reloaded);
  if (kept > 0) {
    report += ", kept " + std::to_string(kept) + " with unsaved changes -- :reload! to discard";
  }
  if (failed > 0) {
    report += ", " + std::to_string(failed) + " failed: " + first_error;
    ed.status.Fail(report);
  } else if (kept > 0) {
    ed.status.Warn(report);
  } else {
    ed.status = report;
  }
}

// Every buffer a pane is drawing, not only the focused one. A split leaves the
// other panes showing text their files no longer hold, and nothing else looks
// at them: the disk check runs on focus-in alone, so a stale pane stayed stale
// until the user happened to run :reload. Buffers with no pane on them are left
// out on purpose -- nobody is reading those, and swapping the ground under a
// buffer the user has not looked at in an hour is the surprise this check is
// careful to avoid for a modified one.
void CheckDiskChange(Editor& ed) {
  std::vector<std::string> took;
  std::vector<std::string> held;
  std::vector<std::string> failed;
  std::string first_error;

  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    Document& doc = (ed.buffers.empty() || (i == ed.active)) ? ed.doc : ed.buffers[i];
    if (!HasDiskFile(doc)) continue;
    if (!BufferOnScreen(ed, i)) continue;
    if (!ExternallyModified(doc)) continue;
    if (doc.modified) {
      held.push_back(DisplayPath(doc.file));
      continue;
    }
    if (const ErrorCtx err = ReloadInto(doc); err) {
      failed.push_back(DisplayPath(doc.file));
      if (first_error.empty()) first_error = FormatErrorCtx(err);
      continue;
    }
    took.push_back(DisplayPath(doc.file));
  }

  if (took.empty() && held.empty() && failed.empty()) return;

  // Named rather than counted. "3 files changed on disk" is a fact the reader
  // then has to go and turn into names one buffer at a time, which is the work
  // this message exists to save them.
  const auto list = [](const std::vector<std::string>& names) {
    constexpr std::size_t kMaxNamed = 3;
    std::string out;
    for (std::size_t i = 0; (i < names.size()) && (i < kMaxNamed); ++i) {
      if (i > 0) out += ", ";
      out += names[i];
    }
    if (names.size() > kMaxNamed) {
      out += " and " + std::to_string(names.size() - kMaxNamed) + " more";
    }
    return out;
  };

  std::string report;
  if (!took.empty()) report = list(took) + " changed on disk -- reloaded";
  if (!held.empty()) {
    if (!report.empty()) report += "; ";
    report += list(held) + " changed on disk -- :reload to take that, :w! to overwrite";
  }
  if (!failed.empty()) {
    if (!report.empty()) report += "; ";
    report += "cannot reload " + list(failed) + ": " + first_error;
    ed.status.Fail(report);
    return;
  }
  ed.status.Warn(report);
}

namespace {

void OpenVerb(Editor& ed, std::string_view rest) {
  if (rest.empty()) {
    ed.status = "open what? -- :o <path>";
    return;
  }
  OpenTarget(ed, rest);
}

void NewVerb(Editor& ed, std::string_view rest) {
  Document fresh;
  fresh.tab_width = ed.doc.tab_width;
  fresh.insert_spaces = ed.doc.insert_spaces;
  ResetToOriginal(fresh.table, "");
  if (!rest.empty()) fresh.file = std::filesystem::path{rest};

  AddBuffer(ed, std::move(fresh));
  ed.doc.selections.Set(Selection{0, 0, -1});
  ApplyModeInvariants(ed);
  AttachSyntax(ed);
  ed.status = rest.empty() ? "new buffer -- :w <path> to name it"
                           : ("new buffer: " + DisplayPath(ed.doc.file));
}

void ConfigOpenVerb(Editor& ed, std::string_view) {
  const std::filesystem::path config = ConfigPath();
  if (config.empty()) {
    ed.status.Warn("no config path -- there is no $HOME to put one under");
    return;
  }
  OpenFile(ed, config);
}

void ConfigReferenceVerb(Editor& ed, std::string_view) {
  const std::filesystem::path found = FindRuntimeFile("config.reference.toml");
  if (found.empty()) {
    ed.status.Fail("config.reference.toml is not installed beside this build");
    return;
  }
  OpenFile(ed, found);
}

void ThemeVerb(Editor& ed, std::string_view rest) {
  if (rest.empty()) {
    ed.status = "theme: " + ed.settings.theme;
  } else if (ApplyTheme(ed, rest)) {
    ed.status = "theme: " + std::string{rest};
  }
}

bool SlotOf(Editor& ed, std::string_view verb, std::string_view rest, int& value) {
  const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), value);
  if (rest.empty() || (ec != std::errc{}) || (ptr != rest.data() + rest.size())) {
    ed.status = ":" + std::string{verb} + " needs a small whole number";
    return false;
  }
  return true;
}

constexpr std::array kTypable = std::to_array<TypableDef>({
    {"w", "", "write the file"},
    {"write", "", "write the file"},
    {"w!", "", "write, overwriting an external change"},
    {"write!", "", "write, overwriting an external change"},
    {"q", "", "quit"},
    {"quit", "", "quit"},
    {"q!", "", "quit, discarding changes"},
    {"quit!", "", "quit, discarding changes"},
    {"wq", "", "write and quit"},
    {"x", "", "write and quit"},
    {"wq!", "", "write and quit, overwriting an external change"},
    {"x!", "", "write and quit, overwriting an external change"},
    {"wa", "", "write all"},
    {"write-all", "", "write all"},
    {"wa!", "", "write all, overwriting external changes"},
    {"qa", "", "quit all"},
    {"quit-all", "", "quit all"},
    {"qa!", "", "quit all, discarding changes"},
    {"quit-all!", "", "quit all, discarding changes"},
    {"wqa", "", "write all and quit"},
    {"write-all-quit", "", "write all and quit"},
    {"wqa!", "", "write all and quit, overwriting external changes"},
    {"o", "<path>", "open a file", OpenVerb},
    {"open", "<path>", "open a file", OpenVerb},
    {"new", "[path]", "start an empty buffer", NewVerb},
    {"bn", "", "next buffer", [](Editor& ed, std::string_view) { StepBuffer(ed, true); }},
    {"buffer-next", "", "next buffer", [](Editor& ed, std::string_view) { StepBuffer(ed, true); }},
    {"bp", "", "previous buffer", [](Editor& ed, std::string_view) { StepBuffer(ed, false); }},
    {"buffer-previous", "", "previous buffer",
     [](Editor& ed, std::string_view) { StepBuffer(ed, false); }},
    {"bc", "", "close this buffer", [](Editor& ed, std::string_view) { CloseBuffer(ed, false); }},
    {"buffer-close", "", "close this buffer",
     [](Editor& ed, std::string_view) { CloseBuffer(ed, false); }},
    {"bc!", "", "close this buffer, discarding changes",
     [](Editor& ed, std::string_view) { CloseBuffer(ed, true); }},
    {"buffer-close!", "", "close this buffer, discarding changes",
     [](Editor& ed, std::string_view) { CloseBuffer(ed, true); }},
    {"buffers", "", "list open buffers", [](Editor& ed, std::string_view) { ListBuffers(ed); }},
    {"ls", "", "list open buffers", [](Editor& ed, std::string_view) { ListBuffers(ed); }},
    {"hsplit", "", "split this window top and bottom",
     [](Editor& ed, std::string_view) { SplitWindow(ed, false); }},
    {"sp", "", "split this window top and bottom",
     [](Editor& ed, std::string_view) { SplitWindow(ed, false); }},
    {"vsplit", "", "split this window left and right",
     [](Editor& ed, std::string_view) { SplitWindow(ed, true); }},
    {"vs", "", "split this window left and right",
     [](Editor& ed, std::string_view) { SplitWindow(ed, true); }},
    {"wclose", "", "close this window", [](Editor& ed, std::string_view) { CloseWindow(ed); }},
    {"wonly", "", "close every window but this one",
     [](Editor& ed, std::string_view) { CloseOtherWindows(ed); }},
    {"bco", "", "close every buffer but this one",
     [](Editor& ed, std::string_view) { CloseOtherBuffers(ed, false); }},
    {"buffer-close-other", "", "close every buffer but this one",
     [](Editor& ed, std::string_view) { CloseOtherBuffers(ed, false); }},
    {"bco!", "", "close every buffer but this one, discarding changes",
     [](Editor& ed, std::string_view) { CloseOtherBuffers(ed, true); }},
    {"buffer-close-other!", "", "close every buffer but this one, discarding changes",
     [](Editor& ed, std::string_view) { CloseOtherBuffers(ed, true); }},
    {"config-open", "", "open the koi config", ConfigOpenVerb},
    {"config-reference", "", "open the shipped reference config", ConfigReferenceVerb},
    {"theme", "<name>", "switch theme", ThemeVerb},
    {"set-indent", "<1-16>|tab", "indent this buffer with n spaces, or with a tab",
     [](Editor& ed, std::string_view rest) { SetIndent(ed, rest); }},
    {"set-excerpt-context", "<0-100>", "lines of context in the excerpt view, rebuilding it",
     [](Editor& ed, std::string_view rest) { SetExcerptContext(ed, rest); }},
    {"increment-excerpt-context", "", "a line more of context in the excerpt view",
     [](Editor& ed, std::string_view) { AdjustExcerptContext(ed, 1); }},
    {"decrement-excerpt-context", "", "a line less of context in the excerpt view",
     [](Editor& ed, std::string_view) { AdjustExcerptContext(ed, -1); }},
    {"set-language", "<lang>", "highlight this buffer as another language",
     [](Editor& ed, std::string_view rest) { SetLanguage(ed, rest); }},
    {"set-multicursor-paste", "spread|full",
     "paste N lines into N cursors one line each, or the whole text at each",
     [](Editor& ed, std::string_view rest) { SetMulticursorPaste(ed, rest); }},
    {"config-reload", "", "re-read the config without restarting",
     [](Editor& ed, std::string_view) { ed.reload_config = true; }},
    {"reload", "", "re-read every buffer from disk, keeping unsaved changes",
     [](Editor& ed, std::string_view) { ReloadEveryBuffer(ed, false); }},
    {"reload!", "", "re-read every buffer from disk, discarding unsaved changes",
     [](Editor& ed, std::string_view) { ReloadEveryBuffer(ed, true); }},
    {"sh", "<command>", "run a shell command",
     [](Editor& ed, std::string_view rest) { RunShellCommand(ed, rest, ShellMode::kDiscard); }},
    {"run-shell-command", "<command>", "run a shell command",
     [](Editor& ed, std::string_view rest) { RunShellCommand(ed, rest, ShellMode::kDiscard); }},
    {"pipe-to", "<command>", "pipe the selection to a command, ignoring its output",
     [](Editor& ed, std::string_view rest) { RunShellCommand(ed, rest, ShellMode::kPipeTo); }},
    {"pipe", "<command>", "replace the selection with what a command makes of it",
     [](Editor& ed, std::string_view rest) { RunShellCommand(ed, rest, ShellMode::kPipe); }},
    {"insert-output", "<command>", "insert a command's output before the selection",
     [](Editor& ed, std::string_view rest) {
       RunShellCommand(ed, rest, ShellMode::kInsertOutput);
     }},
    {"append-output", "<command>", "insert a command's output after the selection",
     [](Editor& ed, std::string_view rest) {
       RunShellCommand(ed, rest, ShellMode::kAppendOutput);
     }},
    {"from", "<command>", "run a command once; its file:line output becomes excerpts",
     [](Editor& ed, std::string_view rest) { CommandExcerpts(ed, rest); }},
    {"from-watched", "<command>", "like :from, re-running the command after every save",
     [](Editor& ed, std::string_view rest) { CommandExcerpts(ed, rest, true, false); }},
    {"from-with-msg", "<command>", "like :from for file:line:col: message output; the message rides the header",
     [](Editor& ed, std::string_view rest) { CommandExcerpts(ed, rest, false, true); }},
    {"from-watched-with-msg", "<command>", "watched, and with messages on the headers",
     [](Editor& ed, std::string_view rest) { CommandExcerpts(ed, rest, true, true); }},
    {"from-cancel", "[all]", "cancel this view's :from command (`all` for every one), or discard a finished result",
     [](Editor& ed, std::string_view rest) { FromCommandCancel(ed, rest); }},
    {"watch", "", "re-run this view's command on every save, like :from-watched",
     [](Editor& ed, std::string_view) { WatchView(ed); }},
    {"unwatch", "", "stop re-running this view's command on every save",
     [](Editor& ed, std::string_view) { UnwatchView(ed); }},
    {"pins-excerpt", "", "every pinned file, excerpted where you last were in it",
     [](Editor& ed, std::string_view) { PinExcerpts(ed); }},
    {"messages", "", "recent status messages, as a view",
     [](Editor& ed, std::string_view) { MessagesView(ed); }},
    {"pin", "<1-4>", "pin this file to a slot",
     [](Editor& ed, std::string_view rest) {
       if (int v = 0; SlotOf(ed, "pin", rest, v)) SetPinHere(ed, v);
     }},
    {"clear-pin", "<1-4>", "clear a pin slot",
     [](Editor& ed, std::string_view rest) {
       if (int v = 0; SlotOf(ed, "clear-pin", rest, v)) ClearPinSlot(ed, v);
     }},
    {"jump-pin", "<1-4>", "go to a pinned file, where you last were in it",
     [](Editor& ed, std::string_view rest) {
       if (int v = 0; SlotOf(ed, "jump-pin", rest, v)) JumpToPin(ed, v);
     }},
    {"jump-symbol", "<n>", "go to the nth most looked-up symbol",
     [](Editor& ed, std::string_view rest) {
       if (int v = 0; SlotOf(ed, "jump-symbol", rest, v)) JumpToHotSymbol(ed, v);
     }},
});

void RunTypableCommandBody(Editor& ed, std::string_view line) {
  line = TrimFront(line, " ");
  std::string_view verb = line;
  std::string_view rest;
  if (const size_t space = line.find(' '); space != std::string_view::npos) {
    verb = line.substr(0, space);
    rest = Trim(line.substr(space + 1), " \t");
  }

  const bool write_all = (verb == "wa") || (verb == "write-all") || (verb == "wa!");
  const bool write_force = (verb == "w!") || (verb == "write!") || (verb == "wa!");
  const bool quit_all = (verb == "qa") || (verb == "quit-all");
  const bool quit_all_force = (verb == "qa!") || (verb == "quit-all!");

  const bool write_all_quit =
      (verb == "wqa") || (verb == "write-all-quit") || (verb == "wqa!");
  const bool write_quit = (verb == "wq") || (verb == "x");
  const bool write_quit_force = (verb == "wq!") || (verb == "x!") || (verb == "wqa!");

  if (write_all) {
    std::ignore = SaveAllBuffers(ed, write_force);
  } else if ((verb == "w") || (verb == "write") || write_force) {
    if (SaveBuffer(ed, rest, write_force) && !IsExcerptView(ed.doc)) {
      ed.status = "wrote " + DisplayPath(ed.doc.file);
    }
  } else if ((verb == "q") || (verb == "quit") || quit_all) {

    if (const std::vector<std::string> unsaved = UnsavedBuffers(ed); !unsaved.empty()) {
      ed.status.Warn(UnsavedSummary(unsaved) + " -- :wqa to write all, :qa! to discard");
    } else {
      ed.quit = true;
    }
  } else if ((verb == "q!") || (verb == "quit!") || quit_all_force) {
    ed.quit = true;
  } else if (write_all_quit) {
    if (SaveAllBuffers(ed, write_quit_force, false)) {
      ed.quit = true;
    } else {
      std::ignore = RerunWatchedViews(ed);
    }
  } else if (write_quit || write_quit_force) {
    if (!SaveBuffer(ed, rest, write_quit_force, false)) return;
    if (const std::vector<std::string> unsaved = UnsavedBuffers(ed); !unsaved.empty()) {
      ed.status.Warn(UnsavedSummary(unsaved) + " -- :wqa to write all, :qa! to discard");
      std::ignore = RerunWatchedViews(ed);
    } else {
      ed.quit = true;
    }
  } else if (const auto found = std::ranges::find(kTypable, verb, &TypableDef::name);
             (found != kTypable.end()) && (found->run != nullptr)) {
    found->run(ed, rest);
  } else {
    ed.status.Warn("unknown command :" + std::string{verb});
  }
}

}

std::span<const TypableDef> TypableCommands() { return kTypable; }

std::vector<const TypableDef*> PromptCompletions(const Editor& ed) {
  std::vector<const TypableDef*> out;
  if (!ed.prompt_active || (ed.prompt_kind != PromptKind::kCommand)) return out;
  const std::string_view input{ed.prompt_input};
  if (input.find(' ') != std::string_view::npos) return out;
  for (const TypableDef& def : kTypable) {
    if (def.name.starts_with(input)) out.push_back(&def);
  }
  return out;
}

bool PromptComplete(Editor& ed) {
  const std::vector<const TypableDef*> matches = PromptCompletions(ed);
  if (matches.empty()) return false;

  std::string common{matches.front()->name};
  for (const TypableDef* def : matches) {
    std::size_t i = 0;
    while ((i < common.size()) && (i < def->name.size()) && (common[i] == def->name[i])) ++i;
    common.resize(i);
  }
  if (matches.size() == 1) common += ' ';
  if (common.size() <= ed.prompt_input.size()) return false;

  ed.prompt_input = common;
  ed.prompt_cursor = ed.prompt_input.size();
  return true;
}

void RunTypableCommand(Editor& ed, std::string_view line) {
  // Also reached straight from the `:` prompt, which is outside any batch: a
  // typed `:s/a/b` edits through the wrappers and has no other way to say where
  // the cursors were. Most of these commands (`:w`, `:q`, `:e`) never edit, so
  // the note goes back the moment the command is done.
  NoteCursorsForNextEdit(ed);
  RunTypableCommandBody(ed, line);
  DropCursorNote(ed.doc.table);
  ApplyModeInvariants(ed);
}

void RunCommands(Editor& ed, const std::vector<std::string>& names) {
  // Per command, not once for the batch: the commands before this one have
  // already moved the cursors, and what undo owes the user is where they were
  // when the edit happened -- not where the batch that led there started.
  // Batches that only move (every motion binding, the scroll wheel) must leave
  // no note at all, so the run drops whatever it did not spend on the way out.
  //
  // Any command at all ends the run of typed graphemes the re-indent memory
  // belongs to -- a motion, a mode change, an undo, the Return that opens the
  // next line. Only one more keystroke on the same word may consume it.
  ed.reindent = {};
  for (const std::string& name : names) {
    if (!name.empty() && (name.front() == ':')) {
      // Notes and drops for itself -- it is also reachable from the prompt.
      RunTypableCommand(ed, std::string_view{name}.substr(1));
      if (ed.quit) return;
      continue;
    }
    const CommandDef* def = FindCommand(name);
    if (def == nullptr) {
      ed.status.Warn(IsKnownUnimplemented(name)
                         ? ("\"" + name + "\" is a helix command koi does not implement yet")
                         : ("unknown command \"" + name + "\""));
      break;
    }
    NoteCursorsForNextEdit(ed);
    def->fn(ed);
  }
  DropCursorNote(ed.doc.table);

  if (ed.pending_char == PendingChar::kNone) ed.pending_count = 0;
  ApplyModeInvariants(ed);
}

}
