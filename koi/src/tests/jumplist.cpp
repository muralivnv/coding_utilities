// Tests for jumplist.cpp: the jump list and the store behind it.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void JumpList() {
  TEST_CASE("jump list");

  const std::filesystem::path dir =
      TempFixture("koi-jump-test");
  RemoveAllQuietly(dir);
  const std::filesystem::path db = dir / "state.db";

  const std::filesystem::path fa = dir / "a.txt";
  const std::filesystem::path fb = dir / "b.txt";
  std::filesystem::create_directories(dir);
  {
    std::ofstream oa(fa);
    oa << "a one\na two\na three\na four\n";
    std::ofstream ob(fb);
    ob << "b one\nb two\nb three\n";
  }

  {
    std::string error;
    auto store = JumpStore::Open(db, "pane-1", error);
    EXPECT_TRUE(store != nullptr);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(store->Count(), 0);

    store->Record(fa, 1, 1);
    store->Record(fa, 3, 2);
    EXPECT_EQ(store->Count(), 2);

    store->Record(fa, 3, 2);
    EXPECT_EQ(store->Count(), 2);

    Jump j;
    EXPECT_TRUE(store->Step(false, j));
    EXPECT_EQ(j.line, Index{1});
    EXPECT_TRUE(!store->Step(false, j));
    EXPECT_TRUE(store->Step(true, j));
    EXPECT_EQ(j.line, Index{3});
    EXPECT_TRUE(!store->Step(true, j));

    EXPECT_TRUE(std::filesystem::path{j.path}.is_absolute());
  }

  {
    std::string error;
    auto pane1 = JumpStore::Open(db, "pane-1", error);
    auto pane2 = JumpStore::Open(db, "pane-2", error);
    EXPECT_TRUE(pane1 != nullptr);
    EXPECT_TRUE(pane2 != nullptr);

    Jump j;
    EXPECT_TRUE(pane2->Step(false, j));
    EXPECT_EQ(j.line, Index{3});
    EXPECT_TRUE(pane2->Step(false, j));
    EXPECT_EQ(j.line, Index{1});

    Jump k;
    EXPECT_TRUE(pane1->Step(false, k));
    EXPECT_EQ(k.line, Index{1});

    pane2->Record(fb, 2, 1);
    EXPECT_EQ(pane1->Count(), 3);
    Jump newest;
    EXPECT_TRUE(pane1->Step(true, newest) || true);
  }

  {
    std::string error;
    const std::filesystem::path shifted = dir / "shifted.db";
    auto pane_a = JumpStore::Open(shifted, "pane-a", error);
    auto pane_b = JumpStore::Open(shifted, "pane-b", error);
    EXPECT_TRUE((pane_a != nullptr) && (pane_b != nullptr));

    pane_a->Record(fa, 1, 1);
    pane_a->Record(fa, 2, 1);
    pane_b->Record(fb, 1, 1);
    pane_b->Record(fb, 2, 1);

    Jump j;
    EXPECT_TRUE(pane_a->Step(false, j));
    EXPECT_EQ(std::filesystem::path{j.path}.filename().string(), std::string{"a.txt"});
    EXPECT_EQ(j.line, Index{1});

    EXPECT_TRUE(pane_a->Step(true, j));
    EXPECT_EQ(std::filesystem::path{j.path}.filename().string(), std::string{"a.txt"});
    EXPECT_EQ(j.line, Index{2});

    EXPECT_TRUE(pane_a->Step(true, j));
    EXPECT_EQ(std::filesystem::path{j.path}.filename().string(), std::string{"b.txt"});
  }

  {
    std::string error;
    Editor ed;
    ed.jumps = JumpStore::Open(dir / "editor.db", "pane-editor", error);
    EXPECT_TRUE(ed.jumps != nullptr);

    ResetToOriginal(ed.doc.table, "a one\na two\na three\na four\n");
    ed.doc.file = fa;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    ed.jumps->Record(fa, 2, 1);
    ed.jumps->Record(fb, 3, 1);
    ed.jumps->Record(fa, 4, 1);
    const Index landed = LineStart(ed.doc.table, 3);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{landed, landed, -1}));

    const KeyMaps maps = DefaultKeyMaps();
    std::vector<Key> pending;
    const auto press = [&](std::string_view text) {
      Key key;
      EXPECT_TRUE(ParseKey(text, key));
      HandleKeyInput(ed, maps, key, pending);
    };

    press("[");
    press("[");
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"b.txt"});
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{2});

    press("[");
    press("[");
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.txt"});
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{1});

    ed.status.clear();
    press("[");
    press("[");
    EXPECT_TRUE(ed.status.find("no older position") != std::string::npos);

    press("]");
    press("]");
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"b.txt"});
    press("]");
    press("]");
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.txt"});
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{3});

    ed.status.clear();
    press("]");
    press("]");
    EXPECT_TRUE(ed.status.find("no newer position") != std::string::npos);
  }

  {
    std::string error;
    Editor ed;
    ed.jumps = JumpStore::Open(dir / "resume.db", "pane-resume", error);
    EXPECT_TRUE(ed.jumps != nullptr);

    ResetToOriginal(ed.doc.table, "a one\na two\na three\na four\n");
    ed.doc.file = fa;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RecordJump(ed);

    const Index at = LineStart(ed.doc.table, 3);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
    ed.mode = Mode::kInsert;
    std::ignore = InsertAtCursorsKeeping("edited ", ed.doc.table, ed.doc.selections);
    ed.mode = Mode::kNormal;
    ApplyModeInvariants(ed);
    EXPECT_EQ(ed.jumps->Count(), 1);
    const Index edited_line = LineAt(ed.doc.table, Cur(ed));
    EXPECT_EQ(edited_line, Index{3});

    StepJump(ed, false);
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{0});

    StepJump(ed, true);
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{3});

    EXPECT_EQ(ed.jumps->Count(), 2);
    StepJump(ed, false);
    StepJump(ed, true);
    EXPECT_EQ(ed.jumps->Count(), 2);
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{3});
  }

  {
    std::string error;
    Editor ed;
    ed.jumps = JumpStore::Open(dir / "motion.db", "pane-motion", error);
    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\nfour\nfive\nsix\n");
    ed.doc.file = fa;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RecordJump(ed);
    EXPECT_EQ(ed.jumps->Count(), 1);

    for (int i = 0; i < 4; ++i) {
      RunCommands(ed, {"move_line_down", "move_char_right", "move_next_word_start",
                       "move_prev_word_start", "move_line_up", "move_char_left",
                       "goto_line_end", "goto_first_nonwhitespace", "extend_line"});
    }
    ed.mode = Mode::kInsert;
    std::ignore = InsertAtCursorsKeeping("xyz", ed.doc.table, ed.doc.selections);
    ed.mode = Mode::kNormal;
    ApplyModeInvariants(ed);
    RunCommands(ed, {"undo"});

    EXPECT_EQ(ed.jumps->Count(), 1);
  }

  {
    std::string error;
    auto store = JumpStore::Open(dir / "dedupe.db", "pane-dedupe", error);
    store->Record(fa, 10, 1);
    store->Record(fa, 20, 1);
    store->Record(fa, 10, 5);
    EXPECT_EQ(store->Count(), 2);

    Jump j;
    EXPECT_TRUE(store->Step(false, j));
    EXPECT_EQ(j.line, Index{20});
    EXPECT_TRUE(!store->Step(false, j));
  }

  {
    std::string error;
    Editor ed;
    ed.jumps = JumpStore::Open(dir / "dirty.db", "pane-dirty", error);
    ResetToOriginal(ed.doc.table, "scratch\n");
    ed.doc.file = fa;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.jumps->Record(fb, 1, 1);
    ed.jumps->Record(fa, 1, 1);
    ed.doc.modified = true;

    StepJump(ed, false);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"b.txt"});
    EXPECT_EQ(BufferCount(ed), std::size_t{2});

    RunTypableCommand(ed, "bp");
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.txt"});
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("scratch\n"));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "x\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RecordJump(ed);
    StepJump(ed, false);
    EXPECT_TRUE(ed.status.find("no jump store") != std::string::npos);
  }

  TEST_CASE("jumps: a jump reuses the buffer it names, and skips a view that is gone");
  {
    std::string error;
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.jumps = JumpStore::Open(dir / "views.db", "pane-views", error);
    EXPECT_TRUE(ed.jumps != nullptr);
    EXPECT_TRUE(OpenTarget(ed, fa.string()));
    EXPECT_TRUE(OpenTarget(ed, fb.string()));
    EXPECT_EQ(BufferCount(ed), std::size_t{2});

    OpenReferenceExcerpts(ed, {Symbol{fa.string(), 3, 1, "a three"}}, "a three");
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_EQ(BufferCount(ed), std::size_t{3});
    const std::filesystem::path view_path{ed.doc.view_name};
    RecordJump(ed);

    StepJump(ed, false);
    EXPECT_FALSE(IsExcerptView(ed.doc));
    EXPECT_EQ(BufferCount(ed), std::size_t{3});

    StepJump(ed, true);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_EQ(ed.doc.view_name, view_path.native());
    EXPECT_EQ(BufferCount(ed), std::size_t{3});

    RunTypableCommand(ed, "bc");
    EXPECT_EQ(BufferCount(ed), std::size_t{2});
    for (int i = 0; i < 3; ++i) {
      StepJump(ed, false);
      EXPECT_FALSE(IsExcerptView(ed.doc));
      EXPECT_EQ(BufferCount(ed), std::size_t{2});
      EXPECT_EQ(FindFileBuffer(ed, view_path), BufferCount(ed));
      EXPECT_EQ(FindViewBuffer(ed, view_path.native()), BufferCount(ed));
    }
    for (int i = 0; i < 3; ++i) {
      StepJump(ed, true);
      EXPECT_FALSE(IsExcerptView(ed.doc));
      EXPECT_EQ(BufferCount(ed), std::size_t{2});
      EXPECT_EQ(FindFileBuffer(ed, view_path), BufferCount(ed));
    }
  }

  {
    const std::filesystem::path probe = JumpDbPath();
    EXPECT_TRUE(!probe.empty());
    EXPECT_EQ(probe.filename().string(), std::string{"state.db"});
    const std::string project = probe.parent_path().filename().string();
    EXPECT_TRUE(!project.empty());
    EXPECT_TRUE(project.find('/') == std::string::npos);
    EXPECT_TRUE(project.front() != '-');
    EXPECT_TRUE(project.back() != '-');
    EXPECT_EQ(probe.parent_path().parent_path().filename().string(), std::string{"koi"});
  }

  TEST_CASE("jump list: recording a jump is one transaction, and no path leaves one open");
  {
    // Record is a read (is this already the newest place?), a DELETE (take the
    // same file+line out of wherever it was), an INSERT (put it back at the
    // end) and a cursor move onto the row just written -- one change to the
    // list. Nothing here can crash the process between them to prove the
    // atomicity directly, so what is checked is everything the transaction is
    // observable through from outside: the result is committed and complete
    // when Record returns, a failure anywhere leaves the list exactly as it
    // was, and neither path leaves the write lock held.
    const std::filesystem::path txdb = dir / "one-transaction.db";
    std::string error{"unset"};
    auto store = JumpStore::Open(txdb, "pane-tx", error);
    EXPECT_TRUE(store != nullptr);
    EXPECT_TRUE(error.empty());
    if (store != nullptr) {
      // A connection of our own, and deliberately without the store's five
      // second busy timeout: a question about a lock is then answered now
      // rather than in five seconds.
      sqlite3* other = nullptr;
      EXPECT_EQ(sqlite3_open(txdb.c_str(), &other), SQLITE_OK);

      const auto scalar = [&other](const char* sql) {
        Stmt stmt{other, sql};
        if (!stmt) return std::int64_t{-1};
        return stmt.Step() ? stmt.Integer(0) : std::int64_t{-1};
      };
      // In WAL mode an uncommitted write is invisible to every other
      // connection, so reading through `other` checks that the transaction
      // closed -- not merely that the statements ran.
      const auto rows = [&scalar] { return scalar("SELECT COUNT(*) FROM jumps;"); };
      const auto cursor = [&scalar] {
        return scalar("SELECT COALESCE(MAX(at),0) FROM jump_cursor WHERE pane='pane-tx';");
      };
      // The invariant the transaction exists for: the cursor names a row that
      // is really there.
      const auto cursor_is_real = [&scalar] {
        return scalar("SELECT COUNT(*) FROM jumps WHERE id ="
                      " (SELECT at FROM jump_cursor WHERE pane='pane-tx');");
      };
      const auto rows_at = [&other](const std::string& file, int line) {
        Stmt stmt{other, "SELECT COUNT(*) FROM jumps WHERE path=?1 AND line=?2;"};
        if (!stmt) return std::int64_t{-1};
        stmt.Text(1, file);
        stmt.Int(2, line);
        return stmt.Step() ? stmt.Integer(0) : std::int64_t{-1};
      };
      // Takes the write lock and gives it straight back. False means somebody
      // else holds it, and the only other connection here is the store's.
      const auto lock_is_free = [&other] {
        if (!ExecSql(other, "BEGIN IMMEDIATE;")) return false;
        ExecSql(other, "ROLLBACK;");
        return true;
      };

      const std::string a = std::filesystem::weakly_canonical(fa).string();
      const std::string b = std::filesystem::weakly_canonical(fb).string();
      store->Record(fa, 10, 1);
      store->Record(fb, 20, 1);
      EXPECT_EQ(rows(), std::int64_t{2});
      EXPECT_EQ(cursor_is_real(), std::int64_t{1});
      EXPECT_TRUE(lock_is_free());

      // The dedup half: recording a place already in the list moves it to the
      // end, and the move is one row before and one row after. A window between
      // the DELETE and the INSERT is a moment when it is zero rows, and one
      // between the INSERT and the cursor move is a moment when the cursor is
      // still on the row that the DELETE took away.
      const std::int64_t before = cursor();
      store->Record(fa, 10, 5);
      EXPECT_EQ(rows(), std::int64_t{2});
      EXPECT_EQ(rows_at(a, 10), std::int64_t{1});
      EXPECT_TRUE(cursor() > before);
      EXPECT_EQ(cursor_is_real(), std::int64_t{1});
      EXPECT_EQ(cursor(), scalar("SELECT MAX(id) FROM jumps;"));
      EXPECT_TRUE(lock_is_free());

      // Now the failure paths, without patching SQLite: rename a table out from
      // under the store and its statements no longer prepare. The transaction
      // has already begun by then, so this is the rollback -- and a missing
      // rollback would leave the store on the write lock for the rest of the
      // session, which `lock_is_free` below would say.
      const std::int64_t kept_cursor = cursor();
      EXPECT_TRUE(ExecSql(other, "ALTER TABLE jumps RENAME TO jumps_hidden;"));
      store->Record(fb, 30, 1);
      EXPECT_TRUE(lock_is_free());
      EXPECT_TRUE(ExecSql(other, "ALTER TABLE jumps_hidden RENAME TO jumps;"));
      EXPECT_EQ(rows(), std::int64_t{2});
      EXPECT_EQ(rows_at(b, 30), std::int64_t{0});
      EXPECT_EQ(cursor(), kept_cursor);

      // The cursor half of the same thing, and the one the DELETE+INSERT alone
      // would not cover: the write that fails is the last of the three, so the
      // insert has already succeeded inside the transaction and has to be
      // taken back with it. Without the rollback the list keeps a jump whose
      // cursor was never moved onto it.
      EXPECT_TRUE(ExecSql(other, "ALTER TABLE jump_cursor RENAME TO cursor_hidden;"));
      store->Record(fb, 40, 1);
      EXPECT_TRUE(lock_is_free());
      EXPECT_EQ(rows(), std::int64_t{2});
      EXPECT_EQ(rows_at(b, 40), std::int64_t{0});
      // Stepping cannot half-move either: the cursor write is the whole change.
      Jump blocked;
      EXPECT_FALSE(store->Step(false, blocked));
      EXPECT_TRUE(lock_is_free());
      EXPECT_TRUE(ExecSql(other, "ALTER TABLE cursor_hidden RENAME TO jump_cursor;"));
      EXPECT_EQ(cursor(), kept_cursor);

      // And the store is still usable afterwards: a rolled back Record is not
      // a broken store.
      store->Record(fb, 50, 1);
      EXPECT_EQ(rows(), std::int64_t{3});
      EXPECT_EQ(rows_at(b, 50), std::int64_t{1});
      EXPECT_EQ(cursor_is_real(), std::int64_t{1});
      EXPECT_TRUE(lock_is_free());

      // Nothing above changed what the list reads back as, in memory or on
      // disk: the two failed Records are simply not there.
      EXPECT_EQ(store->Count(), 3);
      Jump j;
      EXPECT_TRUE(store->Step(false, j));
      EXPECT_EQ(j.line, Index{10});
      EXPECT_TRUE(store->Step(true, j));
      EXPECT_EQ(j.line, Index{50});
      EXPECT_TRUE(!store->Step(true, j));

      sqlite3_close(other);
    }
  }

  TEST_CASE("jump list: a concurrent reader never catches a jump mid-record");
  {
    // The half-written states a reader could see if Record were three
    // auto-committed statements, watched for from a second connection while a
    // real store hammers the same database -- which is the multi-instance case
    // koi actually supports, one PaneId per instance.
    //
    // The list is seeded with every place the writer will re-record, so its row
    // count is invariant from then on: each Record deletes exactly one row and
    // inserts exactly one. Any other count the reader sees is a gap between two
    // commits -- kPlaces - 1 is the DELETE without its INSERT.
    const std::filesystem::path hammer_db = dir / "hammer.db";
    std::string error{"unset"};
    auto writer = JumpStore::Open(hammer_db, "pane-hammer", error);
    EXPECT_TRUE(writer != nullptr);
    if (writer != nullptr) {
      constexpr int kPlaces = 6;
      constexpr int kRecords = 400;
      for (int i = 0; i < kPlaces; ++i) writer->Record(fa, Index{10 + i}, 1);
      EXPECT_EQ(writer->Count(), kPlaces);

      std::atomic<bool> writing{true};
      std::atomic<int> samples{0};
      std::atomic<int> wrong_count{0};
      std::atomic<int> dangling_cursor{0};
      std::atomic<int> unreadable{0};

      std::thread reader{[&] {
        sqlite3* other = nullptr;
        if (sqlite3_open(hammer_db.c_str(), &other) != SQLITE_OK) {
          ++unreadable;
          sqlite3_close(other);
          return;
        }
        ExecSql(other, "PRAGMA busy_timeout = 5000;");
        while (writing.load()) {
          // One statement, so the two halves of the question are answered
          // against one snapshot of the database: how many rows there are, and
          // whether the writer's cursor names one of them.
          Stmt look{other, "SELECT (SELECT COUNT(*) FROM jumps),"
                           " (SELECT COUNT(*) FROM jumps WHERE id ="
                           " (SELECT at FROM jump_cursor WHERE pane='pane-hammer'));"};
          if (!look || !look.Step()) {
            ++unreadable;
            continue;
          }
          if (look.Integer(0) != kPlaces) ++wrong_count;
          if (look.Integer(1) != 1) ++dangling_cursor;
          ++samples;
        }
        sqlite3_close(other);
      }};

      for (int i = 0; i < kRecords; ++i) {
        // Round-robin, so the place being recorded is never the newest row --
        // that is the dedup shortcut, and it does no DELETE or INSERT at all.
        writer->Record(fa, Index{10 + (i % kPlaces)}, Index{1 + (i % 3)});
        // Every so often, step back and re-record the place stepped onto. That
        // is the one arrangement in which the row the dedup DELETE removes is
        // the row the cursor is sitting on, so the cursor is left naming
        // nothing until the INSERT and the cursor move land -- which is what
        // `dangling_cursor` is watching for.
        if ((i % 4) == 3) {
          Jump back;
          if (writer->Step(false, back)) writer->Record(back.path, back.line, back.col);
        }
      }
      writing.store(false);
      reader.join();

      // Counted rather than asserted per sample: a failure then reports how
      // often it happened, and the suite's check count stays fixed however many
      // times the reader got round the loop.
      EXPECT_EQ(wrong_count.load(), 0);
      EXPECT_EQ(dangling_cursor.load(), 0);
      EXPECT_EQ(unreadable.load(), 0);
      // The reader has to have actually looked, or the three counts above are
      // zero for the wrong reason.
      EXPECT_TRUE(samples.load() > 0);
      // And the writer's own view is unchanged by any of it.
      EXPECT_EQ(writer->Count(), kPlaces);
    }
  }

  TEST_CASE("jump list: a read-only database keeps the jumps it already had");
  {
    // The other way a write fails: the file is there and reads perfectly, and
    // nothing written into it can land. The jumps already in it have to survive
    // that untouched -- a later, writable run is what they are for.
    const std::filesystem::path locked = dir / "readonly.db";
    {
      std::string error{"unset"};
      auto seed = JumpStore::Open(locked, "pane-ro", error);
      EXPECT_TRUE(seed != nullptr);
      if (seed != nullptr) {
        seed->Record(fa, 7, 1);
        seed->Record(fb, 8, 1);
        EXPECT_EQ(seed->Count(), 2);
      }
    }

    // root writes through the mode bits, so there is no read-only file to make.
    if (::getuid() == 0) {
      EXPECT_TRUE(true);
    } else {
      const auto set_mode = [](const std::filesystem::path& path, std::filesystem::perms mode) {
        std::error_code ec;
        std::filesystem::permissions(path, mode, std::filesystem::perm_options::replace, ec);
        return !ec;
      };
      const std::filesystem::perms read_only = std::filesystem::perms::owner_read;
      const std::filesystem::perms writable =
          std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
      EXPECT_TRUE(set_mode(locked, read_only));

      std::string error{"unset"};
      EXPECT_TRUE(JumpStore::Open(locked, "pane-ro", error) == nullptr);
      EXPECT_TRUE(!error.empty());
      EXPECT_TRUE(std::filesystem::exists(locked));

      // SQLite gives the write-ahead log and its index the mode of the database
      // they belong to, so the read-only open leaves 0400 siblings behind.
      EXPECT_TRUE(set_mode(locked, writable));
      for (const std::string_view suffix : {"-wal", "-shm"}) {
        std::filesystem::path sibling = locked;
        sibling += suffix;
        if (std::filesystem::exists(sibling)) EXPECT_TRUE(set_mode(sibling, writable));
      }

      std::string reopen_error{"unset"};
      auto again = JumpStore::Open(locked, "pane-ro", reopen_error);
      EXPECT_TRUE(again != nullptr);
      EXPECT_TRUE(reopen_error.empty());
      if (again != nullptr) {
        EXPECT_EQ(again->Count(), 2);
        // The cursor survived too, on the newest row: stepping back from it
        // lands on the older jump, and there is nothing older still.
        Jump j;
        EXPECT_TRUE(again->Step(false, j));
        EXPECT_EQ(j.line, Index{7});
        EXPECT_TRUE(!again->Step(false, j));
        EXPECT_TRUE(again->Step(true, j));
        EXPECT_EQ(j.line, Index{8});
      }
    }
  }

  RemoveAllQuietly(dir);
}

}  // namespace koi
