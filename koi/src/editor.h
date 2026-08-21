#ifndef KOI_EDITOR_H_
#define KOI_EDITOR_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "piece_doc.h"
#include "selection.h"
#include "symbols.h"
#include "syntax.h"
#include "theme.h"

namespace koi {

struct JumpStore;
struct ProjectStore;
struct Syntax;
class KeyRecorder;

enum class StatusLevel : std::uint8_t { kInfo, kWarning, kError };

struct StatusRecord {
  std::string text;
  StatusLevel level{StatusLevel::kInfo};
};

class StatusMessage {
 public:
  StatusMessage& operator=(std::string text) {
    Keep();
    text_ = std::move(text);
    return *this;
  }
  StatusMessage& operator=(std::string_view text) { return *this = std::string{text}; }
  StatusMessage& operator=(const char* text) { return *this = std::string{text}; }
  StatusMessage& operator+=(std::string_view text) {
    text_.append(text);
    return *this;
  }

  void Warn(std::string text) {
    Keep();
    text_ = std::move(text);
    if (level_ < StatusLevel::kWarning) level_ = StatusLevel::kWarning;
  }
  void Fail(std::string text) {
    Keep();
    text_ = std::move(text);
    level_ = StatusLevel::kError;
  }
  void Log(std::string text, StatusLevel level) {
    if (text.empty()) return;
    if (!log_->empty() && (log_->back().text == text)) return;
    log_->push_back(StatusRecord{std::move(text), level});
    if (log_->size() > kLogCap) log_->erase(log_->begin());
  }

  void clear() {
    Keep();
    text_.clear();
    level_ = StatusLevel::kInfo;
  }
  bool empty() const { return text_.empty(); }
  std::size_t find(std::string_view needle) const { return text_.find(needle); }
  const std::string& text() const { return text_; }
  StatusLevel level() const { return level_; }
  const std::vector<StatusRecord>& log() const { return *log_; }
  operator const std::string&() const { return text_; }

 private:
  void Keep() { Log(text_, level_); }

  static constexpr std::size_t kLogCap = 200;
  std::string text_;
  StatusLevel level_{StatusLevel::kInfo};
  std::shared_ptr<std::vector<StatusRecord>> log_{std::make_shared<std::vector<StatusRecord>>()};
};

struct Viewport {
  Index top_line{0};
  Index left_column{0};
  Index rows{24};
  Index columns{80};
  Index scrolloff{3};
  Index top_row{0};
};

struct ExcerptRef {
  std::string path;
  Index line{1};
  Index col{0};
  std::string msg;
};

struct ExcerptBlock {
  Index header_line{0};
  std::string header;
  std::string path;
  Index line{1};
  Index col{0};
  Index first{1};
  Index last{1};
  std::string header_prefix;
  std::string header_note;
  std::string original;
  bool synthesized_newline{false};
  bool no_body{false};
  std::vector<std::string> prior_headers;
};

struct DroppedExcerpt {
  ExcerptBlock block;
  std::vector<ExcerptRef> refs;
};

// What a file looked like when koi last read or wrote it, for "has it changed
// under us?" -- asked by the excerpt views of every file they quote, and by
// every file-backed buffer before it writes itself back.
//
// The mtime alone cannot answer that. Its granularity is a filesystem's
// business -- a whole second on ext3, HFS+ and many NFS mounts, and on Linux a
// coarse clock tick for any inode nobody has stat()ed since it was last
// written -- so a write that lands in the same tick as the one we recorded
// carries the very stamp we already hold. koi's own writes make that the
// common case rather than the rare one: AtomicWriteFile renames a fresh temp
// file into place, and that inode's timestamp comes from the coarse clock
// however carefully the old one was measured. Measured on the filesystem this
// suite's fixtures live on (btrfs, Linux 7.1), 199903 of 200000 write-then-
// rename pairs left the file's mtime bit-identical to what it was before.
//
// The size is the other half of the answer, it comes from the same stat, and
// it moves for every change that adds or removes a byte -- which is what an
// edit written back to a file almost always does.
struct FileStamp {
  std::string path;
  // min() is "koi has never read this file": no stat can return it, so a
  // default-constructed stamp compares unequal to whatever is on disk, and the
  // holder is told the file changed rather than told it did not.
  std::filesystem::file_time_type mtime{std::filesystem::file_time_type::min()};
  std::uintmax_t size{0};

  bool SameFile(const FileStamp& other) const {
    return (mtime == other.mtime) && (size == other.size);
  }
};

struct ExcerptView {
  enum class Kind : std::uint8_t { kReferences, kDefinitions, kSearch, kCommand, kPins };
  Kind kind{Kind::kReferences};
  std::string word;
  bool watched{false};
  bool with_msg{false};
  bool rebuild_on_focus{false};
  std::vector<ExcerptRef> refs;
  std::vector<ExcerptBlock> blocks;
  std::vector<DroppedExcerpt> dropped;
  std::vector<std::string> header_index;
  std::vector<std::string> anchor_index;
  std::vector<std::string> capture_names;
  std::function<void(std::string_view line, std::vector<Interval>& out)> paint_line;
  bool refs_stale{false};
  std::vector<FileStamp> stamps;
};

struct ExcerptEpochs {
  std::vector<ExcerptView> store;
  std::vector<Index> boundaries;
  std::size_t active{0};
};

struct ScanJob {
  std::atomic<bool> stop{false};
  std::atomic<bool> done{false};
  // Set by the worker as it picks the job up, not by the thread that queued it.
  // The pool has more than one worker but fewer than the queue can hold, so a
  // scan can sit waiting behind others: counting that wait as time spent
  // running let the watchdog give up on a job that had never run, and made
  // ":from-cancel" report a queued job as one that had been working for 4s.
  // `begun_at` is steady_clock ticks, and is only meaningful once `begun`.
  std::atomic<bool> begun{false};
  std::atomic<std::int64_t> begun_at{0};
  std::vector<ExcerptRef> hits;
  std::vector<Symbol> hot;
  std::vector<Symbol> symbols;
  std::string error;
};

struct PendingCommand {
  std::string command;
  bool watched{false};
  bool with_msg{false};
  enum class Then : std::uint8_t { kOpen, kRebuild };
  Then then{Then::kOpen};
  ExcerptView::Kind kind{ExcerptView::Kind::kCommand};
  std::string view_name;
  int pid{-1};
  int fd{-1};
  std::string output;
  bool overflowed{false};
  std::shared_ptr<ScanJob> scan;
  bool done{false};
  int exit_status{0};
  std::chrono::steady_clock::time_point started{};
};

inline constexpr std::string_view kExcerptHeaderScope = "ui.excerpt.header";
inline constexpr std::string_view kExcerptMatchScope = "ui.excerpt.match";

// A name for a document that outlives nothing else it could be confused with.
// A position in ed.buffers is not one -- closing a buffer slides every later
// document down a slot -- and neither is table.revision, which is 1 for every
// freshly loaded file. Never reused, so an id held across a buffer being
// closed and another opened names the document it was taken from or nothing.
inline Index NextDocumentId() {
  static std::atomic<Index> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

struct Document {
  // Moving a document between slots carries its identity with it, which is
  // right: it is the same document. Copying one would hand two documents the
  // same name -- nothing in the editor copies a Document, and nothing should.
  Index id{NextDocumentId()};

  PieceTable table;
  SelectionSet selections;
  std::filesystem::path file;
  std::string view_name;
  bool modified{false};
  Index tab_width{4};
  bool insert_spaces{true};

  bool read_only{false};

  ExcerptView excerpts;
  ExcerptEpochs excerpt_epochs;

  FileStamp disk_stamp;

  Index saved_undo_serial{0};

  Viewport view;

  std::shared_ptr<Syntax> syntax;

  std::vector<Style> capture_styles;

  std::vector<Interval> search_cache;
  std::string search_cache_pattern;
  Index search_cache_revision{-1};
};

inline bool IsExcerptView(const Document& doc) { return !doc.view_name.empty(); }
inline bool HasDiskFile(const Document& doc) { return !doc.file.empty(); }
inline bool OwnsGrammar(const Document& doc) { return !IsExcerptView(doc); }

struct Rect {
  int x{0};
  int y{0};
  int w{0};
  int h{0};
  bool operator==(const Rect&) const = default;
};

inline constexpr int kMinPaneWidth = 4;
inline constexpr int kMinPaneHeight = 2;

struct WindowNode {
  enum class Kind : std::uint8_t { kLeaf, kRow, kColumn };
  Kind kind{Kind::kLeaf};

  std::size_t buffer{0};
  Viewport view{};
  SelectionSet selections{};
  Index selections_revision{0};

  int first{-1};
  int second{-1};

  double ratio{0.5};

  bool dead{false};
};

struct Target {
  std::filesystem::path path;
  Index line{0};
  Index column{0};
  bool has_line{false};
  bool has_column{false};
};

Target ParseTarget(std::string_view arg);

void GoToTarget(Document& doc, const Target& target);

ErrorCtx LoadDocument(const std::filesystem::path& path, Document& doc);

void DetectIndentation(Document& doc);

std::filesystem::path GlobalConfigPath();
std::filesystem::path LocalConfigPath();

std::vector<std::filesystem::path> ConfigPaths();

std::filesystem::path ConfigPath();

ErrorCtx SaveDocumentAs(Document& doc, const std::filesystem::path& path);

ErrorCtx AtomicWriteFile(const std::filesystem::path& path, std::string_view text);

bool ExternallyModified(const Document& doc);

// The mtime and size of `path` as one reading. False -- and `out` untouched --
// when the file cannot be stat()ed at all, which is the caller's cue that it
// has nothing to compare against later.
bool StampFile(const std::string& path, FileStamp& out);

std::filesystem::path CanonicalOf(const std::filesystem::path& path);

struct Settings {
  Index tab_width{4};
  bool insert_spaces{true};
  Index scrolloff{3};
  bool relative_line_numbers{true};

  bool cursorline{true};
  Index excerpt_context{2};

  bool render_tabs{true};
  std::string tab_glyph{"→"};
  std::string tab_pad{"·"};

  bool icons{true};
  std::string mode_indicator{"bar"};
  std::string mode_normal{"🐒"};
  std::string mode_insert{"🐑"};
  std::string icon_error{""};
  std::string icon_warning{""};
  std::string icon_info{""};
  std::string icon_readonly{""};
  std::string icon_modified{""};
  std::string icon_file{""};

  bool auto_pairs{true};
  // Pasting into N cursors text that is itself N lines: give each cursor one
  // line (true), or the whole text (false). VS Code spells this
  // editor.multiCursorPaste = "spread" | "full". This applies only to text koi
  // did not write -- its own multi-cursor copy always goes back the way it
  // came, whatever this says.
  bool multi_cursor_paste_spread{false};
  bool trim_trailing_whitespace_on_save{true};
  bool soft_wrap{false};
  std::string wrap_indicator{"↳ "};
  Index max_wrap{20};
  std::string theme{"ronin"};
  std::string file_filter;
  bool record{false};
  // Worker threads for the scan pool (searches, references, the file filter).
  // They are I/O bound over a file list, so this buys concurrency between
  // scans rather than throughput within one. The pool only ever grows while
  // koi runs -- see EnsureScanWorker -- so lowering it takes a restart.
  Index scan_workers{1};
};

struct WrapMetrics {
  bool enabled{false};
  Index width{80};
  Index indent{0};
  Index max_wrap{20};
  Index tab_width{4};
};

void LayoutLine(const PieceTable& table, Index line, const WrapMetrics& wrap,
                std::vector<Index>& row_starts, std::string& scratch);

Index WrappedRows(const PieceTable& table, Index line, const WrapMetrics& wrap);

Index RowOfPosition(const std::vector<Index>& row_starts, Index pos);

Index ColumnBetween(const PieceTable& table, Index from, Index pos, Index tab_width);

Index ByteForColumnFrom(const PieceTable& table, Index from, Index end, Index column,
                        Index tab_width);

Viewport ScrollToCursor(const Document& doc, Viewport view, const WrapMetrics& wrap = {});

enum class Mode { kNormal, kInsert };

enum class PendingChar {
  kNone,
  kFindNext,
  kTillNext,
  kFindPrev,
  kTillPrev,
  kSplitOn,
  kReplaceChar,
  kSurroundAdd,
  kSurroundDelete,
  kSurroundReplaceFrom,
  kSurroundReplaceTo,
  kTextObjectInner,
  kTextObjectAround,
  kLeapFirst,
  kLeapSecond,
  kLeapLabel,
};

// One target a leap is offering, and the key that takes it.
struct LeapLabel {
  Index at{0};
  char key{0};
};

// A leap-style two-character jump, from the first character typed until a
// target is picked or the mode is cancelled.
//
// Everything here is in document bytes rather than screen cells, so a scroll
// or a resize leaves it describing the same text and the renderer simply clips
// whatever has moved off the pane. What byte positions cannot survive is the
// text moving underneath them -- a finished background job swaps a buffer or
// rebuilds one's contents between two keystrokes, with no keystroke of its own
// to catch it on -- so `buffer` and `revision` say which document they were
// measured in, and both the overlay and the jump refuse to act when the answer
// has changed.
struct LeapState {
  enum class Stage : std::uint8_t { kOff, kSecond, kLabel };
  Stage stage{Stage::kOff};
  std::string first;
  // What the overlay tints: every visible occurrence of `first` while the
  // second character is awaited, and every visible pair once it has arrived.
  // Sorted, disjoint and merged where overlapping pairs ("aaa" searched with
  // "aa") would leave them touching -- the renderer binary-searches this once
  // per drawn grapheme, which overlapping ranges would quietly break.
  std::vector<Interval> spans;
  // Every visible pair, nearest to the caret first. That order is the label
  // assignment, and front() is what Enter takes.
  std::vector<Index> matches;
  // The labels on the screen right now, sorted by position for the renderer.
  std::vector<LeapLabel> labels;
  // Matches a capital label has picked so far: each becomes a caret when the
  // mode ends through enter or a lowercase label. Cleared with the rest of
  // the state on every exit, so esc drops them by dropping everything.
  std::vector<Index> picked;
  // Which group of `matches` those labels were cut from, when there are more
  // matches than there are label keys.
  std::size_t page{0};
  // What this stage of the mode is for, in words. It lives here rather than in
  // ed.status because ed.status is one slot shared with every other message in
  // the editor, and a background job's warning can take it between two
  // keystrokes -- leaving the labels on the screen with nothing saying what
  // they are. See LeapHint().
  std::string hint;
  // Which document the positions above were measured in, by identity rather
  // than by slot: see NextDocumentId.
  Index doc_id{-1};
  Index revision{-1};
};

enum class PromptKind {
  kCommand,
  kSearch,
  kSelectRegex,
  kSearchExcerpts,
};

// One line the re-indent-on-type trigger moved on its own, and the whitespace it
// moved it away from.
struct ReindentedLine {
  Index line{0};
  // What the line's leading whitespace was before the *first* adjustment of this
  // typing run, not before the last one: four keystrokes of `else` may move the
  // line four times, and what has to be restorable is where it started.
  std::string original;
  // What was left there. A line whose leading whitespace is no longer this is a
  // line something else has touched, and the memory of it is dead.
  std::string written;
};

// What the previous keystroke's re-indent did, kept only until the next one
// answers for it.
//
// `else` dedents its line and `elsewhere` does not, so the `w` that turns one
// into the other has to be able to put back what the `e` took away. That is the
// whole purpose: this is not a cache and nothing reads it for an answer.
//
// It cannot survive anything but one more keystroke of the same word. `document`
// is a Document::id, never reused, so a memory taken in one buffer is never
// mistaken for one in another; `revision` is the table revision the adjustment
// left behind, and every mutation of any kind moves it -- the next keystroke's
// own insertion included, which is why the check happens before it lands. An
// undo, a paste, a command, a jump to another file: all of them move one of the
// two. RunCommands clears it outright besides.
struct ReindentMemory {
  Index document{-1};
  Index revision{-1};
  std::vector<ReindentedLine> lines;
};

// What the indent engine last complained about, so that it complains once.
//
// Only one thing ever reaches here: an `indents.scm` that will not compile --
// the user's own file, named with the line it broke on. Everything else the
// engine can decline for is silent by construction (indent.h), because a
// message about a buffer too large to parse is not a message anybody can act
// on and it arrives on every Enter forever.
//
// A broken query, though, fails identically for every cursor of every Enter,
// and `ed.status` is one slot shared with every other message in the editor:
// said once it is news, said again on the next keystroke it is a buffer that
// has taken the status line over. `document` is a Document::id, never reused,
// so the same message in another buffer is that buffer's own news and is
// shown; an Enter that reports nothing clears the memory, so a failure that
// returns after things worked is news again too.
struct IndentWarning {
  Index document{-1};
  std::string message;
};

struct Editor {

  Document doc;

  std::vector<Document> buffers;
  std::size_t active{0};

  std::vector<WindowNode> windows;
  int focused{-1};

  int screen_w{80};
  int screen_h{24};

  Settings settings;
  Mode mode{Mode::kNormal};
  StatusMessage status;
  bool quit{false};

  std::vector<PendingCommand> pending_commands;

  std::vector<std::string> registers;

  // What we last handed the system clipboard, one entry per selection at the
  // time of the copy. A paste consults it so that copying with N cursors and
  // pasting with N cursors hands each cursor its own piece instead of the
  // joined text. Only trusted while the clipboard still holds exactly the bytes
  // we wrote -- anything else and we are looking at somebody else's copy.
  std::vector<std::string> clipboard_parts;

  PendingChar pending_char{PendingChar::kNone};
  bool pending_char_extend{false};
  std::string pending_char_arg;

  LeapState leap;

  ReindentMemory reindent;

  IndentWarning indent_warned;

  Index pending_count{0};

  bool collapse_insert_caret{false};

  bool align_view_once{false};

  bool status_overlay{false};

  bool prompt_active{false};
  PromptKind prompt_kind{PromptKind::kCommand};
  std::string prompt_input;
  std::size_t prompt_cursor{0};
  // Display column the prompt row starts at, for input wider than the row.
  int prompt_scroll{0};
  std::vector<std::string> prompt_history;
  std::vector<std::string> search_history;
  std::size_t prompt_history_index{0};

  std::vector<Selection> prompt_return_ranges;
  std::size_t prompt_return_primary{0};
  Viewport prompt_return_view;

  std::string search_pattern;

  bool search_highlight{false};

  bool reload_config{false};

  Theme theme;

  std::shared_ptr<JumpStore> jumps;

  std::shared_ptr<ProjectStore> project;

  std::shared_ptr<KeyRecorder> recorder;

  void (*suspend_terminal)(){nullptr};
  void (*resume_terminal)(){nullptr};

  void (*draw_now)(Editor&){nullptr};

  void (*live_document_changed)(Editor&){nullptr};
};

std::size_t WindowCount(const Editor& ed);

std::vector<int> WindowOrder(const Editor& ed);

bool BufferOnScreen(const Editor& ed, std::size_t buffer);

std::vector<Rect> LayoutWindows(const Editor& ed, Rect screen);

std::vector<Rect> LayoutNodes(const Editor& ed, Rect screen);

int SplitAt(int span, double ratio);

double SplitRatio(int span, int first);

void SplitWindow(Editor& ed, bool vertical);

void CloseWindow(Editor& ed);

void KeepOnlyFocusedWindow(Editor& ed);

void FocusWindow(Editor& ed, bool forward);

void FocusWindowAt(Editor& ed, int leaf);

enum class WindowDir : std::uint8_t { kLeft, kRight, kUp, kDown };

int WindowToward(const Editor& ed, WindowDir dir);

void JumpWindow(Editor& ed, WindowDir dir);

void SwapWindow(Editor& ed, WindowDir dir);

void TransposeWindow(Editor& ed);

enum class ResizeAxis : std::uint8_t { kWidth, kHeight };

enum class ResizeResult : std::uint8_t {
  kMoved,
  kNoNeighbour,
  kAtLimit,
};

ResizeResult ResizePane(Editor& ed, ResizeAxis axis, bool grow, Rect area);

int DividerAt(const Editor& ed, int x, int y, Rect area);

bool MoveDivider(Editor& ed, int node, int x, int y, Rect area);

Rect PaneArea(const Editor& ed);

std::size_t BufferCount(const Editor& ed);

const Document& BufferAt(const Editor& ed, std::size_t i);

std::size_t FindFileBuffer(const Editor& ed, const std::filesystem::path& path);

std::size_t FindViewBuffer(const Editor& ed, std::string_view name);

void SwitchToBuffer(Editor& ed, std::size_t index);

void AddBuffer(Editor& ed, Document doc);

void CloseActiveBuffer(Editor& ed);

std::vector<std::string> UnsavedBuffers(const Editor& ed);

bool AddCursorVertically(Editor& ed, bool below);

Index CountOr(const Editor& ed, Index fallback);

void ApplyModeInvariants(Editor& ed);

enum class StatusTone : std::uint8_t {
  kAccent,
  kStrong,
  kNormal,
  kDim,
  kError,
  kWarning,
  kInfo,
};

struct StatusSpan {
  std::string text;
  StatusTone tone{StatusTone::kNormal};
};

struct StatusLine {
  std::vector<StatusSpan> left;
  std::vector<StatusSpan> right;
  static constexpr std::size_t kNoMessage = static_cast<std::size_t>(-1);
  std::size_t message_from{kNoMessage};
};

StatusLine StatusBar(const Editor& ed, bool focused = true);

StatusLine StatusBar(const Editor& ed, const Document& doc, const SelectionSet& sel,
                     std::size_t buffer, bool focused);

void SyncWindowSelections(const PieceTable& table, WindowNode& node);

void RetargetPane(WindowNode& node, std::size_t buffer, const PieceTable& table);

std::string DisplayPath(const std::filesystem::path& path);

std::string DisplayName(const Document& doc);

constexpr std::string_view TrimFront(std::string_view s, std::string_view chars) {
  const std::size_t at = s.find_first_not_of(chars);
  return (at == std::string_view::npos) ? std::string_view{} : s.substr(at);
}

constexpr std::string_view TrimBack(std::string_view s, std::string_view chars) {
  const std::size_t at = s.find_last_not_of(chars);
  return (at == std::string_view::npos) ? std::string_view{} : s.substr(0, at + 1);
}

constexpr std::string_view Trim(std::string_view s, std::string_view chars) {
  return TrimBack(TrimFront(s, chars), chars);
}

template <typename PositionOf>
void SetCursors(Editor& ed, PositionOf at, bool keep_goal = false) {
  auto ranges = ed.doc.selections.Ranges();
  for (Selection& s : ranges) {
    const Index pos = at(s);
    s.anchor = pos;
    s.head = pos;
    if (!keep_goal) s.goal_column = -1;
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
}

void PromptOpen(Editor& ed, PromptKind kind = PromptKind::kCommand);
void PromptCancel(Editor& ed);

void PromptRestoreSearchOrigin(Editor& ed);
std::string_view PromptSigil(const Editor& ed);
std::vector<std::string>& PromptHistoryOf(Editor& ed);
void PromptInsert(Editor& ed, std::string_view text);
void PromptBackspace(Editor& ed);
void PromptDeleteForward(Editor& ed);
void PromptMoveLeft(Editor& ed);
void PromptMoveRight(Editor& ed);
void PromptHome(Editor& ed);
void PromptEnd(Editor& ed);
void PromptHistory(Editor& ed, bool back);

}

#endif
