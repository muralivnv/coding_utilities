#ifndef KOI_PROJECT_H_
#define KOI_PROJECT_H_

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "piece_doc.h"
#include "symbols.h"

// The connection, not sqlite3.h: the one caller that needs it is the jump list,
// which reads and writes `locations` through the store's own handle rather than
// opening a second one onto the same file.
struct sqlite3;

namespace koi {

inline constexpr int kPinSlots = 4;
inline constexpr int kHotSymbolSlots = 7;

struct FileVisit {
  std::string path;
  Index line{1};
  Index column{0};
  double last_ts{0};
  // What the row scored, for the reads that rank by frecency; 0 from the ones
  // that do not. It is here because the file picker has a term of its own to
  // multiply in (BranchDiffFiles), and a position in a sorted list is not a
  // thing a multiplier can be applied to.
  double score{0};
};

struct SymbolVisit {
  std::string symbol;
  std::string file;
  Index line{0};
};

// A pinned file, and where you last were in it. Only the path is stored: the
// line and column are read back out of `files` on every Pins() call, so a pin
// follows you around the file instead of naming a line number that nothing
// updates and every edit above invalidates.
struct Pin {
  std::string path;
  Index line{1};
  Index column{0};
};

// One place visited, as the recorder saw it. Everything past `line`/`col` is
// what makes the row findable again after the file moves under it (the healing
// ladder in docs/smart-jump.md); a recorder with no live syntax, or one
// stepping back into a view, leaves those empty and the row keeps whatever it
// already held.
struct LocationRecord {
  // A path valid from the current directory, or an excerpt view's name. Keyed
  // on the way in, exactly once -- callers hand over what they have.
  std::string path;
  Index line{1};
  Index col{0};
  // 0 visit, 1 edit. An edit never degrades to a visit on merge: a place you
  // have edited stays one.
  int kind{0};

  // Jump motions set this: merge only onto a row at this exact line. The wide
  // rule (same symbol, +/-10 lines) is right for the corpus and wrong for the
  // list -- a departure record that merges onto the row it is jumping from
  // makes jump_backward a no-op for any jump shorter than the window.
  bool exact{false};

  // Empty means "not resolved here", not "there is none": a merge keeps the
  // symbol the row already carries rather than clearing it.
  std::string symbol;

  // Whether this record had the buffer in hand at all. False for a jump into a
  // view, or any record made without the text -- and then `content`, `context`,
  // `blob` and `uniq` say nothing, and a merge leaves all four alone.
  bool has_text{false};
  std::string content;
  std::string context;
  // git's blob id of the file *on disk*, empty when the buffer is dirty against
  // it or there is no file. Null in the row means the first two rungs of the
  // resolve ladder cannot run for it.
  std::string blob;
  // How many lines of the buffer normalise to `content`. 0 is "unknown".
  std::int64_t uniq{0};
};

// A record merges into an existing row for the same path when the two share an
// enclosing symbol, or when they are within this many lines of each other.
// IntelliJ merges nearby same-file places the same way; the number is the
// design's (docs/smart-jump.md, Recording).
inline constexpr Index kLocationMergeLines = 10;

// How long a row's visit count is held still after a touch. Without it an edit
// boundary per keystroke would make `visits` a count of keys pressed, and
// arriving twice at the place you are already standing in would count twice.
inline constexpr double kLocationVisitDebounce = 30.0;

// One `locations` row as healing reads it: everything the resolve ladder is
// allowed to look at, and nothing else. The ranking columns are deliberately
// absent -- a heal must not be able to move a row's standing.
struct AnchorRow {
  std::int64_t id{0};
  std::int64_t seq{0};
  Index line{1};
  std::string content;
  std::string context;
  std::string blob;
  // Consecutive heals that could not find this row. Read so the blob gate can
  // clear one it does not otherwise write: a file restored to what the row was
  // recorded against is a hit, and a hit resets the counter.
  std::int64_t misses{0};
  std::string symbol;
  // Null in the row rather than empty text. Only a null one is refilled: a name
  // the recorder wrote is the recorder's, and a fresh parse does not overrule
  // it.
  bool symbol_null{true};
  // Not a column. Set by the heal trigger when `line` was replaced with an open
  // buffer's live position, so `line` and `blob` then describe two different
  // states of the file and the ladder must not mix them.
  bool live_line{false};
};

// What healing decided about one row, and the only shape a heal may be written
// back in. There is no "delete" here on purpose: a miss counts and the row
// stays (docs/smart-jump.md, "Never delete on a miss").
struct AnchorHeal {
  std::int64_t id{0};
  // The `seq` the row had when the job took its snapshot. Every write-back is
  // conditional on it: a job can be minutes old -- a file read, a `git cat-file`
  // and a parse behind other pool work -- and a row the recorder has touched
  // since has a newer and truer line than anything computed from that snapshot.
  // Such a row is left exactly as it is, which is the rule the shadow's
  // AdoptAnchorRows already applies.
  std::int64_t seq{0};
  // 1-based, and meaningless when `miss`.
  Index line{0};
  int rung{8};
  bool miss{false};
  // Whether the match was close enough and unambiguous enough to rewrite what
  // the row says about itself. False leaves `content` and `context` exactly as
  // they were, which is what keeps a run of small repairs from walking the
  // anchor onto a line nobody visited.
  bool refresh_text{false};
  std::string content;
  std::string context;
  // The file's blob id, written on any hit: the row now describes *this* state
  // of the file, so this is the state its blob should name. Left alone on a
  // miss, or the next heal's blob gate would call the row true of a file it was
  // never found in and stop looking.
  std::string blob;
  bool set_symbol{false};
  std::string symbol;
};

struct ProjectStore {
  // Null means there is no store and `error` says why. Non-null with a
  // NON-EMPTY `error` is a warning, not a failure: the database on disk was
  // corrupt, has been renamed to `<db>.corrupt`, and this store is a fresh
  // empty one. Callers that only branch on null keep working; callers with a
  // status line should show it.
  static std::shared_ptr<ProjectStore> Open(const std::filesystem::path& db, std::string& error);

  virtual ~ProjectStore() = default;

  virtual void RecordVisit(std::string_view path, Index line, Index column) = 0;

  virtual void RecordEdit(std::string_view path, Index line, Index column) = 0;

  virtual void RecordSymbolVisit(std::string_view symbol, std::string_view file, Index line) = 0;

  virtual void RecordCoVisit(std::string_view from_file, std::string_view to_file) = 0;

  // The one writer of `locations`: boundary recording and the jump list both
  // come through here, so the merge rule has one implementation and the two
  // cannot disagree about what "the same place" is.
  //
  // Merge or insert, and the answer is the row's `seq` afterwards -- which is
  // what the jump list moves its cursor onto -- or 0 when nothing was written
  // (an unstorable path, a statement that would not run). A row already at the
  // front of the list keeps its seq: it is already where a merge would move it,
  // and taking a new one per keystroke would run the counter for nothing.
  //
  // Runs inside the caller's transaction when one is open, and opens its own
  // BEGIN IMMEDIATE when none is -- so a jump, which is a write plus a cursor
  // move, stays one transaction, and a boundary record is one too.
  virtual std::int64_t WriteLocation(const LocationRecord& row) = 0;

  // The `want` most recently visited files, newest first. `want <= 0` asks for
  // every row -- which is what this used to do unconditionally, and what made a
  // read of the newest handful a full table scan plus one stat() per row. A
  // caller that shows n rows asks for n.
  //
  // A row whose file is no longer on disk is skipped, so the answer can be
  // shorter than `want` even when the table is longer.
  virtual std::vector<FileVisit> RecentFiles(int want) = 0;

  virtual bool LastVisit(std::string_view path, Index& line, Index& column) = 0;

  // The `want` files with the highest frecency (visits and edits, decayed by
  // how long ago the file was last touched). Same `want` rules as RecentFiles.
  //
  // The one caller in the editor passes 0 on purpose: the file picker ranks
  // every path the file filter produced, so a cap there would not shorten a
  // list, it would silently drop files out of frecency order and back into
  // find(1) order.
  virtual std::vector<FileVisit> FrecentFiles(int want) = 0;

  virtual std::vector<SymbolVisit> HotSymbols(int limit) = 0;

  virtual size_t RankSymbols(std::vector<Symbol>& rows, std::string_view current_file) = 0;

  virtual std::vector<std::string> HotFiles(int limit, std::string_view current_file) = 0;

  // Every slot in slot order, with an empty path for each one that is unset, so
  // the result is always kPinSlots long and indexable by slot - 1.
  virtual std::vector<Pin> Pins() = 0;

  // Pins the file, not a position in it. Pinning a file that already holds
  // another slot moves it rather than letting it occupy two.
  virtual void SetPin(int slot, std::string_view file) = 0;

  virtual void ClearPin(int slot) = 0;

  virtual int FileCount() = 0;

  // Every location recorded in `path`, oldest row first. The healer's read: it
  // takes the whole file's worth at once, because the job it feeds is one file
  // read and one hash pass shared by all of them.
  virtual std::vector<AnchorRow> AnchorsFor(std::string_view path) = 0;

  // Where a file's rows sit and what a record last did to them: (id, line, seq)
  // for every row of `path`. The live-shift shadow's read, and it is separate
  // from AnchorsFor because it runs at every jump and wants three integers, not
  // five strings.
  virtual std::vector<AnchorRow> AnchorPositionsFor(std::string_view path) = 0;

  // Writes a heal pass back, all of it in one BEGIN IMMEDIATE.
  //
  // Deliberately narrow: `line`, the miss counter, the blob, and -- only where
  // the heal said so -- the text columns and a null symbol. `last_ts`, `seq`,
  // `visits`, `counted_ts` and `kind` are never touched, so healing a file
  // cannot reorder the jump list or move a row's frecency.
  //
  // Each write is also keyed on the `seq` the heal carries, so a row the
  // recorder has written since the job started is skipped rather than reverted.
  // False only when the transaction itself failed; a batch that was entirely
  // skipped still committed and is still true.
  virtual bool ApplyHeals(const std::vector<AnchorHeal>& heals) = 0;

  // Firefox's adaptive input history, and the one thing in this design that
  // learns. On every accepted jump, every progressive prefix of what was typed
  // is credited with the target: typing "key cpp" and landing on keymap.cpp
  // teaches both `key` -> keymap.cpp and `key cpp` -> keymap.cpp, so the shorter
  // query gets faster the next time too.
  //
  // `use_count = use_count * 0.9 + 1`, which climbs towards an asymptote of 10 --
  // a pair confirmed a hundred times cannot run away from one confirmed ten.
  // One BEGIN IMMEDIATE for the whole set of prefixes.
  //
  // `target` is stored exactly as it is handed over: a target is not always a
  // path (a symbol, a location), so keying one here would key the wrong things.
  // Callers hand over what the ranking will look the target up by.
  virtual void RecordQueryAccept(std::string_view typed_terms, std::string_view target) = 0;

  // What that pair is worth now: the stored count decayed by 0.975 per day since
  // it was last confirmed. The decay is applied on read and never written back --
  // there is no background job, and a row nobody asks about costs nothing until
  // the next open prunes it.
  virtual double AdaptiveUse(std::string_view prefix, std::string_view target) = 0;

  // The store's own connection. The jump list lives in `locations` in this
  // database now, and two connections onto one file are two write locks where
  // the design says one.
  virtual sqlite3* Connection() = 0;
};

// The `queries` numbers, in one place (docs/smart-jump.md, Scoring).
inline constexpr double kQueryAcceptDecay = 0.9;
inline constexpr double kQueryDecayPerDay = 0.975;
inline constexpr double kQueryDropBelow = 0.1;

// The prefixes RecordQueryAccept writes, shortest first: one per progressive
// prefix of the whitespace-separated term list, each spelled as its terms joined
// by single spaces. "key  cpp" gives {"key", "key cpp"}.
//
// Exported because a lookup has to build the same string a write did, and two
// spellings of "the terms typed so far" would mean the table never hits.
std::vector<std::string> QueryPrefixes(std::string_view typed_terms);

inline constexpr int kDefaultHotFileLimit = 200;

// The project-root rule itself: walk up from `from` looking for a `.git` or
// `.ronin` marker, never going above `stop` (koi's $HOME), and fall back to
// `from` when there is no marker to find. An empty `stop` means "walk to the
// filesystem root".
//
// Exposed because ProjectRoot() answers from a process-lifetime cache -- the
// walk runs once, on the first call, and nothing can make it run again -- so
// this is the only door the rule can be tested through.
std::filesystem::path FindProjectRoot(const std::filesystem::path& from,
                                      const std::filesystem::path& stop);

std::filesystem::path ProjectRoot();

// A path read back out of the project database, turned into a path valid from
// koi's current directory -- which is what everything that opens, stats or
// compares one expects. Stored paths are keyed against the project root, so the
// two agree only when koi was started there.
//
// The root overload is for a loop over rows: deriving it once is the same
// saving the row reads inside the store make. Apply this exactly once, where
// store data leaves the store; a relative result resolves against the current
// directory, so putting one back through would move it again.
std::string ResolveStorePath(const std::filesystem::path& root, std::string_view key);
std::string ResolveStorePath(std::string_view key);

// The one spelling every path in this database is stored under: relative to the
// project root, or absolute when the file lies outside it. The rule is written
// out at the definition; the short form is that what goes in is a path valid
// from the current directory, and it is keyed exactly once.
std::string ProjectKey(std::string_view path);

// Whether a key is worth storing. Measured against the live store, ~70% of
// `files` was not: /tmp scratch and prompt files, `.git/COMMIT_EDITMSG`, and
// `.claude/worktrees/agent-*/...` copies of paths the repository already holds.
// A key that fails this is never written, and the next open deletes the rows
// that hold one.
//
// The file filter's own exclusions are not reused. They live inside a shell
// command the user can rewrite -- the built-in default and the shipped config
// already spell them two different ways -- so there is no set to honour, only a
// command to guess at. This list is fixed instead, and short.
bool StorablePath(std::string_view key);

// Whether a symbol name is worth storing. The old recorder wrote whole call
// expressions into `symbols` -- `Foo(bar[i], baz)` -- and a name like that is
// junk twice over: it is not a definition, and four scattered letters of it
// fuzzy-match anything. Identifiers, qualified names and operators pass;
// anything with argument punctuation in it does not.
bool StorableSymbolName(std::string_view name);

// The key a *location* is stored under. Almost ProjectKey, and the difference
// is the excerpt views: "references: split" is not a path, nothing on disk
// answers to it, and the step back into one finds its buffer by this exact
// string -- so a relative name with no file behind it is stored as it is.
std::string LocationKey(std::string_view path);

// The branch `root` is on: the name from `ref: refs/heads/<name>`, the first
// twelve characters of the object id for a detached HEAD, and empty when there
// is no repository to ask. It reads `.git/HEAD` itself -- a worktree's `.git`
// is a file naming the real directory, and that is followed one level -- so it
// costs a stat and no subprocess. Memoised on the HEAD file's timestamp.
std::string GitBranch(const std::filesystem::path& root);

// The files changed on the current branch -- `git diff --name-only` against the
// merge base with whichever of origin/HEAD, main or master resolves -- as
// project keys, so they compare against what the store holds.
//
// Empty, silently, when there is no repository, no git binary, or the commands
// fail: no repository is a supported shape, not a failure.
//
// This one does shell out, so it is the opposite of GitBranch in what it may be
// called from: never per keystroke and never per record, only lazily from
// ranking when a picker opens. Cached on the branch and HEAD's timestamp, so it
// runs at most once per branch switch.
const std::vector<std::string>& BranchDiffFiles();

// The next value of the store-wide `locations.seq` counter, taken inside the
// caller's transaction. Every touch of a location row takes one, and the order
// they hand back is the jump list's order. Zero means the counter could not be
// moved, and a caller that gets one must write no row and roll back.
std::int64_t NextStoreSeq(sqlite3* db);

std::filesystem::path ProjectDbPath();

// Where the jump list lived before v4 folded it into `locations`:
// $HOME/.local/share/koi/<project>/state.db. The v3 -> v4 migration reads it
// and leaves it on disk as the backup; nothing else opens it any more.
std::filesystem::path LegacyJumpDbPath();

void SetProjectDbPath(std::filesystem::path path);

void SetProjectRoot(std::filesystem::path path);

std::string FlattenPathComponent(std::string_view path);

std::string ProjectDirName(const std::filesystem::path& project);

std::filesystem::path LastPickerStatePath();

std::filesystem::path KeyLogDbPath();

}

#endif
