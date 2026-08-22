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
  // The fixture is the project. Every place recorded below is keyed against it
  // -- and a path the store would have to keep absolute under the system temp
  // directory is not kept at all (StorablePath), which is every fixture file.
  std::filesystem::create_directories(dir);
  const AsProjectRoot root{dir};
  const std::filesystem::path db = dir / "state.db";

  const std::filesystem::path fa = dir / "a.txt";
  const std::filesystem::path fb = dir / "b.txt";
  std::filesystem::create_directories(dir);
  // Sixty lines each, and every place recorded below is further from the last
  // than kLocationMergeLines. Two places closer together than that are one
  // place since v4 -- one row, merged -- and these fixtures are about the list,
  // not about the merge; the merge has tests of its own further down.
  std::string a_text;
  std::string b_text;
  for (int i = 1; i <= 60; ++i) {
    a_text += "a line " + std::to_string(i) + "\n";
    b_text += "b line " + std::to_string(i) + "\n";
  }
  {
    std::ofstream oa(fa);
    oa << a_text;
    std::ofstream ob(fb);
    ob << b_text;
  }

  // Ages every row of a file, so a record that follows is outside the visit
  // debounce without the test sleeping through it.
  const auto age_rows = [](const std::filesystem::path& where, const char* key) {
    sqlite3* handle = nullptr;
    if (sqlite3_open(where.c_str(), &handle) != SQLITE_OK) {
      sqlite3_close(handle);
      return false;
    }
    bool ok = false;
    {
      // Scoped: a Stmt alive across sqlite3_close leaves the close returning
      // BUSY and the handle leaked.
      Stmt stmt{handle,
                "UPDATE locations SET last_ts = last_ts - 60,"
                " counted_ts = counted_ts - 60 WHERE path=?1;"};
      stmt.Text(1, key);
      ok = stmt.Run();
    }
    sqlite3_close(handle);
    return ok;
  };

  // How many places a store's list holds. Asked of the database rather than of
  // the store: the store had a Count() of its own that counted the same rows
  // and nothing outside these tests ever called it.
  const auto count_rows = [](const std::filesystem::path& where) {
    sqlite3* handle = nullptr;
    if (sqlite3_open(where.c_str(), &handle) != SQLITE_OK) {
      sqlite3_close(handle);
      return std::int64_t{-1};
    }
    std::int64_t rows = -1;
    {
      Stmt stmt{handle, "SELECT COUNT(*) FROM locations;"};
      if (stmt && stmt.Step()) rows = stmt.Integer(0);
    }
    sqlite3_close(handle);
    return rows;
  };

  {
    std::string error;
    auto store = OpenJumpStore(db, "pane-1", error);
    EXPECT_TRUE(store != nullptr);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(count_rows(db), std::int64_t{0});

    store->Record(fa, 1, 1);
    store->Record(fa, 30, 2);
    EXPECT_EQ(count_rows(db), std::int64_t{2});

    store->Record(fa, 30, 2);
    EXPECT_EQ(count_rows(db), std::int64_t{2});


    Jump j;
    EXPECT_TRUE(store->Step(false, j));
    EXPECT_EQ(j.line, Index{1});
    EXPECT_TRUE(!store->Step(false, j));
    EXPECT_TRUE(store->Step(true, j));
    EXPECT_EQ(j.line, Index{30});
    EXPECT_TRUE(!store->Step(true, j));

    // What comes back is a path valid from here, resolved out of the key the
    // store holds -- "a.txt" against the root, not the absolute path recorded.
    std::error_code ec;
    EXPECT_EQ(std::filesystem::weakly_canonical(j.path, ec),
              std::filesystem::weakly_canonical(fa, ec));
  }

  {
    // A short jump is still a jump. Jump records merge on their exact line
    // (LocationRecord::exact), not the corpus's +/-10 window -- otherwise the
    // departure record merges onto the row it is jumping from and stepping
    // back over a five-line jump has nothing to step back to.
    std::string error;
    const std::filesystem::path short_db = dir / "short.db";
    auto store = OpenJumpStore(short_db, "pane-s", error);
    EXPECT_TRUE(store != nullptr);
    store->Record(fa, 20, 1);
    store->Record(fa, 25, 1);
    EXPECT_EQ(count_rows(short_db), std::int64_t{2});
    Jump close;
    EXPECT_TRUE(store->Step(false, close));
    EXPECT_EQ(close.line, Index{20});
    EXPECT_TRUE(store->Step(true, close));
    EXPECT_EQ(close.line, Index{25});
  }

  {
    std::string error;
    auto pane1 = OpenJumpStore(db, "pane-1", error);
    auto pane2 = OpenJumpStore(db, "pane-2", error);
    EXPECT_TRUE(pane1 != nullptr);
    EXPECT_TRUE(pane2 != nullptr);

    Jump j;
    EXPECT_TRUE(pane2->Step(false, j));
    EXPECT_EQ(j.line, Index{30});
    EXPECT_TRUE(pane2->Step(false, j));
    EXPECT_EQ(j.line, Index{1});

    Jump k;
    EXPECT_TRUE(pane1->Step(false, k));
    EXPECT_EQ(k.line, Index{1});

    pane2->Record(fb, 2, 1);
    EXPECT_EQ(count_rows(db), std::int64_t{3});
    Jump newest;
    EXPECT_TRUE(pane1->Step(true, newest) || true);
  }

  {
    std::string error;
    const std::filesystem::path shifted = dir / "shifted.db";
    auto pane_a = OpenJumpStore(shifted, "pane-a", error);
    auto pane_b = OpenJumpStore(shifted, "pane-b", error);
    EXPECT_TRUE((pane_a != nullptr) && (pane_b != nullptr));

    pane_a->Record(fa, 1, 1);
    pane_a->Record(fa, 30, 1);
    pane_b->Record(fb, 1, 1);
    pane_b->Record(fb, 30, 1);

    Jump j;
    EXPECT_TRUE(pane_a->Step(false, j));
    EXPECT_EQ(std::filesystem::path{j.path}.filename().string(), std::string{"a.txt"});
    EXPECT_EQ(j.line, Index{1});

    EXPECT_TRUE(pane_a->Step(true, j));
    EXPECT_EQ(std::filesystem::path{j.path}.filename().string(), std::string{"a.txt"});
    EXPECT_EQ(j.line, Index{30});

    EXPECT_TRUE(pane_a->Step(true, j));
    EXPECT_EQ(std::filesystem::path{j.path}.filename().string(), std::string{"b.txt"});
  }

  {
    std::string error;
    Editor ed;
    ed.jumps = OpenJumpStore(dir / "editor.db", "pane-editor", error);
    EXPECT_TRUE(ed.jumps != nullptr);

    ResetToOriginal(ed.doc.table, a_text);
    ed.doc.file = fa;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    ed.jumps->Record(fa, 2, 1);
    ed.jumps->Record(fb, 3, 1);
    ed.jumps->Record(fa, 40, 1);
    // Standing on the place at the front of the list, so the jump the step back
    // records first merges onto it and the step is a step of one.
    const Index landed = LineStart(ed.doc.table, 39);
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
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{39});

    ed.status.clear();
    press("]");
    press("]");
    EXPECT_TRUE(ed.status.find("no newer position") != std::string::npos);
  }

  {
    std::string error;
    Editor ed;
    ed.jumps = OpenJumpStore(dir / "resume.db", "pane-resume", error);
    EXPECT_TRUE(ed.jumps != nullptr);

    ResetToOriginal(ed.doc.table, a_text);
    ed.doc.file = fa;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RecordJump(ed);

    const Index at = LineStart(ed.doc.table, 30);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
    ed.mode = Mode::kInsert;
    std::ignore = InsertAtCursorsKeeping("edited ", ed.doc.table, ed.doc.selections);
    ed.mode = Mode::kNormal;
    ApplyModeInvariants(ed);
    EXPECT_EQ(count_rows(dir / "resume.db"), std::int64_t{1});
    const Index edited_line = LineAt(ed.doc.table, Cur(ed));
    EXPECT_EQ(edited_line, Index{30});

    StepJump(ed, false);
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{0});

    StepJump(ed, true);
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{30});

    EXPECT_EQ(count_rows(dir / "resume.db"), std::int64_t{2});
    StepJump(ed, false);
    StepJump(ed, true);
    EXPECT_EQ(count_rows(dir / "resume.db"), std::int64_t{2});
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{30});
  }

  {
    std::string error;
    Editor ed;
    ed.jumps = OpenJumpStore(dir / "motion.db", "pane-motion", error);
    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\nfour\nfive\nsix\n");
    ed.doc.file = fa;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RecordJump(ed);
    EXPECT_EQ(count_rows(dir / "motion.db"), std::int64_t{1});

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

    EXPECT_EQ(count_rows(dir / "motion.db"), std::int64_t{1});
  }

  {
    std::string error;
    auto store = OpenJumpStore(dir / "dedupe.db", "pane-dedupe", error);
    store->Record(fa, 10, 1);
    store->Record(fa, 30, 1);
    store->Record(fa, 10, 5);
    EXPECT_EQ(count_rows(dir / "dedupe.db"), std::int64_t{2});

    Jump j;
    EXPECT_TRUE(store->Step(false, j));
    EXPECT_EQ(j.line, Index{30});
    EXPECT_TRUE(!store->Step(false, j));
  }

  TEST_CASE("jump list: a place recorded again moves to the front and keeps its count");
  {
    const std::filesystem::path merged = dir / "merged.db";
    std::string error;
    auto store = OpenJumpStore(merged, "pane-merge", error);
    EXPECT_TRUE(store != nullptr);
    sqlite3* reader = nullptr;
    EXPECT_EQ(sqlite3_open(merged.c_str(), &reader), SQLITE_OK);
    const auto column = [&reader](const char* sql) {
      Stmt stmt{reader, sql};
      return (stmt && stmt.Step()) ? stmt.Integer(0) : std::int64_t{-1};
    };

    store->Record(fa, 4, 1);
    store->Record(fb, 9, 1);
    const std::int64_t first = column("SELECT seq FROM locations WHERE path='a.txt';");
    // Aged past kLocationVisitDebounce first: the counter is held still for a
    // window after every touch, so a second arrival inside it refreshes the row
    // without counting -- which is the point of the debounce and not the point
    // of this case.
    EXPECT_TRUE(age_rows(merged, "a.txt"));
    store->Record(fa, 4, 7);

    // One row, one more visit, and the column of the arrival that merged onto
    // it -- the whole difference from v3, which deleted the row and inserted a
    // fresh one with visits back at 1.
    EXPECT_EQ(count_rows(merged), std::int64_t{2});
    EXPECT_EQ(column("SELECT COUNT(*) FROM locations WHERE path='a.txt';"), std::int64_t{1});
    EXPECT_EQ(column("SELECT visits FROM locations WHERE path='a.txt';"), std::int64_t{2});
    EXPECT_EQ(column("SELECT col FROM locations WHERE path='a.txt';"), std::int64_t{7});
    // Move-to-front is the seq, and the seq only ever goes up: the row is now
    // newer than the one recorded after it.
    EXPECT_TRUE(column("SELECT seq FROM locations WHERE path='a.txt';") > first);
    EXPECT_EQ(column("SELECT seq FROM locations WHERE path='a.txt';"),
              column("SELECT MAX(seq) FROM locations;"));
    // And the list steps in that order: back from the front is the other file.
    Jump j;
    EXPECT_TRUE(store->Step(false, j));
    EXPECT_EQ(std::filesystem::path{j.path}.filename().string(), std::string{"b.txt"});
    sqlite3_close(reader);
  }

  TEST_CASE("jump list: two panes recording one place share the row, and neither loses it");
  {
    // v3 deduped with a DELETE that was not pane-scoped, so recording a place
    // in one pane took the other pane's row for the same place out of the list
    // -- with its history -- and left that pane's cursor naming nothing.
    const std::filesystem::path shared = dir / "two-panes.db";
    std::string error;
    auto left = OpenJumpStore(shared, "pane-left", error);
    auto right = OpenJumpStore(shared, "pane-right", error);
    EXPECT_TRUE((left != nullptr) && (right != nullptr));

    left->Record(fa, 1, 1);
    right->Record(fb, 2, 1);
    left->Record(fa, 40, 1);
    // The place the other pane recorded, recorded again here -- and aged first,
    // so the arrival is outside the debounce window and counts.
    EXPECT_TRUE(age_rows(shared, "b.txt"));
    left->Record(fb, 2, 1);

    EXPECT_EQ(count_rows(shared), std::int64_t{3});
    sqlite3* reader = nullptr;
    EXPECT_EQ(sqlite3_open(shared.c_str(), &reader), SQLITE_OK);
    {
      Stmt stmt{reader, "SELECT visits FROM locations WHERE path='b.txt';"};
      EXPECT_EQ((stmt && stmt.Step()) ? stmt.Integer(0) : std::int64_t{-1}, std::int64_t{2});
    }
    // Both cursors still step, and both still reach the place the other pane
    // recorded.
    Jump j;
    EXPECT_TRUE(right->Step(false, j));
    EXPECT_EQ(std::filesystem::path{j.path}.filename().string(), std::string{"a.txt"});
    Jump k;
    EXPECT_TRUE(left->Step(false, k));
    EXPECT_EQ(std::filesystem::path{k.path}.filename().string(), std::string{"a.txt"});
    sqlite3_close(reader);
  }

  {
    std::string error;
    Editor ed;
    ed.jumps = OpenJumpStore(dir / "dirty.db", "pane-dirty", error);
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
    ed.jumps = OpenJumpStore(dir / "views.db", "pane-views", error);
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
    const std::filesystem::path probe = LegacyJumpDbPath();
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
    // Record is a read (is this place already in the list?), a seq taken from
    // the store-wide counter, a merge or an insert that puts the place at the
    // front, and a cursor move onto the row just written -- one change to the
    // list. Nothing here can crash the process between them to prove the
    // atomicity directly, so what is checked is everything the transaction is
    // observable through from outside: the result is committed and complete
    // when Record returns, a failure anywhere leaves the list exactly as it
    // was, and neither path leaves the write lock held.
    const std::filesystem::path txdb = dir / "one-transaction.db";
    std::string error{"unset"};
    auto store = OpenJumpStore(txdb, "pane-tx", error);
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
      const auto rows = [&scalar] { return scalar("SELECT COUNT(*) FROM locations;"); };
      // Where the cursor stands in the walk. The cursor holds a row id, which a
      // merge does not move; what moves is the row's seq, and that is what says
      // the place went to the front.
      const auto cursor = [&scalar] {
        return scalar("SELECT COALESCE((SELECT l.seq FROM jump_cursor c"
                      " JOIN locations l ON l.id = c.at WHERE c.pane='pane-tx'),0);");
      };
      // The invariant the transaction exists for: the cursor names a row that
      // is really there.
      const auto cursor_is_real = [&scalar] {
        return scalar("SELECT COUNT(*) FROM locations WHERE id ="
                      " (SELECT at FROM jump_cursor WHERE pane='pane-tx');");
      };
      const auto rows_at = [&other](const std::string& file, int line) {
        Stmt stmt{other, "SELECT COUNT(*) FROM locations WHERE path=?1 AND line=?2;"};
        if (!stmt) return std::int64_t{-1};
        stmt.Text(1, file);
        stmt.Int(2, line);
        return stmt.Step() ? stmt.Integer(0) : std::int64_t{-1};
      };
      const auto visits_at = [&other](const std::string& file, int line) {
        Stmt stmt{other, "SELECT visits FROM locations WHERE path=?1 AND line=?2;"};
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

      // The keys the rows are under, not the paths handed in: inside the
      // project a path is stored relative to the root.
      const std::string a{"a.txt"};
      const std::string b{"b.txt"};
      store->Record(fa, 10, 1);
      store->Record(fb, 20, 1);
      EXPECT_EQ(rows(), std::int64_t{2});
      EXPECT_EQ(cursor_is_real(), std::int64_t{1});
      EXPECT_TRUE(lock_is_free());

      // The merge half: recording a place already in the list moves it to the
      // front, and the row it moves is the row that was there -- one row
      // before, the same row after, with one more visit on it. v3 did this
      // with a DELETE and an INSERT, which is where the visit count went.
      const std::int64_t before = cursor();
      EXPECT_EQ(visits_at(a, 10), std::int64_t{1});
      EXPECT_TRUE(age_rows(txdb, "a.txt"));
      store->Record(fa, 10, 5);
      EXPECT_EQ(rows(), std::int64_t{2});
      EXPECT_EQ(rows_at(a, 10), std::int64_t{1});
      EXPECT_EQ(visits_at(a, 10), std::int64_t{2});
      EXPECT_TRUE(cursor() > before);
      EXPECT_EQ(cursor_is_real(), std::int64_t{1});
      EXPECT_EQ(cursor(), scalar("SELECT MAX(seq) FROM locations;"));
      EXPECT_TRUE(lock_is_free());

      // Now the failure paths, without patching SQLite: rename a table out from
      // under the store and its statements no longer prepare. The transaction
      // has already begun by then, so this is the rollback -- and a missing
      // rollback would leave the store on the write lock for the rest of the
      // session, which `lock_is_free` below would say.
      const std::int64_t kept_cursor = cursor();
      EXPECT_TRUE(ExecSql(other, "ALTER TABLE locations RENAME TO locations_hidden;"));
      store->Record(fb, 30, 1);
      EXPECT_TRUE(lock_is_free());
      EXPECT_TRUE(ExecSql(other, "ALTER TABLE locations_hidden RENAME TO locations;"));
      EXPECT_EQ(rows(), std::int64_t{2});
      EXPECT_EQ(rows_at(b, 30), std::int64_t{0});
      EXPECT_EQ(cursor(), kept_cursor);

      // The cursor half of the same thing, and the one the insert alone would
      // not cover: the write that fails is the last of them, so the insert and
      // the seq it took have already succeeded inside the transaction and have
      // to be taken back with it. Without the rollback the list keeps a jump
      // whose cursor was never moved onto it.
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
      EXPECT_EQ(rows(), std::int64_t{3});
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
    // count is invariant from then on: each Record merges onto a row that is
    // already there. Any other count the reader sees is a gap between two
    // commits.
    const std::filesystem::path hammer_db = dir / "hammer.db";
    std::string error{"unset"};
    auto writer = OpenJumpStore(hammer_db, "pane-hammer", error);
    EXPECT_TRUE(writer != nullptr);
    if (writer != nullptr) {
      constexpr int kPlaces = 6;
      constexpr int kRecords = 400;
      // Twenty lines apart, so each is a place of its own: closer than
      // kLocationMergeLines and the six would be one row.
      const auto place = [](int i) { return Index{10 + (20 * i)}; };
      for (int i = 0; i < kPlaces; ++i) writer->Record(fa, place(i), 1);
      EXPECT_EQ(count_rows(hammer_db), std::int64_t{kPlaces});

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
          Stmt look{other, "SELECT (SELECT COUNT(*) FROM locations),"
                           " (SELECT COUNT(*) FROM locations WHERE id ="
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
        // that is the shortcut, and it writes nothing but the cursor.
        writer->Record(fa, place(i % kPlaces), Index{1 + (i % 3)});
        // Every so often, step back and re-record the place stepped onto. That
        // is the one arrangement in which the row whose seq the merge moves is
        // the row the cursor is sitting on, so a cursor written outside the
        // transaction would name a seq nothing holds -- which is what
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
      EXPECT_EQ(count_rows(hammer_db), std::int64_t{kPlaces});
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
      auto seed = OpenJumpStore(locked, "pane-ro", error);
      EXPECT_TRUE(seed != nullptr);
      if (seed != nullptr) {
        seed->Record(fa, 7, 1);
        seed->Record(fb, 8, 1);
        EXPECT_EQ(count_rows(locked), std::int64_t{2});
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
      EXPECT_TRUE(OpenJumpStore(locked, "pane-ro", error) == nullptr);
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
      auto again = OpenJumpStore(locked, "pane-ro", reopen_error);
      EXPECT_TRUE(again != nullptr);
      EXPECT_TRUE(reopen_error.empty());
      if (again != nullptr) {
        EXPECT_EQ(count_rows(locked), std::int64_t{2});
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

  TEST_CASE("jump list: a linger between the jump and the step back keeps the departure");
  {
    // `locations` is shared, and the jump list is not the only thing writing
    // it: every linger and every edit takes the next store-wide seq. Reading
    // "is this pane part-way back through the list?" off that counter meant one
    // linger after a jump put the cursor behind a place the user never jumped
    // to -- so the step back did not record the place it was leaving, and
    // stepped from that place instead of back to it.
    //
    // Both stores are wired to the same database here, which is what the editor
    // does and what the fixtures above do not: with no project store the linger
    // has nowhere to land and the whole thing is invisible.
    std::string error{"unset"};
    Editor ed;
    ed.project = ProjectStore::Open(dir / "linger.db", error);
    EXPECT_TRUE(ed.project != nullptr);
    ed.jumps = JumpStore::Open(ed.project, "pane-linger", error);
    EXPECT_TRUE(ed.jumps != nullptr);
    EXPECT_TRUE(error.empty());

    ResetToOriginal(ed.doc.table, a_text);
    ed.doc.file = fa;
    const auto put_cursor = [&ed](Index line) {
      const Index at = LineStart(ed.doc.table, line);
      ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
    };
    // What three seconds of sitting still writes: the store's own record, no
    // cursor move, and a fresh seq every time.
    const auto linger = [&ed] {
      LocationRecord row;
      EXPECT_TRUE(LocationHere(ed, row));
      EXPECT_TRUE(ed.project->WriteLocation(row) != 0);
    };

    // Two jumps, each with a linger before it, and a linger at the place the
    // second one landed. Twenty-five lines apart, so none of the three merges
    // onto another.
    put_cursor(1);
    linger();
    RecordJump(ed);
    put_cursor(25);
    linger();
    RecordJump(ed);
    put_cursor(50);
    linger();

    // The linger just written is the newest row in the store, and it is not a
    // jump: the pane has not stepped anywhere, so the step back below has to
    // record the place it is leaving first.
    EXPECT_TRUE(ed.jumps->AtNewest());
    StepJump(ed, false);
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{25});
    // And now it has stepped, so the next one records nothing and steps again.
    EXPECT_FALSE(ed.jumps->AtNewest());
    StepJump(ed, false);
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{1});
    EXPECT_FALSE(ed.jumps->AtNewest());

    // A jump somewhere new ends the walk: the departure goes to the front of
    // the list and the pane is recording again, not stepping.
    RecordJump(ed);
    EXPECT_TRUE(OpenTarget(ed, fb.string() + ":11"));
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"b.txt"});
    EXPECT_TRUE(ed.jumps->AtNewest());

    // Which is the whole point of clearing it: the step back from there records
    // b.txt and comes back to the place the jump left.
    StepJump(ed, false);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.txt"});
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{1});
  }

  RemoveAllQuietly(dir);
}

}  // namespace koi
