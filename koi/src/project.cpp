#include "project.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "sqlite.h"
#include "subprocess.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

// A multiplier on the weight, not a decay of it. `visits + 3*edits`
// accumulates and never decays by the clock; how long ago the row was last
// touched only multiplies what it has already earned (docs/smart-jump.md,
// Frecency). A wall-clock half-life instead would cut a month-old row by a
// thousand -- the rows most worth protecting -- and what does take weight away
// is the event-driven aging below.
//
// ?1 is now. The timestamp column is an argument so one expression serves a
// query that has joined `symbols` to `files` as well as one over `files`.
#define KOI_MULT_SQL(ts)                        \
  "CASE"                                        \
  " WHEN (?1 - " ts ") / 3600.0 < 1   THEN 4.0" \
  " WHEN (?1 - " ts ") / 3600.0 < 24  THEN 2.0" \
  " WHEN (?1 - " ts ") / 3600.0 < 168 THEN 1.0" \
  " WHEN (?1 - " ts ") / 3600.0 < 720 THEN 0.5" \
  " ELSE 0.25 END"

// Branch is a bonus, never a filter: a row from another branch is still a place
// you have been, and a WHERE would rebuild the mode problem inside the ranking.
//
// ?3 is the branch koi is on, or NULL when there is no repository to ask -- and
// `branch = NULL` is NULL rather than true, so the CASE never fires and a
// project without git ranks exactly as it did before this existed. Same for a
// row stamped before there was a branch to stamp it with.
#define KOI_BRANCH_SQL(branch) "CASE WHEN " branch " = ?3 THEN 1.25 ELSE 1.0 END"

constexpr int kMaxCoVisitFiles = 32;

// How many stored rows a bounded read may look at for every row it returns.
//
// The over-fetch is for StillThere: a row whose file has been deleted or
// renamed is dropped in C++, and the query has no way to know that, so it hands
// back more than the caller asked for and the loop stops as soon as it has
// enough. Four-to-one with a floor of 256 fills a twenty-row read in one query
// even on a store where three quarters of the history is gone from disk.
//
// Over-fetching is very nearly free: the rows arrive in index order and the
// ones past the cap are never turned into a FileVisit, never copied into a
// std::string, and never stat()ed.
constexpr int kVisitOverFetch = 4;
constexpr int kVisitScanFloor = 256;

// The newest visits kept in `files` at open. Nothing else ever deleted from
// this table -- every file ever opened stayed in it forever, and every read
// scanned and stat()ed all of it -- so an editor left running against a big
// repository grew a permanent tax on the file picker.
//
// 5000 is far past what anything here distinguishes: a picker ranking that
// reaches 5000 visited files is ordering rows the user last touched 5000 other
// files ago. It is deliberately generous, because the cost of keeping a row is
// one row and the cost of dropping one that mattered is a lost cursor position.
#define KOI_MAX_FILE_ROWS "5000"

// One statement, and exact: "everything but the newest N by last visit". The
// obvious cheaper spelling -- delete everything below the Nth row's timestamp
// -- gets ties wrong in both directions, and this runs once per open.
constexpr const char* kPruneFiles =
    "DELETE FROM files WHERE path NOT IN"
    " (SELECT path FROM files ORDER BY last_ts DESC LIMIT " KOI_MAX_FILE_ROWS ");";

// The same bound for `symbols`, which was never pruned either and grows faster
// than `files`: one row per file *and* symbol jumped to, not one per file. It is
// the table HotSymbols, HotFiles and RankSymbols read, and the only thing that
// ever limited how much of it they had to look at was how much the user had
// done.
//
// Four times the file cap because a row here is a quarter of the story a file
// row is -- a jump target inside a file rather than the file -- and because
// nothing distinguishes anything near it: a hot-symbol jump reaches seven, and a
// ranking that reaches the 20000th least recently visited symbol is ordering
// rows against history the user cannot remember making.
#define KOI_MAX_SYMBOL_ROWS "20000"

// `rowid`, not the primary key: `symbols` is keyed by (file, symbol), and "NOT
// IN a list of pairs" has no spelling as simple as the one `files` gets. Every
// row of a rowid table has one, and it identifies the row exactly, which is all
// the NOT IN needs.
//
// The leading count is a guard, not decoration. Nothing indexes `symbols.last_ts`
// -- and nothing should; see the note on HotSymbols -- so the subquery sorts the
// whole table, which at the cap measured 23 ms per open to delete nothing at all.
// The count is a constant subquery, so SQLite evaluates it once and never builds
// the list when it is false: 0.9 ms in the steady state, and the sort is paid
// only by the opens that actually have rows to drop.
constexpr const char* kPruneSymbols =
    "DELETE FROM symbols WHERE (SELECT COUNT(*) FROM symbols) > " KOI_MAX_SYMBOL_ROWS
    " AND rowid NOT IN"
    " (SELECT rowid FROM symbols ORDER BY last_ts DESC LIMIT " KOI_MAX_SYMBOL_ROWS ");";

// The jump list's own cap, carried over from the database it used to live in
// (kKeepRows there). It bounds `locations` the same way the two above bound
// their tables, and by `seq` rather than by timestamp: seq is the order the
// jump list steps through, so "the newest 2000" and "the 2000 the user can
// still step back to" are the same set.
//
// Rows are never dropped for naming a file that is missing right now. A branch
// switch makes half a repository transiently missing, and a history that
// deleted itself over one would be worth nothing on the way back.
#define KOI_MAX_LOCATION_ROWS "2000"

constexpr const char* kPruneLocations =
    "DELETE FROM locations WHERE (SELECT COUNT(*) FROM locations) > " KOI_MAX_LOCATION_ROWS
    " AND id NOT IN"
    " (SELECT id FROM locations ORDER BY seq DESC LIMIT " KOI_MAX_LOCATION_ROWS ");";

// `co_visits` is deliberately not pruned, and that is not an oversight.
//
// Both of its readers -- HotFiles and RankSymbols -- ask for one `from_file`,
// which is the primary key's first column, so a read seeks straight to that
// file's rows and the size of the table never enters the cost. Table growth is
// one row per distinct (file you were in, file you jumped to) pair, created only
// by an explicit symbol jump.
//
// And there is nothing honest to prune *by*: the table has no timestamp, so
// "the newest N" cannot be asked. The tempting proxy -- drop rows whose
// `from_file` is no longer in the pruned `files` -- deletes on a signal that is
// not the one it claims to be, and the one thing that must never happen to this
// table is losing a pair the user made this session.

// The one thing that ever takes weight away, and it is driven by the work done
// rather than by the calendar: when a table gets heavy, every weight in that
// table is scaled down at once. A two-week gap therefore costs a row nothing --
// the clock only ticks while koi is being used -- and an old-but-heavy row
// keeps its standing against shallow new visits.
//
// It is also the age-out. Nothing else expires a row by frecency: an unvisited
// one drifts under the floor over enough aging passes and is deleted here.
constexpr double kAgeThreshold = 10000.0;

// The deletion floor, and it has to sit strictly below the weight a row is born
// at (1 visit) or a pass deletes every row that has only ever been visited once
// -- which is most of `locations` and `symbols`. With the scale below, a row
// nothing touches again survives six passes and goes on the seventh.
#define KOI_AGE_FLOOR "0.5"

// The most one pass may take off. Enough of the threshold/total factor is kept
// to stop a pass overshooting far under the gate, but never enough to put an
// entry-weight row through the floor: `symbols` holds 20000 rows and half of
// them can be at one visit, so total/threshold alone is a factor of 0.5 and
// would empty the table in a single open.
constexpr double kAgeScale = 0.9;

// Each table is gated and scaled on its own weight, never on another's. `files`
// carries every visit since schema v1 and is past the threshold on any real
// store; gating `locations` and `symbols` on it scaled two tables that were
// nowhere near heavy, and their single-visit rows fell straight through the
// floor.
//
// The counts stay fractional, and that is the choice rather than an oversight.
// `visits` and `edits` have INTEGER affinity, which stores the REAL that
// `visits * factor` produces as it is, and the two whole-number spellings are
// both wrong: rounded, a row at 1 visit stays at 1 for ever and can never age
// out, which is the whole mechanism; truncated, every single-visit row dies on
// the first pass however recently it was visited.
struct AgeTable {
  const char* total;  // the table's own weight
  const char* scale;  // ?1 is the factor
  const char* cull;   // may be more than one statement
};

constexpr AgeTable kAgeTables[] = {
    {"SELECT COALESCE(SUM(visits + edits * 3), 0) FROM files;",
     "UPDATE files SET visits = visits * ?1, edits = edits * ?1;",
     "DELETE FROM files WHERE visits + edits * 3 < " KOI_AGE_FLOOR ";"},
    {"SELECT COALESCE(SUM(visits), 0) FROM symbols;",
     "UPDATE symbols SET visits = visits * ?1;",
     "DELETE FROM symbols WHERE visits < " KOI_AGE_FLOOR ";"},
    {"SELECT COALESCE(SUM(visits), 0) FROM locations;",
     "UPDATE locations SET visits = visits * ?1;",
     // A row a pane's jump cursor sits on ages like every other row and stops at
     // the floor that would delete it. Losing one does not cost history, it
     // moves the user's place in the list -- the next step back would start from
     // somewhere they never were.
     //
     // Matched on `id`, which is what the cursor holds since v6. Matching on
     // `seq` protected nobody: a merge gives the row a new seq, and only the
     // merging pane's cursor moved with it.
     "UPDATE locations SET visits = " KOI_AGE_FLOOR
     " WHERE visits < " KOI_AGE_FLOOR " AND id IN (SELECT at FROM jump_cursor);"
     "DELETE FROM locations WHERE visits < " KOI_AGE_FLOOR ";"},
};

// v1: paths were stored in whatever spelling the writer happened to have.
// v2: every path is a ProjectKey -- relative to the project root, or absolute
//     when the file lies outside it.
// v3: a pin is a file, not a position in one. `pins` is gone; `file_pins` holds
//     the slot and the path, and the position comes from `files`.
// v4: one database. The jump list was a second one under ~/.local/share/koi
//     with its own path spelling; its `jumps` are `locations` here, keyed like
//     everything else. `queries` comes with it so the stamp moves once.
// v5: `locations.counted_ts`. The visit debounce used to be measured against
//     `last_ts`, which every touch refreshes, so it was a sliding window and
//     `visits` never left 1.
// v6: `jump_cursor.at` is a `locations.id`, not a seq, and `jump_cursor.walking`
//     says whether the pane is part-way back through the list. A seq moves
//     every time its row merges forward, so a cursor holding one named a row
//     that was no longer there; and "mid-walk" used to be read off the
//     store-wide seq counter, which every linger record advances without the
//     list having moved at all.
//
// An upgraded database is refused by every older koi, and that is the intent
// rather than an accident to route around: a v1 build would go on writing
// cwd-relative paths into tables v2 has just made consistent, and a v2 build
// would go on writing positions into a table v3 has removed. Refusing is the
// only answer that does not corrupt meaning.
constexpr std::int64_t kSchemaVersion = 6;

constexpr const char* kSchema =
    "CREATE TABLE IF NOT EXISTS files ("
    "  path      TEXT    PRIMARY KEY,"
    "  visits    INTEGER NOT NULL DEFAULT 0,"
    "  edits     INTEGER NOT NULL DEFAULT 0,"
    "  last_ts   REAL    NOT NULL DEFAULT 0,"
    "  last_line INTEGER NOT NULL DEFAULT 1,"
    "  last_col  INTEGER NOT NULL DEFAULT 0,"
    // The branch the last visit was made on. A bonus at ranking time, never a
    // filter: a row from another branch is still a place you have been.
    // Databases from v3 get this column from the migration, not from here --
    // CREATE TABLE IF NOT EXISTS says nothing about a table that exists.
    "  branch    TEXT);"
    "CREATE TABLE IF NOT EXISTS symbols ("
    "  file    TEXT    NOT NULL,"
    "  symbol  TEXT    NOT NULL,"
    "  visits  INTEGER NOT NULL DEFAULT 0,"
    "  last_ts REAL    NOT NULL DEFAULT 0,"
    "  line    INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (file, symbol));"
    "CREATE TABLE IF NOT EXISTS co_visits ("
    "  from_file TEXT    NOT NULL,"
    "  to_file   TEXT    NOT NULL,"
    "  count     INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (from_file, to_file));"
    // No line and no column. v2 pinned a position, and a position in a table
    // nothing updates is wrong the moment anything is inserted above it -- the
    // pin still names line 3503 after the function that was there moved. A pin
    // names the file; where you were in it comes from `files` at read time.
    "CREATE TABLE IF NOT EXISTS file_pins ("
    "  slot INTEGER PRIMARY KEY,"
    "  file TEXT    NOT NULL);"
    "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    // The order every recency read wants, so "the newest 20 files" reads 20
    // index entries instead of sorting the whole table. DESC so the scan runs
    // forward; SQLite would walk an ASC index backwards just as happily, but
    // the plan is easier to read this way.
    //
    // Not a schema *version* change: an index is invisible to a reader that
    // does not know about it, so a v2 koi without this line opens a database
    // that has it and behaves exactly as before. That is why it belongs in
    // kSchema, which every open replays, rather than behind a v3 stamp that
    // would lock older builds out for nothing.
    "CREATE INDEX IF NOT EXISTS files_by_ts ON files(last_ts DESC);"
    // One row per place visited, and the jump list is a view over it. `line` is
    // a cache that healing keeps true, not what the row is; `symbol`,
    // `content`, `context`, `blob` and `uniq` are what will identify it once
    // recording fills them, and stay null until then.
    //
    // `seq` is the store-wide order (see NextStoreSeq): every touch takes the
    // next one, which is how a merge moves a place to the front of the jump
    // list without the DELETE + INSERT that used to destroy its visit count.
    "CREATE TABLE IF NOT EXISTS locations ("
    "  id      INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  path    TEXT    NOT NULL,"
    "  line    INTEGER NOT NULL,"
    "  col     INTEGER NOT NULL DEFAULT 0,"
    "  symbol  TEXT,"
    "  content TEXT,"
    "  context TEXT,"
    "  blob    TEXT,"
    "  uniq    INTEGER NOT NULL DEFAULT 0,"
    "  kind    INTEGER NOT NULL DEFAULT 0,"  // 0 visit, 1 edit
    "  visits  INTEGER NOT NULL DEFAULT 1,"
    "  misses  INTEGER NOT NULL DEFAULT 0,"
    "  last_ts REAL    NOT NULL DEFAULT 0,"
    // When the visit counter last moved, which is what the 30 s debounce is
    // measured against. `last_ts` cannot be: every touch refreshes it, so the
    // window slides ahead of itself and no second visit is ever counted.
    // Databases from v4 get this column from the migration, not from here.
    "  counted_ts REAL NOT NULL DEFAULT 0,"
    "  branch  TEXT,"
    "  seq     INTEGER NOT NULL);"
    // Where each pane sits in the jump list. `at` is a `locations.id`: it names
    // the row itself, which a merge does not change, and the order the list
    // walks in comes from that row's `seq` through a join. `walking` is whether
    // the pane is part-way back through the list -- set by a step back, cleared
    // by a record or by a step forward that reaches the front. Databases from v5
    // get `walking` from the migration, not from here.
    "CREATE TABLE IF NOT EXISTS jump_cursor ("
    "  pane    TEXT PRIMARY KEY,"
    "  at      INTEGER NOT NULL,"
    "  walking INTEGER NOT NULL DEFAULT 0);"
    // Unused until smart-jump has a prompt to type into. It is created now so
    // that adding it later does not cost a second schema stamp, and every
    // build in between refuse each other's databases for nothing.
    "CREATE TABLE IF NOT EXISTS queries ("
    "  prefix    TEXT NOT NULL,"
    "  target    TEXT NOT NULL,"
    "  use_count REAL NOT NULL DEFAULT 0,"
    "  last_ts   REAL NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (prefix, target));"
    "CREATE INDEX IF NOT EXISTS locations_by_path ON locations(path);"
    "CREATE INDEX IF NOT EXISTS locations_by_seq ON locations(seq DESC);"
    "CREATE INDEX IF NOT EXISTS queries_by_prefix ON queries(prefix);";

double Now() {
  const auto since = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration<double>(since).count();
}

// Null rather than "" when there is no repository, so a row from outside one is
// distinguishable from a row on a branch whose name was not read.
void BindBranch(Stmt& stmt, int at) {
  const std::string branch = GitBranch(ProjectRoot());
  if (branch.empty()) {
    stmt.Null(at);
  } else {
    stmt.Text(at, branch);
  }
}

// The root is a parameter so a loop over rows can derive it once. It used to be
// fetched -- and copied, path and all -- inside every call, which on a read that
// walked the whole files table was one allocation per row before the stat().
bool StillThere(const fs::path& root, const std::string& path) {
  std::error_code ec;
  return fs::exists(root.empty() ? fs::path{path} : (root / path), ec);
}

bool StillThere(const std::string& path) { return StillThere(ProjectRoot(), path); }

// The SQL LIMIT for a read that wants `want` rows back. Negative is SQLite's
// "no limit", which is what a non-positive `want` asks for.
std::int64_t ScanLimit(int want) {
  if (want <= 0) return -1;
  if (want > (std::numeric_limits<int>::max() / kVisitOverFetch)) return -1;
  return std::max(kVisitScanFloor, want * kVisitOverFetch);
}

}

// The one spelling every path in this database is stored under.
//
// Two producers used to reach these tables with two different spellings of the
// same file. The editor keyed its own paths against ProjectRoot() before
// handing them over; the file filter (`find . -printf '%P\n'`) runs in koi's
// current directory, and koi never chdirs, so its paths were relative to that.
// The two agree only when the editor was started at the project root. Started
// one directory down, `symbols.file` and `co_visits.to_file` held "project.cpp"
// while `files.path` held "koi/src/project.cpp" -- and since every read resolves
// a stored path against the root (StillThere), every symbol row failed to exist
// and every hot-symbol jump found nothing, forever.
//
// Fixing it at the call sites would mean fixing it again at the next one, so
// the invariant lives here instead: nothing reaches a table without passing
// through this function first, and the callers hand over what they have.
//
// The rule this defines is one sentence: **a path given to the store is a path
// valid from the current directory**. That is what the file filter produces,
// what an argv path is, and what `ed.doc.file` holds; `fs::absolute` resolves
// it, and the result is expressed relative to the root. A path outside the
// project keeps its absolute form -- it has no root-relative spelling, and
// "../../elsewhere.cpp" would be a key that means different things from
// different directories.
//
// The rule is what it is because only one of the two readings is always
// available: every producer can say where a file is from here, and only the
// editor could already say where it is from the root.
//
// Note this is NOT idempotent when the current directory is not the root:
// re-keying an already-root-relative "sub/f.cpp" from inside "sub" would yield
// "sub/sub/f.cpp". So a key never goes back in -- which is why CurrentFile()
// stopped keying and now hands over `ed.doc.file` as it is, and why the one
// place that must read both spellings (LegacyKey, migrating v1 rows written
// under the old contract) is written out separately and says so.
//
// It costs one weakly_canonical (so a few stats) per call. These are visit- and
// keystroke-frequency calls -- a file opened, a symbol jumped to -- not per
// frame, so the cost is not worth a cache; RankSymbols, the one caller that can
// see thousands of rows at once, memoises across its own rows instead.
std::string ProjectKey(std::string_view path) {
  if (path.empty()) return {};
  std::error_code ec;
  const fs::path root = ProjectRoot();
  const fs::path abs = fs::weakly_canonical(fs::absolute(fs::path{path}, ec), ec);
  if (ec || root.empty()) return std::string{path};
  const fs::path rel = fs::relative(abs, root, ec);
  if (ec || rel.empty() || rel.native().starts_with("..")) return abs.string();
  return rel.string();
}

namespace {

// True when `path` is `dir` itself extended by at least one component.
// Component-wise, not a string prefix: "/tmpfoo/x" is not under "/tmp".
bool UnderDirectory(const fs::path& dir, const fs::path& path) {
  if (dir.empty()) return false;
  auto want = dir.begin();
  auto have = path.begin();
  for (; want != dir.end(); ++want, ++have) {
    if ((have == path.end()) || (*have != *want)) return false;
  }
  return have != path.end();
}

// Derived once. TMPDIR is read from the environment koi was started in, and a
// process that changes its own TMPDIR mid-session is not a case worth a getenv
// per stored row -- the prune walks every path in three tables through here.
const std::vector<fs::path>& TempRoots() {
  static const std::vector<fs::path> roots = [] {
    std::vector<fs::path> out{fs::path{"/tmp"}, fs::path{"/var/tmp"}};
    if (const char* dir = std::getenv("TMPDIR"); (dir != nullptr) && (*dir != '\0')) {
      std::string text{dir};
      while ((text.size() > 1) && (text.back() == '/')) text.pop_back();
      out.emplace_back(text);
    }
    return out;
  }();
  return roots;
}

}

// The rules are the junk measured in the live store, and nothing beyond it.
// Every one of them describes a file the editor can legitimately be sitting in
// and whose history is worth nothing tomorrow: a scratch buffer under /tmp, a
// prompt file, a commit message, an agent's copy of a file the repository
// already holds under its own path.
//
// Only absolute keys are tested against the temp directories, and that is the
// whole of why a project checked out under /tmp still records: inside the root
// a key is relative, and outside it there is nothing here to remember.
bool StorablePath(std::string_view key) {
  if (key.empty()) return false;
  const fs::path path{key};
  bool after_claude = false;
  for (const fs::path& part : path) {
    const std::string component = part.string();
    if (component == ".git") return false;
    if (after_claude && (component == "worktrees")) return false;
    after_claude = (component == ".claude");
  }
  const std::string name = path.filename().string();
  if (name.starts_with("claude-prompt-") && name.ends_with(".md")) return false;
  if (!path.is_absolute()) return true;
  for (const fs::path& root : TempRoots()) {
    if (UnderDirectory(root, path)) return false;
  }
  return true;
}

bool StorableSymbolName(std::string_view name) {
  if (name.empty() || (name.size() > 200)) return false;
  // `operator()` and friends are the one legitimate use of this punctuation.
  if (name.starts_with("operator")) return true;
  return name.find_first_of(" \t\n([{,;") == std::string_view::npos;
}

// The same test both ways round: a relative name that no file answers to is a
// view name and goes in as it is, and everything else is a path and goes
// through the one keying rule. It used to live in jumplist.cpp, which was the
// only writer of `locations`; it is here now because it is a property of the
// table rather than of the jump list.
std::string LocationKey(std::string_view path) {
  if (path.empty()) return {};
  const fs::path as_path{path};
  std::error_code ec;
  if (!as_path.is_absolute() && !fs::exists(as_path, ec)) return std::string{path};
  return ProjectKey(path);
}

namespace {

// The first line of `<git>/HEAD`, turned into the name to store.
std::string BranchFromHead(const fs::path& head) {
  std::ifstream in{head, std::ios::binary};
  if (!in) return {};
  std::string line;
  std::getline(in, line);
  while (!line.empty() && ((line.back() == '\r') || (line.back() == ' '))) line.pop_back();
  constexpr std::string_view kRef = "ref: ";
  if (line.starts_with(kRef)) {
    std::string name = line.substr(kRef.size());
    constexpr std::string_view kHeads = "refs/heads/";
    if (name.starts_with(kHeads)) name.erase(0, kHeads.size());
    // A branch name may hold slashes (feature/x), so only the prefix is cut.
    return name;
  }
  // Detached: HEAD is the object id itself. Twelve characters is what git
  // shows and is past the point where a project has two objects sharing one.
  constexpr std::size_t kAbbrev = 12;
  if (line.size() < kAbbrev) return {};
  for (const char c : line) {
    const bool hex = ((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f')) ||
                     ((c >= 'A') && (c <= 'F'));
    if (!hex) return {};
  }
  return line.substr(0, kAbbrev);
}

// `<root>/.git/HEAD`, or empty when there is no repository at `root`. The file
// is not read here: what it names is both the branch and the stamp that says
// when the branch last changed, and two callers want one each.
fs::path GitHeadFile(const fs::path& root) {
  if (root.empty()) return {};
  std::error_code ec;
  fs::path git = root / ".git";
  if (fs::is_regular_file(git, ec)) {
    // A worktree, a submodule: `.git` is a file naming the real directory.
    // Followed one level and no further -- the file it points at is a real
    // git directory, and a chain of them is not a shape git makes.
    std::ifstream in{git, std::ios::binary};
    std::string line;
    std::getline(in, line);
    while (!line.empty() && ((line.back() == '\r') || (line.back() == ' '))) line.pop_back();
    constexpr std::string_view kGitDir = "gitdir: ";
    if (!line.starts_with(kGitDir)) return {};
    const fs::path named{line.substr(kGitDir.size())};
    git = named.is_absolute() ? named : (root / named);
  }
  return git / "HEAD";
}

}

std::string GitBranch(const fs::path& root) {
  const fs::path head = GitHeadFile(root);
  if (head.empty()) return {};
  std::error_code ec;
  const fs::file_time_type stamp = fs::last_write_time(head, ec);
  if (ec) return {};

  // One entry: koi has one project root for the life of the process, and the
  // stamp is what makes a checkout visible without re-reading the file on
  // every visit recorded. Plain statics -- this runs on the one UI thread.
  static fs::path memo_head;
  static fs::file_time_type memo_stamp;
  static std::string memo_branch;
  if ((memo_head == head) && (memo_stamp == stamp)) return memo_branch;
  memo_branch = BranchFromHead(head);
  memo_head = head;
  memo_stamp = stamp;
  return memo_branch;
}

namespace {

// Single quotes, so the shell sees one word whatever is in the path.
std::string Quoted(const std::string& text) {
  std::string out{"'"};
  for (const char c : text) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += '\'';
  return out;
}

// One git command, output captured, diagnostics discarded. Every failure --
// no git binary, no repository, a ref that does not resolve -- comes back the
// same way, as no output, because none of them is worth a word on the status
// line: the caller's answer is simply that it has no branch diff to offer.
std::string GitOutput(const std::string& args) {
  const common::CmdResult result = common::RunCmdWithCapture(
      "git " + args, common::CaptureMode::kPipe, common::CaptureMode::kDevNull);
  if (!result.output || (result.exit_status != 0)) return {};
  std::string text{result.output->buffer, result.output->size};
  while (!text.empty() && ((text.back() == '\n') || (text.back() == '\r'))) text.pop_back();
  return text;
}

}

// Not on any path that runs per keystroke or per record: this is up to four
// subprocesses, and it is called when a picker opens and only then.
//
// Cached on the branch and on HEAD's timestamp together, which is exactly "once
// per branch switch" -- a checkout rewrites HEAD, and nothing else the user does
// changes which commit the diff is against. The cache also absorbs the case that
// costs the most, a project with no repository: the key is the same every time
// and the empty answer is handed straight back.
const std::vector<std::string>& BranchDiffFiles() {
  static std::string memo_key{"\1"};  // No real key starts with this.
  static std::vector<std::string> memo;

  const fs::path root = ProjectRoot();
  const fs::path head = GitHeadFile(root);
  std::error_code ec;
  const fs::file_time_type stamp = fs::last_write_time(head, ec);
  std::string key = head.string();
  key += '\0';
  if (!ec) key += std::to_string(stamp.time_since_epoch().count());
  key += '\0';
  key += GitBranch(root);
  if (key == memo_key) return memo;
  memo_key = std::move(key);
  memo.clear();
  if (head.empty() || ec) return memo;

  // The first of the three that resolves wins. `merge-base` over all three at
  // once would answer something else entirely -- the ancestor common to every
  // one of them -- and a repository with no origin, or one whose default branch
  // is master, has to come out the same as the usual case.
  const std::string dir = Quoted(root.string());
  std::string base;
  for (const std::string_view against : {"origin/HEAD", "main", "master"}) {
    base = GitOutput("-C " + dir + " merge-base HEAD " + std::string{against});
    if (!base.empty()) break;
  }
  if (base.empty()) return memo;

  // --relative, so the paths come back relative to the root koi keys against
  // rather than to the top of the repository. The two differ whenever the
  // project marker is below the git one, and then an unrelativised path matches
  // no row in the store.
  const std::string changed = GitOutput("-C " + dir + " diff --name-only --relative " + base);
  for (std::size_t at = 0; at < changed.size();) {
    const std::size_t eol = std::min(changed.find('\n', at), changed.size());
    std::string_view line{changed.data() + at, eol - at};
    while (!line.empty() && (line.back() == '\r')) line.remove_suffix(1);
    if (!line.empty()) memo.emplace_back(line);
    at = eol + 1;
  }
  return memo;
}

// `meta` is the authority and MAX(seq) is the floor. Either alone is enough in
// the normal case and neither is enough alone: the counter has to survive a
// prune that takes every row away, and it must never hand back a number a row
// already holds if an older build or a failed write left `meta` behind.
std::int64_t NextStoreSeq(sqlite3* db) {
  std::int64_t next = 0;
  {
    Stmt read{db,
              "SELECT MAX("
              " COALESCE((SELECT CAST(value AS INTEGER) FROM meta WHERE key = 'seq'), 0),"
              " COALESCE((SELECT MAX(seq) FROM locations), 0));"};
    if (!read || !read.Step()) return 0;
    next = read.Integer(0) + 1;
  }
  Stmt put{db,
           "INSERT INTO meta(key, value) VALUES('seq', ?1)"
           " ON CONFLICT(key) DO UPDATE SET value = excluded.value;"};
  if (!put) return 0;
  put.Text(1, std::to_string(next));
  return put.Run() ? next : 0;
}

// One entry per progressive prefix, each spelled as its terms joined by single
// spaces so that the key a lookup builds is the key a write made. Whitespace
// runs collapse and leading and trailing whitespace is dropped: "key  cpp " and
// "key cpp" are the same query, and a table that thought otherwise would learn
// each spelling separately and confirm neither.
std::vector<std::string> QueryPrefixes(std::string_view typed_terms) {
  const auto is_space = [](char c) {
    return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\v') || (c == '\f') || (c == '\r');
  };
  std::vector<std::string> out;
  std::string joined;
  std::size_t at = 0;
  while (at < typed_terms.size()) {
    while ((at < typed_terms.size()) && is_space(typed_terms[at])) ++at;
    const std::size_t start = at;
    while ((at < typed_terms.size()) && !is_space(typed_terms[at])) ++at;
    if (at == start) break;
    if (!joined.empty()) joined += ' ';
    joined.append(typed_terms.substr(start, at - start));
    out.push_back(joined);
  }
  return out;
}

namespace {

// The decay, applied on read and never written back. A row is worth what it was
// worth when it was last confirmed, times 0.975 for every day since -- which
// halves it in a month and takes it under the drop threshold in about three.
double DecayedUse(double use_count, double last_ts, double now) {
  const double days = (now - last_ts) / 86400.0;
  if (days <= 0.0) return use_count;
  return use_count * std::pow(kQueryDecayPerDay, days);
}

struct SqliteProject final : ProjectStore {
  sqlite3* db{nullptr};

  // The one statement worth keeping prepared. Every other read here runs once
  // per user action; this one runs kSmartAdaptiveProbe times per keystroke, and
  // prepare+finalize was most of what it cost. A prepared statement is not a
  // snapshot -- each Step re-reads the table -- so a write landing between two
  // calls is seen by the next one.
  std::optional<Stmt> adaptive;

  ~SqliteProject() override {
    // Before the close, not after: sqlite3_close refuses a connection that
    // still owns a statement, and hands the handle back unclosed.
    adaptive.reset();
    if (db != nullptr) sqlite3_close(db);
  }

  void Exec(const char* sql) { ExecSql(db, sql); }

  void BumpFile(std::string_view raw, Index line, Index column, bool edit) {
    const std::string path = ProjectKey(raw);
    if (!StorablePath(path)) return;
#define KOI_BUMP_SQL(field)                                                  \
  "INSERT INTO files(path, " field ", last_ts, last_line, last_col, branch)" \
  " VALUES(?1,1,?2,?3,?4,?5)"                                                \
  " ON CONFLICT(path) DO UPDATE SET " field " = " field " + 1,"              \
  " last_ts = excluded.last_ts, last_line = excluded.last_line,"             \
  " last_col = excluded.last_col, branch = excluded.branch;"
    Stmt stmt{db, edit ? KOI_BUMP_SQL("edits") : KOI_BUMP_SQL("visits")};
#undef KOI_BUMP_SQL
    if (!stmt) return;
    stmt.Text(1, path);
    stmt.Real(2, Now());
    stmt.Int(3, std::max<Index>(1, line));
    stmt.Int(4, std::max<Index>(0, column));
    BindBranch(stmt, 5);
    stmt.Run();
  }

  void RecordVisit(std::string_view path, Index line, Index column) override {
    BumpFile(path, line, column, false);
  }

  void RecordEdit(std::string_view path, Index line, Index column) override {
    BumpFile(path, line, column, true);
  }

  void RecordSymbolVisit(std::string_view symbol, std::string_view raw_file, Index line) override {
    const std::string file = ProjectKey(raw_file);
    if (!StorableSymbolName(symbol) || !StorablePath(file)) return;
    Stmt stmt{db,
              "INSERT INTO symbols(file, symbol, visits, last_ts, line) VALUES(?1,?2,1,?3,?4)"
              " ON CONFLICT(file, symbol) DO UPDATE SET visits = visits + 1,"
              " last_ts = excluded.last_ts,"
              " line = CASE WHEN excluded.line > 0 THEN excluded.line ELSE line END;"};
    if (!stmt) return;
    stmt.Text(1, file);
    stmt.Text(2, symbol);
    stmt.Real(3, Now());
    stmt.Int(4, std::max<Index>(0, line));
    stmt.Run();
  }

  void RecordCoVisit(std::string_view raw_from, std::string_view raw_to) override {
    // Keyed before the comparison, not after: the two arguments arrive from
    // different producers in different spellings (`from` is the open buffer,
    // `to` is a picker row), so a file co-visiting itself sailed straight past
    // an `==` on the raw strings.
    const std::string from_file = ProjectKey(raw_from);
    const std::string to_file = ProjectKey(raw_to);
    if (!StorablePath(from_file) || !StorablePath(to_file) || (from_file == to_file)) return;
    Stmt stmt{db,
              "INSERT INTO co_visits(from_file, to_file, count) VALUES(?1,?2,1)"
              " ON CONFLICT(from_file, to_file) DO UPDATE SET count = count + 1;"};
    if (!stmt) return;
    stmt.Text(1, from_file);
    stmt.Text(2, to_file);
    stmt.Run();
  }

  // The row this record belongs on, or 0 for none. Same path, and either the
  // same enclosing symbol or a line close enough that the two are one place.
  //
  // Newest first, because a merge candidate is a place the user was recently in
  // and because that is the row the jump list would step to next. There is
  // normally at most one; a migrated database, or a record made before the
  // symbol was resolvable, can leave two.
  std::int64_t MergeTarget(const std::string& key, const LocationRecord& row, std::int64_t& seq,
                           double& counted_ts) {
    Stmt stmt{db,
              row.exact
                  // The jump list's rule: this line and no other. Symbol-merge
                  // is off too -- it reaches across a whole function, and a
                  // step target must stay distinct from the place it left.
                  ? "SELECT id, seq, counted_ts FROM locations WHERE path=?1 AND line=?3"
                    " ORDER BY seq DESC LIMIT 1;"
                  : "SELECT id, seq, counted_ts FROM locations WHERE path=?1"
                    " AND ((?2 IS NOT NULL AND symbol IS NOT NULL AND symbol=?2)"
                    "      OR (line BETWEEN ?3 AND ?4))"
                    " ORDER BY seq DESC LIMIT 1;"};
    if (!stmt) return 0;
    stmt.Text(1, key);
    if (row.exact) {
      stmt.Int(3, row.line);
    } else {
      if (row.symbol.empty()) {
        stmt.Null(2);
      } else {
        stmt.Text(2, row.symbol);
      }
      stmt.Int(3, row.line - kLocationMergeLines);
      stmt.Int(4, row.line + kLocationMergeLines);
    }
    if (!stmt.Step()) return 0;
    seq = stmt.Integer(1);
    counted_ts = stmt.Double(2);
    return stmt.Integer(0);
  }

  // The text half of a row, bound the same way by the insert and the update.
  // `has_text` is what decides whether it is said at all -- a record made
  // without the buffer leaves the four columns alone rather than clearing them.
  static void BindText(Stmt& stmt, int at, const LocationRecord& row) {
    if (row.content.empty()) {
      stmt.Null(at);
    } else {
      stmt.Text(at, row.content);
    }
    if (row.context.empty()) {
      stmt.Null(at + 1);
    } else {
      stmt.Text(at + 1, row.context);
    }
    if (row.blob.empty()) {
      stmt.Null(at + 2);
    } else {
      stmt.Text(at + 2, row.blob);
    }
    stmt.Int(at + 3, std::max<std::int64_t>(0, row.uniq));
  }

  std::int64_t WriteLocation(const LocationRecord& row) override {
    if (db == nullptr) return 0;
    const std::string key = LocationKey(row.path);
    if (!StorablePath(key)) return 0;

    // One transaction whichever way this is reached: the jump list opens its
    // own around the write and the cursor move, and a boundary record has none
    // to join. Nested BEGINs are an error in SQLite, so this asks.
    const bool own = (sqlite3_get_autocommit(db) != 0);
    if (own && !ExecSql(db, "BEGIN IMMEDIATE;")) return 0;
    const std::int64_t seq = WriteLocationLocked(key, row);
    if (own) {
      if ((seq != 0) && ExecSql(db, "COMMIT;")) return seq;
      ExecSql(db, "ROLLBACK;");
      return 0;
    }
    return seq;
  }

  std::int64_t WriteLocationLocked(const std::string& key, const LocationRecord& row) {
    const Index line = std::max<Index>(1, row.line);
    const double now = Now();

    std::int64_t at_seq = 0;
    double counted_ts = 0;
    const std::int64_t id = MergeTarget(key, row, at_seq, counted_ts);
    if (id == 0) {
      const std::int64_t next = NextStoreSeq(db);
      if (next <= 0) return 0;
      Stmt stmt{db,
                "INSERT INTO locations(path,line,col,symbol,content,context,blob,uniq,kind,"
                " visits,misses,last_ts,counted_ts,branch,seq)"
                " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,1,0,?10,?10,?11,?12);"};
      if (!stmt) return 0;
      stmt.Text(1, key);
      stmt.Int(2, line);
      stmt.Int(3, std::max<Index>(0, row.col));
      if (row.symbol.empty()) {
        stmt.Null(4);
      } else {
        stmt.Text(4, row.symbol);
      }
      BindText(stmt, 5, row);
      stmt.Int(9, row.kind);
      stmt.Real(10, now);
      BindBranch(stmt, 11);
      stmt.Int(12, next);
      return stmt.Run() ? next : 0;
    }

    // Already at the front: the row is where a move-to-front would put it, so
    // the seq stands and the counter is not run for a place the user has not
    // left. Everything else about the row still refreshes.
    std::int64_t newest = 0;
    {
      Stmt stmt{db, "SELECT COALESCE(MAX(seq),0) FROM locations;"};
      if (!stmt || !stmt.Step()) return 0;
      newest = stmt.Integer(0);
    }
    std::int64_t next = at_seq;
    if (at_seq < newest) {
      next = NextStoreSeq(db);
      if (next <= 0) return 0;
    }
    // The debounce: a row touched again within the window keeps its count. It
    // is measured against the row rather than against the recorder so that two
    // panes on one place cannot double-count it either -- and against the last
    // visit that counted rather than the last touch, so an hour of work at one
    // place is worth an hour rather than one visit. Read and written inside the
    // caller's transaction, so the two cannot interleave.
    const bool counts = ((now - counted_ts) >= kLocationVisitDebounce);
    const std::int64_t visit = counts ? 1 : 0;

    // `misses=0` because a record is by definition a hit: a row hidden at 3
    // misses is out of the `c` corpus until a heal resolves it, and the user
    // standing in it is the strongest evidence there is that it is still there.
    Stmt stmt{db, row.has_text
                      ? "UPDATE locations SET line=?1, col=?2, symbol=COALESCE(?3,symbol),"
                        " content=?4, context=?5, blob=?6, uniq=?7, misses=0,"
                        " kind=MAX(kind,?8), visits=visits+?9, last_ts=?10, branch=?11, seq=?12,"
                        " counted_ts=?14"
                        " WHERE id=?13;"
                      : "UPDATE locations SET line=?1, col=?2, symbol=COALESCE(?3,symbol),"
                        " misses=0,"
                        " kind=MAX(kind,?8), visits=visits+?9, last_ts=?10, branch=?11, seq=?12,"
                        " counted_ts=?14"
                        " WHERE id=?13;"};
    if (!stmt) return 0;
    stmt.Int(1, line);
    stmt.Int(2, std::max<Index>(0, row.col));
    if (row.symbol.empty()) {
      stmt.Null(3);
    } else {
      stmt.Text(3, row.symbol);
    }
    if (row.has_text) BindText(stmt, 4, row);
    stmt.Int(8, row.kind);
    stmt.Int(9, visit);
    stmt.Real(10, now);
    BindBranch(stmt, 11);
    stmt.Int(12, next);
    stmt.Int(13, id);
    stmt.Real(14, counts ? now : counted_ts);
    return stmt.Run() ? next : 0;
  }

  // Both queries take the row cap as ?2 and hand back five columns; only the
  // frecency one references ?1 (now) and ?3 (the branch), and the recency one
  // selects a literal 0 for the score it has none of. Binding an index the
  // statement does not mention is fine -- SQLite sizes its parameter array by
  // the largest index that appears -- so the two share one binder.
  std::vector<FileVisit> Query(const char* sql, bool frecency, int want) {
    std::vector<FileVisit> out;
    Stmt stmt{db, sql};
    if (!stmt) return out;
    if (frecency) {
      stmt.Real(1, Now());
      BindBranch(stmt, 3);
    }
    stmt.Int(2, ScanLimit(want));
    if (want > 0) out.reserve(static_cast<std::size_t>(want));
    // Once, not once per row: see StillThere.
    const fs::path root = ProjectRoot();
    while (stmt.Step()) {
      FileVisit entry;
      entry.path = stmt.Column(0);
      entry.line = static_cast<Index>(stmt.Integer(1));
      entry.column = static_cast<Index>(stmt.Integer(2));
      entry.last_ts = stmt.Double(3);
      entry.score = stmt.Double(4);
      if (!StillThere(root, entry.path)) continue;
      out.push_back(std::move(entry));
      // The SQL limit is the over-fetch; this is the cap the caller asked for.
      // Stopping here is also what keeps the stat()s down to the rows that were
      // going to be shown.
      if ((want > 0) && (std::ssize(out) >= want)) break;
    }
    return out;
  }

  std::vector<FileVisit> RecentFiles(int want) override {
    return Query("SELECT path, last_line, last_col, last_ts, 0 FROM files"
                 " WHERE last_ts > 0 ORDER BY last_ts DESC LIMIT ?2;",
                 false, want);
  }

  bool LastVisit(std::string_view raw, Index& line, Index& column) override {
    Stmt stmt{db, "SELECT last_line, last_col FROM files WHERE path = ? AND last_ts > 0;"};
    if (!stmt) return false;
    stmt.Text(1, ProjectKey(raw));
    if (!stmt.Step()) return false;
    line = static_cast<Index>(stmt.Integer(0));
    column = static_cast<Index>(stmt.Integer(1));
    return line > 0;
  }

  // No index can serve this ORDER BY -- the sort key is an expression over
  // `visits`, `edits` and the current time -- so SQLite scans `files` and sorts.
  // The LIMIT still earns its place: the sorter keeps only that many rows, and
  // nothing past it is copied into a std::string or stat()ed. Bounding the
  // *scan* is the prune's job, not this query's.
  //
  // The score comes back with the row rather than only ordering it: the file
  // picker has one more term of its own to apply -- the branch diff, which
  // costs a subprocess and cannot go in here -- and an ordinal is not something
  // a multiplier can be applied to.
  std::vector<FileVisit> FrecentFiles(int want) override {
    return Query("SELECT path, last_line, last_col, last_ts,"
                 " (visits + edits * 3) * (" KOI_MULT_SQL("last_ts") ") *"
                 " (" KOI_BRANCH_SQL("branch") ")"
                 " FROM files ORDER BY 5 DESC LIMIT ?2;",
                 true, want);
  }

  // The limit belongs in the SQL, not only in the loop below.
  //
  // Stopping the loop at `limit` never stopped the work: the ORDER BY is an
  // expression over `visits` and the current time, so SQLite has to run every
  // row of `symbols` through a sorter before it can answer which one comes
  // first, and the first Step() therefore paid for the whole table however few
  // rows the caller was going to keep. LIMIT does not remove the scan -- only
  // the prune does that -- but it caps the sorter, which is what the scan was
  // spending its time filling.
  //
  // No index is added for this, and one was considered: an ORDER BY over an
  // expression cannot be served by a column index, so `symbols(visits, last_ts)`
  // leaves the temp B-tree exactly where it is and only changes the scan into an
  // index range scan plus a row lookup per row. Measured at 20000 rows that made
  // this query slower, not faster (5.3 ms -> 9.4 ms), and HotFiles' with it. The
  // sorter is the cost, and the LIMIT and the prune are what bound it.
  std::vector<SymbolVisit> HotSymbols(int limit) override {
    std::vector<SymbolVisit> out;
    if (limit <= 0) return out;
    // `symbols` has no branch of its own -- a symbol is a place in a file, and
    // the file is what was visited on a branch -- so the bonus is joined in
    // from `files`. LEFT, because a symbol row can outlive the file row that
    // named it, and then it simply gets no bonus: one primary-key seek per row,
    // against a sorter this query was already paying for.
    Stmt stmt{db,
              "SELECT s.symbol, s.file, s.line FROM symbols s"
              " LEFT JOIN files f ON f.path = s.file"
              " WHERE s.line > 0 AND s.last_ts > 0 AND s.visits > 0"
              " ORDER BY s.visits * (" KOI_MULT_SQL("s.last_ts") ") *"
              " (" KOI_BRANCH_SQL("f.branch") ") DESC LIMIT ?2;"};
    if (!stmt) return out;
    stmt.Real(1, Now());
    BindBranch(stmt, 3);
    // Over-fetched for the same reason the file reads are: rows naming a file
    // that is gone are dropped here, and the query cannot know which those are.
    stmt.Int(2, ScanLimit(limit));
    // Once, not once per row: see StillThere.
    const fs::path root = ProjectRoot();
    while (stmt.Step() && (std::ssize(out) < limit)) {
      SymbolVisit entry;
      entry.symbol = stmt.Column(0);
      entry.file = stmt.Column(1);
      entry.line = static_cast<Index>(stmt.Integer(2));
      if (StillThere(root, entry.file)) out.push_back(std::move(entry));
    }
    return out;
  }

  std::vector<std::string> HotFiles(int limit, std::string_view raw_current) override {
    const std::string current_file = ProjectKey(raw_current);
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    const auto add = [&](std::string path) {
      if (path.empty() || !seen.insert(path).second) return;
      out.push_back(std::move(path));
    };
    add(current_file);
    if (db == nullptr) return out;

    if (limit > 0) {
      Stmt stmt{db,
                "SELECT s.file FROM symbols s"
                " LEFT JOIN files f ON f.path = s.file"
                " WHERE s.visits > 0 AND s.last_ts > 0"
                " ORDER BY s.visits * (" KOI_MULT_SQL("s.last_ts") ") *"
                " (" KOI_BRANCH_SQL("f.branch") ") DESC LIMIT ?2;"};
      if (stmt) {
        stmt.Real(1, Now());
        stmt.Int(2, limit);
        BindBranch(stmt, 3);
        while (stmt.Step()) add(stmt.Column(0));
      }
    }

    Stmt stmt{db,
              "SELECT to_file FROM co_visits WHERE from_file = ?1 ORDER BY count DESC LIMIT ?2;"};
    if (stmt) {
      stmt.Text(1, current_file);
      stmt.Int(2, kMaxCoVisitFiles);
      while (stmt.Step()) add(stmt.Column(0));
    }
    return out;
  }

  size_t RankSymbols(std::vector<Symbol>& rows, std::string_view raw_current) override {
    if (rows.empty() || (db == nullptr)) return 0;
    const std::string current_file = ProjectKey(raw_current);

    // The rows are the scanner's, spelled however the file filter spelled them;
    // `scores`, `co_visits` and `current_file` are all in the database's
    // spelling. Comparing the two directly is what kept the current file out of
    // group 0 whenever koi was started below the project root. The rows
    // themselves are left alone -- the caller opens those paths, and they are
    // valid where the scan ran.
    //
    // A file contributes many symbols, so the memo turns "one weakly_canonical
    // per row" into one per distinct file.
    std::unordered_map<std::string, std::string> keyed;
    const auto key_of = [&keyed](const std::string& path) -> const std::string& {
      const auto found = keyed.find(path);
      if (found != keyed.end()) return found->second;
      return keyed.emplace(path, ProjectKey(path)).first->second;
    };
    // The distinct files these rows came from, keyed once, in first-seen order.
    // std::unordered_map never invalidates a value's address, so holding views
    // into the memo is safe however many more paths go into it.
    std::vector<std::string_view> files;
    {
      std::unordered_set<std::string_view> seen;
      for (const Symbol& row : rows) {
        const std::string_view key = key_of(row.path);
        if (seen.insert(key).second) files.push_back(key);
      }
    }

    // Read the score of the rows being ranked, not of every symbol ever
    // visited. This used to pull the whole table into the map on every call --
    // and it is called once per picker result set, on the keystroke that opens
    // the picker -- to look up at most one entry per row handed in.
    //
    // One seek per distinct file, on the (file, symbol) primary key, so the cost
    // follows the input instead of the store. The input is bounded and small:
    // every caller builds `rows` by scanning HotFiles(kDefaultHotFileLimit),
    // which returns at most the current file plus that limit plus
    // kMaxCoVisitFiles paths.
    std::unordered_map<std::string, double> scores;
    {
      Stmt stmt{db,
                "SELECT s.symbol, s.visits * (" KOI_MULT_SQL("s.last_ts") ") *"
                " (" KOI_BRANCH_SQL("f.branch") ")"
                " FROM symbols s LEFT JOIN files f ON f.path = s.file WHERE s.file = ?2;"};
      if (stmt) {
        const double now = Now();
        std::string joined;
        for (const std::string_view file : files) {
          stmt.Reset();
          stmt.Real(1, now);
          stmt.Text(2, file);
          // Reset() clears the bindings, so the branch goes back on per file.
          // GitBranch answers from a memo keyed on HEAD's timestamp, so this is
          // a stat and no more.
          BindBranch(stmt, 3);
          while (stmt.Step()) {
            joined.assign(file).append(1, '\0').append(stmt.Column(0));
            scores[joined] = stmt.Double(1);
          }
        }
      }
    }

    std::unordered_map<std::string, double> co_visits;
    if (!current_file.empty()) {
      Stmt stmt{db, "SELECT to_file, count FROM co_visits WHERE from_file = ?1;"};
      if (stmt) {
        stmt.Text(1, current_file);
        while (stmt.Step()) co_visits[stmt.Column(0)] = static_cast<double>(stmt.Integer(1));
      }
    }

    struct Key {
      std::uint32_t at;
      std::uint8_t group;
      double score;
    };
    std::vector<Key> keys;
    keys.reserve(rows.size());
    std::string joined;
    size_t head = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
      const Symbol& row = rows[i];
      const std::string& path = key_of(row.path);
      double score = 0;
      joined.assign(path).append(1, '\0').append(row.name);
      if (const auto found = scores.find(joined); found != scores.end()) score = found->second;
      if (const auto found = co_visits.find(path); found != co_visits.end()) {
        score += found->second * 0.5;
      }
      const auto group = static_cast<std::uint8_t>((path == current_file) ? 0
                                                   : (score > 0)         ? 1
                                                                         : 2);
      if (group < 2) ++head;
      keys.push_back(Key{static_cast<std::uint32_t>(i), group, score});
    }

    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
      if (a.group != b.group) return a.group < b.group;
      return a.score > b.score;
    });

    std::vector<Symbol> ordered;
    ordered.reserve(rows.size());
    for (const Key& key : keys) ordered.push_back(std::move(rows[key.at]));
    rows = std::move(ordered);
    return head;
  }

  // The join is the whole feature: `file_pins` holds the file, `files` holds
  // where that file was last left, and a pin is the two together. LEFT, so a
  // file pinned before it was ever visited still comes back -- at line 1, which
  // is where opening it would land anyway.
  std::vector<Pin> Pins() override {
    std::vector<Pin> out(static_cast<size_t>(kPinSlots));
    Stmt stmt{db,
              "SELECT p.slot, p.file, COALESCE(f.last_line, 1), COALESCE(f.last_col, 0)"
              " FROM file_pins p LEFT JOIN files f ON f.path = p.file"
              " WHERE p.slot BETWEEN 1 AND ?1;"};
    if (!stmt) return out;
    stmt.Int(1, kPinSlots);
    while (stmt.Step()) {
      const auto slot = static_cast<int>(stmt.Integer(0));
      if ((slot < 1) || (slot > kPinSlots)) continue;
      Pin& pin = out[static_cast<size_t>(slot - 1)];
      pin.path = stmt.Column(1);
      pin.line = static_cast<Index>(stmt.Integer(2));
      pin.column = static_cast<Index>(stmt.Integer(3));
    }
    return out;
  }

  // Pinning a file is one change to the pin table, not two, so it is one
  // transaction.
  //
  // The two statements below are a pair: the DELETE takes the file out of
  // whatever slot it was in, and the INSERT puts it in the slot the user asked
  // for. Run as two implicit transactions -- which is what every unwrapped
  // statement is -- each commits on its own, and the gap between them is a
  // window where the pin exists nowhere. A crash lands in it, so does an INSERT
  // that fails for any reason (Run() is allowed to fail and the store has no
  // channel to report it on), and so does a second koi on the same database:
  // its own dedup DELETE for the same file can commit between this one's DELETE
  // and INSERT, and the user is left with one fewer pin than either pane asked
  // for. Losing a pin the user just placed is losing the state they most expect
  // to survive.
  //
  // BEGIN IMMEDIATE, the same as the migration: it takes the write lock up
  // front, so the other koi blocks on it (busy_timeout, five seconds) instead
  // of interleaving with it. If it cannot be taken, nothing runs -- the pins
  // stay exactly as they were, which is the one outcome worth having when the
  // write cannot be made atomically. Anything that fails after it rolls back,
  // and every path here ends with the transaction closed.
  void SetPin(int slot, std::string_view raw_file) override {
    const std::string file = ProjectKey(raw_file);
    if ((slot < 1) || (slot > kPinSlots) || file.empty()) return;
    if (!ExecSql(db, "BEGIN IMMEDIATE;")) return;

    bool ok = false;
    // Scoped so both statements are finalised before the COMMIT below, and so
    // neither outlives the transaction it belongs to.
    {
      Stmt drop{db, "DELETE FROM file_pins WHERE file = ?1 AND slot <> ?2;"};
      Stmt put{db, "INSERT OR REPLACE INTO file_pins(slot, file) VALUES(?1,?2);"};
      if (drop && put) {
        drop.Text(1, file);
        drop.Int(2, slot);
        put.Int(1, slot);
        put.Text(2, file);
        // Both, in order, and both have to have run: a DELETE that failed is a
        // duplicate pin left behind, and an INSERT that failed with the DELETE
        // committed is the lost pin this transaction exists to prevent.
        ok = drop.Run() && put.Run();
      }
    }
    ExecSql(db, ok ? "COMMIT;" : "ROLLBACK;");
  }

  void ClearPin(int slot) override {
    Stmt stmt{db, "DELETE FROM file_pins WHERE slot = ?1;"};
    if (!stmt) return;
    stmt.Int(1, slot);
    stmt.Run();
  }

  int FileCount() override {
    Stmt stmt{db, "SELECT COUNT(*) FROM files;"};
    if (!stmt || !stmt.Step()) return 0;
    return static_cast<int>(stmt.Integer(0));
  }

  std::vector<AnchorRow> AnchorsFor(std::string_view path) override {
    std::vector<AnchorRow> out;
    // No `uniq`: the ladder counts a line's occurrences in the file it is
    // holding, which is the same number measured against the truth.
    Stmt stmt{db, "SELECT id,seq,line,content,context,blob,symbol,misses FROM locations"
                  " WHERE path=?1 ORDER BY id;"};
    if (!stmt) return out;
    stmt.Text(1, path);
    while (stmt.Step()) {
      AnchorRow row;
      row.id = stmt.Integer(0);
      row.seq = stmt.Integer(1);
      row.line = stmt.Integer(2);
      row.content = stmt.Column(3);
      row.context = stmt.Column(4);
      row.blob = stmt.Column(5);
      row.symbol_null = (sqlite3_column_type(stmt.handle, 6) == SQLITE_NULL);
      row.symbol = stmt.Column(6);
      row.misses = stmt.Integer(7);
      out.push_back(std::move(row));
    }
    return out;
  }

  std::vector<AnchorRow> AnchorPositionsFor(std::string_view path) override {
    std::vector<AnchorRow> out;
    Stmt stmt{db, "SELECT id,seq,line FROM locations WHERE path=?1 ORDER BY id;"};
    if (!stmt) return out;
    stmt.Text(1, path);
    while (stmt.Step()) {
      AnchorRow row;
      row.id = stmt.Integer(0);
      row.seq = stmt.Integer(1);
      row.line = stmt.Integer(2);
      out.push_back(row);
    }
    return out;
  }

  // One transaction for the file, and every statement finalised inside it --
  // same shape as the jump list's Record. A heal that lands half written is a
  // set of rows describing two different states of one file.
  //
  // Every statement is keyed on (id, seq), so a row the recorder has written
  // since the job took its snapshot matches nothing and is left alone. That is
  // a skip and not a failure: the batch is still coherent, and the true answer
  // for that row is the one already in the store. True means the transaction
  // committed, however many rows it turned out to touch.
  bool ApplyHeals(const std::vector<AnchorHeal>& heals) override {
    if ((db == nullptr) || heals.empty()) return false;
    if (!ExecSql(db, "BEGIN IMMEDIATE;")) return false;
    bool ok = true;
    {
      // A miss is a counter and nothing else: not the line, not the text, not
      // the blob. Leaving the blob alone is what keeps the next heal from
      // deciding through the blob gate that a row it could not find is true.
      Stmt missed{db, "UPDATE locations SET misses=misses+1 WHERE id=?1 AND seq=?2;"};
      Stmt moved{db, "UPDATE locations SET line=?2, misses=0, blob=?3"
                     " WHERE id=?1 AND seq=?4;"};
      Stmt retext{db, "UPDATE locations SET line=?2, misses=0, blob=?3,"
                      " content=?4, context=?5 WHERE id=?1 AND seq=?6;"};
      // COALESCE the other way round from the recorder's: this only ever fills
      // a null in, and never replaces a name the recorder resolved from live
      // syntax.
      Stmt named{db, "UPDATE locations SET symbol=COALESCE(symbol,?2)"
                     " WHERE id=?1 AND seq=?3;"};
      for (const AnchorHeal& heal : heals) {
        if (heal.miss) {
          missed.Reset();
          missed.Int(1, heal.id);
          missed.Int(2, heal.seq);
          ok = missed.Run() && ok;
          continue;
        }
        Stmt& write = heal.refresh_text ? retext : moved;
        write.Reset();
        write.Int(1, heal.id);
        write.Int(2, std::max<Index>(1, heal.line));
        if (heal.blob.empty()) {
          write.Null(3);
        } else {
          write.Text(3, heal.blob);
        }
        if (heal.refresh_text) {
          write.Text(4, heal.content);
          write.Text(5, heal.context);
          write.Int(6, heal.seq);
        } else {
          write.Int(4, heal.seq);
        }
        ok = write.Run() && ok;
        if (!heal.set_symbol || heal.symbol.empty()) continue;
        named.Reset();
        named.Int(1, heal.id);
        named.Text(2, heal.symbol);
        named.Int(3, heal.seq);
        ok = named.Run() && ok;
      }
    }
    if (ok && ExecSql(db, "COMMIT;")) return true;
    ExecSql(db, "ROLLBACK;");
    return false;
  }

  void RecordQueryAccept(std::string_view typed_terms, std::string_view target) override {
    if ((db == nullptr) || target.empty()) return;
    const std::vector<std::string> prefixes = QueryPrefixes(typed_terms);
    if (prefixes.empty()) return;

    // One transaction for the whole set, and it joins the caller's when there is
    // one: an accept is a jump plus this, and the two should not be able to land
    // half each.
    const bool own = (sqlite3_get_autocommit(db) != 0);
    if (own && !ExecSql(db, "BEGIN IMMEDIATE;")) return;
    // One prepared statement, reset per prefix. The decay factor is bound rather
    // than spelled into the SQL so the constant lives in exactly one place.
    Stmt stmt{db,
              "INSERT INTO queries(prefix, target, use_count, last_ts) VALUES(?1,?2,1,?3)"
              " ON CONFLICT(prefix, target) DO UPDATE SET"
              " use_count = use_count * ?4 + 1, last_ts = excluded.last_ts;"};
    bool ok = static_cast<bool>(stmt);
    const double now = Now();
    for (const std::string& prefix : prefixes) {
      if (!ok) break;
      stmt.Reset();
      stmt.Text(1, prefix);
      stmt.Text(2, target);
      stmt.Real(3, now);
      stmt.Real(4, kQueryAcceptDecay);
      ok = stmt.Run();
    }
    if (!own) return;
    if (ok && ExecSql(db, "COMMIT;")) return;
    ExecSql(db, "ROLLBACK;");
  }

  double AdaptiveUse(std::string_view prefix, std::string_view target) override {
    if (db == nullptr) return 0.0;
    if (!adaptive) {
      adaptive.emplace(db,
                       "SELECT use_count, last_ts FROM queries WHERE prefix = ?1 AND target = ?2;");
    }
    Stmt& stmt = *adaptive;
    if (!stmt) return 0.0;
    stmt.Text(1, prefix);
    stmt.Text(2, target);
    // Reset before returning, whichever way it went: a statement left standing
    // on a row holds a read open on the connection, and the next write on it
    // would be arguing with a cursor of its own.
    const double use = stmt.Step() ? DecayedUse(stmt.Double(0), stmt.Double(1), Now()) : 0.0;
    stmt.Reset();
    return use;
  }

  sqlite3* Connection() override { return db; }
};

// One row per (file, symbol) once the file column has been re-keyed, and one
// per (from, to) once both have. Merging is the whole point: before v2 the same
// file could hold rows under two spellings, and those are one file's history
// split in half.
struct SymbolRow {
  std::int64_t visits{0};
  double last_ts{0};
  std::int64_t line{0};
};

// The key for a path read out of a v1 table, where the spelling is genuinely
// ambiguous and ProjectKey's rule alone is not enough.
//
// v1 held both spellings at once -- `symbols.file` and `co_visits.to_file` came
// from the file filter (relative to whatever directory koi ran in, possibly in
// an older session), `co_visits.from_file` came from the editor (relative to
// the project root). "sub/file.cpp" could be either, and nothing in the row
// says which. Live writes have no such problem: ProjectKey's contract is "a
// path valid from the current directory", and every caller honours it.
//
// So the tie is broken by the one witness available: which reading names a file
// that is actually there. The cwd reading is tried first because it is the rule
// everything else follows, and the root reading is the fallback that recovers
// the editor's spelling when koi is migrating from a subdirectory. A path that
// is not a file under either reading is dropped -- it was already invisible,
// since every read gates on StillThere.
bool LegacyKey(std::string_view stored, std::string& key) {
  key = ProjectKey(stored);
  if (!key.empty() && StillThere(key)) return true;
  if (!stored.empty() && StillThere(std::string{stored})) {
    key.assign(stored);
    return true;
  }
  return false;
}

// The stamp is read once, before any lock is taken, so two koi processes
// opening the same project at the same moment both decide to migrate: the
// second blocks on BEGIN IMMEDIATE, the first commits, and the second then
// copies everything a second time into tables with no unique index to stop it.
// Re-read the stamp under the write lock and bail.
//
// This works because each migration now stamps its own version inside its own
// transaction (the COMMITs below), so the winner's stamp is visible to the loser
// the instant the lock changes hands. Stamping per step is also what stops a v3
// step that fails from making the v2 step run twice on the next open --
// ProjectKey is not idempotent below the root.
bool AlreadyMigrated(sqlite3* db, std::int64_t version) {
  Stmt stmt{db, "PRAGMA user_version;"};
  return stmt && stmt.Step() && (stmt.Integer(0) >= version);
}

// Rewrites every stored path into the one spelling ProjectKey defines, and
// throws away what no longer resolves.
//
// Read-everything-then-rewrite rather than an UPDATE per row: the merge is not
// expressible as an UPDATE (two rows collapsing into one, summing visits and
// keeping the later timestamp, would hit the primary key), the tables are a few
// hundred rows on a well-used project, and doing it in memory makes the whole
// thing one DELETE and one pass of inserts inside a single transaction.
//
// The transaction is BEGIN IMMEDIATE so a second koi opening the same database
// at the same moment blocks on the write lock here rather than half-way
// through. Any failure rolls back and the caller must not stamp v2 -- a
// database left with v1 data under a v2 stamp would never be migrated again.
bool MigratePathsToProjectKeys(sqlite3* db, std::string& error) {
  std::string why;
  if (!ExecSql(db, "BEGIN IMMEDIATE;", &why)) {
    error = "cannot migrate project database: " + why;
    return false;
  }
  if (AlreadyMigrated(db, 2)) {
    ExecSql(db, "ROLLBACK;");
    return true;
  }
  const auto fail = [db, &error](std::string what) {
    ExecSql(db, "ROLLBACK;");
    error = "cannot migrate project database: " + std::move(what);
    return false;
  };

  // std::map, not unordered: the insert order below is then the same on every
  // run, which is what makes a failed migration reproducible.
  std::map<std::pair<std::string, std::string>, SymbolRow> symbols;
  {
    Stmt read{db, "SELECT file, symbol, visits, last_ts, line FROM symbols;"};
    if (!read) return fail("cannot read symbols");
    while (read.Step()) {
      const std::string symbol = read.Column(1);
      // A path that no longer names a file under the root is a row no reader
      // could ever have used: every read gates on StillThere.
      std::string file;
      if (symbol.empty() || !LegacyKey(read.Column(0), file)) continue;
      SymbolRow& into = symbols[{file, symbol}];
      into.visits += read.Integer(2);
      const double ts = read.Double(3);
      const std::int64_t line = read.Integer(4);
      if (ts > into.last_ts) {
        into.last_ts = ts;
        if (line > 0) into.line = line;
      }
      // A known line beats no line whichever row it came from -- a row with
      // line 0 is invisible to HotSymbols.
      if ((into.line == 0) && (line > 0)) into.line = line;
    }
  }

  std::map<std::pair<std::string, std::string>, std::int64_t> co_visits;
  {
    Stmt read{db, "SELECT from_file, to_file, count FROM co_visits;"};
    if (!read) return fail("cannot read co_visits");
    while (read.Step()) {
      std::string from;
      std::string to;
      if (!LegacyKey(read.Column(0), from) || !LegacyKey(read.Column(1), to)) continue;
      // The two columns arrived in two different spellings, so a file
      // co-visiting itself was invisible to v1's `==`. It is visible now.
      if (from == to) continue;
      co_visits[{from, to}] += read.Integer(2);
    }
  }

  if (!ExecSql(db, "DELETE FROM symbols; DELETE FROM co_visits;", &why)) {
    return fail(std::move(why));
  }

  {
    Stmt write{db, "INSERT INTO symbols(file, symbol, visits, last_ts, line) VALUES(?1,?2,?3,?4,?5);"};
    if (!write) return fail("cannot write symbols");
    for (const auto& [key, row] : symbols) {
      write.Reset();
      write.Text(1, key.first);
      write.Text(2, key.second);
      write.Int(3, row.visits);
      write.Real(4, row.last_ts);
      write.Int(5, row.line);
      if (!write.Run()) return fail("cannot write symbols");
    }
  }
  {
    Stmt write{db, "INSERT INTO co_visits(from_file, to_file, count) VALUES(?1,?2,?3);"};
    if (!write) return fail("cannot write co_visits");
    for (const auto& [key, count] : co_visits) {
      write.Reset();
      write.Text(1, key.first);
      write.Text(2, key.second);
      write.Int(3, count);
      if (!write.Run()) return fail("cannot write co_visits");
    }
  }

  if (!ExecSql(db, "PRAGMA user_version = 2; COMMIT;", &why)) return fail(std::move(why));
  return true;
}

// v2 pinned file+line+col, v3 pins the file. The line and column are dropped
// rather than carried anywhere: they were a raw line number that no edit ever
// adjusted, so the ones worth keeping are indistinguishable from the ones that
// had rotted, and where the file was last left is a better answer than either.
//
// Two v2 pins in the same file collapse into one v3 pin -- the lower slot wins,
// which is what the ORDER BY is for -- and the higher slot is left empty rather
// than backfilled. Deciding what the user meant to put there is not this
// function's business.
//
// A database with no `pins` table is not an error: that is every database
// created by this build, and every one that has already run this migration. The
// prepare fails, there is nothing to read, and the DROP below is a no-op.
bool MigratePinsToFilePins(sqlite3* db, std::string& error) {
  std::string why;
  if (!ExecSql(db, "BEGIN IMMEDIATE;", &why)) {
    error = "cannot migrate project database: " + why;
    return false;
  }
  if (AlreadyMigrated(db, 3)) {
    ExecSql(db, "ROLLBACK;");
    return true;
  }
  const auto fail = [db, &error](std::string what) {
    ExecSql(db, "ROLLBACK;");
    error = "cannot migrate project database: " + std::move(what);
    return false;
  };

  std::vector<std::pair<std::int64_t, std::string>> pins;
  {
    Stmt read{db, "SELECT slot, file FROM pins ORDER BY slot;"};
    if (read) {
      std::unordered_set<std::string> seen;
      while (read.Step()) {
        std::string file = read.Column(1);
        if (file.empty() || !seen.insert(file).second) continue;
        pins.emplace_back(read.Integer(0), std::move(file));
      }
    }
  }

  {
    Stmt write{db, "INSERT OR REPLACE INTO file_pins(slot, file) VALUES(?1,?2);"};
    if (!write && !pins.empty()) return fail("cannot write file_pins");
    for (const auto& [slot, file] : pins) {
      if ((slot < 1) || (slot > kPinSlots)) continue;
      write.Reset();
      write.Int(1, slot);
      write.Text(2, file);
      if (!write.Run()) return fail("cannot write file_pins");
    }
  }

  if (!ExecSql(db, "DROP TABLE IF EXISTS pins;", &why)) return fail(std::move(why));
  if (!ExecSql(db, "PRAGMA user_version = 3; COMMIT;", &why)) return fail(std::move(why));
  return true;
}

// Whether a table already has a column, so an ALTER that has run once does not
// run again. SQLite has no "ADD COLUMN IF NOT EXISTS", and a migration that
// cannot be replayed is one bad open away from never completing.
bool HasColumn(sqlite3* db, const char* table, std::string_view column) {
  const std::string sql = "PRAGMA table_info(" + std::string{table} + ");";
  Stmt stmt{db, sql.c_str()};
  while (stmt.Step()) {
    if (stmt.Column(1) == column) return true;
  }
  return false;
}

// ALTER TABLE ADD COLUMN, asked twice. Two koi processes opening the same
// project can both find the column missing, and the loser's ALTER comes back
// "duplicate column name" -- which is the answer it wanted, not a failure. The
// second HasColumn is what tells the two apart.
bool AddColumn(sqlite3* db, const char* table, std::string_view column, const char* sql,
               std::string* why) {
  if (HasColumn(db, table, column)) return true;
  if (ExecSql(db, sql, why)) return true;
  return HasColumn(db, table, column);
}

// v5's column, added here as well as by the v5 step: a v3 database whose
// `locations` was created by an older v4 build has the table without it, and
// the v4 import below writes to it. ADD COLUMN with a default is transactional
// in SQLite, so this may run either side of a BEGIN.
bool AddCountedTs(sqlite3* db, std::string* why) {
  return AddColumn(db, "locations", "counted_ts",
                   "ALTER TABLE locations ADD COLUMN counted_ts REAL NOT NULL DEFAULT 0;", why);
}

// One row per place, newest wins: two absolute paths (a symlink, a directory
// renamed under koi) can key to one, and "one row per place" is what the merge
// rule downstream assumes. `seq` is the old `id`, so the order the list steps
// through is exactly the order it had.
struct Copied {
  std::string path;
  std::int64_t seq{0};  // the old id
  std::int64_t line{0};
  std::int64_t col{0};
  double ts{0};
};

// ATTACH is not allowed inside a transaction, so it brackets one. A jump
// database that will not attach -- corrupt, gone between the stat and the open,
// from a newer build -- is not a reason to refuse the project store: there is
// simply no jump history to carry over.
bool AttachLegacyJumps(sqlite3* db, const fs::path& legacy) {
  Stmt attach{db, "ATTACH DATABASE ?1 AS jumpdb;"};
  if (!attach) return false;
  attach.Text(1, legacy.string());
  return attach.Run();
}

// Reads the attached v3 jump list into the keyed, deduped form both the v4
// import and the re-import insert. Read first, write second: the dedup needs the
// whole table before it knows which row for a place is the newest.
std::vector<Copied> ReadLegacyJumps(sqlite3* db) {
  std::vector<Copied> rows;
  Stmt read{db, "SELECT id, ts, path, line, col FROM jumpdb.jumps ORDER BY id;"};
  if (!read) return rows;
  std::unordered_map<std::string, std::size_t> newest;
  while (read.Step()) {
    const std::string key = ProjectKey(read.Column(2));
    if (!StorablePath(key)) continue;
    const std::int64_t line = read.Integer(3);
    const std::string place = key + '\0' + std::to_string(line);
    const auto found = newest.find(place);
    const std::size_t at = (found != newest.end()) ? found->second : rows.size();
    if (found == newest.end()) {
      newest.emplace(place, at);
      rows.push_back(Copied{.path = key});
    }
    rows[at].seq = read.Integer(0);
    rows[at].line = line;
    rows[at].col = read.Integer(4);
    rows[at].ts = read.Double(1);
  }
  return rows;
}

// v3 kept the jump list in a second database -- ~/.local/share/koi/<project>/
// state.db -- with absolute paths, its own dedup rule and no idea what a
// project key was. v4 folds it into `locations` here.
//
// What changes on the way in: the path is keyed like every other path in this
// database, and a row whose path is not worth storing is dropped rather than
// carried. What does not: `seq` is the old `id`, and `jump_cursor.at` -- which
// named an id -- names the same number as a seq without being touched. The v6
// step below turns those seqs back into ids, so this one leaves them as they
// are rather than trying to write a row id that is not assigned yet.
//
// The old file is left where it is. It is the only copy of that history, and a
// rename buys nothing a backup does not. ReimportLostJumps below is what that
// copy turned out to be for.
bool MigrateJumpsToLocations(sqlite3* db, std::string& error) {
  std::string why;
  // Before the ATTACH, because a v3 `files` has no `branch` to write to and
  // because the insert below names a column v5 added.
  if (!AddColumn(db, "files", "branch", "ALTER TABLE files ADD COLUMN branch TEXT;", &why) ||
      !AddCountedTs(db, &why)) {
    error = "cannot migrate project database: " + why;
    return false;
  }

  const fs::path legacy = LegacyJumpDbPath();
  std::error_code ec;
  if (legacy.empty() || !fs::is_regular_file(legacy, ec)) return true;
  if (!AttachLegacyJumps(db, legacy)) return true;
  const auto detach = [db] { ExecSql(db, "DETACH DATABASE jumpdb;"); };

  const std::vector<Copied> rows = ReadLegacyJumps(db);

  std::vector<std::pair<std::string, std::int64_t>> cursors;
  {
    Stmt read{db, "SELECT pane, at FROM jumpdb.jump_cursor;"};
    while (read.Step()) cursors.emplace_back(read.Column(0), read.Integer(1));
  }

  if (!ExecSql(db, "BEGIN IMMEDIATE;", &why)) {
    detach();
    error = "cannot migrate project database: " + why;
    return false;
  }
  const auto fail = [db, &detach, &error](std::string what) {
    ExecSql(db, "ROLLBACK;");
    detach();
    error = "cannot migrate project database: " + std::move(what);
    return false;
  };
  // The stamp was read before the lock: another koi may have done all of this
  // while this one waited here. `locations` has no unique index to stop a second
  // copy, so the check has to be this one.
  if (AlreadyMigrated(db, 4)) {
    ExecSql(db, "ROLLBACK;");
    detach();
    return true;
  }

  {
    Stmt write{db,
               "INSERT INTO locations(path, line, col, kind, visits, misses, last_ts,"
               " counted_ts, seq) VALUES(?1,?2,?3,0,1,0,?4,?4,?5);"};
    if (!write && !rows.empty()) return fail("cannot write locations");
    for (const Copied& row : rows) {
      write.Reset();
      write.Text(1, row.path);
      write.Int(2, row.line);
      write.Int(3, row.col);
      write.Real(4, row.ts);
      write.Int(5, row.seq);
      if (!write.Run()) return fail("cannot write locations");
    }
  }
  {
    Stmt write{db, "INSERT OR REPLACE INTO jump_cursor(pane, at) VALUES(?1,?2);"};
    if (!write && !cursors.empty()) return fail("cannot write jump_cursor");
    for (const auto& [pane, at] : cursors) {
      if (pane.empty()) continue;
      write.Reset();
      write.Text(1, pane);
      write.Int(2, at);
      if (!write.Run()) return fail("cannot write jump_cursor");
    }
  }

  if (!ExecSql(db, "PRAGMA user_version = 4; COMMIT;", &why)) return fail(std::move(why));
  detach();
  return true;
}

// v4 measured the visit debounce against `last_ts`, which every touch refreshes,
// so the window slid ahead of itself and no second visit was ever counted. v5
// gives `locations` a column only a counted visit moves.
bool MigrateLocationsCountedTs(sqlite3* db, std::string& error) {
  std::string why;
  if (!ExecSql(db, "BEGIN IMMEDIATE;", &why)) {
    error = "cannot migrate project database: " + why;
    return false;
  }
  if (AlreadyMigrated(db, 5)) {
    ExecSql(db, "ROLLBACK;");
    return true;
  }
  if (!AddCountedTs(db, &why) || !ExecSql(db, "PRAGMA user_version = 5; COMMIT;", &why)) {
    ExecSql(db, "ROLLBACK;");
    error = "cannot migrate project database: " + why;
    return false;
  }
  return true;
}

// v5 kept the jump cursor as a seq and read "am I part-way back through the
// list?" off the store-wide seq counter. Both are wrong once `locations` is
// shared: a merge gives a row a new seq and leaves every other pane's cursor
// naming a number nothing holds (11 of the 23 cursors in the live store), and
// every linger record advances the counter without the list having moved.
//
// v6 keys the cursor by `locations.id`, which a merge does not touch, and gives
// each pane a `walking` flag the jump list owns.
//
// The old seqs are converted where the row is still there. The ones that are
// not are deleted rather than kept: a stale seq is a plausible `id` -- both are
// small integers out of the same table -- so leaving one would silently point
// the pane at whatever row happens to hold that id. A pane with no cursor row
// starts from the front, which is where a pane that lost its place should be.
// The delete therefore has to run before the conversion, or a converted cursor
// looks the same as a stale one.
bool MigrateJumpCursorToIds(sqlite3* db, std::string& error) {
  std::string why;
  if (!ExecSql(db, "BEGIN IMMEDIATE;", &why)) {
    error = "cannot migrate project database: " + why;
    return false;
  }
  if (AlreadyMigrated(db, 6)) {
    ExecSql(db, "ROLLBACK;");
    return true;
  }
  const bool ok =
      AddColumn(db, "jump_cursor", "walking",
                "ALTER TABLE jump_cursor ADD COLUMN walking INTEGER NOT NULL DEFAULT 0;", &why) &&
      ExecSql(db,
              "DELETE FROM jump_cursor WHERE at NOT IN (SELECT seq FROM locations);"
              "UPDATE jump_cursor SET at ="
              " (SELECT id FROM locations WHERE locations.seq = jump_cursor.at);"
              "PRAGMA user_version = 6; COMMIT;",
              &why);
  if (!ok) {
    ExecSql(db, "ROLLBACK;");
    error = "cannot migrate project database: " + why;
    return false;
  }
  return true;
}

// How empty the migrated range has to be before the rows are considered lost:
// fewer than half the places the old list held still present under the highest
// migrated seq. A store that kept its import is left alone.
constexpr std::int64_t kReimportShare = 2;

// The imported jump list used to be deleted by the aging pass in the same open
// that wrote it -- every row arrives at 1 visit, and the pass culled everything
// under 1. The old database is still on disk, so the rows can be put back.
//
// Not gated on the schema stamp: a store that lost them is already v4 or later,
// so `found < 4` will never fire for it again. The gates are a `meta` flag,
// which makes this one-shot, and the migrated seq range being nearly empty,
// which is what the loss looks like from here.
//
// Nothing is inserted twice. A place whose row survived -- a jump cursor stood
// on it, or the user has been back since -- is matched either by its seq or by
// its (path, line), and the insert says so itself rather than trusting a count.
void ReimportLostJumps(sqlite3* db) {
  const auto reimported = [db] {
    Stmt flag{db, "SELECT COUNT(*) FROM meta WHERE key = 'jumps_reimported';"};
    return !flag || !flag.Step() || (flag.Integer(0) > 0);
  };
  if (reimported()) return;

  const fs::path legacy = LegacyJumpDbPath();
  std::error_code ec;
  if (legacy.empty() || !fs::is_regular_file(legacy, ec)) return;
  if (!AttachLegacyJumps(db, legacy)) return;
  const auto detach = [db] { ExecSql(db, "DETACH DATABASE jumpdb;"); };

  const std::vector<Copied> rows = ReadLegacyJumps(db);
  if (rows.empty()) {
    detach();
    return;
  }
  std::int64_t high = 0;
  for (const Copied& row : rows) high = std::max(high, row.seq);

  if (!ExecSql(db, "BEGIN IMMEDIATE;")) {
    detach();
    return;
  }
  const auto give_up = [db, &detach] {
    ExecSql(db, "ROLLBACK;");
    detach();
  };
  // Re-read under the lock, the same shape the migrations use: two panes
  // opening at once both arrive here, and the second must find the flag set.
  if (reimported()) {
    give_up();
    return;
  }

  std::int64_t present = 0;
  {
    Stmt count{db, "SELECT COUNT(*) FROM locations WHERE seq <= ?1;"};
    if (!count) {
      give_up();
      return;
    }
    count.Int(1, high);
    if (!count.Step()) {
      give_up();
      return;
    }
    present = count.Integer(0);
  }

  if ((present * kReimportShare) < static_cast<std::int64_t>(rows.size())) {
    Stmt write{db,
               "INSERT INTO locations(path, line, col, kind, visits, misses, last_ts,"
               " counted_ts, seq) SELECT ?1,?2,?3,0,1,0,?4,?4,?5"
               " WHERE NOT EXISTS (SELECT 1 FROM locations WHERE seq = ?5)"
               "   AND NOT EXISTS (SELECT 1 FROM locations WHERE path = ?1 AND line = ?2);"};
    if (!write) {
      give_up();
      return;
    }
    for (const Copied& row : rows) {
      write.Reset();
      write.Text(1, row.path);
      write.Int(2, row.line);
      write.Int(3, row.col);
      write.Real(4, row.ts);
      write.Int(5, row.seq);
      if (!write.Run()) {
        give_up();
        return;
      }
    }
  }

  // Set whichever way the decision went: the answer does not change on the next
  // open, and the flag is what keeps this off the open path for ever after.
  {
    Stmt flag{db,
              "INSERT INTO meta(key, value) VALUES('jumps_reimported', '1')"
              " ON CONFLICT(key) DO UPDATE SET value = excluded.value;"};
    if (!flag || !flag.Run()) {
      give_up();
      return;
    }
  }
  if (!ExecSql(db, "COMMIT;")) ExecSql(db, "ROLLBACK;");
  detach();
}

// Rows written before the gate existed -- ~70% of the live `files` table, and
// every worktree copy in `symbols`. Deleted by what they name, never by whether
// the file is on disk this second: a branch switch makes half a repository
// missing, and history that deletes itself over one is worth nothing on the way
// back.
void PruneUnstorablePaths(sqlite3* db) {
  struct Table {
    const char* read;
    const char* drop;
  };
  static constexpr Table kTables[] = {
      {"SELECT DISTINCT path FROM files;", "DELETE FROM files WHERE path = ?1;"},
      {"SELECT DISTINCT file FROM symbols;", "DELETE FROM symbols WHERE file = ?1;"},
      {"SELECT DISTINCT path FROM locations;", "DELETE FROM locations WHERE path = ?1;"},
  };
  std::vector<std::pair<const char*, std::string>> bad;
  for (const Table& table : kTables) {
    Stmt read{db, table.read};
    while (read.Step()) {
      std::string path = read.Column(0);
      if (!StorablePath(path)) bad.emplace_back(table.drop, std::move(path));
    }
  }
  // Names the gate above would refuse today: rows the old recorder wrote.
  // Symbol rows die with the name; a location only loses the name, because the
  // place is still real and healable.
  {
    Stmt read{db, "SELECT DISTINCT symbol FROM symbols;"};
    while (read.Step()) {
      std::string name = read.Column(0);
      if (!StorableSymbolName(name)) {
        bad.emplace_back("DELETE FROM symbols WHERE symbol = ?1;", std::move(name));
      }
    }
  }
  {
    Stmt read{db, "SELECT DISTINCT symbol FROM locations WHERE symbol IS NOT NULL;"};
    while (read.Step()) {
      std::string name = read.Column(0);
      if (!StorableSymbolName(name)) {
        bad.emplace_back("UPDATE locations SET symbol = NULL WHERE symbol = ?1;", std::move(name));
      }
    }
  }
  if (bad.empty()) return;
  // One transaction for the lot. Unchecked like the other prunes: a store that
  // cannot delete is still a store worth having.
  if (!ExecSql(db, "BEGIN IMMEDIATE;")) return;
  for (const auto& [sql, path] : bad) {
    Stmt drop{db, sql};
    if (!drop) continue;
    drop.Text(1, path);
    drop.Run();
  }
  if (!ExecSql(db, "COMMIT;")) ExecSql(db, "ROLLBACK;");
}

// A confirmed pair drops out of `queries` once its decayed count falls under
// 0.1 -- the design's threshold, and the reason nothing here needs an expiry of
// its own.
//
// Not one DELETE, because the decay is a read-time rule and the SQL for it wants
// pow(), which SQLite has only when it was built with the math functions. The
// table is small enough that reading it costs nothing.
//
// The WHERE is a sufficient prefilter rather than the rule itself. A write
// leaves `use_count` at 1 or above (0 * 0.9 + 1), so the fastest a row can reach
// 0.1 is log(0.1)/log(0.975) ~ 91 days; anything newer cannot qualify and is
// never looked at. In the steady state this reads no rows at all.
void PruneQueries(sqlite3* db) {
  constexpr double kEarliestDrop = 90.0 * 86400.0;
  const double now = Now();
  std::vector<std::pair<std::string, std::string>> dead;
  {
    Stmt read{db, "SELECT prefix, target, use_count, last_ts FROM queries WHERE last_ts < ?1;"};
    if (!read) return;
    read.Real(1, now - kEarliestDrop);
    while (read.Step()) {
      if (DecayedUse(read.Double(2), read.Double(3), now) >= kQueryDropBelow) continue;
      dead.emplace_back(read.Column(0), read.Column(1));
    }
  }
  if (dead.empty()) return;
  // Unchecked like the prunes beside it, and the ROLLBACK is not optional: a
  // statement that fails partway leaves the write lock held for the session.
  if (!ExecSql(db, "BEGIN IMMEDIATE;")) return;
  Stmt drop{db, "DELETE FROM queries WHERE prefix = ?1 AND target = ?2;"};
  for (const auto& [prefix, target] : dead) {
    if (!drop) break;
    drop.Reset();
    drop.Text(1, prefix);
    drop.Text(2, target);
    drop.Run();
  }
  if (!ExecSql(db, "COMMIT;")) ExecSql(db, "ROLLBACK;");
}

// The event that drives the aging is an open, which is the only moment the
// whole store is in hand and nothing is reading it. The gate is read first
// rather than written into the UPDATE's WHERE: the sum it tests is the very
// thing the first row updated would change.
//
// Unchecked, like the prunes beside it -- a store that cannot age is still a
// store. The ROLLBACK is not optional though: a statement that fails partway
// leaves the transaction open, and a connection sitting on the write lock for
// the rest of the session locks out every other pane.
void AgeStore(sqlite3* db) {
  // Unlocked first pass: the common open has nothing to age, and taking the
  // write lock to find that out would block every other pane for the length of
  // three SUMs.
  bool heavy = false;
  for (const AgeTable& table : kAgeTables) {
    Stmt weight{db, table.total};
    if (weight && weight.Step() && (weight.Double(0) > kAgeThreshold)) heavy = true;
  }
  if (!heavy) return;

  if (!ExecSql(db, "BEGIN IMMEDIATE;")) return;
  for (const AgeTable& table : kAgeTables) {
    // Re-read under the lock: another pane may have aged this table between the
    // pass above and here, and scaling twice on one open is the thing the
    // single-pass factor exists to avoid.
    //
    // Scoped, so the read is finalised before the write begins: a statement that
    // has stepped and not been reset is still walking the table the UPDATE below
    // rewrites.
    double total = 0;
    {
      Stmt weight{db, table.total};
      if (!weight || !weight.Step()) continue;
      total = weight.Double(0);
    }
    if (total <= kAgeThreshold) continue;
    {
      Stmt scale{db, table.scale};
      if (!scale) continue;
      // Land on the threshold when that costs less than kAgeScale, so a table
      // barely over is not taken 10% under for nothing; otherwise take the
      // fixed step and let the next open take another.
      scale.Real(1, std::max(kAgeThreshold / total, kAgeScale));
      if (!scale.Run()) continue;
    }
    ExecSql(db, table.cull);
  }
  if (!ExecSql(db, "COMMIT;")) ExecSql(db, "ROLLBACK;");
}

fs::path DataHome() {
  if (const char* home = std::getenv("HOME"); (home != nullptr) && (*home != '\0')) {
    return fs::path{home} / ".local" / "share";
  }
  return {};
}

std::string CanonicalProjectPath(const fs::path& project) {
  std::error_code ec;
  fs::path canon = fs::weakly_canonical(project, ec);
  if (ec || canon.empty()) canon = project.lexically_normal();
  std::string text = canon.string();
  while ((text.size() > 1) && (text.back() == fs::path::preferred_separator)) text.pop_back();
  return text;
}

std::string ShortDigest(std::string_view text) {
  std::uint64_t h = 14695981039346656037ull;
  for (const char c : text) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  std::string out(8, '0');
  for (std::size_t i = 8; i > 0; --i) {
    out[i - 1] = "0123456789abcdef"[h & 0xfu];
    h >>= 4;
  }
  return out;
}

// Take over the state directory written before the names carried a digest.
//
// The legacy name is the bare FlattenPathComponent, and that is not injective:
// /w/a/b and /w/a-b both look up "w-a-b". The only test available here is "my
// digest directory does not exist yet and the legacy name is a directory",
// which is true for *whichever* of two colliding projects opens first after the
// upgrade -- so a move handed one project the other's entire database (pins,
// visits, last positions) and left the rightful owner with nothing, silently and
// permanently, since a move is one-shot.
//
// So copy, and never destroy the legacy claim. A colliding project seeds from
// the same history instead of emptying it, and the true owner still adopts its
// own state the first time it runs. The cost is one stale directory on disk,
// from before the upgrade, that nothing reads again.
void AdoptLegacyProjectDir(const fs::path& legacy, const fs::path& dir) {
  if (legacy == dir) return;
  // A project path that flattens to nothing (root, "/") makes the legacy name
  // the shared base directory, which *contains* `dir`. Copying that into itself
  // is not adoption.
  if (legacy.filename().empty()) return;
  std::error_code ec;
  if (fs::exists(dir, ec)) return;
  if (!fs::is_directory(legacy, ec)) return;
  fs::copy(legacy, dir, fs::copy_options::recursive, ec);
  if (ec) {
    // A copy that stopped halfway -- a full disk -- would open as a half-empty
    // database that looks like a real one. Drop it so the next launch retries;
    // the legacy directory is untouched either way. (A torn copy is also
    // possible if another koi is writing the legacy database as we read it,
    // which rename had no answer for either; the corrupt-database recycling in
    // OpenAndCheckDatabase turns that into a fresh empty database inside the
    // new directory rather than a broken one.)
    std::error_code cleanup;
    fs::remove_all(dir, cleanup);
  }
}

// The name is memoised; the adoption probe is not, and the split is deliberate.
//
// Deriving the name is the expensive half and it cannot change: canonicalising
// the project path is a stat per component of it, then an FNV pass over the
// result, then a flatten, then two path concatenations -- all to produce one
// string that depends on nothing but the root and $HOME. Every call did the
// whole of it, and the callers are not rare: LastPickerStatePath on each picker
// open *and* close, the keylog path, and ProjectDbPath itself.
//
// AdoptLegacyProjectDir stays on every call because its answer *can* change: it
// is gated on "my state directory does not exist yet", and in the steady state
// that is a single stat that returns true and stops. Caching that away would
// mean a state directory removed under a running koi -- which is what the
// adoption test does, and what a user clearing ~/.local/share does -- never
// being seeded again for the rest of the session.
//
// Plain statics, no mutex: this runs on koi's one UI thread. The scan pool is
// handed everything it needs by value before a worker starts (see ScanInputs),
// and nothing else here is threaded. A guard would be free of contention and
// still be a claim about the code that is not true.
fs::path ProjectDir() {
  const fs::path home = DataHome();
  const fs::path project = ProjectRoot();
  if (home.empty() || project.empty()) return {};

  // Keyed on both inputs, so SetProjectRoot -- the test suite's way of moving
  // between projects, and the only way the root changes at runtime -- is its
  // own invalidation, and so is a HOME that moves under the process.
  static fs::path memo_home;
  static fs::path memo_project;
  static fs::path memo_legacy;
  static fs::path memo_dir;
  if (memo_dir.empty() || (memo_home != home) || (memo_project != project)) {
    const fs::path base = home / "ronin";
    memo_legacy = base / FlattenPathComponent(project.string());
    memo_dir = base / ProjectDirName(project);
    memo_home = home;
    memo_project = project;
  }
  AdoptLegacyProjectDir(memo_legacy, memo_dir);
  return memo_dir;
}

}

std::string ProjectDirName(const fs::path& project) {
  const std::string canon = CanonicalProjectPath(project);
  std::string label = FlattenPathComponent(canon);
  // The label is one directory-entry name, and Linux caps a single entry at
  // NAME_MAX (255) bytes no matter how much PATH_MAX budget remains. A project
  // deep enough to flatten past that made create_directories fail ENAMETOOLONG,
  // and since every state file hangs off this one directory, that was the
  // database, jump list, key log and picker state all gone at once. Keep the tail -- it is the distinctive part -- and let the digest,
  // which is always of the full canonical path, carry the uniqueness.
  constexpr std::size_t kMaxLabel = 200;
  if (label.size() > kMaxLabel) {
    label.erase(0, label.size() - kMaxLabel);
    while (!label.empty() && (label.front() == '-')) label.erase(0, 1);
  }
  return label + "-" + ShortDigest(canon);
}

std::string FlattenPathComponent(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  for (const char c : path) {
    const bool keep = ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
                      ((c >= '0') && (c <= '9'));
    if (keep) {
      out += c;
    } else if (!out.empty() && (out.back() != '-')) {
      out += '-';
    }
  }
  while (!out.empty() && (out.back() == '-')) out.pop_back();
  return out;
}

namespace {

fs::path g_project_db;
fs::path g_project_root;

}

// The marker is tested *before* the stop is honoured, and that order is the
// whole of the rule for one arrangement: a git repository rooted at $HOME --
// dotfiles, which is how a great many people keep $HOME.
//
// Breaking on `at == stop` first meant $HOME/.git was the one marker the walk
// could never see. Launched from anywhere below it the repository was invisible
// and the fallback made each directory its own project, so the state -- pins,
// visits, last positions, jump list -- fragmented into a new database per
// directory the user happened to cd into; launched from $HOME itself the same
// fallback returned $HOME. One repository, a different root depending on how
// deep you were standing, and no way to notice except that nothing was ever
// remembered.
//
// The stop still does its job, which is to stop the walk from climbing *above*
// $HOME: /home and / are not projects, and a stray .git in either would
// otherwise capture every directory on the machine. Testing the marker first
// only adds the boundary directory itself to what the walk may claim.
fs::path FindProjectRoot(const fs::path& from, const fs::path& stop) {
  std::error_code ec;
  for (fs::path at = from; !at.empty(); at = at.parent_path()) {
    if (fs::exists(at / ".git", ec) || fs::exists(at / ".ronin", ec)) return at;
    if (!stop.empty() && (at == stop)) break;
    if (at == at.parent_path()) break;
  }
  return from;
}

fs::path ProjectRoot() {
  if (!g_project_root.empty()) return g_project_root;
  static const fs::path root = [] {
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (ec) return fs::path{};

    fs::path stop;
    if (const char* home = std::getenv("HOME"); (home != nullptr) && (*home != '\0')) {
      stop = fs::weakly_canonical(fs::path{home}, ec);
      if (ec) stop.clear();
    }
    return FindProjectRoot(cwd, stop);
  }();
  return root;
}

// The inverse of the keying rule, and the other half of it.
//
// ProjectKey's contract is "a path given to the store is a path valid from the
// current directory", and what it stores is that path expressed against the
// root. Reading a row back therefore hands out a spelling that is valid from
// the *root*, and every consumer that goes on to open or stat one -- a pin jump,
// the excerpt refs a pins view is built from, the hot file list a symbol scan
// reads -- resolves it against the current directory
// instead. Started at the root the two are the same string and nothing was
// ever wrong; started one directory down, "src/main.cpp" read back from inside
// src/ opened "src/src/main.cpp", which is nothing.
//
// So this turns a key back into what the rest of the editor deals in: a path
// valid from here. `root / key` is where the file actually is (the rule
// StillThere already resolves rows by), and expressing that from the current
// directory is what makes the answer interchangeable with a path the file
// filter produced or the user typed -- which matters, because these paths are
// not only opened: they are deduplicated against scan results and displayed in
// excerpt headers. At the root it returns the key unchanged, so nothing that
// was already right can move.
//
// A key that is absolute -- a file outside the project, which has no
// root-relative spelling -- is already the answer and comes back untouched.
// That is the one sense in which this is idempotent: a *relative* result must
// not be fed back in, since it is relative to the current directory and not to
// the root. Resolve once, at the boundary where store data leaves the store.
std::string ResolveStorePath(const fs::path& root, std::string_view key) {
  if (key.empty()) return {};
  const fs::path stored{key};
  if (root.empty() || stored.is_absolute()) return std::string{key};
  const fs::path full = root / stored;
  std::error_code ec;
  const fs::path here = fs::current_path(ec);
  if (ec) return full.string();
  const fs::path from_here = fs::relative(full, here, ec);
  if (ec || from_here.empty()) return full.string();
  return from_here.string();
}

std::string ResolveStorePath(std::string_view key) { return ResolveStorePath(ProjectRoot(), key); }

void SetProjectDbPath(fs::path path) { g_project_db = std::move(path); }

void SetProjectRoot(fs::path path) { g_project_root = std::move(path); }

fs::path ProjectDbPath() {
  if (!g_project_db.empty()) return g_project_db;
  const fs::path dir = ProjectDir();
  return dir.empty() ? fs::path{} : (dir / "state.db");
}

namespace {

fs::path BesideDatabase(const char* name) {
  const fs::path db = ProjectDbPath();
  return db.empty() ? fs::path{} : (db.parent_path() / name);
}

}

// Derived here rather than in jumplist.cpp, which no longer opens a database at
// all. The rule is the one the jump list used: the project *root*, not the
// working directory, and ProjectDirName rather than a bare flatten -- keying on
// the cwd gave one project a jump list per directory koi was started from, and
// the bare flatten is the non-injective name that had /w/a-b and /w/a/b sharing
// a state directory.
fs::path LegacyJumpDbPath() {
  const fs::path home = DataHome();
  const fs::path root = ProjectRoot();
  if (home.empty() || root.empty()) return {};
  const std::string project = ProjectDirName(root);
  if (project.empty()) return {};
  return home / "koi" / project / "state.db";
}

fs::path LastPickerStatePath() { return BesideDatabase("koi-last-picker.txt"); }

fs::path KeyLogDbPath() { return BesideDatabase("keylog.db"); }

std::shared_ptr<ProjectStore> ProjectStore::Open(const fs::path& path, std::string& error) {
  error.clear();
  if (path.empty()) {
    error = "no project database path";
    return nullptr;
  }
  auto store = std::make_shared<SqliteProject>();
  // Read the header before writing anything: an unusable database has to fail
  // the open, not come back as a store that drops every write in silence. It
  // also means the DDL below never runs against a database this build would
  // then refuse for being too new.
  //
  // This may also come back true with `error` set -- the file was corrupt and
  // has been moved aside, so `store` is a working but empty database. Nothing
  // below writes to `error` on the way to success, so that warning survives to
  // the caller.
  std::int64_t found = 0;
  if (!OpenAndCheckDatabase(path, store->db, kSchemaVersion, error, &found)) return nullptr;
  store->Exec("PRAGMA synchronous = NORMAL;");
  std::string why;
  if (!ExecSql(store->db, kSchema, &why)) {
    error = "cannot create project tables: " + why;
    return nullptr;
  }
  // v1 stored two spellings of the same path: root-relative from the editor,
  // cwd-relative from the file filter. v2 stores one. v3 replaced pinned
  // positions with pinned files. v4 takes in the jump list that used to live in
  // a database of its own. Each rewrite runs once, gated on the stamp below,
  // and only after the DDL -- a database that was empty a moment ago has the
  // tables the migrations read.
  //
  // The v4 step runs for a database this build just created, too, and that is
  // deliberate: a project with jump history and no project store yet is the
  // same import, and it is the only chance to make it.
  //
  // The v1 rewrite leaves `files` and the pins alone. Both were only ever
  // written from the editor's own root-relative spelling, so they are already
  // what v2 wants; rows in `files` naming a path that no longer exists are a
  // separate question, and the readers already skip them.
  //
  // A migration that fails leaves the stamp where it was on purpose: the next
  // open tries again. Stamping over half-rewritten rows would mean never trying
  // again.
  //
  // Each step is gated on the version it upgrades *from*, not on
  // kSchemaVersion. Gating both on the latest would re-run the v1 path-rekeying
  // against a v2 database, which keys already-keyed paths a second time
  // (ProjectKey is not idempotent below the root) and would quietly corrupt
  // every row it touched.
  if ((found < 2) && !MigratePathsToProjectKeys(store->db, error)) return nullptr;
  if ((found < 3) && !MigratePinsToFilePins(store->db, error)) return nullptr;
  if ((found < 4) && !MigrateJumpsToLocations(store->db, error)) return nullptr;
  if ((found < 5) && !MigrateLocationsCountedTs(store->db, error)) return nullptr;
  if ((found < 6) && !MigrateJumpCursorToIds(store->db, error)) return nullptr;
  if (!StampSchemaVersion(store->db, kSchemaVersion, error)) return nullptr;
  // Repair, not migration, and gated on its own flag rather than on the stamp:
  // the stores that need it are already stamped past the import that lost their
  // rows. Before the prunes and the aging below, so the restored rows are held
  // to the same rules as any other.
  ReimportLostJumps(store->db);
  // The only thing that ever bounded `files` and `symbols` was how much the
  // user had done, and that only goes up. Once per open, not per write: a
  // session's worth of visits cannot outgrow either cap by more than a session,
  // each statement is one transaction, and putting them here means the tables
  // the readers see are already the size they will stay.
  //
  // Deliberately unchecked. A store that cannot prune -- a read-only database,
  // a full disk -- is still a store worth having, and StampSchemaVersion above
  // has already refused the databases that cannot be written to at all. The WAL
  // grows by the deleted pages until the next checkpoint, which is what WAL
  // does and what the next open reuses.
  store->Exec(kPruneFiles);
  store->Exec(kPruneSymbols);
  store->Exec(kPruneLocations);
  // `queries` prunes by what a row is worth rather than by how many there are:
  // its decay is the expiry, and this is where it is collected.
  PruneQueries(store->db);
  // Aging is the other half of "prune on open", and the one that is about
  // weight rather than about row counts. Same unchecked contract.
  AgeStore(store->db);
  // Last, so it also sees whatever the migration above just copied in.
  PruneUnstorablePaths(store->db);
  return store;
}

}

#undef KOI_MULT_SQL
#undef KOI_BRANCH_SQL
#undef KOI_MAX_FILE_ROWS
#undef KOI_MAX_SYMBOL_ROWS
#undef KOI_MAX_LOCATION_ROWS
#undef KOI_AGE_FLOOR
