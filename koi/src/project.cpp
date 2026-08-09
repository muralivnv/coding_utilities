#include "project.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "sqlite.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

#define KOI_DECAY_SQL          \
  "CASE"                       \
  " WHEN (?1 - last_ts) / 3600.0 < 1   THEN 1.0" \
  " WHEN (?1 - last_ts) / 3600.0 < 6   THEN 0.8" \
  " WHEN (?1 - last_ts) / 3600.0 < 24  THEN 0.6" \
  " WHEN (?1 - last_ts) / 3600.0 < 168 THEN 0.4" \
  " WHEN (?1 - last_ts) / 3600.0 < 720 THEN 0.2" \
  " ELSE 0.1 END"

constexpr int kMaxCoVisitFiles = 32;

// How many stored rows a bounded read may look at for every row it returns.
//
// The over-fetch is for StillThere: a row whose file has been deleted or
// renamed is dropped in C++, and the query has no way to know that, so it hands
// back more than the caller asked for and the loop stops as soon as it has
// enough. Four-to-one with a floor of 256 fills a twenty-entry trail in one
// query even on a store where three quarters of the history is gone from disk.
//
// Over-fetching is very nearly free: the rows arrive in index order and the
// ones past the cap are never turned into a FileVisit, never copied into a
// std::string, and never stat()ed.
constexpr int kVisitOverFetch = 4;
constexpr int kVisitScanFloor = 256;

// The newest visits kept in `files` at open. Nothing else ever deleted from
// this table -- every file ever opened stayed in it forever, and every read
// scanned and stat()ed all of it -- so an editor left running against a big
// repository grew a permanent tax on the sidebar.
//
// 5000 is far past what anything here distinguishes: the trail shows five, the
// excerpt view twenty, and a picker ranking that reaches 5000 visited files is
// ordering rows the user last touched 5000 other files ago. It is deliberately
// generous, because the cost of keeping a row is one row and the cost of
// dropping one that mattered is a lost cursor position.
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
// nothing distinguishes anything near it: the sidebar shows seven symbols, and a
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

// v1: paths were stored in whatever spelling the writer happened to have.
// v2: every path is a ProjectKey -- relative to the project root, or absolute
//     when the file lies outside it.
//
// A v2 database is refused by a v1 koi, and that is the intent rather than an
// accident to route around: a v1 build would go on writing cwd-relative paths
// into tables the v2 build has just made consistent, and the two would fight
// over every row. Refusing is the only answer that does not corrupt meaning.
constexpr std::int64_t kSchemaVersion = 2;

constexpr const char* kSchema =
    "CREATE TABLE IF NOT EXISTS files ("
    "  path      TEXT    PRIMARY KEY,"
    "  visits    INTEGER NOT NULL DEFAULT 0,"
    "  edits     INTEGER NOT NULL DEFAULT 0,"
    "  last_ts   REAL    NOT NULL DEFAULT 0,"
    "  last_line INTEGER NOT NULL DEFAULT 1,"
    "  last_col  INTEGER NOT NULL DEFAULT 0);"
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
    "CREATE TABLE IF NOT EXISTS pins ("
    "  slot INTEGER PRIMARY KEY,"
    "  file TEXT    NOT NULL,"
    "  line INTEGER NOT NULL DEFAULT 1,"
    "  col  INTEGER NOT NULL DEFAULT 0);"
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
    "CREATE INDEX IF NOT EXISTS files_by_ts ON files(last_ts DESC);";

double Now() {
  const auto since = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration<double>(since).count();
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
// and the Symbols section of the sidebar was blank forever.
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

struct SqliteProject final : ProjectStore {
  sqlite3* db{nullptr};

  ~SqliteProject() override {
    if (db != nullptr) sqlite3_close(db);
  }

  void Exec(const char* sql) { ExecSql(db, sql); }

  void BumpFile(std::string_view raw, Index line, Index column, bool edit) {
    const std::string path = ProjectKey(raw);
    if (path.empty()) return;
#define KOI_BUMP_SQL(field)                                          \
  "INSERT INTO files(path, " field ", last_ts, last_line, last_col)" \
  " VALUES(?1,1,?2,?3,?4)"                                           \
  " ON CONFLICT(path) DO UPDATE SET " field " = " field " + 1,"      \
  " last_ts = excluded.last_ts, last_line = excluded.last_line,"     \
  " last_col = excluded.last_col;"
    Stmt stmt{db, edit ? KOI_BUMP_SQL("edits") : KOI_BUMP_SQL("visits")};
#undef KOI_BUMP_SQL
    if (!stmt) return;
    stmt.Text(1, path);
    stmt.Real(2, Now());
    stmt.Int(3, std::max<Index>(1, line));
    stmt.Int(4, std::max<Index>(0, column));
    stmt.Run();
  }

  void RecordVisit(std::string_view path, Index line, Index column) override {
    BumpFile(path, line, column, false);
  }

  void RecordEdit(std::string_view path, Index line, Index column) override {
    // Both take the caller's spelling, and each keys it once. Handing the key
    // from one to the other would be the same thing; handing a key back into a
    // keying function would not (see ProjectKey).
    BumpFile(path, line, column, true);
    MovePinIn(path, line, column);
  }

  void RecordSymbolVisit(std::string_view symbol, std::string_view raw_file, Index line) override {
    const std::string file = ProjectKey(raw_file);
    if (symbol.empty() || file.empty()) return;
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
    if (from_file.empty() || to_file.empty() || (from_file == to_file)) return;
    Stmt stmt{db,
              "INSERT INTO co_visits(from_file, to_file, count) VALUES(?1,?2,1)"
              " ON CONFLICT(from_file, to_file) DO UPDATE SET count = count + 1;"};
    if (!stmt) return;
    stmt.Text(1, from_file);
    stmt.Text(2, to_file);
    stmt.Run();
  }

  // Both queries take the row cap as ?2, and only the frecency one references
  // ?1. Binding an index the statement does not mention is fine -- SQLite sizes
  // its parameter array by the largest index that appears, and ?2 puts 1 inside
  // it -- so the two share one binder.
  std::vector<FileVisit> Query(const char* sql, bool bind_now, int want) {
    std::vector<FileVisit> out;
    Stmt stmt{db, sql};
    if (!stmt) return out;
    if (bind_now) stmt.Real(1, Now());
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
    return Query("SELECT path, last_line, last_col, last_ts FROM files"
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
  std::vector<FileVisit> FrecentFiles(int want) override {
    return Query(
        "SELECT path, last_line, last_col, last_ts FROM files"
        " ORDER BY (visits + edits * 3) * (" KOI_DECAY_SQL ") DESC LIMIT ?2;",
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
    Stmt stmt{db,
              "SELECT symbol, file, line FROM symbols"
              " WHERE line > 0 AND last_ts > 0 AND visits > 0"
              " ORDER BY visits * (" KOI_DECAY_SQL ") DESC LIMIT ?2;"};
    if (!stmt) return out;
    stmt.Real(1, Now());
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
                "SELECT file FROM symbols"
                " WHERE visits > 0 AND last_ts > 0"
                " ORDER BY visits * (" KOI_DECAY_SQL ") DESC LIMIT ?2;"};
      if (stmt) {
        stmt.Real(1, Now());
        stmt.Int(2, limit);
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
      Stmt stmt{db, "SELECT symbol, visits * (" KOI_DECAY_SQL ") FROM symbols WHERE file = ?2;"};
      if (stmt) {
        const double now = Now();
        std::string joined;
        for (const std::string_view file : files) {
          stmt.Reset();
          stmt.Real(1, now);
          stmt.Text(2, file);
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

  std::vector<Pin> Pins() override {
    std::vector<Pin> out(static_cast<size_t>(kPinSlots));
    Stmt stmt{db, "SELECT slot, file, line, col FROM pins WHERE slot BETWEEN 1 AND ?1;"};
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

  // Pinning a place is one change to the pin table, not two, so it is one
  // transaction.
  //
  // The two statements below are a pair: the DELETE takes the file+line out of
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
  void SetPin(int slot, std::string_view raw_file, Index line, Index column) override {
    const std::string file = ProjectKey(raw_file);
    if ((slot < 1) || (slot > kPinSlots) || file.empty()) return;
    if (!ExecSql(db, "BEGIN IMMEDIATE;")) return;

    bool ok = false;
    // Scoped so both statements are finalised before the COMMIT below, and so
    // neither outlives the transaction it belongs to.
    {
      Stmt drop{db, "DELETE FROM pins WHERE file = ?1 AND line = ?2 AND slot <> ?3;"};
      Stmt put{db, "INSERT OR REPLACE INTO pins(slot, file, line, col) VALUES(?1,?2,?3,?4);"};
      if (drop && put) {
        drop.Text(1, file);
        drop.Int(2, std::max<Index>(1, line));
        drop.Int(3, slot);
        put.Int(1, slot);
        put.Text(2, file);
        put.Int(3, std::max<Index>(1, line));
        put.Int(4, std::max<Index>(0, column));
        // Both, in order, and both have to have run: a DELETE that failed is a
        // duplicate pin left behind, and an INSERT that failed with the DELETE
        // committed is the lost pin this transaction exists to prevent.
        ok = drop.Run() && put.Run();
      }
    }
    ExecSql(db, ok ? "COMMIT;" : "ROLLBACK;");
  }

  void ClearPin(int slot) override {
    Stmt stmt{db, "DELETE FROM pins WHERE slot = ?1;"};
    if (!stmt) return;
    stmt.Int(1, slot);
    stmt.Run();
  }

  int FileCount() override {
    Stmt stmt{db, "SELECT COUNT(*) FROM files;"};
    if (!stmt || !stmt.Step()) return 0;
    return static_cast<int>(stmt.Integer(0));
  }

  void MovePinIn(std::string_view raw, Index line, Index column) {
    const std::string path = ProjectKey(raw);
    if (path.empty()) return;
    Stmt stmt{db,
              "UPDATE pins SET line = ?1, col = ?2"
              " WHERE file = ?3 AND (line <> ?1 OR col <> ?2)"
              " AND (SELECT COUNT(*) FROM pins WHERE file = ?3) = 1;"};
    if (!stmt) return;
    stmt.Int(1, std::max<Index>(1, line));
    stmt.Int(2, std::max<Index>(0, column));
    stmt.Text(3, path);
    stmt.Run();
  }
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

  if (!ExecSql(db, "COMMIT;", &why)) return fail(std::move(why));
  return true;
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
// trail, last positions) and left the rightful owner with nothing, silently and
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
// open *and* close, the keylog path, the sidebar pane path, and ProjectDbPath
// itself.
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
// `koi --sidebar` is its own process, and nothing else here is threaded. A
// guard would be free of contention and still be a claim about the code that
// is not true.
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
  // database, jump list, key log, sidebar pane, and picker state all gone at
  // once. Keep the tail -- it is the distinctive part -- and let the digest,
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
// trail, last positions, jump list -- fragmented into a new database per
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
// the *root*, and every consumer that goes on to open or stat one -- the trail
// jump, a pin jump, the excerpt refs a pins/trail view is built from, the hot
// file list a symbol scan reads -- resolves it against the current directory
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

Trail TrailOf(ProjectStore& store, int entries) {
  Trail trail;
  // Two things eat into the answer before the caller sees it: the newest row
  // becomes `current` rather than an entry, and a pinned file is dropped
  // instead of being shown in two places at once. Asking for that many extra
  // rows means neither can starve a full trail.
  const int want = (entries > 0) ? (entries + 1 + kPinSlots) : 0;
  std::vector<FileVisit> recent = store.RecentFiles(want);
  if (recent.empty()) return trail;
  trail.current = recent.front().path;
  recent.erase(recent.begin());

  const std::vector<Pin> pins = store.Pins();
  std::erase_if(recent, [&pins](const FileVisit& visit) {
    return std::ranges::any_of(pins, [&visit](const Pin& pin) { return pin.path == visit.path; });
  });
  if ((entries > 0) && (std::ssize(recent) > entries)) {
    recent.resize(static_cast<std::size_t>(entries));
  }
  trail.entries = std::move(recent);
  return trail;
}

fs::path LastPickerStatePath() { return BesideDatabase("koi-last-picker.txt"); }

fs::path KeyLogDbPath() { return BesideDatabase("keylog.db"); }

fs::path SidebarPanePath() { return BesideDatabase("sidebar.pane"); }

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
  // cwd-relative from the file filter. v2 stores one. The rewrite runs once,
  // gated on the stamp below, and only after the DDL -- a database that was
  // empty a moment ago has the tables the migration reads.
  //
  // `files` and `pins` are not touched. Both were only ever written from the
  // editor's own root-relative spelling, so they are already what v2 wants;
  // rows in `files` naming a path that no longer exists are a separate
  // question, and the readers already skip them.
  //
  // A migration that fails leaves the stamp at v1 on purpose: the next open
  // tries again. Stamping v2 over half-rewritten rows would mean never trying
  // again.
  if ((found < kSchemaVersion) && !MigratePathsToProjectKeys(store->db, error)) return nullptr;
  if (!StampSchemaVersion(store->db, kSchemaVersion, error)) return nullptr;
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
  return store;
}

}

#undef KOI_DECAY_SQL
#undef KOI_MAX_FILE_ROWS
#undef KOI_MAX_SYMBOL_ROWS
