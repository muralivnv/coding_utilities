#ifndef KOI_NAVIGATE_H_
#define KOI_NAVIGATE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor.h"
#include "mmap_stream.h"
#include "project.h"
#include "smartjump.h"
#include "symbols.h"

namespace koi {

void FilePicker(Editor& ed, std::string_view query = "");

void BufferPicker(Editor& ed, std::string_view query = "");

void ContentPicker(Editor& ed, std::string_view query = "");

void SymbolPicker(Editor& ed, std::string_view query = "");

void BufferSymbolPicker(Editor& ed, std::string_view query = "");

void GotoDefinition(Editor& ed);

void ShowReferences(Editor& ed);

void ShowReferenceExcerpts(Editor& ed);
void GotoExcerptSource(Editor& ed);

void OpenReferenceExcerpts(Editor& ed, const std::vector<Symbol>& found, std::string_view word);

void SearchExcerpts(Editor& ed, std::string_view query);

bool SaveExcerptView(Editor& ed);

void ShowDefinitionExcerpts(Editor& ed);

void CommandExcerpts(Editor& ed, std::string_view command, bool watched = false,
                     bool with_msg = false);

void PinExcerpts(Editor& ed);
void MessagesView(Editor& ed);
void WatchView(Editor& ed);
void UnwatchView(Editor& ed);

bool StartCommandJob(Editor& ed, std::string_view command, bool watched, bool with_msg,
                     PendingCommand::Then then);

bool PumpCommandJobs(Editor& ed, bool defer_present = false);

bool CancelCommandJob(Editor& ed);

void KillAllCommandJobs(Editor& ed);

// Grows the scan pool to `workers` live threads; never shrinks it (shrinking
// joins, and a join blocks on whatever a worker is scanning).
void StartScanWorker(Index workers);
void StopScanWorker();

void MaybeRefreshExcerptView(Editor& ed);

int RerunWatchedViews(Editor& ed);

void MarkLiveViewsStale(Editor& ed, ExcerptView::Kind kind);

void RefreshLiveExcerptViews(Editor& ed);

void DropExcerptHunk(Editor& ed);

// What one line of an excerpt view is. The renderer paints from this and
// SelectExcerptMatches selects from it, so the two cannot drift: whatever wears
// `ui.excerpt.match` on screen is exactly what the command selects.
enum class ExcerptLine : std::uint8_t { kPlain, kHeader, kWholeLineMatch, kSpanMatches };

// `line` is a line's content without its newline. `spans` is cleared, and
// filled with byte offsets into `line` -- clamped to it, never empty -- only
// for kSpanMatches.
ExcerptLine ClassifyExcerptLine(const ExcerptView& view, std::string_view line,
                                std::vector<Interval>& spans);

// Every painted match in the view, as a cursor. Refuses outside an excerpt
// view, and leaves the selections alone when nothing is painted.
void SelectExcerptMatches(Editor& ed);

bool RebuildExcerptView(Editor& ed);

void AlignExcerptModel(Document& doc);

void DropUnreachableEpochs(Document& doc);

void LastPicker(Editor& ed);


void SetPinHere(Editor& ed, int slot);
void ClearPinSlot(Editor& ed, int slot);
void JumpToPin(Editor& ed, int slot);

void GoToLastEdit(Editor& ed);

// -- smart jump ---------------------------------------------------------------
//
// The prompt half of docs/smart-jump.md: the parser and the pipeline are in
// smartjump.h, and these are what drive them from the editor. No prompt of its
// own -- the existing one, with the parse's answer on the status line, so that
// what Enter will do is visible before it is pressed.

// Opens the prompt and takes the corpus snapshot. One store read and one stat
// sweep; nothing after this touches disk until Enter.
void SmartJumpPrompt(Editor& ed);

// One keystroke: re-score the whole snapshot and say what Enter would do. Does
// nothing unless the smart-jump prompt is the one that is open.
void SmartJumpPreview(Editor& ed);

// Enter. Lands on the best match, always -- stepping is the disambiguator, not
// a list view. Nothing found says so and hands the query to the picker; this
// never widens on its own.
void SmartJumpSubmit(Editor& ed, std::string_view line);

// Tab: close the prompt and hand what is typed to the picker the deciding
// clause names -- the same handoff the dead end makes, one keystroke earlier.
// Never a silent widening: the user asked for it.
void SmartJumpToPicker(Editor& ed);

// The last query's ranked list, one row at a time, wrapping like search does.
// One of the two backends picker_jump_next dispatches into, and the only one
// that touches the store's own walking state -- no key is bound to it.
void SmartJumpStep(Editor& ed, bool forward);

// How long a smart-jump arrival has to be stood in before it is recorded. An
// abandoned mis-jump that records itself is zoxide's documented trust-killer,
// so this one records nothing at all (docs/smart-jump.md, Recording).
inline constexpr double kBounceSeconds = 2.0;

void JumpToHotSymbol(Editor& ed, int index);

// Bumps the file's own row: this is where the buffer was last left, and one
// more visit to it. The jump-motion call sites are where a place is worth
// counting -- everything else the recorder learns, it learns at a boundary.
void RecordVisitHere(Editor& ed);

// One edit boundary at the current place: a `locations` row of kind edit, the
// `files` bump behind it, the enclosing symbol. Debounced, so a burst of typing
// is one record and not one per keystroke.
void RecordEditHere(Editor& ed);

// How long the cursor has to sit still before where it is sitting counts as a
// place visited. Three seconds is the design's; it is long enough that passing
// through a file does not record every line of it, and short enough that
// stopping to read something records it.
inline constexpr double kLingerSeconds = 3.0;

// Everything a `locations` row says about where the primary cursor is: the
// position, the enclosing symbol from live syntax only, the line's text and its
// neighbours, how unique that text is in the buffer, and the disk blob when the
// buffer is clean against its file. False -- and `out` untouched -- for an
// excerpt view and for a buffer with no file, which are not places in the
// corpus.
bool LocationHere(Editor& ed, LocationRecord& out);

// That a row for the current place has just been written by somebody else --
// the jump list, which owns its own transaction and the pane's cursor. Stops
// the linger recording the same place again, and bumps the symbol.
void NoteRecordedHere(Editor& ed, const LocationRecord& row);

// The two halves of boundary recording, called from the input loop around the
// command an event turns into. Before: the linger check, which needs the place
// as it was while the user was sitting in it. After: whether the command edited
// the buffer, and where the cursor has ended up.
void NoteInputBoundary(Editor& ed);
void NoteCommandBoundary(Editor& ed);

void RestoreLastPosition(Editor& ed);

// Which picker ran last, and with what query. Exposed as a pair so a test can
// check the reader inverts the writer.
void WriteLastPicker(std::string_view name, std::string_view query);
bool ReadLastPicker(std::string& name, std::string& query);

std::string FileFilterCommand(const Editor& ed);

// One of the file filter's paths, ranked: frecency order with the branch
// bonus, line and column from the store where it knows the file.
struct RankedFile {
  std::string path;
  Index line{1};
  Index column{1};
  bool visited{false};
};

std::vector<RankedFile> RankedFiles(Editor& ed);

// One open buffer as a picker row: `text` is what the band shows, `payload` is
// what ChooseBufferRow acts on.
struct BufferRow {
  std::string text;
  std::string payload;
};

std::vector<BufferRow> BufferPickerRows(const Editor& ed);

// False when nothing was switched to: an empty payload, an `#index` past the
// end of the buffer list, or a path that would not open.
bool ChooseBufferRow(Editor& ed, std::string_view payload);

// -- in-process picker --------------------------------------------------------
//
// The smart-jump box with a band of rows hanging off it (IN_PROCESS_PICKER.md).
// Every picker there is: files, buffers, defs, refs, this file's symbols, and
// the two the band consumes a child for -- project symbols and content.

// Rows the band draws: a window on the filtered list, not a cap on it. Buffers
// grows past this to whatever the screen fits (RenderInto); everywhere else the
// selection scrolls the window instead.
inline constexpr std::size_t kPickerRows = 5;

// Narrowest the card is drawn, whatever the rows are: a band that shrinks to
// two words reads as a tooltip rather than as a list. Files and buffers only --
// everything else takes the screen (kPickerCardWide).
inline constexpr int kPickerCardMin = 40;

// What a picker over file content asks for instead of a measurement: wider than
// any screen, which the renderer turns into the whole screen width. Defs, refs,
// symbols and content all ask for it -- their rows are lines of source and
// paths, and a card measured to them either fills the screen anyway or, worse,
// gets only the columns to the right of a caret that happens to sit late in a
// line. Measuring content's rows would also mean a DisplayWidth pass over
// hundreds of thousands of lines to learn the longest is longer than the screen.
inline constexpr int kPickerCardWide = 1 << 20;

// Files the line cache holds at once. A refs session walking a hundred files
// must not keep every one of their bytes until the prompt closes; past this the
// least recently used file is dropped, and a walk coming back re-reads it. Tune
// from feel: big enough that stepping back and forth over a handful of files
// never re-reads, small enough that the cache is not the session's memory.
inline constexpr std::size_t kPickerCacheFiles = 16;

// Bytes of a child's output one wake takes off the pipe. ReadAvailable's own
// ceiling is eight megabytes, which is a whole frame of parsing and matching if
// the producer can fill it; what the band wants is a steady trickle it can
// finish with. Tune from feel: too low and the list fills lazily, too high and
// typing stutters mid-scan.
inline constexpr std::size_t kPickerWakeBudget = 1u << 20;

// Bytes of corpus one wake carries a changed pattern over. A keystroke does not
// wait for the whole corpus: the catch-up rides the pump a budget at a time and
// the band keeps the last good list until it lands. Bigger than the read
// budget, so a catch-up gains on a live scan instead of chasing it forever.
inline constexpr std::size_t kPickerMatchBudget = 2u << 20;

// How many of a picker's filtered rows the walk keeps once the prompt is gone.
// Past a few thousand matches n/N is not how anyone navigates, and the cap is
// what lets accept copy the rows out and free everything the pick was holding.
inline constexpr std::size_t kWalkRows = 2000;

// One band row. `text` is the left of the row and what the filter matches;
// `detail` is dimmed -- a tail on a file row, the `path:line` at the card's
// right edge on a symbol row. Files carry a path to open at line:column,
// buffers the payload ChooseBufferRow already knows how to act on. A defs or
// refs row's text is the target line's own content, captured by the scan that
// found the row -- so the filter judges every row without opening anything, and
// only the rows the band is showing read the file again to stay current with
// it; `read` is whether that has happened. A symbol list's row is the name
// itself and never reads at all. `name` is the symbol a landing records either
// way, and what a row falls back to when its line will not read.
struct PickerEntry {
  std::string text;
  std::string detail;
  std::string target;
  Index line{1};
  Index column{1};
  std::string name;
  bool read{false};
};

// A file's bytes under the stamp the read saw: a selection stepping through one
// file's rows reads it once, and a file rewritten under an open picker is read
// again. Only files no buffer holds -- an open buffer answers from its own
// table, unsaved edits and all.
//
// Flat: the whole file in one buffer plus the offset of each line's newline, so
// a line is a view and not a string. At most eight of them are ever displayed
// (the band's rows and the block's lines) and those are the only ones copied.
// The bytes are stable while the entry lives -- the map's nodes do not move on
// rehash -- so a view outlives the call that took it; only eviction ends one,
// and nothing holds a view across that.
struct PickerFileLines {
  FileStamp stamp;
  std::string bytes;
  // One per line: where the line ends, at its '\n' or at the end of the bytes
  // for a last line nothing terminated. The line before it starts one past the
  // entry before, or at 0.
  std::vector<std::size_t> eols;
  // When this file was last asked for, against PickerState::lines_clock. The
  // smallest is what eviction drops.
  std::uint64_t used{0};

  std::size_t LineCount() const { return eols.size(); }

  // Line `at`, 0-based, without its newline or the \r before it.
  std::string_view Line(std::size_t at) const {
    if (at >= eols.size()) return {};
    const std::size_t from = (at == 0) ? 0 : (eols[at - 1] + 1);
    std::string_view line{bytes.data() + from, eols[at] - from};
    while (!line.empty() && (line.back() == '\r')) line.remove_suffix(1);
    return line;
  }
};

// The child behind a streaming picker, and everything the prompt keeps for it.
// RAII on purpose: the destructor kills the child's process group, closes the
// pipe and reaps it, so every way a prompt can end is already a cleanup path --
// PromptCancel resetting the state, accept moving it out, a new picker
// replacing a live one. No caller has to remember, and none can leak a child
// or leave a zombie behind.
struct PickerScan {
  int pid{-1};
  int fd{-1};
  // The child's stdout as it arrives, and how much of it has become rows. Only
  // complete lines are parsed; a half-written one waits for the next read and
  // pipe close flushes it. `parsed` is a byte offset and never a pointer,
  // because the mapping relocates as it grows.
  common::MmapStream out;
  std::size_t parsed{0};
  // How far the newline search has already looked, which is ahead of `parsed`
  // only inside a stretch with no newline in it. One enormous line -- a minified
  // bundle -- would otherwise be searched from `parsed` again every wake, and a
  // memchr over the same megabytes per read is quadratic.
  std::size_t searched{0};
  // Complete lines the corpus has yielded so far. For content that is the m the
  // count says, because there are no rows to count -- the corpus is the rows.
  std::size_t lines{0};
  // The pattern rows are judged by as they land, and the one the band's list was
  // built with. A half-typed one keeps the band's last good list, and rows
  // arriving under it join that list rather than one nobody asked for.
  std::string filter;
  // The catch-up: a changed pattern being carried over the corpus-so-far a
  // budget of bytes per wake, so a keystroke never waits behind more than one
  // budget of matching. `refilter_shown` is the list being built and the band
  // keeps showing the old one until the two meet -- the same thing a pattern
  // that will not compile leaves there. While this runs the pump appends
  // nothing itself: the bytes it parses are ahead of `refilter_at`, and the
  // catch-up is what judges them, once.
  bool refiltering{false};
  std::string refilter_pattern;
  std::size_t refilter_at{0};
  std::vector<std::size_t> refilter_shown;
  // `picker-corpus-max-bytes` as it read when this scan's child was spawned.
  // Its own copy because the pump weighs the corpus against it every wake --
  // the same cost the constant it replaced had -- and because a live scan must
  // not change size under itself: a :config-reload lands on the next scan.
  std::uintmax_t corpus_max{kDefaultPickerCorpusMax};
  // The pipe has closed: no more rows, and the count drops its note.
  bool done{false};
  // The corpus reached `corpus_max` and the rest of the child's output was never
  // read. What landed is on the band and is not the whole answer, which is the
  // one thing the count must not let the reader assume.
  bool truncated{false};

  PickerScan() = default;
  PickerScan(const PickerScan&) = delete;
  PickerScan& operator=(const PickerScan&) = delete;
  ~PickerScan();
};

struct PickerState {
  enum class Source : std::uint8_t {
    kFiles,
    kBuffers,
    kDefs,
    kRefs,
    kFileSymbols,
    kProjectSymbols,
    kContent,
  };
  Source source{Source::kFiles};
  // Empty for content: its candidates are unbounded, so the scan's corpus is the
  // row storage and a match is a byte offset into it (PickerRowText below).
  std::vector<PickerEntry> rows;
  // Indices into rows, in row order: the filter selects, never reorders. For
  // content, byte offsets of matched lines in the corpus, in corpus order --
  // same meaning, a different thing indexed.
  std::vector<std::size_t> shown;
  // Which shown row is selected -- anywhere in the list, not just on the band --
  // and the first shown row the band is drawing, scrolled to keep the selection
  // in view. `window` is how many rows the band drew last frame: only the
  // renderer knows what the screen fits, and kPickerRows until it says.
  std::size_t selected{0};
  std::size_t offset{0};
  std::size_t window{kPickerRows};
  // The one width the block and the band are both drawn at. Computed when the
  // shown list changes and never on a step, so walking into a longer file does
  // not resize the card under the eyes; long rows clip instead.
  int card_w{0};
  // The band's lead column for a source that draws a block: the width its
  // details are aligned at, measured by the renderer over the rows one frame
  // drew -- the shown list is unbounded and a pass over it per keystroke is
  // not. Grows and never shrinks while the filter stands, so stepping into a
  // longer row widens the column once instead of moving every detail on every
  // step. Cleared with card_w, where the list is rebuilt.
  int lead_w{0};
  // The widest detail the same rows carried, measured the same way: the lead's
  // column is capped at what this leaves, so a short path hands its slack to
  // the lines instead of a fixed fraction wasting it.
  int detail_w{0};
  // What last_picker reopens this pick with. Defs and refs keep the word they
  // were looked up for -- what is typed into their band filters that word's
  // hits, it does not name another word -- everywhere else it is what is typed.
  std::string query;
  // The input the list on the band was built for. A key that changes nothing --
  // delete at the end of the input, ctrl-u at column 0 -- still comes through
  // the filter pass, and a rebuilt list is the selection back at the top. Unset
  // rather than empty until the open's own pass has run, because empty is an
  // input like any other and the first list has to be built.
  std::optional<std::string> last_input;
  // The last input that compiled, which is what is written down for
  // last_picker. A pattern that threw filtered nothing, and recording it would
  // reopen the same throw every time, for good. Empty until one does, so a
  // picker that never had a good pattern resumes unfiltered.
  std::string good_input;
  // The lines around the selected row's target and the number of the first of
  // them, refilled whenever the selection moves. Empty for a source that draws
  // no context block, and for a target whose file cannot be read.
  std::vector<std::string> context;
  Index context_first{0};
  // Which of those lines is the target, so the renderer marks it without asking
  // what holds the row. Empty context, and it means nothing.
  Index context_target{0};
  // What has been read off disk for the rows and the block, per path, capped at
  // kPickerCacheFiles. `lines_clock` ticks once per lookup and is what the
  // entries' `used` is stamped from, so eviction knows which file is coldest.
  std::unordered_map<std::string, PickerFileLines> lines;
  std::uint64_t lines_clock{0};
  // `picker-file-max-bytes` as it read when this prompt opened. Its own copy
  // because the cache weighs a stat against it on every lookup, and because a
  // reload lands on the next prompt rather than changing what a live one reads.
  std::uintmax_t file_max{kDefaultPickerFileMax};
  // The scan still feeding `rows`, for a source that streams. Null everywhere
  // else, and dead the moment this state is.
  std::unique_ptr<PickerScan> scan;
};

// What a picker records itself as, and the name LastPicker reopens it by. One
// mapping, so a source that cannot be reopened is an arm missing from
// LastPicker rather than a name written down in two places.
std::string_view PickerSourceName(PickerState::Source source);

// -- resolving a row ----------------------------------------------------------
//
// What `shown[at]` names depends on the source: an entry in `rows` everywhere
// but content, and a byte offset into the scan's corpus for content, whose
// `rows` is empty. Everything that reads a shown row -- the band, the card, the
// block, accept, the walk -- goes through these, so only they know the
// difference. Nothing here copies the corpus: a content row's text is a view
// into it and dies with it.

// The row's text: what the filter matches. For content it is the whole matched
// line, its `path:line:` head included -- the query filters by path as much as
// by content, so the head has to stay matchable. What is drawn is that line
// split in two, by the pair below.
std::string_view PickerRowText(const PickerState& state, std::size_t at);

// What leads the row on the band, and what the card is measured against: the
// text without the `path:line:` head content's rows carry, because that head
// rides the right edge as the detail instead of taking width off the line the
// row is for. Every other source leads with its whole text.
std::string_view PickerRowLead(const PickerState& state, std::size_t at);

// Dimmed at the card's right edge: where the row points. `path:line` for a
// source that draws a block, parsed back off the head for content; a file row's
// visited line alone, which reads as part of the path it follows.
std::string_view PickerRowDetail(const PickerState& state, std::size_t at);

// The entry form, for the two passes that walk `rows` by index rather than the
// shown list. Content never reaches them -- it has no rows to walk.
inline std::string_view PickerRowText(const PickerState&, const PickerEntry& row) {
  return row.text;
}

// Where a shown row points, and the symbol a landing records. Parsed off the
// corpus line for content -- on demand, for the handful of rows the block, the
// accept and the walk's copy-out actually want. False, and `out` cleared, for no
// such row or a line with no `path:line:` head.
struct PickerTarget {
  std::string path;
  Index line{1};
  Index column{1};
  std::string name;
};
bool PickerRowTarget(const PickerState& state, std::size_t at, PickerTarget& out);

// How many candidates there are: rows for a list, complete corpus lines for
// content. The m in the band's i/n/m.
std::size_t PickerTotal(const PickerState& state);

// Columns a count takes, without building the string to find out. The band pads
// its index to the shown count's width and the card budgets for the whole of
// `i/n/m`, so both ask this rather than measuring a number they printed.
inline int PickerCountDigits(std::size_t count) {
  int digits = 1;
  while (count >= 10) {
    count /= 10;
    ++digits;
  }
  return digits;
}

// Symbol, reference and content rows carry the line they point at and draw a
// context block around the selected one; file and buffer rows do neither -- a
// path is its own evidence. The two go together: a source that draws a block is
// exactly a source whose rows keep `path:line` in a column of its own, out of
// the one the row's own text runs in.
bool PickerShowsContext(PickerState::Source source);

// The card's width, from the shown rows. Called where the shown list changes
// and nowhere else: the renderer draws at what is stored, so no step and no
// scroll can resize the card.
int PickerCardWidth(const PickerState& state);

// One keystroke: recompile "(?i)" + input and keep the rows that match. A
// pattern that does not compile keeps the last good list and says why in the
// branch row. Declines unless the picker prompt is the one open.
void PickerRefilter(Editor& ed);

// Tab / arrows: move the selection through the whole filtered list, wrapping at
// its ends, with the band's window scrolled to keep the selection on screen.
void PickerStep(Editor& ed, bool forward);

// Reads what the band's window and the context block need for the current
// selection -- called where the selection or the list moves, and by the
// excerpt-context keys so a new depth shows without a step.
void PickerFillShown(Editor& ed, PickerState& state);

// The window follows the selection rather than the other way round: it moves
// only when the selection would be off it, so walking inside the band leaves
// the rows where the eyes left them. Called on a step, and by the renderer when
// a resize changes how many rows the band has.
void PickerScrollToSelected(PickerState& state);

// Enter, or alt+digit: open the window's row `at` (0-based, so a digit names
// the same row wherever the band is scrolled), negative opening the selected
// row.
// Closes the prompt and frees the state on success, and does nothing on a row
// the band is not showing.
void PickerAccept(Editor& ed, int at = -1);

// -- streaming ----------------------------------------------------------------
//
// The project-symbol scan stays a subprocess and the prompt becomes its
// consumer. Rows only ever join at the bottom: the hot head the child sends
// first never moves, and nothing the eye is on is reordered under it.

// What the count says while the child is live, dimmed beside the n/m.
inline constexpr std::string_view kPickerScanNote = " scanning…";

// And what it says instead when the corpus hit its ceiling. Same width as the
// scanning note, so the card the scan sized does not move when it lands.
inline constexpr std::string_view kPickerCutNote = " truncated";

// The child a streaming source runs: the file filter piped into one producer.
// "content" and "symbols" are the only two; anything else is empty, because
// every other picker builds its rows in process.
std::string PickerScanCommand(const Editor& ed, std::string_view name);

// Opens the picker prompt over a child's output, with no rows yet. `command` is
// a whole shell command; the tests hand it a producer instead of a scan.
bool PickerStartScan(Editor& ed, PickerState::Source source, const std::string& command,
                     std::string_view query);

// The band is still moving: a child still writing, or a changed pattern still
// being carried over what it already wrote. Either way the list on screen is
// not the whole answer yet, which is what the count's note says.
bool PickerScanLive(const PickerState& state);

// What the count carries beside the n/m: that the list is still coming, or that
// it stopped short of the project. Empty when n/m is the whole answer -- which
// includes a scan a read error ended, because there the warning says what
// happened and a note on the band could only say it again, smaller.
std::string_view PickerCountNote(const PickerState& state);

// What the input loop polls on: a picker prompt whose band is still moving. The
// catch-up needs the wakes as much as the child does -- the child can be long
// gone and the pattern still have megabytes to meet.
bool PickerScanning(const Editor& ed);

// One wake's worth of the child: read a budget of bytes, turn the complete
// lines into rows, filter the new ones onto the band, and carry a changed
// pattern a budget further over the corpus. Append-only -- no existing shown row
// moves, nothing is re-sorted, and the selection stays where it is -- except at
// the one moment a catch-up finishes and hands its list over. True when the
// band changed, so the caller can tell a wake that did something from one that
// did not.
bool PickerPumpScan(Editor& ed);

// -- the walk -----------------------------------------------------------------
//
// One contract, two lists behind it: enter lands on the first, n/N walk what it
// came from, the branch row names where the next press goes.

// One row the walk owns. `target` is a path, or the payload ChooseBufferRow
// knows; `name` is the symbol a landing records, empty for a file or a buffer.
// `display` is what the branch row calls it -- kept rather than rebuilt,
// because a buffer row's payload is an index and names nothing.
struct WalkRow {
  std::string target;
  std::string display;
  std::string name;
  Index line{1};
  Index column{1};
};

// A picker's filtered list, alive past its prompt the way SmartJumpState is
// alive past the smart-jump prompt. Rows owned outright, never offsets into a
// corpus, so accept can free the pick whole.
struct WalkList {
  PickerState::Source source{PickerState::Source::kFiles};
  std::vector<WalkRow> rows;
  // Which row the last landing was, so next and prev step from where you are.
  std::size_t at{0};
};

// picker_jump_next / picker_jump_prev. The picker walk when a pick made the
// last list, smart-jump's own stepping otherwise -- whichever produced a list
// last owns the keys, and a new list frees the one it replaces.
void PickerJumpStep(Editor& ed, bool forward);

// A scratch or excerpt row's target is `#index` -- a position in the buffer
// list, not a buffer -- so closing one re-points every row past it. Called from
// the close, which drops the walk rather than repairing it: a buffers list is a
// screenful just seen, not a session artefact. Named-file rows re-resolve by
// path, but the list is one source and goes whole.
void DropBuffersWalk(Editor& ed);

}

#endif
