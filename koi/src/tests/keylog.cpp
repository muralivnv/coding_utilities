// Tests for keylog.cpp: recording keys, replaying them, and the log they are
// written to.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

namespace {

struct LoggedEvent {
  std::string mode;
  std::string outcome;
  std::string key;
  std::string keys;
  std::string commands;
  std::int64_t count_prefix{0};
  std::string file;
  std::int64_t line{0};
  std::int64_t col{0};
  std::int64_t cursors{0};
  std::string selections;
  std::string prompt;
  bool has_context{false};
  std::int64_t context_first_line{0};
  std::string context;
};

std::string TextColumn(sqlite3_stmt* row, int at) {
  const unsigned char* text = sqlite3_column_text(row, at);
  return (text == nullptr) ? std::string{} : std::string{reinterpret_cast<const char*>(text)};
}

std::vector<LoggedEvent> ReadLog(const std::filesystem::path& db) {
  std::vector<LoggedEvent> out;
  sqlite3* handle = nullptr;
  if (sqlite3_open(db.c_str(), &handle) != SQLITE_OK) {
    if (handle != nullptr) sqlite3_close(handle);
    return out;
  }
  sqlite3_stmt* row = nullptr;
  const char* sql =
      "SELECT e.mode, e.outcome, e.key, e.keys, e.commands, e.count_prefix, e.file, e.line,"
      "       e.col, e.cursors, e.selections, e.prompt, e.ctx_hash, c.first_line, c.text"
      " FROM events e LEFT JOIN contexts c ON c.hash = e.ctx_hash"
      " ORDER BY e.seq;";
  if (sqlite3_prepare_v2(handle, sql, -1, &row, nullptr) == SQLITE_OK) {
    while (sqlite3_step(row) == SQLITE_ROW) {
      LoggedEvent one;
      one.mode = TextColumn(row, 0);
      one.outcome = TextColumn(row, 1);
      one.key = TextColumn(row, 2);
      one.keys = TextColumn(row, 3);
      one.commands = TextColumn(row, 4);
      one.count_prefix = sqlite3_column_int64(row, 5);
      one.file = TextColumn(row, 6);
      one.line = sqlite3_column_int64(row, 7);
      one.col = sqlite3_column_int64(row, 8);
      one.cursors = sqlite3_column_int64(row, 9);
      one.selections = TextColumn(row, 10);
      one.prompt = TextColumn(row, 11);
      one.has_context = sqlite3_column_type(row, 12) != SQLITE_NULL;
      one.context_first_line = sqlite3_column_int64(row, 13);
      one.context = TextColumn(row, 14);
      out.push_back(std::move(one));
    }
    sqlite3_finalize(row);
  }
  sqlite3_close(handle);
  return out;
}

std::int64_t RowsIn(const std::filesystem::path& db, const char* table) {
  sqlite3* handle = nullptr;
  if (sqlite3_open(db.c_str(), &handle) != SQLITE_OK) {
    if (handle != nullptr) sqlite3_close(handle);
    return -1;
  }
  const std::string sql = std::string{"SELECT COUNT(*) FROM "} + table + ";";
  sqlite3_stmt* row = nullptr;
  std::int64_t count = -1;
  if (sqlite3_prepare_v2(handle, sql.c_str(), -1, &row, nullptr) == SQLITE_OK) {
    if (sqlite3_step(row) == SQLITE_ROW) count = sqlite3_column_int64(row, 0);
    sqlite3_finalize(row);
  }
  sqlite3_close(handle);
  return count;
}

}  // namespace

void KeyRecording(Rng& rng) {
  const Scratch scratch{"koi-keylog"};
  const std::filesystem::path was_root = ProjectRoot();
  SetProjectRoot(scratch.dir);
  struct Restore {
    std::filesystem::path back;
    ~Restore() { SetProjectRoot(back); }
  } restore{was_root};

  const KeyMaps maps = DefaultKeyMaps();

  struct Rig {
    Editor ed;
    std::vector<Key> pending;
    std::filesystem::path db;

    void Press(const KeyMaps& maps, std::string_view text) {
      Key key;
      EXPECT_TRUE(ParseKey(text, key));
      HandleKeyInput(ed, maps, key, pending);
    }
  };

  const auto rig_for = [&](const std::filesystem::path& db, const std::filesystem::path& file) {
    auto rig = std::make_unique<Rig>();
    rig->db = db;
    EXPECT_FALSE(LoadDocument(file, rig->ed.doc));
    std::string error;
    rig->ed.recorder = KeyRecorder::Open(db, "test-pane", error);
    EXPECT_TRUE(rig->ed.recorder != nullptr);
    if (rig->ed.recorder) rig->ed.recorder->SetKeyMap(KeyMapFingerprint(maps));
    return rig;
  };

  TEST_CASE("key recording: off unless a config asks for it");
  {
    EXPECT_FALSE(Settings{}.record);

    Settings settings;
    KeyMaps parsed;
    std::vector<std::string> errors;
    EXPECT_FALSE(ParseKeyMapConfig("[editor]\nrecord = true\n", parsed, settings, errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(settings.record);

    Settings bad_settings;
    KeyMaps bad_maps;
    std::vector<std::string> bad_errors;
    std::ignore = ParseKeyMapConfig("[editor]\nrecord = \"yes\"\n", bad_maps, bad_settings,
                                    bad_errors);
    EXPECT_FALSE(bad_settings.record);
    EXPECT_EQ(bad_errors.size(), std::size_t{1});
  }

  TEST_CASE("key recording: an editor with no recorder writes nothing and does not care");
  {
    const std::filesystem::path file = scratch.Write("quiet.txt", NumberedLines(5));
    Editor ed;
    EXPECT_FALSE(LoadDocument(file, ed.doc));
    EXPECT_TRUE(ed.recorder == nullptr);
    std::vector<Key> pending;
    for (const std::string_view text : {"k", "k", "u", "esc", "g", "t"}) {
      Key key;
      EXPECT_TRUE(ParseKey(text, key));
      HandleKeyInput(ed, maps, key, pending);
    }

    EXPECT_TRUE(ed.recorder == nullptr);
  }

  TEST_CASE("key recording: one row per key, each labelled with what became of it");
  {
    const std::filesystem::path db = scratch.dir / "outcomes.db";
    const std::filesystem::path file = scratch.Write("outcomes.txt", NumberedLines(40));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    rig->Press(maps, "k");
    rig->Press(maps, "3");
    rig->Press(maps, "k");
    rig->Press(maps, "g");
    rig->Press(maps, "t");
    rig->Press(maps, "A-e");
    rig->Press(maps, "z");
    rig->Press(maps, "esc");
    rig->ed.recorder->Flush();

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), std::size_t{8});
    if (log.size() != 8) return;

    EXPECT_EQ(log[0].outcome, std::string{"binding"});
    EXPECT_EQ(log[0].commands, std::string{"move_line_down"});
    EXPECT_EQ(log[0].mode, std::string{"normal"});

    EXPECT_EQ(log[1].outcome, std::string{"count"});
    EXPECT_EQ(log[1].key, std::string{"3"});

    EXPECT_EQ(log[2].outcome, std::string{"binding"});

    EXPECT_EQ(log[2].count_prefix, std::int64_t{3});
    EXPECT_EQ(log[1].count_prefix, std::int64_t{0});

    EXPECT_EQ(log[3].outcome, std::string{"pending"});
    EXPECT_EQ(log[3].keys, std::string{"g"});

    EXPECT_EQ(log[4].outcome, std::string{"binding"});
    EXPECT_EQ(log[4].keys, std::string{"g t"});
    EXPECT_EQ(log[4].commands, std::string{"goto_file_start"});

    EXPECT_EQ(log[5].outcome, std::string{"binding"});
    EXPECT_EQ(log[5].commands, std::string{"collapse_selection,insert_mode"});

    EXPECT_EQ(log[6].outcome, std::string{"insert-text"});
    EXPECT_EQ(log[6].key, std::string{"z"});

    EXPECT_EQ(log[6].mode, std::string{"insert"});

    EXPECT_EQ(log[7].outcome, std::string{"binding"});
    EXPECT_EQ(log[7].commands, std::string{"normal_mode"});
    EXPECT_EQ(log[7].mode, std::string{"insert"});
  }

  TEST_CASE("key recording: an unbound key is recorded as one");
  {
    const std::filesystem::path db = scratch.dir / "unbound.db";
    const std::filesystem::path file = scratch.Write("unbound.txt", NumberedLines(5));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    rig->Press(maps, "A-y");
    rig->ed.recorder->Flush();

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), std::size_t{1});
    if (log.empty()) return;
    EXPECT_EQ(log[0].outcome, std::string{"unbound"});
  }

  TEST_CASE("key recording: a chord abandoned in insert mode is counted once, not twice");
  {

    const std::filesystem::path db = scratch.dir / "abandon.db";
    const std::filesystem::path file = scratch.Write("abandon.txt", NumberedLines(5));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    rig->Press(maps, "A-e");
    rig->Press(maps, "j");
    rig->Press(maps, "esc");
    rig->ed.recorder->Flush();

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), std::size_t{3});
    if (log.size() != 3) return;
    EXPECT_EQ(log[1].outcome, std::string{"pending"});

    EXPECT_EQ(log[2].key, std::string{"esc"});
    EXPECT_EQ(log[2].outcome, std::string{"binding"});
    EXPECT_EQ(log[2].commands, std::string{"normal_mode"});

    EXPECT_TRUE(ReadDocRange(rig->ed.doc.table, Interval(0, 6)).starts_with("jline"));
  }

  TEST_CASE("key recording: a chord that times out is recorded as text");
  {
    const std::filesystem::path db = scratch.dir / "timeout.db";
    const std::filesystem::path file = scratch.Write("timeout.txt", NumberedLines(5));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    rig->Press(maps, "A-e");
    rig->Press(maps, "j");

    rig->ed.recorder->NoteChordTimeout(rig->ed, rig->pending);
    FlushPendingAsText(rig->ed, rig->pending);
    rig->ed.recorder->Flush();

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), std::size_t{3});
    if (log.size() != 3) return;
    EXPECT_EQ(log[2].outcome, std::string{"chord-timeout"});
    EXPECT_EQ(log[2].keys, std::string{"j"});
  }

  TEST_CASE("key recording: prompt keys carry the prompt as it was before them");
  {
    const std::filesystem::path db = scratch.dir / "prompt.db";
    const std::filesystem::path file = scratch.Write("prompt.txt", NumberedLines(5));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    rig->Press(maps, ":");
    rig->Press(maps, "w");
    rig->Press(maps, "q");
    rig->ed.recorder->Flush();

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), std::size_t{3});
    if (log.size() != 3) return;
    EXPECT_EQ(log[0].outcome, std::string{"binding"});
    EXPECT_EQ(log[1].outcome, std::string{"prompt"});
    EXPECT_EQ(log[1].prompt, std::string{});
    EXPECT_EQ(log[2].outcome, std::string{"prompt"});
    EXPECT_EQ(log[2].prompt, std::string{"w"});
  }

  TEST_CASE("key recording: the context is the lines around the cursor as they were");
  {
    const std::filesystem::path db = scratch.dir / "context.db";
    const std::filesystem::path file = scratch.Write("context.txt", NumberedLines(60));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    const Index at = LineStart(rig->ed.doc.table, 20);
    rig->ed.doc.selections.Set(MinWidth1(rig->ed.doc.table, Selection{at, at, -1}));
    rig->Press(maps, "k");
    rig->ed.recorder->Flush();

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), std::size_t{1});
    if (log.empty()) return;

    EXPECT_TRUE(log[0].has_context);
    EXPECT_EQ(log[0].line, std::int64_t{21});
    EXPECT_EQ(log[0].context_first_line, std::int64_t{11});
    EXPECT_TRUE(log[0].context.starts_with("line-11\n"));
    EXPECT_TRUE(log[0].context.ends_with("line-31\n"));

    EXPECT_EQ(LineAt(rig->ed.doc.table, CursorOf(rig->ed.doc.table,
                                                 rig->ed.doc.selections.Primary())),
              Index{21});

    EXPECT_EQ(log[0].file, std::string{"context.txt"});
    EXPECT_EQ(log[0].cursors, std::int64_t{1});
  }

  TEST_CASE("key recording: the context window grows downward near the top of a file");
  {
    const std::filesystem::path db = scratch.dir / "ctxlast.db";
    const std::filesystem::path file = scratch.Write("ctxlast.txt", NumberedLines(60));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    // Both cursors clamp `first` to line 1, so the memo key -- revision, length,
    // first line, file -- is identical for the two. The windows are not: the one
    // below reaches ten lines further down.
    for (const Index line : {Index{0}, Index{5}}) {
      const Index at = LineStart(rig->ed.doc.table, line);
      rig->ed.doc.selections.Set(MinWidth1(rig->ed.doc.table, Selection{at, at, -1}));
      rig->Press(maps, "k");
    }
    rig->ed.recorder->Flush();

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), std::size_t{2});
    if (log.size() < 2) return;

    EXPECT_EQ(log[0].context_first_line, std::int64_t{1});
    EXPECT_TRUE(log[0].context.ends_with("line-11\n"));
    EXPECT_EQ(log[1].context_first_line, std::int64_t{1});
    EXPECT_TRUE(log[1].context.ends_with("line-16\n"));
    EXPECT_TRUE(log[0].context != log[1].context);
    EXPECT_EQ(RowsIn(db, "contexts"), std::int64_t{2});
  }

  TEST_CASE("key recording: a window that has not changed is stored once");
  {
    const std::filesystem::path db = scratch.dir / "dedup.db";
    const std::filesystem::path file = scratch.Write("dedup.txt", NumberedLines(60));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    const Index at = LineStart(rig->ed.doc.table, 30);
    rig->ed.doc.selections.Set(MinWidth1(rig->ed.doc.table, Selection{at, at, -1}));

    for (int i = 0; i < 20; ++i) rig->Press(maps, (i % 2 == 0) ? "o" : "u");
    rig->ed.recorder->Flush();

    EXPECT_EQ(RowsIn(db, "events"), std::int64_t{20});
    EXPECT_EQ(RowsIn(db, "contexts"), std::int64_t{1});
  }

  TEST_CASE("key recording: an edit is a new context, not the old one");
  {
    const std::filesystem::path db = scratch.dir / "edited.db";
    const std::filesystem::path file = scratch.Write("edited.txt", NumberedLines(30));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    rig->Press(maps, "A-e");
    rig->Press(maps, "a");
    rig->Press(maps, "b");
    rig->ed.recorder->Flush();

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), std::size_t{3});
    if (log.size() != 3) return;

    EXPECT_TRUE(log[2].context.starts_with("aline-1\n"));
    EXPECT_TRUE(log[1].context.starts_with("line-1\n"));
  }

  TEST_CASE("key recording: switching files does not carry the first file's context");
  {

    const std::filesystem::path db = scratch.dir / "switch.db";
    const std::filesystem::path first = scratch.Write("first.txt", "AAA\nAAA\nAAA\n");
    const std::filesystem::path second = scratch.Write("second.txt", "BBB\nBBB\nBBB\n");
    auto rig = rig_for(db, first);
    if (!rig->ed.recorder) return;

    Document other;
    EXPECT_FALSE(LoadDocument(second, other));
    AddBuffer(rig->ed, std::move(other));
    EXPECT_EQ(BufferCount(rig->ed), std::size_t{2});

    EXPECT_EQ(BufferAt(rig->ed, 0).table.revision, BufferAt(rig->ed, 1).table.revision);
    EXPECT_EQ(DocLength(BufferAt(rig->ed, 0).table), DocLength(BufferAt(rig->ed, 1).table));

    SwitchToBuffer(rig->ed, 0);
    rig->Press(maps, "k");
    SwitchToBuffer(rig->ed, 1);
    rig->Press(maps, "k");
    SwitchToBuffer(rig->ed, 0);
    rig->Press(maps, "k");
    rig->ed.recorder->Flush();

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), std::size_t{3});
    if (log.size() != 3) return;
    EXPECT_EQ(log[0].file, std::string{"first.txt"});
    EXPECT_TRUE(log[0].context.starts_with("AAA"));
    EXPECT_EQ(log[1].file, std::string{"second.txt"});
    EXPECT_TRUE(log[1].context.starts_with("BBB"));
    EXPECT_EQ(log[2].file, std::string{"first.txt"});
    EXPECT_TRUE(log[2].context.starts_with("AAA"));
  }

  TEST_CASE("key recording: a file whose contents are sensitive logs the key, not the text");
  {
    EXPECT_TRUE(ContentsAreSensitive(".env"));
    EXPECT_TRUE(ContentsAreSensitive("deploy/.env.production"));
    EXPECT_TRUE(ContentsAreSensitive("/home/x/.ssh/id_ed25519"));
    EXPECT_TRUE(ContentsAreSensitive("certs/Server.PEM"));

    EXPECT_FALSE(ContentsAreSensitive("src/secrets_test.cpp"));
    EXPECT_FALSE(ContentsAreSensitive("src/keymap.cpp"));
    EXPECT_FALSE(ContentsAreSensitive(""));

    const std::filesystem::path db = scratch.dir / "secret.db";
    const std::filesystem::path file = scratch.Write(".env", "TOKEN=hunter2\nAPI=abcdef\n");
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    rig->Press(maps, "k");
    rig->ed.recorder->Flush();

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), std::size_t{1});
    EXPECT_EQ(RowsIn(db, "contexts"), std::int64_t{0});
    if (log.empty()) return;
    EXPECT_FALSE(log[0].has_context);
    EXPECT_EQ(log[0].outcome, std::string{"binding"});
    EXPECT_EQ(log[0].file, std::string{".env"});
    EXPECT_TRUE(log[0].context.empty());

    // Typing into the file is the case the guard is really for: in insert mode
    // the key is the text, so a verbatim key column rebuilds the secret one row
    // at a time. The event still has to be there -- what was typed does not.
    rig->Press(maps, "A-e");  // collapse_selection, insert_mode
    const std::string secret = "hunter2";
    for (const char c : secret) rig->Press(maps, std::string(1, c));
    rig->ed.recorder->Flush();

    const std::vector<LoggedEvent> typed = ReadLog(db);
    std::size_t inserts = 0;
    std::string reconstructed;
    for (const LoggedEvent& e : typed) {
      if (e.outcome != "insert-text") continue;
      ++inserts;
      reconstructed += e.key;
      reconstructed += e.keys;
    }
    EXPECT_EQ(inserts, secret.size());
    EXPECT_EQ(reconstructed, std::string{});
    EXPECT_EQ(RowsIn(db, "contexts"), std::int64_t{0});

    // The same keys on an ordinary file are still recorded in full: this
    // redacts what is sensitive, it does not stop recording.
    const std::filesystem::path plain_db = scratch.dir / "plain.db";
    const std::filesystem::path plain = scratch.Write("plain.txt", "x\n");
    auto plain_rig = rig_for(plain_db, plain);
    if (!plain_rig->ed.recorder) return;
    plain_rig->Press(maps, "A-e");
    for (const char c : secret) plain_rig->Press(maps, std::string(1, c));
    plain_rig->ed.recorder->Flush();
    std::string plain_text;
    for (const LoggedEvent& e : ReadLog(plain_db)) {
      if (e.outcome == "insert-text") plain_text += e.key;
    }
    EXPECT_EQ(plain_text, secret);
  }

  TEST_CASE("key recording: nothing reaches the database until it is flushed");
  {
    const std::filesystem::path db = scratch.dir / "buffered.db";
    const std::filesystem::path file = scratch.Write("buffered.txt", NumberedLines(10));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    EXPECT_FALSE(rig->ed.recorder->Buffered());
    for (int i = 0; i < 5; ++i) rig->Press(maps, "k");
    EXPECT_TRUE(rig->ed.recorder->Buffered());
    EXPECT_EQ(RowsIn(db, "events"), std::int64_t{0});

    rig->ed.recorder->Flush();
    EXPECT_FALSE(rig->ed.recorder->Buffered());
    EXPECT_EQ(RowsIn(db, "events"), std::int64_t{5});

    rig->ed.recorder->Flush();
    EXPECT_EQ(RowsIn(db, "events"), std::int64_t{5});
    EXPECT_EQ(RowsIn(db, "sessions"), std::int64_t{1});
  }

  TEST_CASE("key recording: closing the recorder flushes what it was holding");
  {
    const std::filesystem::path db = scratch.dir / "closing.db";
    const std::filesystem::path file = scratch.Write("closing.txt", NumberedLines(10));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    for (int i = 0; i < 3; ++i) rig->Press(maps, "k");
    rig->ed.recorder.reset();
    EXPECT_EQ(RowsIn(db, "events"), std::int64_t{3});
  }

  TEST_CASE("key recording: changing the keymap starts a new session");
  {
    const std::filesystem::path db = scratch.dir / "sessions.db";
    const std::filesystem::path file = scratch.Write("sessions.txt", NumberedLines(10));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    rig->Press(maps, "k");
    KeyMaps changed = maps;
    std::vector<std::string> errors;
    std::ignore = ParseKeyMapConfig("[keys.normal]\nQ = \"move_line_up\"\n", changed, errors);
    EXPECT_TRUE(errors.empty());
    rig->ed.recorder->SetKeyMap(KeyMapFingerprint(changed));
    rig->Press(maps, "k");
    rig->ed.recorder->Flush();

    EXPECT_EQ(RowsIn(db, "events"), std::int64_t{2});
    EXPECT_EQ(RowsIn(db, "sessions"), std::int64_t{2});
  }

  TEST_CASE("keymap fingerprint: same bindings agree, one changed binding does not");
  {
    EXPECT_EQ(KeyMapFingerprint(DefaultKeyMaps()), KeyMapFingerprint(DefaultKeyMaps()));

    KeyMaps added = DefaultKeyMaps();
    std::vector<std::string> errors;
    std::ignore = ParseKeyMapConfig("[keys.normal]\nQ = \"move_line_up\"\n", added, errors);
    EXPECT_TRUE(KeyMapFingerprint(added) != KeyMapFingerprint(DefaultKeyMaps()));

    KeyMaps rebound = DefaultKeyMaps();
    errors.clear();
    std::ignore = ParseKeyMapConfig("[keys.normal]\nk = \"move_line_up\"\n", rebound, errors);
    EXPECT_TRUE(KeyMapFingerprint(rebound) != KeyMapFingerprint(DefaultKeyMaps()));

    KeyMaps insert_only = DefaultKeyMaps();
    errors.clear();
    std::ignore = ParseKeyMapConfig("[keys.insert]\nQ = \"normal_mode\"\n", insert_only, errors);
    EXPECT_TRUE(KeyMapFingerprint(insert_only) != KeyMapFingerprint(DefaultKeyMaps()));
    EXPECT_TRUE(KeyMapFingerprint(insert_only) != KeyMapFingerprint(added));
  }

  TEST_CASE("key recording: a database it cannot use is replaced, not fatal");
  {
    std::string error;
    EXPECT_TRUE(KeyRecorder::Open({}, "pane", error) == nullptr);
    EXPECT_FALSE(error.empty());

    const std::filesystem::path corrupt = scratch.dir / "corrupt-keylog.db";
    {
      std::ofstream out{corrupt, std::ios::binary};
      out << "SQLite format 3\0not a database, only noise";
      for (int i = 0; i < 4096; ++i) out << static_cast<char>(i % 251);
    }
    error.clear();
    const std::shared_ptr<KeyRecorder> broken = KeyRecorder::Open(corrupt, "pane", error);
    // The key log goes through the same gate as the project store, so a file
    // that is not a database moves aside and a fresh log takes its place --
    // with the warning that says so riding out alongside the working recorder.
    EXPECT_TRUE(broken != nullptr);
    EXPECT_FALSE(error.empty());
    {
      std::filesystem::path aside = corrupt;
      aside += ".corrupt";
      EXPECT_TRUE(std::filesystem::exists(aside));
    }

    if (broken != nullptr) {
      const std::filesystem::path file = scratch.Write("hostile.txt", NumberedLines(4));
      Editor ed;
      EXPECT_FALSE(LoadDocument(file, ed.doc));
      ed.recorder = broken;
      std::vector<Key> pending;
      for (const std::string_view text : {"k", "i", "esc", ":"}) {
        Key key;
        EXPECT_TRUE(ParseKey(text, key));
        HandleKeyInput(ed, maps, key, pending);
      }
      ed.recorder->Flush();
    }
    EXPECT_TRUE(true);
  }

  TEST_CASE("key recording: an unnamed buffer and an enormous line are recorded, not refused");
  {
    const std::filesystem::path db = scratch.dir / "hostile.db";
    Editor ed;

    ResetToOriginal(ed.doc.table, std::string(400000, 'x') + "\n");
    ed.doc.selections.Set(Selection{});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    std::string error;
    ed.recorder = KeyRecorder::Open(db, "pane", error);
    EXPECT_TRUE(ed.recorder != nullptr);
    if (!ed.recorder) return;

    std::vector<Key> pending;
    Key key;
    EXPECT_TRUE(ParseKey("o", key));
    HandleKeyInput(ed, maps, key, pending);
    ed.recorder->Flush();

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), std::size_t{1});
    if (log.empty()) return;
    EXPECT_TRUE(log[0].file.empty());
    EXPECT_TRUE(log[0].context.size() <= std::size_t{16 * 1024});
    EXPECT_TRUE(log[0].context.size() > std::size_t{0});
  }

  TEST_CASE("key recording: a capped context is still valid UTF-8");
  {

    const std::filesystem::path db = scratch.dir / "utf8.db";
    std::string wide;
    while (wide.size() < 40000) wide += "→αβ✓";
    wide += '\n';
    EXPECT_TRUE(IsWellFormedUtf8(wide));

    Editor ed;
    ResetToOriginal(ed.doc.table, wide);
    ed.doc.selections.Set(Selection{});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    std::string error;
    ed.recorder = KeyRecorder::Open(db, "pane", error);
    EXPECT_TRUE(ed.recorder != nullptr);
    if (!ed.recorder) return;

    std::vector<Key> pending;
    Key key;
    EXPECT_TRUE(ParseKey("o", key));
    HandleKeyInput(ed, maps, key, pending);
    ed.recorder->Flush();

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), std::size_t{1});
    if (log.empty()) return;
    EXPECT_TRUE(log[0].context.size() <= std::size_t{16 * 1024});

    EXPECT_TRUE(log[0].context.size() > std::size_t{16 * 1024} - 4);
    EXPECT_TRUE(IsWellFormedUtf8(log[0].context));
  }

  TEST_CASE("key recording: many cursors are counted exactly and written in part");
  {
    const std::filesystem::path db = scratch.dir / "cursors.db";
    const std::filesystem::path file = scratch.Write("cursors.txt", NumberedLines(300));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    std::vector<Selection> many;
    for (Index i = 0; i < 200; ++i) {
      const Index at = LineStart(rig->ed.doc.table, i);
      many.push_back(MinWidth1(rig->ed.doc.table, Selection{at, at, -1}));
    }
    rig->ed.doc.selections.Replace(rig->ed.doc.table, std::move(many));
    rig->Press(maps, "k");
    rig->ed.recorder->Flush();

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), std::size_t{1});
    if (log.empty()) return;
    EXPECT_EQ(log[0].cursors, std::int64_t{200});
    EXPECT_TRUE(log[0].selections.size() < std::size_t{2000});
  }

  TEST_CASE("key recording: every press is one row, whichever way the handler leaves");
  {

    const std::filesystem::path db = scratch.dir / "one-row-each.db";
    const std::filesystem::path file = scratch.Write("fuzz.txt", NumberedLines(120));
    auto rig = rig_for(db, file);
    if (!rig->ed.recorder) return;

    constexpr std::array kAlphabet = std::to_array<std::string_view>({

        "i", "k", "u", "o", "j", "l", "2", "7",

        "a", "q", "x", "d", "c", "%", "S", "_", "~", "`", "C-z", "C-y",

        "Z", "esc",

        "f", "t", "r", "w", "W",

        "A-e", "ret", "tab", "backspace",

        "A-y", "C-g",

        "h", "e", "z",
    });

    for (const std::string_view text : kAlphabet) {
      Key key;
      EXPECT_TRUE(ParseKey(text, key));
    }

    std::int64_t presses = 0;
    for (int step = 0; step < 3000; ++step) {
      const std::string_view text = kAlphabet[static_cast<std::size_t>(
          rng.Pick(0, static_cast<int>(kAlphabet.size()) - 1))];
      Key key;
      if (!ParseKey(text, key)) continue;
      HandleKeyInput(rig->ed, maps, key, rig->pending);
      ++presses;

      if (!EditorInvariants(rig->ed).empty()) {
        EXPECT_EQ(EditorInvariants(rig->ed), std::string{});
        break;
      }
    }
    rig->ed.recorder->Flush();

    EXPECT_EQ(presses, std::int64_t{3000});
    EXPECT_EQ(RowsIn(db, "events"), presses);

    const std::vector<LoggedEvent> log = ReadLog(db);
    EXPECT_EQ(log.size(), static_cast<std::size_t>(presses));
    std::size_t labelled = 0;
    for (const LoggedEvent& one : log) {
      if (!one.outcome.empty() && (one.outcome != "other") && !one.keys.empty()) ++labelled;
    }
    EXPECT_EQ(labelled, log.size());
  }
}

}  // namespace koi
