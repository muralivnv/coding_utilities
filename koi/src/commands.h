#ifndef KOI_COMMANDS_H_
#define KOI_COMMANDS_H_

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "editor.h"
#include "keymap.h"

namespace koi {

using CommandFn = void (*)(Editor&);

struct CommandDef {
  std::string_view name;
  CommandFn fn;
  std::string_view help;
};

const CommandDef* FindCommand(std::string_view name);

std::span<const CommandDef> AllCommands();

bool IsKnownUnimplemented(std::string_view name);

void RunCommands(Editor& ed, const std::vector<std::string>& names);

void ApplyPendingChar(Editor& ed, std::string_view grapheme);

// -- leap --------------------------------------------------------------------

// The label keys, in the order they are handed out -- home row first, so the
// nearest matches get the keys the fingers are already on. Its length is also
// the cap on how many targets can wear a label at once; space offers the next
// group, and cycles back to the first after the last.
inline constexpr std::string_view kLeapKeys = "asdfghjklqwertuiopzxcvbnm";

// Whether the capture in flight is a leap's, at any of its three stages. Here
// rather than in commands.cpp's anonymous namespace because the key path has
// to know: a leap says in its own words why it ended, and the pending-char
// router's vocabulary -- "not a character" -- is about `r` and `f`, whose
// argument really is one.
inline bool IsLeapPending(PendingChar kind) {
  return (kind == PendingChar::kLeapFirst) || (kind == PendingChar::kLeapSecond) ||
         (kind == PendingChar::kLeapLabel);
}

void StartLeap(Editor& ed);

// One keypress of an armed leap. Returns true when it armed itself for the
// next one, false when the mode is over -- a jump, a cancel and a warning all
// end here.
bool StepLeap(Editor& ed, PendingChar stage, std::string_view grapheme);

// Whether the leap overlay still describes the text it was measured in. False
// once the focused buffer or its contents have been swapped underneath it,
// which is the renderer's cue to draw nothing rather than to paint labels over
// a document they were never computed for.
bool LeapIsLive(const Editor& ed);

// What the status line says while a leap is armed, and empty when none is or
// when the one armed no longer describes the text. The status bar draws this
// in preference to ed.status: a background warning can take ed.status with no
// keystroke to catch it, and the mode has to keep explaining itself for as
// long as its labels are on the screen. The warning is not lost -- it is still
// in ed.status and in :messages, and the bar shows it once the mode ends.
std::string_view LeapHint(const Editor& ed);

// The terminal changed size. A mode measured against the pane that is gone
// ends here, before the event is dropped and the next fit re-measures
// everything else.
void HandleResize(Editor& ed);

// The label key on the match starting at `pos`, or 0 when none does.
char LeapLabelAt(const LeapState& leap, Index pos);

// What StepLeap reads as "pick this label's match as an extra cursor": alt on
// a label key arrives as a chord rather than a character, so the key path
// spells it as this prefix plus the lowercase letter. A control byte
// IsSelfInsert refuses, so no typed grapheme can arrive looking like one.
inline constexpr char kLeapPickPrefix = '\x01';

struct TypableDef {
  std::string_view name;
  std::string_view args;
  std::string_view help;
  void (*run)(Editor& ed, std::string_view rest){nullptr};
};

std::span<const TypableDef> TypableCommands();

std::vector<const TypableDef*> PromptCompletions(const Editor& ed);

bool PromptComplete(Editor& ed);

void PromptSubmit(Editor& ed);

void RunSearch(Editor& ed, std::string_view pattern);

void SearchStep(Editor& ed, bool forward);

void SelectRegex(Editor& ed, std::string_view pattern);

std::string_view ActiveSearchPattern(const Editor& ed);

bool SearchIsConfinedToSelections(const Editor& ed);

void AttachSyntax(Editor& ed);

bool ApplyTheme(Editor& ed, std::string_view name);

void RefreshCaptureStyles(Editor& ed);

Index MatchingBracket(const PieceTable& table, Index at, Index window = 128 * 1024);

void RecordJump(Editor& ed);

void StepJump(Editor& ed, bool forward);

void RunTypableCommand(Editor& ed, std::string_view line);

bool ReloadDocument(Editor& ed);

void ReloadEveryBuffer(Editor& ed, bool force = false);

void CheckDiskChange(Editor& ed);

bool OpenTarget(Editor& ed, std::string_view spec);

void HandleKeyInput(Editor& ed, const KeyMaps& maps, const Key& key, std::vector<Key>& pending);

bool IsSelfInsert(const Key& key);

std::string KeyText(const Key& key);

void FlushPendingAsText(Editor& ed, std::vector<Key>& pending);

void ApplyPaste(Editor& ed, std::string_view text);

// How clipboard text divides over `cursors` cursors. Returns one piece per
// cursor when the text can be spread, and otherwise a single piece that every
// cursor pastes a copy of. `remembered` is what koi last wrote to the
// clipboard, one entry per selection, and is believed only while the clipboard
// still holds exactly those bytes. `spread` is editor.multi-cursor-paste.
std::vector<std::string> ClipboardPieces(std::string_view text,
                                         std::span<const std::string> remembered,
                                         std::size_t cursors, bool spread);

}

#endif
