// Tests for what koi does with a file on disk: atomic writes and what they
// preserve, the stamp a buffer is guarded by, and the reload when the file
// changes underneath it.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void ReloadEveryBufferSuite() {
  const Scratch scratch{"koi-reload-all"};

  TEST_CASE(":reload re-reads the buffers you cannot see, not just the one you can");
  {
    const std::filesystem::path a = scratch.Write("a.txt", "a original\n");
    const std::filesystem::path b = scratch.Write("b.txt", "b original\n");

    Editor ed;
    EXPECT_FALSE(LoadDocument(a, ed.doc));
    Document other;
    EXPECT_FALSE(LoadDocument(b, other));
    AddBuffer(ed, std::move(other));
    EXPECT_EQ(BufferCount(ed), std::size_t{2});

    scratch.Write("a.txt", "a changed\n");
    scratch.Write("b.txt", "b changed\n");

    RunTypableCommand(ed, "reload");
    EXPECT_TRUE(ed.status.find("reloaded 2") != std::string::npos);
    EXPECT_EQ(AssembleDocContents(BufferAt(ed, 0).table), std::string{"a changed\n"});
    EXPECT_EQ(AssembleDocContents(BufferAt(ed, 1).table), std::string{"b changed\n"});
  }

  TEST_CASE(":reload keeps unsaved changes in a buffer that is not on screen");
  {
    const std::filesystem::path a = scratch.Write("keep-a.txt", "a original\n");
    const std::filesystem::path b = scratch.Write("keep-b.txt", "b original\n");

    Editor ed;
    EXPECT_FALSE(LoadDocument(b, ed.doc));

    RunCommands(ed, {"collapse_selection", "insert_mode"});
    std::vector<Key> pending{K("Z")};
    FlushPendingAsText(ed, pending);
    RunCommands(ed, {"normal_mode"});
    EXPECT_TRUE(ed.doc.modified);

    Document front;
    EXPECT_FALSE(LoadDocument(a, front));
    AddBuffer(ed, std::move(front));
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"keep-a.txt"});

    scratch.Write("keep-a.txt", "a changed\n");
    scratch.Write("keep-b.txt", "b changed\n");

    RunTypableCommand(ed, "reload");
    EXPECT_TRUE(ed.status.find("reloaded 1") != std::string::npos);
    EXPECT_TRUE(ed.status.find("kept 1 with unsaved changes") != std::string::npos);

    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string{"a changed\n"});
    const std::size_t behind = (ed.active == 0) ? 1u : 0u;
    EXPECT_TRUE(AssembleDocContents(BufferAt(ed, behind).table).starts_with("Z"));
    EXPECT_TRUE(BufferAt(ed, behind).modified);
  }

  TEST_CASE(":reload keeps the focused buffer's unsaved changes; :reload! discards them");
  {

    const std::filesystem::path only = scratch.Write("only.txt", "original\n");
    Editor ed;
    EXPECT_FALSE(LoadDocument(only, ed.doc));
    RunCommands(ed, {"collapse_selection", "insert_mode"});
    std::vector<Key> pending{K("Q")};
    FlushPendingAsText(ed, pending);
    RunCommands(ed, {"normal_mode"});
    EXPECT_TRUE(ed.doc.modified);

    scratch.Write("only.txt", "theirs\n");
    RunTypableCommand(ed, "reload");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string{"Qoriginal\n"});
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_TRUE(ed.status.find("kept 1") != std::string::npos);
    EXPECT_TRUE(ed.status.find(":reload!") != std::string::npos);

    RunTypableCommand(ed, "reload!");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string{"theirs\n"});
    EXPECT_FALSE(ed.doc.modified);

    EXPECT_TRUE(CanUndo(ed.doc.table));
  }

  TEST_CASE(":reload! discards unsaved changes behind you, the way it says it will");
  {
    const std::filesystem::path a = scratch.Write("force-a.txt", "a original\n");
    const std::filesystem::path b = scratch.Write("force-b.txt", "b original\n");

    Editor ed;
    EXPECT_FALSE(LoadDocument(b, ed.doc));
    RunCommands(ed, {"collapse_selection", "insert_mode"});
    std::vector<Key> pending{K("Z")};
    FlushPendingAsText(ed, pending);
    RunCommands(ed, {"normal_mode"});
    EXPECT_TRUE(ed.doc.modified);

    Document front;
    EXPECT_FALSE(LoadDocument(a, front));
    AddBuffer(ed, std::move(front));
    const std::size_t behind = (ed.active == 0) ? 1u : 0u;

    scratch.Write("force-a.txt", "a changed\n");
    scratch.Write("force-b.txt", "b changed\n");

    RunTypableCommand(ed, "reload");
    EXPECT_TRUE(ed.status.find("kept 1 with unsaved changes") != std::string::npos);
    EXPECT_TRUE(BufferAt(ed, behind).modified);

    RunTypableCommand(ed, "reload!");
    EXPECT_TRUE(ed.status.find("reloaded 2") != std::string::npos);
    EXPECT_TRUE(ed.status.find("kept") == std::string::npos);
    EXPECT_EQ(AssembleDocContents(BufferAt(ed, behind).table), std::string{"b changed\n"});
    EXPECT_FALSE(BufferAt(ed, behind).modified);
  }

  TEST_CASE(":reload on a scratch buffer says there is nothing to read");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "no file here\n");
    ed.doc.selections.Set(Selection{});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    RunTypableCommand(ed, "reload");
    EXPECT_TRUE(ed.status.find("no file to reload") != std::string::npos);
  }

  TEST_CASE(":reload reports a file that has gone away without losing the others");
  {
    const std::filesystem::path here = scratch.Write("here.txt", "here\n");
    const std::filesystem::path gone = scratch.Write("gone.txt", "gone\n");

    Editor ed;
    EXPECT_FALSE(LoadDocument(gone, ed.doc));
    Document second;
    EXPECT_FALSE(LoadDocument(here, second));
    AddBuffer(ed, std::move(second));

    std::error_code ec;
    std::filesystem::remove(gone, ec);
    scratch.Write("here.txt", "here changed\n");

    RunTypableCommand(ed, "reload");
    EXPECT_TRUE(ed.status.find("failed") != std::string::npos);

    const std::size_t which = (ed.doc.file.filename() == "here.txt")
                                  ? ed.active
                                  : ((ed.active == 0) ? 1u : 0u);
    EXPECT_EQ(AssembleDocContents(BufferAt(ed, which).table), std::string{"here changed\n"});

    const std::size_t deleted = (which == 0) ? 1u : 0u;
    EXPECT_EQ(AssembleDocContents(BufferAt(ed, deleted).table), std::string{"gone\n"});
  }

  TEST_CASE(":reload leaves every window pointing somewhere real");
  {

    const std::filesystem::path big = scratch.Write("big.txt", NumberedLines(200));
    Editor ed;
    EXPECT_FALSE(LoadDocument(big, ed.doc));
    SplitWindow(ed, true);
    RunCommands(ed, {"goto_last_line"});
    FocusWindow(ed, true);
    RunCommands(ed, {"goto_last_line"});

    scratch.Write("big.txt", "one line now\n");
    RunTypableCommand(ed, "reload");

    for (std::size_t i = 0; i < WindowCount(ed); ++i) {
      FocusWindow(ed, true);
      const Index length = DocLength(ed.doc.table);
      for (const Selection& s : ed.doc.selections.Ranges()) {
        EXPECT_TRUE((s.anchor >= 0) && (s.anchor <= length));
        EXPECT_TRUE((s.head >= 0) && (s.head <= length));
      }
      EXPECT_EQ(EditorInvariants(ed), std::string{});
    }
  }
}

void SavingOntoAnOpenBufferIsRefused() {
  TEST_CASE("save: writing over a file another buffer has open needs :w!");
  const Scratch scratch{"koi-dup-path"};
  const std::filesystem::path a = scratch.Write("a.txt", "AAAA\n");
  const std::filesystem::path b = scratch.Write("b.txt", "BBBB\n");

  const auto claiming = [](const Editor& ed, const std::filesystem::path& p) {
    int n = 0;
    for (std::size_t i = 0; i < BufferCount(ed); ++i) {
      if (BufferAt(ed, i).file.filename() == p.filename()) ++n;
    }
    return n;
  };

  Editor ed;
  ed.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(ed, a.string()));
  EXPECT_TRUE(OpenTarget(ed, b.string()));
  RunTypableCommand(ed, "bp");
  EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.txt"});

  RunTypableCommand(ed, "w " + b.string());
  EXPECT_TRUE(ed.status.find("open in another buffer") != std::string::npos);
  EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.txt"});
  EXPECT_EQ(claiming(ed, b), 1);
  EXPECT_EQ(ReadDocRange(BufferAt(ed, FindFileBuffer(ed, b)).table,
                         {0, DocLength(BufferAt(ed, FindFileBuffer(ed, b)).table)}),
            std::string{"BBBB\n"});

  const std::filesystem::path fresh = scratch.dir / "c.txt";
  RunTypableCommand(ed, "w " + fresh.string());
  EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);
  EXPECT_EQ(ed.doc.file.filename().string(), std::string{"c.txt"});

  RunTypableCommand(ed, "w");
  EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);
}

void AtomicWriteKeepsPermissions() {
  TEST_CASE("save: the temp file is never more readable than what it replaces");
  const Scratch scratch{"koi-atomic-write"};

  const auto mode_of = [](const std::filesystem::path& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return -1;
    return static_cast<int>(st.st_mode & 07777);
  };

  {
    const std::filesystem::path secret = scratch.Write("secret.txt", "before\n");
    std::error_code ec;
    std::filesystem::permissions(secret, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_write,
                                 ec);
    EXPECT_EQ(mode_of(secret), 0600);

    ExpectOk(AtomicWriteFile(secret, "after\n"), "write over a private file");
    EXPECT_EQ(ReadWholeFile(secret, ec), std::string{"after\n"});
    EXPECT_EQ(mode_of(secret), 0600);
    EXPECT_FALSE(std::filesystem::exists(secret.string() + ".koi-tmp"));
  }

  {
    const std::filesystem::path script = scratch.Write("run.sh", "#!/bin/sh\ntrue\n");
    std::error_code ec;
    std::filesystem::permissions(script, std::filesystem::perms::owner_all, ec);
    EXPECT_EQ(mode_of(script), 0700);
    ExpectOk(AtomicWriteFile(script, "#!/bin/sh\nfalse\n"), "write over a script");
    EXPECT_EQ(mode_of(script), 0700);
  }

  {
    const std::filesystem::path file = scratch.dir / "leftover.txt";
    const std::filesystem::path tmp = file.string() + ".koi-tmp";
    std::error_code ec;
    { std::ofstream junk{tmp, std::ios::binary | std::ios::trunc}; junk << "junk"; }
    EXPECT_TRUE(std::filesystem::exists(tmp));

    ExpectOk(AtomicWriteFile(file, "fresh\n"), "write past a leftover temp file");
    EXPECT_EQ(ReadWholeFile(file, ec), std::string{"fresh\n"});
    EXPECT_FALSE(std::filesystem::exists(tmp));
  }

  {
    const std::filesystem::path file = scratch.Write("edited.txt", "one\ntwo\n");
    std::error_code ec;
    std::filesystem::permissions(file, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write,
                                 ec);
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, file.string()));
    TypeInto(ed, 'X');
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);
    EXPECT_EQ(mode_of(file), 0600);
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".koi-tmp"));
    EXPECT_EQ(ReadWholeFile(file, ec), AssembleDocContents(ed.doc.table));
  }
}

void DiskChangeOnFocus() {
  TEST_CASE("focus-in disk check: reload when nothing is at stake, warn when it is");
  namespace fs = std::filesystem;

  const fs::path dir = TempFixture("koi-disk-change");
  RemoveAllQuietly(dir);
  fs::create_directories(dir);
  const fs::path file = dir / "watched.txt";
  const auto write_file = [&](const std::string& text) { WriteFixtureFile(file, text); };
  const auto text_of = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };

  {
    write_file("alpha\nbeta\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.empty());
    EXPECT_EQ(text_of(ed), std::string("alpha\nbeta\n"));
  }

  {
    write_file("alpha\nbeta\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"move_line_down"});
    write_file("ALPHA\nbeta\ngamma\n");
    CheckDiskChange(ed);
    EXPECT_EQ(text_of(ed), std::string("ALPHA\nbeta\ngamma\n"));
    EXPECT_TRUE(ed.status.find("reloaded") != std::string::npos);
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_EQ(LineAt(ed.doc.table, ed.doc.selections.Primary().head), Index{1});
  }

  {
    write_file("alpha\nbeta\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.mode = Mode::kInsert;
    std::ignore = InsertAtCursorsKeeping("typed ", ed.doc.table, ed.doc.selections);
    ed.doc.modified = true;
    write_file("something else entirely\n");
    CheckDiskChange(ed);
    EXPECT_EQ(text_of(ed), std::string("typed alpha\nbeta\n"));
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_TRUE(ed.status.find("changed on disk") != std::string::npos);
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "scratch\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.empty());
    EXPECT_FALSE(ReloadDocument(ed));
  }

  TEST_CASE("focus-in disk check: every pane on screen, not just the focused one");
  {
    const fs::path front = dir / "front.txt";
    const fs::path beside = dir / "beside.txt";
    const fs::path offscreen = dir / "offscreen.txt";
    WriteFixtureFile(front, "front original\n");
    WriteFixtureFile(beside, "beside original\n");
    WriteFixtureFile(offscreen, "offscreen original\n");

    Editor ed;
    EXPECT_TRUE(!LoadDocument(front, ed.doc));
    Document second;
    EXPECT_TRUE(!LoadDocument(beside, second));
    AddBuffer(ed, std::move(second));
    Document third;
    EXPECT_TRUE(!LoadDocument(offscreen, third));
    AddBuffer(ed, std::move(third));

    // Two panes: the focused one on `front`, the other on `beside`. `offscreen`
    // is open but nothing is drawing it.
    SplitWindow(ed, true);
    SwitchToBuffer(ed, 1);
    FocusWindow(ed, true);
    SwitchToBuffer(ed, 0);

    WriteFixtureFile(front, "front changed on disk\n");
    WriteFixtureFile(beside, "beside changed on disk\n");
    WriteFixtureFile(offscreen, "offscreen changed on disk\n");

    ed.status.clear();
    CheckDiskChange(ed);

    EXPECT_EQ(AssembleDocContents(BufferAt(ed, 0).table), std::string{"front changed on disk\n"});
    EXPECT_EQ(AssembleDocContents(BufferAt(ed, 1).table), std::string{"beside changed on disk\n"});
    // Nothing is drawing this one, so nothing pulled the ground out from under
    // it -- :reload is still what takes a buffer nobody is looking at.
    EXPECT_EQ(AssembleDocContents(BufferAt(ed, 2).table), std::string{"offscreen original\n"});

    EXPECT_FALSE(ExternallyModified(BufferAt(ed, 0)));
    EXPECT_FALSE(ExternallyModified(BufferAt(ed, 1)));
    EXPECT_TRUE(ExternallyModified(BufferAt(ed, 2)));

    // Both reloaded files are named, so the reader does not have to go and find
    // out which of the panes moved under them.
    EXPECT_TRUE(ed.status.find("front.txt") != std::string::npos);
    EXPECT_TRUE(ed.status.find("beside.txt") != std::string::npos);
    EXPECT_TRUE(ed.status.find("offscreen.txt") == std::string::npos);
    EXPECT_TRUE(ed.status.find("reloaded") != std::string::npos);
  }

  TEST_CASE("focus-in disk check: a pane with unsaved edits is warned about, not reloaded");
  {
    const fs::path clean = dir / "clean.txt";
    const fs::path dirty = dir / "dirty.txt";
    WriteFixtureFile(clean, "clean original\n");
    WriteFixtureFile(dirty, "dirty original\n");

    Editor ed;
    EXPECT_TRUE(!LoadDocument(dirty, ed.doc));
    RunCommands(ed, {"collapse_selection", "insert_mode"});
    std::vector<Key> pending{K("Z")};
    FlushPendingAsText(ed, pending);
    RunCommands(ed, {"normal_mode"});
    EXPECT_TRUE(ed.doc.modified);

    Document front;
    EXPECT_TRUE(!LoadDocument(clean, front));
    AddBuffer(ed, std::move(front));

    // The dirty buffer sits in the pane behind the focused one.
    SplitWindow(ed, true);
    SwitchToBuffer(ed, 0);
    FocusWindow(ed, true);
    SwitchToBuffer(ed, 1);

    WriteFixtureFile(clean, "clean changed on disk\n");
    WriteFixtureFile(dirty, "dirty changed on disk\n");

    ed.status.clear();
    CheckDiskChange(ed);

    EXPECT_EQ(AssembleDocContents(BufferAt(ed, 1).table), std::string{"clean changed on disk\n"});
    EXPECT_TRUE(AssembleDocContents(BufferAt(ed, 0).table).starts_with("Z"));
    EXPECT_TRUE(BufferAt(ed, 0).modified);

    EXPECT_TRUE(ed.status.find("clean.txt") != std::string::npos);
    EXPECT_TRUE(ed.status.find("reloaded") != std::string::npos);
    EXPECT_TRUE(ed.status.find("dirty.txt") != std::string::npos);
    EXPECT_TRUE(ed.status.find(":w! to overwrite") != std::string::npos);
  }

  RemoveAllQuietly(dir);
}

void DiskStampIsMtimeAndSize() {
  TEST_CASE("write guard: a foreign write inside one mtime tick is still caught");
  namespace fs = std::filesystem;

  const fs::path dir = TempFixture("koi-disk-stamp");
  RemoveAllQuietly(dir);
  fs::create_directories(dir);
  const fs::path file = dir / "stamped.txt";

  const auto file_text = [&] {
    std::ifstream in(file, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };
  // Someone else's write, landing in the very mtime tick koi's own last write
  // took -- not a contrived state but the ordinary one, since AtomicWriteFile
  // renames a fresh inode into place and that inode's stamp comes off the
  // coarse clock (see FileStamp). Forced here so the case is the same on the
  // fine-grained filesystems this suite also runs on.
  const auto foreign_write_same_tick = [&](std::string_view contents) {
    const auto held = fs::last_write_time(file);
    {
      std::ofstream out{file, std::ios::binary | std::ios::trunc};
      out << contents;
    }
    fs::last_write_time(file, held);
    EXPECT_TRUE(fs::last_write_time(file) == held);
  };

  {
    WriteFixtureFile(file, "alpha\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);

    // Same mtime, different size: the size is the whole of what tells these
    // apart, and koi refuses to write over what it never read.
    foreign_write_same_tick("theirs, and longer than ours\n");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("changed on disk") != std::string::npos);
    EXPECT_EQ(file_text(), std::string{"theirs, and longer than ours\n"});

    // The old detection, unregressed: mtime moved, size did not.
    RunTypableCommand(ed, "w!");
    EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);
    EXPECT_EQ(file_text(), std::string{"alpha\n"});
    {
      const auto held = fs::last_write_time(file);
      std::ofstream out{file, std::ios::binary | std::ios::trunc};
      out << "bravo\n";
      out.close();
      fs::last_write_time(file, held + std::chrono::seconds{2});
    }
    EXPECT_EQ(fs::file_size(file), std::uintmax_t{6});
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("changed on disk") != std::string::npos);
    EXPECT_EQ(file_text(), std::string{"bravo\n"});
  }

  {
    // No foreign write at all: koi's own saves re-stamp both halves, so a save
    // that changes the file's size does not read as somebody else's.
    WriteFixtureFile(file, "alpha\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);

    ed.mode = Mode::kInsert;
    std::ignore = InsertAtCursorsKeeping("much more text ", ed.doc.table, ed.doc.selections);
    ed.doc.modified = true;
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);
    EXPECT_TRUE(ed.status.find("changed on disk") == std::string::npos);
    EXPECT_EQ(file_text(), std::string{"much more text alpha\n"});
  }

  {
    // :reload takes the file, and the stamp with it -- including the size, or
    // the next :w would warn about a change the buffer already holds.
    WriteFixtureFile(file, "alpha\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);

    foreign_write_same_tick("theirs, at a length of its own\n");
    RunTypableCommand(ed, "reload");
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}),
              std::string{"theirs, at a length of its own\n"});
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);
    EXPECT_TRUE(ed.status.find("changed on disk") == std::string::npos);
  }

  {
    // The focus-in check reads the same stamp: an unmodified buffer takes the
    // same-tick foreign write instead of sitting on text that is no longer
    // anybody's.
    WriteFixtureFile(file, "alpha\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    foreign_write_same_tick("theirs, once more with feeling\n");
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.find("changed on disk") != std::string::npos);
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}),
              std::string{"theirs, once more with feeling\n"});
  }

  RemoveAllQuietly(dir);
}

void TrimOnSave() {
  TEST_CASE("trim_trailing_whitespace_on_save");

  const auto read_file = [](const std::filesystem::path& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };

  {
    Scratch scratch{"koi-trim-test"};
    const std::filesystem::path file = scratch.Write("t.txt", "one   \ntwo\t\nthree  \nfour\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.settings.trim_trailing_whitespace_on_save = true;
    RunCommands(ed, {":w"});
    EXPECT_EQ(read_file(file), std::string("one\ntwo\nthree\nfour\n"));

    RunCommands(ed, {"undo"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}),
              std::string("one   \ntwo\t\nthree  \nfour\n"));
  }

  {
    Scratch scratch{"koi-trim-off-test"};
    const std::filesystem::path file = scratch.Write("t.txt", "keep me   \n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    // The setting is pinned rather than assumed: what is under test is that the
    // trim does not run when it is off, not what the default happens to be.
    ed.settings.trim_trailing_whitespace_on_save = false;
    RunCommands(ed, {":w"});
    EXPECT_EQ(read_file(file), std::string("keep me   \n"));
  }

  {
    Scratch scratch{"koi-trim-crlf-test"};
    const std::filesystem::path file = scratch.Write("t.txt", "alpha  \r\nbeta\r\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.settings.trim_trailing_whitespace_on_save = true;
    RunCommands(ed, {":w"});
    EXPECT_EQ(read_file(file), std::string("alpha\r\nbeta\r\n"));
  }
}

void AtomicWriteFollowsSymlinksAndKeepsHardLinks() {
  TEST_CASE("atomic write: rename must not eat the link it was reached by");
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-atomic-links"};

  const auto read = [](const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };

  // rename() replaces the link, not its target. Left alone, :w through a
  // symlink deleted the symlink, put the edit in a new regular file beside it,
  // and left the repository copy the link pointed into holding the old text.
  {
    fs::create_directories(scratch.dir / "real");
    scratch.Write("real/dotfile", "original\n");
    std::error_code ec;
    fs::create_symlink(scratch.dir / "real" / "dotfile", scratch.dir / "link", ec);
    EXPECT_FALSE(static_cast<bool>(ec));
    EXPECT_FALSE(static_cast<bool>(AtomicWriteFile(scratch.dir / "link", "edited\n")));
    EXPECT_TRUE(fs::is_symlink(scratch.dir / "link"));
    EXPECT_EQ(read(scratch.dir / "real" / "dotfile"), std::string("edited\n"));
  }

  // A relative target, resolved against the link's own directory.
  {
    fs::create_directories(scratch.dir / "sub");
    scratch.Write("sub/real", "old\n");
    std::error_code ec;
    fs::create_symlink("sub/real", scratch.dir / "rel", ec);
    EXPECT_FALSE(static_cast<bool>(AtomicWriteFile(scratch.dir / "rel", "via-rel\n")));
    EXPECT_TRUE(fs::is_symlink(scratch.dir / "rel"));
    EXPECT_EQ(read(scratch.dir / "sub" / "real"), std::string("via-rel\n"));
  }

  // A chain, and a dangling link whose target has to be created.
  {
    std::error_code ec;
    scratch.Write("chain-end", "old\n");
    fs::create_symlink(scratch.dir / "chain-end", scratch.dir / "chain-b", ec);
    fs::create_symlink(scratch.dir / "chain-b", scratch.dir / "chain-a", ec);
    EXPECT_FALSE(static_cast<bool>(AtomicWriteFile(scratch.dir / "chain-a", "via-chain\n")));
    EXPECT_TRUE(fs::is_symlink(scratch.dir / "chain-a"));
    EXPECT_EQ(read(scratch.dir / "chain-end"), std::string("via-chain\n"));

    fs::create_symlink(scratch.dir / "not-yet", scratch.dir / "dangling", ec);
    EXPECT_FALSE(static_cast<bool>(AtomicWriteFile(scratch.dir / "dangling", "created\n")));
    EXPECT_EQ(read(scratch.dir / "not-yet"), std::string("created\n"));
  }

  // A loop has to be an error, not a write to whichever link the walk gave up
  // on -- that link is still a link, and renaming over it is the whole defect.
  {
    std::error_code ec;
    fs::create_symlink(scratch.dir / "loop-b", scratch.dir / "loop-a", ec);
    fs::create_symlink(scratch.dir / "loop-a", scratch.dir / "loop-b", ec);
    EXPECT_TRUE(static_cast<bool>(AtomicWriteFile(scratch.dir / "loop-a", "nope\n")));
    EXPECT_TRUE(fs::is_symlink(scratch.dir / "loop-a"));
  }

  // More than one name for the inode: a rename would leave the others on the
  // old contents, so the write goes in place and keeps the inode.
  {
    std::error_code ec;
    scratch.Write("h1", "old\n");
    fs::create_hard_link(scratch.dir / "h1", scratch.dir / "h2", ec);
    EXPECT_FALSE(static_cast<bool>(ec));
    EXPECT_FALSE(static_cast<bool>(AtomicWriteFile(scratch.dir / "h1", "shared-new\n")));
    EXPECT_EQ(read(scratch.dir / "h2"), std::string("shared-new\n"));
    EXPECT_EQ(static_cast<Index>(fs::hard_link_count(scratch.dir / "h1")), Index{2});
    // Shrinking has to truncate, not leave the tail of the longer write.
    EXPECT_FALSE(static_cast<bool>(AtomicWriteFile(scratch.dir / "h1", "x\n")));
    EXPECT_EQ(read(scratch.dir / "h2"), std::string("x\n"));
  }

  // Nothing left behind on any path.
  {
    int strays = 0;
    for (const auto& entry : fs::recursive_directory_iterator(scratch.dir)) {
      if (entry.path().string().ends_with(".koi-tmp")) ++strays;
    }
    EXPECT_EQ(strays, 0);
  }
}

void ConcurrentSavesNeitherBlendNorShareATemp() {
  TEST_CASE("atomic write: two writers on one file never share a temp or blend their bytes");
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-atomic-race"};

  const auto read = [](const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };
  const auto temps_in = [](const fs::path& dir) {
    std::set<std::string> found;
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}; !ec && (it != fs::directory_iterator{});
         it.increment(ec)) {
      // Names only, and no stat: the entries under a live writer come and go,
      // and anything that touches the inode races with the rename.
      const std::string name = it->path().filename().string();
      if (name.ends_with(".koi-tmp")) found.insert(name);
    }
    return found;
  };

  const fs::path target = scratch.dir / "contested.txt";
  // Wide enough that one save is many write() calls plus an fsync, so the two
  // writers genuinely overlap -- with one shared temp name that overlap is the
  // corruption, and without the overlap the test pins nothing.
  const std::string a = std::string(600 * 1024, 'a') + "\n";
  const std::string b = std::string(600 * 1024, 'b') + "\n";

  std::set<std::string> temps;
  int reported = 0;
  int blended = 0;
  constexpr int kRounds = 16;
  for (int round = 0; round < kRounds; ++round) {
    std::atomic<int> running{2};
    bool first_failed = false;
    bool second_failed = false;
    std::thread first{[&] {
      first_failed = static_cast<bool>(AtomicWriteFile(target, a));
      running.fetch_sub(1);
    }};
    std::thread second{[&] {
      second_failed = static_cast<bool>(AtomicWriteFile(target, b));
      running.fetch_sub(1);
    }};
    // The only view of the temp names there is: whatever is on disk while the
    // writers are mid-save.
    while (running.load() != 0) temps.merge(temps_in(scratch.dir));
    first.join();
    second.join();

    if (first_failed || second_failed) ++reported;
    const std::string got = read(target);
    if ((got != a) && (got != b)) ++blended;
  }

  // Counted rather than asserted per round, so a failure reports how often it
  // happened instead of once per round.
  EXPECT_EQ(reported, 0);
  EXPECT_EQ(blended, 0);
  // Distinct names, not merely some name: one temp per writer is the fix, and
  // the old scheme would leave exactly one -- the fixed one.
  EXPECT_TRUE(temps.size() >= 2);
  EXPECT_FALSE(temps.contains("contested.txt.koi-tmp"));
  EXPECT_TRUE(temps_in(scratch.dir).empty());

  TEST_CASE("atomic write: a save that cannot start leaves nothing behind");
  {
    const fs::path pen = scratch.dir / "sealed";
    fs::create_directories(pen);
    const fs::path sealed = pen / "file.txt";
    { std::ofstream out{sealed, std::ios::binary}; out << "old\n"; }

    if (::getuid() == 0) {
      // root writes through the directory's mode bits, so the create cannot be
      // made to fail this way.
      EXPECT_TRUE(true);
    } else {
      // The fixture removes this tree on the way out, which needs the directory
      // writable again however the case ends.
      struct RestoreDir {
        fs::path dir;
        ~RestoreDir() {
          std::error_code ec;
          fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
        }
      } restore_dir{pen};
      std::error_code ec;
      fs::permissions(pen, fs::perms::owner_read | fs::perms::owner_exec,
                      fs::perm_options::replace, ec);
      EXPECT_FALSE(static_cast<bool>(ec));

      EXPECT_TRUE(static_cast<bool>(AtomicWriteFile(sealed, "new\n")));
      fs::permissions(pen, fs::perms::owner_all, fs::perm_options::replace, ec);
      // Failed means failed: the old contents stand, and no half-written temp
      // is left beside them.
      EXPECT_EQ(read(sealed), std::string("old\n"));
      EXPECT_TRUE(temps_in(pen).empty());
    }
  }
}

}  // namespace koi
