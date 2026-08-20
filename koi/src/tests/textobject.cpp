// Tests for textobject.cpp: the text objects, and the bounds their lookups
// run under.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void TextObjectSuite() {
  TEST_CASE("textobjects: every textobjects.scm compiles");

  for (const LanguageSample& sample : kLanguageSamples) {
    std::vector<ObjectRange> out;
    std::string error;
    PieceTable empty = MakeTable("");
    const bool ok = TextObjectRanges(empty, std::filesystem::path{sample.filename}, "function",
                                     std::array<std::string_view, 1>{"around"}, out, error);
    if (!ok && (error.find("no textobjects.scm") == std::string::npos) &&
        (error.find("has no function") == std::string::npos)) {
      ++common::g_test_failures;
      std::cerr << "FAIL [" << common::g_test_case << "] " << sample.language << ": " << error
                << std::endl;
    } else {
      ++common::g_test_checks;
    }
  }

  TEST_CASE("textobjects: tree-sitter objects");

  const Scratch scratch{"koi-textobject-test"};
  const std::string source =
      "int add(int a, int b) {\n"
      "  return a + b;\n"
      "}\n"
      "\n"
      "int sub(int x, int y) {\n"
      "  return x - y;\n"
      "}\n";
  const std::filesystem::path file = scratch.Write("sample.cpp", source);

  {
    PieceTable table = MakeTable(source);
    std::vector<ObjectRange> around;
    std::string error;
    EXPECT_TRUE(TextObjectRanges(table, file, "function",
                                 std::array<std::string_view, 1>{"around"}, around, error));
    EXPECT_EQ(around.size(), size_t{2});

    std::string syntax_error;
    const auto syntax = OpenSyntax(file, syntax_error);
    EXPECT_TRUE(syntax != nullptr);
    if (syntax != nullptr) {
      std::vector<ObjectRange> via_tree;
      EXPECT_TRUE(TextObjectRanges(table, file, "function",
                                   std::array<std::string_view, 1>{"around"}, via_tree, error,
                                   syntax.get()));
      EXPECT_EQ(via_tree.size(), around.size());
      for (size_t i = 0; (i < via_tree.size()) && (i < around.size()); ++i) {
        EXPECT_EQ(via_tree[i].from, around[i].from);
        EXPECT_EQ(via_tree[i].to, around[i].to);
      }
    }
  }

  Editor ed;
  EXPECT_TRUE(!LoadDocument(file, ed.doc));
  ed.doc.view.rows = 10;
  ed.doc.view.columns = 60;
  const auto sel_text = [&ed] {
    return ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range());
  };

  const auto place = [&ed](Index at) {
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
  };
  place(static_cast<Index>(source.find("a + b")));
  RunCommands(ed, {"select_textobject_inner"});
  ApplyPendingChar(ed, "f");
  EXPECT_EQ(sel_text(), std::string{"{\n  return a + b;\n}"});
  RunCommands(ed, {"select_textobject_around"});
  ApplyPendingChar(ed, "f");
  EXPECT_EQ(sel_text(), std::string{"int add(int a, int b) {\n  return a + b;\n}"});

  place(static_cast<Index>(source.find("int b")));
  RunCommands(ed, {"select_textobject_inner"});
  ApplyPendingChar(ed, "a");
  EXPECT_EQ(sel_text(), std::string{"int b"});

  place(0);
  RunCommands(ed, {"goto_next_function"});
  EXPECT_EQ(sel_text(), std::string{"{\n  return a + b;\n}"});
  RunCommands(ed, {"goto_next_function"});
  EXPECT_EQ(sel_text(), std::string{"int sub(int x, int y) {\n  return x - y;\n}"});
  RunCommands(ed, {"goto_prev_function"});
  EXPECT_EQ(sel_text(), std::string{"int add(int a, int b) {\n  return a + b;\n}"});

  ed.status.clear();
  RunCommands(ed, {"goto_next_test"});
  EXPECT_TRUE(ed.status.find("test") != std::string::npos);

  TEST_CASE("textobjects: paragraphs");

  {
    Editor p;
    ResetToOriginal(p.doc.table, "one\ntwo\n\nthree\nfour\n\n\nfive\n");
    p.doc.view.rows = 10;
    p.doc.view.columns = 40;
    p.doc.selections.Set(MinWidth1(p.doc.table, Selection{0, 0, -1}));

    RunCommands(p, {"goto_next_paragraph"});
    EXPECT_EQ(p.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(p.doc.selections.Primary().To(), Index{9});

    RunCommands(p, {"goto_next_paragraph"});
    EXPECT_EQ(p.doc.selections.Primary().From(), Index{9});
    EXPECT_EQ(p.doc.selections.Primary().To(), Index{22});

    RunCommands(p, {"goto_prev_paragraph"});
    EXPECT_EQ(p.doc.selections.Primary().From(), Index{9});
    EXPECT_TRUE(p.doc.selections.Primary().Backward());
  }

  {
    Editor p;
    ResetToOriginal(p.doc.table, "alpha\nbeta\n\ngamma\n");
    p.doc.view.rows = 10;
    p.doc.view.columns = 40;
    p.doc.selections.Set(MinWidth1(p.doc.table, Selection{0, 0, -1}));
    RunCommands(p, {"select_textobject_inner"});
    ApplyPendingChar(p, "p");
    EXPECT_EQ(ReadDocRange(p.doc.table, p.doc.selections.Primary().Range()),
              std::string{"alpha\nbeta\n"});
    RunCommands(p, {"select_textobject_around"});
    ApplyPendingChar(p, "p");
    EXPECT_EQ(ReadDocRange(p.doc.table, p.doc.selections.Primary().Range()),
              std::string{"alpha\nbeta\n\n"});

    const Index in_gamma = 13;
    p.doc.selections.Set(MinWidth1(p.doc.table, Selection{in_gamma, in_gamma, -1}));
    RunCommands(p, {"select_textobject_around"});
    ApplyPendingChar(p, "p");
    EXPECT_EQ(ReadDocRange(p.doc.table, p.doc.selections.Primary().Range()),
              std::string{"gamma\n"});
  }

  {
    Editor p;
    ResetToOriginal(p.doc.table, "alpha\nbeta\n\ngamma\n\n\n");
    p.doc.view.rows = 10;
    p.doc.view.columns = 40;
    p.doc.selections.Set(MinWidth1(p.doc.table, Selection{13, 13, -1}));
    RunCommands(p, {"select_textobject_around"});
    ApplyPendingChar(p, "p");
    EXPECT_EQ(ReadDocRange(p.doc.table, p.doc.selections.Primary().Range()),
              std::string{"gamma\n\n\n"});
    p.doc.selections.Set(MinWidth1(p.doc.table, Selection{13, 13, -1}));
    RunCommands(p, {"select_textobject_inner"});
    ApplyPendingChar(p, "p");
    EXPECT_EQ(ReadDocRange(p.doc.table, p.doc.selections.Primary().Range()),
              std::string{"gamma\n"});
  }
}

void TextObjectLookupsAreBoundedAndSayWhenTheyFallShort() {
  TEST_CASE("textobjects: a runaway query is cut off on the keystroke that ran it");

  // TextObjectRanges budgeted its parse and left the query after it unbounded:
  // no deadline, no match limit, nothing consulted afterwards. This runs on the
  // UI thread under `mi`/`ma`, synchronously, so until the query returns no key
  // is read -- not Esc, not Ctrl-C -- and matching is quadratic in the depth of
  // the tree over a range every ancestor contains, so a file that parses in
  // milliseconds can match for minutes.
  //
  // Measured here with the budget lifted, the fixture below costs about six
  // seconds against the 500 ms it is allowed: a machine ten times quicker than
  // this one still sees the deadline fire.
  const FakeQueryDir queries{"cpp"};
  EXPECT_TRUE(queries.Ready());
  if (!queries.Ready()) return;

  const std::string kBudgetLine = "text objects exceeded the 500ms budget -- some are missing";
  const std::string kMatchLine = "too many query matches -- some text objects are missing";

  const Scratch scratch{"koi-textobject-budget"};

  // -- the deadline: partial ranges, and a line saying so ------------------
  queries.Write("textobjects.scm", PilingUpQuery(64, "function.inside", "function.around"));
  const std::string runaway = WideCalls(24, 1000, true);
  const std::filesystem::path runaway_file = scratch.Write("runaway.cpp", runaway);

  OnAThreadOfItsOwn([&] {
    PieceTable table = MakeTable(runaway);
    std::vector<ObjectRange> out;
    std::string error;
    bool ok = false;
    const long long ms = MillisecondsOf([&] {
      ok = TextObjectRanges(table, runaway_file, "function",
                            std::array<std::string_view, 1>{"around"}, out, error);
    });
    // True, not false: what it found is usable, and throwing it away because
    // the rest is missing would be the second failure mode of the two.
    EXPECT_TRUE(ok);
    EXPECT_EQ(error, kBudgetLine);
    EXPECT_TRUE(!out.empty());
    EXPECT_TRUE(ms < 4000);
  });

  // The same file through the keystroke that reaches it. The complaint has to
  // survive as far as the status line: a lookup that came back short and said
  // nothing is indistinguishable from a file with no functions in it.
  OnAThreadOfItsOwn([&] {
    Editor ed;
    ed.doc.file = runaway_file;
    ResetToOriginal(ed.doc.table, runaway);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    const long long ms = MillisecondsOf([&] {
      RunCommands(ed, {"select_textobject_around"});
      ApplyPendingChar(ed, "f");
    });
    EXPECT_EQ(ed.status.text(), kBudgetLine);
    EXPECT_TRUE(ed.status.level() == StatusLevel::kWarning);
    EXPECT_TRUE(ms < 4000);
  });

  // -- the match limit: the other way it can come back short ---------------
  //
  // Same shape, dialled the other way: few patterns, one call, and enough
  // identifiers in it that the states in flight pass the 4096 cap. Left
  // uncapped, tree-sitter allocates a capture list per state and the file
  // decides how many there are.
  queries.Write("textobjects.scm", PilingUpQuery(8, "function.inside", "function.around"));
  const std::string wide = WideCalls(300, 1, true);
  const std::filesystem::path wide_file = scratch.Write("wide.cpp", wide);

  OnAThreadOfItsOwn([&] {
    PieceTable table = MakeTable(wide);
    std::vector<ObjectRange> out;
    std::string error;
    EXPECT_TRUE(TextObjectRanges(table, wide_file, "function",
                                 std::array<std::string_view, 1>{"around"}, out, error));
    // Which of the two lines wins is a matter of how fast this machine is --
    // saturating the cap costs about 200 ms here, and the budget is 500 -- and
    // both are the same promise kept. What must not happen is silence.
    EXPECT_TRUE(!error.empty());
    EXPECT_TRUE((error == kMatchLine) || (error == kBudgetLine));
  });

  TEST_CASE("textobjects: the Syntax-backed path admits a short query too");

  // Everything above is the standalone path, which is the one a lookup with no
  // buffer behind it takes. A buffer open in the editor has a Syntax on it, and
  // then the lookup runs through Captures over the tree that is already there --
  // which is the path every `mi`, `ma` and `]f` in a real session takes, and the
  // one that asked for no report and got none. A query the frame budget cut in
  // half came back as a shorter list of objects and not a word about it, so the
  // jump landed on the nearest of the objects that survived the cut: silently
  // the wrong one, where the standalone path's answer is the right one with a
  // complaint over it.
  const std::string kShortLine =
      "the text-object query came back short -- some objects are missing";

  // Dialled the way the symbol scan's own match-limit case is dialled: many
  // patterns over few identifiers, which fills the pool inside the first call
  // rather than after a second of matching, so what the run hits is the cap and
  // not the clock.
  queries.Write("textobjects.scm", PilingUpQuery(128, "function.inside", "function.around"));
  const std::string dense = WideCalls(24, 8, true);
  const std::filesystem::path dense_file = scratch.Write("dense.cpp", dense);

  OnAThreadOfItsOwn([&] {
    PieceTable table = MakeTable(dense);
    std::string syntax_error;
    const auto syntax = OpenSyntax(dense_file, syntax_error);
    EXPECT_TRUE(syntax != nullptr);
    if (syntax == nullptr) return;

    std::vector<ObjectRange> out;
    std::string error;
    EXPECT_TRUE(TextObjectRanges(table, dense_file, "function",
                                 std::array<std::string_view, 1>{"around"}, out, error,
                                 syntax.get()));
    EXPECT_EQ(error, kShortLine);
  });

  // And as far as the status line, through the keystroke itself. `]f` rather
  // than `maf`: a select asks about the bytes under the carets and a jump asks
  // about the whole document, and it is the second that meets the bound.
  OnAThreadOfItsOwn([&] {
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.doc.file = dense_file;
    ResetToOriginal(ed.doc.table, dense);
    AttachSyntax(ed);
    EXPECT_TRUE(ed.doc.syntax != nullptr);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"goto_next_function"});
    EXPECT_EQ(ed.status.text(), kShortLine);
    EXPECT_TRUE(ed.status.level() == StatusLevel::kWarning);
  });

  // -- a node with no width in it -------------------------------------------
  //
  // Auto-indentation asks Syntax::Captures to keep captures whose node spans no
  // bytes, because mid-keystroke that is what a brace-less body is. Nothing here
  // asks, and nothing here may get them: a text object is a range somebody is
  // about to put a cursor in, and an empty one selects nothing while still
  // counting as a match. The query below captures the `if`'s consequence, which
  // in this buffer is an `expression_statement` holding only the `;` the parser
  // supplied -- so it fires, and both paths still hand back nothing.
  queries.Write("textobjects.scm",
                "(if_statement consequence: (_) @function.inside) @function.around");
  const std::string half_typed = "int main() {\n    if (x)\n}\n";
  const std::filesystem::path half_typed_file = scratch.Write("halftyped.cpp", half_typed);

  OnAThreadOfItsOwn([&] {
    PieceTable table = MakeTable(half_typed);
    std::string syntax_error;
    const auto syntax = OpenSyntax(half_typed_file, syntax_error);
    EXPECT_TRUE(syntax != nullptr);

    std::vector<ObjectRange> out;
    std::string error;
    // The standalone path, which runs its own cursor and carries its own guard.
    EXPECT_TRUE(TextObjectRanges(table, half_typed_file, "function",
                                 std::array<std::string_view, 1>{"inside"}, out, error));
    EXPECT_EQ(error, std::string{});
    EXPECT_TRUE(out.empty());
    // And the Syntax-backed one, which relies on Captures defaulting to today's
    // behaviour. The `around` capture is the whole `if_statement` and does come
    // back, so this is the empty span being dropped and not the query failing to
    // match.
    if (syntax != nullptr) {
      out.clear();
      EXPECT_TRUE(TextObjectRanges(table, half_typed_file, "function",
                                   std::array<std::string_view, 1>{"inside"}, out, error,
                                   syntax.get()));
      EXPECT_TRUE(out.empty());
      out.clear();
      EXPECT_TRUE(TextObjectRanges(table, half_typed_file, "function",
                                   std::array<std::string_view, 1>{"around"}, out, error,
                                   syntax.get()));
      EXPECT_EQ(out.size(), std::size_t{1});
    }
  });

  // -- and an ordinary file, with the query koi ships ------------------------
  //
  // The control that makes the two cases above about the budget and not about
  // the fixture: same call, real query, and not a word out of it.
  queries.Forget();
  const std::string plain =
      "int add(int a, int b) {\n"
      "  return a + b;\n"
      "}\n"
      "\n"
      "int sub(int x, int y) {\n"
      "  return x - y;\n"
      "}\n";
  const std::filesystem::path plain_file = scratch.Write("plain.cpp", plain);

  OnAThreadOfItsOwn([&] {
    PieceTable table = MakeTable(plain);
    std::vector<ObjectRange> out;
    std::string error;
    EXPECT_TRUE(TextObjectRanges(table, plain_file, "function",
                                 std::array<std::string_view, 1>{"around"}, out, error));
    EXPECT_EQ(error, std::string{});
    EXPECT_EQ(out.size(), std::size_t{2});
  });
}

}  // namespace koi
