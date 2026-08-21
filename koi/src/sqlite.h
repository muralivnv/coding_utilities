#ifndef KOI_SQLITE_H_
#define KOI_SQLITE_H_

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace koi {

// Returns false, and fills `error` when one is asked for, if the statement did
// not run. Callers that pass no `error` are saying "a failure here is not worth
// reporting"; callers opening a database are not among them -- a discarded
// failure at open time is what turns an unusable database into a store that
// accepts every write and keeps none.
inline bool ExecSql(sqlite3* db, const char* sql, std::string* error = nullptr) {
  if (db == nullptr) {
    if (error != nullptr) *error = "no database";
    return false;
  }
  char* msg = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) == SQLITE_OK) return true;
  if (error != nullptr) *error = (msg != nullptr) ? msg : "sql failed";
  sqlite3_free(msg);
  return false;
}

inline bool OpenDatabase(const std::filesystem::path& path, sqlite3*& db, std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    error = "cannot create " + path.parent_path().string() + ": " + ec.message();
    return false;
  }
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    error = "cannot open " + path.string() + ": " +
            ((db != nullptr) ? sqlite3_errmsg(db) : "out of memory");
    // sqlite allocates the handle whether or not the open succeeds, and the
    // message above is the last thing that needs it. Callers treat `false` as
    // "there is no database" and never look at `db` again, so closing it here
    // is the only chance.
    sqlite3_close(db);
    db = nullptr;
    return false;
  }
  ExecSql(db, "PRAGMA journal_mode = WAL;");
  ExecSql(db, "PRAGMA busy_timeout = 5000;");
  return true;
}

struct Stmt {
  sqlite3_stmt* handle{nullptr};

  Stmt(sqlite3* db, const char* sql) {
    if (db != nullptr) sqlite3_prepare_v2(db, sql, -1, &handle, nullptr);
  }
  ~Stmt() {
    if (handle != nullptr) sqlite3_finalize(handle);
  }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  explicit operator bool() const { return handle != nullptr; }

  // A statement that would not prepare -- a corrupt file, a schema older than
  // this build, a database that never opened -- degrades to a no-op rather than
  // taking the editor down with it. That is the contract the callers are
  // written against, and it used to hold only because SQLite happens to
  // null-check most of its own entry points. It does not check them all:
  // sqlite3_clear_bindings reads pStmt->db->mutex before anything else, so
  // Reset() on an unprepared statement was a null dereference. Guarding here
  // makes the property belong to this class instead of to SQLite's argument
  // handling, where the next accessor added would silently not inherit it.
  void Text(int at, std::string_view value) {
    if (handle == nullptr) return;
    sqlite3_bind_text(handle, at, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
  }
  void Int(int at, std::int64_t value) {
    if (handle == nullptr) return;
    sqlite3_bind_int64(handle, at, value);
  }
  void Real(int at, double value) {
    if (handle == nullptr) return;
    sqlite3_bind_double(handle, at, value);
  }
  void Null(int at) {
    if (handle == nullptr) return;
    sqlite3_bind_null(handle, at);
  }

  bool Step() { return (handle != nullptr) && (sqlite3_step(handle) == SQLITE_ROW); }

  // True when the statement actually ran. Callers are free to ignore it -- a
  // failed write at runtime still degrades to a no-op, because ProjectStore has
  // no channel to report one on -- but the answer exists now, so a caller that
  // grows one can use it.
  bool Run() {
    if (handle == nullptr) return false;
    const int rc = sqlite3_step(handle);
    return (rc == SQLITE_DONE) || (rc == SQLITE_ROW);
  }

  void Reset() {
    if (handle == nullptr) return;
    sqlite3_reset(handle);
    sqlite3_clear_bindings(handle);
  }

  std::string Column(int at) {
    if (handle == nullptr) return {};
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(handle, at));
    return (text == nullptr) ? std::string{} : std::string{text};
  }
  std::int64_t Integer(int at) { return (handle == nullptr) ? 0 : sqlite3_column_int64(handle, at); }
  double Double(int at) { return (handle == nullptr) ? 0.0 : sqlite3_column_double(handle, at); }
};

enum class Schema { kUsable, kUnreadable, kTooNew, kCorrupt };

// True only for the two answers that mean "these bytes are not a database and
// no amount of waiting will make them one": SQLITE_NOTADB (the header is not
// SQLite's) and SQLITE_CORRUPT (it is, and the pages under it disagree with
// themselves). Everything else -- BUSY, LOCKED, READONLY, CANTOPEN, the whole
// IOERR family -- describes the world around the file, not the file, and a
// healthy database can produce any of them; those must never reach the recycler.
//
// SQLite builds an extended code as `primary | (n << 8)`, so masking to the low
// byte turns SQLITE_CORRUPT_VTAB, _INDEX and _SEQUENCE back into SQLITE_CORRUPT
// without having to name each one, and does the same for codes added later. The
// unmasked comparison is kept beside it so the plain codes are still matched if
// a build ever hands them back untouched.
inline bool IsCorruptionCode(int rc) {
  const int primary = rc & 0xFF;
  return (primary == SQLITE_CORRUPT) || (primary == SQLITE_NOTADB) || (rc == SQLITE_CORRUPT) ||
         (rc == SQLITE_NOTADB);
}

// Decides whether this build may use the database, and hands back the version
// it found. It does not stamp: stamping belongs after the schema DDL has
// succeeded (see StampSchemaVersion), because a database stamped before its
// tables exist would claim to be current while holding nothing.
//
// This is also the first statement that reads the file at all. sqlite3_open()
// only records the name -- it never touches the header -- so a truncated write,
// a file that is not a database, a directory that cannot be locked all arrive
// here, and only here, as a "cannot read" answer. Treating that as success is
// what produced a store that swallowed every pin and every visit for a whole
// session without a word.
inline Schema CheckSchemaVersion(sqlite3* db, std::int64_t expected, std::string& error,
                                 std::int64_t* found = nullptr) {
  Stmt stmt{db, "PRAGMA user_version;"};
  if (!stmt || !stmt.Step()) {
    // Stmt's constructor drops the prepare result and Step() folds every
    // non-row answer into `false`, so the reason is not in either return value.
    // It is still on the connection: SQLite records the last failure there, and
    // the extended form is the one that distinguishes SQLITE_CORRUPT_VTAB from
    // a plain SQLITE_ERROR. Reading it here, right after the call that failed
    // and before `stmt` is finalised, is the only place the answer is still
    // fresh -- and telling corrupt apart from busy is what stands between
    // "start a new database" and "throw away a database another koi is using".
    const int rc = (db != nullptr) ? sqlite3_extended_errcode(db) : SQLITE_OK;
    const bool corrupt = (db != nullptr) && IsCorruptionCode(rc);
    error = std::string{corrupt ? "corrupt database: " : "unreadable database: "} +
            ((db != nullptr) ? sqlite3_errmsg(db) : "no database");
    return corrupt ? Schema::kCorrupt : Schema::kUnreadable;
  }
  const std::int64_t version = stmt.Integer(0);
  if (found != nullptr) *found = version;
  if (version > expected) {
    error = "database schema v" + std::to_string(version) + " is newer than this koi (v" +
            std::to_string(expected) + ")";
    return Schema::kTooNew;
  }
  return Schema::kUsable;
}

// Stamps unconditionally, even when the version already matches. Rewriting one
// header field per open is nothing, and it is the only write every open is
// guaranteed to attempt -- which makes it the probe that catches a database
// that reads fine and cannot be written. `CREATE TABLE IF NOT EXISTS` does not:
// on a read-only database whose tables already exist it returns SQLITE_OK
// without touching anything, so the schema step alone would wave a read-only
// database through into a session whose every write is discarded.
inline bool StampSchemaVersion(sqlite3* db, std::int64_t expected, std::string& error) {
  const std::string stamp = "PRAGMA user_version = " + std::to_string(expected) + ";";
  std::string why;
  if (ExecSql(db, stamp.c_str(), &why)) return true;
  error = "cannot write to database: " + why;
  return false;
}

// Moves a database SQLite has declared corrupt out of the way, so that the next
// open creates a new one in its place.
//
// It renames; it never deletes the database itself. The bytes are the only copy
// of whatever the user's editor remembered, a later SQLite or a human with a
// hex editor may recover more from them than this build can, and a rename is
// the one operation that cannot lose them by being wrong. `<db>.corrupt` is a
// fixed name rather than a timestamped one on purpose: a database that keeps
// corrupting would otherwise fill the state directory with corpses, and on
// POSIX rename() replaces an existing regular file, so the latest one wins.
//
// The write-ahead log and its shared-memory index are the exception, and they
// are removed rather than moved: they describe the file that just left. Left in
// place, the fresh database created under the old name would adopt them and be
// handed back the very corruption it exists to escape.
inline bool RecycleCorruptDatabase(const std::filesystem::path& path, std::string& error) {
  std::filesystem::path aside = path;
  aside += ".corrupt";
  std::error_code ec;
  std::filesystem::rename(path, aside, ec);
  if (ec) {
    error = "cannot move " + path.string() + " aside to " + aside.string() + ": " + ec.message();
    return false;
  }
  for (const std::string_view suffix : {"-wal", "-shm"}) {
    std::filesystem::path sibling = path;
    sibling += suffix;
    std::error_code ignored;
    // Derived data, and the database they belonged to is still on disk under
    // its new name, so a failure to remove one costs nothing worth reporting.
    std::filesystem::remove(sibling, ignored);
  }
  return true;
}

// Opens the database and gates on the version it finds, so a caller that gets
// `true` has a handle it can read and a schema this build understands.
//
// `error` carries two different meanings, and the return value plus the handle
// say which:
//
//   returns false                  -- `error` is why there is no database.
//   returns true, `error` empty    -- the usual case, nothing to say.
//   returns true, `error` NON-EMPTY -- a WARNING, not a failure: the database
//                                     was corrupt, has been moved aside, and
//                                     the handle is a brand new empty one. The
//                                     session works; the old state is gone.
//
// Reusing one string keeps every `Open()` in the editor at its current
// signature while still giving the caller something to put on the status line.
// The callers that only branch on the handle stay correct by ignoring it.
//
// Only Schema::kCorrupt takes the recycling path. That restriction is the whole
// safety argument: a file that will not read *this second* may be a perfectly
// healthy database that a second koi holds locked, that is mid-recovery from a
// rollback journal, or that sits on a filesystem which has gone away. BUSY,
// LOCKED, READONLY, CANTOPEN and every IOERR are reported and the file is not
// touched. A database from a newer koi is likewise kept -- it reads fine, and
// that build is still using it.
inline bool OpenAndCheckDatabase(const std::filesystem::path& path, sqlite3*& db,
                                 std::int64_t expected, std::string& error,
                                 std::int64_t* version = nullptr) {
  error.clear();
  if (!OpenDatabase(path, db, error)) return false;
  const Schema found = CheckSchemaVersion(db, expected, error, version);
  if (found == Schema::kUsable) return true;
  if (found != Schema::kCorrupt) return false;

  const std::string corruption = error;
  // The handle has the dead file open. Close it before the rename so no part of
  // this process is still writing through it, and null it so a caller that
  // ignores the return value cannot reach it.
  sqlite3_close(db);
  db = nullptr;

  std::string why;
  if (!RecycleCorruptDatabase(path, why)) {
    // Both halves of the story: what was wrong with the file, and why it is
    // still there. There is no fall-through to deleting it and none to using
    // it -- a corrupt database that cannot be moved is simply no database.
    error = corruption + "; " + why;
    return false;
  }

  if (!OpenDatabase(path, db, error)) return false;
  // No second recycle. If a file this call just created already reads as
  // corrupt, the fault is not a stale database, and looping would move a fresh
  // corpse aside on every open while never reporting anything.
  if (CheckSchemaVersion(db, expected, error, version) != Schema::kUsable) return false;
  // Phrased as a clause with no subject so each caller can name the thing that
  // was lost -- "project state ...", "jump list ...", "key recording ...".
  error = "was corrupt -- started fresh (old file kept beside it as " +
          path.filename().string() + ".corrupt)";
  return true;
}

}

#endif
