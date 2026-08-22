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
struct SmartJumpState;
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

// A document's `locations` rows and where the edits since the last heal have
// moved them. Lines are 1-based, like the store's.
//
// Lazy on purpose. Nothing here updates per edit: it carries a revision and
// catches up through the journal when read, exactly like the stashed window
// selections beside it (SyncWindowSelections). Defined in anchor.h terms by
// SyncAnchorShadow; it lives on Document because it is per buffer and dies with
// it.
struct AnchorShadow {
  struct Row {
    std::int64_t id{0};
    Index line{1};
    // The store's `seq` when the row was adopted, which is what tells "the
    // recorder moved this row" from "the line I am holding is the newer one".
    std::int64_t seq{0};
    // An edit landed on this anchor's own line. It is not shifted and it is not
    // clamped to a neighbour -- it is re-resolved by content at the next heal.
    bool dirty{false};
  };
  bool valid{false};
  Index revision{-1};
  std::vector<Row> rows;
};

struct AnchorJob;

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

  // git's blob id of the file on disk, taken at load and re-taken at save --
  // the two moments the bytes are already in hand. Empty when there is no file
  // behind the buffer or it was too big to be worth hashing (kMaxBlobBytes).
  //
  // Stale the moment the buffer is modified or somebody else writes the file,
  // which is exactly why a recorder only uses it on a clean buffer: it names
  // the disk state a stored location was recorded against, not this text.
  std::string disk_blob;

  Index saved_undo_serial{0};

  Viewport view;

  std::shared_ptr<Syntax> syntax;

  std::vector<Style> capture_styles;

  std::vector<Interval> search_cache;
  std::string search_cache_pattern;
  Index search_cache_revision{-1};

  // Where this buffer's stored locations have drifted to since the store last
  // saw them. Read and moved through anchor.h.
  AnchorShadow anchors;
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

// `wrote`, when given, takes the bytes that went to the file -- the same string
// the write used, handed over rather than built a second time by the caller.
ErrorCtx SaveDocumentAs(Document& doc, const std::filesystem::path& path,
                        std::string* wrote = nullptr);

ErrorCtx AtomicWriteFile(const std::filesystem::path& path, std::string_view text);

bool ExternallyModified(const Document& doc);

// The mtime and size of `path` as one reading. False -- and `out` untouched --
// when the file cannot be stat()ed at all, which is the caller's cue that it
// has nothing to compare against later.
bool StampFile(const std::string& path, FileStamp& out);

// Past this many bytes a document gets no blob id. Hashing is linear and about
// as fast as reading, so the cap is not about SHA-1: it is that a file this
// size is one koi already takes a visible moment to open, and a location in it
// heals by content just as well without the O(1) first rung.
inline constexpr std::size_t kMaxBlobBytes = 4u * 1024u * 1024u;

// Past this many bytes a file is out of scope for the line-level work anchors
// are built on: the recorder leaves `uniq` at 0 rather than pausing the input
// loop for a census, and a heal declines the file rather than splitting it and
// indexing it on a pool thread (a 97 MB file measured at 330 MB of peak memory
// and two seconds, for every save and every focus-in). One judgement, so one
// number: a generated file this size is not worth the pass either way.
inline constexpr Index kMaxUniqBytes = 2 * 1024 * 1024;

// git's name for these bytes as a blob, or empty when there are too many of
// them. The two callers are the two moments a document's disk text is already
// in hand -- load and save -- and nothing else may hash a buffer: the record
// path copies what they left on the Document.
std::string BlobOidOf(std::string_view text);

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
  // Whether a lone smart-jump match jumps on its own once the prompt is quiet.
  bool smart_jump_auto{true};
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
  kSmartJump,
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

// What the boundary recorder remembers between events. It is here rather than
// in a static inside navigate.cpp because it is per editor and because a test
// has to be able to age it: the linger rule is "three seconds at one place",
// and nothing in the suite should sleep for three seconds to reach it.
//
// Times are wall clock seconds, the same scale the store's own timestamps use.
struct BoundaryRecorder {
  // Where the cursor is sitting, and since when. `recorded` says a row already
  // covers this stay, so the linger fires once and not once per event.
  Index document{0};
  Index line{-1};
  double since{0};
  bool recorded{false};
  // The document's revision at the last boundary. An edit is a boundary where
  // this moved.
  Index revision{-1};

  // The last row written, and the last `files` bump: what keeps a burst of
  // typing at one place out of the database after the first record of it. The
  // store debounces its own counters (kLocationVisitDebounce); this is what
  // stops the place being described at all. Held as a document id rather than a
  // path because it is the same identity and it costs no string.
  Index wrote_document{-1};
  Index wrote_line{-1};
  int wrote_kind{-1};
  double wrote_at{0};
  double files_at{0};

  // Whether the store will take this file's path, and the path that answer was
  // reached from. It takes a canonicalisation to decide, so it is decided once
  // per file rather than once per boundary; the string is compared and never
  // rebuilt, so a rename under the same buffer is still noticed.
  std::string storable_path;
  bool storable{false};

  // The document and enclosing symbol of the last record that resolved one, so
  // that staying inside a function does not count as visiting it again -- and
  // the line the tree was last asked about. The cursor has to leave a line
  // before it can be inside a different function, so a boundary that has not
  // left it already knows the answer.
  Index symbol_document{0};
  std::string symbol;
  Index note_document{-1};
  Index note_line{-1};

  // A smart-jump arrival, held back until it has earned a row: two seconds
  // standing in it, or the first edit there. Leaving sooner cancels it and
  // nothing is written -- a mis-jump that records itself is the failure zoxide
  // documents as the trust-killer, and this corpus is only worth having while
  // it is honest about where its owner actually went.
  //
  // Here rather than in a machine of its own because it is the same question
  // this struct already answers -- where the cursor is and since when -- asked
  // with a different clock, and two machines would have to agree about what
  // "still there" means.
  bool pending{false};
  Index pending_document{0};
  Index pending_line{-1};
  Index pending_revision{-1};
  double pending_since{0};
  // The query credit rides the same confirmation as the row: a mis-jump that
  // is bounced must teach `queries` nothing, so the accept is held here until
  // the arrival is stood in. Empty for a step, which never credits.
  std::string pending_typed;
  std::string pending_target;
  // The prompt text an auto-fired jump fired on, and empty for one submitted by
  // hand. Reopening the prompt while such an arrival is still unconfirmed types
  // it back in: the fire is a guess the user never pressed a key for, so taking
  // it back has to cost nothing.
  std::string pending_query;
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

  // Heal jobs in flight, one at a time per file. Drained by PumpAnchorHeals
  // (anchor.h), which the same pump that drains the scans calls.
  std::vector<std::shared_ptr<AnchorJob>> anchor_jobs;

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
  std::vector<std::string> jump_history;
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

  // The corpus snapshot the open prompt is scoring against, and the last
  // query's ranked list. It outlives the prompt: smart_jump_next steps through
  // that list long after the prompt has closed. Held by pointer so editor.h
  // does not have to know what is in it.
  std::shared_ptr<SmartJumpState> smart_jump;

  BoundaryRecorder record;

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
