// Tests for crash.cpp: the dump a crash leaves, and the recovery that reads
// it back.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void CrashRecovery() {
  TEST_CASE("crash recovery: the dump is the buffer, a save removes it");
  namespace fs = std::filesystem;

  EXPECT_EQ(RecoveryPathFor("/a/b/file.txt"), std::string("/a/b/.file.txt.koi-recover"));
  EXPECT_EQ(RecoveryPathFor(""), std::string("koi-unnamed.koi-recover"));

  const fs::path dir = TempFixture("koi-crash-recovery");
  RemoveAllQuietly(dir);
  fs::create_directories(dir);
  const fs::path file = dir / "victim.txt";
  {
    std::ofstream out(file);
    out << "alpha\nbravo\ncharlie\n";
  }

  Editor ed;
  EXPECT_TRUE(!LoadDocument(file, ed.doc));
  ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

  SetCrashDocument(&ed.doc.table, &ed.doc.modified, ed.doc.file.string());
  EXPECT_FALSE(WriteRecoveryFile());
  const fs::path recover{RecoveryPathFor(file.string())};
  EXPECT_FALSE(fs::exists(recover));

  std::ignore = Insert("delta ", 0, ed.doc.table);
  std::ignore = Delete({6, 12}, ed.doc.table);
  std::ignore = Insert("echo\n", DocLength(ed.doc.table), ed.doc.table);
  ed.doc.modified = true;
  EXPECT_TRUE(WriteRecoveryFile());
  {
    std::ifstream in(recover);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text, ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}));
  }

  EXPECT_TRUE(!SaveDocumentAs(ed.doc, ed.doc.file));
  EXPECT_FALSE(fs::exists(recover));

  RemoveAllQuietly(dir);
}

void CrashRecoveryFollowsTheLiveBuffer() {
  TEST_CASE("crash recovery: the dump goes to the file it came from");
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-crash-live-buffer"};
  const fs::path a = scratch.Write("a.txt", "AAAA\n");
  const fs::path b = scratch.Write("b.txt", "BBBB\n");

  const auto recovered = [](const fs::path& file) {
    const fs::path recover{RecoveryPathFor(file.string())};
    if (!fs::exists(recover)) return std::string{"<none>"};
    std::ifstream in(recover);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!text.empty() && (text.back() == '\n')) text.pop_back();
    return text;
  };
  const auto forget = [](const fs::path& file) {
    std::error_code ec;
    std::filesystem::remove(RecoveryPathFor(file.string()), ec);
  };

  Editor ed;
  ed.theme = BuiltinTheme();

  ed.live_document_changed = [](Editor& e) {
    SetCrashDocument(&e.doc.table, &e.doc.modified, e.doc.file.string());
  };

  EXPECT_TRUE(OpenTarget(ed, a.string()));
  EXPECT_TRUE(OpenTarget(ed, b.string()));

  std::ignore = Insert("edited-b ", 0, ed.doc.table);
  ed.doc.modified = true;
  RunTypableCommand(ed, "bp");
  EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.txt"});
  std::ignore = Insert("edited-a ", 0, ed.doc.table);
  ed.doc.modified = true;

  SetCrashDocument(&ed.doc.table, &ed.doc.modified, ed.doc.file.string());
  forget(a);
  forget(b);

  SplitWindow(ed, false);
  RunTypableCommand(ed, "bn");
  EXPECT_EQ(ed.doc.file.filename().string(), std::string{"b.txt"});

  const std::vector<int> order = WindowOrder(ed);
  EXPECT_EQ(order.size(), std::size_t{2});
  const int elsewhere = (order.front() == ed.focused) ? order.back() : order.front();
  FocusWindowAt(ed, elsewhere);
  EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.txt"});
  EXPECT_TRUE(WriteRecoveryFile());
  EXPECT_EQ(recovered(a), std::string{"edited-a AAAA"});
  EXPECT_EQ(recovered(b), std::string{"<none>"});
  forget(a);

  FocusWindow(ed, true);
  EXPECT_EQ(ed.doc.file.filename().string(), std::string{"b.txt"});
  EXPECT_TRUE(WriteRecoveryFile());
  EXPECT_EQ(recovered(b), std::string{"edited-b BBBB"});
  EXPECT_EQ(recovered(a), std::string{"<none>"});
  forget(b);

  RunTypableCommand(ed, "bp");
  EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.txt"});
  EXPECT_TRUE(WriteRecoveryFile());
  EXPECT_EQ(recovered(a), std::string{"edited-a AAAA"});
  EXPECT_EQ(recovered(b), std::string{"<none>"});
  forget(a);

  Surface frame;
  FitFocusedViewport(ed, 60, 20);
  RenderTo(ed, frame, 60, 20);
  EXPECT_TRUE(WriteRecoveryFile());
  EXPECT_EQ(recovered(a), std::string{"edited-a AAAA"});
  EXPECT_EQ(recovered(b), std::string{"<none>"});
  forget(a);
  forget(b);
}

void CrashRecoveryCoversEveryBuffer() {
  TEST_CASE("crash recovery: every modified buffer is dumped, not just the active one");
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-crash-many-buffers"};
  const fs::path a = scratch.Write("a.txt", "AAAA\n");
  const fs::path b = scratch.Write("b.txt", "BBBB\n");

  const auto recovered = [](const fs::path& file) {
    const fs::path recover{RecoveryPathFor(file.string())};
    if (!fs::exists(recover)) return std::string{"<none>"};
    std::ifstream in(recover);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!text.empty() && (text.back() == '\n')) text.pop_back();
    return text;
  };
  const auto forget = [](const fs::path& file) {
    std::error_code ec;
    std::filesystem::remove(RecoveryPathFor(file.string()), ec);
  };

  Editor ed;
  EXPECT_TRUE(OpenTarget(ed, a.string()));
  EXPECT_TRUE(OpenTarget(ed, b.string()));
  std::ignore = Insert("edited-b ", 0, ed.doc.table);
  ed.doc.modified = true;
  RunTypableCommand(ed, "bp");
  EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.txt"});
  std::ignore = Insert("edited-a ", 0, ed.doc.table);
  ed.doc.modified = true;

  std::vector<CrashDoc> docs;
  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    const Document& doc = BufferAt(ed, i);
    docs.push_back(CrashDoc{&doc.table, &doc.modified, doc.file.string()});
  }
  SetCrashDocuments(docs);
  forget(a);
  forget(b);

  EXPECT_TRUE(WriteRecoveryFile());
  EXPECT_EQ(recovered(a), std::string{"edited-a AAAA"});
  EXPECT_EQ(recovered(b), std::string{"edited-b BBBB"});
  forget(a);
  forget(b);
}

}  // namespace koi
