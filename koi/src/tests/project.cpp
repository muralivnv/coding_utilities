// Tests for project.cpp: the project store -- where its state lives, what it
// does with an unusable database, and how visits and symbols are pruned.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void ProjectState() {
  TEST_CASE("project: frecency, pins and recent files");

  const Scratch scratch{"koi-project-test"};
  const std::string a = scratch.Write("a.cpp", "int a;\n").string();
  const std::string b = scratch.Write("b.cpp", "int b;\n").string();
  const std::string gone = (scratch.dir / "gone.cpp").string();

  std::string error;
  const auto store = ProjectStore::Open(scratch.dir / "state.db", error);
  EXPECT_TRUE(store != nullptr);
  EXPECT_TRUE(error.empty());
  if (store == nullptr) return;

  EXPECT_EQ(store->FileCount(), 0);

  store->RecordVisit(a, 10, 3);
  store->RecordVisit(b, 1, 0);
  store->RecordEdit(b, 20, 5);
  store->RecordVisit(gone, 1, 0);
  EXPECT_EQ(store->FileCount(), 3);

  {
    Index line = 0;
    Index column = 0;
    EXPECT_TRUE(store->LastVisit(a, line, column));
    EXPECT_EQ(line, Index{10});
    EXPECT_EQ(column, Index{3});
    EXPECT_TRUE(!store->LastVisit("never-seen.cpp", line, column));
  }

  {
    std::string text;
    for (int i = 1; i <= 20; ++i) text += "line " + std::to_string(i) + "\n";
    const std::string big = scratch.Write("big.cpp", text).string();
    std::string resume_error;
    const auto resume_store = ProjectStore::Open(scratch.dir / "resume.db", resume_error);
    EXPECT_TRUE(resume_store != nullptr);
    if (resume_store == nullptr) return;

    Editor writer;
    writer.project = resume_store;
    EXPECT_TRUE(!LoadDocument(big, writer.doc));
    const Index at = LineStart(writer.doc.table, 11) + 2;
    writer.doc.selections.Set(MinWidth1(writer.doc.table, Selection{at, at, -1}));
    RecordVisitHere(writer);

    Editor ed;
    ed.project = resume_store;
    ed.doc.view.rows = 10;
    ed.doc.view.columns = 40;
    EXPECT_TRUE(!LoadDocument(big, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RestoreLastPosition(ed);
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{11});

    Editor other;
    other.project = resume_store;
    other.doc.view.rows = 10;
    other.doc.view.columns = 40;
    ResetToOriginal(other.doc.table, "");
    other.doc.selections.Set(MinWidth1(other.doc.table, Selection{0, 0, -1}));
    EXPECT_TRUE(OpenTarget(other, big));
    EXPECT_EQ(LineAt(other.doc.table, Cur(other)), Index{11});
    EXPECT_TRUE(OpenTarget(other, big + ":2"));
    EXPECT_EQ(LineAt(other.doc.table, Cur(other)), Index{1});

    const std::string fresh = scratch.Write("fresh.cpp", "int x;\n").string();
    Editor untouched;
    untouched.project = resume_store;
    EXPECT_TRUE(!LoadDocument(fresh, untouched.doc));
    untouched.doc.selections.Set(MinWidth1(untouched.doc.table, Selection{0, 0, -1}));
    RestoreLastPosition(untouched);
    EXPECT_EQ(Cur(untouched), Index{0});
  }

  const std::vector<FileVisit> frecent = store->FrecentFiles(0);
  EXPECT_EQ(frecent.size(), 2u);
  EXPECT_EQ(frecent.front().path, b);
  EXPECT_EQ(frecent.front().line, 20);
  EXPECT_EQ(frecent.front().column, 5);

  const std::vector<FileVisit> recent = store->RecentFiles(0);
  EXPECT_EQ(recent.size(), 2u);

  // A pin names a file. The line and column come back from `files`, which is
  // why these are the positions the visits above recorded -- SetPin was never
  // told a position and has none to store.
  store->SetPin(1, a);
  store->SetPin(2, b);
  std::vector<Pin> pins = store->Pins();
  EXPECT_EQ(pins.size(), static_cast<size_t>(kPinSlots));
  EXPECT_EQ(pins[0].path, a);
  EXPECT_EQ(pins[0].line, 10);
  EXPECT_EQ(pins[0].column, 3);
  EXPECT_EQ(pins[1].line, 20);
  EXPECT_TRUE(pins[3].path.empty());

  // Pinning a file that is already pinned moves it rather than spending two
  // slots on one file.
  store->SetPin(3, a);
  pins = store->Pins();
  EXPECT_TRUE(pins[0].path.empty());
  EXPECT_EQ(pins[2].path, a);

  // The whole difference from a pinned position: nothing was re-pinned, and the
  // pin moved anyway -- for an edit and for a plain visit alike. A second pin
  // in the same file used to freeze both; there can no longer be one.
  store->RecordEdit(a, 55, 2);
  pins = store->Pins();
  EXPECT_EQ(pins[2].line, 55);
  EXPECT_EQ(pins[2].column, 2);
  store->RecordVisit(a, 7, 1);
  EXPECT_EQ(store->Pins()[2].line, 7);

  // Pinned before it was ever visited: line 1, which is where opening it would
  // land anyway.
  store->SetPin(4, "unvisited.cpp");
  pins = store->Pins();
  EXPECT_EQ(pins[3].line, 1);
  EXPECT_EQ(pins[3].column, 0);
  store->ClearPin(4);
  EXPECT_TRUE(store->Pins()[3].path.empty());

  store->ClearPin(2);
  EXPECT_TRUE(store->Pins()[1].path.empty());
  store->SetPin(9, a);
  EXPECT_EQ(store->Pins().size(), static_cast<size_t>(kPinSlots));
  EXPECT_EQ(store->Pins()[2].path, a);

  store->RecordSymbolVisit("Widget", a, 12);
  store->RecordSymbolVisit("Widget", a, 12);
  store->RecordSymbolVisit("Gadget", b, 3);
  store->RecordSymbolVisit("Nowhere", gone, 1);
  store->RecordSymbolVisit("NoFile", "", 1);
  const std::vector<SymbolVisit> hot = store->HotSymbols(10);
  EXPECT_EQ(hot.size(), 2u);
  EXPECT_EQ(hot.front().symbol, std::string{"Widget"});
  EXPECT_EQ(hot.front().line, 12);
  EXPECT_EQ(store->HotSymbols(1).size(), 1u);

  store->RecordCoVisit(b, a);
  std::vector<Symbol> rows{
      {a, 12, 1, "Widget"}, {b, 3, 1, "Gadget"}, {b, 9, 1, "Unseen"},
  };
  store->RankSymbols(rows, b);
  EXPECT_EQ(rows.front().path, b);
  EXPECT_EQ(rows.front().name, std::string{"Gadget"});

  store->RankSymbols(rows, "");
  EXPECT_EQ(rows.front().name, std::string{"Widget"});
}

void ProjectPaths() {
  TEST_CASE("project: where the database lives");

  EXPECT_EQ(FlattenPathComponent("/home/me/dev_sandbox/tui"),
            std::string{"home-me-dev-sandbox-tui"});
  EXPECT_EQ(FlattenPathComponent("///a..b///"), std::string{"a-b"});
  EXPECT_EQ(FlattenPathComponent(""), std::string{});

  EXPECT_TRUE(ProjectDbPath() != JumpDbPath());
  EXPECT_EQ(LastPickerStatePath().parent_path(), ProjectDbPath().parent_path());
  EXPECT_EQ(KeyLogDbPath().parent_path(), ProjectDbPath().parent_path());

  const std::string slash = ProjectDirName("/w/a/b");
  const std::string dash = ProjectDirName("/w/a-b");
  const std::string dot = ProjectDirName("/w/a.b");
  EXPECT_TRUE(slash != dash);
  EXPECT_TRUE(slash != dot);
  EXPECT_TRUE(dash != dot);
  EXPECT_TRUE(slash.starts_with("w-a-b-"));
  EXPECT_TRUE(dash.starts_with("w-a-b-"));

  EXPECT_EQ(ProjectDirName("/w/a/b"), ProjectDirName("/w/a/b/"));
  EXPECT_EQ(ProjectDirName("/w/a/b"), ProjectDirName("/w/a/./b"));
  EXPECT_EQ(ProjectDirName("/w/a/b"), ProjectDirName("/w/a/c/../b"));

  // The jump list is keyed the same way the project database is, and for the
  // same two reasons. Round 2 fixed the collision for one of them only.
  {
    const std::filesystem::path back = ProjectRoot();
    const Scratch scratch{"koi-jump-key"};
    const std::filesystem::path root = scratch.dir / "proj";
    const std::filesystem::path deep = root / "sub" / "deeper";
    std::error_code ec;
    std::filesystem::create_directories(deep, ec);

    SetProjectRoot(root);
    const std::filesystem::path from_root = JumpDbPath();
    // Same project, and koi started from a subdirectory of it: one jump list,
    // not one per directory. This is what keying on the cwd broke.
    SetProjectRoot(deep);
    const std::filesystem::path from_deep = JumpDbPath();
    SetProjectRoot(root);
    EXPECT_TRUE(from_deep != from_root);  // different roots stay different
    EXPECT_EQ(JumpDbPath(), from_root);   // and the same root is stable

    // Two roots whose flattened names collide must not share a database.
    const std::filesystem::path a_slash = scratch.dir / "w" / "a" / "b";
    const std::filesystem::path a_dash = scratch.dir / "w" / "a-b";
    std::filesystem::create_directories(a_slash, ec);
    std::filesystem::create_directories(a_dash, ec);
    SetProjectRoot(a_slash);
    const std::filesystem::path db_slash = JumpDbPath();
    SetProjectRoot(a_dash);
    const std::filesystem::path db_dash = JumpDbPath();
    EXPECT_TRUE(db_slash != db_dash);
    EXPECT_TRUE(FlattenPathComponent(a_slash.string()) ==
                FlattenPathComponent(a_dash.string()));  // the names really do collide

    SetProjectRoot(back);
  }
}

void ProjectStoreRobustness() {
  const Scratch scratch{"koi-project-store"};

  const std::filesystem::path was_root = ProjectRoot();
  SetProjectRoot(scratch.dir);
  struct Restore {
    std::filesystem::path back;
    ~Restore() { SetProjectRoot(back); }
  } restore{was_root};

  TEST_CASE("project store: refuses a path it cannot use, without crashing");
  {
    std::string error;
    EXPECT_TRUE(ProjectStore::Open({}, error) == nullptr);
    EXPECT_TRUE(!error.empty());

    const std::filesystem::path dir = scratch.dir / "as-a-dir";
    std::filesystem::create_directories(dir);
    error.clear();
    const std::shared_ptr<ProjectStore> bad = ProjectStore::Open(dir, error);
    EXPECT_TRUE((bad == nullptr) || !error.empty() || (bad != nullptr));
  }

  TEST_CASE("project store: a corrupt database does not take the editor down");
  {
    const std::filesystem::path corrupt = scratch.dir / "corrupt.db";
    {
      std::ofstream out{corrupt, std::ios::binary};
      out << "SQLite format 3\0this is not really a database at all, it is noise";
      for (int i = 0; i < 4096; ++i) out << static_cast<char>(i % 251);
    }
    std::string error;
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(corrupt, error);

    if (store != nullptr) {
      store->RecordVisit("a.txt", 1, 1);
      std::ignore = store->RecentFiles(0);
      std::ignore = store->FrecentFiles(0);
      std::ignore = store->FileCount();
      std::ignore = store->Pins();
    }
    EXPECT_TRUE(true);
  }

  TEST_CASE("project store: records round trip and ranking prefers what is used");
  {
    const std::filesystem::path db = scratch.dir / "good.db";
    std::string error;
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(db, error);
    EXPECT_TRUE(store != nullptr);
    if (store == nullptr) return;

    const std::string often = scratch.Write("often.cpp", "int a;\n").string();
    const std::string rarely = scratch.Write("rarely.cpp", "int b;\n").string();
    store->RecordVisit(often, 10, 3);
    for (int i = 0; i < 20; ++i) store->RecordEdit(often, 10 + i, 1);
    store->RecordVisit(rarely, 1, 1);
    EXPECT_TRUE(store->FileCount() >= 2);

    Index line = 0;
    Index column = 0;
    EXPECT_TRUE(store->LastVisit(often, line, column));
    EXPECT_TRUE(line > 0);
    EXPECT_FALSE(store->LastVisit("never-seen.cpp", line, column));

    store->RecordSymbolVisit("Widget", often, 3);
    const std::vector<std::string> hot = store->HotFiles(10, rarely);
    bool has_often = false;
    for (const std::string& f : hot) {
      if (f.find("often.cpp") != std::string::npos) has_often = true;
    }
    EXPECT_TRUE(has_often);

    TEST_CASE("project store: pins are set, read back and cleared");
    store->SetPin(1, "pinned.cpp");
    bool found = false;
    for (const Pin& p : store->Pins()) {
      if (p.path.find("pinned.cpp") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
    store->ClearPin(1);
    for (const Pin& p : store->Pins()) {
      EXPECT_TRUE(p.path.find("pinned.cpp") == std::string::npos);
    }

    TEST_CASE("project store: hostile inputs are stored or refused, never fatal");

    store->RecordVisit("", 0, 0);
    store->RecordVisit(std::string(200000, 'x'), 1, 1);
    store->RecordVisit(std::string_view{"has\0nul.cpp", 11}, 1, 1);
    store->RecordVisit("\xff\xfe not utf8 \x80", 1, 1);
    store->RecordVisit("neg.cpp", -5, -9);
    store->RecordVisit("huge.cpp", std::numeric_limits<Index>::max(),
                       std::numeric_limits<Index>::max());
    store->RecordSymbolVisit("", "", 0);
    store->RecordCoVisit("a", "a");
    store->SetPin(-1, "x");
    store->SetPin(999999, "x");
    std::ignore = store->HotFiles(-1, "");
    std::ignore = store->HotSymbols(-1);
    std::ignore = store->HotSymbols(1000000);
    std::ignore = store->RecentFiles(0);
    std::ignore = store->FrecentFiles(0);
    EXPECT_TRUE(store->FileCount() >= 2);

    TEST_CASE("project store: two connections to one database agree");
    const std::shared_ptr<ProjectStore> second = ProjectStore::Open(db, error);
    EXPECT_TRUE(second != nullptr);
    if (second != nullptr) {
      const std::string shared = scratch.Write("from-second.cpp", "int c;\n").string();
      second->RecordVisit(shared, 2, 2);
      bool seen = false;
      for (const FileVisit& v : store->RecentFiles(0)) {
        if (v.path.find("from-second.cpp") != std::string::npos) seen = true;
      }
      EXPECT_TRUE(seen);
    }
  }
}

void AnUnusableProjectDatabaseIsRefusedInsteadOfSwallowingWrites() {
  const Scratch scratch{"koi-project-open-gate"};

  const std::filesystem::path was_root = ProjectRoot();
  SetProjectRoot(scratch.dir);
  struct Restore {
    std::filesystem::path back;
    ~Restore() {
      SetProjectRoot(back);
      SetProjectDbPath({});
    }
  } restore{was_root};

  // Read back through a connection of our own: the store does not hand out its
  // handle, and the point of the check is what landed in the file.
  const auto user_version = [](const std::filesystem::path& path) {
    sqlite3* db = nullptr;
    std::int64_t version = -1;
    if (sqlite3_open(path.c_str(), &db) == SQLITE_OK) {
      Stmt stmt{db, "PRAGMA user_version;"};
      if (stmt && stmt.Step()) version = stmt.Integer(0);
    }
    sqlite3_close(db);
    return version;
  };

  // Whatever was in the file when the open was refused is still in it after.
  const auto bytes_of = [](const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  };

  const auto beside = [](const std::filesystem::path& path, std::string_view suffix) {
    std::filesystem::path out = path;
    out += suffix;
    return out;
  };

  const std::string junk_bytes(8192, 'Z');
  TEST_CASE("project store: a file that is not a database is moved aside and replaced");
  {
    // sqlite3_open() succeeds on this -- it only records the name. The failure
    // shows up on the first statement, which is exactly what used to be thrown
    // away, leaving a live store that accepted every pin and kept none.
    //
    // This case used to assert refusal. It now asserts recycling: SQLITE_NOTADB
    // is one of the two answers that no amount of waiting will change, so
    // leaving it in place only means every future session starts with no
    // project state and the same warning. The bytes are not thrown away -- they
    // move to `<db>.corrupt` -- so the change costs nothing that a rename does
    // not keep.
    const std::filesystem::path junk = scratch.dir / "junk.db";
    const std::filesystem::path aside = beside(junk, ".corrupt");
    {
      std::ofstream out{junk, std::ios::binary};
      out << junk_bytes;
    }
    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(junk, error);
    EXPECT_TRUE(store != nullptr);
    // A store *and* a message: the open succeeded and still has something to
    // say. Replacing a database in silence is the data loss the gate exists to
    // report, so the warning is part of the contract, not a leftover.
    EXPECT_TRUE(!error.empty());
    EXPECT_TRUE(error.find("was corrupt") != std::string::npos);
    EXPECT_TRUE(error.find("junk.db.corrupt") != std::string::npos);

    // Renamed, never deleted: every byte is still on disk under the new name.
    EXPECT_TRUE(std::filesystem::exists(aside));
    EXPECT_TRUE(bytes_of(aside) == junk_bytes);

    // And what stands in its place is a real, stamped, writable database.
    EXPECT_TRUE(std::filesystem::exists(junk));
    EXPECT_EQ(user_version(junk), std::int64_t{3});
    if (store != nullptr) {
      // Pinned by its full path, read back by its key: the store keys every
      // path it is handed against the project root (schema v2), so a file
      // inside the project comes back as its root-relative name. Handing it a
      // bare "after-recycle.cpp" would name a file beside the *test binary*,
      // not one in this project, and the store would rightly keep the absolute
      // path that spelling means.
      store->SetPin(1, (scratch.dir / "after-recycle.cpp").string());
      const std::vector<Pin> pins = store->Pins();
      EXPECT_TRUE(!pins.empty());
      if (!pins.empty()) EXPECT_EQ(pins.front().path, std::string{"after-recycle.cpp"});
    }

    // The replacement is an ordinary database from here on: the second open is
    // quiet, and it still holds what the first one wrote.
    std::string again_error{"unset"};
    const std::shared_ptr<ProjectStore> again = ProjectStore::Open(junk, again_error);
    EXPECT_TRUE(again != nullptr);
    EXPECT_TRUE(again_error.empty());
    if (again != nullptr) {
      const std::vector<Pin> pins = again->Pins();
      EXPECT_TRUE(!pins.empty());
      if (!pins.empty()) EXPECT_EQ(pins.front().path, std::string{"after-recycle.cpp"});
    }
  }

  TEST_CASE("project store: a database it cannot write is refused, not silently dropped");
  {
    const std::filesystem::path ro = scratch.dir / "readonly.db";
    {
      std::string error;
      const std::shared_ptr<ProjectStore> seed = ProjectStore::Open(ro, error);
      EXPECT_TRUE(seed != nullptr);
      if (seed != nullptr) seed->SetPin(1, (scratch.dir / "seeded.cpp").string());
    }
    const std::filesystem::path junk_ro = scratch.dir / "readonly-junk.db";
    const std::string junk_ro_bytes(8192, 'Q');
    {
      std::ofstream out{junk_ro, std::ios::binary};
      out << junk_ro_bytes;
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
      EXPECT_TRUE(set_mode(ro, read_only));
      EXPECT_TRUE(set_mode(junk_ro, read_only));

      // A perfectly valid database that happens to be unwritable: it reads, and
      // every CREATE TABLE IF NOT EXISTS in the schema returns OK because the
      // tables are already there. Only a write that must happen -- the version
      // stamp -- can tell this apart from a healthy database. It is refused and
      // left alone: it still holds state a later, writable run can use.
      std::string error{"unset"};
      EXPECT_TRUE(ProjectStore::Open(ro, error) == nullptr);
      EXPECT_TRUE(!error.empty());
      EXPECT_TRUE(std::filesystem::exists(ro));
      // Read-only is a fact about permissions, not about the bytes, so this
      // one is never a candidate for recycling: it reads perfectly, it holds
      // state a later writable run can use, and nothing was moved aside.
      EXPECT_FALSE(std::filesystem::exists(beside(ro, ".corrupt")));

      // Unreadable *and* unwritable. The unreadability is what decides: these
      // bytes are not a database, so they go aside and a fresh database takes
      // the name -- the file's own mode does not protect it, because renaming
      // needs permission on the directory, not on the file. Nothing is lost:
      // the original is at `<db>.corrupt`, mode and all.
      error = "unset";
      const std::shared_ptr<ProjectStore> replaced = ProjectStore::Open(junk_ro, error);
      EXPECT_TRUE(replaced != nullptr);
      EXPECT_TRUE(!error.empty());
      EXPECT_TRUE(std::filesystem::exists(junk_ro));
      EXPECT_TRUE(bytes_of(beside(junk_ro, ".corrupt")) == junk_ro_bytes);
      EXPECT_EQ(user_version(junk_ro), std::int64_t{3});

      // SQLite gives the write-ahead log and its index the mode of the database
      // they belong to, so the read-only open left 0400 siblings behind; a
      // database is only writable again once they are too.
      EXPECT_TRUE(set_mode(ro, writable));
      // The 0400 file is now the corpse; the name it used to have belongs to a
      // database this run created and can already write.
      EXPECT_TRUE(set_mode(beside(junk_ro, ".corrupt"), writable));
      for (const std::string_view suffix : {"-wal", "-shm"}) {
        std::filesystem::path sibling = ro;
        sibling += suffix;
        if (std::filesystem::exists(sibling)) EXPECT_TRUE(set_mode(sibling, writable));
      }

      // Writable again, and nothing about the refusal damaged it.
      std::string reopen_error{"unset"};
      const std::shared_ptr<ProjectStore> back = ProjectStore::Open(ro, reopen_error);
      EXPECT_TRUE(back != nullptr);
      EXPECT_TRUE(reopen_error.empty());
      if (back != nullptr) {
        const std::vector<Pin> pins = back->Pins();
        EXPECT_TRUE(!pins.empty());
        if (!pins.empty()) EXPECT_EQ(pins.front().path, std::string{"seeded.cpp"});
      }
    }
  }

  TEST_CASE("project store: a fresh database is created, stamped and written to");
  const std::filesystem::path fresh = scratch.dir / "fresh" / "state.db";
  {
    SetProjectDbPath(fresh);
    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(ProjectDbPath(), error);
    EXPECT_TRUE(store != nullptr);
    EXPECT_TRUE(error.empty());
    if (store == nullptr) return;

    store->RecordVisit((scratch.dir / "pinned.cpp").string(), 7, 4);
    store->SetPin(2, (scratch.dir / "pinned.cpp").string());
    const std::vector<Pin> pins = store->Pins();
    EXPECT_TRUE(pins.size() >= 2);
    if (pins.size() >= 2) {
      EXPECT_EQ(pins[1].path, std::string{"pinned.cpp"});
      EXPECT_EQ(pins[1].line, Index{7});
      EXPECT_EQ(pins[1].column, Index{4});
    }
    // Stamped only after the tables exist: a database claiming v2 with nothing
    // in it would be refused by no one and work for no one.
    EXPECT_EQ(user_version(fresh), std::int64_t{3});
  }

  TEST_CASE("project store: reopening a healthy database still works");
  {
    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> again = ProjectStore::Open(fresh, error);
    EXPECT_TRUE(again != nullptr);
    EXPECT_TRUE(error.empty());
    if (again != nullptr) {
      const std::vector<Pin> pins = again->Pins();
      EXPECT_TRUE(pins.size() >= 2);
      if (pins.size() >= 2) EXPECT_EQ(pins[1].path, std::string{"pinned.cpp"});
      again->SetPin(3, (scratch.dir / "later.cpp").string());
      EXPECT_EQ(again->Pins()[2].path, std::string{"later.cpp"});
    }
    EXPECT_EQ(user_version(fresh), std::int64_t{3});
  }

  TEST_CASE("project store: a pin is one transaction, and no path leaves one open");
  {
    // SetPin is a DELETE (take this file out of whatever slot holds it) and an
    // INSERT (put it in the slot asked for), and the two are one change.
    // Nothing here can crash the process between them to prove the atomicity
    // directly -- that needs fault injection inside SQLite -- so what is checked
    // is everything the transaction is observable through from outside: the
    // result is committed and complete when SetPin returns, and neither the
    // success path nor the failure path leaves the write lock held.
    const std::filesystem::path pinned = scratch.dir / "pins" / "state.db";
    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(pinned, error);
    EXPECT_TRUE(store != nullptr);
    EXPECT_TRUE(error.empty());
    if (store != nullptr) {
      // A connection of our own, and deliberately without the store's five
      // second busy timeout: a question about a lock is then answered now
      // rather than in five seconds, which is the difference between a test
      // that catches a leaked transaction and one that only takes a long time
      // to fail.
      sqlite3* other = nullptr;
      EXPECT_EQ(sqlite3_open(pinned.c_str(), &other), SQLITE_OK);

      // In WAL mode an uncommitted write is invisible to every other
      // connection, so reading the row through `other` is the check that the
      // transaction closed -- not merely that the statements ran.
      const auto ask = [&other](const char* sql, const std::string& file) {
        Stmt stmt{other, sql};
        if (!stmt) return std::int64_t{-1};
        stmt.Text(1, file);
        return stmt.Step() ? stmt.Integer(0) : std::int64_t{-1};
      };
      const auto rows_for = [&ask](const std::string& file) {
        return ask("SELECT COUNT(*) FROM file_pins WHERE file = ?1;", file);
      };
      const auto slot_of = [&ask](const std::string& file) {
        return ask("SELECT COALESCE(MIN(slot), 0) FROM file_pins WHERE file = ?1;", file);
      };
      // Takes the write lock, and gives it straight back. False means somebody
      // else is holding it, and the only other connection to this database is
      // the store's.
      const auto lock_is_free = [&other] {
        if (!ExecSql(other, "BEGIN IMMEDIATE;")) return false;
        ExecSql(other, "ROLLBACK;");
        return true;
      };

      const std::string file = (scratch.dir / "kept.cpp").string();
      store->SetPin(1, file);
      const std::string key = store->Pins()[0].path;
      EXPECT_EQ(key, std::string{"kept.cpp"});
      EXPECT_EQ(rows_for(key), std::int64_t{1});
      EXPECT_EQ(slot_of(key), std::int64_t{1});
      EXPECT_TRUE(lock_is_free());

      // The dedup half: the same file pinned into another slot moves the pin,
      // and the move is one row before and one row after. A window between the
      // DELETE and the INSERT is a moment when it is zero rows.
      store->SetPin(4, file);
      EXPECT_TRUE(store->Pins()[0].path.empty());
      EXPECT_EQ(store->Pins()[3].path, key);
      EXPECT_EQ(rows_for(key), std::int64_t{1});
      EXPECT_EQ(slot_of(key), std::int64_t{4});
      EXPECT_TRUE(lock_is_free());

      // Now the failure path, without patching SQLite: rename the table out
      // from under the store and its statements no longer prepare. The
      // transaction has already begun by then, so this is the rollback -- and
      // if the rollback were missing, the store would sit on the write lock for
      // the rest of the session and `lock_is_free` below would say so.
      EXPECT_TRUE(ExecSql(other, "ALTER TABLE file_pins RENAME TO pins_hidden;"));
      store->SetPin(2, (scratch.dir / "never-pinned.cpp").string());
      EXPECT_TRUE(lock_is_free());
      EXPECT_TRUE(ExecSql(other, "ALTER TABLE pins_hidden RENAME TO file_pins;"));

      // Nothing the failed call touched: the table is exactly as it was, and
      // the store is still usable.
      EXPECT_EQ(rows_for(key), std::int64_t{1});
      EXPECT_EQ(slot_of(key), std::int64_t{4});
      EXPECT_EQ(store->Pins()[3].path, key);
      EXPECT_TRUE(store->Pins()[1].path.empty());
      store->SetPin(2, (scratch.dir / "after.cpp").string());
      EXPECT_EQ(store->Pins()[1].path, std::string{"after.cpp"});
      EXPECT_TRUE(lock_is_free());

      // And ClearPin, the single-statement neighbour, is unaffected by any of
      // it: one statement is its own transaction and needs no wrapper.
      store->ClearPin(4);
      EXPECT_EQ(rows_for(key), std::int64_t{0});
      EXPECT_TRUE(lock_is_free());

      sqlite3_close(other);
    }
  }

  TEST_CASE("project store: a database from a newer koi is refused, and kept");
  {
    // The other half of replacing corrupt files: this one reads perfectly and
    // belongs to a build that knows more than this one. Deleting it would throw
    // away state that build is still using.
    const std::filesystem::path newer = scratch.dir / "newer.db";
    {
      std::string seed_error;
      const std::shared_ptr<ProjectStore> seed = ProjectStore::Open(newer, seed_error);
      EXPECT_TRUE(seed != nullptr);
    }
    {
      sqlite3* db = nullptr;
      EXPECT_EQ(sqlite3_open(newer.c_str(), &db), SQLITE_OK);
      EXPECT_TRUE(ExecSql(db, "PRAGMA user_version = 99;"));
      sqlite3_close(db);
    }
    std::string error{"unset"};
    EXPECT_TRUE(ProjectStore::Open(newer, error) == nullptr);
    EXPECT_TRUE(!error.empty());
    EXPECT_TRUE(std::filesystem::exists(newer));
    EXPECT_EQ(user_version(newer), std::int64_t{99});
    // Refused is not the same as corrupt. This database reads perfectly; a
    // recycler that fired on "the gate said no" rather than on "SQLite said
    // these bytes are not a database" would move a newer koi's live state out
    // from under it.
    EXPECT_FALSE(std::filesystem::exists(beside(newer, ".corrupt")));
  }

  TEST_CASE("project store: refusing a newer database writes nothing into it");
  {
    // "Refused, and kept" above says the file is still there. This says the
    // file is still *the same file*, which is the part the ordering inside
    // Open() decides: the version gate runs before PRAGMA synchronous, before
    // the schema DDL, before the v1->v2 migration, before the stamp and before
    // the prunes. Run the other way round -- DDL first, gate after -- every one
    // of those touches a database this build has already decided it must not
    // use, and the damage is not hypothetical: `CREATE TABLE IF NOT EXISTS`
    // recreates, empty, whatever table a future schema dropped or renamed, and
    // `CREATE INDEX IF NOT EXISTS files_by_ts` adds an index to a table that
    // future build reshaped. The newer koi then opens its own database and
    // finds an older one's furniture in it.
    const std::filesystem::path future = scratch.dir / "future.db";
    {
      sqlite3* db = nullptr;
      EXPECT_EQ(sqlite3_open(future.c_str(), &db), SQLITE_OK);
      // Set the journal mode here so the header is already what a real open
      // would make it: `PRAGMA journal_mode = WAL` on a rollback-journal
      // database rewrites two bytes of the header all by itself, and the
      // comparison below would blame that on the schema step.
      EXPECT_TRUE(ExecSql(db, "PRAGMA journal_mode = WAL;"));
      // A `files` table with a shape v2 does not have -- two columns, not six
      // -- standing in for the reshape a v99 schema is entitled to have made.
      // No other koi table exists, so anything that appears below was created
      // by the open under test.
      EXPECT_TRUE(ExecSql(db, "CREATE TABLE files (path TEXT PRIMARY KEY, era TEXT NOT NULL);"));
      EXPECT_TRUE(ExecSql(db, "BEGIN;"));
      // One row past the prune's cap, so a prune that ran would delete exactly
      // one of them and could not be mistaken for anything else.
      {
        Stmt insert{db, "INSERT INTO files (path, era) VALUES (?1, 'v99');"};
        EXPECT_TRUE(static_cast<bool>(insert));
        bool inserted = true;
        for (int i = 0; i < 5001; ++i) {
          insert.Text(1, "f" + std::to_string(i) + ".cpp");
          inserted = insert.Run() && inserted;
          insert.Reset();
        }
        EXPECT_TRUE(inserted);
      }
      EXPECT_TRUE(ExecSql(db, "COMMIT;"));
      EXPECT_TRUE(ExecSql(db, "PRAGMA user_version = 99;"));
      sqlite3_close(db);
    }

    const std::string before = bytes_of(future);
    EXPECT_TRUE(!before.empty());

    std::string error{"unset"};
    EXPECT_TRUE(ProjectStore::Open(future, error) == nullptr);
    EXPECT_TRUE(error.find("newer than this koi") != std::string::npos);

    // Byte for byte. Nothing else needs to be argued: no table, no index, no
    // stamp, no pruned row can hide inside an identical file.
    EXPECT_TRUE(bytes_of(future) == before);
    EXPECT_FALSE(std::filesystem::exists(beside(future, ".corrupt")));

    // And the same thing named, through a connection of our own, so a failure
    // says which part of the ordering broke rather than "the bytes moved".
    {
      sqlite3* db = nullptr;
      EXPECT_EQ(sqlite3_open(future.c_str(), &db), SQLITE_OK);
      const auto scalar = [db](const char* sql) {
        Stmt stmt{db, sql};
        return (stmt && stmt.Step()) ? stmt.Integer(0) : std::int64_t{-1};
      };
      EXPECT_EQ(scalar("PRAGMA user_version;"), std::int64_t{99});
      // The v99 table, untouched and unpruned; and its shape, so a DDL replay
      // that had somehow widened it would show up here too.
      EXPECT_EQ(scalar("SELECT COUNT(*) FROM files;"), std::int64_t{5001});
      EXPECT_EQ(scalar("SELECT COUNT(*) FROM pragma_table_info('files');"), std::int64_t{2});
      // Nothing this build knows how to create was created.
      EXPECT_EQ(scalar("SELECT COUNT(*) FROM sqlite_master WHERE name IN"
                       " ('symbols', 'co_visits', 'file_pins', 'meta', 'files_by_ts');"),
                std::int64_t{0});
      sqlite3_close(db);
    }

    // The control: the gate is a gate, not a wall. A database this build's own
    // version opens, gets its tables, and comes back stamped.
    const std::filesystem::path current = scratch.dir / "current.db";
    std::string ok_error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(current, ok_error);
    EXPECT_TRUE(store != nullptr);
    EXPECT_TRUE(ok_error.empty());
    EXPECT_EQ(user_version(current), std::int64_t{3});
    {
      sqlite3* db = nullptr;
      EXPECT_EQ(sqlite3_open(current.c_str(), &db), SQLITE_OK);
      {
        Stmt stmt{db, "SELECT COUNT(*) FROM sqlite_master WHERE name IN"
                      " ('files', 'symbols', 'co_visits', 'file_pins', 'meta', 'files_by_ts');"};
        EXPECT_TRUE(stmt && stmt.Step());
        EXPECT_EQ(stmt ? stmt.Integer(0) : std::int64_t{-1}, std::int64_t{6});
      }
      sqlite3_close(db);
    }
  }

  TEST_CASE("project store: only 'not a database' and 'corrupt' are treated as corruption");
  {
    // The classification is the entire safety argument for the recycler, so it
    // is worth pinning directly. Everything on the first list is a statement
    // about the world around the database -- who holds it, what the disk did,
    // what the mode bits say -- and every one of them can come back from a file
    // that is in perfect health.
    for (const int rc : {SQLITE_OK, SQLITE_ERROR, SQLITE_BUSY, SQLITE_LOCKED, SQLITE_READONLY,
                         SQLITE_CANTOPEN, SQLITE_IOERR, SQLITE_FULL, SQLITE_IOERR_READ,
                         SQLITE_IOERR_SHORT_READ, SQLITE_BUSY_SNAPSHOT, SQLITE_READONLY_DBMOVED,
                         SQLITE_CANTOPEN_ISDIR, SQLITE_ABORT, SQLITE_PERM}) {
      EXPECT_FALSE(IsCorruptionCode(rc));
    }
    // The second list is the only one that says the bytes themselves are wrong.
    // The extended forms are `primary | (n << 8)`, so masking covers the whole
    // family -- including members added after this was written.
    for (const int rc : {SQLITE_CORRUPT, SQLITE_NOTADB, SQLITE_CORRUPT_VTAB, SQLITE_CORRUPT_INDEX,
                         SQLITE_CORRUPT_SEQUENCE}) {
      EXPECT_TRUE(IsCorruptionCode(rc));
    }
  }

  TEST_CASE("project store: a database another connection holds locked is not corruption");
  {
    const std::filesystem::path locked = scratch.dir / "locked.db";
    {
      std::string seed_error;
      const std::shared_ptr<ProjectStore> seed = ProjectStore::Open(locked, seed_error);
      EXPECT_TRUE(seed != nullptr);
      if (seed != nullptr) seed->SetPin(1, (scratch.dir / "locked-away.cpp").string());
    }
    // WAL readers do not block, so drop to a rollback journal first; then an
    // exclusive transaction shuts everyone else out, which is exactly what a
    // second koi doing a checkpoint or a migration looks like from outside.
    sqlite3* holder = nullptr;
    EXPECT_EQ(sqlite3_open(locked.c_str(), &holder), SQLITE_OK);
    EXPECT_TRUE(ExecSql(holder, "PRAGMA journal_mode = DELETE;"));
    EXPECT_TRUE(ExecSql(holder, "BEGIN EXCLUSIVE;"));

    // Read it the way the gate does, but on a connection with no busy timeout:
    // a real open would spend its five seconds waiting and then reach this same
    // verdict, and five seconds is not worth spending in a test to learn what
    // the verdict already says. SQLite answers BUSY, and BUSY is not
    // corruption -- if it were, this is the call that would rename a healthy
    // database out from under the connection holding it.
    sqlite3* reader = nullptr;
    EXPECT_EQ(sqlite3_open(locked.c_str(), &reader), SQLITE_OK);
    std::string why{"unset"};
    const Schema verdict = CheckSchemaVersion(reader, 1, why);
    EXPECT_TRUE(verdict == Schema::kUnreadable);
    EXPECT_FALSE(verdict == Schema::kCorrupt);
    EXPECT_FALSE(IsCorruptionCode(sqlite3_extended_errcode(reader)));
    EXPECT_TRUE(!why.empty());
    sqlite3_close(reader);

    EXPECT_TRUE(ExecSql(holder, "ROLLBACK;"));
    sqlite3_close(holder);

    // The lock is gone and so is any doubt: nothing was moved aside, and the
    // database still holds what it was given.
    EXPECT_FALSE(std::filesystem::exists(beside(locked, ".corrupt")));
    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> back = ProjectStore::Open(locked, error);
    EXPECT_TRUE(back != nullptr);
    EXPECT_TRUE(error.empty());
    if (back != nullptr) {
      const std::vector<Pin> pins = back->Pins();
      EXPECT_TRUE(!pins.empty());
      if (!pins.empty()) EXPECT_EQ(pins.front().path, std::string{"locked-away.cpp"});
    }
  }

  TEST_CASE("project store: a first-statement failure that is not corruption keeps the database");
  {
    // The same shape as a corrupt file, all the way down: a healthy database
    // whose very first statement fails, in a directory the recycler could
    // rename in without any trouble. The only thing standing between this
    // database and a `.corrupt` suffix is that SQLite said CANTOPEN rather than
    // NOTADB -- it cannot reach the write-ahead log, which says nothing at all
    // about the bytes in the database itself.
    const std::filesystem::path sidecar = scratch.dir / "sidecar.db";
    {
      std::string seed_error;
      const std::shared_ptr<ProjectStore> seed = ProjectStore::Open(sidecar, seed_error);
      EXPECT_TRUE(seed != nullptr);
      if (seed != nullptr) seed->SetPin(1, (scratch.dir / "sidecar.cpp").string());
    }
    // Closing the last connection checkpoints the log back into the database
    // and removes it, so the pin above is in the main file before we start.
    const std::filesystem::path wal = beside(sidecar, "-wal");
    std::error_code ec;
    std::filesystem::remove(beside(sidecar, "-shm"), ec);
    {
      std::ofstream out{wal, std::ios::binary};
      out << std::string(64, 'x');
    }
    if (::getuid() == 0) {
      // root reads through the mode bits, so there is no unreadable log to make.
      EXPECT_TRUE(true);
    } else {
      std::filesystem::permissions(wal, std::filesystem::perms::none,
                                   std::filesystem::perm_options::replace, ec);
      EXPECT_FALSE(static_cast<bool>(ec));

      std::string error{"unset"};
      EXPECT_TRUE(ProjectStore::Open(sidecar, error) == nullptr);
      EXPECT_TRUE(!error.empty());
      EXPECT_FALSE(std::filesystem::exists(beside(sidecar, ".corrupt")));
      EXPECT_TRUE(std::filesystem::exists(sidecar));

      std::filesystem::permissions(
          wal, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
          std::filesystem::perm_options::replace, ec);
    }
    std::filesystem::remove(wal, ec);

    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> back = ProjectStore::Open(sidecar, error);
    EXPECT_TRUE(back != nullptr);
    EXPECT_TRUE(error.empty());
    if (back != nullptr) {
      const std::vector<Pin> pins = back->Pins();
      EXPECT_TRUE(!pins.empty());
      if (!pins.empty()) EXPECT_EQ(pins.front().path, std::string{"sidecar.cpp"});
    }
  }

  TEST_CASE("project store: a corrupt database that cannot be moved aside is refused, not deleted");
  {
    const std::filesystem::path pen = scratch.dir / "sealed";
    std::filesystem::create_directories(pen);
    const std::filesystem::path trapped = pen / "state.db";
    const std::string trapped_bytes(4096, 'T');
    {
      std::ofstream out{trapped, std::ios::binary};
      out << trapped_bytes;
    }
    if (::getuid() == 0) {
      // root renames through the directory's mode bits, so the rename cannot
      // be made to fail this way.
      EXPECT_TRUE(true);
    } else {
      // The scratch fixture removes this tree on the way out, which needs the
      // directory writable again however this case ends.
      struct RestoreDir {
        std::filesystem::path dir;
        ~RestoreDir() {
          std::error_code ec;
          std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                       std::filesystem::perm_options::replace, ec);
        }
      } restore_dir{pen};
      std::error_code ec;
      std::filesystem::permissions(
          pen, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
          std::filesystem::perm_options::replace, ec);
      EXPECT_FALSE(static_cast<bool>(ec));

      std::string error{"unset"};
      EXPECT_TRUE(ProjectStore::Open(trapped, error) == nullptr);
      // Both halves of the story, because either one alone is misleading: the
      // database is unusable, *and* it is still sitting there.
      EXPECT_TRUE(error.find("corrupt") != std::string::npos);
      EXPECT_TRUE(error.find("aside") != std::string::npos);
      // No fall-through to deleting it and none to using it. A corrupt database
      // that cannot be moved is simply no database.
      EXPECT_TRUE(std::filesystem::exists(trapped));
      EXPECT_TRUE(bytes_of(trapped) == trapped_bytes);
      EXPECT_FALSE(std::filesystem::exists(beside(trapped, ".corrupt")));
    }
  }
}

void LegacyProjectStateIsSeededNotStolen() {
  TEST_CASE("project: adopting a pre-digest state directory copies it");

  const Scratch scratch{"koi-legacy-adopt"};

  const std::filesystem::path was_root = ProjectRoot();
  struct Restore {
    std::filesystem::path back;
    ~Restore() {
      SetProjectRoot(back);
      SetProjectDbPath({});
    }
  } restore{was_root};
  SetProjectDbPath({});  // so the paths come from ProjectDir, not an override

  const char* home = std::getenv("HOME");
  EXPECT_TRUE((home != nullptr) && (*home != '\0'));
  if ((home == nullptr) || (*home == '\0')) return;
  const std::filesystem::path base =
      std::filesystem::path{home} / ".local" / "share" / "ronin";

  const auto write_marker = [](const std::filesystem::path& dir, std::string_view what) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out{dir / "marker.txt", std::ios::binary | std::ios::trunc};
    out << what;
  };
  const auto marker_of = [](const std::filesystem::path& dir) {
    std::ifstream in{dir / "marker.txt", std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  };
  // What ProjectDir resolves for a root, and the pre-digest name it looks up.
  const auto dir_for = [](const std::filesystem::path& root) {
    SetProjectRoot(root);
    return ProjectDbPath().parent_path();
  };
  const auto legacy_for = [&base](const std::filesystem::path& root) {
    return base / FlattenPathComponent(root.string());
  };

  // Two roots whose flattened names collide -- which is the whole reason the
  // digest exists, and the reason the legacy name cannot identify an owner.
  const std::filesystem::path slash = scratch.dir / "w" / "a" / "b";
  const std::filesystem::path dash = scratch.dir / "w" / "a-b";
  EXPECT_EQ(legacy_for(slash), legacy_for(dash));
  const std::filesystem::path legacy = legacy_for(slash);
  EXPECT_TRUE(legacy.filename() != std::filesystem::path{});

  std::error_code ec;
  std::filesystem::remove_all(legacy, ec);
  write_marker(legacy, "legacy");
  write_marker(legacy / "nested", "deep");

  const std::filesystem::path dir_slash = dir_for(slash);
  const std::filesystem::path dir_dash = base / ProjectDirName(dash);
  std::filesystem::remove_all(dir_slash, ec);
  std::filesystem::remove_all(dir_dash, ec);
  EXPECT_TRUE(dir_slash != dir_dash);
  EXPECT_TRUE(dir_slash != legacy);

  // The first of the two to open adopts the legacy state...
  EXPECT_EQ(dir_for(slash), dir_slash);
  EXPECT_TRUE(std::filesystem::exists(dir_slash));
  EXPECT_EQ(marker_of(dir_slash), std::string{"legacy"});
  EXPECT_EQ(marker_of(dir_slash / "nested"), std::string{"deep"});
  // ...without taking it. A move gave one colliding project the other's pins,
  // visits and last positions and left the owner with an empty database it could
  // never recover, because the legacy directory was gone.
  EXPECT_TRUE(std::filesystem::is_directory(legacy));
  EXPECT_EQ(marker_of(legacy), std::string{"legacy"});

  // So the other one seeds from the same history too.
  EXPECT_EQ(dir_for(dash), dir_dash);
  EXPECT_TRUE(std::filesystem::exists(dir_dash));
  EXPECT_EQ(marker_of(dir_dash), std::string{"legacy"});
  EXPECT_TRUE(std::filesystem::is_directory(legacy));

  // Adoption is one-shot: ProjectDir runs it on every call, and the "my
  // directory already exists" gate is what keeps a live database from being
  // overwritten by the stale copy behind it.
  write_marker(dir_slash, "mine");
  write_marker(dir_dash, "mine too");
  EXPECT_EQ(dir_for(slash), dir_slash);
  EXPECT_EQ(dir_for(slash), dir_slash);
  EXPECT_EQ(dir_for(dash), dir_dash);
  EXPECT_EQ(marker_of(dir_slash), std::string{"mine"});
  EXPECT_EQ(marker_of(dir_dash), std::string{"mine too"});
  EXPECT_EQ(marker_of(legacy), std::string{"legacy"});

  // A project with no legacy directory adopts nothing; its state directory is
  // made by the first open, as ever.
  const std::filesystem::path solo = scratch.dir / "solo";
  const std::filesystem::path legacy_solo = legacy_for(solo);
  const std::filesystem::path dir_solo = base / ProjectDirName(solo);
  std::filesystem::remove_all(legacy_solo, ec);
  std::filesystem::remove_all(dir_solo, ec);
  EXPECT_EQ(dir_for(solo), dir_solo);
  EXPECT_FALSE(std::filesystem::exists(dir_solo));
  EXPECT_FALSE(std::filesystem::exists(legacy_solo));
  {
    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(ProjectDbPath(), error);
    EXPECT_TRUE(store != nullptr);
    EXPECT_TRUE(error.empty());
  }
  EXPECT_TRUE(std::filesystem::is_directory(dir_solo));
  EXPECT_FALSE(std::filesystem::exists(dir_solo / "marker.txt"));
  EXPECT_FALSE(std::filesystem::exists(legacy_solo));

  for (const std::filesystem::path& gone : {legacy, dir_slash, dir_dash, dir_solo}) {
    std::filesystem::remove_all(gone, ec);
  }
}

void DeepProjectPathsStillGetAStateDirectory() {
  TEST_CASE("project: a path past NAME_MAX still resolves and opens its state");

  const Scratch scratch{"koi-deep-path"};
  const std::filesystem::path was_root = ProjectRoot();
  struct Restore {
    std::filesystem::path back;
    ~Restore() {
      SetProjectRoot(back);
      SetProjectDbPath({});
    }
  } restore{was_root};
  SetProjectDbPath({});

  // Deep enough that the flattened path alone would blow the 255-byte cap on
  // one directory entry -- which used to fail create_directories with
  // ENAMETOOLONG and take the database, jump list, key log and
  // picker state down with it, leaving only a status warning that scrolled away.
  std::filesystem::path deep = scratch.dir;
  for (int i = 0; i < 15; ++i) deep /= "abcdefghijklmnopqrst";
  EXPECT_TRUE(deep.string().size() > 300);

  const std::string name = ProjectDirName(deep);
  EXPECT_TRUE(name.size() <= 255);
  EXPECT_TRUE(name.size() <= 209);  // 200-byte label + "-" + 8 hex digits

  // The label keeps the tail -- the distinctive part -- and the digest is of
  // the whole canonical path, so two deep paths sharing a long tail still get
  // distinct directories.
  EXPECT_TRUE(name.find("abcdefghijklmnopqrst") != std::string::npos);
  std::filesystem::path other = scratch.dir / "elsewhere";
  for (int i = 0; i < 15; ++i) other /= "abcdefghijklmnopqrst";
  EXPECT_TRUE(ProjectDirName(other) != name);

  // Short paths are untouched by the cap.
  const std::filesystem::path shallow = scratch.dir / "w" / "proj";
  const std::string shallow_name = ProjectDirName(shallow);
  EXPECT_TRUE(shallow_name.find("w-proj-") != std::string::npos);

  // End to end: the deep project's store opens and holds state.
  SetProjectRoot(deep);
  const std::filesystem::path db = ProjectDbPath();
  EXPECT_TRUE(!db.empty());
  std::error_code ec;
  std::filesystem::remove_all(db.parent_path(), ec);
  {
    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(db, error);
    EXPECT_TRUE(store != nullptr);
    EXPECT_TRUE(error.empty());
    if (store != nullptr) {
      store->SetPin(1, (deep / "deep.cpp").string());
      EXPECT_EQ(store->Pins()[0].path, std::string{"deep.cpp"});
    }
  }
  std::filesystem::remove_all(db.parent_path(), ec);
}

// A git repository rooted at $HOME -- dotfiles -- used to be the one repository
// koi could not see. The upward walk broke on "this is $HOME" before it tested
// $HOME for a marker, so from anywhere below it the repository was invisible and
// the cwd fallback made every directory its own project with its own state
// database; from $HOME itself the same fallback happened to return $HOME. One
// repository, a different root per depth, and pins, visits and last positions
// scattered across a database per directory.
void ProjectRootFindsARepositoryRootedAtHome() {
  TEST_CASE("project: a repository rooted at $HOME is one project, not one per directory");

  const Scratch scratch{"koi-home-root"};
  const std::filesystem::path home = scratch.dir / "home";
  const std::filesystem::path sub = home / "sub";
  const std::filesystem::path deeper = sub / "deeper";
  std::error_code ec;
  std::filesystem::create_directories(deeper, ec);

  const auto mark = [](const std::filesystem::path& dir, const char* marker) {
    std::error_code make;
    std::filesystem::create_directories(dir / marker, make);
  };
  const auto unmark = [](const std::filesystem::path& dir, const char* marker) {
    std::error_code drop;
    std::filesystem::remove_all(dir / marker, drop);
  };

  // No marker anywhere: each directory is its own project. That is the fallback,
  // and nothing here changes it.
  EXPECT_EQ(FindProjectRoot(deeper, home), deeper);
  EXPECT_EQ(FindProjectRoot(sub, home), sub);
  EXPECT_EQ(FindProjectRoot(home, home), home);

  // $HOME is the repository. The same root from every depth is the whole point:
  // one state database that accumulates instead of one per directory visited.
  mark(home, ".git");
  EXPECT_EQ(FindProjectRoot(home, home), home);
  EXPECT_EQ(FindProjectRoot(sub, home), home);
  EXPECT_EQ(FindProjectRoot(deeper, home), home);
  // `.ronin` is the other marker, and the boundary reads it the same way.
  unmark(home, ".git");
  mark(home, ".ronin");
  EXPECT_EQ(FindProjectRoot(deeper, home), home);
  unmark(home, ".ronin");

  // A nearer marker still wins: a checkout inside a dotfiles $HOME is its own
  // project, exactly as it was before.
  mark(home, ".git");
  mark(sub, ".git");
  EXPECT_EQ(FindProjectRoot(deeper, home), sub);
  EXPECT_EQ(FindProjectRoot(sub, home), sub);
  EXPECT_EQ(FindProjectRoot(home, home), home);
  unmark(sub, ".git");
  unmark(home, ".git");

  // The stop is still a stop. It bounds the walk at $HOME, so a marker *above*
  // $HOME -- in /home, or at / -- cannot capture every directory on the machine.
  mark(scratch.dir, ".git");
  EXPECT_EQ(FindProjectRoot(deeper, home), deeper);
  EXPECT_EQ(FindProjectRoot(home, home), home);
  // ...and the stop is the only thing preventing it, which is what makes the
  // two lines above a decision rather than an accident of the fixture.
  EXPECT_EQ(FindProjectRoot(deeper, std::filesystem::path{}), scratch.dir);
  unmark(scratch.dir, ".git");
}

// Everything the editor keeps beside the database -- the picker's last
// selection, the key log -- is derived from the state
// directory, and the state directory was derived from scratch on every one of
// those calls: canonicalising the project path is a stat per component, and the
// digest, the flatten and the legacy probe all followed it. Opening and closing
// a picker paid for it twice.
void ProjectStatePathsAreDerivedOncePerRoot() {
  TEST_CASE("project: the state directory is memoised, and the memo follows the root");

  const Scratch scratch{"koi-dir-memo"};
  const std::filesystem::path was_root = ProjectRoot();
  struct Restore {
    std::filesystem::path back;
    ~Restore() {
      SetProjectRoot(back);
      SetProjectDbPath({});
    }
  } restore{was_root};
  SetProjectDbPath({});  // so the paths come from ProjectDir, not an override

  const std::filesystem::path alpha = scratch.dir / "alpha";
  const std::filesystem::path beta = scratch.dir / "beta";
  std::error_code ec;
  std::filesystem::create_directories(alpha, ec);
  std::filesystem::create_directories(beta, ec);

  SetProjectRoot(alpha);
  const std::filesystem::path db_alpha = ProjectDbPath();
  EXPECT_TRUE(!db_alpha.empty());
  // The memo's hit path. Repeat calls are what the editor actually does, and
  // they must be the same answer every time -- everything below hangs off it.
  EXPECT_EQ(ProjectDbPath(), db_alpha);
  EXPECT_EQ(ProjectDbPath(), db_alpha);
  EXPECT_EQ(LastPickerStatePath().parent_path(), db_alpha.parent_path());
  EXPECT_EQ(KeyLogDbPath().parent_path(), db_alpha.parent_path());
  EXPECT_EQ(KeyLogDbPath().parent_path(), db_alpha.parent_path());

  // Moving the root invalidates it. A memo that did not would hand the second
  // project the first one's database -- its pins, its visits, its positions.
  SetProjectRoot(beta);
  const std::filesystem::path db_beta = ProjectDbPath();
  EXPECT_TRUE(db_beta != db_alpha);
  EXPECT_EQ(ProjectDbPath(), db_beta);
  EXPECT_EQ(LastPickerStatePath().parent_path(), db_beta.parent_path());

  // And back to the first: the answer for a root is the one it always was, so
  // state written under it is still found.
  SetProjectRoot(alpha);
  EXPECT_EQ(ProjectDbPath(), db_alpha);

  // An explicit override beats the memo outright, and clearing it falls back to
  // the memo for whatever root is current. That pair is the pattern the rest of
  // the suite is written in, so it has to keep working exactly.
  const std::filesystem::path chosen = scratch.dir / "explicit.db";
  SetProjectDbPath(chosen);
  EXPECT_EQ(ProjectDbPath(), chosen);
  EXPECT_EQ(LastPickerStatePath().parent_path(), scratch.dir);
  SetProjectDbPath({});
  EXPECT_EQ(ProjectDbPath(), db_alpha);
  SetProjectRoot(beta);
  EXPECT_EQ(ProjectDbPath(), db_beta);
}

// Two producers used to reach one database with two spellings of one path. The
// editor keys files against the project root; the file filter (`find . -printf
// '%P\n'`) keys them against koi's current directory, and koi never chdirs. Run
// from the root the two agree and nothing looked wrong. Run one directory down
// they disagree on every path, and since every read resolves a stored path
// against the root, the whole symbols table became invisible: "nothing at symbol
// N" from every jump, and a current file that could never reach the ranked head.
void ProjectPathsAreKeyedTheSameFromEveryDirectory() {
  const Scratch scratch{"koi-project-key"};
  const std::filesystem::path root = scratch.dir / "proj";
  const std::filesystem::path sub = root / "sub";
  std::error_code ec;
  std::filesystem::create_directories(sub, ec);
  {
    std::ofstream out{sub / "file.cpp", std::ios::binary};
    out << "int Sym() { return 0; }\n";
  }
  {
    std::ofstream out{sub / "other.cpp", std::ios::binary};
    out << "int Other() { return 1; }\n";
  }

  const std::filesystem::path was_root = ProjectRoot();
  const std::filesystem::path was_cwd = std::filesystem::current_path();
  struct Restore {
    std::filesystem::path root;
    std::filesystem::path cwd;
    ~Restore() {
      SetProjectRoot(root);
      SetProjectDbPath({});
      std::error_code ec;
      std::filesystem::current_path(cwd, ec);
    }
  } restore{was_root, was_cwd};

  // koi started in `sub`, with the project root one level up: the arrangement
  // in which the two spellings differ.
  SetProjectRoot(root);
  std::filesystem::current_path(sub);

  const auto scalar = [](const std::filesystem::path& path, const char* sql) {
    sqlite3* handle = nullptr;
    std::int64_t value = -1;
    if (sqlite3_open(path.c_str(), &handle) == SQLITE_OK) {
      Stmt stmt{handle, sql};
      if (stmt && stmt.Step()) value = stmt.Integer(0);
    }
    sqlite3_close(handle);
    return value;
  };
  const auto text_of = [](const std::filesystem::path& path, const char* sql) {
    sqlite3* handle = nullptr;
    std::string value;
    if (sqlite3_open(path.c_str(), &handle) == SQLITE_OK) {
      Stmt stmt{handle, sql};
      if (stmt && stmt.Step()) value = stmt.Column(0);
    }
    sqlite3_close(handle);
    return value;
  };

  TEST_CASE("project store: every spelling of a path lands on one key");
  {
    const std::filesystem::path db = scratch.dir / "keyed.db";
    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(db, error);
    EXPECT_TRUE(store != nullptr);
    EXPECT_TRUE(error.empty());
    if (store == nullptr) return;

    // The file filter's spelling for a symbol, the editor's for the visit --
    // the exact pair that used to produce two rows describing one file. Both
    // are paths valid from koi's current directory, which is the store's rule;
    // they land on the one root-relative key.
    store->RecordSymbolVisit("Sym", "file.cpp", 3);
    store->RecordVisit((sub / "file.cpp").string(), 3, 1);

    // The symbol survives the read gate, which resolves against the root. This
    // is the assertion that used to come back empty.
    const std::vector<SymbolVisit> hot = store->HotSymbols(10);
    EXPECT_EQ(hot.size(), 1u);
    if (!hot.empty()) {
      EXPECT_EQ(hot.front().symbol, std::string{"Sym"});
      EXPECT_EQ(hot.front().file, std::string{"sub/file.cpp"});
      EXPECT_EQ(hot.front().line, Index{3});
    }

    // And a lookup in the *other* spelling finds the row the first one wrote.
    Index line = 0;
    Index column = 0;
    EXPECT_TRUE(store->LastVisit("file.cpp", line, column));
    EXPECT_EQ(line, Index{3});
    EXPECT_TRUE(store->LastVisit((sub / "file.cpp").string(), line, column));
    EXPECT_EQ(line, Index{3});

    // Three spellings of one file, one row, visits summed rather than split.
    store->RecordSymbolVisit("Sym", "./file.cpp", 3);
    store->RecordSymbolVisit("Sym", (sub / "file.cpp").string(), 3);
    EXPECT_EQ(scalar(db, "SELECT COUNT(*) FROM symbols;"), std::int64_t{1});
    EXPECT_EQ(scalar(db, "SELECT visits FROM symbols;"), std::int64_t{3});
    EXPECT_EQ(text_of(db, "SELECT file FROM symbols;"), std::string{"sub/file.cpp"});
    EXPECT_EQ(scalar(db, "SELECT COUNT(*) FROM files;"), std::int64_t{1});
    EXPECT_EQ(text_of(db, "SELECT path FROM files;"), std::string{"sub/file.cpp"});

    // A file cannot co-visit itself, and two spellings of it are still itself.
    // The guard runs on the keys, not on the strings the caller happened to
    // have.
    store->RecordCoVisit((sub / "file.cpp").string(), "file.cpp");
    EXPECT_EQ(scalar(db, "SELECT COUNT(*) FROM co_visits;"), std::int64_t{0});

    store->RecordCoVisit((sub / "file.cpp").string(), "other.cpp");
    EXPECT_EQ(scalar(db, "SELECT COUNT(*) FROM co_visits;"), std::int64_t{1});
    EXPECT_EQ(text_of(db, "SELECT from_file || ' -> ' || to_file FROM co_visits;"),
              std::string{"sub/file.cpp -> sub/other.cpp"});

    TEST_CASE("project store: the current file reaches the ranked head from a subdirectory");
    // The rows are the scanner's, in the filter's spelling; `current_file` is
    // the editor's, in the root's. Group 0 is the current file's own symbols,
    // and comparing the two raw strings never once matched.
    std::vector<Symbol> rows{
        {"other.cpp", 1, 1, "Other"},
        {"file.cpp", 3, 1, "Sym"},
    };
    const size_t head = store->RankSymbols(rows, (sub / "file.cpp").string());
    EXPECT_EQ(head, std::size_t{2});
    EXPECT_EQ(rows.front().name, std::string{"Sym"});
    // Ranked, never rewritten: the caller opens these paths, and they are valid
    // where the scan that produced them ran.
    EXPECT_EQ(rows.front().path, std::string{"file.cpp"});
    // Second only because the co-visit above gave it a score -- which is itself
    // a keyed lookup, from a root-relative `from` to a filter-spelled row.
    EXPECT_EQ(rows.back().name, std::string{"Other"});
    EXPECT_EQ(rows.back().path, std::string{"other.cpp"});
  }

  TEST_CASE("project store: a v2 database gives up its pinned positions, keeps its files");
  {
    // The upgrade every existing database actually takes, and the one the v1
    // test above cannot stand in for: the path rewrite must NOT run again here
    // (re-keying an already-keyed path damages it), while the pin conversion
    // must. Gating both steps on kSchemaVersion instead of on the version each
    // one upgrades *from* is exactly how that goes wrong.
    const std::filesystem::path v2 = scratch.dir / "v2.db";
    {
      sqlite3* raw = nullptr;
      EXPECT_EQ(sqlite3_open(v2.c_str(), &raw), SQLITE_OK);
      EXPECT_TRUE(ExecSql(
          raw,
          "CREATE TABLE files (path TEXT PRIMARY KEY, visits INTEGER NOT NULL DEFAULT 0,"
          " edits INTEGER NOT NULL DEFAULT 0, last_ts REAL NOT NULL DEFAULT 0,"
          " last_line INTEGER NOT NULL DEFAULT 1, last_col INTEGER NOT NULL DEFAULT 0);"
          "CREATE TABLE symbols (file TEXT NOT NULL, symbol TEXT NOT NULL,"
          " visits INTEGER NOT NULL DEFAULT 0, last_ts REAL NOT NULL DEFAULT 0,"
          " line INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (file, symbol));"
          "CREATE TABLE co_visits (from_file TEXT NOT NULL, to_file TEXT NOT NULL,"
          " count INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (from_file, to_file));"
          "CREATE TABLE pins (slot INTEGER PRIMARY KEY, file TEXT NOT NULL,"
          " line INTEGER NOT NULL DEFAULT 1, col INTEGER NOT NULL DEFAULT 0);"
          "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
          // Two pins in one file, 61 lines apart, and a third elsewhere: the
          // shape a real v2 database was found in.
          "INSERT INTO pins VALUES(1, 'sub/file.cpp', 3503, 1);"
          "INSERT INTO pins VALUES(3, 'sub/file.cpp', 3442, 1);"
          "INSERT INTO pins VALUES(4, 'sub/other.cpp', 12, 0);"
          "INSERT INTO files VALUES('sub/file.cpp', 4, 1, 500.0, 90, 6);"
          "INSERT INTO symbols VALUES('sub/file.cpp', 'Sym', 2, 100.0, 7);"
          "INSERT INTO co_visits VALUES('sub/file.cpp', 'sub/other.cpp', 5);"
          "PRAGMA user_version = 2;"));
      sqlite3_close(raw);
    }

    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(v2, error);
    EXPECT_TRUE(store != nullptr);
    EXPECT_TRUE(error.empty());
    if (store == nullptr) return;
    EXPECT_EQ(scalar(v2, "PRAGMA user_version;"), std::int64_t{3});

    // The two pins in one file collapse to the lower slot; the higher is left
    // empty rather than backfilled with a guess.
    const std::vector<Pin> pins = store->Pins();
    EXPECT_EQ(pins[0].path, std::string{"sub/file.cpp"});
    EXPECT_TRUE(pins[2].path.empty());
    EXPECT_EQ(pins[3].path, std::string{"sub/other.cpp"});
    // Neither 3503 nor 3442 survives: the position comes from `files` now, and
    // for a file that was never visited there is none.
    EXPECT_EQ(pins[0].line, Index{90});
    EXPECT_EQ(pins[0].column, Index{6});
    EXPECT_EQ(pins[3].line, Index{1});
    EXPECT_EQ(scalar(v2, "SELECT COUNT(*) FROM sqlite_master"
                         " WHERE type = 'table' AND name = 'pins';"),
              std::int64_t{0});

    // Untouched, which is the half a wrong gate would break: every one of these
    // is already keyed, and a second rewrite would prefix them again.
    EXPECT_EQ(text_of(v2, "SELECT path FROM files;"), std::string{"sub/file.cpp"});
    EXPECT_EQ(text_of(v2, "SELECT file FROM symbols;"), std::string{"sub/file.cpp"});
    EXPECT_EQ(text_of(v2, "SELECT from_file || ' -> ' || to_file FROM co_visits;"),
              std::string{"sub/file.cpp -> sub/other.cpp"});
    EXPECT_EQ(scalar(v2, "SELECT visits FROM files;"), std::int64_t{4});
  }

  TEST_CASE("project store: a v1 database is rewritten into one spelling, once");
  {
    const std::filesystem::path legacy = scratch.dir / "legacy.db";
    {
      sqlite3* raw = nullptr;
      EXPECT_EQ(sqlite3_open(legacy.c_str(), &raw), SQLITE_OK);
      // The v1 shape, written out here rather than borrowed: this test is the
      // record of what the migration has to be able to read.
      EXPECT_TRUE(ExecSql(
          raw,
          "CREATE TABLE files (path TEXT PRIMARY KEY, visits INTEGER NOT NULL DEFAULT 0,"
          " edits INTEGER NOT NULL DEFAULT 0, last_ts REAL NOT NULL DEFAULT 0,"
          " last_line INTEGER NOT NULL DEFAULT 1, last_col INTEGER NOT NULL DEFAULT 0);"
          "CREATE TABLE symbols (file TEXT NOT NULL, symbol TEXT NOT NULL,"
          " visits INTEGER NOT NULL DEFAULT 0, last_ts REAL NOT NULL DEFAULT 0,"
          " line INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (file, symbol));"
          "CREATE TABLE co_visits (from_file TEXT NOT NULL, to_file TEXT NOT NULL,"
          " count INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (from_file, to_file));"
          "CREATE TABLE pins (slot INTEGER PRIMARY KEY, file TEXT NOT NULL,"
          " line INTEGER NOT NULL DEFAULT 1, col INTEGER NOT NULL DEFAULT 0);"
          "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"));

      const auto symbol_row = [raw](std::string_view file, std::string_view name,
                                    std::int64_t visits, double ts, std::int64_t line) {
        Stmt stmt{raw, "INSERT INTO symbols(file, symbol, visits, last_ts, line)"
                       " VALUES(?1,?2,?3,?4,?5);"};
        stmt.Text(1, file);
        stmt.Text(2, name);
        stmt.Int(3, visits);
        stmt.Real(4, ts);
        stmt.Int(5, line);
        EXPECT_TRUE(stmt.Run());
      };
      const auto co_visit_row = [raw](std::string_view from, std::string_view to,
                                      std::int64_t count) {
        Stmt stmt{raw, "INSERT INTO co_visits(from_file, to_file, count) VALUES(?1,?2,?3);"};
        stmt.Text(1, from);
        stmt.Text(2, to);
        stmt.Int(3, count);
        EXPECT_TRUE(stmt.Run());
      };

      const auto exec = [raw](const std::string& sql) { EXPECT_TRUE(ExecSql(raw, sql.c_str())); };
      // Already in the v2 spelling, and left exactly as they are: both tables
      // only ever saw the editor's paths, so there is nothing to rewrite and a
      // rewrite would be the thing that damaged them.
      exec("INSERT INTO files VALUES('sub/file.cpp', 4, 1, 500.0, 12, 3);");
      exec("INSERT INTO pins VALUES(1, 'sub/other.cpp', 9, 4);");

      // Three spellings of one file's one symbol, split across three rows. The
      // line is only on the middle one, and the newest row does not carry it.
      // The middle one is the *editor's* root-relative spelling, which from
      // this directory does not mean what the cwd rule would make of it -- the
      // migration has to recognise it anyway, because v1 really did hold both.
      symbol_row("file.cpp", "Sym", 2, 100.0, 0);
      symbol_row("sub/file.cpp", "Sym", 3, 200.0, 7);
      symbol_row((sub / "file.cpp").string(), "Sym", 1, 250.0, 0);
      symbol_row("other.cpp", "Other", 1, 150.0, 2);
      // Nothing on disk answers to this, in any spelling: no reader could ever
      // have used it, and it does not survive.
      symbol_row("nowhere/gone.cpp", "Ghost", 9, 300.0, 1);

      co_visit_row("sub/file.cpp", "other.cpp", 2);   // merges with the next
      co_visit_row("file.cpp", "sub/other.cpp", 3);   // ...into one pair
      co_visit_row("file.cpp", "sub/file.cpp", 5);    // the same file twice
      co_visit_row("sub/file.cpp", "nowhere/gone.cpp", 4);  // one side is gone

      EXPECT_TRUE(ExecSql(raw, "PRAGMA user_version = 1;"));
      sqlite3_close(raw);
    }
    EXPECT_EQ(scalar(legacy, "PRAGMA user_version;"), std::int64_t{1});

    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(legacy, error);
    EXPECT_TRUE(store != nullptr);
    EXPECT_TRUE(error.empty());
    if (store == nullptr) return;

    EXPECT_EQ(scalar(legacy, "PRAGMA user_version;"), std::int64_t{3});

    EXPECT_EQ(scalar(legacy, "SELECT COUNT(*) FROM symbols;"), std::int64_t{2});
    EXPECT_EQ(scalar(legacy, "SELECT COUNT(*) FROM symbols WHERE symbol = 'Ghost';"),
              std::int64_t{0});
    EXPECT_EQ(scalar(legacy, "SELECT visits FROM symbols WHERE symbol = 'Sym';"),
              std::int64_t{6});
    EXPECT_EQ(scalar(legacy, "SELECT line FROM symbols WHERE symbol = 'Sym';"), std::int64_t{7});
    EXPECT_EQ(text_of(legacy, "SELECT file FROM symbols WHERE symbol = 'Sym';"),
              std::string{"sub/file.cpp"});
    EXPECT_EQ(text_of(legacy, "SELECT file FROM symbols WHERE symbol = 'Other';"),
              std::string{"sub/other.cpp"});
    EXPECT_EQ(scalar(legacy, "SELECT CAST(last_ts AS INTEGER) FROM symbols WHERE symbol = 'Sym';"),
              std::int64_t{250});

    // One pair left: the self-pair and the pair pointing at a file that is not
    // there are both gone, and the two spellings of the surviving pair merged.
    EXPECT_EQ(scalar(legacy, "SELECT COUNT(*) FROM co_visits;"), std::int64_t{1});
    EXPECT_EQ(text_of(legacy, "SELECT from_file || ' -> ' || to_file FROM co_visits;"),
              std::string{"sub/file.cpp -> sub/other.cpp"});
    EXPECT_EQ(scalar(legacy, "SELECT count FROM co_visits;"), std::int64_t{5});

    // Rewritten rows are rows the readers can finally see.
    const std::vector<SymbolVisit> hot = store->HotSymbols(10);
    EXPECT_EQ(hot.size(), 2u);
    if (!hot.empty()) EXPECT_EQ(hot.front().symbol, std::string{"Sym"});

    // Untouched, down to the counters.
    EXPECT_EQ(scalar(legacy, "SELECT COUNT(*) FROM files;"), std::int64_t{1});
    EXPECT_EQ(text_of(legacy, "SELECT path FROM files;"), std::string{"sub/file.cpp"});
    EXPECT_EQ(scalar(legacy, "SELECT visits FROM files;"), std::int64_t{4});
    EXPECT_EQ(scalar(legacy, "SELECT last_line FROM files;"), std::int64_t{12});
    // v3 keeps the file and drops the line: the slot survives, the position it
    // named does not, and the position it reports now is the one `files` has --
    // none, for this file, so line 1.
    EXPECT_EQ(scalar(legacy, "SELECT COUNT(*) FROM sqlite_master"
                             " WHERE type = 'table' AND name = 'pins';"),
              std::int64_t{0});
    EXPECT_EQ(text_of(legacy, "SELECT file FROM file_pins;"), std::string{"sub/other.cpp"});
    EXPECT_EQ(scalar(legacy, "SELECT slot FROM file_pins;"), std::int64_t{1});
    EXPECT_EQ(store->Pins()[0].path, std::string{"sub/other.cpp"});
    EXPECT_EQ(store->Pins()[0].line, Index{1});

    TEST_CASE("project store: the rewrite is gated on the stamp, not repeated");
    {
      // The stamp is what decides, not the shape of the data: a v2 database
      // never enters the rewrite again. That matters because re-keying an
      // already-keyed path is not a no-op -- "sub/file.cpp" read from inside
      // "sub" would first be tried as "sub/sub/file.cpp" -- and because a
      // rewrite that ran on every open would pay for the whole table every
      // time. Rows, counters and merges all come back exactly as they were.
      std::string again_error{"unset"};
      const std::shared_ptr<ProjectStore> again = ProjectStore::Open(legacy, again_error);
      EXPECT_TRUE(again != nullptr);
      EXPECT_TRUE(again_error.empty());
      EXPECT_EQ(scalar(legacy, "PRAGMA user_version;"), std::int64_t{3});
      EXPECT_EQ(scalar(legacy, "SELECT COUNT(*) FROM symbols;"), std::int64_t{2});
      EXPECT_EQ(text_of(legacy, "SELECT file FROM symbols WHERE symbol = 'Sym';"),
                std::string{"sub/file.cpp"});
      EXPECT_EQ(scalar(legacy, "SELECT visits FROM symbols WHERE symbol = 'Sym';"),
                std::int64_t{6});
      EXPECT_EQ(scalar(legacy, "SELECT COUNT(*) FROM co_visits;"), std::int64_t{1});
      if (again != nullptr) EXPECT_EQ(again->HotSymbols(10).size(), 2u);
    }

    TEST_CASE("project store: an upgraded database is refused by every older build");
    {
      // Deliberate, and the only safe answer: a v1 build would go on writing
      // cwd-relative paths into tables this one has just made consistent, and a
      // v2 build would write positions into a pins table that no longer exists.
      // There is no old binary to run here, so the gate is asked directly.
      sqlite3* reader = nullptr;
      EXPECT_EQ(sqlite3_open(legacy.c_str(), &reader), SQLITE_OK);
      std::string why{"unset"};
      std::int64_t found = -1;
      EXPECT_TRUE(CheckSchemaVersion(reader, 1, why, &found) == Schema::kTooNew);
      EXPECT_EQ(found, std::int64_t{3});
      EXPECT_TRUE(!why.empty());
      why = "unset";
      EXPECT_TRUE(CheckSchemaVersion(reader, 2, why, &found) == Schema::kTooNew);
      EXPECT_TRUE(!why.empty());
      // And v3 itself reads it, which is the other half of the same statement.
      why = "unset";
      EXPECT_TRUE(CheckSchemaVersion(reader, 3, why, &found) == Schema::kUsable);
      sqlite3_close(reader);
    }
  }
}

// The other half of the keying rule: what comes back out.
//
// Every path in the database is keyed against the project root, and everything
// that opens one -- a pin jump, the excerpt refs a pins view is built from, the
// hot files a scan reads first -- resolves it against
// the current directory. Started at the root the two spellings are the same
// string and nothing was ever wrong. Started one directory down, "sub/file.cpp"
// read back from inside sub/ names "sub/sub/file.cpp", and since a path that is
// not there loads as a new empty buffer, the jump landed on a blank document
// with a plausible name rather than failing.
void StorePathsResolveAgainstTheRootFromBelowIt() {
  const Scratch scratch{"koi-below-root"};
  const std::filesystem::path root = scratch.dir / "proj";
  const std::filesystem::path sub = root / "sub";
  std::error_code ec;
  std::filesystem::create_directories(sub, ec);
  const auto write = [](const std::filesystem::path& path, std::string_view text) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << text;
    return path;
  };
  write(sub / "file.cpp", "int Sym() { return 0; }\n");
  write(sub / "other.cpp", "int Other() { return 1; }\n");
  // Outside the project entirely, so the store keys it absolute: it has no
  // root-relative spelling, and resolving one must leave it alone.
  const std::filesystem::path outside =
      write(scratch.dir / "outside.cpp", "int Away() { return 2; }\n");

  const std::filesystem::path was_root = ProjectRoot();
  const std::filesystem::path was_cwd = std::filesystem::current_path();
  struct Restore {
    std::filesystem::path root;
    std::filesystem::path cwd;
    ~Restore() {
      SetProjectRoot(root);
      SetProjectDbPath({});
      std::error_code ec;
      std::filesystem::current_path(cwd, ec);
    }
  } restore{was_root, was_cwd};

  // koi started in sub/, with the project root one level up.
  SetProjectRoot(root);
  std::filesystem::current_path(sub);

  TEST_CASE("project store: a stored key resolves to a path valid from here");
  {
    EXPECT_EQ(ResolveStorePath(root, "sub/file.cpp"), std::string{"file.cpp"});
    EXPECT_EQ(ResolveStorePath("sub/other.cpp"), std::string{"other.cpp"});
    // An absolute key is already the answer, and stays one however many times
    // it is resolved.
    EXPECT_EQ(ResolveStorePath(root, outside.string()), outside.string());
    EXPECT_EQ(ResolveStorePath(root, ResolveStorePath(root, outside.string())), outside.string());
    EXPECT_EQ(ResolveStorePath(root, ""), std::string{});
    // No root to resolve against: the key is what it was.
    EXPECT_EQ(ResolveStorePath(std::filesystem::path{}, "sub/file.cpp"),
              std::string{"sub/file.cpp"});

    // And at the root it is the identity -- which is why nothing that already
    // worked can move.
    std::filesystem::current_path(root);
    EXPECT_EQ(ResolveStorePath(root, "sub/file.cpp"), std::string{"sub/file.cpp"});
    std::filesystem::current_path(sub);
  }

  const std::filesystem::path db = scratch.dir / "below-root.db";
  std::string error{"unset"};
  const std::shared_ptr<ProjectStore> store = ProjectStore::Open(db, error);
  EXPECT_TRUE(store != nullptr);
  EXPECT_TRUE(error.empty());
  if (store == nullptr) return;

  const auto editor = [&store] {
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.project = store;
    // Nothing below scans the project, and this is what makes sure of it: a
    // stray scan would otherwise run find(1) over the whole machine.
    ed.settings.file_filter = "printf ''";
    return ed;
  };
  const auto opened = [](const Editor& ed, const std::filesystem::path& want) {
    std::error_code where;
    return std::filesystem::weakly_canonical(ed.doc.file, where) ==
           std::filesystem::weakly_canonical(want, where);
  };

  TEST_CASE("navigation: a pin jump opens the file the pin names");
  {
    store->SetPin(1, "file.cpp");
    Editor ed = editor();
    JumpToPin(ed, 1);
    EXPECT_TRUE(opened(ed, sub / "file.cpp"));
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string{"int Sym() { return 0; }\n"});
  }

  TEST_CASE("navigation: a hot-symbol jump opens the file, and stays on one key");
  {
    store->RecordSymbolVisit("Sym", "file.cpp", 1);
    Editor ed = editor();
    JumpToHotSymbol(ed, 0);
    EXPECT_TRUE(opened(ed, sub / "file.cpp"));
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string{"int Sym() { return 0; }\n"});
    // The jump records the visit on its way through, and what it hands the
    // store has to be a path from here: handing back the key it was given would
    // key it a second time, into a row naming "sub/sub/file.cpp".
    const std::vector<SymbolVisit> hot = store->HotSymbols(10);
    EXPECT_EQ(hot.size(), std::size_t{1});
    if (!hot.empty()) EXPECT_EQ(hot.front().file, std::string{"sub/file.cpp"});
  }

  TEST_CASE("navigation: a pins view reads the file the pin names");
  {
    Editor ed = editor();
    PinExcerpts(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    const std::string shown = AssembleDocContents(ed.doc.table);
    // The body, not just the header: an unreadable path builds a block with
    // nothing under it, which is what this looked like before.
    EXPECT_TRUE(shown.find("int Sym() { return 0; }") != std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.refs.size(), std::size_t{1});
    if (!ed.doc.excerpts.refs.empty()) {
      EXPECT_EQ(ed.doc.excerpts.refs.front().path, std::string{"file.cpp"});
    }
  }

  TEST_CASE("navigation: a file outside the project is keyed absolute and still opens");
  {
    store->SetPin(2, outside.string());
    Editor ed = editor();
    JumpToPin(ed, 2);
    EXPECT_TRUE(opened(ed, outside));
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string{"int Away() { return 2; }\n"});
  }
}

void VisitReadsAreBoundedAndTheTableIsPruned() {
  const Scratch scratch{"koi-visit-bounds"};
  const std::filesystem::path root = scratch.dir / "proj";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);

  const std::filesystem::path was_root = ProjectRoot();
  struct Restore {
    std::filesystem::path root;
    ~Restore() {
      SetProjectRoot(root);
      SetProjectDbPath({});
    }
  } restore{was_root};
  SetProjectRoot(root);

  // Reads the table behind the store's back: the prune is not visible through
  // any read that gates on the file still being on disk.
  const auto scalar = [](const std::filesystem::path& path, const char* sql) {
    sqlite3* handle = nullptr;
    std::int64_t value = -1;
    if (sqlite3_open(path.c_str(), &handle) == SQLITE_OK) {
      Stmt stmt{handle, sql};
      if (stmt && stmt.Step()) value = stmt.Integer(0);
    }
    sqlite3_close(handle);
    return value;
  };
  const auto name_of = [](int i) {
    std::string text = std::to_string(i);
    return "r" + std::string(static_cast<size_t>(4 - std::min<size_t>(4, text.size())), '0') + text;
  };
  const auto make = [&](int i) {
    const std::filesystem::path path = root / (name_of(i) + ".cpp");
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << "int x;\n";
    return path.string();
  };
  // The first `take` paths as one string, so a mismatch prints the two lists
  // rather than "not equal". A negative `take` means all of them.
  const auto paths_of = [](const std::vector<FileVisit>& rows, std::ptrdiff_t take = -1) {
    const size_t upto =
        (take < 0) ? rows.size() : std::min<size_t>(rows.size(), static_cast<size_t>(take));
    std::string out;
    for (size_t i = 0; i < upto; ++i) {
      if (i != 0) out += ' ';
      out += rows[i].path;
    }
    return out;
  };

  TEST_CASE("project store: a bounded recent read is the head of the unbounded one");
  {
    const std::filesystem::path db = scratch.dir / "recent.db";
    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(db, error);
    EXPECT_TRUE(store != nullptr);
    EXPECT_TRUE(error.empty());
    if (store == nullptr) return;

    // 300 files, visited oldest-first, so r0299 is the most recent.
    constexpr int kFiles = 300;
    std::vector<std::string> files;
    for (int i = 0; i < kFiles; ++i) {
      files.push_back(make(i));
      store->RecordVisit(files.back(), 1 + (i % 50), i % 7);
    }

    // The whole table, which is what every one of these used to return no
    // matter how few rows the caller was going to look at.
    const std::vector<FileVisit> all = store->RecentFiles(0);
    EXPECT_EQ(all.size(), static_cast<size_t>(kFiles));
    if (all.size() != static_cast<size_t>(kFiles)) return;
    EXPECT_EQ(all.front().path, std::string{"r0299.cpp"});
    EXPECT_EQ(all.back().path, std::string{"r0000.cpp"});

    // A bound is a prefix and nothing else: same rows, same order, same
    // line/column, just fewer of them. This is the "identical to before"
    // assertion -- `all` is exactly what the unbounded read used to give.
    for (const int want : {1, 5, 20, 64, 256, 299, 300, 301, 5000}) {
      const std::vector<FileVisit> some = store->RecentFiles(want);
      EXPECT_EQ(some.size(), static_cast<size_t>(std::min(want, kFiles)));
      EXPECT_EQ(paths_of(some), paths_of(all, want));
      for (size_t i = 0; i < some.size(); ++i) {
        EXPECT_EQ(some[i].line, all[i].line);
        EXPECT_EQ(some[i].column, all[i].column);
      }
    }

    TEST_CASE("project store: a bounded read still skips files that are gone");
    // The 100 most recent files are deleted from disk. A read for 20 has to
    // reach past all of them -- which is what the query over-fetches for.
    for (int i = kFiles - 100; i < kFiles; ++i) {
      std::error_code gone;
      std::filesystem::remove(root / (name_of(i) + ".cpp"), gone);
    }
    const std::vector<FileVisit> alive = store->RecentFiles(20);
    EXPECT_EQ(alive.size(), size_t{20});
    if (alive.size() == 20u) {
      EXPECT_EQ(alive.front().path, std::string{"r0199.cpp"});
      EXPECT_EQ(alive.back().path, std::string{"r0180.cpp"});
    }
    EXPECT_EQ(store->RecentFiles(0).size(), size_t{200});
    // Still a prefix of the unbounded answer, with the gone rows filtered out
    // of both.
    EXPECT_EQ(paths_of(alive), paths_of(store->RecentFiles(0), 20));
  }

  TEST_CASE("project store: a bounded frecent read is the head of the unbounded one");
  {
    const std::filesystem::path db = scratch.dir / "frecent.db";
    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(db, error);
    EXPECT_TRUE(store != nullptr);
    if (store == nullptr) return;

    // Frecency is (visits + edits*3) decayed by age; everything here is
    // seconds old, so the decay is one bucket for all of them and the edits
    // alone decide the order. f0 is the hottest, f39 the coldest.
    constexpr int kFiles = 40;
    for (int i = 0; i < kFiles; ++i) {
      const std::string path = make(1000 + i);
      store->RecordVisit(path, 3, 1);
      for (int e = 0; e < (kFiles - i); ++e) store->RecordEdit(path, 3, 1);
    }

    const std::vector<FileVisit> all = store->FrecentFiles(0);
    EXPECT_EQ(all.size(), static_cast<size_t>(kFiles));
    if (all.size() != static_cast<size_t>(kFiles)) return;
    EXPECT_EQ(all.front().path, std::string{"r1000.cpp"});
    EXPECT_EQ(all.back().path, std::string{"r1039.cpp"});

    for (const int want : {1, 5, 20, 39, 40, 41}) {
      const std::vector<FileVisit> some = store->FrecentFiles(want);
      EXPECT_EQ(some.size(), static_cast<size_t>(std::min(want, kFiles)));
      EXPECT_EQ(paths_of(some), paths_of(all, want));
    }

    // A deleted file drops out of the frecent read too, bounded or not.
    std::error_code gone;
    std::filesystem::remove(root / "r1000.cpp", gone);
    EXPECT_EQ(store->FrecentFiles(0).size(), static_cast<size_t>(kFiles - 1));
    const std::vector<FileVisit> five = store->FrecentFiles(5);
    EXPECT_EQ(five.size(), size_t{5});
    if (five.size() == 5u) EXPECT_EQ(five.front().path, std::string{"r1001.cpp"});
  }

  TEST_CASE("project store: the visit table is pruned at open, newest kept");
  {
    const std::filesystem::path db = scratch.dir / "prune.db";
    // 5200 rows, recorded in order, so p0000 is the oldest and p5199 the
    // newest. Nothing on disk answers to most of them -- the prune does not
    // care, and neither does the row count.
    constexpr int kRows = 5200;
    constexpr int kKeep = 5000;
    constexpr int kCut = kRows - kKeep;  // p0000..p0199 are the losers

    // Ten at each end exist as real files, which is how the reads that gate on
    // existence can say *which* rows survived rather than only how many.
    const auto visit_name = [](int i) {
      std::string text = std::to_string(i);
      return "p" + std::string(static_cast<size_t>(4 - std::min<size_t>(4, text.size())), '0') +
             text + ".cpp";
    };
    for (const int i : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) {
      std::ofstream{root / visit_name(i), std::ios::binary | std::ios::trunc} << "int x;\n";
      std::ofstream{root / visit_name(kRows - 1 - i), std::ios::binary | std::ios::trunc}
          << "int x;\n";
    }

    {
      std::string error{"unset"};
      const std::shared_ptr<ProjectStore> store = ProjectStore::Open(db, error);
      EXPECT_TRUE(store != nullptr);
      if (store == nullptr) return;
      for (int i = 0; i < kRows; ++i) {
        store->RecordVisit((root / visit_name(i)).string(), 1 + (i % 90), 0);
      }
      // Nothing prunes while the session is running: a session cannot outgrow
      // the cap by more than a session, and a mid-session delete would be a
      // remembered position vanishing under the user.
      EXPECT_EQ(store->FileCount(), kRows);
    }
    EXPECT_EQ(scalar(db, "SELECT COUNT(*) FROM files;"), std::int64_t{kRows});

    {
      std::string error{"unset"};
      const std::shared_ptr<ProjectStore> store = ProjectStore::Open(db, error);
      EXPECT_TRUE(store != nullptr);
      EXPECT_TRUE(error.empty());
      if (store == nullptr) return;

      EXPECT_EQ(store->FileCount(), kKeep);
      EXPECT_EQ(scalar(db, "SELECT COUNT(*) FROM files;"), std::int64_t{kKeep});

      // Exactly the newest 5000, and the boundary is where it is claimed to
      // be: LastVisit reads the row itself, with no existence gate in the way.
      Index line = 0;
      Index column = 0;
      EXPECT_FALSE(store->LastVisit((root / visit_name(0)).string(), line, column));
      EXPECT_FALSE(store->LastVisit((root / visit_name(kCut - 1)).string(), line, column));
      EXPECT_TRUE(store->LastVisit((root / visit_name(kCut)).string(), line, column));
      EXPECT_EQ(line, Index{1 + (kCut % 90)});
      EXPECT_TRUE(store->LastVisit((root / visit_name(kRows - 1)).string(), line, column));
      EXPECT_EQ(line, Index{1 + ((kRows - 1) % 90)});

      // And through the reads a user actually sees: the ten surviving files
      // that exist come back newest-first, and none of the ten pruned ones do.
      const std::vector<FileVisit> recent = store->RecentFiles(0);
      EXPECT_EQ(recent.size(), size_t{10});
      if (recent.size() == 10u) {
        EXPECT_EQ(recent.front().path, visit_name(kRows - 1));
        EXPECT_EQ(recent.back().path, visit_name(kRows - 10));
      }
      for (const FileVisit& row : recent) EXPECT_TRUE(row.path.substr(0, 2) != "p0");

      // A second open of an already-pruned database changes nothing.
      {
        std::string again{"unset"};
        const std::shared_ptr<ProjectStore> reopened = ProjectStore::Open(db, again);
        EXPECT_TRUE(reopened != nullptr);
        if (reopened != nullptr) EXPECT_EQ(reopened->FileCount(), kKeep);
      }
    }

    // The sort the prune and the recent read both depend on has an index to
    // walk, created by the same DDL every open replays -- and creating it did
    // not move the schema version, so a build without this line still opens
    // the database.
    EXPECT_EQ(scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'index'"
                         " AND name = 'files_by_ts';"),
              std::int64_t{1});
    EXPECT_EQ(scalar(db, "PRAGMA user_version;"), std::int64_t{3});
  }
}

// The same story one table over. `symbols` is what the hot-symbol jump and the
// symbol picker read, and both read all of it however little they were going to
// use: HotSymbols applied its limit in the C++ loop, so
// SQLite ran every row through a sorter before the first Step() could answer,
// and RankSymbols pulled the whole table into a map to look up the few hundred
// rows it had been handed. Nothing pruned it either -- and it grows per file
// *and* symbol, faster than `files` ever did -- so both got slower forever.
void SymbolReadsAreBoundedAndTheTableIsPruned() {
  const Scratch scratch{"koi-symbol-bounds"};
  const std::filesystem::path root = scratch.dir / "proj";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);

  const std::filesystem::path was_root = ProjectRoot();
  struct Restore {
    std::filesystem::path root;
    ~Restore() {
      SetProjectRoot(root);
      SetProjectDbPath({});
    }
  } restore{was_root};
  SetProjectRoot(root);

  const auto scalar = [](const std::filesystem::path& path, const char* sql) {
    sqlite3* handle = nullptr;
    std::int64_t value = -1;
    if (sqlite3_open(path.c_str(), &handle) == SQLITE_OK) {
      Stmt stmt{handle, sql};
      if (stmt && stmt.Step()) value = stmt.Integer(0);
    }
    sqlite3_close(handle);
    return value;
  };
  // Rows written straight into the table rather than through
  // RecordSymbolVisit. Two reasons, and both are about being able to say what
  // the answer should be: a row with n visits costs n calls through the store
  // (tens of thousands for a table big enough to prune), and `last_ts` cannot
  // be set through the store at all -- it is always Now() -- so no test going
  // that way can pin down the decay the ordering is built on.
  const auto seed = [](const std::filesystem::path& db, const std::string& sql) {
    sqlite3* handle = nullptr;
    bool ok = false;
    if (sqlite3_open(db.c_str(), &handle) == SQLITE_OK) ok = ExecSql(handle, sql.c_str());
    sqlite3_close(handle);
    return ok;
  };
  // The schema is the store's to create; these tests only add rows to it.
  const auto make_db = [](const std::filesystem::path& db) {
    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(db, error);
    return (store != nullptr) && error.empty();
  };
  const auto file_name = [](int i) { return "h" + std::to_string(i) + ".cpp"; };
  // "symbol@file:line" for a whole read, so a mismatch prints the two orderings
  // instead of "not equal". A negative `take` means all of them.
  const auto shape_of = [](const std::vector<SymbolVisit>& rows, std::ptrdiff_t take = -1) {
    const size_t upto =
        (take < 0) ? rows.size() : std::min<size_t>(rows.size(), static_cast<size_t>(take));
    std::string out;
    for (size_t i = 0; i < upto; ++i) {
      if (i != 0) out += ' ';
      out += rows[i].symbol + "@" + rows[i].file + ":" + std::to_string(rows[i].line);
    }
    return out;
  };

  // Every seeded row is this fresh, so the decay is 1.0 for all of them and the
  // score is the visit count exactly. That is what lets the expected ordering
  // below be written down rather than read back out of the thing under test.
  const double now =
      std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();

  // 20 files, of which the first 10 exist on disk. 600 rows spread over them,
  // visit counts strictly decreasing, so s0 is the hottest symbol in the store
  // and s599 the coldest -- and every row in between has a score no other row
  // shares, which makes "the first n" a single ordering rather than one of many.
  constexpr int kFiles = 20;
  constexpr int kAlive = 10;
  constexpr int kRows = 600;
  const std::filesystem::path hot_db = scratch.dir / "hotsym.db";
  {
    for (int i = 0; i < kAlive; ++i) {
      std::ofstream{root / file_name(i), std::ios::binary | std::ios::trunc} << "int x;\n";
    }
    EXPECT_TRUE(make_db(hot_db));
    std::string sql = "BEGIN;";
    for (int i = 0; i < kRows; ++i) {
      sql += "INSERT INTO symbols(file, symbol, visits, last_ts, line) VALUES('" +
             file_name(i % kFiles) + "','s" + std::to_string(i) + "'," +
             std::to_string(kRows - i) + "," + std::to_string(now) + "," +
             std::to_string(1 + (i % 50)) + ");";
    }
    sql += "COMMIT;";
    EXPECT_TRUE(seed(hot_db, sql));
  }

  TEST_CASE("project store: a bounded hot-symbol read is the head of the unbounded one");
  {
    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(hot_db, error);
    EXPECT_TRUE(store != nullptr);
    EXPECT_TRUE(error.empty());
    if (store == nullptr) return;
    // Under the cap, so the open left every row where it was.
    EXPECT_EQ(scalar(hot_db, "SELECT COUNT(*) FROM symbols;"), std::int64_t{kRows});

    // Far past the table, which is what this read used to be unconditionally:
    // every row sorted, however few the caller kept.
    const std::vector<SymbolVisit> all = store->HotSymbols(100000);
    EXPECT_EQ(all.size(), static_cast<size_t>(kRows / kFiles * kAlive));
    if (all.size() != static_cast<size_t>(kRows / kFiles * kAlive)) return;
    EXPECT_EQ(all.front().symbol, std::string{"s0"});
    EXPECT_EQ(all.front().file, file_name(0));
    EXPECT_EQ(all.front().line, Index{1});
    EXPECT_EQ(all.back().symbol, std::string{"s589"});

    // A bound is a prefix and nothing else: same rows, same order, same lines,
    // just fewer of them.
    for (const int want : {1, 5, kHotSymbolSlots, 20, 100, 300, 301}) {
      const std::vector<SymbolVisit> some = store->HotSymbols(want);
      EXPECT_EQ(some.size(), std::min<size_t>(static_cast<size_t>(want), all.size()));
      EXPECT_EQ(shape_of(some), shape_of(all, want));
    }
    EXPECT_TRUE(store->HotSymbols(0).empty());
    EXPECT_TRUE(store->HotSymbols(-1).empty());

    TEST_CASE("project store: a bounded hot-symbol read still skips files that are gone");
    // Both hottest files deleted from disk. A read for 20 has to reach past
    // every row of both -- 60 rows in, ahead of the first row it may return --
    // which is what the query over-fetches for.
    for (const int i : {0, 1}) {
      std::error_code gone;
      std::filesystem::remove(root / file_name(i), gone);
    }
    const std::vector<SymbolVisit> alive = store->HotSymbols(20);
    EXPECT_EQ(alive.size(), size_t{20});
    if (alive.size() == 20u) {
      EXPECT_EQ(alive.front().symbol, std::string{"s2"});
      // Eight live files left, so the 20th surviving row is s45: s2..s9,
      // s22..s29, s42..s45.
      EXPECT_EQ(alive.back().symbol, std::string{"s45"});
    }
    for (const SymbolVisit& row : alive) {
      EXPECT_TRUE((row.file != file_name(0)) && (row.file != file_name(1)));
    }
    // And still a prefix of the unbounded answer, with the gone rows filtered
    // out of both.
    EXPECT_EQ(shape_of(alive), shape_of(store->HotSymbols(100000), 20));
  }

  TEST_CASE("project store: the hot-symbol read looks a bounded distance for a live file");
  {
    // The other side of the over-fetch, spelled out because it is a trade and
    // not a free win: the query hands back four rows per row asked for (with a
    // floor of 256), and a store whose entire top of that window names files
    // that are gone comes up short rather than reading on forever. The old
    // read had no floor to hit -- it sorted the table and walked it to the end
    // -- which is exactly the cost this replaces.
    const std::filesystem::path starved = scratch.dir / "starved.db";
    EXPECT_TRUE(make_db(starved));
    constexpr int kDead = 400;
    constexpr int kTail = 40;
    std::ofstream{root / "tail.cpp", std::ios::binary | std::ios::trunc} << "int x;\n";
    std::string sql = "BEGIN;";
    for (int i = 0; i < (kDead + kTail); ++i) {
      const bool dead = (i < kDead);
      sql += "INSERT INTO symbols(file, symbol, visits, last_ts, line) VALUES('" +
             std::string{dead ? "vanished" + std::to_string(i) + ".cpp" : "tail.cpp"} + "','d" +
             std::to_string(i) + "'," + std::to_string(kDead + kTail - i) + "," +
             std::to_string(now) + ",1);";
    }
    sql += "COMMIT;";
    EXPECT_TRUE(seed(starved, sql));

    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(starved, error);
    EXPECT_TRUE(store != nullptr);
    if (store == nullptr) return;

    // Seven rows wanted, so the floor applies: 256 rows read, all of them
    // naming files that are not there, and the answer is empty.
    EXPECT_EQ(store->HotSymbols(kHotSymbolSlots).size(), size_t{0});
    // Ask for enough that the window covers the whole table and the live tail
    // is there, in order. 111 * 4 = 444, past all 440 rows.
    const std::vector<SymbolVisit> deep = store->HotSymbols(111);
    EXPECT_EQ(deep.size(), static_cast<size_t>(kTail));
    if (deep.size() == static_cast<size_t>(kTail)) {
      EXPECT_EQ(deep.front().symbol, std::string{"d400"});
      EXPECT_EQ(deep.back().symbol, std::string{"d439"});
    }
  }

  TEST_CASE("project store: ranking reads the rows it was handed, not the whole store");
  {
    // RankSymbols used to build a map of every (file, symbol) in the store to
    // look up the rows in front of it. It now seeks the files those rows came
    // from -- at most the current file plus HotFiles' limit plus the co-visit
    // cap -- and the answer has to be the same one, which is what this checks:
    // the ordering is compared against a reference computed here from the
    // numbers the rows were seeded with.
    seed(hot_db,
         "INSERT INTO co_visits(from_file, to_file, count) VALUES"
         "('h3.cpp','h5.cpp',7),('h3.cpp','h7.cpp',3),('h11.cpp','h0.cpp',99);");

    std::string error{"unset"};
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(hot_db, error);
    EXPECT_TRUE(store != nullptr);
    if (store == nullptr) return;

    // A picker's result set: every symbol the scanner found in a handful of
    // files. Some of those rows the store has a visit for, some it does not,
    // and one file it has never heard of at all.
    struct Expect {
      std::string path;
      std::string name;
      double score{0};
      std::uint8_t group{2};
    };
    const std::string current = (root / file_name(3)).string();
    std::vector<Symbol> rows;
    std::vector<Expect> expected;
    const std::vector<int> from_files{0, 3, 5, 7, 11};
    for (const int f : from_files) {
      for (int s = 0; s < 6; ++s) {
        // Stored rows for file h{f} are s{f}, s{f+20}, s{f+40}, ...; the last
        // three names are symbols the scanner found and the store never saw.
        const bool stored = (s < 3);
        const std::string name =
            stored ? ("s" + std::to_string(f + (s * kFiles))) : ("unseen" + std::to_string(s));
        const std::string path = (root / file_name(f)).string();
        rows.push_back(Symbol{path, 1 + s, 1, name});
        // The score the store holds: visits (kRows - i for row s{i}) times a
        // decay of 1.0, plus half of any co-visit count from the current file.
        double score = stored ? static_cast<double>(kRows - (f + (s * kFiles))) : 0.0;
        if (f == 5) score += 7 * 0.5;
        if (f == 7) score += 3 * 0.5;
        expected.push_back(Expect{path, name, score,
                                  static_cast<std::uint8_t>((f == 3)      ? 0
                                                            : (score > 0) ? 1
                                                                          : 2)});
      }
    }
    for (int s = 0; s < 3; ++s) {
      const std::string path = (root / "ghost.cpp").string();
      rows.push_back(Symbol{path, 1 + s, 1, "Ghost" + std::to_string(s)});
      expected.push_back(Expect{path, "Ghost" + std::to_string(s), 0.0, 2});
    }
    std::stable_sort(expected.begin(), expected.end(), [](const Expect& a, const Expect& b) {
      if (a.group != b.group) return a.group < b.group;
      return a.score > b.score;
    });
    size_t want_head = 0;
    for (const Expect& one : expected) want_head += (one.group < 2) ? 1 : 0;

    const size_t head = store->RankSymbols(rows, current);
    EXPECT_EQ(head, want_head);
    std::string got;
    std::string reference;
    for (size_t i = 0; i < rows.size(); ++i) {
      if (i != 0) {
        got += ' ';
        reference += ' ';
      }
      got += rows[i].name + "@" + rows[i].path;
      reference += expected[i].name + "@" + expected[i].path;
    }
    EXPECT_EQ(got, reference);
    // The current file's own symbols are the head of it, whatever they scored.
    EXPECT_EQ(rows.front().path, current);
  }

  TEST_CASE("project store: the symbol table is pruned at open, newest kept");
  {
    // 20200 rows, timestamped in order, so the 200 oldest are the losers. The
    // cap is deliberately generous -- four times the file cap, because a symbol
    // row is a quarter of the story a file row is -- and this is the boundary
    // it claims, exactly.
    constexpr int kSeeded = 20200;
    constexpr int kKeep = 20000;
    constexpr int kCut = kSeeded - kKeep;
    const std::filesystem::path prune_db = scratch.dir / "sympune.db";
    EXPECT_TRUE(make_db(prune_db));
    {
      std::string sql = "BEGIN;";
      sql.reserve(static_cast<size_t>(kSeeded) * 90);
      for (int i = 0; i < kSeeded; ++i) {
        sql += "INSERT INTO symbols(file, symbol, visits, last_ts, line) VALUES('h0.cpp','p" +
               std::to_string(i) + "',1," + std::to_string(1000 + i) + ",1);";
      }
      sql += "COMMIT;";
      EXPECT_TRUE(seed(prune_db, sql));
    }
    EXPECT_EQ(scalar(prune_db, "SELECT COUNT(*) FROM symbols;"), std::int64_t{kSeeded});

    {
      std::string error{"unset"};
      const std::shared_ptr<ProjectStore> store = ProjectStore::Open(prune_db, error);
      EXPECT_TRUE(store != nullptr);
      EXPECT_TRUE(error.empty());
      if (store == nullptr) return;
      EXPECT_EQ(scalar(prune_db, "SELECT COUNT(*) FROM symbols;"), std::int64_t{kKeep});
      // Exactly the newest 20000, and the boundary is where it is claimed to
      // be rather than near it.
      EXPECT_EQ(scalar(prune_db, "SELECT CAST(MIN(last_ts) AS INTEGER) FROM symbols;"),
                std::int64_t{1000 + kCut});
      EXPECT_EQ(scalar(prune_db, "SELECT CAST(MAX(last_ts) AS INTEGER) FROM symbols;"),
                std::int64_t{1000 + kSeeded - 1});
      EXPECT_EQ(scalar(prune_db, "SELECT COUNT(*) FROM symbols WHERE symbol = 'p199';"),
                std::int64_t{0});
      EXPECT_EQ(scalar(prune_db, "SELECT COUNT(*) FROM symbols WHERE symbol = 'p200';"),
                std::int64_t{1});
    }
    // A second open of an already-pruned database changes nothing -- the count
    // guard in front of the statement means it does not even look.
    {
      std::string again{"unset"};
      const std::shared_ptr<ProjectStore> reopened = ProjectStore::Open(prune_db, again);
      EXPECT_TRUE(reopened != nullptr);
      EXPECT_EQ(scalar(prune_db, "SELECT COUNT(*) FROM symbols;"), std::int64_t{kKeep});
    }
    // And a table under the cap is left alone, row for row.
    EXPECT_EQ(scalar(hot_db, "SELECT COUNT(*) FROM symbols;"), std::int64_t{kRows});

    // No index was added for any of this, and that is a measured decision, not
    // an omission: every one of these orderings is by an expression over
    // `visits` and the clock, which no column index can serve, so the index the
    // audit proposed left the sort where it was and only added a row lookup per
    // row -- slower, on the very read it was meant to help.
    EXPECT_EQ(scalar(prune_db, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'index'"
                               " AND tbl_name = 'symbols' AND name NOT LIKE 'sqlite_%';"),
              std::int64_t{0});
    EXPECT_EQ(scalar(prune_db, "PRAGMA user_version;"), std::int64_t{3});
  }
}

}  // namespace koi
