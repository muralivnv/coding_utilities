#ifndef KOI_NAVIGATE_H_
#define KOI_NAVIGATE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "editor.h"
#include "project.h"
#include "smartjump.h"
#include "symbols.h"

namespace koi {

void FilePicker(Editor& ed, std::string_view query = "");

// Which picker pipeline a smart-jump dead end opens -- a name PickerCommand
// knows, so the terms travel through the ordinary picker command.
std::string_view SmartPickerPipeline(SmartPicker picker);

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
// nothing unless the smart-jump prompt is the one that is open. Also arms or
// disarms the auto-jump, which is a lone match plus silence.
void SmartJumpPreview(Editor& ed);

// Whether the open prompt is sitting on a lone match waiting out
// kSmartAutoJumpSettle. The input loop asks so that it polls with a timeout
// instead of blocking: nothing else would wake it, and the settle is time
// passing rather than anything the user does.
bool SmartJumpSettling(const Editor& ed);

// The settle, checked. Fires the lone match the way Enter would once the prompt
// has been quiet for kSmartAutoJumpSettle, and does nothing before then.
void CheckSmartJumpAutoFire(Editor& ed);

// Enter. Lands on the best match, always -- stepping is the disambiguator, not
// a list view. Nothing found says so and hands the query to the picker; this
// never widens on its own.
void SmartJumpSubmit(Editor& ed, std::string_view line);

// Tab: close the prompt and hand what is typed to the picker the deciding
// clause names -- the same handoff the dead end makes, one keystroke earlier.
// Never a silent widening: the user asked for it.
void SmartJumpToPicker(Editor& ed);

// The last query's ranked list, one row at a time, wrapping like search does.
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

std::string ExpandPickerCommand(const Editor& ed, std::string_view command,
                                std::string_view query);

std::string PickerCommand(const Editor& ed, std::string_view name);

// Every picker there is. LastPicker has to know each of these names, or a
// picker records itself as the last one and then cannot be reopened.
std::vector<std::string_view> PickerNames();

// Which picker ran last, and with what query. Exposed as a pair so a test can
// check the reader inverts the writer.
void WriteLastPicker(std::string_view name, std::string_view query);
bool ReadLastPicker(std::string& name, std::string& query);

std::string FileFilterCommand(const Editor& ed);

std::string RankedFileRows(Editor& ed);

std::string BufferPickerItems(const Editor& ed);
void ChooseBufferRow(Editor& ed, std::string_view payload);

}

#endif
