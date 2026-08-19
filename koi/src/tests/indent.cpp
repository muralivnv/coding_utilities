// The tree-sitter indent engine (indent.cpp) and the newline path that consumes
// it (commands.cpp). Every expectation here is an exact indent string, because
// "starts with four spaces" is true of eight as well and this is exactly the
// behaviour that regresses by one level at a time.

#include "indent.h"

#include <thread>

#include "tests.h"

namespace koi {
namespace {

constexpr IndentStyle kFourSpaces{4, true};
constexpr IndentStyle kTabs{4, false};

struct Fixture {
  std::shared_ptr<Syntax> syntax;
  PieceTable table;

  bool ok() const { return syntax != nullptr; }
};

Fixture Open(std::string_view file, std::string_view text) {
  Fixture out;
  std::string error;
  out.syntax = OpenSyntax(std::filesystem::path{file}, error);
  ResetToOriginal(out.table, std::string{text});
  if (out.syntax != nullptr) out.syntax->Sync(out.table);
  return out;
}

// The indent the engine computes for the line a Return at `cursor` would open,
// or a sentinel that no real answer can collide with.
std::string ForNewline(std::string_view file, std::string_view text, Index cursor,
                       const IndentStyle& style = kFourSpaces) {
  Fixture fixture = Open(file, text);
  if (!fixture.ok()) return "<no grammar>";
  std::string error;
  const auto got = TreeIndentForNewline(fixture.table, *fixture.syntax, cursor, style, error);
  return got.has_value() ? *got : std::string{"<declined>"};
}

std::string ForLine(std::string_view file, std::string_view text, Index line,
                    const IndentStyle& style = kFourSpaces) {
  Fixture fixture = Open(file, text);
  if (!fixture.ok()) return "<no grammar>";
  std::string error;
  const auto got = TreeIndentForLine(fixture.table, *fixture.syntax, line, style, error);
  return got.has_value() ? *got : std::string{"<declined>"};
}

// Return pressed at `at`, through the real command, so auto-pairs' closing line
// and the undo grouping ride along.
//
// `paint` draws the whole buffer once first. What knows a byte belongs to an
// injected region is a paint, and in the editor one runs between every pair of
// keystrokes; a test that wants the guard in the picture has to say so.
Editor PressReturn(std::string_view file, std::string_view text, std::span<const Index> carets,
                   bool paint = false) {
  Editor ed;
  std::string error;
  ed.doc.syntax = OpenSyntax(std::filesystem::path{file}, error);
  ed.doc.file = std::filesystem::path{file};
  ResetToOriginal(ed.doc.table, std::string{text});
  if (paint && (ed.doc.syntax != nullptr)) {
    ed.doc.syntax->Sync(ed.doc.table);
    std::vector<CaptureId> painted;
    ed.doc.syntax->Paint(ed.doc.table, Interval(0, DocLength(ed.doc.table)), painted);
  }
  std::vector<Selection> ranges;
  for (const Index at : carets) ranges.push_back(Selection{at, at, -1});
  ed.doc.selections.Replace(ed.doc.table, ranges);
  RunCommands(ed, {"insert_newline"});
  return ed;
}

std::string Pressed(std::string_view file, std::string_view text, Index at) {
  const std::array<Index, 1> carets{at};
  Editor ed = PressReturn(file, text, carets);
  return AssembleDocContents(ed.doc.table);
}

// A buffer with a grammar behind it, in insert mode, with the real key path in
// front of it: keys go to HandleKeyInput exactly as the event loop hands them
// over, so the keymap lookup, the auto-pairs branch, the pending batching and
// the undo grouping are all in the picture and none of them is a test double.
struct Typist {
  Editor ed;
  KeyMaps maps{DefaultKeyMaps()};
  std::vector<Key> pending;

  Typist(std::string_view file, std::string_view text, Index at, bool auto_pairs) {
    std::string error;
    if (!file.empty()) {
      ed.doc.syntax = OpenSyntax(std::filesystem::path{file}, error);
      ed.doc.file = std::filesystem::path{file};
    }
    ed.settings.auto_pairs = auto_pairs;
    ResetToOriginal(ed.doc.table, std::string{text});
    ed.doc.selections.Set(Selection{at, at, -1});
    ed.mode = Mode::kInsert;
  }

  void Type(std::string_view keys) {
    for (const char c : keys) {
      Key key;
      if (c == '\n') {
        key.named = NamedKey::kRet;
      } else {
        key.code = static_cast<std::uint32_t>(static_cast<unsigned char>(c));
      }
      HandleKeyInput(ed, maps, key, pending);
    }
    // A key that is a live prefix in the insert map (`j`, which `jk` completes)
    // sits in `pending` until the chord times out. Nothing typed below is one,
    // and this is what the timeout would do.
    if (!pending.empty()) FlushPendingAsText(ed, pending);
  }

  std::string Text() const { return AssembleDocContents(ed.doc.table); }
};

// What `keys`, typed one at a time into `text` at `at`, leaves in the buffer.
std::string Typed(std::string_view file, std::string_view text, Index at, std::string_view keys,
                  bool auto_pairs = false) {
  Typist typist{file, text, at, auto_pairs};
  typist.Type(keys);
  return typist.Text();
}

// The leading whitespace of `line`, spelled so that a failure prints the
// difference between four spaces and eight rather than "not equal".
std::string LeadingOf(const PieceTable& table, Index line) {
  const Index start = LineStart(table, line);
  Index at = start;
  char c = 0;
  while (ByteAt(table, at, c) && ((c == ' ') || (c == '\t'))) ++at;
  return ReadDocRange(table, Interval(start, at));
}

struct IndentCase {
  std::string_view what;
  std::string_view file;
  std::string_view text;
  Index cursor;
  std::string_view want;
};

void RunNewlineCases(std::span<const IndentCase> cases) {
  for (const IndentCase& one : cases) {
    const std::string got = ForNewline(one.file, one.text, one.cursor);
    if (got != one.want) std::cerr << "  case: " << one.what << "\n";
    EXPECT_EQ(got, std::string{one.want});
  }
}

void RunLineCases(std::span<const IndentCase> cases) {
  for (const IndentCase& one : cases) {
    const std::string got = ForLine(one.file, one.text, one.cursor);
    if (got != one.want) std::cerr << "  case: " << one.what << "\n";
    EXPECT_EQ(got, std::string{one.want});
  }
}

}

void TreeIndentEngine() {
  TEST_CASE("tree indent engine");

  // -- C/C++ ---------------------------------------------------------------
  //
  // The buffers below carry their closing delimiters because that is what the
  // editor's own auto-pairs put there: a `{` typed with auto-pairs on is a `{}`
  // with the caret between them by the time Return is pressed. `int main() {`
  // on its own is covered too -- the parser invents the `}` at the caret, and
  // `Capture::unterminated` is what lets that node still own the line below it.
  static constexpr std::array kC{
      IndentCase{"unterminated block owns the line under it", "a.cpp", "int main() {", 12, "    "},
      IndentCase{"auto-paired block: the caret's line is the closer's, plus one",
                 "a.cpp", "int main() {}", 12, ""},
      IndentCase{"a statement inside a block stays at the block's depth", "a.cpp",
                 "int main() {\n    work()\n}", 23, "    "},
      IndentCase{"an unterminated block still holds its depth through hybrid", "a.cpp",
                 "int main() {\n    work()", 23, "    "},
      IndentCase{"a closed continuation comes back to the block's depth -- gap 2", "a.cpp",
                 "int main() {\n    foo(a,\n        b)\n}", 34, "    "},
      IndentCase{"a brace inside a string is not a block", "a.cpp", "  puts(\"{\");\n", 12, "  "},
      IndentCase{"a brace inside a char literal is not a block", "a.cpp",
                 "  if (c == '}') {\n", 17, "      "},
      IndentCase{"descending into a brace-less body -- scope header", "a.cpp",
                 "int main() {\n    if (x)\n        work();\n}", 23, "        "},
      IndentCase{"pos 0", "a.cpp", "int main() {\n    work();\n}\n", 0, ""},
      IndentCase{"pos at eof", "a.cpp", "int main() {\n    work();\n}\n", 27, ""},
      IndentCase{"an empty buffer", "a.cpp", "", 0, ""},
  };
  RunNewlineCases(kC);

  // Re-indenting lines that already exist. The brace-less body is one level
  // deeper than the block, and the braced one is *also* one level deeper and not
  // two: `(#not-kind-eq? @indent "compound_statement")` is what keeps the
  // header rule off a body that already has a block of its own.
  static constexpr std::array kCLines{
      IndentCase{"brace-less if body", "a.cpp", "int main() {\n    if (x)\n        work();\n}", 2,
                 "        "},
      IndentCase{"braced if body does not double-indent", "a.cpp",
                 "int main() {\n    if (x) {\n        work();\n    }\n}", 2, "        "},
      IndentCase{"the closing brace sits at the block's own depth", "a.cpp",
                 "int main() {\n    work();\n}", 2, ""},
      IndentCase{"an access specifier is outdented to the class's depth", "a.cpp",
                 "class A {\n    public:\n    int x;\n};", 1, ""},
      IndentCase{"a member after it is not", "a.cpp", "class A {\n    public:\n    int x;\n};", 2,
                 "    "},
  };
  RunLineCases(kCLines);

  // -- the body that has not been typed yet --------------------------------
  //
  // Mid-keystroke the document is not the one anybody means to write, and the
  // parser says so with a node that spans no bytes. `if (c)` with the block's
  // `}` already below it -- which is what auto-pairs, on by default, leaves --
  // completes as an `if_statement` whose consequence is an `expression_statement`
  // holding one token, the `;` the grammar supplied, at the caret. That empty
  // node *is* the brace-less body, the header rule captures it, and the line the
  // Return opens is one level inside it. Dropped as a zero-width span, the whole
  // construct disappears and the new line lands at the block's depth instead of
  // the body's -- and the statement typed there is then a level shallow for
  // every line after it.
  static constexpr std::array kEmptyBody{
      IndentCase{"if: the empty consequence is the body the line opens", "a.cpp",
                 "void g() {\n    if (x)\n}", 21, "        "},
      IndentCase{"while: same shape, same rule", "a.cpp", "void g() {\n    while (x)\n}", 24,
                 "        "},
      IndentCase{"for: same shape, same rule", "a.cpp", "void g() {\n    for (;;)\n}", 23,
                 "        "},
      // Two brace-less headers, two levels. The inner `if` is itself the outer
      // one's empty-consequence-turned-real, and it now ends in an invented token
      // too, so `unterminated` carries both scopes over the line being opened.
      IndentCase{"nested brace-less headers stack", "a.cpp",
                 "void g() {\n    if (x)\n        if (y)\n}", 36, "            "},
      // The body already has a block of its own, so the header rule's
      // `(#not-kind-eq? @indent "compound_statement")` keeps it off. The caret is
      // between an auto-paired `{}`, so -- as in the `int main() {}` case above --
      // this is the *closer's* column and the command path puts the caret one
      // level inside it: one level, not two.
      IndentCase{"a braced body does not stack the empty one on top", "a.cpp",
                 "void g() {\n    if (x) {}\n}", 23, "    "},
      // Nothing to descend into, so nothing changes: a `{` on its own line, a
      // statement, a closer. These are the shapes the zero-width captures pass
      // through.
      IndentCase{"a plain statement is unmoved by the empty nodes now in view", "a.cpp",
                 "void g() {\n    work();\n}", 22, "    "},
  };
  RunNewlineCases(kEmptyBody);

  // Without the `}` -- auto-pairs off -- the same keystroke gets a different
  // *tree*, not a different answer from the same one: with the block unclosed the
  // grammar cannot complete the `if` at all and recovers by making the whole
  // prefix one ERROR node, which holds no `if_statement`, no consequence and
  // nothing the C query captures. There is no body to descend into because there
  // is no body, and the honest answer is the block's own depth. helix computes
  // the same 4 from the same tree; this is the grammar's error recovery and not
  // the engine, and it is the twin of the `)` case in the re-indent suite below.
  static constexpr std::array kUnclosedBlock{
      IndentCase{"an unclosed block leaves the if inside an ERROR", "a.cpp",
                 "void g() {\n    if (x)", 21, "    "},
      IndentCase{"and the same for while", "a.cpp", "void g() {\n    while (x)", 24, "    "},
  };
  RunNewlineCases(kUnclosedBlock);

  // A body the user put in the wrong column, which is what the bug above used to
  // leave behind. The tree says `work();` belongs at two levels; it is written at
  // one. The hybrid heuristic measures the difference between the new line and a
  // real one, and the real one it can reach is that body -- so the new line comes
  // out one level *below* where the tree would put it on its own, at column zero.
  //
  // This is helix's arithmetic, unchanged, and koi keeps it deliberately: the
  // whole point of the hybrid is that a line somebody indented by hand outranks
  // the query, and a rule that overruled a baseline whenever the tree disagreed
  // with it would take the yaml case above (two spaces where the query says four)
  // with it. What removes the disaster is that the body no longer lands in the
  // wrong column to begin with -- the case above -- not a clamp here. The
  // sequence the user actually types is pinned end to end in the newline-path
  // suite.
  EXPECT_EQ(ForNewline("a.cpp", "void g() {\n    if (x)\n    work();\n}", 33), std::string(""));
  // Written where the tree wants it, the very next line is the block's depth
  // again: the consequence ends at the caret and owns nothing under it.
  EXPECT_EQ(ForNewline("a.cpp", "void g() {\n    if (x)\n        work();\n}", 37),
            std::string("    "));

  // The other thing the parser invents, and the one place keeping empty spans
  // could have cost something. `foo(a,` left open and a call typed under it
  // parses with a `MISSING )` sitting *between* the `)` of that call and the
  // `;` -- zero-width, mid-line, mid-document, and captured `@outdent` by the
  // very same `[")" "]" "}"] @outdent` line that a real closer is. Counted, it
  // would dedent the line a Return there opens by a level nobody typed a bracket
  // for. `@outdent` names the token that begins a line; a token that is not in
  // the buffer begins nothing, so Collect refuses the role to any span with no
  // bytes in it.
  EXPECT_EQ(ForNewline("a.cpp", "int main() {\n  foo(a,\n  bar();\n}", 29), std::string("  "));

  // The request itself, at the layer that grants it. Only a caller that asks
  // sees the empty spans: text objects and highlights want a range somebody can
  // put a cursor in, and textobject.cpp's second path -- the one that runs its
  // own cursor rather than going through Syntax -- carries the same `to > from`
  // guard, so the two agree by default rather than by coincidence.
  {
    Fixture fixture = Open("a.cpp", "void g() {\n    if (x)\n}");
    EXPECT_TRUE(fixture.ok());
    if (fixture.ok()) {
      const std::array<std::string_view, 1> files{"indents.scm"};
      const Interval whole = Interval(0, DocLength(fixture.table));
      std::string error;

      std::vector<Capture> kept;
      EXPECT_TRUE(
          fixture.syntax->Captures(fixture.table, files, whole, kept, error, nullptr, true));
      // The empty consequence, at the caret, ending in a token the parser wrote:
      // `unterminated` is what makes it own the line below, and `parent_from` is
      // the `if_statement` its `scope "header"` opens the scope at.
      EXPECT_TRUE(std::ranges::any_of(kept, [](const Capture& capture) {
        return (capture.name == "indent") && (capture.from == Index{21}) &&
               (capture.to == Index{21}) && capture.unterminated &&
               (capture.parent_from == Index{15});
      }));

      // The parent is the expensive half of that answer -- reaching it means
      // walking down from the root of the tree -- so it is paid for exactly the
      // captures that can spend it: the ones whose pattern set `scope` to
      // `header`. Everything else in the same run carries its own start, which
      // is what a node with no parent carries anyway.
      EXPECT_TRUE(std::ranges::all_of(kept, [](const Capture& capture) {
        return (capture.parent_from == capture.from) ||
               ((capture.from == Index{21}) && (capture.to == Index{21}));
      }));

      std::vector<Capture> dropped;
      EXPECT_TRUE(fixture.syntax->Captures(fixture.table, files, whole, dropped, error));
      EXPECT_TRUE(std::ranges::none_of(
          dropped, [](const Capture& capture) { return capture.from == capture.to; }));
      // And a caller that did not ask gets neither structure field, at either
      // price: a text-object lookup over a whole file reads no parent and no
      // invented token, and paying for them out of its query budget cost it a
      // third of the captures it came for.
      EXPECT_TRUE(std::ranges::all_of(dropped, [](const Capture& capture) {
        return (capture.parent_from == capture.from) && !capture.unterminated;
      }));
      // And nothing else changed hands: the default run is the same list with
      // the empty spans taken out of it.
      const auto empty = std::ranges::count_if(
          kept, [](const Capture& capture) { return capture.from == capture.to; });
      EXPECT_TRUE(empty > 0);
      EXPECT_EQ(std::ssize(dropped) + empty, std::ssize(kept));
    }
  }

  // -- python: @extend and @extend.prevent-once ----------------------------
  //
  // `def f():` ends exactly at the caret and has no body node yet; what keeps it
  // owning the line below is `@extend`, whose first condition -- the caret is on
  // the line the node ends on -- holds. `return x` carries
  // `@extend.prevent-once`, which cancels that ownership, and the line after it
  // comes back out to the module's depth.
  static constexpr std::array kPython{
      IndentCase{"a header extends over the line it opens", "a.py", "def f():", 8, "    "},
      IndentCase{"and keeps extending while the body is deeper", "a.py", "def f():\n    x = 1", 18,
                 "    "},
      IndentCase{"return ends the extension", "a.py", "def f():\n    return x", 21, ""},
      IndentCase{"pass ends it too", "a.py", "def f():\n    pass", 17, ""},
      IndentCase{"a nested body keeps the levels it is inside", "a.py",
                 "def f():\n    if x:\n        y = 1", 32, "        "},
  };
  RunNewlineCases(kPython);

  // @opaque: a docstring's interior is content, and its leading whitespace is
  // handed back exactly as written rather than recomputed.
  EXPECT_EQ(ForLine("a.py", "def f():\n    \"\"\"a\n  b\n\"\"\"\n", 2), std::string("  "));

  // -- yaml: @indent.always ------------------------------------------------
  static constexpr std::array kYaml{
      IndentCase{"a key with no value opens a block", "a.yaml", "foo:", 4, "    "},
      IndentCase{"a key with a value on its own line does not", "a.yaml", "foo: bar", 8, ""},
      // Hybrid at work: the tree says "the same level as `bar: baz`", and the
      // level that line actually uses is two spaces, not four. Manual
      // indentation wins over the configured unit when the tree implies no
      // change.
      IndentCase{"a key whose value is below it keeps the block open", "a.yaml",
                 "foo:\n  bar: baz", 15, "  "},
  };
  RunNewlineCases(kYaml);

  // -- rust, go, json ------------------------------------------------------
  static constexpr std::array kOthers{
      IndentCase{"rust: a statement inside a block", "a.rs", "fn a() {\n    let x = 1;\n}", 23,
                 "    "},
      // helix's `value: (_) @indent (#not-same-line? ...)` rules cannot reach
      // this -- under containment a scope has to *open above* the line it
      // indents, and the value node either does not exist yet (here) or begins
      // on the very line it would be indenting. koi's additions to rust's file
      // are what answer it, and the whole family is pinned in
      // TreeIndentVendoredQueryAdditions.
      IndentCase{"rust: an unfinished binding opens its continuation", "a.rs",
                 "fn a() {\n    let y =\n}", 20, "        "},
      IndentCase{"go: an unterminated brace is an ERROR the query names", "a.go",
                 "func main() {", 13, "    "},
      IndentCase{"json: an object opens a level", "a.json", "{\n  \"a\": 1\n}", 1, "    "},
  };
  RunNewlineCases(kOthers);

  // -- tabs ----------------------------------------------------------------
  //
  // A level is one tab. An `@align` is a column, which is a spaces idea: it is
  // spelled as the tabs the anchor's own line used, followed by spaces for the
  // rest, so the alignment survives a change of tab width. That is a property of
  // the line being aligned to and not of the document's style -- the same buffer
  // gives the same aligned prefix whichever way the style is set.
  EXPECT_EQ(ForNewline("a.cpp", "int main() {\n\twork()\n}", 20, kTabs), std::string("\t"));
  EXPECT_EQ(ForNewline("a.cpp", "int main() {\n\tfoo(a,\n\t\tb)\n}", 20, kTabs),
            std::string("\t    "));
  EXPECT_EQ(ForNewline("a.cpp", "int main() {\n\tfoo(a,\n\t\tb)\n}", 20, kFourSpaces),
            std::string("\t    "));

  // IndentedOnce / OutdentedOnce work on the rendered string, including one that
  // came from an alignment and has no whole number of levels in it.
  EXPECT_EQ(IndentedOnce("      ", kFourSpaces), std::string("          "));
  EXPECT_EQ(OutdentedOnce("      ", kFourSpaces), std::string("  "));
  EXPECT_EQ(OutdentedOnce("  ", kFourSpaces), std::string(""));
  EXPECT_EQ(OutdentedOnce("", kFourSpaces), std::string(""));
  EXPECT_EQ(IndentedOnce("\t", kTabs), std::string("\t\t"));
  EXPECT_EQ(OutdentedOnce("\t    ", kTabs), std::string("\t"));
}

void TreeIndentFallbacks() {
  TEST_CASE("tree indent fallbacks");

  // Every way the engine can decline, and the guarantee that declining is what
  // it does rather than crashing or guessing.

  // -- a language with no indents.scm at all -------------------------------
  EXPECT_FALSE(HasIndentQuery("markdown"));
  EXPECT_FALSE(HasIndentQuery("diff"));
  EXPECT_TRUE(HasIndentQuery("cpp"));
  EXPECT_TRUE(HasIndentQuery("python"));
  {
    Fixture fixture = Open("a.md", "- item");
    EXPECT_TRUE(fixture.ok());
    if (fixture.ok()) {
      std::string error;
      EXPECT_FALSE(
          TreeIndentForNewline(fixture.table, *fixture.syntax, 6, kFourSpaces, error).has_value());
      // Nothing is wrong, so there is nothing to put on the status line.
      EXPECT_EQ(error, std::string{});
    }
  }

  // -- the budget ----------------------------------------------------------
  //
  // The deadline covers the whole call, the hybrid baselines included, so a
  // budget already spent on entry refuses before it runs a single query.
  {
    Fixture fixture = Open("a.cpp", "int main() {\n    work();\n}\n");
    EXPECT_TRUE(fixture.ok());
    if (fixture.ok()) {
      std::string error;
      EXPECT_FALSE(TreeIndentForNewline(fixture.table, *fixture.syntax, 23, kFourSpaces, error,
                                        std::chrono::milliseconds{0})
                       .has_value());
      EXPECT_EQ(error, std::string{});
      EXPECT_FALSE(TreeIndentForLine(fixture.table, *fixture.syntax, 1, kFourSpaces, error,
                                     std::chrono::milliseconds{0})
                       .has_value());
      // And with the real budget it answers, so the refusal above is the
      // deadline and not the document.
      EXPECT_TRUE(
          TreeIndentForNewline(fixture.table, *fixture.syntax, 23, kFourSpaces, error).has_value());
    }
  }

  // -- a position the document does not have -------------------------------
  //
  // The line and the injection guard are asked about the clamped caret, and the
  // engine's Request used to be handed the raw one: a byte_pos past the end
  // moves no node into post-insertion coordinates, nothing then contains the
  // line being opened, and the answer collapsed to column zero. Out of reach from
  // commands.cpp, which only ever asks about a live cursor, so what is pinned is
  // the equality rather than a column: an out-of-range position answers as the
  // position it clamps to.
  {
    // A block opened at the very end of the buffer: the answer there is a level
    // in, and it is one the hybrid cannot put back by inheriting a neighbour --
    // the only line above is the header itself, at column zero. A buffer whose
    // out-of-range answer the hybrid rescues would pin nothing.
    const std::string text = "int main() {";
    const Index eof = static_cast<Index>(text.size());
    const std::string at_eof = ForNewline("a.cpp", text, eof);
    EXPECT_EQ(at_eof, std::string("    "));
    EXPECT_EQ(ForNewline("a.cpp", text, eof + 1), at_eof);
    EXPECT_EQ(ForNewline("a.cpp", text, eof + 4096), at_eof);
    const std::string at_zero = ForNewline("a.cpp", text, 0);
    EXPECT_EQ(ForNewline("a.cpp", text, -1), at_zero);
    EXPECT_EQ(ForNewline("a.cpp", text, -4096), at_zero);
  }

  // -- an indents.scm that appears mid-session -----------------------------
  //
  // The has-a-query memo remembers only a `yes`, which is CompileQuery's rule
  // for a read that failed: a `no` that never expires keeps a language without
  // tree indentation for the life of the thread, so a query file dropped into
  // ~/.config/ronin/koi/queries under a running koi would stay invisible.
  //
  // On a thread of its own, because the memo it fills is thread_local and the
  // `yes` below would otherwise outlive the fixture that justifies it.
  OnAThreadOfItsOwn([] {
    const FakeQueryDir queries{"markdown"};
    EXPECT_TRUE(queries.Ready());
    if (!queries.Ready()) return;

    // Nothing there yet, and nothing remembered about it.
    EXPECT_FALSE(HasIndentQuery("markdown"));
    queries.Write("indents.scm", "((list) @indent)\n");
    EXPECT_TRUE(HasIndentQuery("markdown"));
    // And a `yes` is sticky, like every other runtime-path cache: the file
    // going away again does not un-answer it.
    queries.Forget();
    EXPECT_TRUE(HasIndentQuery("markdown"));
  });
  // The thread took the memo with it: this one never saw the file.
  EXPECT_FALSE(HasIndentQuery("markdown"));

  // -- an indents.scm that will not compile --------------------------------
  //
  // Run on a thread of its own: CompileQuery's cache and this file's
  // has-a-query memo are both thread_local, so the shadowing file below cannot
  // outlive the thread that saw it and poison the rest of the run.
  {
    const char* home = std::getenv("HOME");
    EXPECT_TRUE((home != nullptr) && (*home != '\0'));
    if ((home != nullptr) && (*home != '\0')) {
      const std::filesystem::path shadow =
          std::filesystem::path{home} / ".config" / "ronin" / "koi" / "queries" / "cmake";
      std::error_code ec;
      std::filesystem::create_directories(shadow, ec);
      WriteFixtureFile(shadow / "indents.scm", "(this is not a query");

      bool declined = false;
      std::string error;
      std::thread worker([&] {
        Fixture fixture = Open("CMakeLists.txt", "if(A)\n");
        if (!fixture.ok()) return;
        const auto got =
            TreeIndentForNewline(fixture.table, *fixture.syntax, 5, kFourSpaces, error);
        declined = !got.has_value();
      });
      worker.join();

      EXPECT_TRUE(declined);
      // Surfaced the way every other Captures failure is: a message naming the
      // query and the line it broke on.
      EXPECT_TRUE(error.find("cmake") != std::string::npos);
      EXPECT_TRUE(error.find("indents.scm") != std::string::npos);

      std::filesystem::remove(shadow / "indents.scm", ec);
      std::filesystem::remove(shadow, ec);
    }
  }

  // -- no syntax at all ----------------------------------------------------
  //
  // The command path, not the engine: a plain-text buffer never reaches
  // indent.cpp and gets exactly the bracket heuristic it always got.
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "fn() {\n");
    EXPECT_TRUE(ed.doc.syntax == nullptr);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{6, 6, -1}));
    RunCommands(ed, {"insert_newline"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("fn() {\n    \n"));
  }
  // Same buffer, same bytes, but with a grammar behind it -- the tree answers
  // and the answer is the same, which is what makes the fallback a fallback and
  // not a different editor.
  EXPECT_EQ(Pressed("a.cpp", "fn() {\n", 6), std::string("fn() {\n    \n"));

  // A language with a grammar and no indents.scm keeps the heuristic verbatim,
  // brackets and all.
  EXPECT_EQ(Pressed("a.md", "- [\n", 3), std::string("- [\n    \n"));
}

void TreeIndentNewlinePath() {
  TEST_CASE("tree indent newline path");

  // The design note's transcript, typed with auto-pairs on -- which is koi's
  // default, and which is why the closing delimiters are already in the buffer
  // by the time Return is pressed.
  //
  //   int main() {          Return -> one unit in, `}` left below at column 0
  //       work()            Return -> stays one unit in
  //       foo(a,            Return -> aligned under `a`
  //           b)            Return -> back to one unit: gap 2
  EXPECT_EQ(Pressed("a.cpp", "int main() {}", 12), std::string("int main() {\n    \n}"));
  EXPECT_EQ(Pressed("a.cpp", "int main() {\n    work()\n}", 23),
            std::string("int main() {\n    work()\n    \n}"));
  // Splitting the auto-paired `(` and `)`: an alignment is a column and not a
  // depth, so the caret does *not* go a level past it -- it goes where the next
  // argument goes, which is the same place a Return with no `)` in front of it
  // lands (the `kAlign` cases above). The `)` pushed onto its own line takes
  // that column too, because that is the answer the tree gives for a line
  // beginning with it, and a closer this code places must be one a later
  // keystroke on the same line will not move again.
  EXPECT_EQ(Pressed("a.cpp", "int main() {\n    foo(a,)\n}", 23),
            std::string("int main() {\n    foo(a,\n        \n        )\n}"));
  // With no alignment in play -- an empty argument list has no anchor to line up
  // under -- the closer keeps the level the tree gives it and the caret goes one
  // inside, exactly as a brace pair does.
  EXPECT_EQ(Pressed("a.cpp", "int main() {\n    foo();\n}", 21),
            std::string("int main() {\n    foo(\n        \n    );\n}"));
  EXPECT_EQ(Pressed("a.cpp", "int main() {\n    foo(a,\n        b)\n}", 34),
            std::string("int main() {\n    foo(a,\n        b)\n    \n}"));

  // The caret lands on the line it opened and not on the closer below it.
  {
    const std::array<Index, 1> carets{12};
    Editor ed = PressReturn("a.cpp", "int main() {}", carets);
    EXPECT_EQ(Cur(ed), Index{17});
  }

  // The brace-less body, through the real command. The buffer carries its `}`
  // because auto-pairs -- on by default -- put it there when the `{` was typed,
  // and that closer is what lets the parser finish the `if` with a body of no
  // width instead of giving up on the whole function.
  EXPECT_EQ(Pressed("a.cpp", "void g() {\n    if (x)\n}", 21),
            std::string("void g() {\n    if (x)\n        \n}"));

  // And typed, one key at a time, with auto-pairs on: the transcript from the
  // bug report, which used to leave `work();` a level short and then put
  // `next();` in column zero.
  //
  //   void g() {        Return -> one unit in, `}` below
  //       if (x)        Return -> two units: the body the `if` opens
  //           work();   Return -> back to one: the consequence ended at the `;`
  //       next();
  EXPECT_EQ(Typed("a.cpp", "", 0, "void g() {\nif (x)\nwork();\nnext();", true),
            std::string("void g() {\n    if (x)\n        work();\n    next();\n}"));

  // A brace inside a string still opens nothing, which the tree decides now and
  // the bracket scan decided before -- both have to keep saying so.
  EXPECT_EQ(Pressed("a.cpp", "  puts(\"{\");\n", 12), std::string("  puts(\"{\");\n  \n"));

  // python and yaml through the same path.
  EXPECT_EQ(Pressed("a.py", "def f():", 8), std::string("def f():\n    "));
  EXPECT_EQ(Pressed("a.py", "def f():\n    return x", 21), std::string("def f():\n    return x\n"));
  EXPECT_EQ(Pressed("a.yaml", "foo:", 4), std::string("foo:\n    "));

  // Two carets in one buffer: each gets the indent its own place in the tree
  // implies, and the whole thing is one undo step.
  {
    const std::string before = "int main() {\n    if (x) {\n        a();\n    }\n    b();\n}";
    // End of `if (x) {` (inside two blocks) and end of `b();` (inside one).
    const std::size_t inner = before.find("if (x) {") + 8;
    const std::size_t outer = before.find("b();") + 4;
    const std::array<Index, 2> carets{static_cast<Index>(inner), static_cast<Index>(outer)};
    Editor ed = PressReturn("a.cpp", before, carets);
    EXPECT_EQ(AssembleDocContents(ed.doc.table),
              std::string("int main() {\n    if (x) {\n        \n        a();\n    }\n    b();\n"
                          "    \n}"));
    EXPECT_EQ(UndoDepth(ed.doc.table), Index{1});
    EXPECT_FALSE(static_cast<bool>(Undo(ed.doc.table)));
    EXPECT_EQ(AssembleDocContents(ed.doc.table), before);
  }
}

void TreeIndentReindentOnType() {
  TEST_CASE("tree indent: re-indent on type");

  // Gap 1 from the design note, in the transcript's own words: with auto-pairs
  // off and no manual indentation, the `}` that closes a block used to land
  // wherever the line above put it.
  EXPECT_EQ(Typed("a.cpp", "int main() {\n    work()\n    ", 28, "}"),
            std::string("int main() {\n    work()\n}"));
  EXPECT_EQ(Typed("a.cpp", "int main() {\n    foo(a,\n        b)\n    ", 39, "}"),
            std::string("int main() {\n    foo(a,\n        b)\n}"));

  // `)` is the same rule and the same query line. The block is closed here
  // because it has to be: with the `}` still missing the whole body is one ERROR
  // node, every `@indent` in the C query is off it, and the tree's honest answer
  // for the `)` line is column zero. The engine declines nothing -- it answers a
  // question about a document that does not parse -- so this is a limit of the
  // grammar's recovery and not of the trigger. koi's own auto-pairs put the `}`
  // there, which is why it is only visible with them off.
  //
  // Where it lands is the list's alignment, not the statement's level: the `)`
  // closes a list whose lines all sit under `a`, and that column is one the
  // outdent is already part of. Typed at the column the previous line's Return
  // left the caret in, the answer is the column it is already in and nothing
  // moves.
  EXPECT_EQ(Typed("a.cpp", "int main() {\n    foo(a,\n        b,\n        \n}", 43, ")"),
            std::string("int main() {\n    foo(a,\n        b,\n        )\n}"));
  // Typed at column zero -- no auto-pairs, no manual indentation, which is gap 1
  // in the design note's own transcript -- it is pulled up to the anchor rather
  // than left where the keystroke put it. Zeroing the alignment's outdent is
  // what makes that answer worth acting on: before it, this line was rewritten
  // to a column (4) that nothing in the list occupies.
  EXPECT_EQ(Typed("a.cpp", "int main() {\n    foo(a,\n        b,\n\n}", 35, ")"),
            std::string("int main() {\n    foo(a,\n        b,\n        )\n}"));
  // The C parameter list from the audit's transcript, with the anchor further
  // along its line than any level count could reach.
  EXPECT_EQ(Typed("a.cpp", "void f(int a,\n       int b,\n\n", 28, ")"),
            std::string("void f(int a,\n       int b,\n       )\n"));
  // The keystroke after it finds the line where the first one left it and the
  // token still an outdent, so the answer is the column it is already in: the
  // memory of the move stands unused rather than springing the line back, which
  // is the `elsewhere` machinery behaving in a shape that has an alignment in it.
  EXPECT_EQ(Typed("a.cpp", "void f(int a,\n       int b,\n\n", 28, ");"),
            std::string("void f(int a,\n       int b,\n       );\n"));
  // python's anchor is its own, and its `)` is the same rule.
  EXPECT_EQ(Typed("a.py", "foo(a,\n    b,\n\n", 14, ")"), std::string("foo(a,\n    b,\n    )\n"));
  // And a closer already in the right column is not an edit at all: with
  // auto-pairs on the keystroke skips over it, and the re-indent that follows
  // computes the string the line already has, so there is nothing to undo.
  {
    Typist typist{"a.cpp", "int main() {\n    foo(a,\n        b,\n        )\n}", 43, true};
    typist.Type(")");
    EXPECT_EQ(typist.Text(), std::string("int main() {\n    foo(a,\n        b,\n        )\n}"));
    EXPECT_EQ(Cur(typist.ed), Index{44});
    EXPECT_EQ(UndoDepth(typist.ed.doc.table), Index{0});
  }

  // The same trigger over a half-typed brace-less body -- the shape the empty
  // consequence lives in. The `}` closes the function, not the `if`, and lands
  // at the function's column; the empty node between it and the caret is not a
  // token anyone typed and moves nothing.
  EXPECT_EQ(Typed("a.cpp", "int main() {\n    if (x)\n    ", 28, "}"),
            std::string("int main() {\n    if (x)\n}"));
  // And the statement that finally fills the body in is left where it was typed,
  // because its leading token outdents nothing -- the same promise as the
  // deliberately-over-indented line below, now with an empty node in view.
  EXPECT_EQ(Typed("a.cpp", "int main() {\n    if (x)\n        \n}", 32, "work;"),
            std::string("int main() {\n    if (x)\n        work;\n}"));

  // -- auto-pairs -----------------------------------------------------------
  //
  // The skip-over: a typed closer that matches the byte in front of the caret
  // moves the caret and inserts nothing. Re-indenting a `}` already in the right
  // column would compute the string it already has, and an identity edit is not
  // an edit -- the buffer is untouched and there is nothing to undo.
  {
    Typist typist{"a.cpp", "int main() {\n    work();\n}", 25, true};
    typist.Type("}");
    EXPECT_EQ(typist.Text(), std::string("int main() {\n    work();\n}"));
    EXPECT_EQ(Cur(typist.ed), Index{26});
    EXPECT_EQ(UndoDepth(typist.ed.doc.table), Index{0});
  }
  // The same skip-over onto a closer sitting in the wrong column does dedent it,
  // and the keystroke that inserted nothing is still one undo step.
  {
    Typist typist{"a.cpp", "int main() {\n    work();\n    }", 29, true};
    typist.Type("}");
    EXPECT_EQ(typist.Text(), std::string("int main() {\n    work();\n}"));
    EXPECT_EQ(UndoDepth(typist.ed.doc.table), Index{1});
    EXPECT_FALSE(static_cast<bool>(Undo(typist.ed.doc.table)));
    EXPECT_EQ(typist.Text(), std::string("int main() {\n    work();\n    }"));
  }

  // -- one undo step --------------------------------------------------------
  //
  // The dedent rides inside the keystroke's own UndoGroup: a typed `}` is one
  // undo, not a `}` and then a re-indent to walk back through.
  {
    Typist typist{"a.cpp", "int main() {\n    work()\n    ", 28, false};
    typist.Type("}");
    EXPECT_EQ(UndoDepth(typist.ed.doc.table), Index{1});
    EXPECT_FALSE(static_cast<bool>(Undo(typist.ed.doc.table)));
    EXPECT_EQ(typist.Text(), std::string("int main() {\n    work()\n    "));
  }

  // -- case / default / public: ---------------------------------------------
  //
  // Keyword outdents, which are the half of gap 1 that no bracket heuristic
  // could ever have reached.
  EXPECT_EQ(Typed("a.cpp", "int main() {\n    switch (x) {\n        \n    }\n}", 38, "case "),
            std::string("int main() {\n    switch (x) {\n    case \n    }\n}"));
  EXPECT_EQ(Typed("a.cpp", "int main() {\n    switch (x) {\n        \n    }\n}", 38, "default:"),
            std::string("int main() {\n    switch (x) {\n    default:\n    }\n}"));
  EXPECT_EQ(Typed("a.cpp", "class A {\n    \n    int x;\n};", 14, "public:"),
            std::string("class A {\npublic:\n    int x;\n};"));

  // -- python ---------------------------------------------------------------
  //
  // `(else_clause "else" @outdent)`: the `else:` comes back out to the `if`'s
  // column, and the body it was typed under keeps its own.
  EXPECT_EQ(Typed("a.py", "if x:\n    y = 1\n    \n    z = 2\n", 20, "else:"),
            std::string("if x:\n    y = 1\nelse:\n    z = 2\n"));
  // With nothing under it the grammar has no `else_clause` for that rule to
  // match at all -- the `if`'s `:`, the body and the `else` come back as one
  // ERROR -- so koi's own `(ERROR ":" "else" @outdent)` names the recovery and
  // the keystrokes land in the same column either way. What it must not do is
  // fire without the `:`, which is what keeps `elsewhere` where it was typed;
  // both are pinned in TreeIndentVendoredQueryAdditions.
  EXPECT_EQ(Typed("a.py", "if x:\n    y = 1\n    ", 20, "else:"),
            std::string("if x:\n    y = 1\nelse:"));

  // -- the `elsewhere` problem ----------------------------------------------
  //
  // bash outdents a bare `else`, so the fourth keystroke of `elsewhere` dedents a
  // line that the fifth must put back. Transient movement is fine; what is not
  // fine is a word left standing in a column nobody asked for. The line ends
  // exactly where it started -- restored to what was typed, not to what the tree
  // would have computed for it, because a line whose first token outdents
  // nothing is not this feature's business.
  {
    Typist typist{"a.sh", "if true; then\n    echo a\n    \nfi\n", 29, false};
    EXPECT_EQ(LeadingOf(typist.ed.doc.table, 2), std::string("    "));
    typist.Type("else");
    // The dedent really happened -- otherwise the restore below proves nothing.
    EXPECT_EQ(LeadingOf(typist.ed.doc.table, 2), std::string(""));
    typist.Type("where");
    EXPECT_EQ(LeadingOf(typist.ed.doc.table, 2), std::string("    "));
    EXPECT_EQ(typist.Text(), std::string("if true; then\n    echo a\n    elsewhere\nfi\n"));
    // Nine letters, fewer than nine undo steps, because consecutive typing
    // coalesces. A keystroke that *moved* the line ends the coalescing run --
    // its edit is at the head of the line rather than next to the letter that
    // caused it, and history only folds adjacent edits together -- so a word that
    // dedents and comes back costs a couple of steps more than one that does not.
    // That is the adjacency rule and not an extra edit: every move is still
    // inside the keystroke that caused it, which the single-`}` case above pins
    // down exactly.
    EXPECT_TRUE(UndoDepth(typist.ed.doc.table) < Index{9});
  }
  // The memory does not outlive the word: an `else` finished and followed by more
  // text keeps the column it earned rather than springing back the moment the
  // line stops being a single token.
  EXPECT_EQ(Typed("a.sh", "if true; then\n    echo a\n    \nfi\n", 29, "else\necho b"),
            std::string("if true; then\n    echo a\nelse\n    echo b\nfi\n"));

  // -- what the memory may not survive --------------------------------------
  //
  // A command -- any command, this one being the cheapest that leaves the caret
  // where it found it -- ends the run of typed graphemes the memory belongs to.
  // The `w` after it finds no memory, so the line keeps the column the `else`
  // earned instead of being pulled back to one nobody is still typing towards.
  {
    Typist typist{"a.sh", "if true; then\n    echo a\n    \nfi\n", 29, false};
    typist.Type("else");
    RunCommands(typist.ed, {"move_char_left", "move_char_right"});
    typist.Type("w");
    EXPECT_EQ(LeadingOf(typist.ed.doc.table, 2), std::string(""));
  }
  // And it is keyed on the document it was taken in, by identity rather than by
  // slot, so a buffer switch between two keystrokes drops it even when the caret
  // lands on a line that looks exactly the same.
  {
    Typist typist{"a.sh", "if true; then\n    echo a\n    \nfi\n", 29, false};
    typist.Type("else");
    typist.ed.doc.id = NextDocumentId();
    typist.Type("w");
    EXPECT_EQ(LeadingOf(typist.ed.doc.table, 2), std::string(""));
  }

  // -- never in the middle of a line ----------------------------------------
  //
  // The caret has code in front of it, so the line is not a single token run and
  // nothing about it is the indenter's to touch.
  EXPECT_EQ(Typed("a.cpp", "int main() {\n    int x = a\n}", 26, "}"),
            std::string("int main() {\n    int x = a}\n}"));
  // Nor is a line whose leading token outdents nothing, however much the tree
  // would like to disagree with the column it is in. Three levels of deliberate
  // indentation inside a one-level block survive being typed on.
  EXPECT_EQ(Typed("a.cpp", "int main() {\n            \n}", 25, "work;"),
            std::string("int main() {\n            work;\n}"));

  // -- only the keystroke that completes the token --------------------------
  //
  // A `}` deliberately parked eight columns in, and a space typed after it. The
  // trigger used to take any self-insert with the caret at *or past* the token's
  // end, so this re-ran the query and moved the line -- and because `}` never
  // stops being an outdent, the memory below could never bring it back: the
  // hand-placed column was gone until an undo. It stays where it was put.
  //
  // The caret starts exactly at the token's end here, which is the boundary the
  // caret rule is written on and the one worth pinning: the trigger is judged
  // after the insertion, so the space has already carried the caret one byte
  // past the `}` by the time it is asked, and the line is not a candidate. The
  // blank-keystroke test in FlushPendingAsText says the same thing earlier and
  // more cheaply; neither of them alone would let this move.
  EXPECT_EQ(Typed("a.cpp", "int main() {\n    work();\n        }\n", 34, " "),
            std::string("int main() {\n    work();\n        } \n"));
  // The second space, with the caret now well past the end. Held down, this used
  // to be a tree query per repeat as well as a line that would not stay put.
  EXPECT_EQ(Typed("a.cpp", "int main() {\n    work();\n        }\n", 34, "  "),
            std::string("int main() {\n    work();\n        }  \n"));
  // The audit's own transcript, whose caret was already past the token.
  EXPECT_EQ(Typed("a.cpp", "int main() {\n    work();\n        }  \n", 36, " "),
            std::string("int main() {\n    work();\n        }   \n"));
  // And the trigger is not dead on that line: the same caret typing a non-blank
  // that grows the token dedents it as it always did, which is what says the
  // space was refused for being blank and not for arriving late.
  EXPECT_EQ(Typed("a.cpp", "int main() {\n    work();\n        }\n", 34, ";"),
            std::string("int main() {\n    work();\n};\n"));

  // -- languages the feature does not exist in ------------------------------
  //
  // No syntax at all, and a grammar with no `indents.scm`: a typed `}` is a
  // typed `}`.
  EXPECT_EQ(Typed("", "    \n", 4, "}"), std::string("    }\n"));
  EXPECT_EQ(Typed("a.md", "    \n", 4, "}"), std::string("    }\n"));

  // -- multi-cursor ---------------------------------------------------------
  //
  // Two carets, two lines, one keystroke: both dedent, the offsets the first edit
  // moved are carried to the second, and the whole keystroke is one undo step.
  {
    const std::string before = "int main() {\n    a();\n    \nvoid g() {\n    b();\n    \n";
    Typist typist{"a.cpp", before, 0, false};
    std::vector<Selection> both{Selection{26, 26, -1}, Selection{51, 51, -1}};
    typist.ed.doc.selections.Replace(typist.ed.doc.table, both);
    typist.Type("}");
    EXPECT_EQ(typist.Text(), std::string("int main() {\n    a();\n}\nvoid g() {\n    b();\n}\n"));
    EXPECT_EQ(UndoDepth(typist.ed.doc.table), Index{1});
    EXPECT_FALSE(static_cast<bool>(Undo(typist.ed.doc.table)));
    EXPECT_EQ(typist.Text(), before);
  }
  // Two carets on *one* line, which is the case the usual back-to-front mapping
  // does not cover: the dedent happens at the head of the line, so the caret
  // earlier in the list is behind the later one but still after the bytes that
  // went away, and it has to move by the same four. Two closers with a caret in
  // front of each and auto-pairs skipping over both -- an over-indented `}}`
  // closing an `if` inside a function -- is the shape that still fires now that
  // only the caret exactly at the token's end qualifies: nothing is inserted,
  // the later caret lands on the end, and the earlier one is at the token's
  // first byte, which is the edge `CaretInsideIndent` deliberately admits.
  {
    Typist typist{"a.cpp", "int main() {\n    if (x) {\n        work();\n        }}\n", 0, true};
    std::vector<Selection> both{Selection{50, 50, -1}, Selection{51, 51, -1}};
    typist.ed.doc.selections.Replace(typist.ed.doc.table, both);
    typist.Type("}");
    EXPECT_EQ(typist.Text(),
              std::string("int main() {\n    if (x) {\n        work();\n    }}\n"));
    EXPECT_EQ(UndoDepth(typist.ed.doc.table), Index{1});
    // Both moved by the four bytes the dedent took, including the one earlier in
    // the list: mapping only the ranges past the firing caret would have left it
    // at 51.
    EXPECT_EQ(RangeTo(typist.ed.doc.selections, 0), Index{47});
    EXPECT_EQ(RangeTo(typist.ed.doc.selections, 1), Index{48});
  }
  // The same dedent, with a vertical motion on each side of it. A goal column is
  // the column a run of Up or Down aims at, and the dedent moved *both* carets
  // into new ones -- so the Up that follows has to use where they are now, not
  // where the line stood before the four spaces went away. The remap in
  // ReindentTypedLines used to clear the goal column of the caret owning the
  // keystroke alone; every other caret on the moved line kept a column four to
  // the right, and one Up jumped it there.
  //
  // It passes without that fix today, because both typing paths -- the auto-pairs
  // one and the batched plain inserts -- go through code that invalidates every
  // goal column before the re-indent is even asked for. That is a promise
  // made in selection.cpp about an edit this file did not make, which is exactly
  // what the rule in the remap no longer leans on. What is pinned here is the
  // answer, from whichever of the two says so.
  {
    Typist typist{"a.cpp", "int main() {\n    if (x) {\n        work();\n        }}\n", 0, true};
    std::vector<Selection> both{Selection{50, 50, -1}, Selection{51, 51, -1}};
    typist.ed.doc.selections.Replace(typist.ed.doc.table, both);
    // Up and back down, which is what leaves a goal column behind: columns 8 and
    // 9, the ones the carets stand in while the line is still over-indented.
    RunCommands(typist.ed, {"move_line_up", "move_line_down"});
    EXPECT_EQ(RangeTo(typist.ed.doc.selections, 0), Index{50});
    EXPECT_EQ(RangeTo(typist.ed.doc.selections, 1), Index{51});

    typist.Type("}");
    EXPECT_EQ(typist.Text(),
              std::string("int main() {\n    if (x) {\n        work();\n    }}\n"));
    EXPECT_EQ(RangeTo(typist.ed.doc.selections, 0), Index{47});
    EXPECT_EQ(RangeTo(typist.ed.doc.selections, 1), Index{48});

    // Columns 5 and 6 now -- the skip-over put both carets a byte further along
    // before the dedent took four spaces off the line -- so the line above,
    // `        work();` beginning at 26, takes them at 31 and 32. The columns
    // they stood in before the dedent were 9 and 10, which up there is 35 and 36.
    RunCommands(typist.ed, {"move_line_up"});
    EXPECT_EQ(RangeTo(typist.ed.doc.selections, 0), Index{31});
    EXPECT_EQ(RangeTo(typist.ed.doc.selections, 1), Index{32});
  }
  // Two carets in the blanks after the token, one of them starting exactly at
  // its end, each typing a space -- IND-M1 with more than one caret. Both used
  // to make the line dedent and neither does now. Two carets, four spaces, and a
  // line still standing where it was.
  {
    Typist typist{"a.cpp", "int main() {\n    work();\n    }  \n}", 0, false};
    std::vector<Selection> both{Selection{30, 30, -1}, Selection{32, 32, -1}};
    typist.ed.doc.selections.Replace(typist.ed.doc.table, both);
    typist.Type(" ");
    EXPECT_EQ(typist.Text(), std::string("int main() {\n    work();\n    }    \n}"));
    EXPECT_EQ(std::ssize(typist.ed.doc.selections.Ranges()), Index{2});
    EXPECT_EQ(RangeTo(typist.ed.doc.selections, 0), Index{31});
    EXPECT_EQ(RangeTo(typist.ed.doc.selections, 1), Index{34});
  }
  // -- carets inside the whitespace the dedent would replace ------------------
  //
  // Nine carets, one after each space of an over-indented `}` line and one past
  // the brace, each typing a space. The caret past the brace is a candidate for
  // the dedent and the other eight are standing in the bytes it would replace --
  // including the spaces they typed this very keystroke. Re-indenting through
  // them used to map all eight onto the head of the line, where Normalize merged
  // them into one and their input went with the whitespace: nine carets came
  // back as two. The line is left exactly as typed instead.
  {
    const std::string before = "int main() {\n    work();\n        }\n";
    const std::string after = "int main() {\n    work();\n                } \n";
    std::vector<Selection> carets;
    for (Index at = 26; at <= 34; ++at) carets.push_back(Selection{at, at, -1});
    {
      Typist typist{"a.cpp", before, 0, false};
      typist.ed.doc.selections.Replace(typist.ed.doc.table, carets);
      typist.Type(" ");
      EXPECT_EQ(typist.Text(), after);
      EXPECT_EQ(std::ssize(typist.ed.doc.selections.Ranges()), Index{9});
      EXPECT_EQ(RangeTo(typist.ed.doc.selections, 0), Index{27});
      EXPECT_EQ(RangeTo(typist.ed.doc.selections, 8), Index{43});
    }
    // The control: a language with no `indents.scm` has no re-indent to get
    // wrong, so what it leaves behind is what nine carets typing a space mean.
    {
      Typist typist{"a.md", before, 0, false};
      typist.ed.doc.selections.Replace(typist.ed.doc.table, carets);
      typist.Type(" ");
      EXPECT_EQ(typist.Text(), after);
      EXPECT_EQ(std::ssize(typist.ed.doc.selections.Ranges()), Index{9});
    }
  }
  // The same with three carets, which is the smallest shape that shows it: two
  // in the indent, one past the brace.
  {
    Typist typist{"a.cpp", "int main() {\n    work();\n        }\n", 0, false};
    std::vector<Selection> three{Selection{27, 27, -1}, Selection{29, 29, -1},
                                 Selection{34, 34, -1}};
    typist.ed.doc.selections.Replace(typist.ed.doc.table, three);
    typist.Type(" ");
    EXPECT_EQ(typist.Text(), std::string("int main() {\n    work();\n          } \n"));
    EXPECT_EQ(std::ssize(typist.ed.doc.selections.Ranges()), Index{3});
    EXPECT_EQ(RangeTo(typist.ed.doc.selections, 0), Index{28});
    EXPECT_EQ(RangeTo(typist.ed.doc.selections, 2), Index{37});
  }
}

// Every language koi ships an `indents.scm` for, on both paths the feature has:
// the newline, and the re-indent a typed outdent token triggers. Each table was
// written against its own vendored query -- what a case expects is what that
// file's patterns claim, not what a C programmer would guess -- and every case
// that comes out somewhere the query does not justify carries a `KNOWN` in its
// name and the reason next to it.
void TreeIndentLanguageMatrix() {
  TEST_CASE("tree indent: every vendored language");

  // -- bash ----------------------------------------------------------------
  //
  // `[(function_definition) (if_statement) (for_statement) (while_statement)
  // (case_statement) (case_item) (pipeline)] @indent` and a six-token
  // `@outdent` list. No `scope header` anywhere, and none is needed: bash's
  // bodies are nodes that span from the header to the closing keyword, so
  // containment reaches them without a descent.
  static constexpr std::array kBash{
      IndentCase{"if/then opens the body", "a.sh", "if true; then", 13, "    "},
      IndentCase{"and keeps it open with fi below", "a.sh", "if true; then\nfi\n", 13, "    "},
      IndentCase{"a statement in the body stays at its depth", "a.sh",
                 "if true; then\n    echo a\nfi\n", 24, "    "},
      IndentCase{"elif opens a body of its own", "a.sh", "if a; then\n    b\nelif c; then\nfi\n",
                 29, "    "},
      IndentCase{"for/do", "a.sh", "for i in 1 2; do\ndone\n", 16, "    "},
      IndentCase{"while/do", "a.sh", "while true; do\ndone\n", 14, "    "},
      IndentCase{"a function body", "a.sh", "f() {\n}\n", 5, "    "},
      // Without the `}` the grammar has no `function_definition` to recover
      // into and the whole line is an ERROR that bash's query names nowhere.
      // koi's auto-pairs put the closer there, which is why this is only
      // reachable with them off.
      IndentCase{"a function whose closer is not there yet", "a.sh", "f() {", 5, ""},
      IndentCase{"case ... in", "a.sh", "case $x in\nesac\n", 10, "    "},
      IndentCase{"a case item", "a.sh", "case $x in\n    a)\nesac\n", 17, "    "},
      IndentCase{"the body of a case item", "a.sh",
                 "case $x in\n    a)\n        echo a\n        ;;\nesac\n", 32, "        "},
      // `(subshell)` is not in bash's indent list, so it opens nothing.
      IndentCase{"a subshell opens nothing", "a.sh", "(\n)\n", 1, ""},
      IndentCase{"an empty buffer", "a.sh", "", 0, ""},
      IndentCase{"pos 0", "a.sh", "if true; then\n    echo a\nfi\n", 0, ""},
      IndentCase{"pos at eof", "a.sh", "if true; then\n    echo a\nfi\n", 28, ""},
  };
  RunNewlineCases(kBash);

  static constexpr std::array kBashLines{
      IndentCase{"the body of an if", "a.sh", "if true; then\n    echo a\nfi\n", 1, "    "},
      IndentCase{"fi comes back to the if's column", "a.sh", "if true; then\n    echo a\n    fi\n",
                 2, ""},
      IndentCase{"done does too", "a.sh", "for i in 1 2; do\n    echo a\n    done\n", 2, ""},
      IndentCase{"esac does too", "a.sh",
                 "case $x in\n    a)\n        echo a\n        ;;\n    esac\n", 4, ""},
      IndentCase{"else", "a.sh", "if true; then\n    echo a\n    else\n    echo b\nfi\n", 2, ""},
      IndentCase{"elif", "a.sh",
                 "if true; then\n    echo a\n    elif false; then\n    echo b\nfi\n", 2, ""},
      IndentCase{"a lone closer at the head of a line", "a.sh", "f() {\n    echo a\n    }\n", 2,
                 ""},
  };
  RunLineCases(kBashLines);

  EXPECT_EQ(Typed("a.sh", "if true; then\n    echo a\n    \n", 29, "fi"),
            std::string("if true; then\n    echo a\nfi\n"));
  EXPECT_EQ(Typed("a.sh", "for i in 1 2; do\n    echo a\n    \n", 32, "done"),
            std::string("for i in 1 2; do\n    echo a\ndone\n"));
  EXPECT_EQ(Typed("a.sh", "case $x in\n    a)\n        echo a\n        ;;\n    \n", 48, "esac"),
            std::string("case $x in\n    a)\n        echo a\n        ;;\nesac\n"));
  EXPECT_EQ(Typed("a.sh", "f() {\n    echo a\n    \n", 21, "}"),
            std::string("f() {\n    echo a\n}\n"));

  // -- cmake ---------------------------------------------------------------
  //
  // Seven node kinds `@indent` -- the conditions, the loops, the definitions,
  // and `normal_command`, which is what wraps an argument list onto its own
  // lines -- against `")"` plus the eight `end*` / `else` / `elseif` nodes
  // `@outdent`.
  static constexpr std::array kCMake{
      IndentCase{"if(...) opens a body", "CMakeLists.txt", "if(A)\nendif()\n", 5, "    "},
      // No `endif()` and there is no `if_condition` to be inside: the grammar
      // recovers, cmake's query names nothing in the recovery, and the honest
      // answer is column zero.
      IndentCase{"with no endif() the recovery gives nothing", "CMakeLists.txt", "if(A)", 5, ""},
      IndentCase{"function(...)", "CMakeLists.txt", "function(f)\nendfunction()\n", 11, "    "},
      IndentCase{"foreach(...)", "CMakeLists.txt", "foreach(x a b)\nendforeach()\n", 14, "    "},
      IndentCase{"while(...)", "CMakeLists.txt", "while(A)\nendwhile()\n", 8, "    "},
      IndentCase{"macro(...)", "CMakeLists.txt", "macro(m)\nendmacro()\n", 8, "    "},
      IndentCase{"a command whose arguments wrap", "CMakeLists.txt", "add_library(a\n)\n", 13,
                 "    "},
      IndentCase{"a statement inside a body", "CMakeLists.txt", "if(A)\n    set(X 1)\nendif()\n",
                 18, "    "},
      IndentCase{"nested ifs stack", "CMakeLists.txt", "if(A)\n    if(B)\n    endif()\nendif()\n",
                 15, "        "},
      IndentCase{"an empty buffer", "CMakeLists.txt", "", 0, ""},
      IndentCase{"pos at eof", "CMakeLists.txt", "if(A)\n    set(X 1)\nendif()\n", 27, ""},
  };
  RunNewlineCases(kCMake);

  static constexpr std::array kCMakeLines{
      IndentCase{"the body of an if", "CMakeLists.txt", "if(A)\n    set(X 1)\nendif()\n", 1,
                 "    "},
      IndentCase{"endif()", "CMakeLists.txt", "if(A)\n    set(X 1)\n    endif()\n", 2, ""},
      IndentCase{"endfunction()", "CMakeLists.txt",
                 "function(f)\n    set(X 1)\n    endfunction()\n", 2, ""},
      IndentCase{"else()", "CMakeLists.txt",
                 "if(A)\n    set(X 1)\n    else()\n    set(X 2)\nendif()\n", 2, ""},
      IndentCase{"a nested endif() comes back one level, not to zero", "CMakeLists.txt",
                 "if(A)\n    if(B)\n    endif()\nendif()\n", 2, "    "},
      IndentCase{"a lone ) at the head of a line", "CMakeLists.txt",
                 "add_library(a\n    b\n    )\n", 2, ""},
  };
  RunLineCases(kCMakeLines);

  EXPECT_EQ(Typed("CMakeLists.txt", "if(A)\n    set(X 1)\n    \n", 23, "endif()"),
            std::string("if(A)\n    set(X 1)\nendif()\n"));
  EXPECT_EQ(Typed("CMakeLists.txt", "function(f)\n    set(X 1)\n    \n", 29, "endfunction()"),
            std::string("function(f)\n    set(X 1)\nendfunction()\n"));
  EXPECT_EQ(Typed("CMakeLists.txt", "if(A)\n    set(X 1)\n    \nendif()\n", 23, "else()"),
            std::string("if(A)\n    set(X 1)\nelse()\nendif()\n"));

  // -- css -----------------------------------------------------------------
  //
  // The whole query is `(block) @indent` and `"}" @outdent`, and it is enough.
  static constexpr std::array kCss{
      IndentCase{"a rule block", "a.css", "a {\n}\n", 3, "    "},
      IndentCase{"a rule block with no closer yet", "a.css", "a {", 3, "    "},
      IndentCase{"a declaration inside one", "a.css", "a {\n    color: red;\n}\n", 19, "    "},
      IndentCase{"a rule inside a media query", "a.css", "@media screen {\n    a {\n    }\n}\n", 23,
                 "        "},
      IndentCase{"an empty buffer", "a.css", "", 0, ""},
      IndentCase{"pos at eof", "a.css", "a {\n    color: red;\n}\n", 22, ""},
  };
  RunNewlineCases(kCss);

  static constexpr std::array kCssLines{
      IndentCase{"a declaration", "a.css", "a {\n    color: red;\n}\n", 1, "    "},
      IndentCase{"the closing brace", "a.css", "a {\n    color: red;\n    }\n", 2, ""},
      IndentCase{"the closing brace of a media query", "a.css",
                 "@media screen {\n    a {\n    }\n    }\n", 3, ""},
  };
  RunLineCases(kCssLines);

  EXPECT_EQ(Typed("a.css", "a {\n    color: red;\n    \n", 24, "}"),
            std::string("a {\n    color: red;\n}\n"));

  // -- dart ----------------------------------------------------------------
  //
  // A long bracketed-scope list, the four brace-less body rules written with
  // unquoted `(#not-kind-eq? @indent block)` -- the predicate parser reads a
  // bare word exactly as it reads a quoted one, which is what keeps a braced
  // body from being counted twice -- and `(string_literal) @opaque`.
  static constexpr std::array kDart{
      IndentCase{"a class body", "a.dart", "class A {\n}\n", 9, "    "},
      IndentCase{"a method body inside it", "a.dart", "class A {\n  void f() {\n  }\n}\n", 22,
                 "      "},
      IndentCase{"a braced if body", "a.dart", "void f() {\n  if (x) {\n  }\n}\n", 21, "      "},
      // dart's brace-less rules carry no `(#set! "scope" "header")`, so under
      // containment the body's scope opens on the line it starts on and the
      // newline after the header gets nothing from it. The rule still works
      // for a body already written -- the line case below -- which is the half
      // helix's `tail` scope was aimed at.
      IndentCase{"a brace-less if body -- this query has no scope header", "a.dart",
                 "void f() {\n  if (x)\n}\n", 19, "  "},
      IndentCase{"a list literal", "a.dart", "var a = [\n];\n", 9, "    "},
      IndentCase{"an argument list", "a.dart", "void f() {\n  g(\n  );\n}\n", 15, "      "},
      IndentCase{"a switch block", "a.dart", "void f() {\n  switch (x) {\n  }\n}\n", 25, "      "},
      IndentCase{"a case label", "a.dart", "void f() {\n  switch (x) {\n    case 1:\n  }\n}\n", 37,
                 "    "},
      IndentCase{"an enum body", "a.dart", "enum E {\n}\n", 8, "    "},
      IndentCase{"an empty buffer", "a.dart", "", 0, ""},
      IndentCase{"pos at eof", "a.dart", "class A {\n  int x = 1;\n}\n", 25, ""},
  };
  RunNewlineCases(kDart);

  static constexpr std::array kDartLines{
      IndentCase{"a braced if body -- #not-kind-eq? block keeps it to one level", "a.dart",
                 "void f() {\n  if (x) {\n  a();\n  }\n}\n", 2, "        "},
      IndentCase{"a brace-less if body", "a.dart", "void f() {\n  if (x)\n  a();\n}\n", 2, "    "},
      IndentCase{"a case label sits inside the switch block", "a.dart",
                 "void f() {\n  switch (x) {\ncase 1:\n  }\n}\n", 2, "        "},
      IndentCase{"the closing brace", "a.dart", "class A {\n  int x = 1;\n  }\n", 2, ""},
  };
  RunLineCases(kDartLines);

  EXPECT_EQ(Typed("a.dart", "class A {\n  int x = 1;\n  \n", 25, "}"),
            std::string("class A {\n  int x = 1;\n}\n"));

  // -- go ------------------------------------------------------------------
  //
  // The one vendored file with ERROR patterns of its own -- `(ERROR "{")
  // @indent @extend` and its `(` and `[` siblings, which are what typing with
  // auto-pairs off leaves behind -- plus the `#not-kind-eq?` guard that keeps
  // a switch's own `}` from outdenting and a `(label_name) @outdent` that
  // pulls `Loop:` back the way gofmt writes it.
  static constexpr std::array kGo{
      IndentCase{"a func body", "a.go", "func main() {\n}\n", 13, "    "},
      IndentCase{"a func body whose closer is not there -- (ERROR \"{\")", "a.go",
                 "func main() {", 13, "    "},
      IndentCase{"a struct type", "a.go", "type T struct {\n}\n", 15, "    "},
      IndentCase{"a var block", "a.go", "var (\n)\n", 5, "    "},
      IndentCase{"an import block", "a.go", "import (\n)\n", 8, "    "},
      IndentCase{"a composite literal", "a.go", "var a = T{\n}\n", 10, "    "},
      IndentCase{"an empty buffer", "a.go", "", 0, ""},
      IndentCase{"pos at eof", "a.go", "package main\n", 13, ""},
  };
  RunNewlineCases(kGo);

  // Go is written with tabs, so the switch and struct cases are measured in
  // them: a level is one tab and the answers below say so.
  EXPECT_EQ(ForNewline("a.go", "type T struct {\n\tA int\n}\n", 22, kTabs), std::string("\t"));
  EXPECT_EQ(ForNewline("a.go", "func f() {\n\tswitch x {\n\t}\n}\n", 22, kTabs), std::string("\t"));
  EXPECT_EQ(ForNewline("a.go", "func f() {\n\tswitch x {\n\tcase 1:\n\t}\n}\n", 31, kTabs),
            std::string("\t"));
  EXPECT_EQ(ForNewline("a.go", "func f() {\n\tswitch x {\n\tcase 1:\n\t\ta()\n\t}\n}\n", 37, kTabs),
            std::string("\t\t"));
  EXPECT_EQ(ForNewline("a.go", "func f() {\n\tg(\n}\n", 14, kTabs), std::string("\t"));

  static constexpr std::array kGoLines{
      IndentCase{"a case label aligns with its switch, as gofmt writes it", "a.go",
                 "func f() {\n\tswitch x {\n\t\tcase 1:\n\t\ta()\n\t}\n}\n", 2, "    "},
      IndentCase{"a label is pulled out a level", "a.go",
                 "func f() {\n\tLoop:\n\tfor {\n\t}\n}\n", 1, ""},
      IndentCase{"the closing brace of a func", "a.go", "func main() {\n\ta()\n\t}\n", 2, ""},
      IndentCase{"the ) closing an import block", "a.go", "import (\n\t\"a\"\n\t)\n", 2, ""},
  };
  RunLineCases(kGoLines);
  // The brace that closes a *switch* is the one the `#not-kind-eq?` triple
  // exempts: it stays at the switch's own depth instead of being pulled out.
  EXPECT_EQ(ForLine("a.go", "func f() {\n\tswitch x {\n\tcase 1:\n\t\ta()\n\t}\n}\n", 4, kTabs),
            std::string("\t"));

  EXPECT_EQ(Typed("a.go", "func main() {\n\ta()\n\t\n", 20, "}"),
            std::string("func main() {\n\ta()\n}\n"));

  // -- html ----------------------------------------------------------------
  static constexpr std::array kHtml{
      IndentCase{"an element's contents", "a.html", "<div>\n</div>\n", 5, "    "},
      IndentCase{"nested elements stack", "a.html", "<div>\n    <p>\n    </p>\n</div>\n", 13,
                 "        "},
      IndentCase{"a completed child does not", "a.html", "<div>\n    <p>a</p>\n</div>\n", 18,
                 "    "},
      IndentCase{"attributes wrapped onto their own lines", "a.html",
                 "<div\n    id=\"x\"\n>\n</div>\n", 15, "    "},
      IndentCase{"a script element's own tag line", "a.html", "<script>\n</script>\n", 8, "    "},
      IndentCase{"an empty buffer", "a.html", "", 0, ""},
      // KNOWN: tree-sitter-html gives a void element no end tag, so `<br>`
      // parses as an `element` that runs to the end of its parent and the
      // `(element) @indent` rule opens a scope over everything after it. The
      // vendored file's own comment says void elements are single-line
      // `element`s and the tail scope is a no-op for them; that is not what
      // this grammar produces. Nothing in koi is wrong here -- the expectation
      // below is the misindent, pinned so a grammar bump that fixes it shows up
      // as a failure rather than as silence.
      IndentCase{"KNOWN a void element opens a scope it should not", "a.html",
                 "<div>\n    <br>\n</div>\n", 14, "        "},
  };
  RunNewlineCases(kHtml);

  static constexpr std::array kHtmlLines{
      IndentCase{"a child element", "a.html", "<div>\n<p>a</p>\n</div>\n", 1, "    "},
      IndentCase{"an end tag", "a.html", "<div>\n    <p>a</p>\n    </div>\n", 2, ""},
      IndentCase{"the > that closes a wrapped start tag", "a.html",
                 "<div\n    id=\"x\"\n    >\n</div>\n", 2, ""},
      IndentCase{"KNOWN and the sibling under a void element is pulled into it", "a.html",
                 "<div>\n    <br>\n<p>x</p>\n</div>\n", 2, "        "},
  };
  RunLineCases(kHtmlLines);

  EXPECT_EQ(Typed("a.html", "<div>\n    <p>a</p>\n    \n", 23, "</div>"),
            std::string("<div>\n    <p>a</p>\n</div>\n"));

  // The `<script>` body is JavaScript, and html's query is the wrong one for
  // it. The engine declines rather than indenting JS by html's rules -- and the
  // caller's bracket heuristic answers instead.
  //
  // What knows a byte is inside an injected region is the layer Paint parses,
  // so the decline needs the buffer to have been painted once. In the editor
  // that is every frame; here it has to be asked for, and the pair of
  // expectations below is what that dependency looks like from the outside.
  {
    const std::string script = "<script>\n    var x = {\n    }\n</script>\n";
    Fixture fixture = Open("a.html", script);
    EXPECT_TRUE(fixture.ok());
    if (fixture.ok()) {
      std::string error;
      // Before a paint nothing knows the region is there, and html's own query
      // answers for it.
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, 22) == Injected::kNo);
      EXPECT_TRUE(
          TreeIndentForNewline(fixture.table, *fixture.syntax, 22, kFourSpaces, error).has_value());

      std::vector<CaptureId> painted;
      fixture.syntax->Paint(fixture.table, Interval(0, DocLength(fixture.table)), painted);
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, 22) == Injected::kYes);
      EXPECT_FALSE(
          TreeIndentForNewline(fixture.table, *fixture.syntax, 22, kFourSpaces, error).has_value());
      EXPECT_EQ(error, std::string{});
      // The host document either side of the region still gets an answer.
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, 0) == Injected::kNo);
      EXPECT_TRUE(
          TreeIndentForNewline(fixture.table, *fixture.syntax, 30, kFourSpaces, error).has_value());
    }
  }
  {
    const std::string style = "<style>\n    a {\n    }\n</style>\n";
    Fixture fixture = Open("a.html", style);
    EXPECT_TRUE(fixture.ok());
    if (fixture.ok()) {
      std::vector<CaptureId> painted;
      fixture.syntax->Paint(fixture.table, Interval(0, DocLength(fixture.table)), painted);
      std::string error;
      EXPECT_FALSE(
          TreeIndentForNewline(fixture.table, *fixture.syntax, 15, kFourSpaces, error).has_value());
    }
  }

  // -- javascript / typescript / tsx ---------------------------------------
  //
  // One `ecma` file behind all three, `_typescript` adding the type
  // declarations and `_jsx` the element rules. `.jsx` is *javascript*, whose
  // inherits line is `_javascript,ecma` and does not reach `_jsx` -- helix
  // spells it that way and koi vendored it unchanged, so a jsx element in a
  // `.js` file gets the parenthesised-expression level and nothing more.
  static constexpr std::array kEcma{
      IndentCase{"js: a function body", "a.js", "function f() {\n}\n", 14, "    "},
      IndentCase{"js: an object literal", "a.js", "const o = {\n};\n", 11, "    "},
      IndentCase{"js: an array literal", "a.js", "const a = [\n];\n", 11, "    "},
      IndentCase{"js: an arrow function's block body", "a.js", "const f = () => {\n};\n", 17,
                 "    "},
      IndentCase{"js: an argument list", "a.js", "f(\n);\n", 2, "    "},
      IndentCase{"js: a brace-less if body -- scope header", "a.js",
                 "function f() {\n    if (x)\n}\n", 25, "        "},
      IndentCase{"js: a class body", "a.js", "class A {\n}\n", 9, "    "},
      IndentCase{"js: a nested object", "a.js", "const o = {\n    a: {\n    },\n};\n", 20,
                 "        "},
      IndentCase{"js: a parenthesised return value", "a.js",
                 "function f() {\n    return (\n    );\n}\n", 27, "        "},
      IndentCase{"js: a case label with a body under it", "a.js",
                 "switch (x) {\n    case 1:\n        a();\n}\n", 24, "        "},
      // With nothing under it the `switch_case` node ends at the label, so the
      // line being opened is past it and only the switch's own level is left.
      IndentCase{"js: a case label with nothing under it yet", "a.js",
                 "switch (x) {\n    case 1:\n}\n", 24, "    "},
      IndentCase{"js: an empty buffer", "a.js", "", 0, ""},
      IndentCase{"js: pos at eof", "a.js", "function f() {\n    a();\n}\n", 26, ""},
      IndentCase{"ts: an interface body", "a.ts", "interface I {\n}\n", 13, "    "},
      IndentCase{"ts: an enum body", "a.ts", "enum E {\n}\n", 8, "    "},
      IndentCase{"ts: an object type", "a.ts", "type T = {\n};\n", 10, "    "},
      IndentCase{"ts: a function body", "a.ts", "function f(): void {\n}\n", 20, "    "},
      IndentCase{"ts: an empty buffer", "a.ts", "", 0, ""},
      IndentCase{"tsx: a jsx element's children", "a.tsx",
                 "const a = (\n    <div>\n    </div>\n);\n", 21, "        "},
      IndentCase{"tsx: a completed child element", "a.tsx",
                 "const a = (\n    <div>\n        <p>x</p>\n    </div>\n);\n", 38, "        "},
      IndentCase{"tsx: a parenthesised expression", "a.tsx", "const a = (\n);\n", 11, "    "},
      IndentCase{"tsx: attributes of a self-closing tag", "a.tsx",
                 "const a = (\n    <div\n        id=\"x\"\n    />\n);\n", 35, "        "},
      IndentCase{"jsx in a .js file gets ecma's rules and not _jsx's", "a.jsx",
                 "const a = (\n    <div>\n    </div>\n);\n", 21, "    "},
      // KNOWN: the line after the last statement of a `case`. `switch_case`
      // ends at that statement, so under containment the new line is outside it
      // and drops back to the switch's level -- a second statement in the same
      // case has to be re-indented by hand. C and go do not have this: their
      // case bodies live inside a block that spans past the caret, which the
      // two expectations below are here to show. It is the engine's containment
      // model and not the ecma query, so it is not a `.scm` fix.
      IndentCase{"KNOWN js: the line after a case's last statement leaves the case", "a.js",
                 "switch (x) {\n    case 1:\n        a();\n}\n", 37, "    "},
      IndentCase{"and C, whose case body is inside a compound_statement, keeps it", "a.cpp",
                 "int main() {\n    switch (x) {\n    case 1:\n        a();\n    }\n}\n", 54,
                 "        "},
  };
  RunNewlineCases(kEcma);
  EXPECT_EQ(ForNewline("a.go", "func f() {\n\tswitch x {\n\tcase 1:\n\t\ta()\n\t}\n}\n", 37, kTabs),
            std::string("\t\t"));

  static constexpr std::array kEcmaLines{
      IndentCase{"js: the closing brace of a function", "a.js", "function f() {\n    a();\n    }\n",
                 2, ""},
      IndentCase{"js: the closing brace of an object", "a.js",
                 "const o = {\n    a: 1,\n    };\n", 2, ""},
      IndentCase{"js: a case label sits one level inside its switch", "a.js",
                 "switch (x) {\ncase 1:\n    a();\n}\n", 1, "    "},
      IndentCase{"js: and the statement under it one level inside that", "a.js",
                 "switch (x) {\n    case 1:\na();\n}\n", 2, "        "},
      IndentCase{"ts: the closing brace of an interface", "a.ts",
                 "interface I {\n    a: number;\n    }\n", 2, ""},
      IndentCase{"tsx: a closing element", "a.tsx",
                 "const a = (\n    <div>\n        <p>x</p>\n        </div>\n);\n", 3, "    "},
  };
  RunLineCases(kEcmaLines);

  EXPECT_EQ(Typed("a.js", "function f() {\n    a();\n    \n", 28, "}"),
            std::string("function f() {\n    a();\n}\n"));
  EXPECT_EQ(Typed("a.ts", "interface I {\n    a: number;\n    \n", 33, "}"),
            std::string("interface I {\n    a: number;\n}\n"));
  EXPECT_EQ(
      Typed("a.tsx", "const a = (\n    <div>\n        <p>x</p>\n        \n);\n", 47, "</div>"),
      std::string("const a = (\n    <div>\n        <p>x</p>\n    </div>\n);\n"));

  // -- make ----------------------------------------------------------------
  //
  // `[(define_directive) (rule)] @indent` and `"endef" @outdent`. A recipe line
  // has to begin with a tab or make will not run it, so these are measured with
  // the tabs style -- which is what `DetectIndentation` sets for any Makefile
  // that already has a recipe in it.
  static constexpr std::array kMakeTabs{
      IndentCase{"a rule opens its recipe", "Makefile", "all:\n", 4, "\t"},
      IndentCase{"and a recipe line keeps the tab it is written with", "Makefile",
                 "all:\n\techo hi\n", 13, "\t"},
      IndentCase{"a second recipe line", "Makefile", "all: dep\n\t@echo hi\n", 18, "\t"},
      IndentCase{"a define directive", "Makefile", "define X\nendef\n", 8, "\t"},
      IndentCase{"a variable assignment opens nothing", "Makefile", "X = 1\n", 5, ""},
      IndentCase{"an empty buffer", "Makefile", "", 0, ""},
  };
  for (const IndentCase& one : kMakeTabs) {
    const std::string got = ForNewline(one.file, one.text, one.cursor, kTabs);
    if (got != one.want) std::cerr << "  case: " << one.what << "\n";
    EXPECT_EQ(got, std::string{one.want});
  }
  EXPECT_EQ(ForLine("Makefile", "all:\n\techo hi\n", 1, kTabs), std::string("\t"));
  EXPECT_EQ(ForLine("Makefile", "define X\n\ta\n\tendef\n", 2, kTabs), std::string(""));
  // KNOWN: the level is spelled by the document's style and the query has no
  // say in it, so an empty Makefile -- nothing for `DetectIndentation` to read
  // -- opens its first recipe with spaces, which make will not accept. Nothing
  // in indent.cpp can fix this; the style would have to be forced per language.
  EXPECT_EQ(ForNewline("Makefile", "all:\n", 4, kFourSpaces), std::string("    "));

  // -- nix -----------------------------------------------------------------
  static constexpr std::array kNix{
      IndentCase{"an attrset", "a.nix", "{\n}\n", 1, "    "},
      IndentCase{"a binding inside one", "a.nix", "{\n  a = 1;\n}\n", 10, "  "},
      IndentCase{"a nested attrset", "a.nix", "{\n  a = {\n  };\n}\n", 9, "      "},
      IndentCase{"let", "a.nix", "let\nin 1\n", 3, "    "},
      IndentCase{"a binding inside a let", "a.nix", "let\n  a = 1;\nin a\n", 12, "  "},
      IndentCase{"the body after in", "a.nix", "let\n  a = 1;\nin\n", 15, "    "},
      IndentCase{"a list", "a.nix", "[\n]\n", 1, "    "},
      IndentCase{"a function's formals", "a.nix", "{ pkgs,\n}: 1\n", 7, "    "},
      IndentCase{"a binding whose value spills onto the next line", "a.nix", "{\n  a =\n}\n", 7,
                 "  "},
      IndentCase{"an if branch -- scope header", "a.nix", "if a then\n  b\nelse c\n", 9, "    "},
      IndentCase{"function application on one line opens nothing", "a.nix", "f a\n", 3, ""},
      IndentCase{"an empty buffer", "a.nix", "", 0, ""},
  };
  RunNewlineCases(kNix);

  static constexpr std::array kNixLines{
      IndentCase{"a binding", "a.nix", "{\na = 1;\n}\n", 1, "    "},
      IndentCase{"the closing brace", "a.nix", "{\n  a = 1;\n  }\n", 2, ""},
      IndentCase{"in comes back out of the bindings", "a.nix", "let\n  a = 1;\n  in a\n", 2, ""},
      // `(indented_string_expression) @opaque`: whatever is between the `''`s
      // is content, and it is handed back exactly as written.
      IndentCase{"the interior of an indented string is left as written", "a.nix",
                 "{\n  a = ''\n     text\n  '';\n}\n", 2, "     "},
  };
  RunLineCases(kNixLines);

  EXPECT_EQ(Typed("a.nix", "{\n  a = 1;\n  \n", 13, "}"), std::string("{\n  a = 1;\n}\n"));

  // -- toml ----------------------------------------------------------------
  //
  // Only `(array)` and `(inline_table)` indent: a table body stays at column
  // zero, which is what the vendored file says and what every hand-written
  // toml does.
  static constexpr std::array kToml{
      IndentCase{"a table header keeps its body flat", "a.toml", "[a]\n", 3, ""},
      IndentCase{"and so does a pair", "a.toml", "[a]\nb = 1\n", 9, ""},
      IndentCase{"an array of tables too", "a.toml", "[[a]]\n", 5, ""},
      IndentCase{"a multi-line array does indent", "a.toml", "b = [\n]\n", 5, "    "},
      IndentCase{"an item inside one", "a.toml", "b = [\n    1,\n]\n", 12, "    "},
      IndentCase{"a nested array", "a.toml", "a = [\n    [\n    ],\n]\n", 11, "        "},
      IndentCase{"an inline table", "a.toml", "b = {\n}\n", 5, "    "},
      IndentCase{"an empty buffer", "a.toml", "", 0, ""},
  };
  RunNewlineCases(kToml);

  static constexpr std::array kTomlLines{
      IndentCase{"a table header is at column zero however it was written", "a.toml", "    [a]\n",
                 0, ""},
      IndentCase{"an array item", "a.toml", "b = [\n1,\n]\n", 1, "    "},
      IndentCase{"the ] that closes an array", "a.toml", "b = [\n    1,\n    ]\n", 2, ""},
  };
  RunLineCases(kTomlLines);

  EXPECT_EQ(Typed("a.toml", "b = [\n    1,\n    \n", 17, "]"), std::string("b = [\n    1,\n]\n"));

  // -- json ----------------------------------------------------------------
  static constexpr std::array kJson{
      IndentCase{"an object", "a.json", "{\n}\n", 1, "    "},
      IndentCase{"an array", "a.json", "[\n]\n", 1, "    "},
      IndentCase{"an array inside an object", "a.json", "{\n    \"a\": [\n    ]\n}\n", 12,
                 "        "},
      IndentCase{"an empty buffer", "a.json", "", 0, ""},
  };
  RunNewlineCases(kJson);
  EXPECT_EQ(ForLine("a.json", "{\n    \"a\": 1\n    }\n", 2), std::string(""));
  EXPECT_EQ(Typed("a.json", "{\n    \"a\": 1\n    \n", 17, "}"),
            std::string("{\n    \"a\": 1\n}\n"));

  // -- the two ends of a buffer, in every language -------------------------
  //
  // A caret before the first byte and one past the last are the two positions
  // no query pattern is written for, and the two the engine has to clamp
  // rather than index with. Nothing contains either of them, so the answer is
  // column zero and the only way to fail is to crash.
  static constexpr std::array kBoundaries{
      IndentCase{"bash at pos 0", "a.sh", "if true; then\nfi\n", 0, ""},
      IndentCase{"bash at eof", "a.sh", "if true; then\nfi\n", 17, ""},
      IndentCase{"cmake at pos 0", "CMakeLists.txt", "if(A)\n    set(X 1)\nendif()\n", 0, ""},
      IndentCase{"css at pos 0", "a.css", "a {\n    color: red;\n}\n", 0, ""},
      IndentCase{"dart at pos 0", "a.dart", "class A {\n  int x = 1;\n}\n", 0, ""},
      IndentCase{"go at pos 0", "a.go", "func main() {\n}\n", 0, ""},
      IndentCase{"go at eof", "a.go", "func main() {\n}\n", 16, ""},
      IndentCase{"html at pos 0", "a.html", "<div>\n</div>\n", 0, ""},
      IndentCase{"html at eof", "a.html", "<div>\n</div>\n", 13, ""},
      IndentCase{"js at pos 0", "a.js", "function f() {\n}\n", 0, ""},
      IndentCase{"ts at eof", "a.ts", "interface I {\n}\n", 16, ""},
      IndentCase{"tsx at pos 0", "a.tsx", "const a = (\n    <div />\n);\n", 0, ""},
      IndentCase{"make at pos 0", "Makefile", "all:\n\techo\n", 0, ""},
      IndentCase{"nix at pos 0", "a.nix", "{\n  a = 1;\n}\n", 0, ""},
      IndentCase{"nix at eof", "a.nix", "{\n  a = 1;\n}\n", 13, ""},
      IndentCase{"toml at pos 0", "a.toml", "[a]\nb = 1\n", 0, ""},
      IndentCase{"toml at eof", "a.toml", "[a]\nb = 1\n", 10, ""},
      IndentCase{"json at pos 0", "a.json", "{\n}\n", 0, ""},
      IndentCase{"json at eof", "a.json", "{\n}\n", 4, ""},
      IndentCase{"rust at pos 0", "a.rs", "fn a() {\n}\n", 0, ""},
      IndentCase{"rust at eof", "a.rs", "fn a() {\n}\n", 11, ""},
  };
  RunNewlineCases(kBoundaries);

  // -- the languages with no indents.scm -----------------------------------
  //
  // markdown and diff have grammars and highlights and no indent query, which
  // is not a failure: the caller keeps the bracket heuristic and nothing
  // reaches a status line.
  EXPECT_FALSE(HasIndentQuery("markdown"));
  EXPECT_FALSE(HasIndentQuery("diff"));
  EXPECT_EQ(ForNewline("a.md", "- item\n", 6), std::string("<declined>"));
  EXPECT_EQ(ForLine("a.md", "- item\n", 0), std::string("<declined>"));
  EXPECT_EQ(ForNewline("a.diff", "--- a\n+++ b\n", 5), std::string("<declined>"));
}

// The three additions koi appends to the vendored queries, each below a marker
// that says where helix's file stops. Every case here failed before its
// addition and the comment on it says how.
void TreeIndentVendoredQueryAdditions() {
  TEST_CASE("tree indent: koi's additions to the vendored queries");

  // -- rust: the continuation of a binding ---------------------------------
  //
  // Two additions, for the two halves of one gap.
  //
  // helix's `value: (_) @indent (#not-same-line? ...)` family opens its scope
  // on the value node itself, and under containment a scope only indents the
  // lines *below* the one it opens on -- so a value written on its own line got
  // nothing at all and `2;` sat at the block's column. koi re-states those
  // patterns with `(#set! "scope" "header")`, which opens the scope at the
  // `let` / the assignment / the `static` instead.
  //
  // That fixes a line already written and cannot fix the line being opened,
  // because `let y =` is not a `let_declaration` at all: with no value the
  // grammar recovers `let`, the name and the `=` as one ERROR. So the second
  // addition names the recovery -- `(ERROR "let" "=") @indent @extend` -- which
  // is the same workaround python's own file uses for `try:` and `def`.
  //
  // Both are needed. With only the first, the newline still landed a level
  // short, the user wrote `2;` there, and the hybrid heuristic read that line
  // as its baseline and put the *next* statement in column zero.
  static constexpr std::array kRust{
      IndentCase{"an unfinished binding opens its continuation", "a.rs",
                 "fn a() {\n    let y =\n}\n", 20, "        "},
      IndentCase{"with a space after the = as well", "a.rs", "fn a() {\n    let y = \n}\n", 21,
                 "        "},
      IndentCase{"deeper in, the levels it is inside come with it", "a.rs",
                 "impl A {\n    fn b() {\n        let y =\n    }\n}\n", 37, "            "},
      IndentCase{"at the top level of a file", "a.rs", "let y =", 7, "    "},
      IndentCase{"mid-continuation", "a.rs", "fn a() {\n    let y =\n        2 +\n}\n", 32,
                 "        "},
      IndentCase{"and the statement after the continuation is back at the block", "a.rs",
                 "fn a() {\n    let y =\n        2;\n}\n", 31, "    "},
      // The `#not-same-line?` predicate is carried over unchanged, so a value
      // that starts on the `let`'s own line matches nothing here and the
      // bracket scope it opens is the only level. One, not two.
      IndentCase{"a value on the let's own line is not doubled: a block", "a.rs",
                 "fn a() {\n    let y = {\n    };\n}\n", 22, "        "},
      IndentCase{"...a call", "a.rs", "fn a() {\n    let y = f(\n    );\n}\n", 23, "        "},
      IndentCase{"...a macro", "a.rs", "fn a() {\n    let y = vec![\n    ];\n}\n", 26, "        "},
      IndentCase{"...a tuple", "a.rs", "fn a() {\n    let y = (\n    );\n}\n", 22, "        "},
      IndentCase{"a finished binding still opens exactly one level", "a.rs",
                 "fn a() {\n    let x = 1;\n}\n", 23, "    "},
      IndentCase{"a struct body", "a.rs", "struct S {\n}\n", 10, "    "},
      IndentCase{"a match block", "a.rs", "fn a() {\n    match x {\n    }\n}\n", 22, "        "},
      IndentCase{"an impl block", "a.rs", "impl A {\n}\n", 8, "    "},
      IndentCase{"a use list", "a.rs", "use a::{\n};\n", 8, "    "},
      IndentCase{"an empty buffer", "a.rs", "", 0, ""},
  };
  RunNewlineCases(kRust);

  static constexpr std::array kRustLines{
      IndentCase{"a continuation line is one level inside its let", "a.rs",
                 "fn a() {\n    let y =\n        2;\n}\n", 2, "        "},
      IndentCase{"and is put there from wherever it was written", "a.rs",
                 "fn a() {\n    let y =\n2;\n}\n", 2, "        "},
      IndentCase{"the statement after it is not", "a.rs",
                 "fn a() {\n    let y =\n        2;\n    let z = 3;\n}\n", 3, "    "},
      IndentCase{"nor is the closing brace", "a.rs", "fn a() {\n    let y =\n        2;\n}\n", 3,
                 ""},
      IndentCase{"an assignment's right-hand side", "a.rs", "fn a() {\n    x =\n        2;\n}\n", 2,
                 "        "},
      IndentCase{"a compound assignment's", "a.rs", "fn a() {\n    x +=\n        2;\n}\n", 2,
                 "        "},
      IndentCase{"an if-let condition's value", "a.rs",
                 "fn a() {\n    if let Some(y) =\n        z\n    {\n    }\n}\n", 2, "        "},
      IndentCase{"a static item's value", "a.rs", "static X: i32 =\n    1;\n", 1, "    "},
      IndentCase{"a type alias's type", "a.rs", "type T =\n    u32;\n", 1, "    "},
      // The value here is a call, which is `@indent` in its own right. The
      // header scope and the call's scope open on the same line, so the
      // same-line collapse folds them into one level -- and the argument list,
      // which opens on the next line, is the second.
      IndentCase{"a value that is itself a call: one level for the let", "a.rs",
                 "fn a() {\n    let y =\n        f(\n            b,\n        );\n}\n", 2,
                 "        "},
      IndentCase{"and one more for its arguments", "a.rs",
                 "fn a() {\n    let y =\n        f(\n            b,\n        );\n}\n", 3,
                 "            "},
      IndentCase{"a struct field", "a.rs", "struct S {\nx: i32,\n}\n", 1, "    "},
      IndentCase{"a match arm", "a.rs", "fn a() {\n    match x {\n1 => 2,\n    }\n}\n", 2,
                 "        "},
      IndentCase{"a let-else body", "a.rs",
                 "fn a() {\n    let Some(y) = z else {\n        return;\n    };\n}\n", 2,
                 "        "},
      IndentCase{"the closing brace", "a.rs", "fn a() {\n    let x = 1;\n    }\n", 2, ""},
  };
  RunLineCases(kRustLines);

  EXPECT_EQ(Typed("a.rs", "fn a() {\n    let x = 1;\n    \n", 28, "}"),
            std::string("fn a() {\n    let x = 1;\n}\n"));
  // The transcript end to end, typed one key at a time with auto-pairs on.
  // Before the additions this came out `let y =` / `2;` at one level and
  // `let z = 3;` in column zero.
  EXPECT_EQ(Typed("a.rs", "", 0, "fn a() {\nlet y =\n2;\nlet z = 3;", true),
            std::string("fn a() {\n    let y =\n        2;\n    let z = 3;\n}"));
  EXPECT_EQ(Typed("a.rs", "", 0, "fn a() {\nlet y = 1;\nlet z = 2;", true),
            std::string("fn a() {\n    let y = 1;\n    let z = 2;\n}"));
  EXPECT_EQ(Typed("a.rs", "", 0, "fn a() {\nif x {\nlet y =\n2;", true),
            std::string("fn a() {\n    if x {\n        let y =\n            2;\n    }\n}"));

  // -- c: unterminated delimiters ------------------------------------------
  //
  // `(ERROR "{") @indent @extend` and its `(` and `[` siblings, which go's own
  // vendored file already carries. What they buy is the *absolute* answer for a
  // line inside a prefix the grammar could not finish: with the recovery node
  // captured, a line written flat inside an unclosed brace comes back to the
  // brace's own level instead of to column zero.
  static constexpr std::array kCErrorLines{
      IndentCase{"a statement inside an unclosed brace, written flat", "a.c",
                 "void g() {\n    if (x) {\na();\n", 2, "    "},
      IndentCase{"a continuation inside an unclosed (", "a.c", "void g() {\n    f(a,\nb,\n", 2,
                 "    "},
      IndentCase{"an initialiser inside an unclosed {", "a.c", "int a[] = {\n1,\n", 1, "    "},
  };
  RunLineCases(kCErrorLines);

  static constexpr std::array kCError{
      IndentCase{"an unclosed brace on its own", "a.c", "void g() {", 10, "    "},
      IndentCase{"a statement under one", "a.c", "void g() {\n    a();", 19, "    "},
      IndentCase{"an unclosed brace-less if -- the whole prefix is one ERROR", "a.c",
                 "void g() {\n    if (x)", 21, "    "},
      IndentCase{"an unclosed (", "a.c", "void g() {\n    f(", 17, "    "},
      IndentCase{"an unclosed [", "a.c", "void g() {\n    int a[", 21, "    "},
      IndentCase{"a body already written at the right depth", "a.c",
                 "void g() {\n    if (x)\n        a();", 34, "    "},
      // The rules need @extend to own the line a newline opens: without it a
      // recovery node ending at the caret with nothing invented in it fails
      // containment, and these two -- reachable only when the prefix above is
      // healthy, so the ERROR holds the delimiter alone -- answered nothing.
      IndentCase{"an unclosed ( after a healthy prefix", "a.c", "void h() { }\nint x = foo(", 25,
                 "    "},
      IndentCase{"an unclosed [ after a healthy prefix", "a.c", "void h() { }\nint x = a[", 23,
                 "    "},
  };
  RunNewlineCases(kCError);

  // KNOWN: what the ERROR patterns cannot reach. The recovery node begins at
  // the *function's* column, so it opens exactly one scope however many
  // constructs are unfinished inside it -- and the brace-less `if` under it has
  // no node of its own at all. So `work()` lands at the function's level rather
  // than the body's, and the hybrid heuristic then reads that line as its
  // baseline and puts `next()` in column zero. Fixing it would take a pattern
  // for the recovered `if` and not one for the recovered `{`, which is past
  // what these three lines were authorised to do. Both branches of the same
  // transcript are pinned: with auto-pairs on -- koi's default -- the closer is
  // in the buffer, the tree is whole, and every line is right.
  EXPECT_EQ(Typed("a.c", "", 0, "void g() {\nif (x)\nwork();\nnext();"),
            std::string("void g() {\n    if (x)\n    work();\nnext();"));
  EXPECT_EQ(Typed("a.cpp", "", 0, "void g() {\nif (x)\nwork();\nnext();"),
            std::string("void g() {\n    if (x)\n    work();\nnext();"));
  EXPECT_EQ(Typed("a.cpp", "", 0, "void g() {\nif (x)\nwork();\nnext();", true),
            std::string("void g() {\n    if (x)\n        work();\n    next();\n}"));
  // Nothing unfinished but the block, and every line is right without them.
  EXPECT_EQ(Typed("a.cpp", "", 0, "void g() {\na();\nb();"),
            std::string("void g() {\n    a();\n    b();"));

  // -- python: else: with nothing under it ---------------------------------
  //
  // `(ERROR ":" "else" @outdent)`. An `else:` typed into a body that has no
  // line under it yet is not an `else_clause`: the grammar folds the `if`'s
  // `:`, the body and the `else` into one ERROR, and
  // `(else_clause "else" @outdent)` has nothing to match. The keyword then
  // stayed in the body's column until a line was typed beneath it. `elif`,
  // `except` and `finally` all recover as real clauses and needed no help --
  // which the cases below check, so that the addition is known to be the one
  // thing it claims to be.
  static constexpr std::array kPythonElse{
      IndentCase{"an else: with nothing under it comes back to the if", "a.py",
                 "if x:\n    y = 1\n    else:", 2, ""},
      IndentCase{"with the newline already typed too", "a.py", "if x:\n    y = 1\n    else:\n", 2,
                 ""},
      IndentCase{"nested one level deeper", "a.py",
                 "def f():\n    if x:\n        y = 1\n        else:", 3, "    "},
      IndentCase{"an else: that does have a body", "a.py",
                 "if x:\n    y = 1\n    else:\n    z = 2\n", 2, ""},
      IndentCase{"a for ... else", "a.py", "for i in x:\n    y = 1\n    else:", 2, ""},
      IndentCase{"elif recovers as a real clause and needed no help", "a.py",
                 "if x:\n    y = 1\n    elif z:", 2, ""},
      IndentCase{"except too", "a.py", "try:\n    y = 1\n    except E:", 2, ""},
      IndentCase{"the body line above it is untouched", "a.py", "if x:\n    y = 1\n    else:", 1,
                 "    "},
      IndentCase{"a complete if/else is not an ERROR and is unaffected", "a.py",
                 "if x:\n    y = 1\nelse:\n    z = 2\n", 2, ""},
      IndentCase{"nor is a nested one", "a.py",
                 "if x:\n    if y:\n        z = 1\n    else:\n        w = 2\n", 3, "    "},
  };
  RunLineCases(kPythonElse);

  // And through the keystrokes, which is where it matters: the `:` is what
  // completes the token, and the line moves on that keystroke.
  EXPECT_EQ(Typed("a.py", "if x:\n    y = 1\n    ", 20, "else:"),
            std::string("if x:\n    y = 1\nelse:"));
  EXPECT_EQ(Typed("a.py", "if x:\n    y = 1\n    \n", 20, "else:"),
            std::string("if x:\n    y = 1\nelse:\n"));
  EXPECT_EQ(Typed("a.py", "if x:\n    y = 1\n    ", 20, "else:\nz = 2"),
            std::string("if x:\n    y = 1\nelse:\n    z = 2"));
  EXPECT_EQ(Typed("a.py", "def f():\n    if x:\n        y = 1\n        ", 41, "else:"),
            std::string("def f():\n    if x:\n        y = 1\n    else:"));
  EXPECT_EQ(Typed("a.py", "if x:\n    if y:\n        z = 1\n        ", 38, "else:"),
            std::string("if x:\n    if y:\n        z = 1\n    else:"));
  // The `elsewhere` problem, which the addition has to leave alone: without the
  // `:` the pattern matches nothing, so the word is never moved in the first
  // place and there is nothing for the memory to put back.
  EXPECT_EQ(Typed("a.py", "if x:\n    y = 1\n    ", 20, "elsewhere"),
            std::string("if x:\n    y = 1\n    elsewhere"));
  EXPECT_EQ(Typed("a.py", "if x:\n    y = 1\n    ", 20, "elsewhere = 1"),
            std::string("if x:\n    y = 1\n    elsewhere = 1"));
  {
    Typist typist{"a.py", "if x:\n    y = 1\n    ", 20, false};
    typist.Type("else");
    EXPECT_EQ(LeadingOf(typist.ed.doc.table, 2), std::string("    "));
    typist.Type(":");
    EXPECT_EQ(LeadingOf(typist.ed.doc.table, 2), std::string(""));
  }
  // An `else:` that already had a body kept working, which is the case the
  // vendored rule always covered.
  EXPECT_EQ(Typed("a.py", "if x:\n    y = 1\n    \n    z = 2\n", 20, "else:"),
            std::string("if x:\n    y = 1\nelse:\n    z = 2\n"));
}

// Three things about the *text* rather than about any one language: a document
// spelled in tabs, one with multi-byte characters left of the caret, and one
// with CRLF terminators. All three are places where an indenter that counted
// bytes, or that assumed a level is four spaces, would be wrong by an amount
// nobody would notice until it was in a file.
void TreeIndentTabsUtf8AndCrlf() {
  TEST_CASE("tree indent: tabs, multi-byte text and CRLF");

  // -- tabs ----------------------------------------------------------------
  //
  // A level is one tab, in every language, on both paths.
  EXPECT_EQ(ForNewline("a.rs", "fn a() {\n\tlet x = 1;\n}\n", 20, kTabs), std::string("\t"));
  EXPECT_EQ(ForNewline("a.py", "def f():\n\tx = 1\n", 15, kTabs), std::string("\t"));
  EXPECT_EQ(ForNewline("a.json", "{\n\t\"a\": [\n\t]\n}\n", 9, kTabs), std::string("\t\t"));
  EXPECT_EQ(ForNewline("a.nix", "{\n\ta = 1;\n}\n", 9, kTabs), std::string("\t"));
  EXPECT_EQ(ForNewline("a.js", "function f() {\n\ta();\n}\n", 20, kTabs), std::string("\t"));
  EXPECT_EQ(ForNewline("a.sh", "if true; then\n\techo a\nfi\n", 21, kTabs), std::string("\t"));
  EXPECT_EQ(ForLine("a.go", "func main() {\n\ta()\n\t}\n", 2, kTabs), std::string(""));
  EXPECT_EQ(ForLine("a.sh", "if true; then\n\techo a\n\tfi\n", 2, kTabs), std::string(""));
  EXPECT_EQ(Typed("a.go", "func main() {\n\ta()\n\t\n", 20, "}"),
            std::string("func main() {\n\ta()\n}\n"));
  EXPECT_EQ(Typed("a.rs", "fn a() {\n\tlet x = 1;\n\t\n", 22, "}"),
            std::string("fn a() {\n\tlet x = 1;\n}\n"));

  // -- multi-byte text -----------------------------------------------------
  //
  // The engine works in bytes throughout -- every offset it holds is one -- and
  // an `@align` is the one place where that could turn into a column. It is
  // stored as the document text left of the anchor and rendered through
  // `GraphemeWidth`, so a two-byte `é` is one column and a three-byte `日` is
  // two, and neither is the three or four spaces a byte count would give.
  static constexpr std::array kAlign{
      IndentCase{"an argument list aligns to a column, not to a byte count", "a.cpp",
                 "int main() {\n    foo(a,\n    );\n}\n", 23, "        "},
      IndentCase{"one character wider", "a.cpp", "int main() {\n    fooo(a,\n    );\n}\n", 24,
                 "         "},
      IndentCase{"a two-byte é is one column, not two", "a.cpp",
                 "int main() {\n    féo(a,\n    );\n}\n", 24, "        "},
      IndentCase{"a three-byte 日 is two columns, not three", "a.cpp",
                 "int main() {\n    f日(a,\n    );\n}\n", 24, "        "},
      IndentCase{"and the same inside a comment before the anchor", "a.cpp",
                 "int main() {\n    /*日*/ foo(a,\n    );\n}\n", 31, "               "},
      IndentCase{"python's align counts the same way", "a.py", "def f():\n    g(a,\n    )\n", 17,
                 "      "},
      IndentCase{"...with a wide character in the name", "a.py", "def f():\n    g日(a,\n    )\n", 20,
                 "        "},
  };
  RunNewlineCases(kAlign);

  // -- the alignment cap cuts between characters, not through one -----------
  //
  // An `@align` is stored as the document text left of the anchor, clipped to
  // the last kMaxAlignBytes of it. A byte cut lands wherever it lands, and left
  // where it fell it kept the last two bytes of a three-byte 日: the width walk
  // measures whatever those orphaned continuation bytes look like to it, a
  // column each here, so the continuation lands two columns right of where the
  // list sits -- 1024 instead of the 1022 below. The cut is snapped forward to
  // the next grapheme boundary now, which can only drop a partial character.
  //
  // Built so that the cap falls in the *middle* of the 日: the line's prefix is
  // 1041 bytes, the cut is therefore 17 bytes into it, and the character sits at
  // bytes 16..19. What survives the snap is the 1015 `y`s and the `*/ foo(` that
  // follows them -- 1022 columns, none of them the character's.
  {
    const std::string padding(1015, 'y');
    const std::string line = "    /*" + std::string(10, 'x') + "\xE6\x97\xA5" + padding +
                             "*/ foo(a,";
    // 1041 bytes up to the anchor, plus the `a,` the anchor itself begins.
    EXPECT_EQ(std::ssize(line), Index{1043});
    const std::string text = "int main() {\n" + line + "\n    );\n}\n";
    const auto caret = static_cast<Index>(text.find(",\n") + 1);
    EXPECT_EQ(ForNewline("a.cpp", text, caret), std::string(1022, ' '));
  }

  // -- an alignment carries its own outdent --------------------------------
  //
  // The line a Return opens in front of a `)` *begins* with that `)`, so the
  // closer's `@outdent` is counted for it -- and an alignment is an absolute
  // column that already has that outdent in it, being the column of the list
  // whose scope the `)` is closing. Subtracting a level from it landed the
  // whole continuation a tab short of the anchor: column 3 for a `void f(`,
  // column 0 for python's `foo(`, which is not where anything in the list
  // sits. The anchor is the answer, for the closer's line as much as for the
  // argument that would have gone there.
  static constexpr std::array kAlignedCloser{
      IndentCase{"a newline in front of an aligned `)` still lands on the anchor", "a.cpp",
                 "void f(int a,\n       int b);", 26, "       "},
      IndentCase{"...and python counts it from its own anchor", "a.py", "foo(a,\n    b)\n", 12,
                 "    "},
  };
  RunNewlineCases(kAlignedCloser);
  // And asked of the line the closer already begins, which is the question the
  // re-indent path puts.
  static constexpr std::array kAlignedCloserLines{
      IndentCase{"the `)` line of a wrapped parameter list is the anchor's column", "a.cpp",
                 "void f(int a,\n       int b\n);", 2, "       "},
      IndentCase{"python's dangling `)` likewise", "a.py", "foo(a,\n    b,\n)\n", 2, "    "},
  };
  RunLineCases(kAlignedCloserLines);

  // A level count is not a column and multi-byte content on the line cannot
  // move it, however many bytes the characters take.
  static constexpr std::array kUtf8Levels{
      IndentCase{"a level count is untouched by multi-byte content on the line", "a.cpp",
                 "int main() {\n    puts(\"日本\");\n}\n", 32, "    "},
      IndentCase{"...and by a multi-byte comment above it", "a.cpp",
                 "int main() {\n    // ééé\n    work();\n}\n", 38, "    "},
      IndentCase{"python keeps its level too", "a.py", "def f():\n    x = \"日\"\n", 22, "    "},
      IndentCase{"and so does yaml", "a.yaml", "éfoo:\n", 6, "    "},
  };
  RunNewlineCases(kUtf8Levels);
  EXPECT_EQ(ForLine("a.cpp", "int main() {\n    work(\"日本\");\n    }\n", 2), std::string(""));

  // -- CRLF ----------------------------------------------------------------
  //
  // The piece table's line index counts a `\r\n` as one terminator and
  // `LineContentRange` stops short of the `\r`, so nothing here has to know
  // about it -- which is exactly what these pin.
  static constexpr std::array kCrlf{
      IndentCase{"a block opened on a CRLF line", "a.cpp", "int main() {\r\n}\r\n", 12, "    "},
      IndentCase{"a statement inside one", "a.cpp", "int main() {\r\n    work();\r\n}\r\n", 25,
                 "    "},
      IndentCase{"python's header extension over CRLF", "a.py", "def f():\r\n", 8, "    "},
  };
  RunNewlineCases(kCrlf);
  EXPECT_EQ(ForLine("a.cpp", "int main() {\r\n    work();\r\n    }\r\n", 2), std::string(""));
  EXPECT_EQ(Typed("a.cpp", "int main() {\r\n    work();\r\n    \r\n", 31, "}"),
            std::string("int main() {\r\n    work();\r\n}\r\n"));
}

// Two patterns naming the same node, one of them a header, and only one of the
// two carrying a parent.
//
// Captures with the same span fold into one Scope, and the span alone cannot say
// which node it came from: a child that fills its parent exactly has the same
// one, and the two have different parents. The parent is therefore taken from
// the capture that made the scope a header -- the only capture Syntax fills the
// field for -- and not from whichever of them reached the fold first.
//
// The fixture below is the shape that tells those two apart, because it puts the
// header's parent and the captured node on different lines: `if (x)` opens on
// line 1, its unbraced body `y;` is the whole of line 2, and a Return at the end
// of line 1 has to open inside the header's scope. Reading the parent off the
// wrong capture gives the body's own line, the scope no longer contains the line
// being opened, and the answer comes out a level short -- four spaces where the
// body already sits at eight.
void TreeIndentHeaderParentFold() {
  TEST_CASE("tree indent: a header scope's parent comes from the header capture");

  const FakeQueryDir queries{"c"};
  EXPECT_TRUE(queries.Ready());
  if (!queries.Ready()) return;

  // The middle pattern is the non-header one. It is rooted at the function
  // rather than at the `if`, which is what puts its match ahead of the header's
  // in the order the cursor hands them over: matches arrive by the start byte of
  // the node the pattern is rooted at, and the function starts first. Written as
  // a plain `(expression_statement) @indent` it would arrive second and the test
  // would pass whichever capture the fold believed.
  queries.Write("indents.scm",
                "(compound_statement) @indent\n"
                "\n"
                "(function_definition\n"
                "  body: (compound_statement\n"
                "          (if_statement\n"
                "            consequence: (_) @indent)))\n"
                "\n"
                "(if_statement\n"
                "  consequence: (_) @indent\n"
                "  (#not-kind-eq? @indent \"compound_statement\")\n"
                "  (#set! \"scope\" \"header\"))\n");

  // A thread of its own: the compiled-query cache is thread_local and sticky, so
  // this is what keeps the shadowing query from being a stale hit here and the
  // real one from being a stale hit everywhere else.
  OnAThreadOfItsOwn([&] {
    const std::string_view text = "void g() {\n    if (x)\n        y;\n}";

    // The premise, before the answer that depends on it: both patterns capture
    // `y;` at [30, 32], the non-header one arrives first carrying nothing but
    // its own start, and the header one arrives second carrying the `if` at 15.
    Fixture fixture = Open("a.c", text);
    EXPECT_TRUE(fixture.ok());
    if (fixture.ok()) {
      const std::array<std::string_view, 1> files{"indents.scm"};
      std::vector<Capture> captures;
      std::string error;
      EXPECT_TRUE(fixture.syntax->Captures(fixture.table, files,
                                           Interval(0, DocLength(fixture.table)), captures, error,
                                           nullptr, true));
      std::vector<Index> parents;
      for (const Capture& capture : captures) {
        if ((capture.from == Index{30}) && (capture.to == Index{32})) {
          parents.push_back(capture.parent_from);
        }
      }
      EXPECT_EQ(std::ssize(parents), 2);
      if (parents.size() == 2) {
        EXPECT_EQ(parents[0], Index{30});
        EXPECT_EQ(parents[1], Index{15});
      }
    }

    // And the answer: a Return at the end of `if (x)` opens the body's line.
    EXPECT_EQ(ForNewline("a.c", text, 21), std::string("        "));
  });
}

// The injected-region guard, across the two things that happen between the paint
// that finds a region and the question asked about it.
//
// One: `Captures` syncs, and a sync used to empty the map the guard read -- so
// the first caret of a multi-cursor command blinded it for every caret after it,
// and three carets with one Return between them opened the <script> body's new
// line at html's answer. The region *had* been painted; a sibling caret
// destroyed the evidence.
//
// Two: the guard never looked at the document, and its only staleness test
// compared the Syntax object's own revision against the cached region's -- two
// counters that move together. Between an edit and the paint after it every
// offset it held was one edit out of date, so it answered false for where the
// region now is and true for where it used to be. That is the state the
// re-indent trigger asks it in, on every keystroke.
//
// Both are asked here through what the callers actually see: an indent that came
// out of html's query where the engine should have declined, and a decline where
// it should have answered.
void TreeIndentInjectionGuardOutlivesEdits() {
  TEST_CASE("tree indent: the injection guard survives sibling carets and edits");

  const std::string page =
      "<div>\n"
      "<script>\n"
      "    var x = {\n"
      "    }\n"
      "</script>\n"
      "    <p>a</p>\n"
      "    <p>b</p>\n"
      "</div>\n";
  const auto at = [&page](std::string_view needle) {
    const std::size_t found = page.find(needle);
    EXPECT_TRUE(found != std::string::npos);
    return static_cast<Index>(found);
  };
  // The `{` inside the region, and the ends of two host lines below it.
  //
  // Both below, and that is the whole fixture. The newline path walks its carets
  // back to front, and it is the *second* caret's query that first meets a table
  // whose revision has moved past the tree's -- the one that re-parses, and used
  // to empty the map the guard read. So the region's caret has to be the third
  // one processed to be asked after that has happened, which is two host carets
  // below it and not one. With one the guard still answered, which is why the
  // one- and two-caret shapes hid this.
  const Index inside = at("    var x = {\n") + 13;
  const Index below_a = at("<p>a</p>\n") + 8;
  const Index below_b = at("<p>b</p>\n") + 8;

  // html's own answer for that caret, which is what the guard exists to keep off
  // the line -- and, taken from an unpainted buffer, exactly what the bug put
  // there. Asserted to differ from the answer below, so this stays a test and
  // does not quietly become a tautology if either changes.
  const std::string html_answer = ForNewline("a.html", page, inside);
  // And its answer for one of the host carets, which is the answer that has to
  // survive unchanged: the region's decline is about the region.
  const std::string host_answer = ForNewline("a.html", page, below_a);

  // What a decline leaves: the caller's bracket heuristic, one level in from the
  // line the `{` is on.
  const std::string declined{"        "};
  EXPECT_TRUE(html_answer != declined);

  // The line a Return at `inside` opened, given the editor it happened in.
  const auto opened_line = [](const Editor& ed) {
    const std::string text = AssembleDocContents(ed.doc.table);
    const std::size_t brace = text.find("var x = {");
    if (brace == std::string::npos) return std::string{"<no brace>"};
    const Index line = LineAt(ed.doc.table, static_cast<Index>(brace));
    return LeadingOf(ed.doc.table, line + 1);
  };

  {
    // One caret: the shape that already worked, kept beside the one that did
    // not so a regression cannot move both together.
    const std::array<Index, 1> alone{inside};
    const Editor ed = PressReturn("a.html", page, alone, true);
    EXPECT_EQ(opened_line(ed), declined);
  }
  {
    // Three carets, one command. IND-H3.
    const std::array<Index, 3> carets{inside, below_a, below_b};
    const Editor ed = PressReturn("a.html", page, carets, true);
    EXPECT_EQ(opened_line(ed), declined);
    // And the host carets still got the host's answer, which is the other half
    // of the claim: the guard declines for the region, not for the document.
    const std::string text = AssembleDocContents(ed.doc.table);
    const Index host_line = LineAt(ed.doc.table, static_cast<Index>(text.find("<p>a</p>")));
    EXPECT_EQ(LeadingOf(ed.doc.table, host_line + 1), host_answer);
  }

  // -- IND-M5: an edit, and no paint after it ------------------------------
  //
  // Everything below asks the engine between the edit and the frame that would
  // have redrawn it, because that is where the re-indent trigger lives.
  //
  // The two halves get a fixture each, and each asks its own question first. An
  // engine call that gets past the guard syncs on its way to the tree, and a
  // sync is one of the things that puts the guard right -- so the two questions
  // asked in one buffer would have the second answered by the first.
  const std::string inserted_line{"<p>inserted</p>\n"};
  const auto shift = static_cast<Index>(inserted_line.size());
  const Index body = at("var x = {");
  {
    // Where the region is now: still the injected language's, still declined.
    // Answering it is the false negative -- JavaScript indented by html's rules.
    Fixture fixture = Open("a.html", page);
    EXPECT_TRUE(fixture.ok());
    if (fixture.ok()) {
      std::vector<CaptureId> painted;
      fixture.syntax->Paint(fixture.table, Interval(0, DocLength(fixture.table)), painted);
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, body) == Injected::kYes);

      // A whole host line inserted above the region. Every byte of it moves.
      EXPECT_FALSE(static_cast<bool>(Insert(inserted_line, at("<script>"), fixture.table)));

      std::string error;
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, body + shift) == Injected::kYes);
      EXPECT_FALSE(TreeIndentForNewline(fixture.table, *fixture.syntax, body + shift, kFourSpaces,
                                        error)
                       .has_value());
      EXPECT_EQ(error, std::string{});
    }
  }
  {
    // And where the region used to be, which the inserted host line now
    // occupies: the guard has no claim on those bytes and html's query answers
    // for them. Declining here is the false positive -- the half of the bug that
    // stops the host document being indented at all.
    Fixture fixture = Open("a.html", page);
    EXPECT_TRUE(fixture.ok());
    if (fixture.ok()) {
      std::vector<CaptureId> painted;
      fixture.syntax->Paint(fixture.table, Interval(0, DocLength(fixture.table)), painted);
      EXPECT_FALSE(static_cast<bool>(Insert(inserted_line, at("<script>"), fixture.table)));

      std::string error;
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, body) == Injected::kNo);
      EXPECT_TRUE(
          TreeIndentForNewline(fixture.table, *fixture.syntax, body, kFourSpaces, error)
              .has_value());
      EXPECT_EQ(error, std::string{});
    }
  }
  {
    // An edit *inside* the region grows it, and the bytes it added are the
    // injected language's too.
    Fixture fixture = Open("a.html", page);
    EXPECT_TRUE(fixture.ok());
    if (fixture.ok()) {
      std::vector<CaptureId> painted;
      fixture.syntax->Paint(fixture.table, Interval(0, DocLength(fixture.table)), painted);

      const Index end = at("</script>");
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, end) == Injected::kNo);

      const std::string added{"    let y = 2;\n"};
      EXPECT_FALSE(static_cast<bool>(Insert(added, at("    }\n"), fixture.table)));
      // The old end of the region is inside it now.
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, end) == Injected::kYes);
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(
                      fixture.table, end + static_cast<Index>(added.size())) == Injected::kNo);
    }
  }

  {
    // And the state the journal cannot cover. `kUnknown` is what is left when
    // the spans cannot be carried at all, and both callers have to read it as
    // "possibly inside": the whole document declines until a paint says
    // otherwise, which costs the bracket heuristic and not the user's
    // whitespace.
    //
    // Reached here by moving the journal out from under the spans by hand. The
    // honest way is 65,536 edits without a frame between them -- the journal cap
    // is what trims it -- and that is a minute of test time to arrive at the
    // three fields below.
    Fixture fixture = Open("a.html", page);
    EXPECT_TRUE(fixture.ok());
    if (fixture.ok()) {
      std::vector<CaptureId> painted;
      fixture.syntax->Paint(fixture.table, Interval(0, DocLength(fixture.table)), painted);
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, body) == Injected::kYes);

      fixture.table.revision += 1;
      fixture.table.journal.clear();
      fixture.table.journal_base = fixture.table.revision;

      std::string error;
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, body) == Injected::kUnknown);
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, 0) == Injected::kUnknown);
      EXPECT_FALSE(
          TreeIndentForNewline(fixture.table, *fixture.syntax, 0, kFourSpaces, error).has_value());
      EXPECT_EQ(error, std::string{});

      // Until a frame is drawn, which is what discovers regions in the first
      // place. Nothing else lowers it -- least of all the syncs in between,
      // which is how the answer could go back to being wrong for free.
      fixture.syntax->Sync(fixture.table);
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, body) == Injected::kUnknown);
      fixture.syntax->Paint(fixture.table, Interval(0, DocLength(fixture.table)), painted);
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, body) == Injected::kYes);
      EXPECT_TRUE(fixture.syntax->InInjectedRegion(fixture.table, 0) == Injected::kNo);
    }
  }

  // -- the re-indent path, through the real keys ---------------------------
  //
  // A Return inside the <script> body and then a `}` typed on the line it
  // opened. The Return is the preceding edit: it is what used to leave the
  // guard answering from offsets that no longer described the buffer, and the
  // `}` is what asks it. No paint runs between the two -- the editor would draw
  // a frame there, and the point is that it does not have to.
  //
  // html has no rule that outdents a `}`, so the line staying put is necessary
  // and not sufficient: the trigger would have found nothing to do with html's
  // answer even if it got one. What it decides on is whether there *is* an
  // answer for that line, so that is asked in the state the `}` will ask it in
  // -- after the Return's edit and before anything has synced. The Return
  // declines, and a decline never reaches the tree, so nothing between the two
  // keystrokes puts the guard right on its way past.
  {
    const std::string script =
        "<div>\n"
        "<script>\n"
        "var x = {\n"
        "</script>\n"
        "</div>\n";
    const auto brace_end = static_cast<Index>(script.find("var x = {")) + 9;
    Typist typist{"a.html", script, brace_end, false};
    EXPECT_TRUE(typist.ed.doc.syntax != nullptr);
    if (typist.ed.doc.syntax != nullptr) {
      PieceTable& table = typist.ed.doc.table;
      typist.ed.doc.syntax->Sync(table);
      std::vector<CaptureId> painted;
      typist.ed.doc.syntax->Paint(table, Interval(0, DocLength(table)), painted);

      typist.Type("\n");
      const Index opened =
          LineAt(table, static_cast<Index>(typist.Text().find("var x = {"))) + 1;
      std::string error;
      EXPECT_FALSE(
          TreeIndentForLine(table, *typist.ed.doc.syntax, opened, kFourSpaces, error).has_value());
      EXPECT_EQ(error, std::string{});

      typist.Type("}");
      EXPECT_EQ(typist.Text(), std::string("<div>\n<script>\nvar x = {\n    }\n</script>\n"
                                           "</div>\n"));
      EXPECT_EQ(LeadingOf(table, opened), std::string("    "));
    }
  }
}

namespace {

// A buffer the parser gave up on, in the state TreeSitterSyntax leaves one in:
// the full parse ran past its budget, no tree came back, and after three such
// attempts Sync stops trying at all -- so every query for the rest of the
// session fails with this same message and `TimedOut` stays raised behind it.
//
// A stub rather than the megabyte of generated C++ that first showed this. The
// contract under test is what a failing Captures may do to `error`, and the real
// thing costs half a second of parser per press to say the same thing; what ties
// the two together is the message below, copied from syntax.cpp's.
struct GaveUpSyntax : Syntax {
  int queries{0};

  std::string_view Language() const override { return "cpp"; }
  std::span<const std::string> CaptureNames() const override { return {}; }
  void Sync(const PieceTable&) override {}
  void Paint(const PieceTable&, Interval, std::vector<CaptureId>& out) override { out.clear(); }
  bool TimedOut() const override { return true; }
  bool InjectionsTruncated() const override { return false; }
  Index InjectionParses() const override { return 0; }
  bool Captures(const PieceTable&, std::span<const std::string_view>, Interval,
                std::vector<Capture>& out, std::string& error, bool* budget_exhausted, bool,
                const std::chrono::steady_clock::time_point*) override {
    ++queries;
    out.clear();
    if (budget_exhausted != nullptr) *budget_exhausted = false;
    error = "parse gave up -- file too large";
    return false;
  }
  bool InLiteralOrComment(Index) override { return false; }
  Injected InInjectedRegion(const PieceTable&, Index) override { return Injected::kNo; }
};

}

// IND-M2, the half that must never be said: a buffer past the parse budget
// warned "parse gave up -- file too large" on every Enter, forever, over
// whatever the status line was showing. The highlighter already says it by
// being off, the indent engine falling back to the bracket heuristic is the
// designed answer, and there is nothing the person typing can do about either.
void TreeIndentSaysNothingAboutAParseItNeverGot() {
  TEST_CASE("tree indent: a buffer the parser gave up on warns about nothing");

  // The engine first: every query fails, so it declines -- and declines with
  // `error` as it found it, which is what makes the class distinguishable at the
  // consumer without anybody matching on message text.
  {
    GaveUpSyntax syntax;
    PieceTable table;
    ResetToOriginal(table, std::string{"int main() {\n    work();\n}\n"});
    std::string error;
    EXPECT_FALSE(TreeIndentForNewline(table, syntax, 12, kFourSpaces, error).has_value());
    EXPECT_EQ(error, std::string{});
    EXPECT_FALSE(TreeIndentForLine(table, syntax, 1, kFourSpaces, error).has_value());
    EXPECT_EQ(error, std::string{});
    // And it did reach the query: the decline above is the failing Captures and
    // not `cpp` turning out to have no indents.scm.
    EXPECT_TRUE(syntax.queries > 0);
  }

  // Then the command, which is where the message used to land. Three Enters, and
  // the status line still says what it said before them.
  {
    Editor ed;
    ed.doc.syntax = std::make_shared<GaveUpSyntax>();
    ed.doc.file = std::filesystem::path{"a.cpp"};
    ResetToOriginal(ed.doc.table, std::string{"int main() {\n"});
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{12, 12, -1}));
    ed.status = "what the user was reading";
    for (int press = 0; press < 3; ++press) RunCommands(ed, {"insert_newline"});
    EXPECT_EQ(ed.status.text(), std::string("what the user was reading"));
    EXPECT_TRUE(ed.indent_warned.message.empty());
    // And the silence covers a buffer that still indents: the bracket heuristic
    // put the first line one level in from the `{` and the rest kept it.
    EXPECT_EQ(AssembleDocContents(ed.doc.table),
              std::string("int main() {\n    \n    \n    \n"));
  }
}

// The other half of IND-M2: what the engine does report -- the user's own
// indents.scm, named with the line it broke on -- still reaches the status line,
// but once per buffer rather than once per Enter.
void TreeIndentWarnsOncePerBrokenQuery() {
  TEST_CASE("tree indent: a broken indents.scm is one message per document");

  const FakeQueryDir queries{"cmake"};
  EXPECT_TRUE(queries.Ready());
  if (!queries.Ready()) return;
  queries.Write("indents.scm", "(this is not a query");

  // A thread of its own, like every other case that shadows a shipped query:
  // CompileQuery's cache and the has-a-query memo are both thread_local, and a
  // failure is cached as hard as a success.
  OnAThreadOfItsOwn([&] {
    const auto document = [](std::string_view text) {
      Document doc;
      std::string error;
      doc.file = std::filesystem::path{"CMakeLists.txt"};
      doc.syntax = OpenSyntax(doc.file, error);
      ResetToOriginal(doc.table, std::string{text});
      doc.selections.Set(MinWidth1(doc.table, Selection{5, 5, -1}));
      return doc;
    };

    Editor ed;
    ed.doc = document("if(A)\n");
    EXPECT_TRUE(ed.doc.syntax != nullptr);
    if (ed.doc.syntax == nullptr) return;

    RunCommands(ed, {"insert_newline"});
    EXPECT_TRUE(ed.status.find("cmake") != std::string::npos);
    EXPECT_TRUE(ed.status.find("indents.scm") != std::string::npos);
    const std::string said = ed.status.text();

    // Same buffer, same broken query, next keystroke: the status line is left
    // for whatever else the editor has to say.
    ed.status = "something else entirely";
    RunCommands(ed, {"insert_newline"});
    EXPECT_EQ(ed.status.text(), std::string("something else entirely"));

    // Another document is another buffer's news, word for word the same message
    // or not -- opening a second file with the same broken query says so.
    AddBuffer(ed, document("if(B)\n"));
    ed.status.clear();
    RunCommands(ed, {"insert_newline"});
    EXPECT_EQ(ed.status.text(), said);

    // And coming back says it again: the memory is of one document, not of the
    // message, so it cannot silence a buffer that never had its say.
    SwitchToBuffer(ed, 0);
    EXPECT_EQ(AssembleDocContents(ed.doc.table).substr(0, 5), std::string("if(A)"));
    ed.status.clear();
    RunCommands(ed, {"insert_newline"});
    EXPECT_EQ(ed.status.text(), said);

    // Still once, though: the repeat inside the buffer we came back to is still
    // dropped.
    ed.status = "and the status line stays theirs";
    RunCommands(ed, {"insert_newline"});
    EXPECT_EQ(ed.status.text(), std::string("and the status line stays theirs"));
  });
}

namespace {

// indent.cpp's walk bound, restated -- it is file-local there, like the render
// guards further down, and a test that wrote its own number would pass whatever
// the code did.
constexpr Index kMaxBaselineLines = 512;

// The real engine with a counted supply of queries: the first `allow` of them
// are answered out of it, and every one after that comes back the way a run the
// budget cut off comes back -- true, empty, `budget_exhausted` raised.
//
// Which is what makes a mid-run budget testable at all. The thing under test is
// what the engine does when it runs out of time *between* its queries -- the
// answer for the new line is in hand and the hybrid's baselines are not -- and
// asking for that with a wall-clock budget is asking the machine to be busy on
// cue. `budget_exhausted` is the one signal indent.cpp reads to learn a run was
// short, so a stub that raises it exercises the same branch a spent deadline
// reaches, on any machine, every time.
struct CutAfterSyntax : Syntax {
  std::shared_ptr<Syntax> inner;
  int allow{0};
  int queries{0};

  std::string_view Language() const override { return inner->Language(); }
  std::span<const std::string> CaptureNames() const override { return inner->CaptureNames(); }
  void Sync(const PieceTable& table) override { inner->Sync(table); }
  void Paint(const PieceTable& table, Interval range, std::vector<CaptureId>& out) override {
    inner->Paint(table, range, out);
  }
  bool TimedOut() const override { return inner->TimedOut(); }
  bool InjectionsTruncated() const override { return inner->InjectionsTruncated(); }
  Index InjectionParses() const override { return inner->InjectionParses(); }
  bool Captures(const PieceTable& table, std::span<const std::string_view> query_files,
                Interval range, std::vector<Capture>& out, std::string& error,
                bool* budget_exhausted, bool keep_zero_width,
                const std::chrono::steady_clock::time_point* deadline) override {
    if (queries++ >= allow) {
      out.clear();
      error.clear();
      if (budget_exhausted != nullptr) *budget_exhausted = true;
      return true;
    }
    return inner->Captures(table, query_files, range, out, error, budget_exhausted,
                           keep_zero_width, deadline);
  }
  bool InLiteralOrComment(Index pos) override { return inner->InLiteralOrComment(pos); }
  Injected InInjectedRegion(const PieceTable& table, Index pos) override {
    return inner->InInjectedRegion(table, pos);
  }
};

}

// IND-M3 and IND-M4: what one keystroke of tree indentation is allowed to cost,
// and what it answers when it runs out part-way.
void TreeIndentBudgetsBoundTheKeystroke() {
  TEST_CASE("tree indent: one budget per keystroke, and a bounded baseline walk");

  // A line the user indented by hand, so that the hybrid's answer (that line's
  // whitespace plus a level) and the tree's absolute one (a level, from column
  // zero) are different strings and it is visible which one came back.
  const std::string manual = "        void f() {\n";
  const auto caret = static_cast<Index>(manual.size() - 1);

  // -- the budget running out between the queries ---------------------------
  //
  // The answer for the new line is in hand and the baseline that would have made
  // it relative is not. That is not a decline: the query that mattered ran, and
  // what it says is an indent level -- so the absolute answer stands, which is
  // the same answer this call gives on a buffer with no line worth measuring
  // against. Declining here would drop the tree's answer for a line it had
  // already worked out and hand the caret back to the bracket heuristic.
  {
    Fixture fixture = Open("a.c", manual);
    EXPECT_TRUE(fixture.ok());
    if (fixture.ok()) {
      // Unhurried, for the contrast: the baseline runs, and the answer is the
      // hand-typed eight columns with a level on top.
      EXPECT_EQ(ForNewline("a.c", manual, caret), std::string("            "));

      CutAfterSyntax cut;
      cut.inner = fixture.syntax;
      cut.allow = 1;
      std::string error;
      const auto got = TreeIndentForNewline(fixture.table, cut, caret, kFourSpaces, error);
      EXPECT_TRUE(got.has_value());
      EXPECT_EQ(got.value_or("<declined>"), std::string("    "));
      // Nothing is wrong with the document, so nothing is said about it.
      EXPECT_EQ(error, std::string{});
      // And it did stop where it was meant to: one query answered, one refused.
      EXPECT_EQ(cut.queries, 2);
    }
  }

  // The other half of the same rule: cut before the *first* query and there is
  // no answer to keep, so the call declines and the caller keeps its heuristic.
  {
    Fixture fixture = Open("a.c", manual);
    EXPECT_TRUE(fixture.ok());
    if (fixture.ok()) {
      CutAfterSyntax cut;
      cut.inner = fixture.syntax;
      std::string error;
      EXPECT_FALSE(TreeIndentForNewline(fixture.table, cut, caret, kFourSpaces, error).has_value());
      EXPECT_EQ(error, std::string{});
    }
  }

  // -- IND-M4: how far the hybrid may walk looking for a baseline -----------
  //
  // Blank lines are skipped without running a query, so nothing on that walk was
  // ever checked against the deadline: a caret under thirty thousand of them
  // measured every one on the way to the top of the buffer -- 350 ms for one
  // Return, per cursor, with the budget nominally in force. The walk is bounded
  // now, at kMaxBaselineLines, and the deadline is consulted on every step of
  // it whether or not the step runs a query.
  //
  // Which is what these expectations are: one buffer shape, a caret at the
  // bottom of a run of blank lines inside the braces, and the run grown across
  // the cap. Under it the hand-indented header line is found and inherited --
  // eight typed columns and a level on top. Past it the walk stops with nothing
  // to be relative to and the tree's absolute answer stands: one level, from
  // column zero, which is what the same caret gets in a file with no comparable
  // line anywhere above it. The cap is the only thing that changed between the
  // two, and the boundary is pinned rather than a round number either side of
  // it: the caret's own line is the first one walked, so the walk sees exactly
  // `kMaxBaselineLines` lines counting from it -- a header that many lines up
  // is still reached, and one line further up is not.
  {
    const auto buffer = [&manual](Index blanks) {
      return manual + std::string(static_cast<std::size_t>(blanks), '\n') + "}\n";
    };
    // The last blank line: its start is the byte after the run's second-to-last
    // newline, which is where a Return would be pressed.
    const auto caret_at = [&manual](Index blanks) {
      return static_cast<Index>(manual.size()) + blanks - 1;
    };

    const Index reaches = kMaxBaselineLines - 1;
    EXPECT_EQ(ForNewline("a.c", buffer(reaches), caret_at(reaches)), std::string("            "));
    EXPECT_EQ(ForNewline("a.c", buffer(reaches + 1), caret_at(reaches + 1)), std::string("    "));
    // And a caret a few lines under the header is untouched by any of it.
    EXPECT_EQ(ForNewline("a.c", buffer(4), caret_at(4)), std::string("            "));
  }

  // -- one line, one query, one keystroke -----------------------------------
  //
  // A line is answered once per keystroke however many carets are on it. Only
  // one caret per line can be *at* the end of its leading token, so the second
  // query this skips is one no shape reachable through the key path seems to
  // ask for -- it is a guard on the loop and not a fix for an answer, and
  // nothing outside can count queries anyway. What is pinned is the half that is
  // observable: two carets on the line still leave it exactly where one does,
  // with the same one entry remembered about the move.
  {
    const std::string before = "int main() {\n    if (x) {\n        work();\n        }}\n";
    Typist typist{"a.cpp", before, 0, true};
    std::vector<Selection> both{Selection{50, 50, -1}, Selection{51, 51, -1}};
    typist.ed.doc.selections.Replace(typist.ed.doc.table, both);
    typist.Type("}");

    Typist alone{"a.cpp", before, 0, true};
    alone.ed.doc.selections.Replace(alone.ed.doc.table, {Selection{51, 51, -1}});
    alone.Type("}");

    EXPECT_EQ(typist.Text(), std::string("int main() {\n    if (x) {\n        work();\n    }}\n"));
    EXPECT_EQ(alone.Text(), typist.Text());
    EXPECT_EQ(std::ssize(typist.ed.reindent.lines), std::ssize(alone.ed.reindent.lines));
    EXPECT_EQ(std::ssize(typist.ed.reindent.lines), Index{1});
  }
}

// ---------------------------------------------------------------------------
// Fuzz.
//
// Everything above pins an answer. Nothing above says what happens to a buffer
// nobody would write: token soup, a line past every scan cap, sixty-four levels
// of nothing, bytes that are not text at all, and the half-typed prefix of each
// of those. The engine runs on Enter, on a document the user is in the middle
// of breaking, so those are the ordinary inputs and not the exotic ones -- and
// what it owes them is not a particular column but a contract: an answer made
// only of whitespace, bounded by the guards indent.cpp states, the same answer
// twice, and a document it did not touch on the way.
//
// Seed-driven like the rest of the suite: every choice comes off main.cpp's
// Rng, so a failure names a KOI_TEST_SEED that reproduces it. KOI_INDENT_FUZZ_ITERS
// scales the run without changing what it does -- the default is what CI pays,
// a campaign sets it to a few hundred.
// ---------------------------------------------------------------------------

namespace {

// indent.cpp's rendering guards, restated. They are file-local there and there
// is no reason for them not to be, but the length bound below is derived from
// them rather than guessed, and a bound that is not derived from anything is
// not an assertion.
constexpr Index kMaxIndentLevels = 64;
constexpr Index kMaxAlignBytes = 1024;
// `IndentUnit` clamps `tab_width` to 16, so a level is at most 16 columns.
constexpr Index kMaxUnitWidth = 16;
constexpr Index kLevelBound = kMaxIndentLevels * kMaxUnitWidth;
// An `@align` is the document text left of the anchor, clipped to
// kMaxAlignBytes and re-spelled as whitespace of the same *width*: at most two
// columns per byte, which no encoding actually reaches and which is therefore a
// bound rather than a measurement.
constexpr Index kAlignBound = 2 * kMaxAlignBytes;

// Not the editor's 25 ms. A budget that expires mid-run is a legitimate answer
// -- nullopt -- but it is a *timing-dependent* one, and two calls that disagree
// about it are not a determinism failure. So the invariant calls run with a
// budget nothing reaches, and the real budget is exercised separately, where
// what is checked is that it comes back at all.
constexpr auto kUnhurried = std::chrono::seconds{30};

// Where "the same answer twice" is an assertion rather than a wish.
//
// The engine is a pure function of the document, the position and the style --
// with one exception that is not its own: `Syntax::Captures` carries a frame
// budget of its own, wall-clock, and entirely outside the budget handed to
// TreeIndentFor*. A scan it cuts off declines where an uncut one answers, and a
// hybrid baseline it cuts off makes the call fall back to the tree's absolute
// answer instead of the relative one -- so two identical calls really can
// differ, on the machine being busy and nothing else. Under a sanitizer, where
// everything is several times slower, that is reachable on the biggest
// generated buffers.
//
// Below this size the whole run is a handful of queries over a few hundred
// bytes and that budget is orders of magnitude away, so a difference there is
// the engine and equality is asserted. Above it the difference is counted and
// printed instead: asserting it would be asserting that CI is idle.
constexpr Index kDeterministicBelow = 4096;

// How many times over. Each unit is one generated buffer per language plus one
// typing session per language, so the default is a couple of seconds and
// KOI_INDENT_FUZZ_ITERS=200 is a campaign.
int FuzzIterations() {
  static const int iterations = [] {
    const char* env = std::getenv("KOI_INDENT_FUZZ_ITERS");
    if ((env == nullptr) || (*env == '\0')) return 2;
    const long long asked = std::strtoll(env, nullptr, 0);
    return static_cast<int>(std::clamp<long long>(asked, 1, 100000));
  }();
  return iterations;
}

bool FuzzIsVerbose() {
  const char* env = std::getenv("KOI_INDENT_FUZZ_ITERS");
  return (env != nullptr) && (*env != '\0');
}

// What the fuzz has done, so that a run that silently stopped generating is
// distinguishable from one that found nothing. Printed only when the scale was
// asked for.
struct FuzzTally {
  long long buffers{0};
  long long engine_calls{0};
  long long keystrokes{0};
  long long answers{0};
  long long declines{0};
  long long longest_answer{0};
  long long slowest_ms{0};
  // Repeat calls on a buffer past kDeterministicBelow that came back different.
  // Not a failure -- see the constant -- but a number that belongs in the run's
  // summary, because a build where it is suddenly large is a build where the
  // frame budget is biting on ordinary buffers.
  long long budget_flips{0};
  // Hybrid answer minus the tree's absolute one for the same line, in levels,
  // bucketed: [0], [1], [2], [3], [4..7], [8..15], [16+].
  std::array<long long, 7> deviation{};
};

FuzzTally g_tally;

void CountDeviation(Index levels) {
  const std::size_t bucket = (levels <= 3)   ? static_cast<std::size_t>(levels)
                             : (levels < 8)  ? 4
                             : (levels < 16) ? 5
                                             : 6;
  ++g_tally.deviation[bucket];
}

// -- generators -------------------------------------------------------------

// Token soup is not random text: it is the language's own vocabulary in an
// order the grammar never sanctioned, which is what a half-written file looks
// like to a parser and is where the recovery rules -- koi's own `(ERROR ...)`
// additions among them -- actually run.
constexpr std::array kCTokens{
    std::string_view{"int "},  std::string_view{"main"},  std::string_view{"void "},
    std::string_view{"if ("},  std::string_view{"else"},  std::string_view{"for ("},
    std::string_view{"while (\n"}, std::string_view{"switch (x) {"},
    std::string_view{"case 1:"}, std::string_view{"default:"}, std::string_view{"return "},
    std::string_view{"struct "}, std::string_view{"{"},      std::string_view{"}"},
    std::string_view{"("},      std::string_view{")"},      std::string_view{"["},
    std::string_view{"]"},      std::string_view{";"},      std::string_view{","},
    std::string_view{" = "},    std::string_view{"\"str\""}, std::string_view{"'c'"},
    std::string_view{"// c\n"}, std::string_view{"/* c */"}, std::string_view{"#include <x>\n"},
    std::string_view{"x"},      std::string_view{"y"},      std::string_view{"\n"},
    std::string_view{"\n    "}, std::string_view{"\n\t"},   std::string_view{"\\"},
    std::string_view{"->"},     std::string_view{"typedef "},
};

constexpr std::array kCppTokens{
    std::string_view{"class A {"}, std::string_view{"public:"}, std::string_view{"private:"},
    std::string_view{"template <"}, std::string_view{"typename T"}, std::string_view{">"},
    std::string_view{"namespace n {"}, std::string_view{"::"}, std::string_view{"auto "},
    std::string_view{"int "},    std::string_view{"if ("},   std::string_view{"else"},
    std::string_view{"for ("},   std::string_view{"return "}, std::string_view{"{"},
    std::string_view{"}"},       std::string_view{"("},      std::string_view{")"},
    std::string_view{"["},       std::string_view{"]"},      std::string_view{";"},
    std::string_view{","},       std::string_view{" = "},    std::string_view{"\"s\""},
    std::string_view{"R\"(raw)\""}, std::string_view{"// c\n"}, std::string_view{"/* c */"},
    std::string_view{"x"},       std::string_view{"\n"},     std::string_view{"\n    "},
    std::string_view{"\n\t"},    std::string_view{"try {"},  std::string_view{"catch (...) {"},
};

constexpr std::array kPythonTokens{
    std::string_view{"def f("},  std::string_view{"class C"}, std::string_view{"if "},
    std::string_view{"elif "},   std::string_view{"else:"},   std::string_view{"for "},
    std::string_view{" in "},    std::string_view{"while "},  std::string_view{"return "},
    std::string_view{"import x\n"}, std::string_view{"try:"}, std::string_view{"except:"},
    std::string_view{"finally:"}, std::string_view{"with "},  std::string_view{"lambda "},
    std::string_view{":"},       std::string_view{"("},       std::string_view{")"},
    std::string_view{"["},       std::string_view{"]"},       std::string_view{"{"},
    std::string_view{"}"},       std::string_view{","},       std::string_view{" = "},
    std::string_view{"# c\n"},   std::string_view{"\"\"\"doc\"\"\""}, std::string_view{"'s'"},
    std::string_view{"f\"{x}\""}, std::string_view{"x"},      std::string_view{"pass"},
    std::string_view{"yield "},  std::string_view{"\n"},      std::string_view{"\n    "},
    std::string_view{"\n        "}, std::string_view{"\\\n"},
};

constexpr std::array kRustTokens{
    std::string_view{"fn f("},   std::string_view{"let "},    std::string_view{"mut "},
    std::string_view{"match "},  std::string_view{"if "},     std::string_view{"else "},
    std::string_view{"for "},    std::string_view{"while "},  std::string_view{"loop "},
    std::string_view{"impl "},   std::string_view{"struct "}, std::string_view{"enum "},
    std::string_view{"trait "},  std::string_view{"pub "},    std::string_view{"use "},
    std::string_view{"mod "},    std::string_view{" => "},    std::string_view{" -> "},
    std::string_view{"::"},      std::string_view{"{"},       std::string_view{"}"},
    std::string_view{"("},       std::string_view{")"},       std::string_view{"["},
    std::string_view{"]"},       std::string_view{";"},       std::string_view{","},
    std::string_view{" = "},     std::string_view{"&"},       std::string_view{"'a"},
    std::string_view{"\"s\""},   std::string_view{"r#\"s\"#"}, std::string_view{"// c\n"},
    std::string_view{"#[derive(Debug)]\n"}, std::string_view{"macro_rules! m {"},
    std::string_view{"x"},       std::string_view{"\n"},      std::string_view{"\n    "},
    std::string_view{"async "},  std::string_view{"unsafe "}, std::string_view{"where "},
};

constexpr std::array kGoTokens{
    std::string_view{"package main\n"}, std::string_view{"func f("}, std::string_view{"import ("},
    std::string_view{"var "},    std::string_view{"const "},  std::string_view{"type "},
    std::string_view{"struct {"}, std::string_view{"interface {"}, std::string_view{"if "},
    std::string_view{"else "},   std::string_view{"for "},    std::string_view{" range "},
    std::string_view{"switch "}, std::string_view{"case 1:"}, std::string_view{"default:"},
    std::string_view{"return "}, std::string_view{"go "},     std::string_view{"defer "},
    std::string_view{"chan "},   std::string_view{"map["},    std::string_view{"{"},
    std::string_view{"}"},       std::string_view{"("},       std::string_view{")"},
    std::string_view{"["},       std::string_view{"]"},       std::string_view{";"},
    std::string_view{","},       std::string_view{" := "},    std::string_view{"\"s\""},
    std::string_view{"`raw`"},   std::string_view{"// c\n"},  std::string_view{"x"},
    std::string_view{"\n"},      std::string_view{"\n\t"},    std::string_view{"select {"},
};

constexpr std::array kBashTokens{
    std::string_view{"if "},     std::string_view{"then"},    std::string_view{"elif "},
    std::string_view{"else"},    std::string_view{"fi"},      std::string_view{"for i"},
    std::string_view{" in "},    std::string_view{"do"},      std::string_view{"done"},
    std::string_view{"while "},  std::string_view{"until "},  std::string_view{"case $x in"},
    std::string_view{"esac"},    std::string_view{";;"},      std::string_view{")"},
    std::string_view{"("},       std::string_view{"{"},       std::string_view{"}"},
    std::string_view{"[["},      std::string_view{"]]"},      std::string_view{"function f"},
    std::string_view{"echo "},   std::string_view{"$x"},      std::string_view{"${x}"},
    std::string_view{"\"s\""},   std::string_view{"'s'"},     std::string_view{"$(cmd)"},
    std::string_view{"# c\n"},   std::string_view{" | "},     std::string_view{" && "},
    std::string_view{";"},       std::string_view{"\n"},      std::string_view{"\n    "},
    std::string_view{"<<EOF\n"}, std::string_view{"EOF\n"},   std::string_view{"\\"},
};

constexpr std::array kYamlTokens{
    std::string_view{"a:"},      std::string_view{"b:"},      std::string_view{"- "},
    std::string_view{"  "},      std::string_view{"    "},    std::string_view{"\n"},
    std::string_view{"key: value"}, std::string_view{"- item"}, std::string_view{" |"},
    std::string_view{"\n  "},    std::string_view{" >"},      std::string_view{"\"s\""},
    std::string_view{"'s'"},     std::string_view{"# c\n"},   std::string_view{"["},
    std::string_view{", "},      std::string_view{"]"},       std::string_view{"{"},
    std::string_view{"}"},       std::string_view{"&anchor "}, std::string_view{"*alias"},
    std::string_view{"---\n"},   std::string_view{"...\n"},   std::string_view{"? "},
    std::string_view{": "},      std::string_view{"!!str "},
};

constexpr std::array kJsonTokens{
    std::string_view{"{"},       std::string_view{"}"},       std::string_view{"["},
    std::string_view{"]"},       std::string_view{","},       std::string_view{":"},
    std::string_view{"\"key\""}, std::string_view{"\"value\""}, std::string_view{"123"},
    std::string_view{"true"},    std::string_view{"false"},   std::string_view{"null"},
    std::string_view{" "},       std::string_view{"\n"},      std::string_view{"\n  "},
    std::string_view{"\\u0041"}, std::string_view{"\\\""},    std::string_view{"-1.5e10"},
};

constexpr std::array kNixTokens{
    std::string_view{"{"},       std::string_view{"}"},       std::string_view{"["},
    std::string_view{"]"},       std::string_view{"("},       std::string_view{")"},
    std::string_view{"let "},    std::string_view{" in "},    std::string_view{"rec "},
    std::string_view{"with "},   std::string_view{"inherit "}, std::string_view{"if "},
    std::string_view{" then "},  std::string_view{" else "},  std::string_view{"assert "},
    std::string_view{";"},       std::string_view{" = "},     std::string_view{":"},
    std::string_view{","},       std::string_view{"\"s\""},   std::string_view{"''str''"},
    std::string_view{"# c\n"},   std::string_view{"/* c */"}, std::string_view{"./path"},
    std::string_view{"x"},       std::string_view{"a.b"},     std::string_view{"${"},
    std::string_view{"import "}, std::string_view{"\n"},      std::string_view{"\n  "},
};

constexpr std::array kHtmlTokens{
    std::string_view{"<div>"},   std::string_view{"</div>"},  std::string_view{"<p>"},
    std::string_view{"</p>"},    std::string_view{"<span "},  std::string_view{"class=\"a\""},
    std::string_view{">"},       std::string_view{"/>"},      std::string_view{"<br/>"},
    std::string_view{"<!-- c -->"}, std::string_view{"<script>"}, std::string_view{"</script>"},
    std::string_view{"<style>"}, std::string_view{"</style>"}, std::string_view{"text"},
    std::string_view{"\n"},      std::string_view{"\n  "},    std::string_view{"<ul>"},
    std::string_view{"<li>"},    std::string_view{"</li>"},   std::string_view{"</ul>"},
    std::string_view{"<html>"},  std::string_view{"</html>"}, std::string_view{"&amp;"},
    std::string_view{"<"},       std::string_view{"<input "},
};

struct FuzzLanguage {
  std::string_view file;
  std::span<const std::string_view> tokens;
  // The brackets this language nests with, for the deep-nesting generator.
  std::string_view open;
  std::string_view close;
};

const std::array<FuzzLanguage, 10> kFuzzLanguages{
    FuzzLanguage{"main.c", kCTokens, "{", "}"},
    FuzzLanguage{"main.cpp", kCppTokens, "{", "}"},
    FuzzLanguage{"main.py", kPythonTokens, "(", ")"},
    FuzzLanguage{"lib.rs", kRustTokens, "{", "}"},
    FuzzLanguage{"ci.yaml", kYamlTokens, "[", "]"},
    FuzzLanguage{"main.go", kGoTokens, "{", "}"},
    FuzzLanguage{"package.json", kJsonTokens, "[", "]"},
    FuzzLanguage{"deploy.sh", kBashTokens, "(", ")"},
    FuzzLanguage{"flake.nix", kNixTokens, "{", "}"},
    FuzzLanguage{"index.html", kHtmlTokens, "<div>", "</div>"},
};

std::string TokenSoup(const FuzzLanguage& lang, Rng& rng, Index count) {
  std::string out;
  for (Index i = 0; i < count; ++i) {
    out += lang.tokens[static_cast<std::size_t>(rng.Pick(0, std::ssize(lang.tokens) - 1))];
  }
  return out;
}

// Bytes, not text. NUL and lone continuation bytes are in here on purpose: the
// piece table stores bytes, a file on disk can hold these, and the grapheme
// walk `@align` renders through is the one place where an ill-formed sequence
// could walk off the end of a string.
void AppendRandomBytes(Rng& rng, std::string& out, Index count) {
  for (Index i = 0; i < count; ++i) {
    switch (rng.Pick(0, 10)) {
      case 0: out.push_back('\0'); break;
      case 1: out.push_back(static_cast<char>(rng.Pick(1, 31))); break;
      case 2:
      case 3:
      case 4: out.push_back(static_cast<char>(rng.Pick(32, 126))); break;
      case 5: {  // two bytes, one column
        const char two[] = {static_cast<char>(0xC3), static_cast<char>(rng.Pick(0x80, 0xBF))};
        out.append(two, 2);
        break;
      }
      case 6: {  // three bytes, two columns
        const char three[] = {static_cast<char>(0xE6), static_cast<char>(0x97),
                              static_cast<char>(0xA0 + rng.Pick(0, 15))};
        out.append(three, 3);
        break;
      }
      case 7: out.append(kFamily); break;  // four bytes, and a cluster of three of them
      case 8: out.push_back(static_cast<char>(rng.Pick(0x80, 0xBF))); break;  // ill-formed
      case 9: out.append(kEAcute); break;
      default: out.push_back('\n'); break;
    }
  }
}

enum class Shape : std::uint8_t {
  kSoup,
  kBytes,
  kSpliced,
  kLongLine,
  kDeepNesting,
  kLongAnchor,
  kTruncated,
  kMisIndented,
  kCount,
};

// Every line's leading whitespace replaced by a random amount of it. This is
// the hybrid heuristic's adversary: it measures the tree's answer against the
// whitespace a nearby line actually has, so a buffer where that whitespace
// means nothing is the buffer where hybrid and absolute come apart.
std::string MisIndent(std::string_view text, Rng& rng) {
  std::string out;
  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t nl = text.find('\n', at);
    const std::size_t end = (nl == std::string_view::npos) ? text.size() : nl;
    std::size_t first = at;
    while ((first < end) && ((text[first] == ' ') || (text[first] == '\t'))) ++first;
    const Index width = rng.Pick(0, 16);
    if (rng.Pick(0, 3) == 0) {
      out.append(static_cast<std::size_t>(width), '\t');
    } else {
      out.append(static_cast<std::size_t>(width), ' ');
    }
    out.append(text.substr(first, end - first));
    if (nl == std::string_view::npos) break;
    out.push_back('\n');
    at = nl + 1;
  }
  return out;
}

std::string_view NameOf(Shape shape) {
  switch (shape) {
    case Shape::kSoup: return "soup";
    case Shape::kBytes: return "bytes";
    case Shape::kSpliced: return "spliced";
    case Shape::kLongLine: return "long-line";
    case Shape::kDeepNesting: return "deep-nesting";
    case Shape::kLongAnchor: return "long-anchor";
    case Shape::kTruncated: return "truncated";
    case Shape::kMisIndented: return "mis-indented";
    default: return "?";
  }
}

std::string GenerateBuffer(const FuzzLanguage& lang, Shape shape, Rng& rng) {
  switch (shape) {
    case Shape::kSoup: return TokenSoup(lang, rng, rng.Pick(4, 120));
    case Shape::kBytes: {
      std::string out;
      AppendRandomBytes(rng, out, rng.Pick(16, 600));
      return out;
    }
    case Shape::kSpliced: {
      std::string out = TokenSoup(lang, rng, rng.Pick(8, 60));
      const std::size_t cut = static_cast<std::size_t>(rng.Pick(0, std::ssize(out)));
      std::string tail = out.substr(cut);
      out.resize(cut);
      AppendRandomBytes(rng, out, rng.Pick(4, 60));
      out += tail;
      return out;
    }
    case Shape::kLongLine: {
      // Past kMaxWhitespaceScan (4096) in both the leading run and the line
      // itself, so both of indent.cpp's scan caps are on the wrong side of the
      // answer.
      std::string out = TokenSoup(lang, rng, 6);
      out += '\n';
      out.append(static_cast<std::size_t>(rng.Pick(4100, 6000)), (rng.Pick(0, 1) == 0) ? ' ' : '\t');
      while (std::ssize(out) < 9000) out += TokenSoup(lang, rng, 1);
      out += '\n';
      out += TokenSoup(lang, rng, 4);
      return out;
    }
    case Shape::kDeepNesting: {
      // Past kMaxIndentLevels (64), so the level cap decides the answer rather
      // than the count.
      const Index depth = rng.Pick(70, 200);
      std::string out;
      for (Index i = 0; i < depth; ++i) {
        out.append(static_cast<std::size_t>(std::min<Index>(i, 60)), ' ');
        out += lang.open;
        out += '\n';
      }
      out += TokenSoup(lang, rng, 3);
      if (rng.Pick(0, 1) == 0) {
        for (Index i = 0; i < depth; ++i) {
          out += '\n';
          out += lang.close;
        }
      }
      return out;
    }
    case Shape::kLongAnchor: {
      // An `@align` anchor further into its line than kMaxAlignBytes (1024), so
      // the clip is what produces the string rather than the line.
      std::string out = TokenSoup(lang, rng, 2);
      out += '\n';
      out.append(static_cast<std::size_t>(rng.Pick(1100, 2400)), 'a');
      out += "(x,\n";
      out += ")\n";
      return out;
    }
    case Shape::kTruncated: {
      const std::string whole = TokenSoup(lang, rng, rng.Pick(8, 60));
      return whole.substr(0, static_cast<std::size_t>(rng.Pick(0, std::ssize(whole))));
    }
    case Shape::kMisIndented: return MisIndent(TokenSoup(lang, rng, rng.Pick(8, 80)), rng);
    default: return {};
  }
}

// -- invariants -------------------------------------------------------------

bool OnlyWhitespace(std::string_view s) {
  return std::ranges::all_of(s, [](char c) { return (c == ' ') || (c == '\t'); });
}

// Where a line's text stops, short of its terminator. commands.cpp and
// selection.cpp each keep one of these to themselves; this is the same offset
// and is here rather than reached for.
Index LineEndOf(const PieceTable& table, Index line) {
  const Interval content = LineContentRange(table, line);
  return content.empty() ? LineStart(table, line) : (content.back() + 1);
}

// The deepest leading-whitespace run in the buffer. The hybrid and opaque paths
// hand back a real line's own indentation, so the bound on an answer's length
// is that plus the level cap -- not a constant, and not the document's length
// either.
Index LongestLeadingWhitespace(const PieceTable& table) {
  Index most = 0;
  const Index lines = LineCount(table);
  for (Index line = 0; line < lines; ++line) {
    const Index start = LineStart(table, line);
    Index at = start;
    char c = 0;
    while (ByteAt(table, at, c) && ((c == ' ') || (c == '\t'))) ++at;
    most = std::max(most, at - start);
  }
  return most;
}

// Everything an answer owes, whatever it is. `what` and the seed are printed
// rather than the buffer: a 9 kB soup in a failure message is not a repro, the
// seed is.
void CheckAnswer(const std::optional<std::string>& got, Index leading, std::string_view what,
                 unsigned long long seed) {
  ++g_tally.engine_calls;
  if (!got.has_value()) {
    ++g_tally.declines;
    return;
  }
  ++g_tally.answers;
  g_tally.longest_answer = std::max<long long>(g_tally.longest_answer, std::ssize(*got));
  const Index bound = std::max(kAlignBound, leading) + kLevelBound;
  if (!OnlyWhitespace(*got) || (std::ssize(*got) > bound)) {
    std::cerr << "  fuzz: " << what << " seed " << seed << " length " << got->size() << " bound "
              << bound << "\n";
  }
  EXPECT_TRUE(OnlyWhitespace(*got));
  EXPECT_TRUE(std::ssize(*got) <= bound);
}

// Asking a question must not be an edit. Compared over the whole document and
// not only the revision counter: a mutation that also bumped the counter would
// pass a counter check, and a mutation that did not bump it is worse.
struct DocProbe {
  Index revision{0};
  Index length{0};
  Index lines{0};
  std::size_t contents{0};

  friend bool operator==(const DocProbe&, const DocProbe&) = default;
  friend std::ostream& operator<<(std::ostream& os, const DocProbe& probe) {
    return os << "rev " << probe.revision << ", " << probe.length << " bytes, " << probe.lines
              << " lines, contents " << probe.contents;
  }
};

DocProbe ProbeOf(const PieceTable& table) {
  return DocProbe{table.revision, DocLength(table), LineCount(table),
                  std::hash<std::string>{}(AssembleDocContents(table))};
}

// Positions worth asking about: the ends, every line's head, interiors, a byte
// inside a multi-byte cluster, and offsets outside the document -- which the
// entry points clamp, and clamping is a promise like any other.
std::vector<Index> FuzzPositions(const PieceTable& table, Rng& rng, int extra) {
  const Index len = DocLength(table);
  const Index lines = LineCount(table);
  std::vector<Index> out{0, len, -1, len + 7};
  if (len > 0) out.push_back(len - 1);
  for (int i = 0; i < extra; ++i) {
    switch (rng.Pick(0, 3)) {
      case 0: out.push_back(rng.Pick(0, len)); break;
      case 1: out.push_back(LineStart(table, rng.Pick(0, std::max<Index>(0, lines - 1)))); break;
      case 2: {
        Index at = rng.Pick(0, len);
        char c = 0;
        for (Index step = 0; (step < 128) && (at < len); ++step, ++at) {
          if (ByteAt(table, at, c) && ((static_cast<unsigned char>(c) & 0xC0) == 0x80)) break;
        }
        out.push_back(std::min(at, len));
        break;
      }
      default: out.push_back(LineEndOf(table, rng.Pick(0, std::max<Index>(0, lines - 1))));
        break;
    }
  }
  return out;
}

// One generated buffer, swept. Both entry points, twice each for determinism,
// with the document checked for having been left alone.
void SweepBuffer(std::string_view file, const std::string& text, Rng& rng, unsigned long long seed,
                 std::string_view what) {
  Fixture fixture = Open(file, text);
  EXPECT_TRUE(fixture.ok());
  if (!fixture.ok()) return;
  ++g_tally.buffers;

  const Index leading = LongestLeadingWhitespace(fixture.table);
  const DocProbe before = ProbeOf(fixture.table);
  const IndentStyle style = (rng.Pick(0, 3) == 0) ? kTabs : kFourSpaces;
  const bool strict = (DocLength(fixture.table) <= kDeterministicBelow);

  for (const Index pos : FuzzPositions(fixture.table, rng, 6)) {
    std::string error;
    const auto first =
        TreeIndentForNewline(fixture.table, *fixture.syntax, pos, style, error, kUnhurried);
    const auto again =
        TreeIndentForNewline(fixture.table, *fixture.syntax, pos, style, error, kUnhurried);
    CheckAnswer(first, leading, what, seed);
    ++g_tally.engine_calls;
    if (!(first == again)) {
      ++g_tally.budget_flips;
      if (strict) {
        std::cerr << "  fuzz: newline not deterministic in " << what << " seed " << seed << " at "
                  << pos << " of " << DocLength(fixture.table) << ": ["
                  << (first.has_value() ? *first : std::string{"<declined>"}) << "] then ["
                  << (again.has_value() ? *again : std::string{"<declined>"}) << "]\n";
      }
    }
    if (strict) EXPECT_TRUE(first == again);
    EXPECT_EQ(ProbeOf(fixture.table), before);

    const Index line = LineAt(fixture.table, std::clamp<Index>(pos, 0, DocLength(fixture.table)));
    bool outdent_token = false;
    const auto by_line = TreeIndentForLine(fixture.table, *fixture.syntax, line, style, error,
                                           kUnhurried, &outdent_token);
    const auto by_line_again = TreeIndentForLine(fixture.table, *fixture.syntax, line, style, error,
                                                 kUnhurried, &outdent_token);
    CheckAnswer(by_line, leading, what, seed);
    ++g_tally.engine_calls;
    if (!(by_line == by_line_again)) {
      ++g_tally.budget_flips;
      if (strict) {
        std::cerr << "  fuzz: line not deterministic in " << what << " seed " << seed << " at line "
                  << line << " of " << LineCount(fixture.table) << ": ["
                  << (by_line.has_value() ? *by_line : std::string{"<declined>"}) << "] then ["
                  << (by_line_again.has_value() ? *by_line_again : std::string{"<declined>"})
                  << "]\n";
      }
    }
    if (strict) EXPECT_TRUE(by_line == by_line_again);
    EXPECT_EQ(ProbeOf(fixture.table), before);
  }

  // The real budget, on the same buffer. What is checked is not the answer but
  // that there is one at all within a wall clock an order of magnitude past the
  // 25 ms it was given -- loose on purpose, because this runs on shared CI and
  // under sanitizers, and what it separates is a budget honoured from a run
  // that never returns.
  std::string error;
  const long long took = MillisecondsOf([&] {
    (void)TreeIndentForNewline(fixture.table, *fixture.syntax, DocLength(fixture.table), style,
                               error);
  });
  ++g_tally.engine_calls;
  g_tally.slowest_ms = std::max(g_tally.slowest_ms, took);
  if (took > 2000) std::cerr << "  fuzz: " << what << " seed " << seed << " took " << took << "ms\n";
  EXPECT_TRUE(took <= 2000);
}

// -- the real key path ------------------------------------------------------

// Weighted towards what actually decides an indent: brackets, colons, quotes
// and newlines, with letters in between so that the outdent-token trigger --
// which only fires on a line holding one run of non-blank bytes -- is both
// reached and left.
char FuzzChar(Rng& rng) {
  static constexpr std::string_view kHeavy = "{}()[]<>:;,\"'\n\n\t ";
  static constexpr std::string_view kWords = "abcdefiklmnorstuwxyz";
  if (rng.Pick(0, 2) != 0) {
    return kHeavy[static_cast<std::size_t>(rng.Pick(0, std::ssize(kHeavy) - 1))];
  }
  return kWords[static_cast<std::size_t>(rng.Pick(0, std::ssize(kWords) - 1))];
}

// The selection contract SelectionSet::Replace establishes and every command is
// entitled to assume: in bounds, sorted, disjoint, primary among them.
std::string SelectionInvariants(const Editor& ed) {
  const PieceTable& table = ed.doc.table;
  const Index len = DocLength(table);
  const std::vector<Selection>& ranges = ed.doc.selections.Ranges();
  if (ranges.empty()) return "no selections";
  if (ed.doc.selections.PrimaryIndex() >= ranges.size()) return "primary is not a selection";
  Index previous = -1;
  for (const Selection& s : ranges) {
    if ((s.anchor < 0) || (s.head < 0)) return "a selection is negative";
    if ((s.anchor > len) || (s.head > len)) return "a selection is past the end";
    if (s.From() < previous) return "selections are not in order";
    previous = s.To();
  }
  return {};
}

// A typing session: real keys through HandleKeyInput, so the keymap, auto-pairs,
// the pending-chord batching, the newline path and the re-indent trigger are all
// in it. Ends by walking the undo chain to the bottom, which has to give back
// the bytes it started with -- a re-indent that moved a line outside the
// keystroke's own undo group would fail exactly there.
void TypingSession(const FuzzLanguage& lang, Rng& rng, bool auto_pairs, int keys) {
  const std::string start = GenerateBuffer(lang, Shape::kSoup, rng);
  Typist typist{lang.file, start, 0, auto_pairs};
  typist.ed.doc.insert_spaces = (rng.Pick(0, 3) != 0);
  typist.ed.doc.tab_width = rng.Pick(2, 8);

  // Two carets some of the time: the re-indent path maps every range and not
  // just the ones past the edit, which is a rule only a second caret can break.
  if (rng.Pick(0, 2) == 0) {
    const Index len = DocLength(typist.ed.doc.table);
    std::vector<Selection> ranges;
    const Index a = rng.Pick(0, len);
    const Index b = rng.Pick(0, len);
    ranges.push_back(Selection{a, a, -1});
    ranges.push_back(Selection{b, b, -1});
    typist.ed.doc.selections.Replace(typist.ed.doc.table, std::move(ranges));
  } else {
    const Index at = rng.Pick(0, DocLength(typist.ed.doc.table));
    typist.ed.doc.selections.Set(Selection{at, at, -1});
  }

  // Modes are changed through the commands the keymap binds to them, not by
  // assigning to `ed.mode`: normal mode owes every caret a grapheme under it,
  // and it is `normal_mode` that arranges that.
  for (int i = 0; i < keys; ++i) {
    switch (rng.Pick(0, 15)) {
      case 0: RunCommands(typist.ed, {"normal_mode", "undo", "insert_mode"}); break;
      case 1: RunCommands(typist.ed, {"normal_mode", "redo", "insert_mode"}); break;
      case 2: RunCommands(typist.ed, {"normal_mode", "insert_mode"}); break;
      case 3: RunCommands(typist.ed, {"insert_mode"}); break;
      default: {
        const char c = FuzzChar(rng);
        typist.Type(std::string_view{&c, 1});
        ++g_tally.keystrokes;
        break;
      }
    }
    if (const std::string broke = SelectionInvariants(typist.ed); !broke.empty()) {
      std::cerr << "  fuzz: selections " << broke << " in " << lang.file << " seed " << rng.seed
                << "\n";
    }
    EXPECT_EQ(SelectionInvariants(typist.ed), std::string{});
  }
  EXPECT_EQ(EditorInvariants(typist.ed), std::string{});

  // All the way back. Consecutive typing coalesces, so the count of steps is
  // not the count of keys and is not what this checks; what it checks is the
  // bytes at the bottom.
  for (int guard = 0; CanUndo(typist.ed.doc.table) && (guard < 4096); ++guard) {
    (void)Undo(typist.ed.doc.table);
  }
  if (typist.Text() != start) {
    std::cerr << "  fuzz: undo did not restore " << lang.file << " seed " << rng.seed << "\n";
  }
  EXPECT_EQ(typist.Text(), start);
}

}

void TreeIndentFuzz(Rng& rng) {
  TEST_CASE("tree indent fuzz");

  const int iterations = FuzzIterations();
  g_tally = FuzzTally{};
  const auto started = std::chrono::steady_clock::now();

  // -- generated buffers, both entry points --------------------------------
  for (int round = 0; round < iterations; ++round) {
    for (const FuzzLanguage& lang : kFuzzLanguages) {
      const auto shape =
          static_cast<Shape>(rng.Pick(0, static_cast<Index>(Shape::kCount) - 1));
      SweepBuffer(lang.file, GenerateBuffer(lang, shape, rng), rng, rng.seed,
                  std::string{lang.file} + "/" + std::string{NameOf(shape)});
    }
    // The two languages that ship a grammar and no `indents.scm`. Whatever is in
    // the buffer, the engine has nothing to say and must say so rather than
    // guessing -- this is the contract commands.cpp's fallback is built on.
    for (const std::string_view file : {std::string_view{"README.md"},
                                        std::string_view{"changes.diff"}}) {
      const std::string text = GenerateBuffer(kFuzzLanguages[0], Shape::kSpliced, rng);
      Fixture fixture = Open(file, text);
      EXPECT_TRUE(fixture.ok());
      if (!fixture.ok()) continue;
      std::string error;
      const Index len = DocLength(fixture.table);
      for (const Index pos : {Index{0}, len / 2, len}) {
        EXPECT_FALSE(TreeIndentForNewline(fixture.table, *fixture.syntax, pos, kFourSpaces, error)
                         .has_value());
        EXPECT_FALSE(TreeIndentForLine(fixture.table, *fixture.syntax,
                                       LineAt(fixture.table, pos), kFourSpaces, error)
                         .has_value());
      }
      EXPECT_EQ(error, std::string{});
    }
  }

  // -- every prefix of a snippet -------------------------------------------
  //
  // The mid-keystroke states, exhaustively rather than sampled: a buffer cut
  // one byte before its closer is the shape the engine spends its whole life
  // in, and the zero-width nodes the parser invents to recover live in exactly
  // those cuts.
  {
    static constexpr std::array kPrefixed{
        std::pair{std::string_view{"main.c"}, std::string_view{"int main() {\n  if (x) {\n    f();\n  }\n}\n"}},
        std::pair{std::string_view{"main.py"}, std::string_view{"def f():\n    if x:\n        g()\n"}},
        std::pair{std::string_view{"lib.rs"}, std::string_view{"fn f() {\n    let x = match y {\n        A => 1,\n    };\n}\n"}},
        std::pair{std::string_view{"deploy.sh"}, std::string_view{"if true; then\n  for i in a; do\n    echo x\n  done\nfi\n"}},
        std::pair{std::string_view{"ci.yaml"}, std::string_view{"a:\n  b:\n    - c\n    - d\n"}},
    };
    for (const auto& [file, whole] : kPrefixed) {
      for (std::size_t cut = 0; cut <= whole.size(); ++cut) {
        Fixture fixture = Open(file, whole.substr(0, cut));
        if (!fixture.ok()) continue;
        const Index leading = LongestLeadingWhitespace(fixture.table);
        const DocProbe before = ProbeOf(fixture.table);
        std::string error;
        CheckAnswer(TreeIndentForNewline(fixture.table, *fixture.syntax,
                                         static_cast<Index>(cut), kFourSpaces, error, kUnhurried),
                    leading, "prefix", rng.seed);
        CheckAnswer(TreeIndentForLine(fixture.table, *fixture.syntax,
                                      LineCount(fixture.table) - 1, kFourSpaces, error, kUnhurried),
                    leading, "prefix", rng.seed);
        EXPECT_EQ(ProbeOf(fixture.table), before);
      }
    }
  }

  // -- zero-width nodes at EOF ---------------------------------------------
  //
  // `scan_to` is clamped to the document length, and every one of these ends at
  // a byte where the parser has invented a token that spans nothing: the caret,
  // the last byte and the clamp are all the same offset. Pinned as exact
  // answers because these are the shapes the feature exists for, and a
  // regression here is a regression in the ordinary case.
  {
    static constexpr std::array kEofCases{
        IndentCase{"an unterminated block at eof", "main.c", "int main() {", 12, "    "},
        IndentCase{"a header at eof", "main.py", "def f():", 8, "    "},
        IndentCase{"a nested header at eof", "main.py", "def f():\n    if x:", 18, "        "},
        IndentCase{"a bash header at eof", "deploy.sh", "if true; then", 13, "    "},
    };
    RunNewlineCases(kEofCases);

    // KNOWN(fuzz): four EOF shapes where the answer is a level short of the one
    // the same buffer gets once the construct is closed. All four are the same
    // defect seen from four sides -- a construct whose *last* token is missing
    // at the end of the document leaves the grammar with no node to recover
    // into, so the query matches nothing and the hybrid baseline hands back the
    // line above verbatim. Closing the construct, which is what koi's own
    // auto-pairs do, gives the level in every case; these are what an editor
    // with auto-pairs off gets. Pinned at what happens today so the suite stays
    // green and a change to any of them is visible.
    static constexpr std::array kEofKnown{
        // `f(` with no `)`: no `argument_list` and therefore no `@align`, so the
        // call contributes nothing and only the enclosing block's level is left.
        // With the `)` there -- `f(a,\n)` -- the same position aligns to column 8.
        IndentCase{"KNOWN: an unterminated call at eof gains no level", "main.c",
                   "int main() {\n    f(", 19, "    "},
        IndentCase{"KNOWN: an unterminated subscript at eof gains no level", "main.c",
                   "int x[] = {\n    a[", 18, "    "},
        // A brace-less body is a *following* sibling that the header rule
        // descends into; at EOF there is no sibling at all, not even the
        // zero-width one an already-typed `;` below would give. With
        // `        work();` under it the same position answers eight.
        IndentCase{"KNOWN: a brace-less if at eof opens no body", "main.c",
                   "int main() {\n    if (x)", 23, "    "},
        // rust's `{` is not c's: with nothing after it the whole line recovers
        // as an ERROR that rust's query names nowhere, so the block opens no
        // level -- where `int main() {` above opens one. `fn f() {\n}` answers
        // four.
        IndentCase{"KNOWN: an unterminated rust block at eof opens no level", "lib.rs",
                   "fn f() {", 8, ""},
    };
    RunNewlineCases(kEofKnown);
    // The same buffers with their closers, so that the four above read as a
    // limit of recovery at EOF and not as the feature being broken.
    EXPECT_EQ(ForNewline("main.c", "int main() {\n    f(a,\n    );\n}\n", 21),
              std::string("      "));
    EXPECT_EQ(ForNewline("main.c", "int main() {\n    if (x)\n        work();\n}", 23),
              std::string("        "));
    EXPECT_EQ(ForNewline("lib.rs", "fn f() {\n}\n", 8), std::string("    "));

    // And every bracket, in every language, at the very end of the buffer --
    // asked for the contract rather than for a column, because what a grammar
    // recovers a lone `[` into is the grammar's business.
    for (const FuzzLanguage& lang : kFuzzLanguages) {
      for (const std::string_view tail : {"{", "(", "[", "<", "\"", "'", "{(", "([", "{[(",
                                          "{\n", "(\n    ", "\\"}) {
        const std::string text = std::string{"x"} + std::string{tail};
        Fixture fixture = Open(lang.file, text);
        if (!fixture.ok()) continue;
        std::string error;
        const DocProbe before = ProbeOf(fixture.table);
        CheckAnswer(TreeIndentForNewline(fixture.table, *fixture.syntax, DocLength(fixture.table),
                                         kFourSpaces, error, kUnhurried),
                    LongestLeadingWhitespace(fixture.table), "eof bracket", rng.seed);
        CheckAnswer(TreeIndentForLine(fixture.table, *fixture.syntax,
                                      LineCount(fixture.table) - 1, kFourSpaces, error, kUnhurried),
                    LongestLeadingWhitespace(fixture.table), "eof bracket", rng.seed);
        EXPECT_EQ(ProbeOf(fixture.table), before);
      }
    }
  }

  // -- degenerate alignment -------------------------------------------------
  //
  // c anchors an `argument_list` on its *first argument*, so the align is the
  // line's text up to that byte -- which makes an argument standing at the head
  // of its own line the degenerate case: no text to its left, an empty align,
  // and an answer of column zero however deep the block around it is. An anchor
  // further into its line than kMaxAlignBytes is clipped to the guard, and the
  // clip is the whole reason a generated file cannot make one keystroke
  // allocate a megabyte.
  {
    // The first argument at column zero: an empty align beats the enclosing
    // block's level, because an alignment is an absolute column and not an
    // addition to one.
    EXPECT_EQ(ForNewline("main.c", "int main() {\nfoo(\na,\n);\n}\n", 20), std::string(""));
    // The same shape with the call on the line: the align is "foolish(", eight
    // columns, and not the four the block alone would have given.
    EXPECT_EQ(ForNewline("main.c", "int main() {\nfoolish(a,\n);\n}\n", 23),
              std::string("        "));
    EXPECT_EQ(ForNewline("main.py", "def f():\n    g(a,\n    )\n", 17), std::string("      "));

    std::string wide = "int main() {\n";
    wide.append(1500, 'a');
    wide += "(x,\n);\n}\n";
    // End of the line the call opens: line 1 begins at 13, the `(` is at 1513
    // and the anchor -- the `x` -- at 1514, which is 1501 bytes into its line.
    const std::string got = ForNewline("main.c", wide, 13 + 1500 + 3);
    EXPECT_TRUE(OnlyWhitespace(got));
    // Clipped to the guard, plus whatever levels are stacked on it -- and never
    // the 1501 columns the line actually holds to the left of the anchor.
    EXPECT_TRUE(std::ssize(got) <= (kAlignBound + kLevelBound));
    EXPECT_TRUE(std::ssize(got) < 1501);
    // And it really is the clip that produced it: a bound nothing reached would
    // pass the line above just as well.
    EXPECT_TRUE(std::ssize(got) > 512);
  }

  // -- ERROR-heavy soup does not compound -----------------------------------
  //
  // koi's own recovery rules -- rust's `(ERROR "let" "=")`, c's `(ERROR "{")`
  // -- capture nodes the grammar built out of text that does not parse, and an
  // ERROR node can span the rest of the file. What must not happen is an indent
  // that grows line over line: two hundred lines, each appended at the answer
  // the engine gave for the one above it, and the answer stays under a hundred
  // levels rather than tracking the line count.
  {
    static constexpr std::array kErrorProne{
        std::pair{std::string_view{"lib.rs"}, std::string_view{"let x = "}},
        std::pair{std::string_view{"main.c"}, std::string_view{"{ if ("}},
        std::pair{std::string_view{"main.py"}, std::string_view{"if x:"}},
    };
    for (const auto& [file, seed_line] : kErrorProne) {
      // Grown by editing rather than by reopening: the document the engine sees
      // is the one an editor sees, incrementally reparsed, and two hundred
      // whole-file parses would be most of this suite's runtime.
      Fixture fixture = Open(file, "");
      if (!fixture.ok()) continue;
      ++g_tally.buffers;
      std::string indent;
      Index widest = 0;
      for (int line = 0; line < 200; ++line) {
        const std::string next =
            indent + std::string{seed_line} + TokenSoup(kFuzzLanguages[0], rng, 1) + "\n";
        if (Insert(next, DocLength(fixture.table), fixture.table)) break;
        fixture.syntax->Sync(fixture.table);
        std::string error;
        const auto got = TreeIndentForNewline(fixture.table, *fixture.syntax,
                                              DocLength(fixture.table), kFourSpaces, error,
                                              kUnhurried);
        ++g_tally.engine_calls;
        if (!got.has_value()) break;
        EXPECT_TRUE(OnlyWhitespace(*got));
        indent = *got;
        widest = std::max<Index>(widest, DisplayWidth(indent, 4));
      }
      if (widest > (100 * 4)) {
        std::cerr << "  fuzz: " << file << " ran away to " << widest << " columns\n";
      }
      EXPECT_TRUE(widest <= (100 * 4));
    }
  }

  // -- the hybrid baseline under randomised indentation ---------------------
  //
  // Every line's whitespace replaced by an amount that means nothing. The
  // hybrid answer is then anchored to a lie, which is by design -- deliberate
  // manual indentation is exactly what it is protecting -- so what is asserted
  // is the contract and not the column, and how far it drifts from the tree's
  // absolute answer is counted rather than bounded.
  for (int round = 0; round < iterations; ++round) {
    for (const FuzzLanguage& lang : kFuzzLanguages) {
      const std::string text = MisIndent(TokenSoup(lang, rng, rng.Pick(20, 60)), rng);
      Fixture fixture = Open(lang.file, text);
      if (!fixture.ok()) continue;
      ++g_tally.buffers;
      const Index leading = LongestLeadingWhitespace(fixture.table);
      const DocProbe before = ProbeOf(fixture.table);
      const Index lines = LineCount(fixture.table);
      // Sampled rather than swept: each line here is two whole engine runs and
      // the drift they measure is a distribution, not a per-line fact.
      const Index step = std::max<Index>(1, lines / 12);
      for (Index line = 0; (line + 1) < lines; line += step) {
        std::string error;
        const auto hybrid =
            TreeIndentForNewline(fixture.table, *fixture.syntax,
                                 LineEndOf(fixture.table, line), kFourSpaces, error,
                                 kUnhurried);
        const auto absolute = TreeIndentForLine(fixture.table, *fixture.syntax, line + 1,
                                                kFourSpaces, error, kUnhurried);
        CheckAnswer(hybrid, leading, "mis-indented", rng.seed);
        CheckAnswer(absolute, leading, "mis-indented", rng.seed);
        if (hybrid.has_value() && absolute.has_value()) {
          const Index drift =
              std::abs(DisplayWidth(*hybrid, 4) - DisplayWidth(*absolute, 4)) / 4;
          CountDeviation(drift);
        }
      }
      EXPECT_EQ(ProbeOf(fixture.table), before);
    }
  }

  // -- the real key path ----------------------------------------------------
  for (int round = 0; round < iterations; ++round) {
    for (const FuzzLanguage& lang : kFuzzLanguages) {
      TypingSession(lang, rng, (round % 2) == 0, 24);
    }
  }

  // -- ReindentMemory -------------------------------------------------------
  //
  // The one promise the trigger makes to whoever is typing: a line whose first
  // token is not what dedents it is never moved, however the keystrokes around
  // it are interleaved. `work` is not an outdent token in any of these
  // languages, so wherever it was typed is where it stays -- across undo, redo,
  // a buffer switch that resets the memory, and a second caret elsewhere.
  {
    static constexpr std::array kHandIndented{
        std::pair{std::string_view{"main.c"}, std::string_view{"int main() {\nX\n}\n"}},
        std::pair{std::string_view{"lib.rs"}, std::string_view{"fn f() {\nX\n}\n"}},
        std::pair{std::string_view{"main.go"}, std::string_view{"func f() {\nX\n}\n"}},
        std::pair{std::string_view{"deploy.sh"}, std::string_view{"if true; then\nX\nfi\n"}},
    };
    for (int round = 0; round < iterations; ++round) {
      for (const auto& [file, shape] : kHandIndented) {
        const Index width = rng.Pick(0, 12);
        const std::string hand(static_cast<std::size_t>(width), ' ');
        std::string text{shape};
        text.replace(text.find('X'), 1, hand);
        const Index caret = static_cast<Index>(text.find('\n') + 1) + width;

        Typist typist{file, text, caret, (rng.Pick(0, 1) == 0)};
        for (int step = 0; step < 12; ++step) {
          switch (rng.Pick(0, 7)) {
            case 0: RunCommands(typist.ed, {"normal_mode", "undo", "redo", "insert_mode"}); break;
            case 1:
              // A buffer switch is what makes the memory stale: it is keyed on
              // the document and the revision, and neither survives one. `:new`
              // only opens a second buffer once this one has been edited --
              // before that it reuses the buffer it is standing in, which would
              // take the line being watched away rather than leave it behind --
              // and the walk back is to index 0, where this session started,
              // rather than a fixed number of steps.
              if (typist.ed.doc.modified && (BufferCount(typist.ed) == 1)) {
                RunCommands(typist.ed, {":new"});
              }
              if (BufferCount(typist.ed) > 1) {
                for (int guard = 0; guard < 8; ++guard) {
                  RunCommands(typist.ed, {"buffer_next"});
                  if (typist.ed.active == 0) break;
                }
                RunCommands(typist.ed, {"insert_mode"});
              }
              break;
            default: {
              const char c = "workinges"[static_cast<std::size_t>(rng.Pick(0, 8))];
              typist.Type(std::string_view{&c, 1});
              ++g_tally.keystrokes;
              break;
            }
          }
        }
        // The line is line 1 throughout: nothing above it was touched.
        const std::string leading = LeadingOf(typist.ed.doc.table, 1);
        if (leading != hand) {
          std::cerr << "  fuzz: " << file << " moved a hand-indented line from " << width
                    << " to " << leading.size() << " columns, seed " << rng.seed
                    << " buffers " << BufferCount(typist.ed) << " doc ["
                    << AssembleDocContents(typist.ed.doc.table) << "]\n";
        }
        EXPECT_EQ(leading, hand);
        EXPECT_EQ(EditorInvariants(typist.ed), std::string{});
      }
    }
  }

  if (FuzzIsVerbose()) {
    std::cout << "indent fuzz: " << iterations << " iterations in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - started)
                     .count()
              << "ms\n";
    std::cout << "indent fuzz: " << g_tally.buffers << " buffers, " << g_tally.engine_calls
              << " engine calls (" << g_tally.answers << " answered, " << g_tally.declines
              << " declined), " << g_tally.keystrokes << " keystrokes, longest answer "
              << g_tally.longest_answer << " bytes, slowest budgeted call " << g_tally.slowest_ms
              << "ms, " << g_tally.budget_flips << " repeat calls the frame budget cut off\n";
    std::cout << "indent fuzz: hybrid-vs-absolute drift in levels 0/1/2/3/4-7/8-15/16+:";
    for (const long long count : g_tally.deviation) std::cout << ' ' << count;
    std::cout << "\n";
  }
}

}
