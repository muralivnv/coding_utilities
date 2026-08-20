// Tests for commands.cpp: the command registry, and what each command does to
// a buffer when it runs.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

namespace {

void RemoveQuietly(const std::filesystem::path& p) {
  std::error_code ec;
  std::filesystem::remove(p, ec);
}

}  // namespace

void CommandRegistry() {
  TEST_CASE("command registry");
  const auto commands = AllCommands();
  EXPECT_TRUE(commands.size() > 40);
  for (const CommandDef& def : commands) {
    EXPECT_TRUE(FindCommand(def.name) == &def);
    EXPECT_TRUE(!def.help.empty());
    EXPECT_FALSE(IsKnownUnimplemented(def.name));
  }
  EXPECT_TRUE(FindCommand("definitely_not_a_command") == nullptr);
  EXPECT_TRUE(IsKnownUnimplemented("repeat_last_motion"));

  KeyMaps maps = DefaultKeyMaps();
  std::vector<std::string> errors;
  EXPECT_FALSE(ParseKeyMapConfig("", maps, errors));
  const auto check = [](auto&& self, const KeyNode& node) -> void {
    for (const std::string& name : node.commands) {
      if (!name.empty() && (name.front() == ':')) continue;
      if (FindCommand(name) == nullptr) {
        ++common::g_test_failures;
        std::cerr << "FAIL [" << common::g_test_case << "] default keymap names unknown command \""
                  << name << "\"" << std::endl;
      } else {
        ++common::g_test_checks;
      }
    }
    for (const auto& [key, child] : node.children) self(self, child);
  };
  check(check, maps.normal.Root());
  check(check, maps.insert.Root());
  EXPECT_FALSE(maps.normal.Empty());
  EXPECT_FALSE(maps.insert.Empty());
}

void EditingModel() {
  TEST_CASE("editing model");

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "0123456789\nabcdefghij\n");
    ed.doc.view.rows = 10;
    ed.doc.selections.Set(Selection{0, 0, -1});
    ApplyModeInvariants(ed);

    ed.pending_count = 5;
    RunCommands(ed, {"move_char_right"});
    EXPECT_EQ(Cur(ed), Index{5});
    EXPECT_EQ(ed.pending_count, Index{0});
    RunCommands(ed, {"move_char_right"});
    EXPECT_EQ(Cur(ed), Index{6});

    ed.pending_count = 3;
    RunCommands(ed, {"move_char_left"});
    EXPECT_EQ(Cur(ed), Index{3});

    ed.pending_count = 2;
    RunCommands(ed, {"move_char_right", "move_char_right"});
    EXPECT_EQ(Cur(ed), Index{7});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\nfour\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    ApplyModeInvariants(ed);

    RunCommands(ed, {"extend_line"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{4});
    RunCommands(ed, {"extend_line"});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{8});
    RunCommands(ed, {"extend_line"});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{14});

    RunCommands(ed, {"delete_selection"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("four\n"));

    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\nfour\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    ApplyModeInvariants(ed);
    ed.pending_count = 3;
    RunCommands(ed, {"extend_line"});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{14});

    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\nfour\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{8, 8, -1}));
    RunCommands(ed, {"extend_line"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{8});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{14});
    EXPECT_FALSE(ed.doc.selections.Primary().Backward());

    RunCommands(ed, {"flip_selections"});
    EXPECT_TRUE(ed.doc.selections.Primary().Backward());
    RunCommands(ed, {"extend_line"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{4});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{14});
    RunCommands(ed, {"extend_line"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{14});
    RunCommands(ed, {"extend_line"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "abcdef\n\nabcdef\nabcdef\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{4, 4, -1}));
    RunCommands(ed, {"copy_selection_on_next_line"});
    EXPECT_EQ(ed.doc.selections.Size(), size_t{2});
    EXPECT_EQ(LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Ranges()[1])), Index{2});
    RunCommands(ed, {"copy_selection_on_next_line"});
    EXPECT_EQ(ed.doc.selections.Size(), size_t{3});
    EXPECT_EQ(LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Ranges()[2])), Index{3});
    for (const Selection& sel : ed.doc.selections.Ranges()) {
      EXPECT_EQ(ColumnForByte(ed.doc.table, CursorOf(ed.doc.table, sel), ed.doc.tab_width), Index{4});
    }
    RunCommands(ed, {"copy_selection_on_next_line"});
    EXPECT_EQ(ed.doc.selections.Size(), size_t{3});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "a b\tc\nnext\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"find_next_char"});
    ApplyPendingChar(ed, " ");
    EXPECT_EQ(Cur(ed), Index{1});

    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"find_next_char"});
    ApplyPendingChar(ed, "\t");
    EXPECT_EQ(Cur(ed), Index{3});

    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.status.clear();
    RunCommands(ed, {"find_next_char"});
    ApplyPendingChar(ed, "\n");
    EXPECT_EQ(Cur(ed), Index{5});
    EXPECT_TRUE(ed.status.empty());
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"find_till_char"});
    ApplyPendingChar(ed, "\n");
    EXPECT_EQ(Cur(ed), Index{4});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "aa\nbb\n\ncc\n");
    ed.doc.selections.Set(Selection{0, DocLength(ed.doc.table), -1});
    RunCommands(ed, {"split_selection_on_newline"});
    EXPECT_EQ(ed.doc.selections.Size(), size_t{4});
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Ranges()[0].Range()), std::string("aa"));
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Ranges()[1].Range()), std::string("bb"));
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Ranges()[3].Range()), std::string("cc"));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "a,bb,ccc\n");
    ed.doc.selections.Set(Selection{0, 8, -1});
    RunCommands(ed, {"split_selection"});
    EXPECT_TRUE(ed.pending_char == PendingChar::kSplitOn);
    ApplyPendingChar(ed, ",");
    EXPECT_EQ(ed.doc.selections.Size(), size_t{3});
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Ranges()[2].Range()), std::string("ccc"));
    EXPECT_TRUE(ed.pending_char == PendingChar::kNone);
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one two three\n");
    ed.doc.selections.Replace(ed.doc.table, {{0, 3, -1}, {4, 7, -1}, {8, 13, -1}});
    RunCommands(ed, {"rotate_selection_contents_forward"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("three one two\n"));
    RunCommands(ed, {"rotate_selection_contents_backward"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("one two three\n"));
    RunCommands(ed, {"undo", "undo"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("one two three\n"));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "abcdef\n");
    ed.doc.selections.Replace(ed.doc.table, {{0, 3, -1}, {3, 6, -1}});
    RunCommands(ed, {"surround_add"});
    ApplyPendingChar(ed, "(");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("(abc)(def)\n"));
    EXPECT_EQ(ed.doc.selections.Ranges().size(), std::size_t{2});
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Ranges()[0].Range()),
              std::string("(abc)"));
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Ranges()[1].Range()),
              std::string("(def)"));

    ResetToOriginal(ed.doc.table, "aaa\nbbb\n");
    ed.doc.selections.Set(Selection{0, 7, -1});
    SelectRegex(ed, ".");
    EXPECT_EQ(ed.doc.selections.Ranges().size(), std::size_t{6});
    RunCommands(ed, {"surround_add"});
    ApplyPendingChar(ed, "(");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("(a)(a)(a)\n(b)(b)(b)\n"));
    EXPECT_EQ(ed.doc.selections.Ranges().size(), std::size_t{6});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "hello world\n");
    ed.doc.selections.Set(Selection{0, 5, -1});
    RunCommands(ed, {"surround_add"});
    ApplyPendingChar(ed, "(");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("(hello) world\n"));
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range()), std::string("(hello)"));

    RunCommands(ed, {"surround_replace"});
    ApplyPendingChar(ed, "(");
    EXPECT_TRUE(ed.pending_char == PendingChar::kSurroundReplaceTo);
    ApplyPendingChar(ed, "[");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("[hello] world\n"));

    RunCommands(ed, {"surround_delete"});
    ApplyPendingChar(ed, "[");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("hello world\n"));

    ResetToOriginal(ed.doc.table, "a(b(c)d)e\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{4, 4, -1}));
    RunCommands(ed, {"surround_delete"});
    ApplyPendingChar(ed, "(");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("a(bcd)e\n"));
    EXPECT_EQ(Cur(ed), Index{3});

    ResetToOriginal(ed.doc.table, "a(b(cd)e)f\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{4, 4, -1}));
    RunCommands(ed, {"select_textobject_inner"});
    ApplyPendingChar(ed, "(");
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range()), std::string("cd"));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{4, 4, -1}));
    RunCommands(ed, {"select_textobject_around"});
    ApplyPendingChar(ed, "(");
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range()), std::string("(cd)"));

    ResetToOriginal(ed.doc.table, "call(alpha, beta) end\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{6, 6, -1}));
    RunCommands(ed, {"select_textobject_inner"});
    ApplyPendingChar(ed, "w");
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range()), std::string("alpha"));

    ed.status.clear();
    RunCommands(ed, {"select_textobject_inner"});
    ApplyPendingChar(ed, "f");
    EXPECT_TRUE(ed.status.find("no tree-sitter grammar") != std::string::npos);
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range()), std::string("alpha"));

    ResetToOriginal(ed.doc.table, "plain\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{2, 2, -1}));
    ed.status.clear();
    RunCommands(ed, {"surround_delete"});
    ApplyPendingChar(ed, "(");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("plain\n"));
    EXPECT_TRUE(ed.status.find("no enclosing pair") != std::string::npos);
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "f(a(b)c)\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{1, 1, -1}));
    RunCommands(ed, {"match_brackets"});
    EXPECT_EQ(Cur(ed), Index{7});
    RunCommands(ed, {"match_brackets"});
    EXPECT_EQ(Cur(ed), Index{1});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "a = 1\nbbbb = 2\ncc = 3\n");
    ed.doc.selections.Replace(ed.doc.table, {{2, 3, -1}, {11, 12, -1}, {18, 19, -1}});
    RunCommands(ed, {"align_selections"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("a    = 1\nbbbb = 2\ncc   = 3\n"));
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Ranges()[0].Range()), std::string("="));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{5, 5, -1}));
    RunCommands(ed, {"add_newline_above"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("one\n\ntwo\n"));
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{2});
    RunCommands(ed, {"add_newline_below"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("one\n\ntwo\n\n"));
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{2});
  }

  // A count at several cursors, small enough that the bound below never comes
  // near: exactly count blank lines per distinct line, in both directions, and
  // the cursors still sit on the text they started on.
  {
    const std::string original = "aa\nbb\ncc\ndd\nee\n";
    for (const bool below : {false, true}) {
      Editor ed;
      ResetToOriginal(ed.doc.table, original);
      // The second character of each line, not the first: a cursor sitting
      // exactly on an insertion point stays put and the selection widens over
      // the new blank lines, which is a separate quirk of MapPosition and not
      // what this case is about.
      ed.doc.selections.Replace(ed.doc.table, {{4, 5, -1}, {7, 8, -1}, {10, 11, -1}});
      EXPECT_EQ(ed.doc.selections.Size(), std::size_t{3});
      ed.pending_count = 2;
      RunCommands(ed, {below ? "add_newline_below" : "add_newline_above"});
      EXPECT_EQ(AssembleDocContents(ed.doc.table),
                below ? std::string("aa\nbb\n\n\ncc\n\n\ndd\n\n\nee\n")
                      : std::string("aa\n\n\nbb\n\n\ncc\n\n\ndd\nee\n"));
      EXPECT_EQ(DocLength(ed.doc.table), static_cast<Index>(original.size()) + 6);
      EXPECT_EQ(ed.doc.selections.Size(), std::size_t{3});
      EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Ranges()[0].Range()), std::string("b"));
      EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Ranges()[1].Range()), std::string("c"));
      EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Ranges()[2].Range()), std::string("d"));
      EXPECT_EQ(EditorInvariants(ed), std::string{});
      EXPECT_TRUE(ed.status.find("too many blank lines") == std::string::npos);
      RunCommands(ed, {"undo"});
      EXPECT_EQ(AssembleDocContents(ed.doc.table), original);
    }
  }

  // The count multiplies against the cursor count, and nothing caps either
  // factor: `%`, `<A-s>` and a slipped six-figure count used to ask for tens of
  // gigabytes of newlines in one keystroke. The product is bounded now, so the
  // document grows by at most the budget however the two factors are chosen.
  {
    constexpr Index kMaxInsertedBytes = 8 << 20;
    constexpr Index kLines = 200;
    std::string original;
    for (Index i = 0; i < kLines; ++i) original += "x\n";

    Editor ed;
    ResetToOriginal(ed.doc.table, original);
    const Index before = DocLength(ed.doc.table);
    RunCommands(ed, {"select_all", "split_selection_on_newline"});
    EXPECT_EQ(ed.doc.selections.Size(), static_cast<std::size_t>(kLines));

    ed.pending_count = 100000;
    RunCommands(ed, {"add_newline_below"});

    const Index growth = DocLength(ed.doc.table) - before;
    EXPECT_TRUE(growth > 0);
    EXPECT_TRUE(growth <= kMaxInsertedBytes);
    // Not a bail-out either: the budget is spent, just not exceeded.
    EXPECT_TRUE(growth > kMaxInsertedBytes - kLines);
    EXPECT_TRUE(ed.status.find("too many blank lines") != std::string::npos);
    EXPECT_EQ(ed.doc.selections.Size(), static_cast<std::size_t>(kLines));
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    RunCommands(ed, {"undo"});
    EXPECT_EQ(DocLength(ed.doc.table), before);
    EXPECT_EQ(AssembleDocContents(ed.doc.table), original);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha beta\nsecond one\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"replace"});
    EXPECT_TRUE(ed.pending_char == PendingChar::kReplaceChar);
    ApplyPendingChar(ed, "X");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("Xlpha beta\nsecond one\n"));

    ResetToOriginal(ed.doc.table, "alpha beta\n");
    ed.doc.selections.Set(Selection{0, 6, -1});
    RunCommands(ed, {"replace"});
    ApplyPendingChar(ed, "Z");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("ZZZZZZbeta\n"));
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{6});

    ResetToOriginal(ed.doc.table, "ab\ncd\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"extend_line", "replace"});
    ApplyPendingChar(ed, "-");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("---cd\n"));

    ResetToOriginal(ed.doc.table, "abc\n");
    ed.doc.selections.Set(Selection{0, 3, -1});
    RunCommands(ed, {"replace"});
    ApplyPendingChar(ed, kCJK);
    EXPECT_EQ(AssembleDocContents(ed.doc.table),
              std::string(kCJK) + std::string(kCJK) + std::string(kCJK) + "\n");
    EXPECT_EQ(CountGraphemes(ed.doc.table, ed.doc.selections.Primary().Range()), Index{3});

    ResetToOriginal(ed.doc.table, "aa\nbb\n");
    ed.doc.selections.Replace(ed.doc.table, {{0, 2, -1}, {3, 5, -1}});
    RunCommands(ed, {"replace"});
    ApplyPendingChar(ed, "Q");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("QQ\nQQ\n"));
    RunCommands(ed, {"undo"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("aa\nbb\n"));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "keep swap\n");
    ed.doc.selections.Set(Selection{0, 4, -1});
    RunCommands(ed, {"yank"});
    ed.doc.selections.Set(Selection{5, 9, -1});
    RunCommands(ed, {"replace_with_yanked"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("keep keep\n"));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "  foo_bar(baz);\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{4, 4, -1}));
    RunCommands(ed, {"expand_selection"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range()), std::string("foo_bar"));
    RunCommands(ed, {"expand_selection"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range()),
              std::string("  foo_bar(baz);"));
    RunCommands(ed, {"expand_selection"});
    EXPECT_EQ(ed.doc.selections.Primary().To(), DocLength(ed.doc.table));

    ResetToOriginal(ed.doc.table, "  foo_bar(baz);\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{6, 6, -1}));
    RunCommands(ed, {"collapse_selection", "expand_selection", "trim_selections"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range()), std::string("foo_bar"));
  }

  {
    // A blank line carries no content, only a terminator. Measuring the line
    // rung against the content range collapsed it onto the line start, no rung
    // was satisfiable, and a single press handed the next d/c/r the whole
    // buffer -- silently, from a caret one line long.
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\n\ntwo three\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{4, 4, -1}));
    RunCommands(ed, {"collapse_selection", "expand_selection"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{4});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{5});
    RunCommands(ed, {"expand_selection"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(ed.doc.selections.Primary().To(), DocLength(ed.doc.table));

    // The control: the ladder on a line that does have content is untouched.
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{5, 5, -1}));
    RunCommands(ed, {"expand_selection"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range()), std::string("two"));
    RunCommands(ed, {"expand_selection"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range()),
              std::string("two three"));
    RunCommands(ed, {"expand_selection"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(ed.doc.selections.Primary().To(), DocLength(ed.doc.table));

    // The same blank line as the last line with a terminator: the rung ends at
    // the end of the document rather than at the next line's start.
    ResetToOriginal(ed.doc.table, "one\n\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{4, 4, -1}));
    RunCommands(ed, {"collapse_selection", "expand_selection"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{4});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{5});
    RunCommands(ed, {"expand_selection"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(ed.doc.selections.Primary().To(), DocLength(ed.doc.table));

    // And past the last terminator, where there is no rung left to climb: the
    // line after the final one has no start, so this must still reach the file
    // rather than run off the end of the line index.
    ed.doc.selections.Set(Selection{DocLength(ed.doc.table), DocLength(ed.doc.table), -1});
    RunCommands(ed, {"expand_selection"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(ed.doc.selections.Primary().To(), DocLength(ed.doc.table));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "ab\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"insert_mode"});
    EXPECT_TRUE(ed.mode == Mode::kInsert);
    ExpectOk(InsertAtCursorsKeeping("XY", ed.doc.table, ed.doc.selections), "typing");
    RunCommands(ed, {"normal_mode"});
    EXPECT_EQ(Cur(ed), Index{2});
    EXPECT_FALSE(ed.doc.selections.Primary().IsEmpty());
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha beta gamma\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"insert_mode", "move_next_word_start"});
    ExpectOk(InsertAtCursorsKeeping("X", ed.doc.table, ed.doc.selections), "type after a motion");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("alphaX beta gamma\n"));

    ResetToOriginal(ed.doc.table, "alpha beta gamma\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.mode = Mode::kNormal;
    RunCommands(ed, {"insert_mode", "move_char_right"});
    ExpectOk(InsertAtCursorsKeeping("X", ed.doc.table, ed.doc.selections), "type after char right");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("aXlpha beta gamma\n"));

    ResetToOriginal(ed.doc.table, "alpha beta gamma\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.mode = Mode::kNormal;
    RunCommands(ed, {"insert_mode", "move_char_left"});
    ExpectOk(InsertAtCursorsKeeping("X", ed.doc.table, ed.doc.selections), "type after char left");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("Xalpha beta gamma\n"));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"insert_newline"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("\nalpha\n"));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"goto_last_line"});
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{2});
    EXPECT_EQ(Cur(ed), Index{8});
    RunCommands(ed, {"extend_to_file_end"});
    EXPECT_EQ(ed.doc.selections.Primary().To(), DocLength(ed.doc.table));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\nbeta\n");
    ed.doc.tab_width = 2;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"indent"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("  alpha\nbeta\n"));
    EXPECT_EQ(Cur(ed), Index{2});
    RunCommands(ed, {"unindent"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("alpha\nbeta\n"));
    EXPECT_EQ(Cur(ed), Index{0});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "    aaa\n    bbb\n    ccc\n");
    ed.doc.tab_width = 4;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{10, 10, -1}));
    RunCommands(ed, {"unindent"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("    aaa\nbbb\n    ccc\n"));
    EXPECT_EQ(ed.doc.selections.Primary().From(), LineStart(ed.doc.table, 1));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "    line-1\n    line-2\n    line-3\n");
    ed.doc.tab_width = 4;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{LineStart(ed.doc.table, 1),
                                                            LineStart(ed.doc.table, 1), -1}));
    RunCommands(ed, {"extend_line", "unindent"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), LineStart(ed.doc.table, 1));
    RunCommands(ed, {"delete_selection"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("    line-1\n    line-3\n"));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "        aaa\n        bbb\n        ccc\n");
    ed.doc.tab_width = 4;
    ed.doc.selections.Set(MinWidth1(
        ed.doc.table, Selection{LineStart(ed.doc.table, 1), DocLength(ed.doc.table) - 1, -1}));
    RunCommands(ed, {"unindent"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("        aaa\n    bbb\n    ccc\n"));
    EXPECT_EQ(ed.doc.selections.Primary().From(), LineStart(ed.doc.table, 1));
    RunCommands(ed, {"unindent"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("        aaa\nbbb\nccc\n"));
    EXPECT_EQ(ed.doc.selections.Primary().From(), LineStart(ed.doc.table, 1));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "\taaa\n\tbbb\n");
    ed.doc.tab_width = 4;
    ed.doc.insert_spaces = false;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{5, 5, -1}));
    RunCommands(ed, {"unindent"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("\taaa\nbbb\n"));
    EXPECT_EQ(ed.doc.selections.Primary().From(), LineStart(ed.doc.table, 1));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"extend_line", "delete_selection"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("two\n"));
    RunCommands(ed, {"undo"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("one\ntwo\n"));
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{4});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "0123456789\n");
    ed.doc.selections.Set(Selection{6, 2, -1});
    RunCommands(ed, {"insert_mode"});
    EXPECT_EQ(ed.doc.selections.Primary().head, Index{2});
    ed.mode = Mode::kNormal;
    ed.doc.selections.Set(Selection{6, 2, -1});
    RunCommands(ed, {"append_mode"});
    EXPECT_EQ(ed.doc.selections.Primary().head, Index{6});
  }

  {
    const auto cursor_offsets = [](const Editor& ed) {
      std::vector<Index> out;
      for (const Selection& s : ed.doc.selections.Ranges()) out.push_back(s.From());
      return out;
    };
    std::string fifty;
    for (int i = 0; i < 50; ++i) fifty += "line-" + std::to_string(i) + "\n";

    const auto with_count = [&](Index count) {
      Editor ed;
      ResetToOriginal(ed.doc.table, fifty);
      ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
      ed.pending_count = count;
      RunCommands(ed, {"copy_selection_on_next_line"});
      return cursor_offsets(ed);
    };

    const std::vector<Index> exact = with_count(49);
    EXPECT_EQ(exact.size(), std::size_t{50});

    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<Index> saturated = with_count(100000);
    EXPECT_TRUE((std::chrono::steady_clock::now() - t0) < std::chrono::seconds{1});
    EXPECT_EQ(saturated.size(), exact.size());
    EXPECT_TRUE(saturated == exact);

    Editor up;
    ResetToOriginal(up.doc.table, fifty);
    const Index last = LineStart(up.doc.table, 49);
    up.doc.selections.Set(MinWidth1(up.doc.table, Selection{last, last, -1}));
    up.pending_count = 100000;
    const auto t1 = std::chrono::steady_clock::now();
    RunCommands(up, {"copy_selection_on_prev_line"});
    EXPECT_TRUE((std::chrono::steady_clock::now() - t1) < std::chrono::seconds{1});
    EXPECT_EQ(up.doc.selections.Size(), std::size_t{50});
  }

  // The sweep runs *every* command, and a few of them (show_definition_excerpts,
  // show_reference_excerpts) start a project-wide scan. With no file_filter set
  // that scan falls back to the built-in `find .`, i.e. whatever happens to be
  // in the working directory -- the live build tree when the suite runs from
  // build/. Pin it to an empty fixture so the sweep scans nothing, always.
  const Scratch nothing{"koi-command-sweep"};
  const std::string empty_filter = "find " + nothing.dir.string() + " -type f -printf '%p\\n'";

  for (const CommandDef& def : AllCommands()) {
    for (const Index count : {Index{0}, Index{3}}) {
      Editor probe;
      probe.settings.file_filter = empty_filter;
      ResetToOriginal(probe.doc.table, "x\n");
      probe.doc.view.rows = 5;
      probe.doc.view.columns = 20;
      probe.doc.selections.Set(Selection{0, 0, -1});
      probe.doc.selections.EnsureBlockCursors(probe.doc.table);
      probe.pending_count = count;
      def.fn(probe);
      probe.doc.selections.EnsureBlockCursors(probe.doc.table);
      EXPECT_TRUE(IsWellFormedUtf8(AssembleDocContents(probe.doc.table)));
      for (const Selection& sel : probe.doc.selections.Ranges()) {
        EXPECT_TRUE(IsGraphemeBoundary(probe.doc.table, sel.anchor));
        EXPECT_TRUE(IsGraphemeBoundary(probe.doc.table, sel.head));
      }
    }
  }
}

void SetIndentAndLanguage() {
  TEST_CASE("typable: :set-indent and :set-language");

  const auto fresh = [] {
    Editor ed;
    ResetToOriginal(ed.doc.table, "int main() {\n\treturn 0;\n}\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    return ed;
  };

  {
    Editor ed = fresh();
    RunTypableCommand(ed, "set-indent 2");
    EXPECT_TRUE(ed.doc.insert_spaces);
    EXPECT_EQ(ed.doc.tab_width, Index{2});
    EXPECT_TRUE(ed.status.find("2 spaces") != std::string::npos);

    RunTypableCommand(ed, "set-indent tab");
    EXPECT_FALSE(ed.doc.insert_spaces);
    EXPECT_EQ(ed.doc.tab_width, Index{2});
    EXPECT_TRUE(ed.status.find("tab") != std::string::npos);

    RunTypableCommand(ed, "set-indent");
    EXPECT_FALSE(ed.doc.insert_spaces);
    EXPECT_EQ(ed.doc.tab_width, Index{2});
    EXPECT_TRUE(ed.status.find("indent: tab") != std::string::npos);
  }

  {
    Editor ed = fresh();
    RunTypableCommand(ed, "set-indent 3");
    RunCommands(ed, {"insert_tab"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, Interval(0, 3)), std::string("   "));

    Editor tabbed = fresh();
    RunTypableCommand(tabbed, "set-indent tab");
    RunCommands(tabbed, {"insert_tab"});
    EXPECT_EQ(ReadDocRange(tabbed.doc.table, Interval(0, 1)), std::string("\t"));
  }

  for (const std::string_view bad : {"0", "17", "-2", "4x", "wat", "99999999999", "2 4", "TAB"}) {
    Editor ed = fresh();
    RunTypableCommand(ed, "set-indent " + std::string{bad});
    EXPECT_TRUE(ed.doc.insert_spaces);
    EXPECT_EQ(ed.doc.tab_width, Index{4});
    EXPECT_TRUE(ed.status.find("1-16") != std::string::npos);
  }

  {
    Editor ed = fresh();
    ed.doc.file = "notes.txt";
    AttachSyntax(ed);
    EXPECT_TRUE(ed.doc.syntax == nullptr);

    RunTypableCommand(ed, "set-language");
    EXPECT_TRUE(ed.status.find("language: none") != std::string::npos);

    RunTypableCommand(ed, "set-language nonesuch");
    EXPECT_TRUE(ed.doc.syntax == nullptr);
    EXPECT_TRUE(ed.status.find("no grammar") != std::string::npos);
    EXPECT_TRUE(ed.status.find("cpp") != std::string::npos);

    RunTypableCommand(ed, "set-language cpp");
    EXPECT_TRUE(ed.doc.syntax != nullptr);
    if (ed.doc.syntax != nullptr) {
      EXPECT_EQ(ed.doc.syntax->Language(), std::string_view{"cpp"});
      EXPECT_EQ(ed.doc.capture_styles.size(), ed.doc.syntax->CaptureNames().size());
      EXPECT_TRUE(!ed.doc.capture_styles.empty());
      RunTypableCommand(ed, "set-language");
      EXPECT_TRUE(ed.status.find("language: cpp") != std::string::npos);
    }
  }

  {
    Editor ed = fresh();
    ed.doc.view_name = "from: make tests.cpp";
    AttachSyntax(ed);
    EXPECT_TRUE(ed.doc.syntax == nullptr);

    RunTypableCommand(ed, "set-language cpp");
    EXPECT_TRUE(ed.doc.syntax == nullptr);
    EXPECT_TRUE(ed.status.find("keeps its own highlighting") != std::string::npos);
  }
}

void TypableCommandList() {
  TEST_CASE("typable commands");
  const auto listed = TypableCommands();
  EXPECT_TRUE(listed.size() > 20);

  for (const TypableDef& def : listed) {
    EXPECT_TRUE(!def.help.empty());
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.file = TempFixture("koi-typable-probe.txt");
    ed.doc.view.rows = 5;
    ed.doc.view.columns = 20;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.status.clear();
    RunTypableCommand(ed, std::string{def.name});
    if (ed.status.find("unknown command") != std::string::npos) {
      ++common::g_test_failures;
      std::cerr << "FAIL [" << common::g_test_case << "] :" << def.name
                << " is listed but does not dispatch" << std::endl;
    } else {
      ++common::g_test_checks;
    }
  }
  RemoveQuietly(TempFixture("koi-typable-probe.txt"));

  {
    const std::filesystem::path other =
        TempFixture("koi-open-probe.txt");
    {
      std::ofstream out(other);
      out << "opened content\n";
    }

    Editor ed;
    ResetToOriginal(ed.doc.table, "original\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.doc.modified = true;

    RunTypableCommand(ed, "o " + other.string());
    EXPECT_EQ(BufferCount(ed), std::size_t{2});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("opened content\n"));

    {
      const std::filesystem::path multi =
          TempFixture("koi-navc-probe.txt");
      {
        std::ofstream out(multi);
        out << "line one\nline two\nline three\n";
      }
      Editor nav;
      ResetToOriginal(nav.doc.table, "");
      nav.doc.selections.Set(MinWidth1(nav.doc.table, Selection{0, 0, -1}));
      RunTypableCommand(nav, "open " + multi.string() + ":2:3");
      EXPECT_EQ(nav.doc.file.filename().string(), multi.filename().string());
      EXPECT_EQ(LineAt(nav.doc.table, Cur(nav)), Index{1});
      EXPECT_EQ(LineCount(nav.doc.table), Index{4});

      Editor nav2;
      ResetToOriginal(nav2.doc.table, "");
      nav2.doc.selections.Set(MinWidth1(nav2.doc.table, Selection{0, 0, -1}));
      RunTypableCommand(nav2, "open " + multi.string() + ":3");
      EXPECT_EQ(LineAt(nav2.doc.table, Cur(nav2)), Index{2});

      const std::filesystem::path odd =
          TempFixture("koi-navc-probe.txt:9");
      {
        std::ofstream out(odd);
        out << "odd\n";
      }
      Editor nav3;
      ResetToOriginal(nav3.doc.table, "");
      nav3.doc.selections.Set(MinWidth1(nav3.doc.table, Selection{0, 0, -1}));
      RunTypableCommand(nav3, "open " + odd.string());
      // Compared against the fixture rather than a literal: the point is that
      // the trailing ":9" is part of the name and not parsed off as a line
      // number, which this still catches.
      EXPECT_EQ(nav3.doc.file.filename().string(), odd.filename().string());
      EXPECT_TRUE(nav3.doc.file.filename().string().ends_with(".txt:9"));
      RemoveQuietly(odd);
      RemoveQuietly(multi);
    }

    RunTypableCommand(ed, "bp");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("original\n"));
    EXPECT_TRUE(ed.doc.modified);

    ed.doc.modified = false;
    RunTypableCommand(ed, "o " + other.string());
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("opened content\n"));
    EXPECT_EQ(ed.doc.file, other);
    EXPECT_FALSE(ed.doc.selections.Primary().IsEmpty());

    EXPECT_EQ(BufferCount(ed), std::size_t{2});

    ed.status.clear();
    RunTypableCommand(ed, "o");
    EXPECT_TRUE(ed.status.find("open what?") != std::string::npos);
    RemoveQuietly(other);
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "x\n");
    EXPECT_FALSE(ed.reload_config);
    RunTypableCommand(ed, "config-reload");
    EXPECT_TRUE(ed.reload_config);
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "x\n");
    RunCommands(ed, {"command_mode"});
    EXPECT_TRUE(ed.prompt_active);
    PromptInsert(ed, "q!");
    EXPECT_EQ(ed.prompt_input, std::string("q!"));
    PromptSubmit(ed);
    EXPECT_TRUE(ed.quit);
  }

  {
    namespace fs = std::filesystem;
    const fs::path dir = TempFixture("koi-write-guard");
    RemoveAllQuietly(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "guarded.txt";
    {
      std::ofstream out(file);
      out << "loaded\n";
    }

    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);

    WriteFixtureFile(file, "theirs\n");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("changed on disk") != std::string::npos);
    {
      std::ifstream in(file);
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      EXPECT_EQ(text, std::string{"theirs\n"});
    }

    RunTypableCommand(ed, "w!");
    EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);
    {
      std::ifstream in(file);
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      EXPECT_EQ(text, std::string{"loaded\n"});
    }

    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);

    WriteFixtureFile(file, "theirs again\n");
    RunTypableCommand(ed, "reload");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);

    const fs::path other = dir / "other.txt";
    {
      std::ofstream out(other);
      out << "someone else's\n";
    }
    ed.status.clear();
    RunTypableCommand(ed, "w " + other.string());
    EXPECT_TRUE(ed.status.find("exists") != std::string::npos);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"guarded.txt"});

    ed.status.clear();
    RunTypableCommand(ed, "w " + (dir / "no-such-dir" / "f.txt").string());
    EXPECT_TRUE(ed.status.find("wrote") == std::string::npos);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"guarded.txt"});

    std::error_code perm_ec;
    fs::permissions(file,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read,
                    perm_ec);
    EXPECT_TRUE(!perm_ec);
    RunTypableCommand(ed, "w");
    const fs::perms after = fs::status(file, perm_ec).permissions();
    EXPECT_TRUE((after & fs::perms::owner_exec) != fs::perms::none);

    RemoveAllQuietly(dir);
  }

  {
    namespace fs = std::filesystem;
    const fs::path dir = TempFixture("koi-dirty-flag");
    RemoveAllQuietly(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "dirty.txt";
    {
      std::ofstream out(file);
      out << "seed\n";
    }

    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.view.rows = 5;
    ed.doc.view.columns = 20;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    EXPECT_FALSE(ed.doc.modified);

    ExpectOk(Insert("x", 0, ed.doc.table), "first edit");
    ed.doc.modified = true;
    RunCommands(ed, {"undo"});
    EXPECT_FALSE(ed.doc.modified);
    RunCommands(ed, {"redo"});
    EXPECT_TRUE(ed.doc.modified);

    RunTypableCommand(ed, "w");
    EXPECT_FALSE(ed.doc.modified);
    ExpectOk(Insert("y", 1, ed.doc.table), "edit after save");
    ed.doc.modified = true;
    EXPECT_EQ(UndoDepth(ed.doc.table), Index{2});

    RunCommands(ed, {"undo"});
    EXPECT_FALSE(ed.doc.modified);
    RunCommands(ed, {"undo"});
    EXPECT_TRUE(ed.doc.modified);
    RunCommands(ed, {"redo"});
    EXPECT_FALSE(ed.doc.modified);
    RunCommands(ed, {"redo"});
    EXPECT_TRUE(ed.doc.modified);

    RemoveAllQuietly(dir);
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\nbe\ngamma longer\n");
    ed.doc.view.rows = 5;
    ed.doc.view.columns = 40;
    const Index start = LineStart(ed.doc.table, 2) + 9;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{start, start, -1}));
    RunCommands(ed, {"insert_mode"});

    RunCommands(ed, {"move_line_up"});
    EXPECT_TRUE(ed.doc.selections.Primary().IsEmpty());
    RunCommands(ed, {"move_line_up"});
    EXPECT_TRUE(ed.doc.selections.Primary().IsEmpty());
    RunCommands(ed, {"move_line_down", "move_line_down"});
    EXPECT_EQ(Cur(ed), start);

    RunCommands(ed, {"move_prev_word_start"});
    EXPECT_TRUE(ed.doc.selections.Primary().IsEmpty());
    RunCommands(ed, {"move_char_right"});
    EXPECT_TRUE(ed.doc.selections.Primary().IsEmpty());

    // Extending backwards off a zero-width caret: the selection runs from the
    // caret, not from one grapheme past it -- the caret holds nothing.
    const Index caret = Cur(ed);
    RunCommands(ed, {"extend_line_up"});
    EXPECT_TRUE(!ed.doc.selections.Primary().IsEmpty());
    EXPECT_EQ(ed.doc.selections.Primary().To(), caret);

    RunCommands(ed, {"normal_mode", "move_prev_word_start"});
    EXPECT_TRUE(!ed.doc.selections.Primary().IsEmpty());
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "aaa\nbbb\nccc\n");
    ed.doc.view.rows = 5;
    ed.doc.view.columns = 20;
    ed.doc.selections.Replace(ed.doc.table, {{0, 0, -1}, {4, 4, -1}, {8, 8, -1}});
    ExpectOk(InsertAtCursors(">> ", ed.doc.table, ed.doc.selections), "three-cursor insert");
    EXPECT_EQ(ed.doc.selections.Size(), size_t{3});
    const std::vector<Selection> typed = ed.doc.selections.Ranges();

    RunCommands(ed, {"undo"});
    EXPECT_EQ(ed.doc.selections.Size(), size_t{3});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string{"aaa\nbbb\nccc\n"});

    RunCommands(ed, {"redo"});
    EXPECT_EQ(ed.doc.selections.Size(), size_t{3});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string{">> aaa\n>> bbb\n>> ccc\n"});
    // Redo restores what the insert recorded: each cursor sat *after* what it
    // typed (widened here to a block cursor, this being normal mode). This used
    // to assert a forward span over the ">> " instead, which is what
    // UndoOrRedo's edit-span fallback produced when it threw away the recorded
    // cursors -- a caret a grapheme short of where typing left it, so the next
    // character landed inside the span.
    const std::vector<Selection>& after_redo = ed.doc.selections.Ranges();
    for (std::size_t i = 0; (i < after_redo.size()) && (i < typed.size()); ++i) {
      EXPECT_EQ(after_redo[i].From(), typed[i].From());
      EXPECT_EQ(ReadDocRange(ed.doc.table, Interval{after_redo[i].From() - 3, after_redo[i].From()}),
                std::string{">> "});
    }
  }
}

void PromptCompletion() {
  TEST_CASE("prompt: pressing : offers every command, and typing narrows it");
  Editor ed;
  ResetToOriginal(ed.doc.table, "one\n");

  EXPECT_TRUE(PromptCompletions(ed).empty());

  PromptOpen(ed, PromptKind::kCommand);
  const std::size_t all = PromptCompletions(ed).size();
  EXPECT_TRUE(all > 20);
  EXPECT_EQ(all, TypableCommands().size());

  PromptInsert(ed, "b");
  const std::vector<const TypableDef*> b = PromptCompletions(ed);
  EXPECT_TRUE(!b.empty());
  EXPECT_TRUE(b.size() < all);
  for (const TypableDef* def : b) EXPECT_TRUE(def->name.starts_with("b"));

  PromptInsert(ed, "uffer-c");
  const std::vector<const TypableDef*> narrowed = PromptCompletions(ed);
  EXPECT_TRUE(!narrowed.empty());
  EXPECT_TRUE(narrowed.size() < b.size());

  TEST_CASE("prompt: a pattern that matches nothing offers nothing");
  PromptOpen(ed, PromptKind::kCommand);
  PromptInsert(ed, "zzzznope");
  EXPECT_TRUE(PromptCompletions(ed).empty());
  EXPECT_FALSE(PromptComplete(ed));

  TEST_CASE("prompt: completion stops where the candidates stop agreeing");
  PromptOpen(ed, PromptKind::kCommand);
  PromptInsert(ed, "buffer-c");

  EXPECT_TRUE(PromptComplete(ed));
  EXPECT_EQ(ed.prompt_input, std::string{"buffer-close"});
  EXPECT_EQ(ed.prompt_cursor, ed.prompt_input.size());

  TEST_CASE("prompt: a single match completes fully and adds a space");
  PromptOpen(ed, PromptKind::kCommand);
  PromptInsert(ed, "buffer-p");
  EXPECT_TRUE(PromptComplete(ed));
  EXPECT_EQ(ed.prompt_input, std::string{"buffer-previous "});

  TEST_CASE("prompt: completing twice adds nothing the second time");
  PromptOpen(ed, PromptKind::kCommand);
  PromptInsert(ed, "vs");
  std::ignore = PromptComplete(ed);
  const std::string after_first = ed.prompt_input;

  const bool second = PromptComplete(ed);
  EXPECT_FALSE(second);
  EXPECT_EQ(ed.prompt_input, after_first);
  EXPECT_TRUE(ed.prompt_input.starts_with("vs"));

  TEST_CASE("prompt: arguments are not command names");
  PromptOpen(ed, PromptKind::kCommand);
  PromptInsert(ed, "open some/path");

  EXPECT_TRUE(PromptCompletions(ed).empty());
  EXPECT_FALSE(PromptComplete(ed));

  TEST_CASE("prompt: search and select-regex are not command menus");
  PromptOpen(ed, PromptKind::kSearch);
  PromptInsert(ed, "b");
  EXPECT_TRUE(PromptCompletions(ed).empty());
  PromptOpen(ed, PromptKind::kSelectRegex);
  PromptInsert(ed, "b");
  EXPECT_TRUE(PromptCompletions(ed).empty());

  TEST_CASE("prompt: everything offered actually runs");
  PromptOpen(ed, PromptKind::kCommand);
  for (const TypableDef* def : PromptCompletions(ed)) {

    EXPECT_TRUE(!def->name.empty());
    EXPECT_TRUE(def->name.find(' ') == std::string::npos);
  }
  PromptCancel(ed);
  EXPECT_TRUE(PromptCompletions(ed).empty());
}

void BufferAndConfigCommands() {
  TEST_CASE("typable: :bco closes the other buffers, and never unsaved ones by accident");
  {
    const Scratch scratch{"koi-bco"};
    const std::filesystem::path a = scratch.Write("ba.txt", "aaa\n");
    const std::filesystem::path b = scratch.Write("bb.txt", "bbb\n");
    const std::filesystem::path c = scratch.Write("bc.txt", "ccc\n");

    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    EXPECT_TRUE(OpenTarget(ed, b.string()));
    EXPECT_TRUE(OpenTarget(ed, c.string()));
    EXPECT_EQ(BufferCount(ed), std::size_t{3});

    RunTypableCommand(ed, "bp");
    TypeInto(ed, 'Z');
    EXPECT_TRUE(ed.doc.modified);
    RunTypableCommand(ed, "bn");
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"bc.txt"});
    ed.status.clear();
    RunTypableCommand(ed, "bco");
    EXPECT_TRUE(ed.status.find("unsaved changes") != std::string::npos);
    EXPECT_TRUE(ed.status.find("bb.txt") != std::string::npos);
    EXPECT_EQ(BufferCount(ed), std::size_t{3});

    RunTypableCommand(ed, "bco!");
    EXPECT_EQ(BufferCount(ed), std::size_t{1});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"bc.txt"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}), std::string{"ccc\n"});
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    ed.status.clear();
    RunTypableCommand(ed, "bco");
    EXPECT_TRUE(ed.status.text().empty());
    EXPECT_EQ(BufferCount(ed), std::size_t{1});

    EXPECT_TRUE(OpenTarget(ed, a.string()));
    EXPECT_TRUE(OpenTarget(ed, b.string()));
    EXPECT_EQ(BufferCount(ed), std::size_t{3});
    RunTypableCommand(ed, "buffer-close-other");
    EXPECT_EQ(BufferCount(ed), std::size_t{1});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"bb.txt"});
  }

  TEST_CASE("typable: :config-reference opens the reference that shipped with the build");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "");
    RunTypableCommand(ed, "config-reference");
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"config.reference.toml"});
    const std::string text = ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
    EXPECT_TRUE(text.find("[editor]") != std::string::npos);
    EXPECT_TRUE(text.find("Static commands:") != std::string::npos);
  }

}

void CommandExecution() {
  TEST_CASE("command execution");
  Editor ed;
  ResetToOriginal(ed.doc.table, "alpha beta\n  gamma delta\nepsilon\n");
  ed.doc.view.rows = 10;
  ed.doc.view.columns = 40;
  ed.doc.selections.Set(Selection{0, 0, -1});

  RunCommands(ed, {"move_next_word_start"});
  EXPECT_EQ(Cur(ed), Index{5});
  EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
  EXPECT_EQ(ed.doc.selections.Primary().To(), Index{6});

  RunCommands(ed, {"goto_line_start", "insert_mode"});
  EXPECT_EQ(Cur(ed), Index{0});
  EXPECT_TRUE(ed.mode == Mode::kInsert);
  RunCommands(ed, {"normal_mode"});

  RunCommands(ed, {"extend_line"});
  EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
  EXPECT_EQ(ed.doc.selections.Primary().To(), Index{11});
  RunCommands(ed, {"yank"});
  EXPECT_EQ(ed.registers.size(), size_t{1});
  EXPECT_EQ(ed.registers[0], std::string("alpha beta\n"));

  RunCommands(ed, {"delete_selection"});
  EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("  gamma delta\nepsilon\n"));
  RunCommands(ed, {"undo"});
  EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("alpha beta\n  gamma delta\nepsilon\n"));

  ed.doc.selections.Set(Selection{0, DocLength(ed.doc.table), -1});
  RunCommands(ed, {"indent"});
  EXPECT_EQ(AssembleDocContents(ed.doc.table),
            std::string("    alpha beta\n      gamma delta\n    epsilon\n"));
  RunCommands(ed, {"undo"});
  EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("alpha beta\n  gamma delta\nepsilon\n"));

  ed.doc.selections.Set(Selection{0, 0, -1});
  ed.status.clear();
  RunCommands(ed, {"move_char_right", "definitely_not_a_command", "goto_last_line"});
  EXPECT_EQ(Cur(ed), Index{1});
  EXPECT_TRUE(ed.status.find("unknown command") != std::string::npos);

  ed.status.clear();
  RunCommands(ed, {"repeat_last_motion"});
  EXPECT_TRUE(ed.status.find("does not implement yet") != std::string::npos);

  ResetToOriginal(ed.doc.table, "one\ntwo\nthree\n");
  ed.doc.selections.Replace(ed.doc.table, {{0, 3, -1}, {4, 7, -1}});
  RunCommands(ed, {"switch_to_uppercase"});
  EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("ONE\nTWO\nthree\n"));
  RunCommands(ed, {"undo"});
  EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("one\ntwo\nthree\n"));

  // Same reason as the sweep in EditingModel: a command that starts a
  // project-wide scan must not be handed the built-in `find .` over whatever
  // the working directory holds.
  const Scratch nothing{"koi-command-sweep-2"};
  const std::string empty_filter = "find " + nothing.dir.string() + " -type f -printf '%p\\n'";

  for (const CommandDef& def : AllCommands()) {
    Editor probe;
    probe.settings.file_filter = empty_filter;
    ResetToOriginal(probe.doc.table, "");
    probe.doc.view.rows = 5;
    probe.doc.view.columns = 20;
    probe.doc.selections.Set(Selection{0, 0, -1});
    def.fn(probe);
    ++common::g_test_checks;

    Editor probe2;
    probe2.settings.file_filter = empty_filter;
    ResetToOriginal(probe2.doc.table, "x\n");
    probe2.doc.view.rows = 5;
    probe2.doc.view.columns = 20;
    probe2.doc.selections.Set(Selection{DocLength(probe2.doc.table), DocLength(probe2.doc.table), -1});
    def.fn(probe2);
    EXPECT_TRUE(IsWellFormedUtf8(AssembleDocContents(probe2.doc.table)));
  }
}

void FindCharAndScroll() {
  TEST_CASE("find-char and scroll");
  Editor ed;
  ResetToOriginal(ed.doc.table, "alpha beta gamma\nsecond line\n");
  ed.doc.view.rows = 10;
  ed.doc.view.columns = 40;
  ed.doc.selections.Set(Selection{0, 0, -1});

  RunCommands(ed, {"find_next_char"});
  EXPECT_TRUE(ed.pending_char == PendingChar::kFindNext);
  EXPECT_EQ(Cur(ed), Index{0});
  ApplyPendingChar(ed, "b");
  EXPECT_EQ(Cur(ed), Index{6});
  EXPECT_TRUE(ed.pending_char == PendingChar::kNone);

  ed.doc.selections.Set(Selection{0, 0, -1});
  RunCommands(ed, {"find_till_char"});
  ApplyPendingChar(ed, "b");
  EXPECT_EQ(Cur(ed), Index{5});

  ed.doc.selections.Set(Selection{0, 0, -1});
  RunCommands(ed, {"find_next_char"});
  ApplyPendingChar(ed, "a");
  const Index first_a = Cur(ed);
  EXPECT_EQ(first_a, Index{4});
  RunCommands(ed, {"find_next_char"});
  ApplyPendingChar(ed, "a");
  EXPECT_TRUE(Cur(ed) > first_a);

  ed.doc.selections.Set(Selection{10, 10, -1});
  RunCommands(ed, {"find_prev_char"});
  ApplyPendingChar(ed, "b");
  EXPECT_EQ(Cur(ed), Index{6});
  ed.doc.selections.Set(Selection{10, 10, -1});
  RunCommands(ed, {"till_prev_char"});
  ApplyPendingChar(ed, "b");
  EXPECT_EQ(Cur(ed), Index{7});

  ed.doc.selections.Set(Selection{0, 0, -1});
  RunCommands(ed, {"extend_next_char"});
  ApplyPendingChar(ed, "g");
  EXPECT_EQ(ed.doc.selections.Primary().anchor, Index{0});
  EXPECT_EQ(Cur(ed), Index{11});

  ed.doc.selections.Set(Selection{0, 0, -1});
  RunCommands(ed, {"find_next_char"});
  ApplyPendingChar(ed, "z");
  EXPECT_EQ(Cur(ed), Index{0});
  RunCommands(ed, {"find_next_char"});
  ApplyPendingChar(ed, "s");
  EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{0});

  ed.doc.selections.Set(Selection{3, 3, -1});
  RunCommands(ed, {"find_next_char"});
  ApplyPendingChar(ed, "");
  EXPECT_EQ(Cur(ed), Index{3});
  EXPECT_TRUE(ed.pending_char == PendingChar::kNone);

  Editor uni;
  ResetToOriginal(uni.doc.table, std::string("ab") + std::string(kCJK) + "cd\n");
  uni.doc.view.rows = 10;
  uni.doc.view.columns = 40;
  uni.doc.selections.Set(Selection{0, 0, -1});
  RunCommands(uni, {"find_next_char"});
  ApplyPendingChar(uni, kCJK);
  EXPECT_EQ(Cur(uni), Index{2});

  Editor big;
  std::string text;
  for (int i = 0; i < 100; ++i) text += "line " + std::to_string(i) + "\n";
  ResetToOriginal(big.doc.table, text);
  big.doc.view.rows = 10;
  big.doc.view.columns = 40;
  big.doc.view.scrolloff = 2;
  big.doc.selections.Set(Selection{0, 0, -1});
  big.doc.view = ScrollToCursor(big.doc, big.doc.view);

  const Index before = LineAt(big.doc.table, Cur(big));
  RunCommands(big, {"scroll_down"});
  EXPECT_EQ(big.doc.view.top_line, Index{1});
  EXPECT_TRUE(LineAt(big.doc.table, Cur(big)) > before);
  big.doc.view = ScrollToCursor(big.doc, big.doc.view);
  EXPECT_EQ(big.doc.view.top_line, Index{1});

  big.doc.selections.Set(Selection{LineStart(big.doc.table, 6), LineStart(big.doc.table, 6), -1});
  big.doc.view.top_line = 0;
  big.doc.view = ScrollToCursor(big.doc, big.doc.view);
  const Index mid = LineAt(big.doc.table, Cur(big));
  RunCommands(big, {"scroll_down"});
  EXPECT_EQ(LineAt(big.doc.table, Cur(big)), mid);

  RunCommands(big, {"scroll_up"});
  RunCommands(big, {"scroll_up"});
  EXPECT_TRUE(big.doc.view.top_line >= 0);

  big.doc.selections.Set(Selection{0, 0, -1});
  big.doc.view.top_line = 0;
  big.doc.view = ScrollToCursor(big.doc, big.doc.view);
  big.pending_count = 5;
  RunCommands(big, {"scroll_down"});
  EXPECT_EQ(big.doc.view.top_line, Index{5});
  EXPECT_EQ(LineAt(big.doc.table, Cur(big)), Index{7});

  {
    // ScrollBy used to walk the cursor to the target one line at a time. The
    // count is not capped to the viewport, so that walk was bounded by the
    // document -- one pass per line, per cursor, per keypress. It is a single
    // counted move now, and it has to land where the walk landed: same bytes,
    // same goal column, and the goal column still live for the next `j`.
    std::string mixed;
    for (int i = 0; i < 400; ++i) mixed += (((i % 7) == 3) ? "ab\n" : "abcdefghij\n");

    const auto seed = [&](Editor& e, Index line, Index column) {
      ResetToOriginal(e.doc.table, mixed);
      e.doc.view.rows = 20;
      e.doc.view.columns = 40;
      e.doc.view.scrolloff = 3;
      const Index at = LineStart(e.doc.table, line) + column;
      e.doc.selections.Set(MinWidth1(e.doc.table, Selection{at, at, -1}));
      e.doc.view = ScrollToCursor(e.doc, e.doc.view);
    };

    const auto same_selections = [](const Editor& a, const Editor& b) {
      EXPECT_EQ(a.doc.selections.Size(), b.doc.selections.Size());
      if (a.doc.selections.Size() != b.doc.selections.Size()) return;
      for (std::size_t i = 0; i < a.doc.selections.Size(); ++i) {
        EXPECT_EQ(a.doc.selections.Ranges()[i].anchor, b.doc.selections.Ranges()[i].anchor);
        EXPECT_EQ(a.doc.selections.Ranges()[i].head, b.doc.selections.Ranges()[i].head);
        EXPECT_EQ(a.doc.selections.Ranges()[i].goal_column,
                  b.doc.selections.Ranges()[i].goal_column);
      }
    };

    for (const bool down : {true, false}) {
      Editor counted;
      seed(counted, down ? 0 : 399, 5);
      const Index top_before = counted.doc.view.top_line;
      counted.pending_count = 250;
      RunCommands(counted, {down ? "scroll_down" : "scroll_up"});

      // The walk the counted move replaced, driven a line at a time through
      // the ordinary vertical motion.
      Editor walked;
      seed(walked, down ? 0 : 399, 5);
      const Index steps = LineAt(counted.doc.table, Cur(counted)) -
                          LineAt(walked.doc.table, Cur(walked));
      EXPECT_TRUE(down ? (steps > 1) : (steps < -1));
      walked.doc.view.top_line = counted.doc.view.top_line;
      for (Index i = 0; i < ((steps < 0) ? -steps : steps); ++i) {
        RunCommands(walked, {down ? "move_line_down" : "move_line_up"});
      }
      same_selections(counted, walked);
      EXPECT_EQ(counted.doc.view.top_line, top_before + (down ? 250 : -250));

      // The goal column outlives the jump, not just the byte it produced: both
      // must find column 5 again on the next long line.
      RunCommands(counted, {"move_line_down"});
      RunCommands(walked, {"move_line_down"});
      same_selections(counted, walked);
    }

    const auto seed_columns = [&](Editor& e, std::initializer_list<std::pair<Index, Index>> at) {
      seed(e, 0, 0);
      std::vector<Selection> ranges;
      for (const auto& [line, column] : at) {
        const Index pos = LineStart(e.doc.table, line) + column;
        ranges.push_back(MinWidth1(e.doc.table, Selection{pos, pos, -1}));
      }
      e.doc.selections.Replace(e.doc.table, std::move(ranges));
    };
    const auto cursor_at = [](const Editor& e, std::size_t i) {
      return CursorOf(e.doc.table, e.doc.selections.Ranges()[i]);
    };
    const auto line_of = [&](const Editor& e, std::size_t i) {
      return LineAt(e.doc.table, cursor_at(e, i));
    };
    const auto column_of = [&](const Editor& e, std::size_t i) {
      return ColumnForByte(e.doc.table, cursor_at(e, i), e.doc.tab_width);
    };

    // Several cursors on separate lines, all of them above the band the scroll
    // opened up: each one is measured against the band on its own account, so
    // they gather on its first line at the columns they came from. Keeping
    // their spacing instead -- which is what shifting the whole set by the
    // primary's correction did -- means keeping two of them off-screen.
    Editor many;
    seed_columns(many, {{0, 1}, {4, 5}, {9, 9}});
    many.pending_count = 120;
    RunCommands(many, {"scroll_down"});
    EXPECT_EQ(many.doc.view.top_line, Index{120});
    EXPECT_EQ(many.doc.selections.Size(), std::size_t{3});
    for (std::size_t i = 0; i < many.doc.selections.Size(); ++i) {
      EXPECT_EQ(line_of(many, i), Index{123});
    }
    EXPECT_EQ(column_of(many, 0), Index{1});
    EXPECT_EQ(column_of(many, 1), Index{5});
    EXPECT_EQ(column_of(many, 2), Index{9});

    // Cursors that only *transiently* collide are the one deliberate
    // difference. Three columns of one line, scrolled across lines too short
    // to hold them apart: the walk normalized after every line, merged them
    // there and never let them come apart again. A counted move keeps each
    // cursor's goal column, so they separate again on the next line long
    // enough -- which is what a counted `j` over the same distance has always
    // done, and the scroll now agrees with it.
    Editor split;
    seed_columns(split, {{0, 1}, {0, 5}, {0, 9}});
    split.pending_count = 120;
    RunCommands(split, {"scroll_down"});
    EXPECT_EQ(split.doc.selections.Size(), std::size_t{3});
    EXPECT_EQ(LineAt(split.doc.table, Cur(split)), Index{123});

    Editor split_j;
    seed_columns(split_j, {{0, 1}, {0, 5}, {0, 9}});
    split_j.pending_count = 123;
    RunCommands(split_j, {"move_line_down"});
    same_selections(split, split_j);

    // A cursor left behind above the viewport is pulled into the band even
    // when the primary needs no correction at all. Measuring the whole set by
    // the primary left it up there for as long as the primary stayed happy.
    //
    // The stray one is a real selection, and moving it costs it its extent --
    // an unextended `j` does exactly that, and the scroll is not the place to
    // invent a second rule.
    Editor stray;
    seed_columns(stray, {{5, 5}, {130, 5}});
    {
      std::vector<Selection> ranges = stray.doc.selections.Ranges();
      const Index line5 = LineStart(stray.doc.table, 5);
      ranges[0] = Selection{line5 + 2, line5 + 8, -1};
      stray.doc.selections.Replace(stray.doc.table, std::move(ranges));
    }
    stray.doc.selections.SetPrimary(1);
    stray.doc.view.top_line = 120;
    RunCommands(stray, {"scroll_down"});
    EXPECT_EQ(stray.doc.view.top_line, Index{121});
    EXPECT_EQ(stray.doc.selections.Size(), std::size_t{2});
    EXPECT_EQ(stray.doc.selections.PrimaryIndex(), std::size_t{1});
    EXPECT_EQ(line_of(stray, 1), Index{130});
    EXPECT_EQ(line_of(stray, 0), Index{124});
    EXPECT_EQ(column_of(stray, 0), Index{7});
    EXPECT_EQ(stray.doc.selections.Ranges()[0].To() - stray.doc.selections.Ranges()[0].From(),
              Index{1});

    // And a cursor already inside the band is not dragged by a delta computed
    // for somebody else: the primary here has 8 lines to make up, which used
    // to push its in-band neighbour out through the bottom of the window. The
    // neighbour is a selection too, and keeps every byte of it: nothing had to
    // move it, so nothing did.
    Editor kept;
    seed_columns(kept, {{0, 5}, {20, 5}});
    {
      std::vector<Selection> ranges = kept.doc.selections.Ranges();
      const Index line20 = LineStart(kept.doc.table, 20);
      ranges[1] = Selection{line20 + 2, line20 + 8, -1};
      kept.doc.selections.Replace(kept.doc.table, std::move(ranges));
    }
    kept.doc.selections.SetPrimary(0);
    kept.doc.view.top_line = 0;
    const Selection in_band = kept.doc.selections.Ranges()[1];
    kept.pending_count = 5;
    RunCommands(kept, {"scroll_down"});
    EXPECT_EQ(kept.doc.view.top_line, Index{5});
    EXPECT_EQ(kept.doc.selections.Size(), std::size_t{2});
    EXPECT_EQ(line_of(kept, 0), Index{8});
    EXPECT_EQ(line_of(kept, 1), Index{20});
    EXPECT_EQ(kept.doc.selections.Ranges()[1].anchor, in_band.anchor);
    EXPECT_EQ(kept.doc.selections.Ranges()[1].head, in_band.head);

    // Goal columns come through the clamp, per cursor and over different
    // distances: both of these land on a band edge two columns wide, and both
    // find their own column again on the next long line.
    Editor short_edge;
    seed_columns(short_edge, {{0, 9}, {4, 1}});
    short_edge.pending_count = 119;
    RunCommands(short_edge, {"scroll_down"});
    EXPECT_EQ(short_edge.doc.selections.Size(), std::size_t{2});
    EXPECT_EQ(line_of(short_edge, 0), Index{122});
    EXPECT_EQ(line_of(short_edge, 1), Index{122});
    EXPECT_EQ(column_of(short_edge, 0), Index{1});
    EXPECT_EQ(column_of(short_edge, 1), Index{2});
    EXPECT_EQ(short_edge.doc.selections.Ranges()[0].goal_column, Index{1});
    EXPECT_EQ(short_edge.doc.selections.Ranges()[1].goal_column, Index{9});
    RunCommands(short_edge, {"move_line_down"});
    EXPECT_EQ(column_of(short_edge, 0), Index{1});
    EXPECT_EQ(column_of(short_edge, 1), Index{9});

    // The set shrinks for one reason only: two cursors on the same grapheme
    // are one cursor. Same column, three lines, one band edge to share.
    Editor stack;
    seed_columns(stack, {{0, 5}, {4, 5}, {9, 5}});
    stack.pending_count = 120;
    RunCommands(stack, {"scroll_down"});
    EXPECT_EQ(stack.doc.selections.Size(), std::size_t{1});
    EXPECT_EQ(line_of(stack, 0), Index{123});
    EXPECT_EQ(column_of(stack, 0), Index{5});

    // The count is not capped to the viewport, so the landing has to cost the
    // same whether it is one line away or five thousand, on a file big enough
    // that walking it a line at a time would show.
    std::string huge;
    for (int i = 0; i < 20000; ++i) huge += (((i % 7) == 3) ? "ab\n" : "abcdefghij\n");
    Editor far;
    ResetToOriginal(far.doc.table, huge);
    far.doc.view.rows = 20;
    far.doc.view.columns = 40;
    far.doc.view.scrolloff = 3;
    std::vector<Selection> spread;
    for (const Index line : {Index{0}, Index{4}, Index{9}}) {
      const Index pos = LineStart(far.doc.table, line) + 5;
      spread.push_back(MinWidth1(far.doc.table, Selection{pos, pos, -1}));
    }
    far.doc.selections.Replace(far.doc.table, std::move(spread));
    far.pending_count = 5000;
    const auto t_scroll = std::chrono::steady_clock::now();
    RunCommands(far, {"scroll_down"});
    EXPECT_TRUE((std::chrono::steady_clock::now() - t_scroll) < std::chrono::seconds{1});
    EXPECT_EQ(far.doc.view.top_line, Index{5000});
    EXPECT_EQ(far.doc.selections.Size(), std::size_t{1});
    EXPECT_EQ(line_of(far, 0), Index{5003});
    EXPECT_EQ(column_of(far, 0), Index{5});
  }

  {
    // Scrolling in insert mode is an unextended motion like any other: the
    // block cursors the clamp leaves behind collapse back to carets before the
    // binding ends, for every cursor it moved and not just the primary.
    Editor ins;
    std::string lines;
    for (int i = 0; i < 200; ++i) lines += "abcdefghij\n";
    ResetToOriginal(ins.doc.table, lines);
    ins.doc.view.rows = 20;
    ins.doc.view.columns = 40;
    ins.doc.view.scrolloff = 3;
    std::vector<Selection> carets;
    for (const auto& [line, column] : {std::pair<Index, Index>{0, 3}, {4, 7}}) {
      const Index pos = LineStart(ins.doc.table, line) + column;
      carets.push_back(Selection{pos, pos, -1});
    }
    ins.doc.selections.Replace(ins.doc.table, std::move(carets));
    ins.mode = Mode::kInsert;
    ins.collapse_insert_caret = false;
    ins.pending_count = 40;
    RunCommands(ins, {"scroll_down"});
    EXPECT_TRUE(ins.mode == Mode::kInsert);
    EXPECT_EQ(ins.doc.selections.Size(), std::size_t{2});
    for (const Selection& s : ins.doc.selections.Ranges()) {
      EXPECT_TRUE(s.IsEmpty());
      EXPECT_EQ(LineAt(ins.doc.table, s.head), Index{43});
    }
  }

  {
    Editor j;
    ResetToOriginal(j.doc.table, "aaa\nbbb\nccc\n");
    j.doc.view.rows = 10;
    j.doc.view.columns = 40;
    j.doc.selections.Set(Selection{0, 4, -1});
    RunCommands(j, {"join_selections"});
    EXPECT_EQ(AssembleDocContents(j.doc.table), std::string{"aaa bbb\nccc\n"});
  }
  {
    Editor j;
    ResetToOriginal(j.doc.table, "aaa\nbbb\nccc\nddd\n");
    j.doc.view.rows = 10;
    j.doc.view.columns = 40;
    j.doc.selections.Set(Selection{0, 12, -1});
    RunCommands(j, {"join_selections"});
    EXPECT_EQ(AssembleDocContents(j.doc.table), std::string{"aaa bbb ccc\nddd\n"});
  }
  {
    Editor j;
    ResetToOriginal(j.doc.table, "aaa\nbbb\nccc\n");
    j.doc.view.rows = 10;
    j.doc.view.columns = 40;
    j.doc.selections.Set(MinWidth1(j.doc.table, Selection{3, 3, -1}));
    RunCommands(j, {"join_selections"});
    EXPECT_EQ(AssembleDocContents(j.doc.table), std::string{"aaa bbb\nccc\n"});
  }

  {
    Editor tc;
    tc.doc.file = "probe.cpp";
    tc.doc.view.rows = 10;
    tc.doc.view.columns = 60;

    ResetToOriginal(tc.doc.table, "int a;\n  int b;\n\nint c;\n");
    tc.doc.selections.Set(Selection{0, DocLength(tc.doc.table), -1});
    RunCommands(tc, {"toggle_comments"});
    EXPECT_EQ(AssembleDocContents(tc.doc.table),
              std::string{"// int a;\n//   int b;\n\n// int c;\n"});

    tc.doc.selections.Set(Selection{0, DocLength(tc.doc.table), -1});
    RunCommands(tc, {"toggle_comments"});
    EXPECT_EQ(AssembleDocContents(tc.doc.table), std::string{"int a;\n  int b;\n\nint c;\n"});

    tc.doc.selections.Set(Selection{0, DocLength(tc.doc.table), -1});
    RunCommands(tc, {"toggle_comments"});
    RunCommands(tc, {"undo"});
    EXPECT_EQ(AssembleDocContents(tc.doc.table), std::string{"int a;\n  int b;\n\nint c;\n"});

    ResetToOriginal(tc.doc.table, "// done\ntodo\n");
    tc.doc.selections.Set(Selection{0, DocLength(tc.doc.table), -1});
    RunCommands(tc, {"toggle_comments"});
    EXPECT_EQ(AssembleDocContents(tc.doc.table), std::string{"// done\n// todo\n"});
    tc.doc.selections.Set(Selection{0, DocLength(tc.doc.table), -1});
    RunCommands(tc, {"toggle_comments"});
    EXPECT_EQ(AssembleDocContents(tc.doc.table), std::string{"done\ntodo\n"});

    ResetToOriginal(tc.doc.table, "  a;\n    b;\n");
    tc.doc.selections.Set(Selection{0, DocLength(tc.doc.table), -1});
    RunCommands(tc, {"toggle_comments"});
    EXPECT_EQ(AssembleDocContents(tc.doc.table), std::string{"  // a;\n  //   b;\n"});

    ResetToOriginal(tc.doc.table, "abc;\n");
    tc.doc.selections.Set(MinWidth1(tc.doc.table, Selection{1, 1, -1}));
    RunCommands(tc, {"toggle_comments"});
    EXPECT_EQ(ReadDocRange(tc.doc.table, tc.doc.selections.Primary().Range()), std::string{"b"});
  }
  {
    Editor tc;
    tc.doc.file = "probe.py";
    tc.doc.view.rows = 10;
    tc.doc.view.columns = 60;
    ResetToOriginal(tc.doc.table, "x = 1\n");
    tc.doc.selections.Set(MinWidth1(tc.doc.table, Selection{0, 0, -1}));
    RunCommands(tc, {"toggle_comments"});
    EXPECT_EQ(AssembleDocContents(tc.doc.table), std::string{"# x = 1\n"});
  }
  {
    Editor tc;
    tc.doc.file = "probe.diff";
    tc.doc.view.rows = 10;
    tc.doc.view.columns = 60;
    ResetToOriginal(tc.doc.table, "+added\n");
    tc.doc.selections.Set(MinWidth1(tc.doc.table, Selection{0, 0, -1}));
    tc.status.clear();
    RunCommands(tc, {"toggle_comments"});
    EXPECT_EQ(AssembleDocContents(tc.doc.table), std::string{"+added\n"});
    EXPECT_TRUE(tc.status.find("no line comment") != std::string::npos);
  }
}

void CommandPrompt() {
  TEST_CASE("command prompt");

  {
    Editor fresh;
    Document blank;
    ExpectOk(LoadDocument(std::filesystem::path{}, blank), "load a nameless buffer");
    fresh.doc = std::move(blank);
    EXPECT_TRUE(fresh.doc.file.empty());
    EXPECT_EQ(DocLength(fresh.doc.table), Index{0});
    EXPECT_EQ(LineCount(fresh.doc.table), Index{1});
    ApplyModeInvariants(fresh);
    EXPECT_EQ(fresh.doc.selections.Size(), size_t{1});

    ExpectOk(InsertAtCursors("scratch", fresh.doc.table, fresh.doc.selections), "type");
    fresh.doc.modified = true;

    RunTypableCommand(fresh, "w");
    EXPECT_TRUE(fresh.status.find("no file name") != std::string::npos);
    EXPECT_TRUE(fresh.doc.modified);

    const std::filesystem::path named =
        TempFixture("koi-new-buffer-test.txt");
    RemoveQuietly(named);
    RunTypableCommand(fresh, "w " + named.string());
    EXPECT_EQ(fresh.doc.file, named);
    EXPECT_FALSE(fresh.doc.modified);
    EXPECT_TRUE(std::filesystem::exists(named));
    {
      std::ifstream in(named, std::ios::binary);
      const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      EXPECT_EQ(got, std::string("scratch"));
    }
    RemoveQuietly(named);
  }

  Editor ed;
  ResetToOriginal(ed.doc.table, "one\ntwo\n");
  ed.doc.view.rows = 10;
  ed.doc.view.columns = 40;

  EXPECT_FALSE(ed.prompt_active);
  RunCommands(ed, {"command_mode"});
  EXPECT_TRUE(ed.prompt_active);
  EXPECT_EQ(ed.prompt_input, std::string(""));

  PromptInsert(ed, "w");
  PromptInsert(ed, "q");
  EXPECT_EQ(ed.prompt_input, std::string("wq"));
  EXPECT_EQ(ed.prompt_cursor, size_t{2});

  PromptMoveLeft(ed);
  EXPECT_EQ(ed.prompt_cursor, size_t{1});
  PromptInsert(ed, "X");
  EXPECT_EQ(ed.prompt_input, std::string("wXq"));
  PromptBackspace(ed);
  EXPECT_EQ(ed.prompt_input, std::string("wq"));
  EXPECT_EQ(ed.prompt_cursor, size_t{1});
  PromptDeleteForward(ed);
  EXPECT_EQ(ed.prompt_input, std::string("w"));
  PromptHome(ed);
  EXPECT_EQ(ed.prompt_cursor, size_t{0});
  PromptEnd(ed);
  EXPECT_EQ(ed.prompt_cursor, size_t{1});

  PromptCancel(ed);
  PromptOpen(ed);
  PromptInsert(ed, kFamily);
  PromptInsert(ed, "x");
  EXPECT_EQ(ed.prompt_cursor, ed.prompt_input.size());
  PromptBackspace(ed);
  PromptBackspace(ed);
  EXPECT_EQ(ed.prompt_input, std::string(""));
  EXPECT_EQ(ed.prompt_cursor, size_t{0});

  PromptOpen(ed);
  PromptInsert(ed, "q!");
  PromptCancel(ed);
  EXPECT_FALSE(ed.prompt_active);
  EXPECT_FALSE(ed.quit);

  PromptOpen(ed);
  PromptInsert(ed, "q");
  PromptSubmit(ed);
  EXPECT_FALSE(ed.prompt_active);
  EXPECT_TRUE(ed.quit);
  EXPECT_EQ(ed.prompt_history.size(), size_t{1});
  EXPECT_EQ(ed.prompt_history[0], std::string("q"));

  ed.quit = false;
  PromptOpen(ed);
  PromptInsert(ed, "   ");
  PromptSubmit(ed);
  EXPECT_FALSE(ed.quit);
  EXPECT_EQ(ed.prompt_history.size(), size_t{1});

  ed.quit = false;
  PromptOpen(ed);
  PromptInsert(ed, "  q  ");
  PromptSubmit(ed);
  EXPECT_TRUE(ed.quit);
  EXPECT_EQ(ed.prompt_history.back(), std::string("q"));

  ed.quit = false;
  ed.status.clear();
  PromptOpen(ed);
  PromptInsert(ed, "nonsense");
  PromptSubmit(ed);
  EXPECT_TRUE(ed.status.find("unknown command") != std::string::npos);

  PromptOpen(ed);
  PromptInsert(ed, "nonsense");
  PromptSubmit(ed);
  const size_t history_size = ed.prompt_history.size();
  PromptOpen(ed);
  PromptHistory(ed, true);
  EXPECT_EQ(ed.prompt_input, std::string("nonsense"));
  EXPECT_EQ(ed.prompt_cursor, ed.prompt_input.size());
  PromptHistory(ed, true);
  EXPECT_EQ(ed.prompt_input, std::string("q"));
  PromptHistory(ed, false);
  EXPECT_EQ(ed.prompt_input, std::string("nonsense"));
  PromptHistory(ed, false);
  EXPECT_EQ(ed.prompt_input, std::string(""));
  PromptCancel(ed);
  EXPECT_EQ(ed.prompt_history.size(), history_size);

  {
    Editor keys;
    ResetToOriginal(keys.doc.table, "");
    const KeyMaps maps = DefaultKeyMaps();
    std::vector<Key> pending;
    Key up{};
    Key down{};
    Key colon{};
    EXPECT_TRUE(ParseKey("up", up));
    EXPECT_TRUE(ParseKey("down", down));
    EXPECT_TRUE(ParseKey(":", colon));

    HandleKeyInput(keys, maps, colon, pending);
    EXPECT_TRUE(keys.prompt_active);
    EXPECT_EQ(PromptSigil(keys), std::string_view{":"});
    PromptInsert(keys, "buffers");
    PromptSubmit(keys);
    HandleKeyInput(keys, maps, colon, pending);
    PromptInsert(keys, "ls");
    PromptSubmit(keys);

    HandleKeyInput(keys, maps, colon, pending);
    EXPECT_TRUE(keys.prompt_input.empty());
    HandleKeyInput(keys, maps, up, pending);
    EXPECT_EQ(keys.prompt_input, std::string("ls"));
    HandleKeyInput(keys, maps, up, pending);
    EXPECT_EQ(keys.prompt_input, std::string("buffers"));
    HandleKeyInput(keys, maps, up, pending);
    EXPECT_EQ(keys.prompt_input, std::string("buffers"));
    HandleKeyInput(keys, maps, down, pending);
    EXPECT_EQ(keys.prompt_input, std::string("ls"));
    HandleKeyInput(keys, maps, down, pending);
    EXPECT_EQ(keys.prompt_input, std::string(""));
    EXPECT_EQ(keys.prompt_cursor, std::size_t{0});
    EXPECT_EQ(LineAt(keys.doc.table, keys.doc.selections.Primary().head), Index{0});
    PromptCancel(keys);
  }

  Editor closed;
  ResetToOriginal(closed.doc.table, "");
  PromptInsert(closed, "x");
  PromptBackspace(closed);
  PromptDeleteForward(closed);
  PromptMoveLeft(closed);
  PromptMoveRight(closed);
  PromptHistory(closed, true);
  PromptSubmit(closed);
  EXPECT_FALSE(closed.prompt_active);
  EXPECT_EQ(closed.prompt_input, std::string(""));
}

void TypableCommandsLeaveNormalModeUsable() {
  TEST_CASE("commands: a `:` command leaves a selection `d` can act on");
  const Scratch scratch{"koi-typable-invariants"};
  const std::filesystem::path file = scratch.Write("t.txt", "alpha\nbravo\ncharlie\ndelta\n");

  const auto usable = [](const Editor& ed) {
    if (ed.mode == Mode::kInsert) return true;
    const Index length = DocLength(ed.doc.table);
    for (const Selection& s : ed.doc.selections.Ranges()) {
      if (s.IsEmpty() && (s.From() < length)) return false;
    }
    return true;
  };

  Editor ed;
  ed.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(ed, file.string()));
  RunCommands(ed, {"vsplit"});
  RunCommands(ed, {"insert_mode"});
  RunCommands(ed, {"jump_view_next"});
  RunCommands(ed, {"normal_mode"});
  EXPECT_TRUE(usable(ed));

  RunTypableCommand(ed, "wclose");
  EXPECT_TRUE(usable(ed));
  EXPECT_EQ(EditorInvariants(ed), std::string{});

  const std::string before = ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  RunCommands(ed, {"delete_selection"});
  const std::string after = ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  EXPECT_TRUE(after.size() < before.size());
}

void BracketedPaste() {
  TEST_CASE("bracketed paste: text is data, in one undo step");

  const auto text_of = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.mode = Mode::kInsert;
    ApplyPaste(ed, "one jk two");
    EXPECT_EQ(text_of(ed), std::string("one jk twoalpha\n"));
    EXPECT_TRUE(ed.mode == Mode::kInsert);
    EXPECT_TRUE(ed.doc.modified);
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.mode = Mode::kInsert;
    ApplyPaste(ed, "one\ntwo\nthree\nfour\n");
    EXPECT_EQ(text_of(ed), std::string("one\ntwo\nthree\nfour\nalpha\n"));
    RunCommands(ed, {"undo"});
    EXPECT_EQ(text_of(ed), std::string("alpha\n"));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.mode = Mode::kInsert;
    ApplyPaste(ed, "one\rtwo\r\nthree\nfour");
    EXPECT_EQ(text_of(ed), std::string("one\ntwo\nthree\nfour"));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.mode = Mode::kNormal;
    ApplyPaste(ed, "new ");
    EXPECT_EQ(text_of(ed), std::string("new alpha\n"));
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{4});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "a\nb\nc\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"copy_selection_on_next_line", "copy_selection_on_next_line"});
    EXPECT_EQ(std::ssize(ed.doc.selections.Ranges()), Index{3});
    ed.mode = Mode::kInsert;
    ApplyPaste(ed, ">");
    EXPECT_EQ(text_of(ed), std::string(">a\n>b\n>c\n"));
    RunCommands(ed, {"undo"});
    EXPECT_EQ(text_of(ed), std::string("a\nb\nc\n"));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"command_mode"});
    EXPECT_TRUE(ed.prompt_active);
    ApplyPaste(ed, "open a.txt\nsecond");
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_EQ(ed.prompt_input, std::string("open a.txt second"));
    EXPECT_EQ(text_of(ed), std::string("alpha\n"));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ApplyPaste(ed, "");
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_EQ(text_of(ed), std::string("alpha\n"));
  }
}

void InsertMotionCollapse() {
  TEST_CASE("insert-mode motions: collapse at the end of the binding, not mid-way");

  const auto painted = [](const Editor& ed) {
    std::string out = ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
    for (const Selection& s : ed.doc.selections.Ranges()) {
      for (Index i = s.From(); (i < s.To()) && (i < std::ssize(out)); ++i) out[i] = '#';
    }
    return out;
  };
  const auto fresh = [](Editor& ed) {
    ResetToOriginal(ed.doc.table, "Helix has this feature.\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.mode = Mode::kInsert;
    ed.collapse_insert_caret = false;
  };

  {
    Editor ed;
    fresh(ed);
    RunCommands(ed, {"move_next_word_start"});
    EXPECT_EQ(painted(ed), std::string("Helix has this feature.\n"));
    EXPECT_TRUE(ed.mode == Mode::kInsert);
  }

  {
    Editor ed;
    fresh(ed);
    RunCommands(ed, {"move_next_word_start", "normal_mode"});
    EXPECT_EQ(painted(ed), std::string("######has this feature.\n"));
    EXPECT_TRUE(ed.mode == Mode::kNormal);
  }

  {
    Editor ed;
    fresh(ed);
    RunCommands(ed, {"move_next_word_start", "normal_mode"});
    RunCommands(ed, {"extend_next_word_start"});
    EXPECT_EQ(painted(ed), std::string("##########this feature.\n"));
  }

  {
    Editor ed;
    fresh(ed);
    RunCommands(ed, {"move_next_word_start", "collapse_selection", "normal_mode"});
    EXPECT_EQ(painted(ed), std::string("Helix#has this feature.\n"));
  }
}

void NewBuffer() {
  TEST_CASE(":new");

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "old contents\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {":new"});
    EXPECT_EQ(DocLength(ed.doc.table), Index{0});
    EXPECT_TRUE(ed.doc.file.empty());
    EXPECT_FALSE(ed.doc.modified);
  }

  {
    Scratch scratch{"koi-new-test"};
    const std::filesystem::path wanted = scratch.dir / "draft.md";
    Editor ed;
    ResetToOriginal(ed.doc.table, "");
    ed.doc.selections.Set(Selection{0, 0, -1});
    RunCommands(ed, {":new " + wanted.string()});
    EXPECT_EQ(ed.doc.file.string(), wanted.string());
    EXPECT_FALSE(std::filesystem::exists(wanted));
  }

  {

    Editor ed;
    ResetToOriginal(ed.doc.table, "typed\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.doc.modified = true;
    RunCommands(ed, {":new"});
    EXPECT_EQ(BufferCount(ed), std::size_t{2});
    EXPECT_EQ(DocLength(ed.doc.table), Index{0});

    RunCommands(ed, {":bp"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}), std::string("typed\n"));
    EXPECT_TRUE(ed.doc.modified);
  }
}

void IndentDetection() {
  TEST_CASE("detect indentation");

  const auto detect = [](std::string_view text, bool want_spaces, Index want_width) {
    Document doc;
    doc.insert_spaces = true;
    doc.tab_width = 4;
    ResetToOriginal(doc.table, std::string{text});
    DetectIndentation(doc);
    ++common::g_test_checks;
    const bool width_ok = (want_width == 0) || (doc.tab_width == want_width);
    if ((doc.insert_spaces == want_spaces) && width_ok) return;
    ++common::g_test_failures;
    std::cerr << "FAIL [" << common::g_test_case << "] detected "
              << (doc.insert_spaces ? "spaces" : "tabs") << '/' << doc.tab_width << ", wanted "
              << (want_spaces ? "spaces" : "tabs") << '/' << want_width << "\n      for: \""
              << text << '"' << std::endl;
  };

  detect("a\n    b\n    c\n        d\n", true, 4);
  detect("a\n  b\n  c\n    d\n      e\n", true, 2);
  detect("a\n   b\n      c\n", true, 3);
  detect("a\n\tb\n\t\tc\n", false, 0);

  detect("fn(a,\n\tb,\n\t  c,\n\t  d)\n", false, 0);

  detect("a\n  b\n    c\n  d\n    e\n", true, 2);

  detect("/**\n * one\n * two\n * three\n */\nfn() {\n    body\n}\n", true, 4);

  detect("a\n b\n c\n", true, 1);
  detect("a\n b\n c\n  d\n", true, 2);

  {
    Document doc;
    doc.insert_spaces = false;
    doc.tab_width = 7;
    ResetToOriginal(doc.table, "one\ntwo\nthree\n");
    DetectIndentation(doc);
    EXPECT_EQ(doc.insert_spaces, false);
    EXPECT_EQ(doc.tab_width, Index{7});
  }
  {
    Document doc;
    doc.insert_spaces = true;
    doc.tab_width = 4;
    ResetToOriginal(doc.table, "");
    DetectIndentation(doc);
    EXPECT_EQ(doc.tab_width, Index{4});
  }

  detect("a\r\n  b\r\n    c\r\n", true, 2);

  detect("fn() {\n  a\n\n  b\n\n  c\n}\n", true, 2);

  {
    std::string big = "class Foo:\n";
    for (int i = 0; i < 200; ++i) {
      big += "    def m" + std::to_string(i) + "(self):\n";
      big += "        if x:\n";
      big += "            return 1\n";
      big += "        return 0\n\n";
    }
    detect(big, true, 4);
  }

  {
    std::string huge = "a\n" + std::string(200 * 1024, 'x') + "\n";
    for (int i = 0; i < 20; ++i) huge += "  indented\n";
    detect(huge, true, 2);
  }
}

void AutoIndentAndPairs() {
  TEST_CASE("auto-indent and auto-pairs");

  const auto press_ret = [&](std::string_view before, Index at, bool with_syntax) {
    Editor ed;
    if (with_syntax) {
      std::string syntax_error;
      ed.doc.syntax = OpenSyntax(std::filesystem::path{"sample.cpp"}, syntax_error);
    }
    ResetToOriginal(ed.doc.table, std::string{before});
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
    RunCommands(ed, {"insert_newline"});
    return AssembleDocContents(ed.doc.table);
  };

  EXPECT_EQ(press_ret("alpha\n", 0, false), std::string("\nalpha\n"));
  EXPECT_EQ(press_ret("alpha\n", 3, false), std::string("alp\nha\n"));

  EXPECT_EQ(press_ret("    alpha\n", 9, false), std::string("    alpha\n    \n"));
  EXPECT_EQ(press_ret("\t\talpha\n", 7, false), std::string("\t\talpha\n\t\t\n"));

  EXPECT_EQ(press_ret("fn() {\n", 6, false), std::string("fn() {\n    \n"));
  EXPECT_EQ(press_ret("fn() {}\n", 7, false), std::string("fn() {}\n\n"));
  EXPECT_EQ(press_ret("fn(a, [\n", 7, false), std::string("fn(a, [\n    \n"));
  EXPECT_EQ(press_ret("  if (x) {\n", 10, false), std::string("  if (x) {\n      \n"));

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "  fn() {}\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{8, 8, -1}));
    RunCommands(ed, {"insert_newline"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("  fn() {\n      \n  }\n"));
    EXPECT_EQ(Cur(ed), Index{15});
  }

  {
    std::string syntax_error;
    const bool have_cpp = OpenSyntax(std::filesystem::path{"sample.cpp"}, syntax_error) != nullptr;
    EXPECT_TRUE(have_cpp);
    if (have_cpp) {
      EXPECT_EQ(press_ret("  puts(\"{\");\n", 12, true), std::string("  puts(\"{\");\n  \n"));
      EXPECT_EQ(press_ret("  // opens with {\n", 17, true), std::string("  // opens with {\n  \n"));
      EXPECT_EQ(press_ret("  if (c == '}') {\n", 17, true),
                std::string("  if (c == '}') {\n      \n"));

      EXPECT_EQ(press_ret("  puts(\"{\");\n", 12, false), std::string("  puts(\"{\");\n      \n"));
      EXPECT_EQ(press_ret("  // opens with {\n", 17, false),
                std::string("  // opens with {\n      \n"));
    }
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "  a {\n    b {\n");
    std::vector<Selection> both{Selection{5, 5, -1}, Selection{13, 13, -1}};
    ed.doc.selections.Replace(ed.doc.table, both);
    RunCommands(ed, {"insert_newline"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("  a {\n      \n    b {\n        \n"));
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{3, 3, -1}));
    RunCommands(ed, {"delete_char_backward"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("alha\n"));
  }
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    ApplyModeInvariants(ed);
    RunCommands(ed, {"delete_char_backward"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("alpha\n"));
  }
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha beta\n");
    ed.doc.selections.Set(Selection{0, 5, -1});
    RunCommands(ed, {"delete_char_backward"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string(" beta\n"));
  }
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(Selection{3, 3, -1});
    ed.mode = Mode::kInsert;
    RunCommands(ed, {"delete_char_backward"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("alha\n"));
  }
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "aé b\n");
    ed.doc.selections.Set(Selection{3, 3, -1});
    ed.mode = Mode::kInsert;
    RunCommands(ed, {"delete_char_backward"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("a b\n"));
  }

  for (const Index gap : {Index{16}, Index{9000}, Index{200000}}) {
    Editor ed;
    const std::string body(static_cast<size_t>(gap), 'x');
    ResetToOriginal(ed.doc.table, "a{" + body + "}b");
    const Index inside = 2 + (gap / 2);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{inside, inside, -1}));
    RunCommands(ed, {"select_textobject_inner"});
    Key brace;
    EXPECT_TRUE(ParseKey("{", brace));
    std::vector<Key> pending;
    HandleKeyInput(ed, DefaultKeyMaps(), brace, pending);
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{2});
    EXPECT_EQ(ed.doc.selections.Primary().To(), 2 + gap);
  }

  const auto type = [](std::string_view before, Index at, std::string_view keys,
                       bool auto_pairs = true) {
    Editor ed;
    ed.settings.auto_pairs = auto_pairs;
    ResetToOriginal(ed.doc.table, std::string{before});
    ed.doc.selections.Set(Selection{at, at, -1});
    ed.mode = Mode::kInsert;
    std::vector<Key> pending;
    for (const char c : keys) {
      Key k;
      EXPECT_TRUE(ParseKey(std::string_view{&c, 1}, k));
      pending.push_back(k);
    }
    FlushPendingAsText(ed, pending);
    return std::pair{AssembleDocContents(ed.doc.table), ed.doc.selections.Primary().head};
  };

  EXPECT_EQ(type("\n", 0, "(").first, std::string("()\n"));
  EXPECT_EQ(type("\n", 0, "(").second, Index{1});
  EXPECT_EQ(type("\n", 0, "[").first, std::string("[]\n"));
  EXPECT_EQ(type("\n", 0, "{").first, std::string("{}\n"));
  EXPECT_EQ(type("\n", 0, "\"").first, std::string("\"\"\n"));

  EXPECT_EQ(type("\n", 0, "{{").first, std::string("{{}}\n"));
  EXPECT_EQ(type("\n", 0, "([{").first, std::string("([{}])\n"));

  EXPECT_EQ(type("()\n", 1, ")").first, std::string("()\n"));
  EXPECT_EQ(type("()\n", 1, ")").second, Index{2});
  EXPECT_EQ(type("\n", 0, ")").first, std::string(")\n"));
  EXPECT_EQ(type("\n", 0, "()").first, std::string("()\n"));
  EXPECT_EQ(type("\n", 0, "()").second, Index{2});

  EXPECT_EQ(type("()\n", 1, "j)").first, std::string("(j)\n"));
  EXPECT_EQ(type("()\n", 1, "j)k").first, std::string("(j)k\n"));

  EXPECT_EQ(type("don\n", 3, "'").first, std::string("don'\n"));
  EXPECT_EQ(type("x = \n", 4, "'").first, std::string("x = ''\n"));

  EXPECT_EQ(type("foo\n", 0, "(").first, std::string("(foo\n"));
  EXPECT_EQ(type(")\n", 0, "(").first, std::string("())\n"));
  EXPECT_EQ(type(" x\n", 0, "(").first, std::string("() x\n"));

  EXPECT_EQ(type("\n", 0, "hello").first, std::string("hello\n"));
  EXPECT_EQ(type("\n", 0, "({", false).first, std::string("({\n"));
  EXPECT_EQ(type("()\n", 1, ")", false).first, std::string("())\n"));

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "foo\nbar\n");
    std::vector<Selection> both{Selection{0, 0, -1}, Selection{7, 7, -1}};
    ed.doc.selections.Replace(ed.doc.table, both);
    ed.mode = Mode::kInsert;
    std::vector<Key> pending;
    Key k;
    EXPECT_TRUE(ParseKey("(", k));
    pending.push_back(k);
    FlushPendingAsText(ed, pending);
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("(foo\nbar()\n"));
  }
}

// -- round 5 --------------------------------------------------------------
//
// One case per finding the round-5 audit fixed. Each was checked by reverting
// its fix and confirming the case goes red -- noted per test where the naive
// version of the assertion does *not*, which is the interesting part.

void InsertModePasteLandsAtTheEndOfThePaste() {
  TEST_CASE("paste: the caret lands after the pasted bytes, not one grapheme short");

  const auto text_of = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };

  // Insert mode holds an empty caret and CursorOf reads a forward selection as
  // the grapheme *behind* its head, so a paste that left a span drew the caret
  // one short and typed the next character inside what it had just pasted.
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"insert_mode"});
    ed.registers.assign(1, "abc");
    RunCommands(ed, {"paste_before"});
    ApplyModeInvariants(ed);
    EXPECT_EQ(text_of(ed), std::string("abcalpha\n"));
    EXPECT_EQ(ed.doc.selections.Primary().head, Index{3});
    EXPECT_TRUE(ed.doc.selections.Primary().IsEmpty());
    EXPECT_EQ(CursorOf(ed.doc.table, ed.doc.selections.Primary()), Index{3});
    // The assertion that actually mattered to the user: the next keystroke
    // appends instead of splitting the paste. A literal tab, so the check does
    // not quietly depend on what insert_spaces defaults to.
    ed.doc.insert_spaces = false;
    RunCommands(ed, {"insert_tab"});
    EXPECT_EQ(text_of(ed), std::string("abc\talpha\n"));
  }

  // Multi-line, and multi-cursor: every caret after its own paste.
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "a\nb\nc\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"copy_selection_on_next_line", "copy_selection_on_next_line", "insert_mode"});
    EXPECT_EQ(std::ssize(ed.doc.selections.Ranges()), Index{3});
    ed.registers.assign(1, "Z");
    RunCommands(ed, {"paste_before"});
    ApplyModeInvariants(ed);
    EXPECT_EQ(text_of(ed), std::string("Za\nZb\nZc\n"));
    for (const Selection& s : ed.doc.selections.Ranges()) EXPECT_TRUE(s.IsEmpty());
    ed.doc.insert_spaces = false;
    RunCommands(ed, {"insert_tab"});
    EXPECT_EQ(text_of(ed), std::string("Z\ta\nZ\tb\nZ\tc\n"));
  }

  // Normal mode is the control: there the span *is* the point of `p`, and the
  // fix must not have touched it.
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.registers.assign(1, "new ");
    RunCommands(ed, {"paste_before"});
    ApplyModeInvariants(ed);
    EXPECT_EQ(text_of(ed), std::string("new alpha\n"));
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{4});
  }

  // The keybinding the report was written against, when a clipboard tool is
  // available to stage a value with. Skipped rather than flaky where none is.
  if (HasClipboard() && ClipboardCopy("abc")) {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"insert_mode", "paste_clipboard_before"});
    ApplyModeInvariants(ed);
    if (text_of(ed) == std::string("abcalpha\n")) {  // i.e. the clipboard held ours
      EXPECT_EQ(CursorOf(ed.doc.table, ed.doc.selections.Primary()), Index{3});
    }
  }
}

void MultiCursorCopyAndPasteCarryEverySelection() {
  TEST_CASE("clipboard: N cursors copy N pieces, and paste gets one each");

  const auto text_of = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  // One selection per non-empty line of `doc`.
  const auto lines_selected = [&](Editor& ed, std::string_view doc) {
    ResetToOriginal(ed.doc.table, std::string{doc});
    const std::string text = text_of(ed);
    std::vector<Selection> sel;
    for (Index at = 0; at < std::ssize(text);) {
      Index end = at;
      while ((end < std::ssize(text)) && (text[end] != '\n')) ++end;
      if (end > at) sel.push_back(Selection{at, end, -1});
      at = end + 1;
    }
    ed.doc.selections.Replace(ed.doc.table, std::move(sel));
  };

  // The copy side. YankToClipboard read Primary() and nothing else, so three
  // cursors put one selection on the clipboard and dropped the other two.
  {
    Editor ed;
    lines_selected(ed, "alpha\nbeta\ngamma\n");
    RunCommands(ed, {"yank_to_clipboard"});
    EXPECT_EQ(ed.registers.size(), std::size_t{3});
    if (ed.registers.size() == 3) {
      EXPECT_EQ(ed.registers[0], std::string("alpha"));
      EXPECT_EQ(ed.registers[2], std::string("gamma"));
    }
    // The count only reaches the status line on the branch where the clipboard
    // really took the text. Where the copy cannot land -- a nix build sandbox,
    // whose PATH is the derivation's inputs and nothing else, or a headless CI
    // with xclip installed but no X server to reach -- the yank still fills the
    // registers and says so instead. Both are the command working; asserting
    // only the first passed on a desktop and failed in the build.
    EXPECT_TRUE(ed.status.find(ed.clipboard_parts.empty() ? "internal register" : "3 selections") !=
                std::string::npos);

    // The primary-selection command is still there, and still means one.
    RunCommands(ed, {"yank_main_selection_to_clipboard"});
    EXPECT_EQ(ed.registers.size(), std::size_t{1});
  }

  // :set-multicursor-paste, which only governs text koi did not copy.
  {
    Editor ed;
    EXPECT_TRUE(!ed.settings.multi_cursor_paste_spread);  // the shipped default is "full"
    RunTypableCommand(ed, "set-multicursor-paste spread");
    EXPECT_TRUE(ed.settings.multi_cursor_paste_spread);
    RunTypableCommand(ed, "set-multicursor-paste full");
    EXPECT_TRUE(!ed.settings.multi_cursor_paste_spread);
    RunTypableCommand(ed, "set-multicursor-paste");
    EXPECT_TRUE(ed.status.find("full") != std::string::npos);
    RunTypableCommand(ed, "set-multicursor-paste sideways");
    EXPECT_EQ(static_cast<int>(ed.status.level()), static_cast<int>(StatusLevel::kWarning));
    EXPECT_TRUE(!ed.settings.multi_cursor_paste_spread);
  }

  // And the same switch from the config file.
  {
    const Scratch scratch{"koi-multicursor-paste-config"};
    KeyMaps maps = DefaultKeyMaps();
    Settings settings;
    std::vector<std::string> errors;
    std::ignore = LoadKeyMapConfig(
        scratch.Write("good.toml", "[editor]\nmulti-cursor-paste = \"spread\"\n"), maps, settings,
        errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(settings.multi_cursor_paste_spread);

    std::ignore = LoadKeyMapConfig(
        scratch.Write("bad.toml", "[editor]\nmulti-cursor-paste = \"sideways\"\n"), maps, settings,
        errors);
    EXPECT_TRUE(!errors.empty());
  }

  // The paste side, as a decision about pieces -- no clipboard tool needed, so
  // this runs everywhere rather than only where wl-copy or xclip exists.
  {
    // So a wrong size reports rather than running off the end.
    const auto at = [](const std::vector<std::string>& v, std::size_t i) {
      return (i < v.size()) ? v[i] : std::string{"<no such piece>"};
    };
    const std::vector<std::string> mine{"alpha", "beta", "gamma"};
    const std::string joined = "alpha\nbeta\ngamma";

    // Our own copy, back into the same number of cursors: one piece each.
    const std::vector<std::string> same = ClipboardPieces(joined, mine, 3, true);
    EXPECT_EQ(same.size(), std::size_t{3});
    EXPECT_EQ(at(same, 1), std::string("beta"));

    // A different number of cursors has no piece-to-cursor mapping, so every
    // cursor takes the whole text -- which is what a single cursor wants too.
    EXPECT_EQ(ClipboardPieces(joined, mine, 2, true).size(), std::size_t{1});
    EXPECT_EQ(ClipboardPieces(joined, mine, 1, true).front(), joined);

    // Text we did not write divides on its line breaks when the count matches,
    // which is VS Code's editor.multiCursorPaste = "spread".
    EXPECT_EQ(ClipboardPieces("x\ny\nz", {}, 3, true).size(), std::size_t{3});
    EXPECT_EQ(at(ClipboardPieces("x\r\ny\r\nz", {}, 3, true), 2), std::string("z"));
    // A trailing newline ends the last line; it does not start a fourth.
    EXPECT_EQ(ClipboardPieces("x\ny\nz\n", {}, 3, true).size(), std::size_t{3});
    // Counts that do not match, and "full", both paste the whole text.
    EXPECT_EQ(ClipboardPieces("x\ny", {}, 3, true).size(), std::size_t{1});
    EXPECT_EQ(ClipboardPieces("x\ny\nz", {}, 3, false).front(), std::string("x\ny\nz"));
  }

  // The mismatched counts, in both directions and from both sources. Every one
  // of these pastes the whole text at every cursor, which is the only answer
  // that loses nothing: dropping lines to fit, or padding with blanks, would
  // both quietly change what was copied.
  {
    const auto whole = [](const std::vector<std::string>& v, std::string_view text) {
      return (v.size() == 1) && (v.front() == text);
    };
    const std::vector<std::string> three{"a", "b", "c"};

    // Fewer lines than cursors, and more.
    EXPECT_TRUE(whole(ClipboardPieces("x\ny", {}, 3, true), "x\ny"));
    EXPECT_TRUE(whole(ClipboardPieces("v\nw\nx\ny\nz", {}, 3, true), "v\nw\nx\ny\nz"));
    // One line, many cursors: the ordinary "paste this word at each" case.
    EXPECT_TRUE(whole(ClipboardPieces("x", {}, 4, true), "x"));
    // Our own copy, into fewer cursors than it came from, and into more.
    EXPECT_TRUE(whole(ClipboardPieces("a\nb\nc", three, 2, true), "a\nb\nc"));
    EXPECT_TRUE(whole(ClipboardPieces("a\nb\nc", three, 4, true), "a\nb\nc"));
    // With spread off, our own copy still comes back in pieces -- the setting
    // governs other people's text, not koi's own round trip.
    EXPECT_EQ(ClipboardPieces("a\nb\nc", three, 3, false).size(), std::size_t{3});
    // A remembered copy whose bytes are no longer on the clipboard is not ours,
    // so with spread off it goes in whole even though the count still matches.
    EXPECT_TRUE(whole(ClipboardPieces("a\nb\nZ", three, 3, false), "a\nb\nZ"));
    // Degenerate texts, which must not divide by surprise.
    EXPECT_TRUE(whole(ClipboardPieces("", {}, 3, true), ""));
    EXPECT_TRUE(whole(ClipboardPieces("\n", {}, 2, true), "\n"));
    EXPECT_EQ(ClipboardPieces("\n\n", {}, 2, true).size(), std::size_t{2});  // two blank lines
    // No cursors at all cannot be spread over.
    EXPECT_TRUE(whole(ClipboardPieces("x\ny\nz", three, 0, true), "x\ny\nz"));
  }

  // An empty selection is a real piece, and has to survive the round trip. The
  // join used to drop the separator whenever what it had so far was empty, so
  // a leading empty selection made {"", "b"} come back as one line.
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "ab\n");
    ed.doc.selections.Replace(ed.doc.table, {Selection{0, 0, -1}, Selection{1, 2, -1}});
    RunCommands(ed, {"yank_to_clipboard"});
    EXPECT_EQ(ed.registers.size(), std::size_t{2});
    if (HasClipboard()) {
      std::string staged;
      if (ClipboardPaste(staged)) {
        // "" then "b" is a blank line then b, not just b.
        EXPECT_EQ(staged, std::string("\nb"));
        EXPECT_EQ(ClipboardPieces(staged, ed.clipboard_parts, 2, false).size(), std::size_t{2});
      }
    }
  }

  // And the mismatch is reported rather than looking like a broken setting.
  if (HasClipboard() && ClipboardCopy("one\ntwo")) {
    Editor ed;
    ed.settings.multi_cursor_paste_spread = true;
    lines_selected(ed, "1\n2\n3\n");
    RunCommands(ed, {"paste_clipboard_before"});
    if (text_of(ed).find("one\ntwo1") != std::string::npos) {  // i.e. the clipboard held ours
      EXPECT_TRUE(ed.status.find("2 lines, 3 cursors") != std::string::npos);
    }
  }

  // End to end, when a clipboard tool is there to stage a value with. Skipped
  // rather than flaky where none is.
  if (HasClipboard()) {
    Editor ed;
    lines_selected(ed, "alpha\nbeta\ngamma\n");
    RunCommands(ed, {"yank_to_clipboard"});
    std::string staged;
    if (ClipboardPaste(staged) && (staged == "alpha\nbeta\ngamma")) {
      lines_selected(ed, "1\n2\n3\n");
      RunCommands(ed, {"paste_clipboard_before"});
      EXPECT_EQ(text_of(ed), std::string("alpha1\nbeta2\ngamma3\n"));

      // One cursor takes the join, so nothing that was copied is lost.
      ResetToOriginal(ed.doc.table, "X\n");
      ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
      RunCommands(ed, {"paste_clipboard_before"});
      EXPECT_EQ(text_of(ed), std::string("alpha\nbeta\ngammaX\n"));
    }
  }
}

// -- round 7 --------------------------------------------------------------

void OpenLineMovesTheCaretsItAlreadyPlaced() {
  TEST_CASE("open_below/open_above: every caret lands on its own new line");

  const auto text_of = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  // An empty caret at the start of each of the three lines.
  const auto caret_per_line = [](Editor& ed) {
    ResetToOriginal(ed.doc.table, "aaa\nbbb\nccc\n");
    std::vector<Selection> sel;
    for (Index line = 0; line < 3; ++line) {
      const Index at = LineStart(ed.doc.table, line);
      sel.push_back(Selection{at, at, -1});
    }
    ed.doc.selections.Replace(ed.doc.table, std::move(sel));
  };
  const auto heads = [](const Editor& ed) {
    std::vector<Index> out;
    for (const Selection& s : ed.doc.selections.Ranges()) out.push_back(s.head);
    return out;
  };
  const auto type = [](Editor& ed, std::string_view key) {
    std::vector<Key> pending{K(key)};
    FlushPendingAsText(ed, pending);
  };

  // OpenLine gathered its new carets in a second vector and so never mapped
  // them: the loop runs back to front, and every "\n" it inserts is *below*
  // every caret already parked, which moves each of them on by a byte. The
  // top caret came out two short and the middle one short, and since the
  // command ends in insert mode the next keystroke edited the line above --
  // "aaa\nX\nbbbX\n\nccXc\n\n" for the case below.
  {
    Editor ed;
    caret_per_line(ed);
    RunCommands(ed, {"open_below"});
    EXPECT_TRUE(ed.mode == Mode::kInsert);
    EXPECT_EQ(text_of(ed), std::string("aaa\n\nbbb\n\nccc\n\n"));
    EXPECT_TRUE(heads(ed) == std::vector<Index>({4, 9, 14}));
    type(ed, "X");
    EXPECT_EQ(text_of(ed), std::string("aaa\nX\nbbb\nX\nccc\nX\n"));
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  {
    Editor ed;
    caret_per_line(ed);
    RunCommands(ed, {"open_above"});
    EXPECT_TRUE(ed.mode == Mode::kInsert);
    EXPECT_EQ(text_of(ed), std::string("\naaa\n\nbbb\n\nccc\n"));
    EXPECT_TRUE(heads(ed) == std::vector<Index>({0, 5, 10}));
    type(ed, "X");
    EXPECT_EQ(text_of(ed), std::string("X\naaa\nX\nbbb\nX\nccc\n"));
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  // One caret is the case that always worked; it must still.
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "aaa\nbbb\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{5, 5, -1}));
    RunCommands(ed, {"open_below"});
    type(ed, "X");
    EXPECT_EQ(text_of(ed), std::string("aaa\nbbb\nX\n"));
  }
}

void NoOpEditsLeaveTheBufferClean() {
  namespace fs = std::filesystem;

  const auto text_of = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  const auto load = [](Editor& ed, std::string_view text, Index at) {
    ResetToOriginal(ed.doc.table, std::string{text});
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
    ed.doc.modified = false;
  };
  // Every no-op shape: the command succeeds, nothing lands, the buffer must
  // come out exactly as clean as it went in.
  const auto stays_clean = [&text_of](Editor& ed, std::string_view command) {
    const std::string before = text_of(ed);
    const Index revision_before = ed.doc.table.revision;
    RunCommands(ed, {std::string{command}});
    EXPECT_EQ(text_of(ed), before);
    EXPECT_EQ(ed.doc.table.revision, revision_before);
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_FALSE(CanUndo(ed.doc.table));
    EXPECT_EQ(UndoDepth(ed.doc.table), Index{0});
    EXPECT_TRUE(UnsavedBuffers(ed).empty());
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  };

  TEST_CASE("a delete that deletes nothing does not mark the buffer modified");
  {
    // Edited() ran the success path unconditionally, and the success path set
    // modified. EditThroughCursors returns Success without touching the
    // document when no cursor described a change, so a backspace at offset 0
    // marked the buffer dirty with no revision behind it -- and UndoOrRedo,
    // the only code that recomputes modified from the undo serial, returns
    // early when there is nothing to undo. The flag never came off again.
    Editor ed;
    load(ed, "hello\n", 0);
    stays_clean(ed, "delete_char_backward");

    load(ed, "hello\n", DocLength(ed.doc.table));
    stays_clean(ed, "delete_char_forward");

    // `d` on a caret rather than a span: nothing selected, nothing deleted.
    load(ed, "hello\n", 0);
    ed.doc.selections.Set(Selection{2, 2, -1});
    stays_clean(ed, "delete_selection_noyank");

    // An empty document has nothing in any direction.
    for (const std::string_view command :
         {"delete_char_backward", "delete_char_forward", "delete_selection_noyank"}) {
      load(ed, "", 0);
      stays_clean(ed, command);
    }
  }

  TEST_CASE("a no-op does not warn about a read-only file either");
  {
    // The "is not writable" warning sat on the same success path.
    Editor ed;
    load(ed, "hello\n", 0);
    ed.doc.read_only = true;
    ed.status.clear();
    RunCommands(ed, {"delete_char_backward"});
    EXPECT_TRUE(ed.status.empty());
    EXPECT_FALSE(ed.doc.modified);

    // A real edit still warns, once.
    RunCommands(ed, {"delete_char_forward"});
    EXPECT_TRUE(ed.status.find("is not writable") != std::string::npos);
    EXPECT_TRUE(ed.doc.modified);
  }

  TEST_CASE("a refused change does not drop into insert mode");
  {
    // `c` entered insert mode whatever came back, so a delete the table
    // refused left the editor typing into a buffer that had just told it no.
    // The refusal used to come from a freeze flag on generated views; those
    // are editable now, so the refusal here is the one that survives -- a
    // range whose end is inside a grapheme cluster.
    Editor ed;
    load(ed, "e\xcc\x81" "x\n", 0);
    ed.doc.selections.Set(Selection{0, 1, -1});
    ed.status.clear();
    RunCommands(ed, {"change_selection"});
    EXPECT_TRUE(ed.mode == Mode::kNormal);
    EXPECT_FALSE(ed.status.empty());
    EXPECT_EQ(text_of(ed), std::string("e\xcc\x81" "x\n"));
    EXPECT_FALSE(ed.doc.modified);
  }

  TEST_CASE("a delete that deletes something still marks and unmarks the buffer");
  {
    Editor ed;
    load(ed, "hello\n", 0);
    RunCommands(ed, {"delete_char_forward"});
    EXPECT_EQ(text_of(ed), std::string("ello\n"));
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_TRUE(CanUndo(ed.doc.table));
    EXPECT_FALSE(UnsavedBuffers(ed).empty());

    RunCommands(ed, {"undo"});
    EXPECT_EQ(text_of(ed), std::string("hello\n"));
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_TRUE(UnsavedBuffers(ed).empty());
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  const Scratch scratch{"koi-noop-edits"};
  const fs::path path = scratch.Write("subject.txt", "alpha\n");

  TEST_CASE("a no-op after a save keeps a saved buffer clean");
  {
    // The shape that rules out asking "is the serial the saved one and is
    // there nothing to undo?": here the serial *is* the saved one and there
    // is plenty to undo, and the buffer is still byte-for-byte the file.
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, path.string()));
    TypeInto(ed, 'X');
    EXPECT_TRUE(ed.doc.modified);
    RunTypableCommand(ed, "w");
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_TRUE(CanUndo(ed.doc.table));
    EXPECT_EQ(CurrentUndoSerial(ed.doc.table), ed.doc.saved_undo_serial);

    const std::string on_disk = text_of(ed);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"delete_char_backward"});
    EXPECT_EQ(text_of(ed), on_disk);
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_TRUE(UnsavedBuffers(ed).empty());
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("a backspace in the scratch buffer does not strand a [no name]");
  {
    // What the user met: one stray backspace in the empty buffer koi starts
    // on, and :q refused to quit for the rest of the session over a buffer
    // that had never held a byte.
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_EQ(DocLength(ed.doc.table), Index{0});
    RunCommands(ed, {"delete_char_backward"});
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_TRUE(UnsavedBuffers(ed).empty());

    // ...and the placeholder is still a placeholder, so opening a file reuses
    // it instead of leaving a second, unquittable buffer behind.
    EXPECT_TRUE(OpenTarget(ed, path.string()));
    EXPECT_EQ(BufferCount(ed), std::size_t{1});
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    ed.quit = false;
    RunTypableCommand(ed, "q");
    EXPECT_TRUE(ed.quit);
  }
}

}  // namespace koi
