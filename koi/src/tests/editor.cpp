// Tests for editor.cpp: editor state that is not the window layer -- open
// targets, viewports, soft wrap, buffer switching, and the status line.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void TargetParsing() {
  TEST_CASE("file:line:col parsing");
  namespace fs = std::filesystem;

  EXPECT_EQ(ParseTarget("new.txt").path.string(), std::string("new.txt"));
  EXPECT_FALSE(ParseTarget("new.txt").has_line);
  EXPECT_EQ(ParseTarget("new.txt:12").path.string(), std::string("new.txt"));
  EXPECT_EQ(ParseTarget("new.txt:12").line, Index{12});
  EXPECT_TRUE(ParseTarget("new.txt:12").has_line);
  EXPECT_FALSE(ParseTarget("new.txt:12").has_column);
  EXPECT_EQ(ParseTarget("new.txt:12:5").path.string(), std::string("new.txt"));
  EXPECT_EQ(ParseTarget("new.txt:12:5").line, Index{12});
  EXPECT_EQ(ParseTarget("new.txt:12:5").column, Index{5});

  EXPECT_EQ(ParseTarget("a:1:2:3").path.string(), std::string("a:1"));

  EXPECT_EQ(ParseTarget("host:port").path.string(), std::string("host:port"));
  EXPECT_FALSE(ParseTarget("host:port").has_line);

  std::error_code ec;
  const fs::path dir = TempFixture("koi-target-test");
  std::filesystem::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  if (!ec) {
    const fs::path colon_name = dir / "odd:12";
    { std::ofstream out(colon_name); out << "x"; }
    const Target t = ParseTarget(colon_name.string());
    EXPECT_EQ(t.path.string(), colon_name.string());
    EXPECT_FALSE(t.has_line);

    const Target t2 = ParseTarget(colon_name.string() + ":3");
    EXPECT_EQ(t2.path.string(), colon_name.string());
    EXPECT_EQ(t2.line, Index{3});

    const fs::path plain = dir / "plain.txt";
    { std::ofstream out(plain); out << "a\nb\nc\n"; }
    const Target t3 = ParseTarget(plain.string() + ":2:1");
    EXPECT_EQ(t3.path.string(), plain.string());
    EXPECT_EQ(t3.line, Index{2});
    EXPECT_EQ(t3.column, Index{1});
    std::filesystem::remove_all(dir, ec);
  }
}

void TargetPositioning() {
  TEST_CASE("positioning at a target");
  Document doc;
  ResetToOriginal(doc.table, "alpha\nbravo charlie\n\tindented\n" + std::string(kFamily) + "tail\n");

  Target t;
  t.line = 2; t.has_line = true;
  GoToTarget(doc, t);
  EXPECT_EQ(LineAt(doc.table, CursorOf(doc.table, doc.selections.Primary())), Index{1});
  EXPECT_EQ(CursorOf(doc.table, doc.selections.Primary()), Index{6});

  t.column = 7; t.has_column = true;
  GoToTarget(doc, t);
  EXPECT_EQ(CursorOf(doc.table, doc.selections.Primary()), Index{12});

  t.column = 999;
  GoToTarget(doc, t);
  EXPECT_EQ(LineAt(doc.table, CursorOf(doc.table, doc.selections.Primary())), Index{1});

  t.line = 9999; t.column = 1;
  GoToTarget(doc, t);
  EXPECT_EQ(LineAt(doc.table, CursorOf(doc.table, doc.selections.Primary())), LineCount(doc.table) - 1);

  t.line = 4; t.column = 3;
  GoToTarget(doc, t);
  EXPECT_TRUE(IsGraphemeBoundary(doc.table, CursorOf(doc.table, doc.selections.Primary())));
}

void ViewportScrolling() {
  TEST_CASE("viewport scrolling");
  Document doc;
  std::string text;
  for (int i = 0; i < 200; ++i) text += "line " + std::to_string(i) + "\n";
  ResetToOriginal(doc.table, text);

  Viewport view;
  view.rows = 10;
  view.columns = 40;
  view.scrolloff = 2;

  doc.selections.Set(Selection{0, 0, -1});
  view = ScrollToCursor(doc, view);
  EXPECT_EQ(view.top_line, Index{0});

  doc.selections.Set(Selection{LineStart(doc.table, 20), LineStart(doc.table, 20), -1});
  view = ScrollToCursor(doc, view);
  EXPECT_EQ(view.top_line, Index{20} + view.scrolloff - view.rows + 1);
  EXPECT_TRUE(view.top_line <= 20);
  EXPECT_TRUE(20 < view.top_line + view.rows);

  doc.selections.Set(Selection{LineStart(doc.table, 5), LineStart(doc.table, 5), -1});
  view = ScrollToCursor(doc, view);
  EXPECT_EQ(view.top_line, Index{3});

  Viewport tight;
  tight.rows = 4;
  tight.columns = 40;
  tight.scrolloff = 50;
  doc.selections.Set(Selection{LineStart(doc.table, 100), LineStart(doc.table, 100), -1});
  const Viewport once = ScrollToCursor(doc, tight);
  const Viewport twice = ScrollToCursor(doc, once);
  EXPECT_EQ(once.top_line, twice.top_line);
  EXPECT_TRUE(once.top_line >= 0);
  EXPECT_TRUE(once.top_line <= 100);

  Document small;
  ResetToOriginal(small.table, "a\nb\n");
  Viewport big;
  big.rows = 40;
  big.columns = 40;
  small.selections.Set(Selection{0, 0, -1});
  EXPECT_EQ(ScrollToCursor(small, big).top_line, Index{0});

  Document wide;
  ResetToOriginal(wide.table, std::string(500, 'x') + "\n");
  Viewport hv;
  hv.rows = 10;
  hv.columns = 20;
  wide.selections.Set(Selection{300, 300, -1});
  hv = ScrollToCursor(wide, hv);
  EXPECT_EQ(hv.left_column, Index{300} - 20 + 1);
  wide.selections.Set(Selection{5, 5, -1});
  hv = ScrollToCursor(wide, hv);
  EXPECT_EQ(hv.left_column, Index{5});
}

void SoftWrapLayout() {
  TEST_CASE("soft wrap");

  WrapMetrics wrap;
  wrap.enabled = true;
  wrap.width = 10;
  wrap.indent = 2;
  wrap.max_wrap = 4;
  wrap.tab_width = 4;

  Document doc;
  std::vector<Index> rows;
  std::string scratch;

  {
    ResetToOriginal(doc.table, std::string(200, 'x') + "\n");
    WrapMetrics off;
    LayoutLine(doc.table, 0, off, rows, scratch);
    EXPECT_EQ(rows.size(), std::size_t{1});
    EXPECT_EQ(rows[0], Index{0});
    EXPECT_EQ(WrappedRows(doc.table, 0, off), Index{1});
  }

  {
    ResetToOriginal(doc.table, std::string(30, 'x') + "\n");
    LayoutLine(doc.table, 0, wrap, rows, scratch);
    EXPECT_EQ(rows.size(), std::size_t{4});
    EXPECT_EQ(rows[1], Index{10});
    EXPECT_EQ(rows[2], Index{18});
    EXPECT_EQ(rows[3], Index{26});
  }

  {
    ResetToOriginal(doc.table, "aaaaaa bbbbbbbbbb\n");
    LayoutLine(doc.table, 0, wrap, rows, scratch);
    EXPECT_EQ(rows.size(), std::size_t{3});
    EXPECT_EQ(rows[1], Index{7});
  }

  {
    ResetToOriginal(doc.table, "a bbbbbbbbbbbbbbbb\n");
    LayoutLine(doc.table, 0, wrap, rows, scratch);
    EXPECT_EQ(rows[1], Index{10});
  }

  {
    ResetToOriginal(doc.table, "abcdef\n");
    WrapMetrics narrow = wrap;
    narrow.width = 1;
    narrow.indent = 4;
    LayoutLine(doc.table, 0, narrow, rows, scratch);
    EXPECT_EQ(rows.size(), std::size_t{6});
  }

  {
    ResetToOriginal(doc.table, std::string(30, 'x') + "\n");
    LayoutLine(doc.table, 0, wrap, rows, scratch);
    EXPECT_EQ(RowOfPosition(rows, 0), Index{0});
    EXPECT_EQ(RowOfPosition(rows, 9), Index{0});
    EXPECT_EQ(RowOfPosition(rows, 10), Index{1});
    EXPECT_EQ(RowOfPosition(rows, 29), Index{3});
    EXPECT_EQ(ColumnBetween(doc.table, rows[1], 13, wrap.tab_width), Index{3});
    EXPECT_EQ(ByteForColumnFrom(doc.table, rows[1], 18, 3, wrap.tab_width), Index{13});
  }

  {
    std::string text;
    for (int i = 0; i < 20; ++i) text += std::string(20, 'x') + "\n";
    ResetToOriginal(doc.table, text);

    Viewport view;
    view.rows = 10;
    view.columns = 10;
    view.scrolloff = 0;

    doc.selections.Set(MinWidth1(doc.table, Selection{0, 0, -1}));
    view = ScrollToCursor(doc, view, wrap);
    EXPECT_EQ(view.top_line, Index{0});
    EXPECT_EQ(view.top_row, Index{0});
    view.left_column = 30;
    view = ScrollToCursor(doc, view, wrap);
    EXPECT_EQ(view.left_column, Index{0});

    const Index at = LineStart(doc.table, 5);
    doc.selections.Set(MinWidth1(doc.table, Selection{at, at, -1}));
    view = ScrollToCursor(doc, view, wrap);
    EXPECT_TRUE(view.top_line > 0);
    EXPECT_TRUE(view.top_line <= 5);
    const Viewport again = ScrollToCursor(doc, view, wrap);
    EXPECT_EQ(again.top_line, view.top_line);
    EXPECT_EQ(again.top_row, view.top_row);
  }

  {
    ResetToOriginal(doc.table, std::string(400, 'x') + "\n");
    Viewport view;
    view.rows = 5;
    view.columns = 10;
    view.scrolloff = 0;
    doc.selections.Set(MinWidth1(doc.table, Selection{380, 380, -1}));
    view = ScrollToCursor(doc, view, wrap);
    EXPECT_EQ(view.top_line, Index{0});
    EXPECT_TRUE(view.top_row > 0);

    LayoutLine(doc.table, 0, wrap, rows, scratch);
    const Index row = RowOfPosition(rows, 380);
    EXPECT_TRUE(row >= view.top_row);
    EXPECT_TRUE(row < view.top_row + view.rows);
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "hello\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.doc.view.left_column = 12;
    EXPECT_TRUE(!ed.settings.soft_wrap);
    RunCommands(ed, {"toggle_soft_wrap"});
    EXPECT_TRUE(ed.settings.soft_wrap);
    EXPECT_EQ(ed.doc.view.left_column, Index{0});
    RunCommands(ed, {"toggle_soft_wrap"});
    EXPECT_TRUE(!ed.settings.soft_wrap);
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{4, 4, -1}));
    RunCommands(ed, {"select_all"});
    EXPECT_EQ(ed.doc.selections.Size(), std::size_t{1});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(ed.doc.selections.Primary().To(), DocLength(ed.doc.table));
  }

  {
    const KeyMaps maps = DefaultKeyMaps();
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    std::vector<Key> pending;
    Key percent;
    EXPECT_TRUE(ParseKey("%", percent));
    HandleKeyInput(ed, maps, percent, pending);
    EXPECT_EQ(ed.doc.selections.Primary().To(), DocLength(ed.doc.table));

    Key alt_z;
    EXPECT_TRUE(ParseKey("A-z", alt_z));
    HandleKeyInput(ed, maps, alt_z, pending);
    EXPECT_TRUE(ed.settings.soft_wrap);
  }

  {
    KeyMaps maps = DefaultKeyMaps();
    Settings settings;
    std::vector<std::string> errors;
    std::ignore = ParseKeyMapConfig(
        "[editor.soft-wrap]\nenable = true\nwrap-indicator = \">> \"\nmax-wrap = 7\n", maps,
        settings, errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(settings.soft_wrap);
    EXPECT_EQ(settings.wrap_indicator, std::string{">> "});
    EXPECT_EQ(settings.max_wrap, Index{7});
  }
}

void BufferSwitching() {
  TEST_CASE("buffers keep their own history, cursors and state");
  const Scratch scratch{"koi-buffers"};
  const std::filesystem::path a = scratch.Write("a.txt", "alpha\n");
  const std::filesystem::path b = scratch.Write("b.txt", "bravo\n");
  const std::filesystem::path c = scratch.Write("c.txt", "charlie\n");

  Editor ed;
  EXPECT_TRUE(OpenTarget(ed, a.string()));
  EXPECT_EQ(BufferCount(ed), std::size_t{1});

  TypeInto(ed, 'X');
  const Index depth_in_a = UndoDepth(ed.doc.table);
  EXPECT_TRUE(depth_in_a > 0);
  EXPECT_TRUE(ed.doc.modified);
  const std::string a_edited = AssembleDocContents(ed.doc.table);

  EXPECT_TRUE(OpenTarget(ed, b.string()));
  EXPECT_EQ(BufferCount(ed), std::size_t{2});
  EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("bravo\n"));
  EXPECT_EQ(UndoDepth(ed.doc.table), Index{0});

  EXPECT_TRUE(OpenTarget(ed, a.string()));
  EXPECT_EQ(BufferCount(ed), std::size_t{2});
  EXPECT_EQ(AssembleDocContents(ed.doc.table), a_edited);
  EXPECT_EQ(UndoDepth(ed.doc.table), depth_in_a);
  EXPECT_TRUE(ed.doc.modified);

  RunCommands(ed, {"undo"});
  EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("alpha\n"));

  TEST_CASE("buffers: reads are correct after a document has been moved");

  char byte = 0;
  EXPECT_TRUE(ByteAt(ed.doc.table, 0, byte));
  EXPECT_EQ(byte, 'a');
  RunTypableCommand(ed, "bn");
  RunTypableCommand(ed, "bp");
  EXPECT_TRUE(ByteAt(ed.doc.table, 0, byte));
  EXPECT_EQ(byte, 'a');
  EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("alpha\n"));

  TEST_CASE("buffers: cycling wraps and re-opening switches rather than duplicating");
  EXPECT_TRUE(OpenTarget(ed, c.string()));
  EXPECT_EQ(BufferCount(ed), std::size_t{3});
  const std::size_t at_c = ed.active;
  RunTypableCommand(ed, "bn");
  EXPECT_EQ(ed.active, (at_c + 1) % 3);
  RunTypableCommand(ed, "bp");
  EXPECT_EQ(ed.active, at_c);

  EXPECT_TRUE(OpenTarget(ed, a.string()));
  EXPECT_EQ(BufferCount(ed), std::size_t{3});

  TEST_CASE("buffers: quitting accounts for buffers it cannot see");

  EXPECT_TRUE(OpenTarget(ed, c.string()));
  TypeInto(ed, 'W');
  EXPECT_TRUE(OpenTarget(ed, b.string()));
  EXPECT_FALSE(ed.doc.modified);

  const std::vector<std::string> unsaved = UnsavedBuffers(ed);
  EXPECT_EQ(unsaved.size(), std::size_t{1});
  EXPECT_TRUE(unsaved.front().find("c.txt") != std::string::npos);

  ed.status.clear();
  ed.quit = false;
  RunTypableCommand(ed, "q");
  EXPECT_FALSE(ed.quit);
  EXPECT_TRUE(ed.status.find("unsaved changes") != std::string::npos);
  EXPECT_TRUE(ed.status.find("c.txt") != std::string::npos);

  RunTypableCommand(ed, "qa!");
  EXPECT_TRUE(ed.quit);

  TEST_CASE("buffers: closing refuses unsaved work unless forced");
  Editor ed2;
  EXPECT_TRUE(OpenTarget(ed2, a.string()));
  EXPECT_TRUE(OpenTarget(ed2, b.string()));
  TypeInto(ed2, 'Z');
  EXPECT_TRUE(ed2.doc.modified);
  ed2.status.clear();
  RunTypableCommand(ed2, "bc");
  EXPECT_EQ(BufferCount(ed2), std::size_t{2});
  EXPECT_TRUE(ed2.status.find("unsaved changes") != std::string::npos);

  RunTypableCommand(ed2, "bc!");
  EXPECT_EQ(BufferCount(ed2), std::size_t{1});
  EXPECT_EQ(ed2.doc.file.filename().string(), std::string{"a.txt"});

  TEST_CASE("buffers: closing the last one leaves an empty buffer, not none");
  RunTypableCommand(ed2, "bc!");
  EXPECT_EQ(BufferCount(ed2), std::size_t{1});
  EXPECT_EQ(DocLength(ed2.doc.table), Index{0});
  EXPECT_TRUE(ed2.doc.file.empty());

  TypeInto(ed2, 'q');
  EXPECT_EQ(AssembleDocContents(ed2.doc.table), std::string("q"));

  TEST_CASE("buffers: the history budget is per file, not per editor");
  {

    constexpr int kEdits = 200;
    const std::filesystem::path subject = scratch.Write("subject.txt", "one\ntwo\nthree\n");

    const auto edit_subject = [&](Editor& target) {
      for (int i = 0; i < kEdits; ++i) {
        BreakUndoCoalescing(target.doc.table);
        TypeInto(target, 'a');
      }
    };

    Index bytes_alone = 0;
    Index depth_alone = 0;
    {
      Editor solo;
      EXPECT_TRUE(OpenTarget(solo, subject.string()));
      edit_subject(solo);
      bytes_alone = HistoryBytes(solo.doc.table);
      depth_alone = UndoDepth(solo.doc.table);
      EXPECT_TRUE(bytes_alone > 0);
      EXPECT_EQ(depth_alone, Index{kEdits});
    }

    Editor many;
    EXPECT_TRUE(OpenTarget(many, subject.string()));
    for (int i = 0; i < 4; ++i) {
      const std::filesystem::path other =
          scratch.Write("noise" + std::to_string(i) + ".txt", "one\ntwo\nthree\n");
      EXPECT_TRUE(OpenTarget(many, other.string()));
      for (int k = 0; k < 2 * kEdits; ++k) {
        BreakUndoCoalescing(many.doc.table);
        TypeInto(many, 'z');
      }
    }
    EXPECT_EQ(BufferCount(many), std::size_t{5});

    SwitchToBuffer(many, 0);
    EXPECT_EQ(many.doc.file.filename().string(), std::string{"subject.txt"});
    edit_subject(many);
    EXPECT_EQ(HistoryBytes(many.doc.table), bytes_alone);
    EXPECT_EQ(UndoDepth(many.doc.table), depth_alone);

    for (std::size_t i = 0; i < BufferCount(many); ++i) {
      SwitchToBuffer(many, i);
      many.doc.table.history_budget_bytes = 64 * 1024;
      BreakUndoCoalescing(many.doc.table);
      TypeInto(many, 'q');
      EXPECT_TRUE(UndoDepth(many.doc.table) >= kMinRevisionsKept - 1);
      EXPECT_TRUE(CanUndo(many.doc.table));
      const std::string before = AssembleDocContents(many.doc.table);
      RunCommands(many, {"undo"});
      EXPECT_TRUE(AssembleDocContents(many.doc.table) != before);
    }
  }

  TEST_CASE("buffers: write-all clears every buffer's modified flag");
  Editor ed3;
  EXPECT_TRUE(OpenTarget(ed3, a.string()));
  TypeInto(ed3, 'P');
  EXPECT_TRUE(OpenTarget(ed3, b.string()));
  TypeInto(ed3, 'Q');
  EXPECT_EQ(UnsavedBuffers(ed3).size(), std::size_t{2});
  RunTypableCommand(ed3, "wa");
  EXPECT_TRUE(UnsavedBuffers(ed3).empty());

  EXPECT_EQ(ed3.doc.file.filename().string(), std::string{"b.txt"});
  ed3.quit = false;
  RunTypableCommand(ed3, "q");
  EXPECT_TRUE(ed3.quit);
}

void ScrolloffIsDerivedNotCopied() {
  TEST_CASE("viewport: every buffer gets the configured scrolloff");
  const Scratch scratch{"koi-scrolloff"};
  std::string many;
  for (int i = 0; i < 200; ++i) many += "line " + std::to_string(i) + "\n";
  const std::filesystem::path a = scratch.Write("a.txt", many);
  const std::filesystem::path b = scratch.Write("b.txt", many);

  Editor ed;
  ed.theme = BuiltinTheme();
  ed.settings.scrolloff = 7;
  EXPECT_TRUE(OpenTarget(ed, a.string()));
  FitFocusedViewport(ed, 60, 20);
  EXPECT_EQ(ed.doc.view.scrolloff, Index{7});

  RunTypableCommand(ed, "new");
  FitFocusedViewport(ed, 60, 20);
  EXPECT_EQ(ed.doc.view.scrolloff, Index{7});

  EXPECT_TRUE(OpenTarget(ed, b.string()));
  SplitWindow(ed, false);
  RunTypableCommand(ed, "bp");
  FocusWindow(ed, true);
  FitFocusedViewport(ed, 60, 20);
  EXPECT_EQ(ed.doc.view.scrolloff, Index{7});

  ed.settings.scrolloff = 2;
  FitFocusedViewport(ed, 60, 20);
  EXPECT_EQ(ed.doc.view.scrolloff, Index{2});

  TEST_CASE("viewport: an align is one frame without padding, not a buffer without scrolloff");
  {
    Editor one;
    one.theme = BuiltinTheme();
    one.settings.scrolloff = 7;
    EXPECT_TRUE(OpenTarget(one, a.string()));
    FitFocusedViewport(one, 60, 20);

    Target middle;
    middle.line = 100;
    middle.has_line = true;
    GoToTarget(one.doc, middle);
    FitFocusedViewport(one, 60, 20);

    RunCommands(one, {"align_view_center"});
    const Index asked = one.doc.view.top_line;
    FitFocusedViewport(one, 60, 20);

    EXPECT_EQ(one.doc.view.top_line, asked);

    FitFocusedViewport(one, 60, 20);
    EXPECT_EQ(one.doc.view.scrolloff, Index{7});
  }
}

void ScrollingPastTheEndOfTheFile() {
  const Scratch scratch{"koi-past-eof"};
  std::string many;
  for (int i = 1; i <= 60; ++i) many += "line " + std::to_string(i) + "\n";
  const std::filesystem::path file = scratch.Write("a.txt", many);

  constexpr int kW = 40;
  constexpr int kH = 21;

  const auto fresh = [&file] {
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.scrolloff = 3;
    EXPECT_TRUE(OpenTarget(ed, file.string()));
    FitFocusedViewport(ed, kW, kH);
    return ed;
  };
  const auto go = [](Editor& ed, Index line) {
    Target at;
    at.line = line;
    at.has_line = true;
    GoToTarget(ed.doc, at);
    FitFocusedViewport(ed, kW, kH);
  };

  TEST_CASE("viewport: the last line may sit anywhere on screen, not only at the bottom");
  {
    Editor ed = fresh();
    const Index rows = ed.doc.view.rows;
    EXPECT_EQ(rows, Index{20});

    go(ed, 58);
    RunCommands(ed, {"align_view_center"});
    const Index centred = LineAt(ed.doc.table, ed.doc.selections.Primary().head) - rows / 2;
    EXPECT_EQ(ed.doc.view.top_line, centred);
    FitFocusedViewport(ed, kW, kH);
    EXPECT_EQ(ed.doc.view.top_line, centred);
    FitFocusedViewport(ed, kW, kH);
    EXPECT_EQ(ed.doc.view.top_line, centred);
    EXPECT_TRUE(ed.doc.view.top_line > (LineCount(ed.doc.table) - rows));

    RunCommands(ed, {"align_view_top"});
    FitFocusedViewport(ed, kW, kH);
    EXPECT_EQ(ed.doc.view.top_line, LineAt(ed.doc.table, ed.doc.selections.Primary().head));
  }

  TEST_CASE("viewport: scrolloff still applies on the last line");
  {
    Editor ed = fresh();
    RunCommands(ed, {"goto_last_line"});
    FitFocusedViewport(ed, kW, kH);
    const Index line = LineAt(ed.doc.table, ed.doc.selections.Primary().head);
    EXPECT_EQ(ed.doc.view.top_line, line + 3 - ed.doc.view.rows + 1);
  }

  TEST_CASE("viewport: scrolling stops at the last line, not the last screenful");
  {
    Editor ed = fresh();
    ed.settings.scrolloff = 0;
    FitFocusedViewport(ed, kW, kH);
    const Index last = LineCount(ed.doc.table) - 1;

    for (int i = 0; i < 200; ++i) RunCommands(ed, {"scroll_down"});
    FitFocusedViewport(ed, kW, kH);
    EXPECT_EQ(ed.doc.view.top_line, last);
    EXPECT_EQ(LineAt(ed.doc.table, ed.doc.selections.Primary().head), last);
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    Surface frame;
    RenderTo(ed, frame, kW, kH);
    EXPECT_EQ(frame.cells.size(), static_cast<std::size_t>(kW) * kH);
    RunCommands(ed, {"scroll_up"});
    FitFocusedViewport(ed, kW, kH);
    RenderTo(ed, frame, kW, kH);
    EXPECT_TRUE(frame.Row(0).find("line 60") != std::string::npos);

    for (int i = 0; i < 200; ++i) RunCommands(ed, {"scroll_up"});
    FitFocusedViewport(ed, kW, kH);
    EXPECT_EQ(ed.doc.view.top_line, Index{0});
  }

  TEST_CASE("viewport: a file shorter than the pane still starts at the top");
  {
    const std::filesystem::path tiny = scratch.Write("tiny.txt", "one\ntwo\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, tiny.string()));
    FitFocusedViewport(ed, kW, kH);
    EXPECT_EQ(ed.doc.view.top_line, Index{0});
    RunCommands(ed, {"goto_last_line", "align_view_center"});
    FitFocusedViewport(ed, kW, kH);
    EXPECT_EQ(ed.doc.view.top_line, Index{0});
  }
}

void OpeningAFileKeepsThePanesFittedSize() {
  TEST_CASE("viewport: opening a file keeps the pane's fitted size, so a view command "
            "chained onto the open works against the real height");
  const Scratch scratch{"koi-open-keeps-fit"};
  std::string many;
  for (int i = 0; i < 400; ++i) many += "line " + std::to_string(i) + "\n";
  const std::filesystem::path a = scratch.Write("a.txt", many);
  const std::filesystem::path b = scratch.Write("b.txt", many);

  Editor ed;
  ed.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(ed, a.string()));
  FitFocusedViewport(ed, 60, 50);
  const Index fitted_rows = ed.doc.view.rows;
  EXPECT_TRUE(fitted_rows > Index{24});

  EXPECT_TRUE(OpenTarget(ed, b.string()));
  EXPECT_EQ(ed.doc.view.rows, fitted_rows);

  Target middle;
  middle.line = 300;
  middle.has_line = true;
  GoToTarget(ed.doc, middle);

  RunCommands(ed, {"align_view_center"});
  const Index line = LineAt(ed.doc.table, ed.doc.selections.Primary().head);
  const Index expected_top = std::max<Index>(0, line - fitted_rows / 2);
  EXPECT_EQ(ed.doc.view.top_line, expected_top);

  FitFocusedViewport(ed, 60, 50);
  EXPECT_EQ(ed.doc.view.top_line, expected_top);
}

void StatusSeverity() {
  TEST_CASE("status line: severity, icons and the read-only marker");

  Editor ed;
  ResetToOriginal(ed.doc.table, "hello\n");
  ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

  ed.status.Fail("broke");
  EXPECT_TRUE(ed.status.level() == StatusLevel::kError);
  ed.status = "just so you know";
  EXPECT_TRUE(ed.status.level() == StatusLevel::kError);
  ed.status.Warn("will not do that");
  EXPECT_TRUE(ed.status.level() == StatusLevel::kError);
  ed.status.clear();
  EXPECT_TRUE(ed.status.level() == StatusLevel::kInfo);
  ed.status.Warn("will not do that");
  EXPECT_TRUE(ed.status.level() == StatusLevel::kWarning);
  ed.status = "informational";
  EXPECT_TRUE(ed.status.level() == StatusLevel::kWarning);
  ed.status.clear();
  EXPECT_TRUE(ed.status.level() == StatusLevel::kInfo);
  EXPECT_TRUE(ed.status.empty());

  const auto joined = [](const std::vector<StatusSpan>& spans, StatusTone tone) {
    std::string out;
    for (const StatusSpan& span : spans) {
      if (span.tone == tone) out += span.text;
    }
    return out;
  };
  const auto all = [](const std::vector<StatusSpan>& spans) {
    std::string out;
    for (const StatusSpan& span : spans) out += span.text;
    return out;
  };

  ed.status.Fail("could not write");
  {
    const StatusLine bar = StatusBar(ed);
    EXPECT_TRUE(joined(bar.left, StatusTone::kError).find("could not write") !=
                std::string::npos);
    EXPECT_TRUE(joined(bar.left, StatusTone::kError).find(ed.settings.icon_error) !=
                std::string::npos);
    EXPECT_TRUE(joined(bar.left, StatusTone::kStrong).find("could not") == std::string::npos);
  }

  ed.status.Fail("first line\nsecond\tline\n\nthird");
  {
    const std::string text = all(StatusBar(ed).left);
    EXPECT_TRUE(text.find('\n') == std::string::npos);
    EXPECT_TRUE(text.find('\t') == std::string::npos);
    EXPECT_TRUE(text.find("first line second line third") != std::string::npos);
  }

  // A marked destination is its own span, toned apart from the words around
  // it, and the whole message still reads back in order. Cleared first: a
  // plain assignment keeps the level the last Fail left behind.
  ed.status.clear();
  ed.status = "jump 1/9  next koi/CMakeLists.txt";
  ed.status.Highlight(15, 18);
  {
    const StatusLine bar = StatusBar(ed);
    EXPECT_TRUE(joined(bar.left, StatusTone::kNext) == std::string{"koi/CMakeLists.txt"});
    EXPECT_TRUE(joined(bar.left, StatusTone::kInfo).find("jump 1/9 next") != std::string::npos);
    EXPECT_TRUE(all(bar.left).find("jump 1/9 next koi/CMakeLists.txt") != std::string::npos);
  }
  // The mark belongs to the message it was set on.
  ed.status = "plain";
  EXPECT_TRUE(joined(StatusBar(ed).left, StatusTone::kNext).empty());

  ed.status.clear();
  ed.doc.file = "some/deep/dir/thing.cpp";
  {
    const StatusLine bar = StatusBar(ed);
    EXPECT_TRUE(joined(bar.left, StatusTone::kStrong) == std::string{"thing.cpp"});
    EXPECT_TRUE(joined(bar.left, StatusTone::kDim).find("some/deep/dir/") != std::string::npos);
  }

  ed.doc.read_only = true;
  ed.doc.modified = true;
  EXPECT_TRUE(all(StatusBar(ed).left).find(ed.settings.icon_readonly) != std::string::npos);
  EXPECT_TRUE(all(StatusBar(ed).left).find(ed.settings.icon_modified) != std::string::npos);
  ed.settings.icons = false;
  {
    const std::string text = all(StatusBar(ed).left);
    EXPECT_TRUE(text.find("[ro]") != std::string::npos);
    EXPECT_TRUE(text.find("[+]") != std::string::npos);
  }
  ed.settings.icons = true;

  {
    EXPECT_TRUE(all(StatusBar(ed).right).find(" sel") == std::string::npos);
    auto ranges = ed.doc.selections.Ranges();
    ranges.push_back(Selection{2, 3, -1});
    ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
    EXPECT_TRUE(all(StatusBar(ed).right).find("2 sel") != std::string::npos);
  }
  EXPECT_TRUE(joined(StatusBar(ed).right, StatusTone::kNormal).find(':') != std::string::npos);
  EXPECT_TRUE(joined(StatusBar(ed).right, StatusTone::kDim).find("᛫") != std::string::npos);
  ed.settings.icons = false;
  EXPECT_TRUE(joined(StatusBar(ed).right, StatusTone::kDim).find("·") != std::string::npos);
  EXPECT_TRUE(joined(StatusBar(ed).right, StatusTone::kDim).find("᛫") == std::string::npos);
  ed.settings.icons = true;
  EXPECT_TRUE(joined(StatusBar(ed).left, StatusTone::kDim).find("ᛉ") != std::string::npos);
  ed.mode = Mode::kInsert;
  EXPECT_TRUE(joined(StatusBar(ed).left, StatusTone::kDim).find("ᚲ") != std::string::npos);
  ed.mode = Mode::kNormal;
  EXPECT_TRUE(all(StatusBar(ed).right).find("cpp") == std::string::npos);
}

}  // namespace koi
