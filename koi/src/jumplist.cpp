#include "jumplist.h"

#include <sqlite3.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <system_error>
#include <utility>

#include "project.h"
#include "sqlite.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

// Out of a key, and back to a path. The mirror of LocationKey: a key that names
// no file is the view name it went in as, and resolving one against the root
// would spell it into a path no buffer answers to.
std::string JumpPath(const fs::path& root, const std::string& key) {
  if (fs::path{key}.is_absolute()) return key;
  std::string resolved = ResolveStorePath(root, key);
  std::error_code ec;
  return fs::exists(resolved, ec) ? resolved : key;
}

// Since v4 the jump list is a view over `locations`, and `seq` is the order it
// walks. Two things follow, and both are the point of the move:
//
// Recording a place already in the list no longer deletes it. v3 kept recency
// by DELETE + INSERT, which threw away the row's visit count -- every one of
// the 238 rows measured in the live list had visits of exactly one -- and the
// DELETE was not pane-scoped, so one pane erased another's row. A merge takes
// the next seq instead: same move-to-front, nothing destroyed, and nothing of
// another pane's to destroy.
//
// The rows are shared and the cursor is per pane, which is what "pane-scoped
// history" means here: two panes recording the same place merge onto one row,
// and each still steps through the list from where it is.
struct SqliteJumps final : JumpStore {
  // Held for the connection: the store owns it, and this must not outlive it.
  std::shared_ptr<ProjectStore> project;
  sqlite3* db{nullptr};
  std::string pane;

  // Where the pane is, and whether it got there by stepping. `at` is a row id,
  // which a merge does not change; `seq` is that row's place in the walk, read
  // through the join, so a row that merges forward carries the cursors on it.
  //
  // A cursor whose row is gone joins to nothing and comes back empty, which is
  // the same answer as never having had one: start from the front, not mid-walk.
  struct At {
    sqlite3_int64 id{0};
    sqlite3_int64 seq{0};
    bool walking{false};
  };

  At Cursor() {
    At at;
    Stmt stmt{db, "SELECT c.at, l.seq, c.walking FROM jump_cursor c"
                  " JOIN locations l ON l.id = c.at WHERE c.pane=?;"};
    if (!stmt) return at;
    stmt.Text(1, pane);
    if (!stmt.Step()) return at;
    at.id = stmt.Integer(0);
    at.seq = stmt.Integer(1);
    at.walking = stmt.Integer(2) != 0;
    return at;
  }

  // Returns whether the cursor actually moved. Every caller runs inside a
  // transaction that has to roll back if it did not: a cursor left behind is a
  // list the user steps through from the wrong place.
  //
  // `walking` is what "part-way back through the list" means, and the jump list
  // is the only thing that writes it: recording clears it, a step back sets it,
  // a step forward that reaches the front clears it again.
  bool SetCursor(sqlite3_int64 id, bool walking) {
    Stmt stmt{db, "INSERT INTO jump_cursor(pane,at,walking) VALUES(?,?,?)"
                  " ON CONFLICT(pane) DO UPDATE SET at=excluded.at, walking=excluded.walking;"};
    if (!stmt) return false;
    stmt.Text(1, pane);
    stmt.Int(2, id);
    stmt.Int(3, walking ? 1 : 0);
    return stmt.Run();
  }

  // The row a seq belongs to. WriteLocation answers in seqs -- it is the store's
  // order, not the list's -- and the cursor is kept in ids.
  sqlite3_int64 IdAtSeq(sqlite3_int64 seq) {
    Stmt stmt{db, "SELECT id FROM locations WHERE seq=?;"};
    if (!stmt) return 0;
    stmt.Int(1, seq);
    return stmt.Step() ? stmt.Integer(0) : 0;
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

  sqlite3_int64 NewestSeq() {
    Stmt stmt{db, "SELECT COALESCE(MAX(seq),0) FROM locations;"};
    if (!stmt) return 0;
    return stmt.Step() ? stmt.Integer(0) : 0;
  }

  bool RowNear(sqlite3_int64 from, bool forward, Jump& out, sqlite3_int64& seq) {
    Stmt stmt{
        db, forward
                ? "SELECT seq,path,line,col,id FROM locations WHERE seq>? ORDER BY seq ASC LIMIT 1;"
                : "SELECT seq,path,line,col,id FROM locations WHERE seq<? ORDER BY seq DESC LIMIT 1;"};
    if (!stmt) return false;
    stmt.Int(1, from);
    if (!stmt.Step()) return false;
    seq = stmt.Integer(0);
    const std::string key = stmt.Column(1);
    out.path = JumpPath(ProjectRoot(), key);
    out.line = stmt.Integer(2);
    out.col = stmt.Integer(3);
    out.id = stmt.Integer(4);
    return !out.path.empty();
  }

  // Recording a jump is one change to the list, not two, so it is one
  // transaction.
  //
  // The write and the cursor move are a chain and each is wrong without the
  // other: the merge or insert that puts this place at the front, and the
  // cursor move onto the row it wrote. Run unwrapped -- which is what every
  // bare statement is, its own implicit transaction -- each commits alone, and
  // the gap between them is a state no reader should see: a row at the front
  // with the cursor still on the row it passed. A crash lands in that gap, a
  // statement that fails for any reason lands in it, and so does a second koi
  // on the same database, which is the normal way this is used.
  //
  // BEGIN IMMEDIATE, the same as SetPin and the project migration: it takes the
  // write lock up front so the other koi blocks on it (busy_timeout, five
  // seconds) instead of interleaving with it, and if the lock cannot be taken
  // nothing runs at all -- the list stays exactly as it was, which is the one
  // outcome worth having when the change cannot be made whole.
  void Record(const LocationRecord& row) override {
    if ((db == nullptr) || (project == nullptr) || row.path.empty()) return;

    if (!ExecSql(db, "BEGIN IMMEDIATE;")) return;
    bool ok = false;
    // Scoped so every statement is finalised before the COMMIT below, and so
    // none outlives the transaction it belongs to.
    {
      // Inside this transaction, not one of its own -- see WriteLocation.
      const sqlite3_int64 seq = project->WriteLocation(row);
      const sqlite3_int64 id = (seq == 0) ? 0 : IdAtSeq(seq);
      // Recording is the end of any walk: the place just written is the front of
      // the list, and the next step back has a departure to record again.
      ok = (id != 0) && SetCursor(id, false);
    }
    Finish(ok);
  }

  // Read the cursor, find the row beside it, move the cursor onto that row: a
  // read-modify-write, and the row reported to the caller has to be the row the
  // cursor ends on. Unwrapped, another pane's Record can commit between the
  // read and the write and move that very row's seq, leaving this pane's cursor
  // behind while the caller is sent somewhere else.
  bool Step(bool forward, Jump& out) override {
    if (db == nullptr) return false;
    if (!ExecSql(db, "BEGIN IMMEDIATE;")) return false;

    bool ok = false;
    Jump found;
    {
      const At at = Cursor();
      sqlite3_int64 cur = at.seq;
      // No cursor and stepping forward is "already at the newest": there is
      // nothing after the end. Backwards it means "start from past the end".
      if ((cur != 0) || !forward) {
        if (cur == 0) cur = NewestSeq() + 1;
        sqlite3_int64 seq = 0;
        if (RowNear(cur, forward, found, seq)) {
          // Stepping back is what makes a pane mid-walk. Stepping forward onto
          // the front row is the end of the walk -- the pane is where a record
          // would have left it, so the next step back records again.
          const bool walking = !forward || (seq < NewestSeq());
          ok = SetCursor(found.id, walking);
        }
      }
    }

    if (!Finish(ok)) return false;
    // Only on the committed path, so a caller that is told to jump is never
    // told it by a transaction that rolled back.
    out = found;
    return true;
  }

  // Not "is the cursor on the newest row" -- the flag the list itself sets.
  // What this used to compare against was MAX(seq) over all of `locations`,
  // which every linger and every edit record advances without the list having
  // moved: the cursor fell behind a place the user never jumped to, and the
  // caller then skipped recording the place it was about to leave. StepJump
  // records the current place before stepping back only when this says yes.
  //
  // One statement and no transaction around it. The old comparison needed two
  // reads of one snapshot; this is a single row, and a read that takes no lock
  // is a read that cannot hold one off a pane that is writing.
  bool AtNewest() override {
    if (db == nullptr) return true;
    return !Cursor().walking;
  }
};

}

void JumpStore::Record(const fs::path& path, Index line, Index col) {
  LocationRecord row;
  row.path = path.string();
  row.line = line;
  row.col = col;
  row.exact = true;
  Record(row);
}

std::shared_ptr<JumpStore> JumpStore::Open(std::shared_ptr<ProjectStore> project, std::string pane,
                                           std::string& error) {
  error.clear();
  sqlite3* db = (project == nullptr) ? nullptr : project->Connection();
  if (db == nullptr) {
    error = "no project database";
    return nullptr;
  }
  auto store = std::make_shared<SqliteJumps>();
  store->project = std::move(project);
  store->db = db;
  store->pane = std::move(pane);
  return store;
}

std::string PaneId() {
  if (const char* pane = std::getenv("TMUX_PANE")) {
    if (*pane != '\0') return pane;
  }
  return "pid-" + std::to_string(static_cast<long long>(::getpid()));
}

}
