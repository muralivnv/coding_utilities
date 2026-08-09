#include "jumplist.h"

#include <sqlite3.h>
#include <unistd.h>

#include <cstdlib>
#include <ctime>
#include <string>
#include <system_error>

#include "project.h"
#include "sqlite.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

constexpr int kPruneEvery = 128;
constexpr int kKeepRows = 2000;

struct SqliteStore final : JumpStore {
  sqlite3* db{nullptr};
  std::string pane;
  int since_prune{0};

  ~SqliteStore() override {
    if (db != nullptr) sqlite3_close(db);
  }

  void Exec(const char* sql) { ExecSql(db, sql); }

  sqlite3_int64 Cursor() {
    Stmt stmt{db, "SELECT at FROM jump_cursor WHERE pane=?;"};
    if (!stmt) return 0;
    stmt.Text(1, pane);
    return stmt.Step() ? stmt.Integer(0) : 0;
  }

  // Returns whether the cursor actually moved. Every caller runs inside a
  // transaction that has to roll back if it did not: a cursor left behind is a
  // list the user steps through from the wrong place.
  bool SetCursor(sqlite3_int64 at) {
    Stmt stmt{db, "INSERT INTO jump_cursor(pane,at) VALUES(?,?)"
                  " ON CONFLICT(pane) DO UPDATE SET at=excluded.at;"};
    if (!stmt) return false;
    stmt.Text(1, pane);
    stmt.Int(2, at);
    return stmt.Run();
  }

  // Closes the transaction the caller opened, and says whether the work in it
  // survived. Same shape as ProjectStore::SetPin and the project migration:
  // COMMIT when everything ran, ROLLBACK otherwise -- and ROLLBACK again when
  // the COMMIT itself failed, because a COMMIT that returns an error (SQLITE_BUSY
  // is the usual one) leaves the transaction open, and a store sitting on the
  // write lock for the rest of the session locks out every other pane.
  bool Finish(bool ok) {
    if (ok && ExecSql(db, "COMMIT;")) return true;
    ExecSql(db, "ROLLBACK;");
    return false;
  }

  sqlite3_int64 NewestId() {
    Stmt stmt{db, "SELECT COALESCE(MAX(id),0) FROM jumps;"};
    if (!stmt) return 0;
    return stmt.Step() ? stmt.Integer(0) : 0;
  }

  bool RowNear(sqlite3_int64 from, bool forward, Jump& out, sqlite3_int64& id) {
    Stmt stmt{db,
              forward ? "SELECT id,path,line,col FROM jumps WHERE id>? ORDER BY id ASC LIMIT 1;"
                      : "SELECT id,path,line,col FROM jumps WHERE id<? ORDER BY id DESC LIMIT 1;"};
    if (!stmt) return false;
    stmt.Int(1, from);
    if (!stmt.Step()) return false;
    id = stmt.Integer(0);
    out.path = stmt.Column(1);
    out.line = stmt.Integer(2);
    out.col = stmt.Integer(3);
    return !out.path.empty();
  }

  // Recording a jump is one change to the jump list, not four, so it is one
  // transaction.
  //
  // The statements are a chain, and each one is wrong without the ones after
  // it: the read that decides whether this place is already the newest row, the
  // DELETE that takes the same file+line out of wherever it sat before, the
  // INSERT that puts it back at the end, and the cursor move onto the row just
  // written. Run unwrapped -- which is what every bare statement is, its own
  // implicit transaction -- each commits alone, and every gap between them is a
  // state no reader should ever see: the jump gone from the list entirely
  // (after the DELETE, before the INSERT), or present with the cursor still
  // pointing at the row it replaced. A crash lands in those gaps, an INSERT
  // that fails for any reason lands in them, and so does a second koi on the
  // same database -- multi-instance is the normal way this is used, each pane
  // with its own PaneId -- whose own dedup DELETE for the same place can commit
  // between this one's DELETE and INSERT.
  //
  // BEGIN IMMEDIATE, the same as SetPin and the project migration: it takes the
  // write lock up front so the other koi blocks on it (busy_timeout, five
  // seconds) instead of interleaving with it, and if the lock cannot be taken
  // nothing runs at all -- the list stays exactly as it was, which is the one
  // outcome worth having when the change cannot be made whole.
  void Record(const fs::path& path, Index line, Index col) override {
    if ((db == nullptr) || path.empty()) return;
    std::error_code ec;
    const fs::path absolute = fs::weakly_canonical(path, ec);
    const std::string text = (ec ? path : absolute).string();

    if (!ExecSql(db, "BEGIN IMMEDIATE;")) return;

    // Predicted rather than counted: `since_prune` is in-memory state that has
    // to agree with the database, so it only advances once the transaction it
    // belongs to has committed. A rolled back Record changed nothing and must
    // not consume a step of the prune cadence either.
    const bool prune_due = (since_prune + 1) >= kPruneEvery;
    bool inserted = false;
    bool ok = false;
    // Scoped so every statement is finalised before the COMMIT below, and so
    // none outlives the transaction it belongs to.
    {
      Jump newest;
      sqlite3_int64 newest_id = 0;
      if (RowNear(NewestId() + 1, false, newest, newest_id) && (newest.path == text) &&
          (newest.line == line)) {
        // Already the newest row: the whole change is the cursor move.
        ok = SetCursor(newest_id);
      } else {
        Stmt del{db, "DELETE FROM jumps WHERE path=? AND line=?;"};
        Stmt insert{db, "INSERT INTO jumps(ts,pane,path,line,col) VALUES(?,?,?,?,?);"};
        if (del && insert) {
          del.Text(1, text);
          del.Int(2, line);
          insert.Int(1, static_cast<sqlite3_int64>(::time(nullptr)));
          insert.Text(2, pane);
          insert.Text(3, text);
          insert.Int(4, line);
          insert.Int(5, col);
          // All three, in order, and all three have to have run. A DELETE that
          // failed is a duplicate left in the list; an INSERT that failed with
          // the DELETE committed is the jump the user just made, lost; a cursor
          // that failed to move points at the row the INSERT replaced.
          ok = del.Run() && insert.Run() && SetCursor(sqlite3_last_insert_rowid(db));
          inserted = ok;
        }
      }

      // Inside the same transaction as the insert that triggered it: after the
      // COMMIT it would be a second write window of its own. Its result is
      // deliberately not folded into `ok` -- trimming the tail is housekeeping,
      // and failing to trim is no reason to throw away the jump.
      if (inserted && prune_due) {
        const std::string prune =
            "DELETE FROM jumps WHERE id NOT IN (SELECT id FROM jumps ORDER BY id DESC LIMIT " +
            std::to_string(kKeepRows) + ");";
        Exec(prune.c_str());
      }
    }

    if (Finish(ok) && inserted) since_prune = prune_due ? 0 : (since_prune + 1);
  }

  // Read the cursor, find the row beside it, move the cursor onto that row: a
  // read-modify-write, and the row reported to the caller has to be the row the
  // cursor ends on. Unwrapped, another pane's Record can commit between the
  // read and the write and dedup that very row away, leaving this pane's cursor
  // on an id that no longer names anything while the caller is sent to it.
  bool Step(bool forward, Jump& out) override {
    if (db == nullptr) return false;
    if (!ExecSql(db, "BEGIN IMMEDIATE;")) return false;

    bool ok = false;
    Jump found;
    {
      sqlite3_int64 cur = Cursor();
      // No cursor and stepping forward is "already at the newest": there is
      // nothing after the end. Backwards it means "start from past the end".
      if ((cur != 0) || !forward) {
        if (cur == 0) cur = NewestId() + 1;
        sqlite3_int64 id = 0;
        if (RowNear(cur, forward, found, id)) ok = SetCursor(id);
      }
    }

    if (!Finish(ok)) return false;
    // Only on the committed path, so a caller that is told to jump is never
    // told it by a transaction that rolled back.
    out = found;
    return true;
  }

  bool AtNewest() override {
    if (db == nullptr) return true;
    // Two reads whose comparison is the answer, so they have to see one
    // database. Between them another pane can append a row, and the caller
    // acts on the answer: StepJump records the current place before stepping
    // back only when this says yes, so a wrong no loses the position the user
    // would have come back to. Deferred BEGIN, not IMMEDIATE -- nothing here
    // writes, and this must not take the write lock away from a pane that does.
    const bool snapshot = ExecSql(db, "BEGIN;");
    const sqlite3_int64 cur = Cursor();
    const bool newest = (cur == 0) || (cur >= NewestId());
    if (snapshot) ExecSql(db, "COMMIT;");
    return newest;
  }

  int Count() override {
    Stmt stmt{db, "SELECT COUNT(*) FROM jumps;"};
    if (!stmt) return 0;
    return stmt.Step() ? static_cast<int>(stmt.Integer(0)) : 0;
  }
};

}

std::shared_ptr<JumpStore> JumpStore::Open(const fs::path& path, std::string pane,
                                           std::string& error) {
  if (path.empty()) {
    error = "no data directory";
    return nullptr;
  }
  auto store = std::make_shared<SqliteStore>();
  store->pane = std::move(pane);
  // The version read comes first: it is the only step that tells an unusable
  // database apart from a fresh one, and the migration below needs the version
  // it found anyway. A true return with `error` set is the warning case: the
  // file was corrupt and has been replaced by an empty one.
  std::int64_t version = 0;
  if (!OpenAndCheckDatabase(path, store->db, 1, error, &version)) return nullptr;
  store->Exec("PRAGMA synchronous=OFF;");
  std::string why;
  const bool tables =
      ExecSql(store->db,
              "CREATE TABLE IF NOT EXISTS jumps("
              " id INTEGER PRIMARY KEY AUTOINCREMENT,"
              " ts INTEGER NOT NULL,"
              " pane TEXT NOT NULL,"
              " path TEXT NOT NULL,"
              " line INTEGER NOT NULL,"
              " col INTEGER NOT NULL);",
              &why) &&
      ExecSql(store->db,
              "CREATE TABLE IF NOT EXISTS jump_cursor(pane TEXT PRIMARY KEY, at INTEGER NOT NULL);",
              &why);
  if (!tables) {
    error = "cannot create jump list tables: " + why;
    return nullptr;
  }
  if (version < 1) store->Exec("DELETE FROM jump_cursor;");
  if (!StampSchemaVersion(store->db, 1, error)) return nullptr;
  return store;
}

fs::path JumpDbPath() {
  const char* home = std::getenv("HOME");
  if ((home == nullptr) || (*home == '\0')) return {};
  const fs::path base = fs::path{home} / ".local" / "share";

  // The project root, not the working directory, and ProjectDirName rather than
  // a bare flatten. Both matter and neither is cosmetic: keying on the cwd gave
  // one project a different jump list per directory koi was started from, and
  // the bare flatten is the same non-injective name that had `/w/a-b` and
  // `/w/a/b` sharing a state directory -- fixed for the project database and
  // missed here, which is why this is the one derivation left that could put two
  // projects' jumps in one file.
  const fs::path root = ProjectRoot();
  if (root.empty()) return {};
  const std::string project = ProjectDirName(root);
  if (project.empty()) return {};
  return base / "koi" / project / "state.db";
}

std::string PaneId() {
  if (const char* pane = std::getenv("TMUX_PANE")) {
    if (*pane != '\0') return pane;
  }
  return "pid-" + std::to_string(static_cast<long long>(::getpid()));
}

}
