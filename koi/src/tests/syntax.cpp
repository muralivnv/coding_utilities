// Tests for syntax.cpp: parsing, highlighting, predicates, and the injected
// regions a document can carry.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void SyntaxLanguages() {
  TEST_CASE("syntax: language table");

  for (const LanguageSample& sample : kLanguageSamples) {
    EXPECT_EQ(LanguageForPath(std::filesystem::path{sample.filename}), sample.language);
  }

  EXPECT_EQ(KnownLanguages().size(), kLanguageSamples.size());

  EXPECT_EQ(LanguageForPath(std::filesystem::path{"editor.h"}), std::string_view{"cpp"});
  EXPECT_EQ(LanguageForPath(std::filesystem::path{"notes.txt"}), std::string_view{});
  EXPECT_EQ(LanguageForPath(std::filesystem::path{"Makefile"}), std::string_view{"make"});
  EXPECT_EQ(LanguageForPath(std::filesystem::path{"fix.patch"}), std::string_view{"diff"});
  EXPECT_EQ(LanguageForPath(std::filesystem::path{"flake.nix"}), std::string_view{"nix"});
  EXPECT_EQ(LanguageForPath(std::filesystem::path{".bashrc"}), std::string_view{"bash"});
  EXPECT_EQ(LanguageForPath(std::filesystem::path{"MAIN.CPP"}), std::string_view{"cpp"});
}

void SyntaxQueriesCompile() {
  TEST_CASE("syntax: every query compiles");

  for (const LanguageSample& sample : kLanguageSamples) {
    std::string error;
    const auto syntax = OpenSyntax(std::filesystem::path{sample.filename}, error);
    if (syntax == nullptr) {
      std::cout << "  " << sample.language << ": " << error << "\n";
    }
    EXPECT_TRUE(syntax != nullptr);
    if (syntax == nullptr) continue;
    EXPECT_EQ(syntax->Language(), sample.language);
    EXPECT_TRUE(!syntax->CaptureNames().empty());
  }
}

void SyntaxHighlighting() {
  TEST_CASE("syntax: highlighting");

  std::string error;
  const auto syntax = OpenSyntax(std::filesystem::path{"sample.cpp"}, error);
  EXPECT_TRUE(syntax != nullptr);
  if (syntax == nullptr) return;

  const std::string source = "int main() {\n  // hi\n  return 0;\n}\n";
  PieceTable table;
  ResetToOriginal(table, source);

  syntax->Sync(table);
  std::vector<CaptureId> painted;
  syntax->Paint(table, Interval(0, DocLength(table)), painted);
  EXPECT_EQ(std::ssize(painted), DocLength(table));

  const auto names = syntax->CaptureNames();
  const auto scope_at = [&](Index pos) -> std::string_view {
    const CaptureId id = painted[static_cast<size_t>(pos)];
    return (id == kNoCapture) ? std::string_view{} : std::string_view{names[id - 1]};
  };

  EXPECT_TRUE(scope_at(source.find("int")).starts_with("type"));
  EXPECT_TRUE(scope_at(source.find("return")).starts_with("keyword"));
  EXPECT_TRUE(scope_at(source.find("// hi")).starts_with("comment"));
  EXPECT_EQ(scope_at(source.find("int") + 3), std::string_view{});

  const Index from = static_cast<Index>(source.find("return"));
  std::vector<CaptureId> window;
  syntax->Paint(table, Interval(from, from + 6), window);
  EXPECT_EQ(std::ssize(window), 6);
  EXPECT_EQ(window[0], painted[static_cast<size_t>(from)]);

  const Index before = table.revision;
  EXPECT_TRUE(!Insert("const ", 0, table));
  EXPECT_TRUE(table.revision > before);
  EXPECT_TRUE(table.revision - table.journal_base <= std::ssize(table.journal));

  syntax->Sync(table);
  syntax->Paint(table, Interval(0, DocLength(table)), painted);
  const std::string after = ReadDocRange(table, Interval(0, DocLength(table)));
  EXPECT_TRUE(scope_at(after.find("// hi")).starts_with("comment"));
  EXPECT_TRUE(scope_at(after.find("const")).starts_with("keyword"));
}

void SyntaxPredicates() {
  TEST_CASE("syntax: query predicates");

  std::string error;
  const auto syntax = OpenSyntax(std::filesystem::path{"p.c"}, error);
  EXPECT_TRUE(syntax != nullptr);
  if (syntax == nullptr) return;

  const std::string source = "int LOUD = 1;\nint quiet = 2;\n";
  PieceTable table;
  ResetToOriginal(table, source);
  syntax->Sync(table);
  std::vector<CaptureId> painted;
  syntax->Paint(table, Interval(0, DocLength(table)), painted);

  const auto names = syntax->CaptureNames();
  const auto scope_at = [&](Index pos) -> std::string_view {
    const CaptureId id = painted[static_cast<size_t>(pos)];
    return (id == kNoCapture) ? std::string_view{} : std::string_view{names[id - 1]};
  };

  EXPECT_TRUE(scope_at(source.find("LOUD")).starts_with("constant"));
  EXPECT_TRUE(!scope_at(source.find("quiet")).starts_with("constant"));
}

void MarkdownHighlightsItsInlineAndFencedLanguages() {
  TEST_CASE("markdown: the inline grammar and fenced code blocks are highlighted");
  const Scratch scratch{"koi-markdown-injections"};

  // The block grammar stops at (inline): emphasis, code spans and links are a
  // second grammar, and a fenced block is whatever its info string says. All
  // three arrive through queries/markdown/injections.scm; without it a
  // paragraph of prose is one unstyled run.
  const std::filesystem::path file =
      scratch.Write("doc.md",
                    "# Title\n"
                    "\n"
                    "Prose with *em*, **strong**, `code` and [link](https://x.example).\n"
                    "\n"
                    "```rust\n"
                    "fn main() { let x = 1; }\n"
                    "```\n");

  Editor ed;
  {
    Theme themed;
    std::string theme_error;
    // The builtin theme defines no markup.* scopes, so a capture there is
    // indistinguishable from plain text and this would pass either way.
    ed.theme = LoadTheme("ronin", themed, theme_error) ? std::move(themed) : BuiltinTheme();
  }
  EXPECT_FALSE(static_cast<bool>(LoadDocument(file, ed.doc)));
  ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
  AttachSyntax(ed);
  EXPECT_TRUE(ed.doc.syntax != nullptr);
  if (ed.doc.syntax == nullptr) return;
  EXPECT_EQ(std::string{ed.doc.syntax->Language()}, std::string{"markdown"});

  std::vector<CaptureId> ids;
  ed.doc.syntax->Sync(ed.doc.table);  // Paint reads the tree Sync builds.
  ed.doc.syntax->Paint(ed.doc.table, Interval(0, DocLength(ed.doc.table)), ids);
  const std::string text = ReadDocRange(ed.doc.table, Interval(0, DocLength(ed.doc.table)));
  EXPECT_EQ(ids.size(), text.size());

  const std::span<const std::string> names = ed.doc.syntax->CaptureNames();
  const auto scope_on = [&](std::string_view needle) {
    const std::size_t at = text.find(needle);
    if ((at == std::string::npos) || (at >= ids.size())) return std::string{};
    const CaptureId id = ids[at];
    if ((id == kNoCapture) || (id > names.size())) return std::string{};
    return names[id - 1];
  };

  EXPECT_EQ(scope_on("em*,"), std::string{"markup.italic"});
  EXPECT_EQ(scope_on("strong**"), std::string{"markup.bold"});
  EXPECT_EQ(scope_on("code`"), std::string{"markup.raw.inline"});
  EXPECT_EQ(scope_on("https://x.example"), std::string{"markup.link.url"});
  // The fenced block is Rust, not one flat markup.raw.block run.
  EXPECT_EQ(scope_on("fn main"), std::string{"keyword.function"});

  // The base grammar still works, and its captures share the id space.
  EXPECT_TRUE(scope_on("# Title").starts_with("markup.heading"));

  // Capture names grow as injected languages are met, which is why the render
  // path resolves styles from CaptureNames() after Paint rather than trusting
  // the length it saw when the syntax was attached.
  EXPECT_TRUE(names.size() > ed.doc.capture_styles.size());

  // The same machinery, one level out: <script> and <style> bodies come back
  // from the html grammar as one opaque (raw_text) node.
  {
    TEST_CASE("html: <script> is javascript and <style> is css");
    const std::filesystem::path page =
        scratch.Write("page.html",
                      "<html><head><style>\n"
                      "body { color: red; }\n"
                      "</style></head><body><script>\n"
                      "const x = 1;\n"
                      "</script></body></html>\n");
    Editor web;
    web.theme = ed.theme;
    EXPECT_FALSE(static_cast<bool>(LoadDocument(page, web.doc)));
    web.doc.selections.Set(MinWidth1(web.doc.table, Selection{0, 0, -1}));
    AttachSyntax(web);
    EXPECT_TRUE(web.doc.syntax != nullptr);
    if (web.doc.syntax != nullptr) {
      std::vector<CaptureId> web_ids;
      web.doc.syntax->Sync(web.doc.table);
      web.doc.syntax->Paint(web.doc.table, Interval(0, DocLength(web.doc.table)), web_ids);
      const std::string web_text =
          ReadDocRange(web.doc.table, Interval(0, DocLength(web.doc.table)));
      const std::span<const std::string> web_names = web.doc.syntax->CaptureNames();
      const auto web_scope = [&](std::string_view needle) {
        const std::size_t at = web_text.find(needle);
        if ((at == std::string::npos) || (at >= web_ids.size())) return std::string{};
        const CaptureId id = web_ids[at];
        if ((id == kNoCapture) || (id > web_names.size())) return std::string{};
        return web_names[id - 1];
      };
      EXPECT_EQ(web_scope("const"), std::string{"keyword.storage.modifier"});
      EXPECT_TRUE(!web_scope("red").empty());
      EXPECT_EQ(web_scope("style"), std::string{"tag"});
    }
  }

  // An unknown fence language is not an error, it just stays plain.
  {
    const std::filesystem::path odd =
        scratch.Write("odd.md", "```mermaid\ngraph TD;\n```\n");
    Editor other;
    other.theme = ed.theme;
    EXPECT_FALSE(static_cast<bool>(LoadDocument(odd, other.doc)));
    other.doc.selections.Set(MinWidth1(other.doc.table, Selection{0, 0, -1}));
    AttachSyntax(other);
    std::vector<CaptureId> odd_ids;
    if (other.doc.syntax != nullptr) {
      other.doc.syntax->Sync(other.doc.table);
      other.doc.syntax->Paint(other.doc.table, Interval(0, DocLength(other.doc.table)), odd_ids);
      EXPECT_EQ(odd_ids.size(), static_cast<std::size_t>(DocLength(other.doc.table)));
    }
  }
}

void InjectedRegionsKeepTheirParsedTree() {
  TEST_CASE("injections: a region's tree outlives a scroll and dies on an edit");
  const Scratch scratch{"koi-injection-cache"};

  // PaintInjections used to read the whole injected region out of the piece
  // tree and run a fresh, non-incremental ParseBuffer over it on *every*
  // Paint, keeping nothing between frames -- while Sync gave the base tree
  // journal replay and ts_tree_edit. Paint's own memo is keyed on
  // (revision, start, end), so scrolling by one line missed it and the entire
  // <script> body was parsed again. Measured on a 196 KB inline script: 53 ms
  // per frame, and once the body outgrew the 50 ms injection budget the block
  // stayed permanently unhighlighted while still costing the 53 ms, silently.
  //
  // No timing here. The counter is the observable: it says how many injected
  // parses ran, and that is exactly what the bug was made of.
  const std::filesystem::path page =
      scratch.Write("page.html",
                    "<html><body>\n"
                    "<p>before</p>\n"
                    "<script>\n"
                    "const alpha = 1;\n"
                    "let beta = 2;\n"
                    "</script>\n"
                    "<p>after</p>\n"
                    "</body></html>\n");

  Editor ed;
  ed.theme = BuiltinTheme();
  EXPECT_FALSE(static_cast<bool>(LoadDocument(page, ed.doc)));
  ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
  AttachSyntax(ed);
  EXPECT_TRUE(ed.doc.syntax != nullptr);
  if (ed.doc.syntax == nullptr) return;
  Syntax& syntax = *ed.doc.syntax;

  const auto doc_text = [&] {
    return ReadDocRange(ed.doc.table, Interval(0, DocLength(ed.doc.table)));
  };
  // The scope painted on the first byte of `needle` as it sits in the document
  // *now*, given a window of ids that began at `base`. Empty means nothing was
  // painted there, which is what a missing injected tree leaves behind; a
  // stale one leaves the right scope at the wrong offset, so looking the
  // needle up afresh each time is the point.
  const auto scope_at = [&](const std::vector<CaptureId>& ids, Index base,
                            std::string_view needle) {
    const std::string text = doc_text();
    const std::size_t at = text.find(needle);
    if (at == std::string::npos) return std::string{"<not in document>"};
    const auto in_window = static_cast<Index>(at) - base;
    if ((in_window < 0) || (in_window >= std::ssize(ids))) return std::string{"<outside window>"};
    const CaptureId id = ids[static_cast<std::size_t>(in_window)];
    const std::span<const std::string> names = syntax.CaptureNames();
    if ((id == kNoCapture) || (id > names.size())) return std::string{};
    return names[id - 1];
  };

  syntax.Sync(ed.doc.table);
  EXPECT_EQ(syntax.InjectionParses(), Index{0});

  // -- the same revision, a second window: reuse, not a second parse --------
  const Index length = DocLength(ed.doc.table);
  std::vector<CaptureId> whole;
  syntax.Paint(ed.doc.table, Interval(0, length), whole);
  EXPECT_EQ(whole.size(), static_cast<std::size_t>(length));
  EXPECT_EQ(syntax.InjectionParses(), Index{1});

  const std::string const_scope = scope_at(whole, 0, "const alpha");
  const std::string let_scope = scope_at(whole, 0, "let beta");
  EXPECT_EQ(const_scope, std::string{"keyword.storage.modifier"});
  EXPECT_TRUE(!let_scope.empty());

  const auto scrolled = static_cast<Index>(doc_text().find("<p>before"));
  EXPECT_TRUE(scrolled > 0);
  std::vector<CaptureId> window;
  syntax.Paint(ed.doc.table, Interval(scrolled, length), window);
  // Paint's memo is keyed on the window, so this one missed it and
  // PaintInjections ran again. The parse must not have.
  EXPECT_EQ(syntax.InjectionParses(), Index{1});

  {
    const std::string text = doc_text();
    const auto body = static_cast<Index>(text.find("const alpha"));
    const auto body_end = static_cast<Index>(text.find("</script>"));
    EXPECT_TRUE((body > 0) && (body_end > body));

    Index coloured = 0;
    bool overlap_agrees = true;
    for (Index at = body; at < body_end; ++at) {
      if (whole[static_cast<std::size_t>(at)] != kNoCapture) ++coloured;
      if (whole[static_cast<std::size_t>(at)] != window[static_cast<std::size_t>(at - scrolled)]) {
        overlap_agrees = false;
      }
    }
    // Not "some of it": the reused tree must paint what the fresh one did.
    EXPECT_TRUE(coloured > 0);
    EXPECT_TRUE(overlap_agrees);
  }

  // -- an edit inside the region: reparsed, and painted where it now is -----
  {
    const auto before_close = static_cast<Index>(doc_text().find("</script>"));
    EXPECT_FALSE(static_cast<bool>(Insert("let zz = 3;\n", before_close, ed.doc.table)));
    syntax.Sync(ed.doc.table);

    const Index parses = syntax.InjectionParses();
    std::vector<CaptureId> after;
    syntax.Paint(ed.doc.table, Interval(0, DocLength(ed.doc.table)), after);
    EXPECT_EQ(syntax.InjectionParses(), parses + 1);
    // The new statement is painted, and so is everything the edit pushed
    // along. Reusing the old tree here paints `const alpha` at the offset it
    // used to have and leaves `let zz` bare.
    EXPECT_EQ(scope_at(after, 0, "let zz"), let_scope);
    EXPECT_EQ(scope_at(after, 0, "let beta"), let_scope);
    EXPECT_EQ(scope_at(after, 0, "const alpha"), const_scope);
  }

  // -- an edit *before* the region: every byte of it moves ------------------
  {
    const auto above = static_cast<Index>(doc_text().find("<p>before"));
    EXPECT_FALSE(static_cast<bool>(Insert("<p>a line above</p>\n", above, ed.doc.table)));
    syntax.Sync(ed.doc.table);

    const Index parses = syntax.InjectionParses();
    std::vector<CaptureId> after;
    syntax.Paint(ed.doc.table, Interval(0, DocLength(ed.doc.table)), after);
    EXPECT_EQ(syntax.InjectionParses(), parses + 1);
    EXPECT_EQ(scope_at(after, 0, "const alpha"), const_scope);
    EXPECT_EQ(scope_at(after, 0, "let beta"), let_scope);
    EXPECT_EQ(scope_at(after, 0, "let zz"), let_scope);
  }

  // -- and back onto the offset an older, staler tree was keyed on ----------
  {
    // The cache is keyed on where a region starts, so the two edits above left
    // an entry under the region's original offset holding the tree it had
    // *before* `let zz` existed. Undo the line above and the region lands back
    // on that key. Reaching that entry is what the revision stamp forbids.
    const auto above = static_cast<Index>(doc_text().find("<p>a line above</p>\n"));
    EXPECT_TRUE(above >= 0);
    EXPECT_FALSE(static_cast<bool>(
        Delete(above, above + static_cast<Index>(std::size("<p>a line above</p>\n") - 1),
               ed.doc.table)));
    syntax.Sync(ed.doc.table);

    const Index parses = syntax.InjectionParses();
    std::vector<CaptureId> after;
    syntax.Paint(ed.doc.table, Interval(0, DocLength(ed.doc.table)), after);
    EXPECT_EQ(syntax.InjectionParses(), parses + 1);
    EXPECT_EQ(scope_at(after, 0, "let zz"), let_scope);
    EXPECT_EQ(scope_at(after, 0, "const alpha"), const_scope);
  }

  // Nothing here drives the gave_up path. kInjectionBudget is a file-local
  // constexpr with no way in from a test, and forcing a real 50 ms timeout
  // would mean either a hook that exists only for tests or an assertion about
  // how fast this machine parses. Both are worse than the gap.
}

void RunawayQueriesAreCutOffLikeRunawayParses() {
  TEST_CASE("syntax: a query too slow to draw is cut short, and admits it");

  // Every parse in here was budgeted and no query was, and the two costs are
  // nothing like each other. Matching is quadratic in the depth of the tree
  // over a byte range that every one of those ancestors contains -- which is
  // what a viewport in the middle of a generated file *is*. Painting is
  // synchronous inside DrawPane, so until it returns no key is read: not Esc,
  // not Ctrl-C.
  //
  // Nested conditionals, wrapped so the file has ordinary lines and the window
  // below is an ordinary viewport rather than a whole 12 KB line. Measured
  // here with the budget lifted, by depth: 500 -> 30 ms, 1,000 -> 110 ms,
  // 2,000 -> 450 ms, 4,000 -> 1.86 s, against a parse that stays under 25 ms
  // throughout. kDepth leaves roughly 40x between the real cost of the query
  // and the budget meant to stop it, so a machine several times quicker than
  // this one still sees the deadline fire.
  //
  // Nothing here asserts on a clock. The deadline firing is the observable.
  const auto deeply_nested = [](std::size_t depth) {
    std::string out = "x=";
    for (std::size_t i = 0; i < depth; ++i) out += (i % 16 == 15) ? "(1?\n" : "(1?";
    out += "1";
    for (std::size_t i = 0; i < depth; ++i) out += (i % 16 == 15) ? ":2)\n" : ":2)";
    out += ";\n";
    return out;
  };
  constexpr std::size_t kDepth = 3000;
  constexpr Index kWindow = 400;

  const std::string plain_text =
      "const alpha = 1;\n"
      "function beta(gamma) { return alpha + gamma; }\n";

  Editor ed;
  ed.theme = BuiltinTheme();
  ed.doc.file = "generated.js";
  AttachSyntax(ed);
  EXPECT_TRUE(ed.doc.syntax != nullptr);
  if (ed.doc.syntax == nullptr) return;
  Syntax& syntax = *ed.doc.syntax;

  const auto coloured = [](const std::vector<CaptureId>& ids) {
    return std::ranges::count_if(ids, [](CaptureId id) { return id != kNoCapture; });
  };

  // -- an ordinary file: painted whole, nothing given up --------------------
  ResetToOriginal(ed.doc.table, plain_text);
  syntax.Sync(ed.doc.table);
  EXPECT_FALSE(syntax.TimedOut());

  std::vector<CaptureId> plain;
  syntax.Paint(ed.doc.table, Interval(0, DocLength(ed.doc.table)), plain);
  EXPECT_FALSE(syntax.TimedOut());
  EXPECT_EQ(plain.size(), static_cast<std::size_t>(DocLength(ed.doc.table)));
  EXPECT_TRUE(coloured(plain) > 0);
  // `const` is a keyword here and stays one; the budget must not cost the
  // fast path a single capture.
  const std::span<const std::string> names = syntax.CaptureNames();
  EXPECT_TRUE(plain[0] != kNoCapture);
  EXPECT_TRUE((plain[0] <= names.size()) &&
              names[plain[0] - 1].starts_with("keyword"));

  // -- the pathological one: parsed, and then cut off while painting --------
  ResetToOriginal(ed.doc.table, deeply_nested(kDepth));
  syntax.Sync(ed.doc.table);
  // The parse fits, comfortably. It is the query after it that does not, which
  // is exactly why a parse budget alone was never enough.
  EXPECT_FALSE(syntax.TimedOut());

  // A viewport in the middle of the file: the window is small, and every one
  // of the 3,000 ancestors above it overlaps it.
  const Index middle = DocLength(ed.doc.table) / 2;
  std::vector<CaptureId> window;
  syntax.Paint(ed.doc.table, Interval(middle, middle + kWindow), window);
  EXPECT_TRUE(syntax.TimedOut());
  // Partial, not empty: what was matched before the deadline is kept, and the
  // rest of the window is left uncoloured rather than left undrawn.
  EXPECT_EQ(window.size(), static_cast<std::size_t>(kWindow));

  // -- and back to an ordinary file: the flag does not stick ----------------
  ResetToOriginal(ed.doc.table, plain_text);
  syntax.Sync(ed.doc.table);
  EXPECT_FALSE(syntax.TimedOut());

  std::vector<CaptureId> again;
  syntax.Paint(ed.doc.table, Interval(0, DocLength(ed.doc.table)), again);
  EXPECT_FALSE(syntax.TimedOut());
  EXPECT_TRUE(again == plain);
}

namespace {

// The scope painted on the byte after `needle` starts -- the character inside
// an emphasis marker, or the first byte of a keyword -- or a word saying why
// there is none. Shared by the two injection-cap tests below.
std::string ScopeAfter(const Syntax& syntax, const std::string& text,
                       const std::vector<CaptureId>& painted, std::string_view needle,
                       std::size_t offset) {
  const std::size_t at = text.find(needle);
  if (at == std::string::npos) return "<missing>";
  if (at + offset >= painted.size()) return "<unpainted>";
  const CaptureId id = painted[at + offset];
  const std::span<const std::string> names = syntax.CaptureNames();
  if ((id == kNoCapture) || (id > names.size())) return "<plain>";
  return names[id - 1];
}

}  // namespace

void EveryInjectedRegionOnScreenIsPaintedNotJustTheFirstFewHundred() {
  TEST_CASE("syntax: injected regions past the old 512 cap are still highlighted");

  // The cap on injected regions used to be a count, 512, applied per Paint and
  // justified by "the viewport bounds how many there are". It does not. Two
  // documents koi already opens put more than 512 regions into a single call,
  // and everything past the 512th came back plain with nothing said about it:
  // markdown gives every paragraph's (inline) node and every (pipe_table_cell)
  // its own region.
  Editor ed;
  ed.theme = BuiltinTheme();
  ed.doc.file = "long.md";
  AttachSyntax(ed);
  EXPECT_TRUE(ed.doc.syntax != nullptr);
  if (ed.doc.syntax == nullptr) return;
  Syntax& syntax = *ed.doc.syntax;

  // -- a whole file painted in one call, which is what --render-mode does ---
  //
  // 600 paragraphs, ~15 KB. Measured here: 600 regions parsed and painted in
  // 18 ms, against the 25 ms a whole frame is allowed -- and the budget cannot
  // cut this short in any case, because a twenty-byte inline tree never runs
  // the query progress callback that would notice the deadline. What used to
  // happen was not slowness: paragraph 511 was styled, paragraph 512 was not.
  constexpr int kParagraphs = 600;
  std::string prose;
  for (int i = 0; i < kParagraphs; ++i) {
    prose += "P" + std::to_string(i) + " with *em" + std::to_string(i) + "* in it.\n\n";
  }
  ResetToOriginal(ed.doc.table, prose);
  syntax.Sync(ed.doc.table);

  std::vector<CaptureId> ids;
  syntax.Paint(ed.doc.table, Interval(0, DocLength(ed.doc.table)), ids);
  EXPECT_EQ(ids.size(), prose.size());

  // The byte after the opening `*` is the emphasised text, which only the
  // injected inline grammar knows about: the block grammar leaves the whole
  // paragraph as one opaque run.
  const auto emphasis = [&](int i) {
    return ScopeAfter(syntax, prose, ids, "*em" + std::to_string(i) + "*", 1);
  };
  EXPECT_EQ(emphasis(0), std::string{"markup.italic"});
  // Either side of where the cliff used to be, and the far end of the file.
  EXPECT_EQ(emphasis(511), std::string{"markup.italic"});
  EXPECT_EQ(emphasis(512), std::string{"markup.italic"});
  EXPECT_EQ(emphasis(kParagraphs - 1), std::string{"markup.italic"});

  int plain_paragraphs = 0;
  for (int i = 0; i < kParagraphs; ++i) {
    if (emphasis(i) != "markup.italic") ++plain_paragraphs;
  }
  EXPECT_EQ(plain_paragraphs, 0);
  // Nothing was given up, so nothing is claimed to have been.
  EXPECT_FALSE(syntax.InjectionsTruncated());

  // -- and the same thing inside one ordinary viewport ----------------------
  //
  // A 15-column pipe table over 40 rows is 600 cells, 6 KB, and fits on one
  // 42-line screen. Row 1's cells used to be styled and rows 38-40 flat, on the
  // same screen, with the boundary falling wherever the 512th cell happened to
  // land.
  {
    constexpr int kCols = 15;
    constexpr int kRows = 40;
    std::string table = "# t\n\n";
    for (int c = 0; c < kCols; ++c) table += "| h" + std::to_string(c) + " ";
    table += "|\n";
    for (int c = 0; c < kCols; ++c) table += "| --- ";
    table += "|\n";
    for (int r = 0; r < kRows; ++r) {
      for (int c = 0; c < kCols; ++c) {
        table += "| *c" + std::to_string(r) + "_" + std::to_string(c) + "* ";
      }
      table += "|\n";
    }
    ResetToOriginal(ed.doc.table, table);
    syntax.Sync(ed.doc.table);

    std::vector<CaptureId> cells;
    syntax.Paint(ed.doc.table, Interval(0, DocLength(ed.doc.table)), cells);
    const auto cell = [&](int r, int c) {
      return ScopeAfter(syntax, table, cells,
                        "*c" + std::to_string(r) + "_" + std::to_string(c) + "*", 1);
    };
    EXPECT_EQ(cell(0, 0), std::string{"markup.italic"});
    EXPECT_EQ(cell(kRows - 1, kCols - 1), std::string{"markup.italic"});
    EXPECT_FALSE(syntax.InjectionsTruncated());

    int plain_cells = 0;
    for (int r = 0; r < kRows; ++r) {
      for (int c = 0; c < kCols; ++c) {
        if (cell(r, c) != "markup.italic") ++plain_cells;
      }
    }
    EXPECT_EQ(plain_cells, 0);
  }
}

void InjectedTextPastTheFrameBudgetIsRefusedAndSaidSo() {
  TEST_CASE("syntax: a paint stops handing text to other grammars, and admits it");

  // Lifting the region count does not mean a Paint will parse a document of any
  // size: what replaced it is a budget in bytes, which is what a parse is
  // actually paid in. Two <style> bodies of 530 KB are over it between them, so
  // the <script> after them is left to the base grammar -- and unlike the old
  // count cap, the giving up is reported.
  //
  // HTML rather than markdown because the base parse has to fit too: the html
  // grammar reads this 1.06 MB document in 17 ms, where the markdown grammar
  // spends the whole 500 ms parse budget on a megabyte and comes back with no
  // tree at all. The two <style> bodies are parsed (and give up on their own
  // 50 ms budget); the whole test is ~110 ms.
  Editor ed;
  ed.theme = BuiltinTheme();
  ed.doc.file = "huge.html";
  AttachSyntax(ed);
  EXPECT_TRUE(ed.doc.syntax != nullptr);
  if (ed.doc.syntax == nullptr) return;
  Syntax& syntax = *ed.doc.syntax;

  const auto page = [](std::size_t style_bytes) {
    std::string out = "<html><body>\n";
    for (int block = 0; block < 2; ++block) {
      out += "<style>\n";
      const std::size_t target = out.size() + style_bytes;
      while (out.size() < target) {
        out += ".cls" + std::to_string(out.size()) + " { color: red; }\n";
      }
      out += "</style>\n";
    }
    out += "<script>const tail = 1;</script>\n</body></html>\n";
    return out;
  };

  const std::string huge = page(530u * 1024u);
  ResetToOriginal(ed.doc.table, huge);
  syntax.Sync(ed.doc.table);
  EXPECT_FALSE(syntax.TimedOut());

  std::vector<CaptureId> ids;
  syntax.Paint(ed.doc.table, Interval(0, DocLength(ed.doc.table)), ids);
  EXPECT_TRUE(syntax.InjectionsTruncated());
  // Past the budget, so the trailing script body is plain rather than
  // javascript. Painted, just not injected: the base grammar still owns it.
  EXPECT_EQ(ids.size(), huge.size());
  EXPECT_EQ(ScopeAfter(syntax, huge, ids, "const tail", 0), std::string{"<plain>"});

  // -- the same page, small enough to fit: styled, and no complaint ---------
  const std::string small = page(1024u);
  ResetToOriginal(ed.doc.table, small);
  syntax.Sync(ed.doc.table);

  std::vector<CaptureId> fits;
  syntax.Paint(ed.doc.table, Interval(0, DocLength(ed.doc.table)), fits);
  // The flag is lowered by the parse of a changed buffer, exactly like the one
  // for a query that ran out of time.
  EXPECT_FALSE(syntax.InjectionsTruncated());
  EXPECT_EQ(ScopeAfter(syntax, small, fits, "const tail", 0),
            std::string{"keyword.storage.modifier"});
}

void AutoIndentSeesStringsInsideInjectedRegions() {
  TEST_CASE("injections: a brace in a JS string inside <script> is not a block");
  const Scratch scratch{"koi-injected-literals"};

  // InLiteralOrComment walked the base tree and nothing else. Inside a
  // <script> body every byte's ancestor chain is raw_text/script_element/
  // element/document -- no "string", no "comment" -- so the guard in
  // IndentForNewline that exists precisely to ignore brackets inside literals
  // saw none of them, and `const s = "{";` opened a block. The same line in a
  // plain .js file did not. The layers that know are the ones Paint parses and
  // `region_trees_` keeps between frames; this asks them.
  const std::filesystem::path script = scratch.Write("script.js",
                                                     "const s = \"{\";\n"
                                                     "// opens with {\n"
                                                     "function f() {\n"
                                                     "}\n");
  const std::filesystem::path page = scratch.Write("page.html",
                                                   "<html><body>\n"
                                                   "<!-- a comment with { in it -->\n"
                                                   "<p>a paragraph with { in it</p>\n"
                                                   "<style>\n"
                                                   "/* a css comment with { */\n"
                                                   "p::after {\n"
                                                   "  color: red;\n"
                                                   "}\n"
                                                   "</style>\n"
                                                   "<script>\n"
                                                   "const s = \"{\";\n"
                                                   "// opens with {\n"
                                                   "function f() {\n"
                                                   "}\n"
                                                   "</script>\n"
                                                   "</body></html>\n");

  // A loaded document with a painted frame behind it: Paint is what fills the
  // injected-tree cache, and in the editor a frame is drawn after every
  // keystroke, so by the time Return is pressed the region has a tree.
  const auto open = [](Editor& ed, const std::filesystem::path& path) {
    ed.theme = BuiltinTheme();
    if (LoadDocument(path, ed.doc)) return false;
    ed.doc.insert_spaces = true;
    ed.doc.tab_width = 4;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    AttachSyntax(ed);
    if (ed.doc.syntax == nullptr) return false;
    ed.doc.syntax->Sync(ed.doc.table);
    std::vector<CaptureId> painted;
    ed.doc.syntax->Paint(ed.doc.table, Interval(0, DocLength(ed.doc.table)), painted);
    return true;
  };

  // -- the differential: the same byte, in .js and inside <script> ----------
  //
  // `offset` steps off the needle to the byte actually asked about, so that a
  // needle can name the line and still point at one character of it.
  const auto literal_at = [&](const std::filesystem::path& path, std::string_view needle,
                              Index offset) {
    Editor ed;
    if (!open(ed, path)) return std::string{"<no syntax>"};
    const std::string text = ReadDocRange(ed.doc.table, Interval(0, DocLength(ed.doc.table)));
    const std::size_t at = text.find(needle);
    if (at == std::string::npos) return std::string{"<not in document>"};
    const Index pos = static_cast<Index>(at) + offset;
    if ((pos < 0) || (pos >= DocLength(ed.doc.table))) return std::string{"<off the end>"};
    if (text[static_cast<std::size_t>(pos)] != '{') return std::string{"<not the brace>"};
    return std::string{ed.doc.syntax->InLiteralOrComment(pos) ? "literal" : "code"};
  };

  // In a .js buffer these have always been right. `const s = "{"` points one
  // past the opening quote; `// opens with {` at its last character.
  EXPECT_EQ(literal_at(script, "const s = \"{\"", 11), std::string{"literal"});
  EXPECT_EQ(literal_at(script, "// opens with {", 14), std::string{"literal"});
  EXPECT_EQ(literal_at(script, "function f() {", 13), std::string{"code"});

  // The same bytes inside <script>, which is the bug.
  EXPECT_EQ(literal_at(page, "const s = \"{\"", 11), std::string{"literal"});
  EXPECT_EQ(literal_at(page, "// opens with {", 14), std::string{"literal"});
  EXPECT_EQ(literal_at(page, "function f() {", 13), std::string{"code"});

  // <style> is injected the same way, and the answer inside it is the one the
  // CSS grammar gives on its own -- which is the whole point, so the sheet is
  // written out and asked the same questions.
  //
  // A comment and not a string: tree-sitter-css parses `content: "{"` with the
  // brace as a bracket token rather than part of a (string_value), in a plain
  // .css buffer as much as in a <style> block. That is a grammar's opinion and
  // not this function's, and asserting parity with the plain file is what says
  // so.
  const std::filesystem::path sheet = scratch.Write("sheet.css",
                                                    "/* a css comment with { */\n"
                                                    "p::after {\n"
                                                    "  color: red;\n"
                                                    "}\n");
  EXPECT_EQ(literal_at(sheet, "/* a css comment with {", 22), std::string{"literal"});
  EXPECT_EQ(literal_at(sheet, "p::after {", 9), std::string{"code"});
  EXPECT_EQ(literal_at(page, "/* a css comment with {", 22), std::string{"literal"});
  EXPECT_EQ(literal_at(page, "p::after {", 9), std::string{"code"});

  // -- bytes no injected region owns: the host tree still answers ----------
  EXPECT_EQ(literal_at(page, "<!-- a comment with { in it -->", 20), std::string{"literal"});
  EXPECT_EQ(literal_at(page, "<p>a paragraph with { in it", 20), std::string{"code"});

  // -- and end to end, through the command that cares --------------------
  //
  // Return at the end of the line holding `needle`, reporting the width of the
  // indent the new line was opened with.
  const auto indent_after = [&](const std::filesystem::path& path, std::string_view needle) {
    Editor ed;
    if (!open(ed, path)) return Index{-1};
    const std::string text = ReadDocRange(ed.doc.table, Interval(0, DocLength(ed.doc.table)));
    const std::size_t at = text.find(needle);
    if (at == std::string::npos) return Index{-2};
    const std::size_t eol = text.find('\n', at);
    if (eol == std::string::npos) return Index{-3};
    const auto cursor = static_cast<Index>(eol);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{cursor, cursor, -1}));
    RunCommands(ed, {"insert_newline"});

    const std::string after = AssembleDocContents(ed.doc.table);
    const std::size_t line = after.find(needle);
    if (line == std::string::npos) return Index{-4};
    const std::size_t opened = after.find('\n', line);
    if (opened == std::string::npos) return Index{-5};
    Index spaces = 0;
    for (std::size_t i = opened + 1; (i < after.size()) && (after[i] == ' '); ++i) ++spaces;
    return spaces;
  };

  EXPECT_EQ(indent_after(script, "const s = \"{\""), Index{0});
  EXPECT_EQ(indent_after(script, "// opens with {"), Index{0});
  EXPECT_EQ(indent_after(script, "function f() {"), Index{4});

  EXPECT_EQ(indent_after(page, "const s = \"{\""), Index{0});
  EXPECT_EQ(indent_after(page, "// opens with {"), Index{0});
  // The control: a real brace inside the region still opens a block, so the
  // fix is not "never indent inside <script>".
  EXPECT_EQ(indent_after(page, "function f() {"), Index{4});
  EXPECT_EQ(indent_after(page, "/* a css comment with {"), Index{0});
  EXPECT_EQ(indent_after(page, "p::after {"), Index{4});
}

void ADocumentCannotNameTheSharedObjectKoiOpens() {
  TEST_CASE("injections: a fence's info string is a grammar name, not a path");

  // GrammarFor's answer is spliced into "libtree-sitter-<name>.so" and into
  // "queries/<name>/highlights.scm", and both are then looked for under every
  // runtime root and dlopened. The name comes out of the document -- the info
  // string of a fenced block, the `type` of an html <script> -- so a markdown
  // file could aim that lookup wherever it liked. Nothing traversed today only
  // because no runtime root happens to hold a directory called
  // `libtree-sitter-..`, which is not a property worth relying on.

  // -- what a grammar name is -----------------------------------------------
  //
  // A bare identifier. Everything koi ships is one, aliases included: `c++`
  // and `c#` are why `+` and `#` are in the set.
  EXPECT_EQ(GrammarFor("cpp"), std::string{"cpp"});
  EXPECT_EQ(GrammarFor("c"), std::string{"c"});
  EXPECT_EQ(GrammarFor("python"), std::string{"python"});
  EXPECT_EQ(GrammarFor("markdown_inline"), std::string{"markdown_inline"});
  EXPECT_EQ(GrammarFor("c#"), std::string{"c#"});
  EXPECT_EQ(GrammarFor("objective-c"), std::string{"objective-c"});
  // Aliases still map, and still after lowercasing.
  EXPECT_EQ(GrammarFor("c++"), std::string{"cpp"});
  EXPECT_EQ(GrammarFor("C++"), std::string{"cpp"});
  EXPECT_EQ(GrammarFor("py"), std::string{"python"});
  EXPECT_EQ(GrammarFor("JS"), std::string{"javascript"});
  EXPECT_EQ(GrammarFor("Sh"), std::string{"bash"});
  EXPECT_EQ(GrammarFor("YML"), std::string{"yaml"});
  EXPECT_EQ(GrammarFor("MD"), std::string{"markdown"});
  // Every alias in the table resolves, so the check cannot have shut one out.
  for (const std::string_view written : {"sh", "shell", "zsh", "console", "c++", "h", "hpp",
                                         "js", "jsx", "mjs", "ts", "py", "rs", "yml", "md",
                                         "markdown", "golang", "patch"}) {
    EXPECT_TRUE(!GrammarFor(written).empty());
  }
  // As does every language koi has queries for.
  for (const std::string_view known : KnownLanguages()) {
    EXPECT_EQ(GrammarFor(known), std::string{known});
  }

  // -- and what it is not ---------------------------------------------------
  EXPECT_EQ(GrammarFor(""), std::string{});
  EXPECT_EQ(GrammarFor("../../../../etc/passwd"), std::string{});
  EXPECT_EQ(GrammarFor(".."), std::string{});
  EXPECT_EQ(GrammarFor("../cpp"), std::string{});
  EXPECT_EQ(GrammarFor("cpp/../.."), std::string{});
  EXPECT_EQ(GrammarFor("/etc/passwd"), std::string{});
  // A separator is a separator even without a slash in it: a `.` would let a
  // name reach `libtree-sitter-x.so.evil`, and no grammar has one.
  EXPECT_EQ(GrammarFor("cpp.so"), std::string{});
  // Whitespace, control characters and a NUL, which would truncate the name
  // somewhere other than where C++ thinks it ends.
  EXPECT_EQ(GrammarFor(" cpp"), std::string{});
  EXPECT_EQ(GrammarFor("cpp\n"), std::string{});
  EXPECT_EQ(GrammarFor(std::string_view{"cpp\0evil", 8}), std::string{});
  EXPECT_EQ(GrammarFor("$(id)"), std::string{});
  EXPECT_EQ(GrammarFor("a;b"), std::string{});
  // Bounded, because the sweep it drives is not free. 32 fits, 33 does not.
  EXPECT_EQ(GrammarFor(std::string(32, 'a')), std::string(32, 'a'));
  EXPECT_EQ(GrammarFor(std::string(33, 'a')), std::string{});
  EXPECT_EQ(GrammarFor(std::string(4096, 'a')), std::string{});

  // -- end to end: the check is in the path a document actually takes -------
  //
  // What the refusal saves is a filesystem sweep and a dlopen attempt, and
  // nothing observable counts those: a hostile name resolved nothing before
  // the check either, because no root holds a file by that name. So this is
  // the other half -- that GrammarFor is what the injection path calls, that a
  // refused fence still paints, and that real fences still inject through the
  // check. InjectionParses counts injected-region parses, and a document that
  // is nothing but one fenced block has exactly one region to offer.
  Editor ed;
  ed.theme = BuiltinTheme();
  ed.doc.file = "fence.md";
  AttachSyntax(ed);
  EXPECT_TRUE(ed.doc.syntax != nullptr);
  if (ed.doc.syntax == nullptr) return;
  Syntax& syntax = *ed.doc.syntax;

  const auto parses_for = [&](std::string_view info) {
    std::string doc = "```";
    doc += info;
    doc += "\nlet x = 1;\n```\n";
    ResetToOriginal(ed.doc.table, doc);
    syntax.Sync(ed.doc.table);
    std::vector<CaptureId> ids;
    syntax.Paint(ed.doc.table, Interval(0, DocLength(ed.doc.table)), ids);
    // Painted either way: refusing the name costs the region its grammar, not
    // its bytes.
    EXPECT_EQ(ids.size(), doc.size());
    return syntax.InjectionParses();
  };

  // The control first, so a zero below cannot just mean "fences never inject".
  const Index good = parses_for("javascript");
  EXPECT_EQ(good, Index{1});
  EXPECT_EQ(parses_for("../../../../etc/passwd"), good);
  EXPECT_EQ(parses_for("../../grammars/javascript"), good);
  EXPECT_EQ(parses_for(std::string(200, 'z')), good);
  // And once more with a real language, so the refusals above did not simply
  // wedge the layer machinery.
  EXPECT_EQ(parses_for("js"), good + 1);
}

}  // namespace koi
