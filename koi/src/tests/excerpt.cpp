// Tests for the excerpt views: navigate.cpp builds them, commands.cpp drives
// them, and editor.cpp holds the buffer they are saved back through, so they
// are tested here together rather than under any one of those names.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

namespace {

// A find() result handed straight to replace() or substr() throws when the
// needle is absent, and an exception out of a test case ends the whole run
// naming no test at all -- which is how a fixture that was not what the case
// expected turns into "the suite died somewhere". Report it as a false instead
// and let the caller assert.
[[nodiscard]] bool ReplaceFirst(std::string& text, std::string_view what, std::string_view with) {
  const std::size_t at = text.find(what);
  if (at == std::string::npos) return false;
  text.replace(at, what.size(), with);
  return true;
}

std::string ExcerptScopeAt(const Editor& ed, const std::string& body, std::size_t byte) {
  std::size_t bol = body.rfind('\n', byte);
  bol = (bol == std::string::npos) ? 0 : ((body[bol] == '\n' && bol == byte) ? body.rfind('\n', byte - 1) : bol);
  bol = (bol == std::string::npos) ? 0 : bol + 1;
  if (bol > byte) bol = 0;
  std::size_t eol = body.find('\n', byte);
  if (eol == std::string::npos) eol = body.size();
  const std::string_view line{body.data() + bol, eol - bol};
  if (std::ranges::binary_search(ed.doc.excerpts.header_index, line)) {
    return std::string{kExcerptHeaderScope};
  }
  if (!ed.doc.excerpts.paint_line) return {};
  std::vector<Interval> spans;
  ed.doc.excerpts.paint_line(line, spans);
  const Index off = static_cast<Index>(byte - bol);
  for (const Interval& span : spans) {
    if ((off >= span.front()) && (off <= span.back())) return std::string{kExcerptMatchScope};
  }
  return {};
}

}  // namespace

void ReferenceExcerptView() {
  const Scratch scratch{"koi-excerpts"};
  std::string alpha;
  for (int i = 1; i <= 24; ++i) {
    alpha += ((i == 4) || (i == 6) || (i == 20)) ? "widget" : ("a" + std::to_string(i));
    alpha += "\n";
  }
  const std::filesystem::path a = scratch.Write("alpha.txt", alpha);
  const std::filesystem::path b = scratch.Write("beta.txt", "b1\nb2\nwidget\n");

  const auto view = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  const std::vector<Symbol> found{
      Symbol{b.string(), 3, 1, "widget"},
      Symbol{a.string(), 20, 1, "widget"},
      Symbol{a.string(), 4, 1, "widget"},
      Symbol{a.string(), 6, 1, "widget"},
  };

  TEST_CASE("excerpts: references become blocks of real file text, grouped and merged");
  Editor ed;
  ed.theme = BuiltinTheme();
  ed.settings.excerpt_context = 2;
  EXPECT_TRUE(OpenTarget(ed, a.string()));
  OpenReferenceExcerpts(ed, found, "widget");

  const std::string text = view(ed);
  EXPECT_TRUE(text.starts_with("4 references to widget\n"));
  EXPECT_TRUE(text.find("\na2\na3\nwidget\na5\nwidget\na7\na8\n") != std::string::npos);
  EXPECT_TRUE(text.find(":18,22\na18\na19\nwidget\na21\na22\n") != std::string::npos);
  EXPECT_TRUE(text.ends_with(":1,3\nb1\nb2\nwidget\n"));
  EXPECT_TRUE(text.find("alpha.txt") < text.find("beta.txt"));
  EXPECT_EQ(std::ranges::count(text, '\n'),
            std::ptrdiff_t{1 + 3 * (1 + 1) + 7 + 5 + 3});
  EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{3});

  TEST_CASE("excerpts: the view edits like any buffer, and knows it is unsaved");
  EXPECT_TRUE(IsExcerptView(ed.doc));
  EXPECT_FALSE(ed.doc.read_only);
  EXPECT_FALSE(ed.doc.modified);

  ed.doc.selections.Set(Selection{0, 0, -1});
  ed.doc.selections.EnsureBlockCursors(ed.doc.table);
  TypeInto(ed, 'Q');
  EXPECT_TRUE(view(ed) != text);
  EXPECT_TRUE(ed.doc.modified);
  EXPECT_FALSE(UnsavedBuffers(ed).empty());
  ed.status.clear();
  RunCommands(ed, {"buffer_close"});
  EXPECT_TRUE(ed.status.find("unsaved changes") != std::string::npos);
  EXPECT_TRUE(IsExcerptView(ed.doc));
  OpenReferenceExcerpts(ed, found, "widget");
  EXPECT_TRUE(ed.status.find("unsaved edits") != std::string::npos);
  EXPECT_TRUE(view(ed).find('Q') != std::string::npos);
  RunCommands(ed, {"undo"});
  EXPECT_EQ(view(ed), text);
  EXPECT_FALSE(ed.doc.modified);

  const std::filesystem::path dumped = scratch.dir / "dump.txt";
  RunTypableCommand(ed, "w " + dumped.string());
  EXPECT_FALSE(std::filesystem::exists(dumped));
  EXPECT_TRUE(ed.status.find("converts") != std::string::npos);
  EXPECT_TRUE(IsExcerptView(ed.doc));
  RunTypableCommand(ed, "w! " + dumped.string());
  EXPECT_TRUE(std::filesystem::exists(dumped));
  EXPECT_FALSE(IsExcerptView(ed.doc));
  EXPECT_TRUE(ed.doc.excerpts.header_index.empty());
  EXPECT_TRUE(ed.doc.excerpts.blocks.empty());
  RunCommands(ed, {"insert_newline"});
  EXPECT_TRUE(view(ed) != text);
  RunTypableCommand(ed, "w");
  EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);

  TEST_CASE("excerpts: a header carries the cursor back to the file it came from");
  OpenReferenceExcerpts(ed, found, "widget");
  const Index at = LineAt(ed.doc.table, static_cast<Index>(view(ed).find("\nwidget\na21")) + 1);
  ed.doc.selections.Set(Selection{LineStart(ed.doc.table, at), LineStart(ed.doc.table, at), -1});
  ed.doc.selections.EnsureBlockCursors(ed.doc.table);
  RunCommands(ed, {"goto_excerpt_source"});
  EXPECT_EQ(ed.doc.file.filename().string(), std::string{"alpha.txt"});
  EXPECT_EQ(LineAt(ed.doc.table, ed.doc.selections.Primary().head), Index{19});
  EXPECT_FALSE(IsExcerptView(ed.doc));

  ed.status.clear();
  RunCommands(ed, {"goto_excerpt_source"});
  EXPECT_TRUE(ed.status.find("not an excerpt view") != std::string::npos);

  TEST_CASE("excerpts: the jump carries the byte column, not just the line");
  OpenReferenceExcerpts(ed, found, "widget");
  {
    const Index body = static_cast<Index>(view(ed).find("\na21\n")) + 1;
    ed.doc.selections.Set(Selection{body + 2, body + 2, -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    RunCommands(ed, {"goto_excerpt_source"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"alpha.txt"});
    const Index cur = CursorOf(ed.doc.table, ed.doc.selections.Primary());
    EXPECT_EQ(LineAt(ed.doc.table, cur), Index{20});
    EXPECT_EQ(cur - LineStart(ed.doc.table, Index{20}), Index{2});
  }

  TEST_CASE("excerpts: a cursor on the line's last cell still jumps to that line");
  OpenReferenceExcerpts(ed, found, "widget");
  {
    const Index w = static_cast<Index>(view(ed).find("\nwidget\na21")) + 1;
    const Index nl = w + Index{6};
    ed.doc.selections.Set(Selection{nl, nl, -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    RunCommands(ed, {"goto_excerpt_source"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"alpha.txt"});
    const Index cur = CursorOf(ed.doc.table, ed.doc.selections.Primary());
    EXPECT_EQ(LineAt(ed.doc.table, cur), Index{19});
  }

  TEST_CASE("excerpts: paste is an ordinary edit in a view");
  OpenReferenceExcerpts(ed, found, "widget");
  {
    const std::string held = view(ed);
    const Index park = LineStart(ed.doc.table, 5);
    ed.doc.selections.Set(Selection{park, park, -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    ed.registers = {"XYZ"};
    RunCommands(ed, {"paste_after"});
    EXPECT_TRUE(view(ed).find("XYZ") != std::string::npos);
    EXPECT_TRUE(ed.doc.modified);
    RunCommands(ed, {"undo"});
    EXPECT_EQ(view(ed), held);
    EXPECT_FALSE(ed.doc.modified);
  }

  TEST_CASE("excerpts: :reload leaves the view alone");
  ed.status.clear();
  RunTypableCommand(ed, "reload");
  EXPECT_TRUE(IsExcerptView(ed.doc));
  EXPECT_TRUE(ed.status.find("cannot") == std::string::npos);

  TEST_CASE("excerpts: asking again replaces the view instead of stacking buffers");
  const std::size_t before = BufferCount(ed);
  OpenReferenceExcerpts(ed, found, "widget");
  OpenReferenceExcerpts(ed, found, "widget");
  EXPECT_EQ(BufferCount(ed), before);
  EXPECT_EQ(view(ed), text);
  EXPECT_EQ(ed.doc.view.top_line, Index{0});

  TEST_CASE("excerpts: a clean view closes without protest");
  EXPECT_TRUE(UnsavedBuffers(ed).empty());
  ed.status.clear();
  RunCommands(ed, {"buffer_close"});
  EXPECT_TRUE(ed.status.find("unsaved changes") == std::string::npos);

  TEST_CASE("excerpts: no references says so and opens nothing");
  Editor empty;
  empty.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(empty, a.string()));
  const std::size_t buffers = BufferCount(empty);
  OpenReferenceExcerpts(empty, {}, "nothing");
  EXPECT_EQ(BufferCount(empty), buffers);
  EXPECT_FALSE(IsExcerptView(empty.doc));
  EXPECT_TRUE(empty.status.find("no reference") != std::string::npos);

  TEST_CASE("excerpts: only the header and the word looked up are coloured");
  {
    const std::filesystem::path cpp = scratch.Write(
        "src.cpp", "int widget = 1;\nint widgetish() { return widget; }\nreturn 2;\n");

    Editor mixed;
    mixed.theme = BuiltinTheme();
    mixed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(mixed, cpp.string()));
    OpenReferenceExcerpts(mixed, {Symbol{cpp.string(), 2, 1, "widget"}}, "widget");
    EXPECT_EQ(EditorInvariants(mixed), std::string{});

    EXPECT_TRUE(mixed.doc.syntax == nullptr);
    EXPECT_EQ(mixed.doc.excerpts.capture_names.size(), std::size_t{2});
    EXPECT_EQ(mixed.doc.capture_styles.size(), std::size_t{2});

    const std::string body = ReadDocRange(mixed.doc.table, {0, DocLength(mixed.doc.table)});
    const auto scope_at = [&](std::size_t byte) { return ExcerptScopeAt(mixed, body, byte); };
    EXPECT_EQ(scope_at(body.find(cpp.string())), std::string{"ui.excerpt.header"});
    EXPECT_EQ(scope_at(body.find(mixed.settings.icon_file)),
              std::string{"ui.excerpt.header"});
    EXPECT_TRUE(body.find(mixed.settings.icon_file + " " + cpp.string() + ":1,3\n") !=
                std::string::npos);
    EXPECT_TRUE(body.find("▸") == std::string::npos);

    EXPECT_EQ(scope_at(body.find("widget = 1")), std::string{"ui.excerpt.match"});
    EXPECT_EQ(scope_at(body.find("return widget")+ 7), std::string{"ui.excerpt.match"});
    EXPECT_EQ(scope_at(body.find("widgetish")), std::string{});
    EXPECT_EQ(scope_at(body.find("int widget")), std::string{});
    EXPECT_EQ(scope_at(body.find("return 2")), std::string{});

    mixed.theme.scopes["ui.excerpt.match"] = Style{Color{true, 0x123456}, {}, 0};
    RefreshCaptureStyles(mixed);
    EXPECT_EQ(mixed.doc.capture_styles[1].fg.rgb, std::uint32_t{0x123456});

    for (const std::string_view name : {"builtin", "ronin", "calm"}) {
      Theme theme = BuiltinTheme();
      std::string error;
      if ((name != "builtin") && !LoadTheme(name, theme, error)) continue;
      for (const std::string_view scope : {kExcerptHeaderScope, kExcerptMatchScope}) {
        const Style style = theme.Get(scope);
        if (!style.fg.set) {
          ++common::g_test_failures;
          std::cerr << "FAIL [" << common::g_test_case << "] " << name << " leaves " << scope
                    << " unpainted" << std::endl;
        } else {
          ++common::g_test_checks;
        }
        EXPECT_TRUE((style.mods & kModBold) != 0);
      }
    }
  }

  TEST_CASE("excerpts: with icons off the header still wears a marker");
  {
    Editor plain_icons;
    plain_icons.theme = BuiltinTheme();
    plain_icons.settings.icons = false;
    EXPECT_TRUE(OpenTarget(plain_icons, a.string()));
    OpenReferenceExcerpts(plain_icons, found, "widget");
    const std::string text_off =
        ReadDocRange(plain_icons.doc.table, {0, DocLength(plain_icons.doc.table)});
    EXPECT_TRUE(text_off.find("▸ ") != std::string::npos);
    EXPECT_TRUE(text_off.find(plain_icons.settings.icon_file) == std::string::npos);
    plain_icons.doc.selections.Set(Selection{
        LineStart(plain_icons.doc.table, plain_icons.doc.excerpts.blocks.front().header_line),
        LineStart(plain_icons.doc.table, plain_icons.doc.excerpts.blocks.front().header_line),
        -1});
    plain_icons.doc.selections.EnsureBlockCursors(plain_icons.doc.table);
    RunCommands(plain_icons, {"goto_excerpt_source"});
    EXPECT_FALSE(IsExcerptView(plain_icons.doc));
  }

  TEST_CASE("excerpts: :set-excerpt-context rebuilds the view around the same reference");
  {
    Editor wide;
    wide.theme = BuiltinTheme();
    wide.settings.excerpt_context = 2;
    EXPECT_TRUE(OpenTarget(wide, a.string()));
    OpenReferenceExcerpts(wide, found, "widget");
    wide.doc.view.rows = 20;

    const std::string before_text =
        ReadDocRange(wide.doc.table, {0, DocLength(wide.doc.table)});
    const Index block20 =
        LineAt(wide.doc.table, static_cast<Index>(before_text.find(":18,22\n")));
    wide.doc.selections.Set(Selection{LineStart(wide.doc.table, block20 + 2),
                                      LineStart(wide.doc.table, block20 + 2), -1});
    wide.doc.selections.EnsureBlockCursors(wide.doc.table);

    RunTypableCommand(wide, "set-excerpt-context 6");
    EXPECT_EQ(wide.settings.excerpt_context, 6);
    EXPECT_EQ(EditorInvariants(wide), std::string{});
    EXPECT_TRUE(IsExcerptView(wide.doc));
    const std::string after = ReadDocRange(wide.doc.table, {0, DocLength(wide.doc.table)});
    EXPECT_TRUE(std::ranges::count(after, '\n') > std::ranges::count(before_text, '\n'));
    EXPECT_TRUE(after.starts_with("4 references to widget\n"));

    const ExcerptBlock* on = nullptr;
    const Index cursor_line = LineAt(wide.doc.table, wide.doc.selections.Primary().head);
    for (const ExcerptBlock& block : wide.doc.excerpts.blocks) {
      if (block.header_line > cursor_line) break;
      on = &block;
    }
    EXPECT_TRUE(on != nullptr);
    if (on != nullptr) EXPECT_EQ(on->line, Index{20});

    RunTypableCommand(wide, "set-excerpt-context 0");
    EXPECT_EQ(wide.settings.excerpt_context, 0);
    EXPECT_TRUE(ReadDocRange(wide.doc.table, {0, DocLength(wide.doc.table)}).ends_with(
        ":3,3\nwidget\n"));

    RunTypableCommand(wide, "set-excerpt-context 101");
    EXPECT_EQ(wide.settings.excerpt_context, 0);
    EXPECT_TRUE(wide.status.find("wants 0-100") != std::string::npos);

    Editor plain;
    plain.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(plain, a.string()));
    RunTypableCommand(plain, "set-excerpt-context 4");
    EXPECT_EQ(plain.settings.excerpt_context, 4);
    EXPECT_TRUE(plain.status.find("excerpt-context: 4 lines") != std::string::npos);
    RunTypableCommand(plain, "set-excerpt-context");
    EXPECT_TRUE(plain.status.find("excerpt-context: 4 lines") != std::string::npos);
  }

  TEST_CASE("excerpts: :increment/:decrement-excerpt-context step by one and clamp");
  {
    Editor step;
    step.theme = BuiltinTheme();
    step.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(step, a.string()));
    OpenReferenceExcerpts(step, found, "widget");
    step.doc.view.rows = 20;

    const std::string narrow = ReadDocRange(step.doc.table, {0, DocLength(step.doc.table)});
    RunTypableCommand(step, "increment-excerpt-context");
    EXPECT_EQ(step.settings.excerpt_context, 2);
    EXPECT_TRUE(IsExcerptView(step.doc));
    const std::string wider = ReadDocRange(step.doc.table, {0, DocLength(step.doc.table)});
    EXPECT_TRUE(std::ranges::count(wider, '\n') > std::ranges::count(narrow, '\n'));

    RunTypableCommand(step, "decrement-excerpt-context");
    EXPECT_EQ(step.settings.excerpt_context, 1);
    EXPECT_EQ(ReadDocRange(step.doc.table, {0, DocLength(step.doc.table)}), narrow);

    RunTypableCommand(step, "decrement-excerpt-context");
    EXPECT_EQ(step.settings.excerpt_context, 0);
    RunTypableCommand(step, "decrement-excerpt-context");
    EXPECT_EQ(step.settings.excerpt_context, 0);
    EXPECT_TRUE(step.status.find("already 0") != std::string::npos);

    step.settings.excerpt_context = 100;
    RunTypableCommand(step, "increment-excerpt-context");
    EXPECT_EQ(step.settings.excerpt_context, 100);
    EXPECT_TRUE(step.status.find("already 100") != std::string::npos);
  }

  TEST_CASE("excerpts: zero context shows just the reference lines");
  Editor tight;
  tight.theme = BuiltinTheme();
  tight.settings.excerpt_context = 0;
  EXPECT_TRUE(OpenTarget(tight, a.string()));
  OpenReferenceExcerpts(tight, found, "widget");
  EXPECT_TRUE(view(tight).ends_with(":3,3\nwidget\n"));
  EXPECT_EQ(tight.doc.excerpts.blocks.size(), std::size_t{4});
}

namespace {

// The status line reduced to the phrase a test is looking for, or left whole
// when the phrase is not in it -- so EXPECT_EQ prints what koi actually said.
//
// This is the excerpt suite's tripwire. Its save and revert cases were the last
// flake in the suite, failing about once in dozens of runs under load, and the
// only evidence they left was `ed.status.find("wrote ...") != npos` being
// false: no path, no reason, nothing to tell a failed whole-file read from a
// logic bug. Every refusal on that path now names the file and the errno, and
// running the assertion through here puts that text in the failure output. The
// next occurrence identifies itself.
std::string StatusAbout(const Editor& ed, std::string_view want) {
  const std::string& text = ed.status.text();
  return (text.find(want) != std::string::npos) ? std::string{want} : text;
}

}  // namespace

void EditableExcerptSave() {
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-excerpt-save"};
  const fs::path home = scratch.Write("home.txt", "just somewhere to stand\n");

  const auto view = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  const auto file_text = [](const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };
  const auto in_body = [&view](Editor& ed, std::string_view needle) {
    const std::string text = view(ed);
    return text.find(needle, text.find("\n\n") + 2);
  };
  const auto edit = [](Editor& ed, std::size_t at, std::size_t take, std::string_view put) {
    if (take > 0) std::ignore = Delete(static_cast<Index>(at), static_cast<Index>(at + take), ed.doc.table);
    if (!put.empty()) std::ignore = Insert(put, static_cast<Index>(at), ed.doc.table);
    ed.doc.modified = true;
  };

  TEST_CASE("excerpt save: an edited hunk is spliced into its file, exactly");
  {
    const fs::path one = scratch.Write("one.txt", "l1\nl2\nl3\nl4\nl5\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{one.string(), 3, 1, "l3"}}, "l3");

    edit(ed, in_body(ed, "l3"), 2, "l3x");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_TRUE(ed.doc.excerpts.refs_stale);
    EXPECT_EQ(file_text(one), std::string{"l1\nl2\nl3x\nl4\nl5\n"});
    EXPECT_TRUE(view(ed).find("one.txt:2,4") != std::string::npos);

    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("nothing to write") != std::string::npos);
  }

  TEST_CASE("excerpt save: a grown hunk shifts the spans below it, and the headers say so");
  {
    std::string six;
    for (int i = 1; i <= 12; ++i) six += "s" + std::to_string(i) + "\n";
    const fs::path path = scratch.Write("six.txt", six);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(
        ed, {Symbol{path.string(), 3, 1, "s3"}, Symbol{path.string(), 9, 1, "s3"}}, "s3");
    EXPECT_TRUE(view(ed).find("six.txt:2,4") != std::string::npos);
    EXPECT_TRUE(view(ed).find("six.txt:8,10") != std::string::npos);

    edit(ed, in_body(ed, "s3\n") + 3, 0, "NEW\n");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_EQ(file_text(path),
              std::string{"s1\ns2\ns3\nNEW\ns4\ns5\ns6\ns7\ns8\ns9\ns10\ns11\ns12\n"});
    EXPECT_TRUE(view(ed).find("six.txt:2,5") != std::string::npos);
    EXPECT_TRUE(view(ed).find("six.txt:9,11") != std::string::npos);

    edit(ed, in_body(ed, "s9"), 2, "S9");
    const std::size_t header = view(ed).find("six.txt:9,11");
    const std::size_t sep_nl = view(ed).rfind('\n', header);
    EXPECT_EQ(view(ed)[sep_nl - 1], '\n');
    edit(ed, sep_nl, 1, "");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_TRUE(file_text(path).find("s8\nS9\ns10\n") != std::string::npos);
    EXPECT_FALSE(ed.doc.modified);
  }

  TEST_CASE("excerpt save: an edited header refuses the whole save");
  {
    const fs::path path = scratch.Write("guard.txt", "g1\ng2\ng3\ng4\ng5\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 3, 1, "g3"}}, "g3");

    const std::string before = file_text(path);
    edit(ed, in_body(ed, "g3"), 2, "g3x");
    edit(ed, in_body(ed, ":2,4") + 1, 1, "9");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("header missing or edited") != std::string::npos);
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_EQ(file_text(path), before);
  }

  TEST_CASE("excerpt save: a body line that reads as a header refuses, and writes nothing");
  {
    std::string twelve;
    for (int i = 1; i <= 12; ++i) twelve += "s" + std::to_string(i) + "\n";
    const fs::path path = scratch.Write("twin.txt", twelve);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(
        ed, {Symbol{path.string(), 3, 1, "s3"}, Symbol{path.string(), 9, 1, "s3"}}, "s3");

    const std::string shown = view(ed);
    const std::size_t header_from = shown.rfind('\n', shown.find("twin.txt:8,10")) + 1;
    const std::size_t header_to = shown.find('\n', header_from);
    const std::string header2 = shown.substr(header_from, header_to - header_from);
    EXPECT_TRUE(header2.find("twin.txt:8,10") != std::string::npos);

    edit(ed, in_body(ed, "s3\n") + 3, 0, header2 + "\n");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("reads as a hunk header") != std::string::npos);
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_EQ(file_text(path), twelve);

    edit(ed, in_body(ed, "s3\n") + 3, header2.size() + 1, "");
    edit(ed, in_body(ed, "s3"), 2, "s3x");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_TRUE(file_text(path).find("s2\ns3x\ns4\n") != std::string::npos);
  }

  TEST_CASE("excerpt save: a file that changed underneath is refused, named");
  {
    const fs::path path = scratch.Write("moved.txt", "m1\nm2\nm3\nm4\nm5\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 3, 1, "m3"}}, "m3");

    scratch.Write("moved.txt", "m1\nCHANGED\nm3\nm4\nm5\n");
    edit(ed, in_body(ed, "m3"), 2, "m3x");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("changed under") != std::string::npos);
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_EQ(file_text(path), std::string{"m1\nCHANGED\nm3\nm4\nm5\n"});
  }

  TEST_CASE("excerpt save: emptying a body deletes the span, and the span can refill");
  {
    const fs::path path = scratch.Write("cut.txt", "t1\nt2\nt3\nt4\nt5\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 3, 1, "t3"}}, "t3");

    const std::size_t body = in_body(ed, "t2\n");
    edit(ed, body, view(ed).size() - body, "");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk"), std::string{"wrote 1 hunk"});
    EXPECT_EQ(file_text(path), std::string{"t1\nt5\n"});
    EXPECT_TRUE(view(ed).find("cut.txt:2,1") != std::string::npos);

    const std::size_t header_end = in_body(ed, "cut.txt:2,1") + std::string{"cut.txt:2,1"}.size();
    edit(ed, header_end + 1, 0, "tX\ntY\n");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(file_text(path), std::string{"t1\ntX\ntY\nt5\n"});
    EXPECT_TRUE(view(ed).find("cut.txt:2,3") != std::string::npos);
  }

  TEST_CASE("excerpt save: a file with no trailing newline keeps not having one");
  {
    const fs::path path = scratch.Write("tail.txt", "f1\nf2");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 2, 1, "f2"}}, "f2");

    edit(ed, in_body(ed, "f2"), 2, "f2y");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk"), std::string{"wrote 1 hunk"});
    EXPECT_EQ(file_text(path), std::string{"f1\nf2y"});
  }

  const auto buffer_text = [](const Editor& ed, const fs::path& p) {
    const std::size_t at = FindFileBuffer(ed, p);
    if (at >= BufferCount(ed)) return std::string{"<not open>"};
    const Document& doc = BufferAt(ed, at);
    return ReadDocRange(doc.table, {0, DocLength(doc.table)});
  };

  TEST_CASE("excerpt save: a file open in a buffer is written, and the buffer goes with it");
  {
    const fs::path path = scratch.Write("open.txt", "o1\no2\no3\no4\no5\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, path.string()));
    ed.doc.selections.Set(Selection{12, 12, -1});
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 3, 1, "o3"}}, "o3");

    edit(ed, in_body(ed, "o3"), 2, "o3xx");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_EQ(file_text(path), std::string{"o1\no2\no3xx\no4\no5\n"});
    EXPECT_EQ(buffer_text(ed, path), std::string{"o1\no2\no3xx\no4\no5\n"});
    const std::size_t at = FindFileBuffer(ed, path);
    EXPECT_TRUE(at < BufferCount(ed));
    EXPECT_FALSE(BufferAt(ed, at).modified);

    SwitchToBuffer(ed, at);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"open.txt"});
    EXPECT_EQ(Cur(ed), Index{14});
    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.empty());
    EXPECT_TRUE(CanUndo(ed.doc.table));
    RunCommands(ed, {"undo"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}),
              std::string{"o1\no2\no3\no4\no5\n"});
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_EQ(Cur(ed), Index{12});
    EXPECT_EQ(file_text(path), std::string{"o1\no2\no3xx\no4\no5\n"});

    RunTypableCommand(ed, "w");
    EXPECT_EQ(file_text(path), std::string{"o1\no2\no3\no4\no5\n"});
    RunCommands(ed, {"redo"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}),
              std::string{"o1\no2\no3xx\no4\no5\n"});
    EXPECT_TRUE(ed.doc.modified);
  }

  TEST_CASE("excerpt save: one undo step per file, in the buffer that file is open in");
  {
    const fs::path fa = scratch.Write("two_a.txt", "a1\na2\na3\na4\na5\na6\na7\na8\na9\n");
    const fs::path fb = scratch.Write("two_b.txt", "b1\nb2\nb3\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    EXPECT_TRUE(OpenTarget(ed, fa.string()));
    EXPECT_TRUE(OpenTarget(ed, fb.string()));
    OpenReferenceExcerpts(ed,
                          {Symbol{fa.string(), 2, 1, "x"}, Symbol{fa.string(), 8, 1, "x"},
                           Symbol{fb.string(), 2, 1, "x"}},
                          "x");

    edit(ed, in_body(ed, "a2"), 2, "A2");
    edit(ed, in_body(ed, "a8"), 2, "A8");
    edit(ed, in_body(ed, "b2"), 2, "B2");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 3 hunks into 2 files"), std::string{"wrote 3 hunks into 2 files"});
    EXPECT_EQ(file_text(fa), std::string{"a1\nA2\na3\na4\na5\na6\na7\nA8\na9\n"});
    EXPECT_EQ(file_text(fb), std::string{"b1\nB2\nb3\n"});
    EXPECT_EQ(buffer_text(ed, fa), std::string{"a1\nA2\na3\na4\na5\na6\na7\nA8\na9\n"});
    EXPECT_EQ(buffer_text(ed, fb), std::string{"b1\nB2\nb3\n"});

    SwitchToBuffer(ed, FindFileBuffer(ed, fa));
    RunCommands(ed, {"undo"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}),
              std::string{"a1\na2\na3\na4\na5\na6\na7\na8\na9\n"});
    EXPECT_EQ(buffer_text(ed, fb), std::string{"b1\nB2\nb3\n"});
    const std::string settled = ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
    RunCommands(ed, {"undo"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}), settled);
  }

  TEST_CASE("excerpt view: a reference to something that is not a file has no body");
  {
    const fs::path dir = scratch.dir / "a-directory";
    std::filesystem::create_directories(dir);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{dir.string(), 1, 1, "x"}}, "x");
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{1});
    if (ed.doc.excerpts.blocks.size() == 1) {
      EXPECT_TRUE(ed.doc.excerpts.blocks.front().no_body);
    }
    EXPECT_TRUE(view(ed).find("a-directory") != std::string::npos);

    edit(ed, view(ed).size(), 0, "typed under a header with nothing under it\n");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("no file bytes") != std::string::npos);
  }

  TEST_CASE("excerpt save: saving twice onto the same open buffer keeps working");
  {
    const fs::path path = scratch.Write("twice.txt", "t1\nt2\nt3\nt4\nt5\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    EXPECT_TRUE(OpenTarget(ed, path.string()));
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 3, 1, "t3"}}, "t3");

    edit(ed, in_body(ed, "t3"), 2, "t3a");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(file_text(path), std::string{"t1\nt2\nt3a\nt4\nt5\n"});

    edit(ed, in_body(ed, "t3a"), 3, "t3bb");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk"), std::string{"wrote 1 hunk"});
    EXPECT_EQ(file_text(path), std::string{"t1\nt2\nt3bb\nt4\nt5\n"});
    EXPECT_EQ(buffer_text(ed, path), std::string{"t1\nt2\nt3bb\nt4\nt5\n"});

    SwitchToBuffer(ed, FindFileBuffer(ed, path));
    RunCommands(ed, {"undo"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}),
              std::string{"t1\nt2\nt3a\nt4\nt5\n"});
    RunCommands(ed, {"undo"});
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}),
              std::string{"t1\nt2\nt3\nt4\nt5\n"});
  }

  TEST_CASE("excerpt save: a pane parked past the file's new end still has text to draw");
  {
    std::string many;
    for (int i = 1; i <= 60; ++i) many += "y" + std::to_string(i) + "\n";
    const fs::path path = scratch.Write("shrink.txt", many);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 40;
    EXPECT_TRUE(OpenTarget(ed, path.string()));

    ed.doc.view.top_line = 50;
    SplitWindow(ed, true);
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 30, 1, "y30"}}, "y30");
    EXPECT_TRUE(view(ed).find("shrink.txt:1,60") != std::string::npos);

    const std::size_t file_at = FindFileBuffer(ed, path);
    EXPECT_TRUE(file_at < BufferCount(ed));
    int parked = 0;
    for (const WindowNode& node : ed.windows) {
      if (node.dead || (node.kind != WindowNode::Kind::kLeaf)) continue;
      if ((node.buffer == file_at) && (node.view.top_line == 50)) ++parked;
    }
    EXPECT_EQ(parked, 1);

    const std::size_t start = in_body(ed, "y1\n");
    const std::size_t keep = in_body(ed, "y56\n");
    EXPECT_TRUE((start != std::string::npos) && (keep > start));
    edit(ed, start, keep - start, "");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk"), std::string{"wrote 1 hunk"});
    EXPECT_EQ(file_text(path), std::string{"y56\ny57\ny58\ny59\ny60\n"});

    const Index lines = LineCount(BufferAt(ed, file_at).table);
    EXPECT_TRUE(lines <= 6);
    for (const WindowNode& node : ed.windows) {
      if (node.dead || (node.kind != WindowNode::Kind::kLeaf)) continue;
      if (node.buffer != file_at) continue;
      EXPECT_TRUE(node.view.top_line < lines);
    }
    EXPECT_TRUE(BufferAt(ed, file_at).view.top_line < lines);
  }

  TEST_CASE("excerpt save: a buffer with unsaved edits stops the save, and nothing is written");
  {
    const fs::path path = scratch.Write("dirty.txt", "u1\nu2\nu3\nu4\nu5\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, path.string()));
    TypeInto(ed, 'Z');
    EXPECT_TRUE(ed.doc.modified);
    const std::string dirty = ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});

    OpenReferenceExcerpts(ed, {Symbol{path.string(), 3, 1, "u3"}}, "u3");
    edit(ed, in_body(ed, "u3"), 2, "u3x");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("unsaved edits in a buffer") != std::string::npos);
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_EQ(file_text(path), std::string{"u1\nu2\nu3\nu4\nu5\n"});
    EXPECT_EQ(buffer_text(ed, path), dirty);
  }

  TEST_CASE("excerpt save: a buffer that has drifted from its file stops the save");
  {
    const fs::path path = scratch.Write("stale.txt", "v1\nv2\nv3\nv4\nv5\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    EXPECT_TRUE(OpenTarget(ed, path.string()));
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 3, 1, "v3"}}, "v3");

    scratch.Write("stale.txt", "v1\nv2\nv3\nv4\nCHANGED\n");
    edit(ed, in_body(ed, "v3"), 2, "v3x");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("differs from the buffer holding it") != std::string::npos);
    EXPECT_EQ(file_text(path), std::string{"v1\nv2\nv3\nv4\nCHANGED\n"});
    EXPECT_EQ(buffer_text(ed, path), std::string{"v1\nv2\nv3\nv4\nv5\n"});
    EXPECT_TRUE(ed.doc.modified);
  }

  TEST_CASE("excerpt save: the other window on the file follows the splice");
  {
    const fs::path path = scratch.Write("split.txt", "w1\nw2\nw3\nw4\nw5\nw6\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    EXPECT_TRUE(OpenTarget(ed, path.string()));
    ed.doc.selections.Set(Selection{15, 15, -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    SplitWindow(ed, true);
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 2, 1, "w2"}}, "w2");

    edit(ed, in_body(ed, "w2"), 2, "w2yyy");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(file_text(path), std::string{"w1\nw2yyy\nw3\nw4\nw5\nw6\n"});

    FocusWindow(ed, true);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"split.txt"});
    EXPECT_EQ(Cur(ed), Index{18});
  }

  TEST_CASE("excerpt save: the jump keeps working while the view holds unsaved edits");
  {
    std::string six;
    for (int i = 1; i <= 12; ++i) six += "s" + std::to_string(i) + "\n";
    const fs::path path = scratch.Write("drift.txt", six);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(
        ed, {Symbol{path.string(), 3, 1, "s3"}, Symbol{path.string(), 9, 1, "s3"}}, "s3");

    edit(ed, in_body(ed, "s3\n") + 3, 0, "AA\nBB\n");
    const Index s9 = LineAt(ed.doc.table, static_cast<Index>(in_body(ed, "s9")));
    ed.doc.selections.Set(Selection{LineStart(ed.doc.table, s9), LineStart(ed.doc.table, s9), -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    RunCommands(ed, {"goto_excerpt_source"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"drift.txt"});
    EXPECT_EQ(LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary())),
              Index{8});
  }

  TEST_CASE("excerpt save: rebuilding is refused while edits are unsaved");
  {
    const fs::path path = scratch.Write("hold.txt", "h1\nh2\nh3\nh4\nh5\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 3, 1, "h3"}}, "h3");

    edit(ed, in_body(ed, "h3"), 2, "h3x");
    ed.status.clear();
    RunTypableCommand(ed, "set-excerpt-context 4");
    EXPECT_TRUE(ed.status.find("unsaved edits") != std::string::npos);
    EXPECT_TRUE(view(ed).find("h3x") != std::string::npos);
  }
}

void ExcerptUndoRevert() {
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-excerpt-revert"};
  const fs::path home = scratch.Write("home.txt", "just somewhere to stand\n");
  // Rebuilding a references view re-scans the project. Left unset, the scan
  // falls back to the built-in `find .` over the working directory -- the build
  // tree when the suite runs from build/ -- so what it finds depends on the
  // machine. Keep every scan inside this fixture.
  const std::string pinned_filter = "find " + scratch.dir.string() + " -type f -printf '%p\\n'";

  const auto view = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  const auto file_text = [](const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };
  const auto in_body = [&view](Editor& ed, std::string_view needle) {
    const std::string text = view(ed);
    return text.find(needle, text.find("\n\n") + 2);
  };
  const auto edit = [](Editor& ed, std::size_t at, std::size_t take, std::string_view put) {
    if (take > 0) std::ignore = Delete(static_cast<Index>(at), static_cast<Index>(at + take), ed.doc.table);
    if (!put.empty()) std::ignore = Insert(put, static_cast<Index>(at), ed.doc.table);
    ed.doc.modified = true;
  };
  const auto cursor_at = [](Editor& ed, std::size_t at) {
    const Index i = static_cast<Index>(at);
    ed.doc.selections.Set(Selection{i, i, -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
  };
  const auto twelve = [](char letter) {
    std::string body;
    for (int i = 1; i <= 12; ++i) body += std::string{letter} + std::to_string(i) + "\n";
    return body;
  };

  TEST_CASE("excerpt revert: undo then :w writes the old bytes back, across renumbering");
  {
    const std::string pristine = twelve('g');
    const fs::path path = scratch.Write("revert.txt", pristine);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 3, 1, "g3"}}, "g3");

    edit(ed, in_body(ed, "g3\n") + 3, 0, "NEW\n");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_TRUE(file_text(path).find("g3\nNEW\ng4\n") != std::string::npos);
    EXPECT_TRUE(view(ed).find("revert.txt:2,5") != std::string::npos);

    RunCommands(ed, {"undo", "undo"});
    EXPECT_TRUE(view(ed).find("revert.txt:2,4") != std::string::npos);
    EXPECT_TRUE(view(ed).find("NEW") == std::string::npos);
    EXPECT_TRUE(ed.doc.modified);

    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_EQ(file_text(path), pristine);
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_TRUE(view(ed).find("revert.txt:2,4") != std::string::npos);

    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("nothing to write") != std::string::npos);
  }

  TEST_CASE("excerpt revert: an undo chain across two saves still parses and reverts");
  {
    const std::string pristine = twelve('t');
    const fs::path path = scratch.Write("twice.txt", pristine);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 3, 1, "t3"}}, "t3");

    edit(ed, in_body(ed, "t3\n") + 3, 0, "AA\n");
    RunTypableCommand(ed, "w");
    edit(ed, in_body(ed, "AA\n") + 3, 0, "BB\n");
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(view(ed).find("twice.txt:2,6") != std::string::npos);
    EXPECT_TRUE(file_text(path).find("t3\nAA\nBB\nt4\n") != std::string::npos);

    RunCommands(ed, {"undo", "undo", "undo", "undo"});
    EXPECT_TRUE(view(ed).find("twice.txt:2,4") != std::string::npos);
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_EQ(file_text(path), pristine);
  }

  TEST_CASE("excerpt revert: undo of a drop re-adopts the hunk; a save cannot fold it away");
  {
    const std::string pristine = twelve('k');
    const fs::path path = scratch.Write("keep.txt", pristine);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.file_filter = pinned_filter;
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(
        ed, {Symbol{path.string(), 3, 1, "k3"}, Symbol{path.string(), 9, 1, "k3"}}, "k3");

    edit(ed, in_body(ed, "k3"), 2, "k3x");
    cursor_at(ed, in_body(ed, "k9"));
    RunCommands(ed, {"excerpt_drop"});
    EXPECT_TRUE(ed.status.find("dropped 1 excerpt") != std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{1});
    EXPECT_EQ(ed.doc.excerpts.dropped.size(), std::size_t{1});

    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("keep.txt:8,10") != std::string::npos);
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_TRUE(file_text(path).find("k2\nk3x\nk4\n") != std::string::npos);
    EXPECT_TRUE(file_text(path).find("k8\nk9\nk10\n") != std::string::npos);
    EXPECT_TRUE(file_text(path).find("keep.txt") == std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{2});
    EXPECT_TRUE(ed.doc.excerpts.dropped.empty());

    RunTypableCommand(ed, "set-excerpt-context 2");
    EXPECT_TRUE(view(ed).find("k3x") != std::string::npos);
    EXPECT_TRUE(view(ed).find("k9") != std::string::npos);
  }

  TEST_CASE("excerpt revert: a resurrected hunk can be dropped again, and jumped from");
  {
    const fs::path path = scratch.Write("again.txt", twelve('d'));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(
        ed, {Symbol{path.string(), 3, 1, "d3"}, Symbol{path.string(), 9, 1, "d3"}}, "d3");

    cursor_at(ed, in_body(ed, "d9"));
    RunCommands(ed, {"excerpt_drop"});
    RunCommands(ed, {"undo"});
    cursor_at(ed, in_body(ed, "d9"));
    ed.status.clear();
    RunCommands(ed, {"excerpt_drop"});
    EXPECT_TRUE(ed.status.find("dropped 1 excerpt") != std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{1});
    EXPECT_TRUE(view(ed).find("again.txt:8,10") == std::string::npos);

    RunCommands(ed, {"undo"});
    cursor_at(ed, in_body(ed, "d9"));
    RunCommands(ed, {"goto_excerpt_source"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"again.txt"});
    EXPECT_EQ(LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary())),
              Index{8});
  }

  TEST_CASE("excerpt revert: undo across an auto-refresh behaves like undo across a reload");
  {
    std::string pristine;
    for (int i = 1; i <= 12; ++i) pristine += "w" + std::to_string(i) + "\n";
    const fs::path path = scratch.Write("watch.txt", pristine);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":3");
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_TRUE(view(ed).find("w3") != std::string::npos);

    std::string moved = pristine;
    EXPECT_TRUE(ReplaceFirst(moved, "w3", "W3"));
    scratch.Write("watch.txt", moved);
    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(view(ed).find("W3") != std::string::npos);
    EXPECT_FALSE(ed.doc.modified);

    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("w3") != std::string::npos);
    EXPECT_TRUE(view(ed).find("W3") == std::string::npos);
    EXPECT_TRUE(ed.doc.excerpts.blocks[0].original.find("w3\n") != std::string::npos);
    EXPECT_TRUE(ed.doc.modified);

    RunCommands(ed, {"redo"});
    EXPECT_TRUE(view(ed).find("W3") != std::string::npos);
    EXPECT_TRUE(ed.doc.excerpts.blocks[0].original.find("W3\n") != std::string::npos);
    EXPECT_FALSE(ed.doc.modified);

    RunCommands(ed, {"undo"});
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk"), std::string{"wrote 1 hunk"});
    EXPECT_EQ(file_text(path), pristine);
    EXPECT_FALSE(ed.doc.modified);

    edit(ed, in_body(ed, "w2"), 2, "w2z");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(file_text(path).find("w2z\n") != std::string::npos);
    EXPECT_TRUE(file_text(path).find("w3\n") != std::string::npos);
  }

  TEST_CASE("excerpt revert: undo puts back a line the file lost, and :w writes it home");
  {
    std::string pristine;
    for (int i = 1; i <= 12; ++i) pristine += "r" + std::to_string(i) + "\n";
    const fs::path path = scratch.Write("revert.txt", pristine);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":6");
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("revert.txt:5,7") != std::string::npos);

    std::string cut = pristine;
    cut.erase(cut.find("r6\n"), 3);
    cut.erase(cut.find("r2\n"), 3);
    scratch.Write("revert.txt", cut);
    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(view(ed).find("r6") == std::string::npos);
    EXPECT_FALSE(ed.doc.modified);

    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("r6") != std::string::npos);
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk"), std::string{"wrote 1 hunk"});
    std::string want = pristine;
    want.erase(want.find("r2\n"), 3);
    EXPECT_EQ(file_text(path), want);
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("excerpt revert: a file rewritten inside one mtime tick still counts as moved");
  {
    std::string pristine;
    for (int i = 1; i <= 12; ++i) pristine += "t" + std::to_string(i) + "\n";
    const fs::path path = scratch.Write("sametick.txt", pristine);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":6");
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("sametick.txt:5,7") != std::string::npos);

    // The stamp the view took when it read the file, put back onto the file
    // after the change below -- so the change carries the very mtime the model
    // already holds. That is not a contrived state: it is what a filesystem
    // with a coarse mtime does by itself (a whole second on ext3, HFS+ and
    // many NFS mounts, a clock tick on Linux for any inode nobody stat()ed in
    // between), and what writing through a temp file and a rename does even on
    // a fine-grained one, because the fresh inode's stamp comes off the coarse
    // clock. Forced here so the case is the same on every filesystem this
    // suite runs on, instead of turning up as a flake on the ones where two
    // writes happen to land in one tick.
    const auto stamped = fs::last_write_time(path);
    std::string cut = pristine;
    cut.erase(cut.find("t6\n"), 3);
    cut.erase(cut.find("t2\n"), 3);
    scratch.Write("sametick.txt", cut);
    fs::last_write_time(path, stamped);
    EXPECT_TRUE(fs::last_write_time(path) == stamped);

    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(view(ed).find("t6") == std::string::npos);
    EXPECT_FALSE(ed.doc.modified);

    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("t6") != std::string::npos);
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk"), std::string{"wrote 1 hunk"});
    std::string want = pristine;
    want.erase(want.find("t2\n"), 3);
    EXPECT_EQ(file_text(path), want);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("excerpt revert: a region koi cannot find again is refused, and says so");
  {
    std::string pristine;
    for (int i = 1; i <= 12; ++i) pristine += "g" + std::to_string(i) + "\n";
    const fs::path path = scratch.Write("gone.txt", pristine);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":6");
    PumpUntilIdle(ed);

    std::string other;
    for (int i = 1; i <= 12; ++i) other += "z" + std::to_string(i) + "\n";
    scratch.Write("gone.txt", other);
    MaybeRefreshExcerptView(ed);
    RunCommands(ed, {"undo"});
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("moved too far") != std::string::npos);
    EXPECT_EQ(file_text(path), other);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("excerpt revert: a save resyncs dropped hunks, so their resurrection stays current");
  {
    const std::string pristine = twelve('p');
    const fs::path path = scratch.Write("park.txt", pristine);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(
        ed, {Symbol{path.string(), 3, 1, "p3"}, Symbol{path.string(), 9, 1, "p3"}}, "p3");

    edit(ed, in_body(ed, "p3\n") + 3, 0, "NEW\n");
    cursor_at(ed, in_body(ed, "p9"));
    RunCommands(ed, {"excerpt_drop"});
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_EQ(ed.doc.excerpts.dropped[0].block.first, Index{9});

    RunCommands(ed, {"undo", "undo"});
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("nothing to write") != std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{2});
    EXPECT_EQ(ed.doc.excerpts.blocks[1].first, Index{9});

    RunCommands(ed, {"undo"});
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_EQ(file_text(path), pristine);
    EXPECT_EQ(ed.doc.excerpts.blocks[1].first, Index{8});
  }

  TEST_CASE("excerpt revert: a context rebuild is one undo step, both directions");
  {
    const fs::path path = scratch.Write("width.txt", twelve('c'));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 4, 1, "c4"}}, "c4");
    EXPECT_TRUE(view(ed).find("width.txt:3,5") != std::string::npos);

    RunTypableCommand(ed, "set-excerpt-context 3");
    EXPECT_TRUE(view(ed).find("width.txt:1,7") != std::string::npos);
    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("width.txt:3,5") != std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks[0].first, Index{3});
    RunCommands(ed, {"redo"});
    EXPECT_TRUE(view(ed).find("width.txt:1,7") != std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks[0].first, Index{1});
    EXPECT_FALSE(ed.doc.modified);
  }

  TEST_CASE("excerpt revert: three refreshes back and forward, each epoch's model in step");
  {
    const auto versioned = [](std::string_view v) {
      std::string body;
      for (int i = 1; i <= 12; ++i) {
        body += (i == 3) ? std::string{v} + "\n" : "e" + std::to_string(i) + "\n";
      }
      return body;
    };
    const fs::path path = scratch.Write("epochs.txt", versioned("vv1"));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":3");
    PumpUntilIdle(ed);
    for (const char* v : {"vv2", "vv3", "vv4"}) {
      scratch.Write("epochs.txt", versioned(v));
      MaybeRefreshExcerptView(ed);
      EXPECT_TRUE(view(ed).find(v) != std::string::npos);
      EXPECT_EQ(EditorInvariants(ed), std::string{});
    }
    for (const char* v : {"vv3", "vv2", "vv1"}) {
      RunCommands(ed, {"undo"});
      EXPECT_TRUE(view(ed).find(v) != std::string::npos);
      EXPECT_TRUE(ed.doc.excerpts.blocks[0].original.find(v) != std::string::npos);
      EXPECT_EQ(EditorInvariants(ed), std::string{});
    }
    for (const char* v : {"vv2", "vv3", "vv4"}) {
      RunCommands(ed, {"redo"});
      EXPECT_TRUE(view(ed).find(v) != std::string::npos);
      EXPECT_TRUE(ed.doc.excerpts.blocks[0].original.find(v) != std::string::npos);
      EXPECT_EQ(EditorInvariants(ed), std::string{});
    }
    EXPECT_FALSE(ed.doc.modified);
  }

  TEST_CASE("excerpt revert: a save from a past epoch abandons the branch and stays coherent");
  {
    const std::string pristine = twelve('b');
    const fs::path path = scratch.Write("branch.txt", pristine);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":3");
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("branch.txt:2,4") != std::string::npos);

    RunTypableCommand(ed, "set-excerpt-context 2");
    EXPECT_EQ(ed.doc.excerpt_epochs.boundaries.size(), std::size_t{1});
    EXPECT_TRUE(view(ed).find("branch.txt:1,5") != std::string::npos);

    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("branch.txt:2,4") != std::string::npos);
    edit(ed, in_body(ed, "b3\n") + 3, 0, "NEW\n");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_TRUE(file_text(path).find("b3\nNEW\nb4\n") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    RunTypableCommand(ed, "set-excerpt-context 2");
    EXPECT_EQ(ed.doc.excerpt_epochs.boundaries.size(), std::size_t{1});
    EXPECT_TRUE(view(ed).find("branch.txt:1,5") != std::string::npos);
    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("branch.txt:2,5") != std::string::npos);
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("nothing to write") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("excerpt revert: an edit after undo forgets the epoch it abandoned");
  {
    const fs::path path = scratch.Write("dead.txt", twelve('d'));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":3");
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("dead.txt:2,4") != std::string::npos);

    RunTypableCommand(ed, "set-excerpt-context 2");
    EXPECT_EQ(ed.doc.excerpt_epochs.boundaries.size(), std::size_t{1});
    EXPECT_EQ(ed.doc.excerpt_epochs.active, std::size_t{1});

    RunCommands(ed, {"undo"});
    EXPECT_EQ(ed.doc.excerpt_epochs.active, std::size_t{0});
    EXPECT_EQ(ed.doc.excerpt_epochs.boundaries.size(), std::size_t{1});

    cursor_at(ed, in_body(ed, "d3"));
    RunCommands(ed, {"delete_char_forward"});
    EXPECT_TRUE(view(ed).find("d3") == std::string::npos);
    EXPECT_TRUE(ed.doc.excerpt_epochs.boundaries.empty());
    EXPECT_EQ(ed.doc.excerpt_epochs.active, std::size_t{0});
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("d3") != std::string::npos);
    EXPECT_TRUE(view(ed).find("dead.txt:2,4") != std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks[0].first, Index{2});
    RunCommands(ed, {"redo"});
    EXPECT_TRUE(view(ed).find("d3") == std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks[0].first, Index{2});
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_TRUE(file_text(path).find("\n3\n") != std::string::npos);
    EXPECT_FALSE(ed.doc.modified);

    RunTypableCommand(ed, "set-excerpt-context 3");
    EXPECT_EQ(ed.doc.excerpt_epochs.boundaries.size(), std::size_t{1});
    EXPECT_TRUE(view(ed).find("dead.txt:1,6") != std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks[0].first, Index{1});
    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("dead.txt:2,4") != std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks[0].first, Index{2});
    EXPECT_EQ(ed.doc.excerpt_epochs.active, std::size_t{0});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("excerpt revert: undoing past the epoch cap degrades to refusal, never corruption");
  {
    const auto stamped = [](int v) {
      std::string body;
      for (int i = 1; i <= 12; ++i) {
        body += (i == 3) ? "cap" + std::to_string(v) + "\n" : "z" + std::to_string(i) + "\n";
      }
      return body;
    };
    const fs::path path = scratch.Write("cap.txt", stamped(0));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":3");
    PumpUntilIdle(ed);
    for (int v = 1; v <= 10; ++v) {
      scratch.Write("cap.txt", stamped(v));
      MaybeRefreshExcerptView(ed);
    }
    EXPECT_EQ(ed.doc.excerpt_epochs.boundaries.size(), std::size_t{8});
    const std::string before = file_text(path);
    for (int back = 0; back < 12; ++back) {
      RunCommands(ed, {"undo"});
      RunTypableCommand(ed, "w");
      EXPECT_EQ(EditorInvariants(ed), std::string{});
    }
    EXPECT_TRUE(file_text(path).find("cap") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("excerpt revert: a save re-stamps its own writes, so the refresh spares the undo history");
  {
    const std::string pristine = twelve('r');
    const fs::path path = scratch.Write("stamp.txt", pristine);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 3, 1, "r3"}}, "r3");

    edit(ed, in_body(ed, "r3\n") + 3, 0, "NEW\n");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});

    MaybeRefreshExcerptView(ed);
    EXPECT_FALSE(ed.doc.modified);
    RunCommands(ed, {"undo", "undo"});
    EXPECT_TRUE(view(ed).find("NEW") == std::string::npos);
    RunTypableCommand(ed, "w");
    EXPECT_EQ(file_text(path), pristine);
  }
}

// Every excerpt path that reads a whole file, made to fail on purpose.
//
// ReadWholeFile answered a failed read with an empty string and, on the read
// itself failing part-way, with a clear error code as well. The excerpt
// machinery called it through an overload that had no error code at all, so
// "this file could not be read" and "this file is empty" were the same answer
// to a build, to a re-anchor, and to the writeback -- and a read that died
// mid-file was worse than either: the truncated image was spliced and written,
// deleting every byte past the failure. These pin what each of the three does
// now that the failure arrives as a failure.
//
// A read that fails part-way cannot be provoked from inside the suite on a file
// koi is willing to excerpt; that class is covered by the LD_PRELOAD shim, and
// by ReadWholeFileContract's /proc/self/mem case at the function's own edge.
void ExcerptReadFailuresAreNamed() {
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-excerpt-read-failure"};
  const fs::path home = scratch.Write("home.txt", "just somewhere to stand\n");

  const auto view = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  const auto file_text = [](const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };
  const auto in_body = [&view](Editor& ed, std::string_view needle) {
    const std::string text = view(ed);
    return text.find(needle, text.find("\n\n") + 2);
  };
  const auto edit = [](Editor& ed, std::size_t at, std::size_t take, std::string_view put) {
    if (take > 0) {
      std::ignore = Delete(static_cast<Index>(at), static_cast<Index>(at + take), ed.doc.table);
    }
    if (!put.empty()) std::ignore = Insert(put, static_cast<Index>(at), ed.doc.table);
    ed.doc.modified = true;
  };

  TEST_CASE("excerpt read failures: the view, the save and the empty file all say which");

  if (::getuid() == 0) {
    // root reads through the mode bits, so there is no unreadable file to make.
    // The shim covers this class from outside when the suite runs as root.
    EXPECT_TRUE(true);
    return;
  }
  const std::string denied = std::make_error_code(std::errc::permission_denied).message();
  const auto lock = [](const fs::path& p, bool shut) {
    std::error_code ec;
    fs::permissions(p,
                    shut ? fs::perms::none : (fs::perms::owner_read | fs::perms::owner_write),
                    fs::perm_options::replace, ec);
    return !ec;
  };

  // A source koi cannot read gets a block that says so. It is still bodyless --
  // there is no body to be had -- but "no bytes" and "no bytes we could get"
  // are no longer the same block, and the reason travels in the header where
  // the user is already looking.
  {
    const fs::path locked = scratch.Write("locked.txt", "kappa1\nkappa2\nkappa3\nkappa4\nkappa5\n");
    EXPECT_TRUE(lock(locked, true));

    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{locked.string(), 3, 1, "kappa3"}}, "kappa3");
    EXPECT_TRUE(IsExcerptView(ed.doc));

    EXPECT_TRUE(view(ed).find("cannot read --") != std::string::npos);
    EXPECT_TRUE(view(ed).find(denied) != std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{1});
    if (!ed.doc.excerpts.blocks.empty()) {
      EXPECT_TRUE(ed.doc.excerpts.blocks.front().no_body);
    }
    // kappa2 is context, so it is in no title and no header: its absence is the
    // body's absence, and its presence would be the file having been read.
    EXPECT_TRUE(view(ed).find("kappa2") == std::string::npos);

    // Readable again, the same refs build the body and drop the note -- so the
    // assertions above are about the failed read and not about this fixture.
    EXPECT_TRUE(lock(locked, false));
    Editor fine;
    fine.theme = BuiltinTheme();
    fine.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(fine, home.string()));
    OpenReferenceExcerpts(fine, {Symbol{locked.string(), 3, 1, "kappa3"}}, "kappa3");
    EXPECT_TRUE(view(fine).find("cannot read") == std::string::npos);
    EXPECT_TRUE(view(fine).find("kappa2") != std::string::npos);
  }

  // The save reads every file it is about to write, to splice the hunks into
  // it. That read failing used to come out as "cannot read <path> to write it
  // back" with no reason -- the same sentence an empty file got -- and a read
  // that failed part-way did not come out at all.
  {
    const std::string pristine = "s1\ns2\ns3\ns4\ns5\n";
    const fs::path path = scratch.Write("save.txt", pristine);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 3, 1, "s3"}}, "s3");

    edit(ed, in_body(ed, "s3"), 2, "s3x");
    EXPECT_TRUE(lock(path, true));
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "cannot read"), std::string{"cannot read"});
    EXPECT_TRUE(ed.status.find(path.string()) != std::string::npos);
    EXPECT_TRUE(ed.status.find(denied) != std::string::npos);
    // Refused, not half-done: the edit is still the user's to retry.
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_TRUE(lock(path, false));
    EXPECT_EQ(file_text(path), pristine);
  }

  // And the branch that refusal used to share: a file that really is empty.
  // Different sentence, because only one of the two is worth retrying.
  {
    const fs::path path = scratch.Write("emptied.txt", "e1\ne2\ne3\ne4\ne5\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{path.string(), 3, 1, "e3"}}, "e3");

    edit(ed, in_body(ed, "e3"), 2, "e3x");
    { std::ofstream truncate{path, std::ios::binary | std::ios::trunc}; }
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "is empty -- rebuild the view"),
              std::string{"is empty -- rebuild the view"});
    EXPECT_TRUE(ed.status.find("cannot read") == std::string::npos);
    EXPECT_EQ(file_text(path), std::string{});
  }
}

void ExcerptCommandModes() {
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-excerpt-from"};
  const fs::path home = scratch.Write("home.txt", "somewhere to stand\n");

  const auto view = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  const auto file_text = [](const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };
  const auto in_body = [&view](Editor& ed, std::string_view needle) {
    const std::string text = view(ed);
    return text.find(needle, text.find("\n\n") + 2);
  };
  const auto edit = [](Editor& ed, std::size_t at, std::size_t take, std::string_view put) {
    if (take > 0) std::ignore = Delete(static_cast<Index>(at), static_cast<Index>(at + take), ed.doc.table);
    if (!put.empty()) std::ignore = Insert(put, static_cast<Index>(at), ed.doc.table);
    ed.doc.modified = true;
  };
  const auto cursor_at = [](Editor& ed, std::size_t at) {
    const Index i = static_cast<Index>(at);
    ed.doc.selections.Set(Selection{i, i, -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
  };
  const auto twelve = [](char letter) {
    std::string body;
    for (int i = 1; i <= 12; ++i) body += std::string{letter} + std::to_string(i) + "\n";
    return body;
  };

  TEST_CASE(":from-with-msg -- the diagnostic rides the header, colons and all");
  {
    std::string body = twelve('d');
    EXPECT_TRUE(ReplaceFirst(body, "d3", "abcdefgh"));
    const fs::path path = scratch.Write("diag.txt", body);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from-with-msg echo " + path.string() + ":3:5: error: bad: colons");
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_TRUE(ed.doc.excerpts.with_msg);
    EXPECT_TRUE(view(ed).find("diag.txt:2,4  error: bad: colons") != std::string::npos);

    {
      Editor go;
      go.theme = BuiltinTheme();
      go.settings.excerpt_context = 1;
      EXPECT_TRUE(OpenTarget(go, home.string()));
      RunTypableCommand(go, "from-with-msg echo " + path.string() + ":3:5: error: bad: colons");
    PumpUntilIdle(go);
      cursor_at(go, view(go).find("diag.txt:2,4"));
      RunCommands(go, {"goto_excerpt_source"});
      EXPECT_EQ(go.doc.file.filename().string(), std::string{"diag.txt"});
      const Index cursor = CursorOf(go.doc.table, go.doc.selections.Primary());
      EXPECT_EQ(LineAt(go.doc.table, cursor), Index{2});
      EXPECT_EQ(cursor - LineStart(go.doc.table, Index{2}), Index{4});
    }

    edit(ed, in_body(ed, "abcdefgh\n") + 9, 0, "NEW\n");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_TRUE(view(ed).find("diag.txt:2,5  error: bad: colons") != std::string::npos);
    EXPECT_TRUE(file_text(path).find("abcdefgh\nNEW\n") != std::string::npos);
  }

  TEST_CASE(":from-with-msg -- diagnostics on one line pool their messages");
  {
    const fs::path path = scratch.Write("pool.txt", twelve('p'));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from-with-msg echo " + path.string() + ":3:1: first && echo " +
                              path.string() + ":3:9: second");
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("pool.txt:2,4  first · second") != std::string::npos);
  }

  TEST_CASE(":from is a snapshot -- the command never re-runs by itself");
  {
    const fs::path path = scratch.Write("snap.txt", twelve('s'));
    const fs::path counter = scratch.dir / "snap_runs.txt";
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo x >> " + counter.string() + " && echo " + path.string() + ":3");
    PumpUntilIdle(ed);
    EXPECT_FALSE(ed.doc.excerpts.watched);
    EXPECT_EQ(file_text(counter), std::string{"x\n"});

    edit(ed, in_body(ed, "s3\n") + 3, 0, "NEW\n");
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(view(ed).find("snap.txt:2,5") != std::string::npos);
    EXPECT_EQ(file_text(counter), std::string{"x\n"});

    std::string moved = file_text(path);
    const std::size_t s2_at = moved.find("s2");
    EXPECT_TRUE(s2_at != std::string::npos);
    if (s2_at == std::string::npos) return;  // replace(npos, ...) throws, losing every later case
    moved.replace(s2_at, 2, "ZZ");
    scratch.Write("snap.txt", moved);
    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(view(ed).find("ZZ") != std::string::npos);
    EXPECT_EQ(file_text(counter), std::string{"x\n"});
  }

  TEST_CASE(":from-watched -- a save re-runs the command and rebuilds the view");
  {
    const fs::path path = scratch.Write("watchw.txt", twelve('w'));
    const fs::path counter = scratch.dir / "watch_runs.txt";
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from-watched echo x >> " + counter.string() + " && echo " +
                              path.string() + ":3");
    PumpUntilIdle(ed);
    EXPECT_TRUE(ed.doc.excerpts.watched);
    EXPECT_EQ(file_text(counter), std::string{"x\n"});

    edit(ed, in_body(ed, "w3\n") + 3, 0, "NEW\n");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_TRUE(ed.status.find("re-running") != std::string::npos);
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).starts_with("1 hit from"));
    EXPECT_EQ(file_text(counter), std::string{"x\nx\n"});
    EXPECT_TRUE(view(ed).find("watchw.txt:2,4") != std::string::npos);
  }

  TEST_CASE(":from-cancel ends the running command; with nothing running it warns");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from-cancel");
    EXPECT_TRUE(ed.status.find("no :from command") != std::string::npos);

    RunTypableCommand(ed, "from sleep 30");
    EXPECT_TRUE(!ed.pending_commands.empty());
    RunTypableCommand(ed, "from-cancel");
    EXPECT_TRUE(ed.pending_commands.empty());
    EXPECT_TRUE(ed.status.find("cancelled") != std::string::npos);
  }

  TEST_CASE(":from-watched -- saving the file re-runs it, not only saving the view");
  {
    const fs::path path = scratch.Write("bysave.txt", twelve('n'));
    const fs::path flag = scratch.dir / "bysave_flag.txt";
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    const std::string cmd = "echo " + path.string() + ":3; test -f " + flag.string() +
                            " || echo " + path.string() + ":9";
    RunTypableCommand(ed, "from-watched " + cmd);
    PumpUntilIdle(ed);
    const std::size_t view_at = ed.active;
    EXPECT_TRUE(view(ed).find("bysave.txt:9,9") != std::string::npos);

    SplitWindow(ed, true);
    EXPECT_TRUE(OpenTarget(ed, path.string()));
    EXPECT_TRUE(ed.active != view_at);
    const auto pane_text = [&ed, view_at] {
      return ReadDocRange(BufferAt(ed, view_at).table,
                          {0, DocLength(BufferAt(ed, view_at).table)});
    };

    std::ofstream{flag, std::ios::binary} << "fixed\n";
    TypeInto(ed, 'Z');
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);
    EXPECT_TRUE(!ed.pending_commands.empty());
    PumpUntilIdle(ed);
    RefreshLiveExcerptViews(ed);
    EXPECT_TRUE(pane_text().find("bysave.txt:9,9") == std::string::npos);
    EXPECT_TRUE(pane_text().find("bysave.txt:3,3") != std::string::npos);
    EXPECT_TRUE(ed.active != view_at);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"bysave.txt"});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE(":from-watched -- a view in another pane keeps up with the file being fixed");
  {
    const fs::path path = scratch.Write("beside.txt", twelve('b'));
    const fs::path flag = scratch.dir / "beside_flag.txt";
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    const std::string cmd = "echo " + path.string() + ":3; test -f " + flag.string() +
                            " || echo " + path.string() + ":9";
    RunTypableCommand(ed, "from-watched " + cmd);
    PumpUntilIdle(ed);
    const std::size_t view_at = ed.active;
    EXPECT_TRUE(ed.doc.excerpts.watched);
    EXPECT_TRUE(view(ed).find("beside.txt:3,3") != std::string::npos);
    EXPECT_TRUE(view(ed).find("beside.txt:9,9") != std::string::npos);

    SplitWindow(ed, true);
    EXPECT_TRUE(OpenTarget(ed, path.string()));
    EXPECT_TRUE(ed.active != view_at);
    const auto pane_text = [&ed, view_at] {
      return ReadDocRange(BufferAt(ed, view_at).table,
                          {0, DocLength(BufferAt(ed, view_at).table)});
    };
    EXPECT_TRUE(pane_text().find("beside.txt:9,9") != std::string::npos);

    std::ofstream{flag, std::ios::binary} << "fixed\n";
    RunCommands(ed, {"jump_view_next"});
    EXPECT_EQ(ed.active, view_at);
    edit(ed, in_body(ed, "b3"), 2, "b3x");
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("re-running") != std::string::npos);
    RunCommands(ed, {"jump_view_next"});
    EXPECT_TRUE(ed.active != view_at);

    PumpUntilIdle(ed);
    EXPECT_TRUE(BufferAt(ed, view_at).excerpts.rebuild_on_focus);
    RefreshLiveExcerptViews(ed);
    EXPECT_TRUE(pane_text().find("beside.txt:9,9") == std::string::npos);
    EXPECT_TRUE(pane_text().find("beside.txt:3,3") != std::string::npos);
    EXPECT_TRUE(pane_text().find("1 hit from") != std::string::npos);
    EXPECT_TRUE(ed.active != view_at);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"beside.txt"});
    EXPECT_FALSE(BufferAt(ed, view_at).excerpts.rebuild_on_focus);
    EXPECT_FALSE(BufferAt(ed, view_at).modified);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE(":from-watched -- a fix that clears the output says so, and empties the view");
  {
    std::string body = twelve('f');
    EXPECT_TRUE(ReplaceFirst(body, "f3", "BUG"));
    const fs::path path = scratch.Write("fixed.txt", body);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from-watched grep -q BUG " + path.string() + " && echo " +
                              path.string() + ":3 || true");
    PumpUntilIdle(ed);
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{1});

    edit(ed, in_body(ed, "BUG"), 3, "OKK");
    RunTypableCommand(ed, "w");
    const std::string saved_said = ed.status;
    PumpUntilIdle(ed);
    EXPECT_EQ(ed.status.text(), saved_said);
    bool reported = false;
    for (const StatusRecord& entry : ed.status.log()) {
      reported = reported || (entry.text.find("reports nothing now") != std::string::npos);
    }
    EXPECT_TRUE(reported);
    EXPECT_TRUE(ed.doc.excerpts.blocks.empty());
    EXPECT_TRUE(view(ed).starts_with("0 hits from"));
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_TRUE(ed.doc.excerpts.watched);
    EXPECT_TRUE(file_text(path).find("OKK") != std::string::npos);
  }

  TEST_CASE(":messages -- displaced status lines replay in a view, oldest first");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    RunTypableCommand(ed, "messages");
    EXPECT_TRUE(ed.status.find("no messages yet") != std::string::npos);
    ed.status.clear();

    ed.status = "first thing said";
    ed.status = "first thing said";
    ed.status.Warn("second thing said");
    ed.status.Fail("third thing said");
    RunTypableCommand(ed, "messages");
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_EQ(ed.doc.view_name, std::string{"messages"});
    const std::string text = ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
    const std::size_t first = text.find("first thing said");
    const std::size_t second = text.find("warn: second thing said");
    const std::size_t third = text.find("error: third thing said");
    EXPECT_TRUE(first != std::string::npos);
    EXPECT_TRUE(second != std::string::npos);
    EXPECT_TRUE(third != std::string::npos);
    EXPECT_TRUE((first < second) && (second < third));
    EXPECT_EQ(text.find("first thing said", first + 1), std::string::npos);
    const Index at = CursorOf(ed.doc.table, ed.doc.selections.Primary());
    EXPECT_EQ(ReadDocRange(ed.doc.table, {at, at + 5}), std::string{"error"});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE(":from and :from-watched are two views; :watch/:unwatch flip one");
  {
    const fs::path path = scratch.Write("twice.txt", twelve('t'));
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    const std::string cmd = "echo " + path.string() + ":2";
    RunTypableCommand(ed, "from " + cmd);
    PumpUntilIdle(ed);
    EXPECT_EQ(ed.doc.view_name, "from: " + cmd);
    EXPECT_FALSE(ed.doc.excerpts.watched);
    RunTypableCommand(ed, "from-watched " + cmd);
    PumpUntilIdle(ed);
    EXPECT_EQ(ed.doc.view_name, "from!: " + cmd);
    EXPECT_TRUE(ed.doc.excerpts.watched);
    EXPECT_TRUE(FindViewBuffer(ed, "from: " + cmd) < BufferCount(ed));
    EXPECT_TRUE(FindViewBuffer(ed, "from!: " + cmd) < BufferCount(ed));
    RunTypableCommand(ed, "unwatch");
    EXPECT_TRUE(ed.doc.excerpts.watched);
    EXPECT_EQ(ed.doc.view_name, "from!: " + cmd);
    EXPECT_TRUE(ed.status.find("is already open") != std::string::npos);

    const std::size_t plain_at = FindViewBuffer(ed, "from: " + cmd);
    EXPECT_TRUE(plain_at < BufferCount(ed));
    SwitchToBuffer(ed, plain_at);
    RunTypableCommand(ed, "watch");
    EXPECT_FALSE(ed.doc.excerpts.watched);
    EXPECT_EQ(ed.doc.view_name, "from: " + cmd);
    EXPECT_TRUE(ed.status.find("is already open") != std::string::npos);
    EXPECT_EQ(RerunWatchedViews(ed), 1);
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    EXPECT_EQ(ed.pending_commands.front().view_name, "from!: " + cmd);
    KillAllCommandJobs(ed);

    const std::string other = "echo " + path.string() + ":3";
    RunTypableCommand(ed, "from-watched " + other);
    PumpUntilIdle(ed);
    EXPECT_EQ(ed.doc.view_name, "from!: " + other);
    RunTypableCommand(ed, "unwatch");
    EXPECT_FALSE(ed.doc.excerpts.watched);
    EXPECT_EQ(ed.doc.view_name, "from: " + other);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE(":from-cancel -- bare takes one job, `all` takes every one");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from sleep 30");
    RunTypableCommand(ed, "from sleep 31");
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{2});
    RunTypableCommand(ed, "from-cancel");
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    RunTypableCommand(ed, "from-cancel bogus");
    EXPECT_TRUE(ed.status.find("takes nothing") != std::string::npos);
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    RunTypableCommand(ed, "from sleep 32");
    RunTypableCommand(ed, "from-cancel all");
    EXPECT_TRUE(ed.status.find("cancelled 2") != std::string::npos);
    EXPECT_TRUE(ed.pending_commands.empty());
    RunTypableCommand(ed, "from-cancel all");
    EXPECT_TRUE(ed.status.find("no :from command is running") != std::string::npos);
  }

  TEST_CASE("armed states say so: f/t prompt, counts trust the bar chip");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "alpha bravo\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunCommands(ed, {"find_next_char"});
    EXPECT_EQ(ed.status.text(), std::string{"find char..."});
    ed.pending_char = PendingChar::kNone;
    RunCommands(ed, {"find_till_char"});
    EXPECT_EQ(ed.status.text(), std::string{"till char..."});
    ed.pending_char = PendingChar::kNone;
    ed.status.clear();

    const KeyMaps maps = DefaultKeyMaps();
    std::vector<Key> pending;
    Key three;
    EXPECT_TRUE(ParseKey("3", three));
    HandleKeyInput(ed, maps, three, pending);
    EXPECT_EQ(ed.pending_count, Index{3});
    EXPECT_TRUE(ed.status.empty());
  }

  TEST_CASE(":from replay stays anchored after saves shift the file");
  {
    const fs::path path = scratch.Write("anchor.txt", twelve('a'));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":3 && echo " + path.string() + ":9");
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("anchor.txt:8,10") != std::string::npos);

    edit(ed, in_body(ed, "a3\n") + 3, 0, "NEW\n");
    RunTypableCommand(ed, "w");
    RunTypableCommand(ed, "set-excerpt-context 1");
    EXPECT_TRUE(view(ed).find("anchor.txt:9,11") != std::string::npos);
    const std::size_t below = view(ed).find("anchor.txt:9,11");
    EXPECT_TRUE(view(ed).find("a9", below) != std::string::npos);
  }

  TEST_CASE("background jobs: several run side by side, each lands in its own view");
  {
    const fs::path a = scratch.Write("par_a.txt", twelve('g'));
    const fs::path b = scratch.Write("par_b.txt", twelve('h'));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    StartCommandJob(ed, "echo " + a.string() + ":3", false, false,
                    PendingCommand::Then::kOpen);
    StartCommandJob(ed, "echo " + b.string() + ":3", false, false,
                    PendingCommand::Then::kOpen);
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{2});
    PumpUntilIdle(ed);
    EXPECT_TRUE(ed.pending_commands.empty());
    EXPECT_TRUE(FindViewBuffer(ed, "from: echo " + a.string() + ":3") < BufferCount(ed));
    EXPECT_TRUE(FindViewBuffer(ed, "from: echo " + b.string() + ":3") < BufferCount(ed));
  }

  TEST_CASE("background jobs: cancel kills the child now, not at its leisure");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from-watched sleep 30 && echo " + home.string() + ":1");
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(CancelCommandJob(ed));
    EXPECT_TRUE(ed.pending_commands.empty());
    EXPECT_TRUE(ed.status.find("cancelled") != std::string::npos);
    EXPECT_TRUE((std::chrono::steady_clock::now() - t0) < std::chrono::seconds{2});
  }

  TEST_CASE("background jobs: a rebuild whose view was closed dies at the next pump");
  {
    const fs::path path = scratch.Write("orphan.txt", twelve('o'));
    const fs::path flag = scratch.dir / "orphan_flag";
    const std::string cmd =
        "[ -e " + flag.string() + " ] && sleep 30; echo " + path.string() + ":3";
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from-watched " + cmd);
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));

    scratch.Write("orphan_flag", "");
    edit(ed, in_body(ed, "o3"), 2, "o3x");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    const int child = ed.pending_commands[0].pid;
    RunTypableCommand(ed, "bc!");
    const auto t0 = std::chrono::steady_clock::now();
    PumpCommandJobs(ed);
    EXPECT_TRUE(ed.pending_commands.empty());
    EXPECT_TRUE((std::chrono::steady_clock::now() - t0) < std::chrono::seconds{2});
    EXPECT_TRUE((kill(child, 0) < 0) && (errno == ESRCH));
  }

  TEST_CASE(":wa re-asks the watched commands once, after the whole batch");
  {
    const fs::path fa = scratch.Write("batch_a.txt", twelve('a'));
    const fs::path fb = scratch.Write("batch_b.txt", twelve('b'));
    const fs::path log = scratch.dir / "batch_runs";
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from-watched echo run >> " + log.string() + " && echo " +
                              fa.string() + ":3");
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));

    EXPECT_TRUE(OpenTarget(ed, fa.string()));
    edit(ed, 0, 0, "x");
    EXPECT_TRUE(OpenTarget(ed, fb.string()));
    edit(ed, 0, 0, "y");
    RunTypableCommand(ed, "wa");
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    PumpUntilIdle(ed);
    const std::string runs = file_text(log);
    EXPECT_EQ(std::ranges::count(runs, '\n'), 2);
  }

  TEST_CASE(":wa reports what it wrote, and says so when there was nothing");
  {
    const fs::path fa = scratch.Write("count_a.txt", "one\n");
    const fs::path fb = scratch.Write("count_b.txt", "two\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, fa.string()));
    EXPECT_TRUE(OpenTarget(ed, fb.string()));

    RunTypableCommand(ed, "wa");
    EXPECT_TRUE(ed.status.find("nothing to write") != std::string::npos);

    edit(ed, 0, 0, "y");
    EXPECT_TRUE(OpenTarget(ed, fa.string()));
    edit(ed, 0, 0, "x");
    RunTypableCommand(ed, "wa");
    EXPECT_EQ(StatusAbout(ed, "wrote 2 buffers"), std::string{"wrote 2 buffers"});
  }

  TEST_CASE("a read-only file warns on the first edit, not at :w time");
  {
    const fs::path locked = scratch.Write("locked.txt", "hands off\n");
    std::filesystem::permissions(locked, std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace);
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, locked.string()));
    if (ed.doc.read_only) {
      ed.status.clear();
      TypeInto(ed, 'x');
      EXPECT_TRUE(ed.status.find("not writable") != std::string::npos);
      ed.status.clear();
      TypeInto(ed, 'y');
      EXPECT_TRUE(ed.status.empty());
    }
    std::filesystem::permissions(locked, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
  }

  TEST_CASE("background jobs: a save supersedes the run already in flight for its view");
  {
    const fs::path path = scratch.Write("gate.txt", twelve('q'));
    const fs::path flag = scratch.dir / "gate_flag";
    const std::string cmd =
        "[ -e " + flag.string() + " ] && sleep 30; echo " + path.string() + ":3";
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from-watched " + cmd);
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));

    scratch.Write("gate_flag", "");
    edit(ed, in_body(ed, "q3"), 2, "q3a");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    const int first_pid = ed.pending_commands[0].pid;
    edit(ed, in_body(ed, "q3a"), 3, "q3b");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    EXPECT_TRUE(ed.pending_commands[0].pid != first_pid);
    EXPECT_TRUE(CancelCommandJob(ed));
  }

  TEST_CASE("background jobs: presentation defers while input is mid-flight");
  {
    const fs::path path = scratch.Write("defer.txt", twelve('m'));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    StartCommandJob(ed, "echo " + path.string() + ":3", false, false,
                    PendingCommand::Then::kOpen);
    bool settled = false;
    for (int i = 0; (i < 20000) && !settled; ++i) {
      settled = !PumpCommandJobs(ed, true);
      usleep(500);
    }
    EXPECT_TRUE(settled);
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    EXPECT_TRUE(ed.pending_commands[0].done);
    EXPECT_EQ(ed.pending_commands[0].pid, -1);
    EXPECT_FALSE(IsExcerptView(ed.doc));
    PumpUntilIdle(ed);
    EXPECT_TRUE(ed.pending_commands.empty());
    EXPECT_TRUE(IsExcerptView(ed.doc));
  }

  TEST_CASE("background jobs: a refresh never restarts the run already in flight");
  {
    const fs::path path = scratch.Write("steady.txt", twelve('s'));
    const fs::path flag = scratch.dir / "steady_flag";
    const std::string cmd =
        "[ -e " + flag.string() + " ] && sleep 30; echo " + path.string() + ":3";
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from-watched " + cmd);
    PumpUntilIdle(ed);

    scratch.Write("steady_flag", "");
    edit(ed, in_body(ed, "s3"), 2, "s3x");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    const int running = ed.pending_commands[0].pid;

    scratch.Write("steady.txt", twelve('s'));
    MaybeRefreshExcerptView(ed);
    MaybeRefreshExcerptView(ed);
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    EXPECT_EQ(ed.pending_commands[0].pid, running);
    EXPECT_TRUE(CancelCommandJob(ed));
  }

  TEST_CASE("background jobs: re-asking over unsaved edits stashes the result, never drops it");
  {
    const fs::path path = scratch.Write("stash.txt", twelve('t'));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":3");
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));

    edit(ed, in_body(ed, "t3"), 2, "t3z");
    RunTypableCommand(ed, "from echo " + path.string() + ":3");
    PumpUntilIdle(ed);
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_TRUE(view(ed).find("t3z") != std::string::npos);
    EXPECT_TRUE(ed.doc.excerpts.rebuild_on_focus);
    EXPECT_TRUE(ed.status.find("results wait for :w") != std::string::npos);

    RunTypableCommand(ed, "w");
    MaybeRefreshExcerptView(ed);
    EXPECT_FALSE(ed.doc.excerpts.rebuild_on_focus);
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_TRUE(view(ed).find("t3z") != std::string::npos);
  }

  TEST_CASE(":from replay onto a shrunken file clamps its spans, never inverts them");
  {
    const fs::path path = scratch.Write("shrunk.txt", twelve('k'));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":10");
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("shrunk.txt:9,11") != std::string::npos);

    scratch.Write("shrunk.txt", "k1\nk2\nk3\n");
    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(view(ed).find("shrunk.txt:2,3") != std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks[0].first, Index{2});
    EXPECT_EQ(ed.doc.excerpts.blocks[0].last, Index{3});
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    edit(ed, in_body(ed, "k3"), 2, "k3x");
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_EQ(file_text(path), std::string{"k1\nk2\nk3x\n"});
  }

  TEST_CASE("background jobs: a byte-identical rebuild leaves no undo step behind");
  {
    const std::string body = twelve('i');
    const fs::path path = scratch.Write("same.txt", body);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":3");
    PumpUntilIdle(ed);
    EXPECT_FALSE(CanUndo(ed.doc.table));

    scratch.Write("same.txt", body);
    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(ed.doc.excerpt_epochs.boundaries.empty());
    EXPECT_FALSE(CanUndo(ed.doc.table));
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_FALSE(ed.doc.excerpts.refs_stale);

    ed.status.clear();
    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(ed.status.empty());
  }

  TEST_CASE("background jobs: results for an unfocused view wait for its next focus");
  {
    const fs::path path = scratch.Write("quiet.txt", twelve('u'));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from-watched echo " + path.string() + ":3");
    PumpUntilIdle(ed);
    const std::size_t view_at = FindViewBuffer(ed, ed.doc.view_name);
    edit(ed, in_body(ed, "u3\n") + 3, 0, "NEW\n");
    RunTypableCommand(ed, "w");

    SwitchToBuffer(ed, FindFileBuffer(ed, home));
    PumpUntilIdle(ed);
    EXPECT_EQ(ed.doc.file, home);
    EXPECT_TRUE(BufferAt(ed, view_at).excerpts.rebuild_on_focus);

    SwitchToBuffer(ed, view_at);
    MaybeRefreshExcerptView(ed);
    EXPECT_FALSE(ed.doc.excerpts.rebuild_on_focus);
    EXPECT_TRUE(view(ed).find("quiet.txt:2,4") != std::string::npos);
  }

  TEST_CASE(":from -- a failing command with no file:line shows its own output");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo ld: undefined reference to koi_main >&2; exit 3");
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_TRUE(ed.doc.excerpts.kind == ExcerptView::Kind::kCommand);
    EXPECT_TRUE(view(ed).find("-- exit 3") != std::string::npos);
    EXPECT_TRUE(view(ed).find("undefined reference to koi_main") != std::string::npos);
    EXPECT_TRUE(ed.status.find("(exit 3)") != std::string::npos);
    EXPECT_TRUE(ed.status.find("output is in the view") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    RunTypableCommand(ed, "w");
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE(":from -- a clean empty result stays a status line, not a view");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    const std::size_t buffers = BufferCount(ed);
    RunTypableCommand(ed, "from true");
    PumpUntilIdle(ed);
    EXPECT_EQ(BufferCount(ed), buffers);
    EXPECT_TRUE(ed.status.find("no file:line in the output") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE(":from -- a failure that still produced positions keeps the excerpts");
  {
    const fs::path path = scratch.Write("mix.txt", twelve('m'));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":3; echo make: Error 1 >&2; exit 2");
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("mix.txt:2,4") != std::string::npos);
    EXPECT_TRUE(view(ed).find("-- exit 2") == std::string::npos);
    EXPECT_TRUE(ed.status.find("(exit 2)") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE(":from -- long failure output keeps the tail and says it trimmed");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from i=0; while [ $i -lt 400 ]; do i=$((i+1)); echo $i; done; "
                          "echo boom >&2; exit 1");
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("(earlier output dropped") != std::string::npos);
    EXPECT_TRUE(view(ed).find("\n400\n") != std::string::npos);
    EXPECT_TRUE(view(ed).find("boom") != std::string::npos);
    EXPECT_TRUE(view(ed).find("\n5\n") == std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE(":from-watched -- failure output swaps in, the next clean run swaps back");
  {
    const fs::path target = scratch.Write("watched.txt", twelve('w'));
    const fs::path script = scratch.Write("cmd.sh", "echo " + target.string() + ":3\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from-watched sh " + script.string());
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("watched.txt:2,4") != std::string::npos);
    EXPECT_TRUE(ed.doc.excerpts.watched);

    scratch.Write("cmd.sh", "echo ld: undefined reference to koi_main >&2\nexit 2\n");
    EXPECT_EQ(RerunWatchedViews(ed), 1);
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("undefined reference to koi_main") != std::string::npos);
    EXPECT_TRUE(view(ed).find("-- exit 2") != std::string::npos);
    EXPECT_TRUE(ed.doc.excerpts.watched);
    EXPECT_TRUE(ed.doc.excerpts.kind == ExcerptView::Kind::kCommand);
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    const std::size_t epochs = ed.doc.excerpt_epochs.boundaries.size();
    EXPECT_EQ(RerunWatchedViews(ed), 1);
    PumpUntilIdle(ed);
    EXPECT_EQ(ed.doc.excerpt_epochs.boundaries.size(), epochs);

    scratch.Write("cmd.sh", "echo " + target.string() + ":3\n");
    EXPECT_EQ(RerunWatchedViews(ed), 1);
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("watched.txt:2,4") != std::string::npos);
    EXPECT_TRUE(view(ed).find("undefined reference") == std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("undefined reference to koi_main") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE(":from-watched -- a failing run never overwrites unsaved edits");
  {
    const fs::path target = scratch.Write("held.txt", twelve('h'));
    const fs::path script = scratch.Write("held.sh", "echo " + target.string() + ":3\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from-watched sh " + script.string());
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("held.txt:2,4") != std::string::npos);
    edit(ed, in_body(ed, "h3\n") + 3, 0, "KEEP\n");

    scratch.Write("held.sh", "echo cannot even >&2\nexit 9\n");
    EXPECT_EQ(RerunWatchedViews(ed), 1);
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("KEEP") != std::string::npos);
    EXPECT_TRUE(view(ed).find("cannot even") == std::string::npos);
    EXPECT_TRUE(ed.status.find("unsaved edits") != std::string::npos);
    EXPECT_TRUE(ed.status.find("(exit 9)") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void ExcerptViewSources() {
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-excerpt-sources"};
  const fs::path home = scratch.Write("home.txt", "somewhere to stand\n");
  const auto view = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  const auto body = [](const std::string& text) {
    std::string out;
    for (std::size_t at = 0; at <= text.size();) {
      const std::size_t eol = std::min(text.find('\n', at), text.size());
      const std::string_view line{text.data() + at, eol - at};
      if (line.find(".txt:") == std::string_view::npos) {
        out.append(line);
        out += '\n';
      }
      if (eol == text.size()) break;
      at = eol + 1;
    }
    return out;
  };

  TEST_CASE(":from -- a command's file:line output becomes excerpts");
  {
    const fs::path hits = scratch.Write("hits.txt", "h1\nh2\nh3\nh4\nh5\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + hits.string() + ":3 && echo noise without a file");
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_TRUE(view(ed).starts_with("1 hit from"));
    EXPECT_TRUE(view(ed).find("h2\nh3\nh4\n") != std::string::npos);
    EXPECT_TRUE(view(ed).find(":2,4") != std::string::npos);

    TEST_CASE(":from -- landing back on the view after the files moved refreshes it");
    scratch.Write("hits.txt", "h1\nH2!\nh3\nh4\nh5\n");
    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(view(ed).find("H2!\nh3\nh4\n") != std::string::npos);
    EXPECT_FALSE(ed.doc.modified);

    TEST_CASE(":from -- unsaved edits are warned about, never clobbered");
    std::ignore = Insert("KEEP ", LineStart(ed.doc.table, 2), ed.doc.table);
    ed.doc.modified = true;
    scratch.Write("hits.txt", "h1\nh2?\nh3\nh4\nh5\n");
    ed.status.clear();
    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(ed.status.find("files changed under the view") != std::string::npos);
    EXPECT_TRUE(view(ed).find("KEEP ") != std::string::npos);
    ed.status.clear();
    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(ed.status.empty());

    TEST_CASE(":from -- nothing position-shaped is a warning, not a view");
    Editor bare;
    bare.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(bare, home.string()));
    RunTypableCommand(bare, "from echo just words");
    PumpUntilIdle(bare);
    EXPECT_FALSE(IsExcerptView(bare.doc));
    EXPECT_TRUE(bare.status.find("no file:line") != std::string::npos);
  }

  TEST_CASE(":pins-excerpt -- the project's pins as a view");
  {
    const fs::path fa = scratch.Write("pa.txt", "p1\np2\np3\np4\np5\n");
    const fs::path fb = scratch.Write("pb.txt", "q1\nq2\nq3\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    std::string db_error;
    ed.project = ProjectStore::Open(scratch.dir / "state.db", db_error);
    EXPECT_TRUE(ed.project != nullptr);
    // A pin is a file, and the line it excerpts around is where that file was
    // last left -- so the visit is what puts the excerpt on p3 and q2, not the
    // pin.
    ed.project->RecordVisit(fa.string(), 3, 1);
    ed.project->RecordVisit(fb.string(), 2, 1);
    ed.project->SetPin(1, fa.string());
    ed.project->SetPin(2, fb.string());

    RunTypableCommand(ed, "pins-excerpt");
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_TRUE(view(ed).starts_with("2 pins"));
    EXPECT_TRUE(view(ed).find("p2\np3\np4\n") != std::string::npos);
    EXPECT_TRUE(view(ed).find("q1\nq2\nq3\n") != std::string::npos);

    Editor homeless;
    homeless.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(homeless, home.string()));
    ed.status.clear();
    RunTypableCommand(homeless, "pins-excerpt");
    EXPECT_TRUE(homeless.status.find("no project database") != std::string::npos);
  }

  TEST_CASE("pins view: every header names the slot its pin sits in");
  {
    const fs::path fa = scratch.Write("slot_a.txt", "sa1\nsa2\nsa3\nsa4\n");
    const fs::path fb = scratch.Write("slot_b.txt", "sb1\nsb2\nsb3\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    std::string db_error;
    ed.project = ProjectStore::Open(scratch.dir / "slots.db", db_error);
    EXPECT_TRUE(ed.project != nullptr);
    ed.project->RecordVisit(fa.string(), 2, 1);
    ed.project->RecordVisit(fb.string(), 3, 1);
    ed.project->SetPin(1, fa.string());
    ed.project->SetPin(3, fb.string());

    RunTypableCommand(ed, "pins-excerpt");
    EXPECT_TRUE(IsExcerptView(ed.doc));
    const std::string text = view(ed);
    EXPECT_TRUE(text.find("slot_a.txt:2,2  pin 1") != std::string::npos);
    EXPECT_TRUE(text.find("slot_b.txt:3,3  pin 3") != std::string::npos);
    EXPECT_TRUE(ed.doc.excerpts.blocks[0].header.find("pin 1") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    RunTypableCommand(ed, "clear-pin 1");
    RefreshLiveExcerptViews(ed);
    EXPECT_TRUE(view(ed).find("pin 1") == std::string::npos);
    EXPECT_TRUE(view(ed).find("slot_b.txt:3,3  pin 3") != std::string::npos);
  }

  TEST_CASE("pins view: it follows the pins, in front and behind");
  {
    const fs::path fa = scratch.Write("live_a.txt", "la1\nla2\nla3\nla4\n");
    const fs::path fb = scratch.Write("live_b.txt", "lb1\nlb2\nlb3\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    std::string db_error;
    ed.project = ProjectStore::Open(scratch.dir / "live.db", db_error);
    EXPECT_TRUE(ed.project != nullptr);

    EXPECT_TRUE(OpenTarget(ed, fa.string()));
    RunTypableCommand(ed, "pin 1");
    EXPECT_EQ(ed.project->Pins()[0].path, fa.string());
    RunTypableCommand(ed, "pins-excerpt");
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_TRUE(body(view(ed)).find("la1") != std::string::npos);
    EXPECT_TRUE(body(view(ed)).find("lb") == std::string::npos);
    const std::size_t pins_at = ed.active;

    EXPECT_TRUE(OpenTarget(ed, fb.string()));
    RunCommands(ed, {"move_line_down"});
    RunTypableCommand(ed, "pin 2");
    EXPECT_EQ(ed.project->Pins()[1].path, fb.string());
    EXPECT_TRUE(BufferAt(ed, pins_at).excerpts.rebuild_on_focus);
    EXPECT_TRUE(ReadDocRange(BufferAt(ed, pins_at).table,
                             {0, DocLength(BufferAt(ed, pins_at).table)})
                    .find("lb2") == std::string::npos);

    SwitchToBuffer(ed, pins_at);
    MaybeRefreshExcerptView(ed);
    EXPECT_FALSE(ed.doc.excerpts.rebuild_on_focus);
    EXPECT_TRUE(body(view(ed)).find("la1") != std::string::npos);
    EXPECT_TRUE(body(view(ed)).find("lb2") != std::string::npos);
    EXPECT_TRUE(view(ed).find("2 pins") != std::string::npos);

    RunTypableCommand(ed, "clear-pin 1");
    RefreshLiveExcerptViews(ed);
    EXPECT_TRUE(ed.project->Pins()[0].path.empty());
    EXPECT_TRUE(body(view(ed)).find("la1") == std::string::npos);
    EXPECT_TRUE(body(view(ed)).find("lb2") != std::string::npos);
    EXPECT_TRUE(view(ed).find("1 pin\n") != std::string::npos);
    EXPECT_FALSE(ed.doc.modified);

    const Index settled = ed.doc.table.revision;
    RefreshLiveExcerptViews(ed);
    RefreshLiveExcerptViews(ed);
    EXPECT_EQ(ed.doc.table.revision, settled);

    RunTypableCommand(ed, "clear-pin 2");
    RefreshLiveExcerptViews(ed);
    EXPECT_TRUE(body(view(ed)).find("lb2") == std::string::npos);
    EXPECT_TRUE(view(ed).find("0 pins") != std::string::npos);
    EXPECT_TRUE(IsExcerptView(ed.doc));

    EXPECT_TRUE(OpenTarget(ed, fa.string()));
    RunCommands(ed, {"move_line_down", "move_line_down"});
    RunTypableCommand(ed, "pin 1");
    SwitchToBuffer(ed, FindViewBuffer(ed, "pins"));
    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(body(view(ed)).find("la3") != std::string::npos);
    EXPECT_TRUE(view(ed).find("1 pin\n") != std::string::npos);
  }

  TEST_CASE("pins view: it follows where you are, with nothing re-pinned");
  {
    const fs::path fa = scratch.Write("walk_a.txt", NumberedLines(20));
    const fs::path fb = scratch.Write("walk_b.txt", NumberedLines(20));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    std::string db_error;
    ed.project = ProjectStore::Open(scratch.dir / "walk.db", db_error);
    EXPECT_TRUE(ed.project != nullptr);

    EXPECT_TRUE(OpenTarget(ed, fa.string() + ":3"));
    RunTypableCommand(ed, "pin 1");
    RunTypableCommand(ed, "pins-excerpt");
    const std::size_t pins_at = ed.active;
    EXPECT_TRUE(body(view(ed)).find("line-3\n") != std::string::npos);

    // Move well away in the pinned file, and pin nothing. Leaving it is what
    // the store hears about; the view is stale from that moment.
    SwitchToBuffer(ed, FindFileBuffer(ed, fa));
    RunCommands(ed, {"move_line_down", "move_line_down", "move_line_down", "move_line_down"});
    EXPECT_TRUE(OpenTarget(ed, fb.string()));
    SwitchToBuffer(ed, pins_at);
    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(body(view(ed)).find("line-7\n") != std::string::npos);
    EXPECT_TRUE(body(view(ed)).find("line-3\n") == std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    // And with no visit in between: focusing the view asks again rather than
    // waiting to be told.
    SwitchToBuffer(ed, FindFileBuffer(ed, fa));
    RunCommands(ed, {"move_line_down", "move_line_down"});
    SwitchToBuffer(ed, pins_at);
    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(body(view(ed)).find("line-9\n") != std::string::npos);
    EXPECT_TRUE(view(ed).find("1 pin\n") != std::string::npos);
  }

  TEST_CASE("view names never answer for files: a file called `pins` survives");
  {
    const fs::path previous = fs::current_path();
    fs::current_path(scratch.dir);
    const fs::path pins_file = scratch.Write("pins", "p1\np2\np3\n");
    const auto edit = [](Editor& ed, std::size_t at, std::size_t take, std::string_view put) {
      if (take > 0) {
        std::ignore = Delete(static_cast<Index>(at), static_cast<Index>(at + take), ed.doc.table);
      }
      if (!put.empty()) std::ignore = Insert(put, static_cast<Index>(at), ed.doc.table);
      ed.doc.modified = true;
    };
    const auto file_text = [](const fs::path& p) {
      std::ifstream in{p, std::ios::binary};
      return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    };

    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    std::string db_error;
    ed.project = ProjectStore::Open(scratch.dir / "collide.db", db_error);
    EXPECT_TRUE(ed.project != nullptr);

    EXPECT_TRUE(OpenTarget(ed, "pins:2"));
    const std::size_t file_at = ed.active;
    ed.project->SetPin(1, pins_file.string());

    RunTypableCommand(ed, "pins-excerpt");
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_TRUE(ed.active != file_at);
    EXPECT_EQ(FindFileBuffer(ed, fs::path{"pins"}), file_at);
    EXPECT_FALSE(IsExcerptView(BufferAt(ed, file_at)));
    EXPECT_EQ(ReadDocRange(BufferAt(ed, file_at).table,
                           {0, DocLength(BufferAt(ed, file_at).table)}),
              std::string{"p1\np2\np3\n"});

    EXPECT_TRUE(OpenTarget(ed, "pins"));
    EXPECT_FALSE(IsExcerptView(ed.doc));
    EXPECT_EQ(ed.active, file_at);

    SwitchToBuffer(ed, FindViewBuffer(ed, "pins"));
    EXPECT_TRUE(IsExcerptView(ed.doc));
    const std::string text = view(ed);
    edit(ed, text.find("p2"), 2, "P2");
    RunTypableCommand(ed, "w");
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_EQ(file_text(pins_file), std::string{"p1\nP2\np3\n"});
    EXPECT_EQ(ReadDocRange(BufferAt(ed, file_at).table,
                           {0, DocLength(BufferAt(ed, file_at).table)}),
              std::string{"p1\nP2\np3\n"});

    SwitchToBuffer(ed, file_at);
    edit(ed, 0, 0, "x");
    RunTypableCommand(ed, "pins-excerpt");
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_TRUE(BufferAt(ed, file_at).modified);
    EXPECT_TRUE(ReadDocRange(BufferAt(ed, file_at).table,
                             {0, DocLength(BufferAt(ed, file_at).table)})
                    .starts_with("xp1"));

    fs::current_path(previous);
  }

  TEST_CASE("pins view: the pinned line is lit the way a reference is");
  {
    const fs::path f = scratch.Write("lit.txt", "aaa\nbbb\nTARGET\nccc\nddd\n");
    const auto lit = [](const Surface& frame, std::string_view needle, int from_row) -> Attr {
      for (int y = from_row; y < frame.height; ++y) {
        const std::string row = frame.Row(y);
        const std::size_t at = row.find(needle);
        if (at == std::string::npos) continue;
        return frame.At(static_cast<int>(at), y).fg;
      }
      return 0;
    };

    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    std::string db_error;
    ed.project = ProjectStore::Open(scratch.dir / "lit.db", db_error);
    EXPECT_TRUE(ed.project != nullptr);
    EXPECT_TRUE(OpenTarget(ed, f.string() + ":3"));
    ed.project->SetPin(1, f.string());
    RunTypableCommand(ed, "pins-excerpt");
    EXPECT_TRUE(std::ranges::find(ed.doc.excerpts.anchor_index, std::string{"TARGET"}) !=
                ed.doc.excerpts.anchor_index.end());
    Surface pinned_frame;
    FitFocusedViewport(ed, 60, 12);
    RenderTo(ed, pinned_frame, 60, 12);
    const Attr pinned = lit(pinned_frame, "TARGET", 2);
    const Attr context_line = lit(pinned_frame, "bbb", 2);
    EXPECT_TRUE(pinned != context_line);

    Editor refs;
    refs.theme = BuiltinTheme();
    refs.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(refs, f.string()));
    OpenReferenceExcerpts(refs, {Symbol{f.string(), 3, 1, "TARGET"}}, "TARGET");
    Surface ref_frame;
    FitFocusedViewport(refs, 60, 12);
    RenderTo(refs, ref_frame, 60, 12);
    EXPECT_EQ(pinned, lit(ref_frame, "TARGET", 2));
  }

  TEST_CASE("live views: a pane showing one follows along without being focused");
  {
    const fs::path fa = scratch.Write("see_a.txt", "sa1\nsa2\nsa3\n");
    const fs::path fb = scratch.Write("see_b.txt", "sb1\nsb2\nsb3\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    std::string db_error;
    ed.project = ProjectStore::Open(scratch.dir / "see.db", db_error);
    EXPECT_TRUE(ed.project != nullptr);

    EXPECT_TRUE(OpenTarget(ed, fa.string()));
    RunTypableCommand(ed, "pin 1");
    RunTypableCommand(ed, "pins-excerpt");
    const std::size_t pins_at = ed.active;

    SplitWindow(ed, true);
    EXPECT_TRUE(OpenTarget(ed, fb.string()));
    EXPECT_TRUE(ed.active != pins_at);
    const auto pane_text = [&ed, pins_at] {
      return ReadDocRange(BufferAt(ed, pins_at).table,
                          {0, DocLength(BufferAt(ed, pins_at).table)});
    };
    EXPECT_TRUE(pane_text().find("sb2") == std::string::npos);

    RunCommands(ed, {"move_line_down"});
    ed.status.clear();
    RunTypableCommand(ed, "pin 2");
    RefreshLiveExcerptViews(ed);
    EXPECT_TRUE(pane_text().find("sb2") != std::string::npos);
    EXPECT_TRUE(pane_text().find("2 pins") != std::string::npos);
    EXPECT_TRUE(ed.active != pins_at);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"see_b.txt"});
    EXPECT_TRUE(ed.status.find("pinned ") != std::string::npos);
    EXPECT_TRUE(ed.status.find(" to 2") != std::string::npos);
    EXPECT_FALSE(BufferAt(ed, pins_at).excerpts.rebuild_on_focus);
    EXPECT_FALSE(BufferAt(ed, pins_at).modified);
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    const Index settled = BufferAt(ed, pins_at).table.revision;
    RunTypableCommand(ed, "pin 2");
    RefreshLiveExcerptViews(ed);
    EXPECT_EQ(BufferAt(ed, pins_at).table.revision, settled);

    SwitchToBuffer(ed, pins_at);
    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("sb2") == std::string::npos);
  }

  TEST_CASE("live views: the same view in two panes, and the one that shrank is not blank");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    std::string db_error;
    ed.project = ProjectStore::Open(scratch.dir / "twopane.db", db_error);
    EXPECT_TRUE(ed.project != nullptr);
    std::vector<fs::path> files;
    for (int i = 0; i < 4; ++i) {
      files.push_back(scratch.Write("tp" + std::to_string(i) + ".txt",
                                    "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\n"));
    }
    EXPECT_TRUE(OpenTarget(ed, files[0].string() + ":4"));
    for (int i = 0; i < 4; ++i) {
      const std::string path = files[static_cast<std::size_t>(i)].string();
      ed.project->RecordVisit(path, 4, 1);
      ed.project->SetPin(i + 1, path);
    }
    RunTypableCommand(ed, "pins-excerpt");
    const std::size_t pins_at = ed.active;
    EXPECT_TRUE(LineCount(ed.doc.table) > 12);

    ed.doc.view.top_line = 12;
    SplitWindow(ed, true);
    int parked = 0;
    for (const WindowNode& node : ed.windows) {
      if (node.dead || (node.kind != WindowNode::Kind::kLeaf)) continue;
      if ((node.buffer == pins_at) && (node.view.top_line == 12)) ++parked;
    }
    EXPECT_EQ(parked, 2);

    for (int slot = 1; slot <= 4; ++slot) RunTypableCommand(ed, "clear-pin " + std::to_string(slot));
    RefreshLiveExcerptViews(ed);
    const Index lines = LineCount(ed.doc.table);
    EXPECT_TRUE(lines <= 2);
    for (const WindowNode& node : ed.windows) {
      if (node.dead || (node.kind != WindowNode::Kind::kLeaf)) continue;
      if (node.buffer != pins_at) continue;
      EXPECT_TRUE(node.view.top_line < lines);
    }
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("live views: refreshing on every change stays bounded");
  {
    const fs::path fa = scratch.Write("bd_a.txt", "ba1\nba2\nba3\n");
    const fs::path fb = scratch.Write("bd_b.txt", "bb1\nbb2\nbb3\n");
    const fs::path fc = scratch.Write("bd_c.txt", "bc1\nbc2\nbc3\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    std::string db_error;
    ed.project = ProjectStore::Open(scratch.dir / "bound.db", db_error);
    EXPECT_TRUE(ed.project != nullptr);
    EXPECT_TRUE(OpenTarget(ed, fa.string()));
    RunTypableCommand(ed, "pin 1");
    RunTypableCommand(ed, "pins-excerpt");
    const std::size_t pins_at = ed.active;
    SplitWindow(ed, true);
    EXPECT_TRUE(OpenTarget(ed, fc.string()));

    int rebuilt = 0;
    Index last = BufferAt(ed, pins_at).table.revision;
    for (int i = 0; i < 120; ++i) {
      const fs::path& next = ((i % 3) == 0) ? fa : (((i % 3) == 1) ? fb : fc);
      std::ignore = OpenTarget(ed, next.string());
      // Slot 2 is a different file every turn, so every turn the view is stale.
      RunTypableCommand(ed, "pin 2");
      RefreshLiveExcerptViews(ed);
      const Index now = BufferAt(ed, pins_at).table.revision;
      if (now != last) ++rebuilt;
      last = now;
    }
    EXPECT_TRUE(rebuilt > 10);

    const Document& held = BufferAt(ed, pins_at);
    EXPECT_TRUE(HistoryBytes(held.table) <= held.table.history_budget_bytes);
    EXPECT_TRUE(held.excerpt_epochs.boundaries.size() <= 8);
    EXPECT_TRUE(held.excerpt_epochs.store.size() <= 9);
    EXPECT_TRUE(held.excerpt_epochs.active < held.excerpt_epochs.store.size());
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("live views: one nobody can see waits, and costs nothing while it waits");
  {
    const fs::path fa = scratch.Write("hid_a.txt", "hd1\nhd2\nhd3\n");
    const fs::path fb = scratch.Write("hid_b.txt", "he1\nhe2\nhe3\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    std::string db_error;
    ed.project = ProjectStore::Open(scratch.dir / "hid.db", db_error);
    EXPECT_TRUE(ed.project != nullptr);
    EXPECT_TRUE(OpenTarget(ed, fa.string()));
    RunTypableCommand(ed, "pin 1");
    RunTypableCommand(ed, "pins-excerpt");
    const std::size_t pins_at = ed.active;

    EXPECT_TRUE(OpenTarget(ed, fb.string()));
    RunTypableCommand(ed, "pin 2");
    const Index settled = BufferAt(ed, pins_at).table.revision;
    RefreshLiveExcerptViews(ed);
    RefreshLiveExcerptViews(ed);
    EXPECT_EQ(BufferAt(ed, pins_at).table.revision, settled);
    EXPECT_TRUE(BufferAt(ed, pins_at).excerpts.rebuild_on_focus);

    SwitchToBuffer(ed, pins_at);
    MaybeRefreshExcerptView(ed);
    EXPECT_TRUE(body(view(ed)).find("he") != std::string::npos);
    EXPECT_FALSE(ed.doc.excerpts.rebuild_on_focus);
  }

  TEST_CASE("pins view: unsaved edits in it are never rebuilt away");
  {
    const fs::path fa = scratch.Write("hold_a.txt", "ha1\nha2\nha3\n");
    const fs::path fb = scratch.Write("hold_b.txt", "hb1\nhb2\nhb3\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    std::string db_error;
    ed.project = ProjectStore::Open(scratch.dir / "hold.db", db_error);
    EXPECT_TRUE(ed.project != nullptr);
    EXPECT_TRUE(OpenTarget(ed, fa.string()));
    RunTypableCommand(ed, "pin 1");
    RunTypableCommand(ed, "pins-excerpt");

    const std::size_t at = view(ed).find("ha1");
    EXPECT_TRUE(at != std::string::npos);
    std::ignore = Insert("EDITED\n", static_cast<Index>(at), ed.doc.table);
    ed.doc.modified = true;
    const std::string held = view(ed);

    RunTypableCommand(ed, "clear-pin 1");
    EXPECT_EQ(view(ed), held);
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_TRUE(ed.doc.excerpts.rebuild_on_focus);
    MaybeRefreshExcerptView(ed);
    EXPECT_EQ(view(ed), held);

    ed.status.clear();
    RunTypableCommand(ed, "pin 3");
    EXPECT_TRUE(ed.status.find("not a file") != std::string::npos);
    EXPECT_EQ(view(ed), held);
  }

  TEST_CASE("show_definition_excerpts -- an empty selection is a warning");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    ed.doc.selections.Set(Selection{0, 0, -1});
    RunCommands(ed, {"show_definition_excerpts"});
    EXPECT_TRUE(ed.status.find("nothing selected") != std::string::npos);
    EXPECT_FALSE(IsExcerptView(ed.doc));
  }

  TEST_CASE("excerpt_drop -- every cursor prunes its hunk, files untouched");
  {
    const fs::path fa = scratch.Write("da.txt", "d1\nd2\nd3\nd4\nd5\nd6\nd7\nd8\nd9\nd10\nd11\nd12\n");
    const fs::path fb = scratch.Write("db.txt", "e1\ne2\ne3\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed,
                          {Symbol{fa.string(), 3, 1, "d3"}, Symbol{fa.string(), 10, 1, "d3"},
                           Symbol{fb.string(), 2, 1, "d3"}},
                          "d3");
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{3});
    const std::string before_a = view(ed);
    EXPECT_TRUE(before_a.starts_with("3 references to d3\n"));

    const Index body1 = static_cast<Index>(before_a.find("d2\n"));
    const Index head3 = static_cast<Index>(before_a.find("db.txt:1,3"));
    std::vector<Selection> cursors;
    cursors.push_back(Selection{body1, body1, -1});
    cursors.push_back(Selection{head3, head3, -1});
    ed.doc.selections.Replace(ed.doc.table, std::move(cursors));
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);

    RunCommands(ed, {"excerpt_drop"});
    EXPECT_TRUE(ed.status.find("dropped 2 excerpts") != std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{1});
    EXPECT_TRUE(view(ed).find("da.txt:2,4") == std::string::npos);
    EXPECT_TRUE(view(ed).find("db.txt:1,3") == std::string::npos);
    EXPECT_TRUE(view(ed).find("da.txt:9,11") != std::string::npos);
    EXPECT_TRUE(view(ed).starts_with("1 reference to d3\n"));
    RunCommands(ed, {"undo"});
    EXPECT_EQ(view(ed), before_a);
    RunCommands(ed, {"redo"});

    ed.doc.modified = false;
    RunTypableCommand(ed, "set-excerpt-context 2");
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{1});
    EXPECT_TRUE(view(ed).find("db.txt") == std::string::npos);
    EXPECT_TRUE(view(ed).starts_with("1 reference to d3\n"));
  }
}

void ExcerptViewFuzz(Rng& rng) {
  TEST_CASE("adversarial: an excerpt view survives arbitrary abuse");
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-excerpt-fuzz"};
  const fs::path home = scratch.Write("home.txt", "somewhere to stand\n");

  std::string body_a;
  for (int i = 1; i <= 14; ++i) body_a += "a" + std::to_string(i) + "\n";
  std::string body_b = "b1\nb2\nb3\nb4\nb5\nb6";
  const fs::path fa = scratch.Write("fuzz_a.txt", body_a);
  const fs::path fb = scratch.Write("fuzz_b.txt", body_b);

  Editor ed;
  ed.theme = BuiltinTheme();
  ed.settings.excerpt_context = 1;
  EXPECT_TRUE(OpenTarget(ed, home.string()));
  const std::vector<Symbol> refs{
      Symbol{fa.string(), 3, 1, "a3"},
      Symbol{fa.string(), 10, 1, "a3"},
      Symbol{fb.string(), 5, 1, "a3"},
  };
  OpenReferenceExcerpts(ed, refs, "a3");
  const std::string view_name = ed.doc.file.string();

  const auto back_to_view = [&] {
    const std::size_t at = FindViewBuffer(ed, view_name);
    if (at < BufferCount(ed)) SwitchToBuffer(ed, at);
  };
  const auto doc_len = [&] { return DocLength(ed.doc.table); };
  const char* garbage[] = {"x",  "xyz\n", "\n\n",  "a3",   " ",
                           "\t", "▸ ",     "b4\nb5", ":9,2", "a3 a3 a3\n"};

  const std::string job_cmds[] = {
      "echo " + fa.string() + ":3",
      "echo " + fa.string() + ":2 && echo " + fb.string() + ":4",
      "echo nonsense without a file",
      "false",
  };

  for (int step = 0; step < 160; ++step) {
    const Index op = rng.Pick(0, 15);
    switch (op) {
      case 0:
      case 1: {
        const Index at = rng.Pick(0, doc_len());
        const std::string_view text = garbage[rng.Pick(0, std::ssize(garbage) - 1)];
        std::ignore = Insert(text, SnapToGraphemeBoundary(ed.doc.table, at), ed.doc.table);
        ed.doc.modified = true;
        break;
      }
      case 2:
      case 3: {
        if (doc_len() == 0) break;
        const Index from = SnapToGraphemeBoundary(ed.doc.table, rng.Pick(0, doc_len() - 1));
        const Index to = SnapToGraphemeBoundary(
            ed.doc.table, std::min(doc_len(), from + rng.Pick(1, 40)));
        if (to > from) std::ignore = Delete(from, to, ed.doc.table);
        ed.doc.modified = true;
        break;
      }
      case 4: {
        const std::string text = ReadDocRange(ed.doc.table, {0, doc_len()});
        if (text.empty()) break;
        const std::size_t bol = static_cast<std::size_t>(rng.Pick(0, std::ssize(text) - 1));
        const std::size_t from = text.rfind('\n', bol) + 1;
        std::size_t to = text.find('\n', bol);
        if (to == std::string::npos) to = text.size();
        std::string line = text.substr(from, to - from) + "\n";
        const Index at = SnapToGraphemeBoundary(ed.doc.table, rng.Pick(0, doc_len()));
        std::ignore = Insert(line, at, ed.doc.table);
        ed.doc.modified = true;
        break;
      }
      case 5: {
        if (doc_len() > 0) std::ignore = Delete(Index{0}, doc_len(), ed.doc.table);
        ed.doc.modified = true;
        break;
      }
      case 6:
        RunCommands(ed, {"undo"});
        break;
      case 7:
        RunCommands(ed, {"redo"});
        break;
      case 8:
        RunTypableCommand(ed, "w");
        break;
      case 9:
        RunTypableCommand(ed, "set-excerpt-context " + std::to_string(rng.Pick(0, 5)));
        break;
      case 10: {
        const Index at = SnapToGraphemeBoundary(ed.doc.table, rng.Pick(0, std::max<Index>(0, doc_len() - 1)));
        ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
        RunCommands(ed, {"goto_excerpt_source"});
        back_to_view();
        break;
      }
      case 11: {
        const Index fate = rng.Pick(0, 2);
        scratch.Write("fuzz_a.txt",
                      (fate == 0) ? body_a : (fate == 1) ? (body_a + "tail\n") : "a1\n");
        break;
      }
      case 12:
        RunCommands(ed, {"excerpt_drop"});
        break;
      case 13:
        MaybeRefreshExcerptView(ed);
        break;
      case 14:
        StartCommandJob(ed, job_cmds[rng.Pick(0, std::ssize(job_cmds) - 1)],
                        rng.Pick(0, 1) == 1, rng.Pick(0, 1) == 1,
                        (rng.Pick(0, 1) == 1) ? PendingCommand::Then::kOpen
                                              : PendingCommand::Then::kRebuild);
        break;
      case 15:
        if (rng.Pick(0, 3) == 0) {
          CancelCommandJob(ed);
        } else {
          PumpCommandJobs(ed, rng.Pick(0, 1) == 1);
        }
        break;
    }

    ed.doc.selections.Normalize(ed.doc.table);
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    if (!IsExcerptView(ed.doc)) back_to_view();
    if ((step % 8) == 0) {
      Surface frame;
      FitFocusedViewport(ed, 80, 24);
      RenderTo(ed, frame, 80, 24);
    }
    const std::string broken = EditorInvariants(ed);
    if (!broken.empty()) {
      ++common::g_test_failures;
      std::cerr << "FAIL [excerpt fuzz] step " << step << " op " << op << ": " << broken
                << " (seed " << rng.seed << ")" << std::endl;
      return;
    }
    ++common::g_test_checks;
  }

  RunTypableCommand(ed, "w");
  RunCommands(ed, {"undo"});
  RunTypableCommand(ed, "set-excerpt-context 2");
  KillAllCommandJobs(ed);
  EXPECT_EQ(EditorInvariants(ed), std::string{});
}

void SearchExcerptView() {
  const Scratch scratch{"koi-search"};
  scratch.Write("one.txt", "alpha widget\nbeta\ngamma widget tail\nSKIP widget\n");
  scratch.Write("two.txt", "widget alone\n");
  scratch.Write("three.txt", "nothing here\n");

  const auto fresh = [&scratch] {
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    ed.settings.file_filter = "find " + scratch.dir.string() + " -type f -printf '%p\\n'";
    return ed;
  };
  const auto view = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };

  TEST_CASE("search: a bare pattern searches every filtered file, as excerpts");
  {
    Editor ed = fresh();
    SearchExcerpts(ed, "widget");
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_EQ(EditorInvariants(ed), std::string{});
    EXPECT_EQ(ed.doc.excerpts.refs.size(), std::size_t{4});
    const std::string text = view(ed);
    EXPECT_TRUE(text.starts_with("4 matches for widget\n"));
    EXPECT_TRUE(text.find("alpha widget\n") != std::string::npos);
    EXPECT_TRUE(text.find("gamma widget tail\n") != std::string::npos);
    EXPECT_TRUE(text.find("widget alone\n") != std::string::npos);
    EXPECT_TRUE(text.find("beta") == std::string::npos);
    EXPECT_TRUE(text.find("nothing here") == std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{3});

    const auto scope_at = [&](std::size_t byte) { return ExcerptScopeAt(ed, text, byte); };
    EXPECT_EQ(scope_at(text.find("alpha widget") + 6), std::string{"ui.excerpt.match"});
    EXPECT_EQ(scope_at(text.find("alpha widget")), std::string{});
  }

  TEST_CASE("search: a refresh keeps the cursor on the excerpt it was reading");
  {
    Editor ed = fresh();
    SearchExcerpts(ed, "widget");
    PumpUntilIdle(ed);
    const std::string before = view(ed);
    const auto at = static_cast<Index>(before.find("gamma widget tail"));
    EXPECT_TRUE(at > 0);
    ed.doc.selections.Set(Selection{at, at, -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);

    scratch.Write("one.txt",
                  "inserted\nalpha widget\nbeta\ngamma widget tail\nSKIP widget\n");
    MaybeRefreshExcerptView(ed);
    PumpUntilIdle(ed);
    const std::string after = view(ed);
    EXPECT_TRUE(after.find("one.txt:4,5") != std::string::npos);

    const Index cursor = CursorOf(ed.doc.table, ed.doc.selections.Primary());
    const Index line = LineAt(ed.doc.table, cursor);
    EXPECT_TRUE(line > 0);
    EXPECT_TRUE(ReadDocRange(ed.doc.table, LineContentRange(ed.doc.table, line))
                    .find("one.txt:4,5") != std::string::npos);

    scratch.Write("one.txt", "alpha widget\nbeta\ngamma widget tail\nSKIP widget\n");
  }

  TEST_CASE("search: undo after a refresh writes the excerpt back into the file");
  {
    const std::string pristine = "alpha widget\nbeta\ngamma widget tail\nSKIP widget\n";
    const auto file_text = [](const std::filesystem::path& p) {
      std::ifstream in{p, std::ios::binary};
      return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    };
    Editor ed = fresh();
    SearchExcerpts(ed, "widget");
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("one.txt:3,4") != std::string::npos);

    scratch.Write("one.txt", "alpha widget\nbeta\ngamma widget tail\n");
    MaybeRefreshExcerptView(ed);
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("SKIP") == std::string::npos);
    EXPECT_FALSE(ed.doc.modified);

    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("SKIP widget") != std::string::npos);
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk"), std::string{"wrote 1 hunk"});
    EXPECT_EQ(file_text(scratch.dir / "one.txt"), pristine);
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    scratch.Write("one.txt", pristine);
  }

  TEST_CASE("search: -e excludes, with gai's meaning");
  {
    Editor ed = fresh();
    SearchExcerpts(ed, "-f widget -e SKIP");
    PumpUntilIdle(ed);
    EXPECT_EQ(ed.doc.excerpts.refs.size(), std::size_t{3});
    const std::string shown = view(ed);
    const std::size_t header_end = shown.find('\n');
    EXPECT_TRUE(header_end != std::string::npos);
    const std::string body = (header_end == std::string::npos) ? shown : shown.substr(header_end);
    EXPECT_TRUE(body.find("SKIP") == std::string::npos);
  }

  TEST_CASE("search: several filters are an OR, the way gai reads them");
  {
    Editor ed = fresh();
    SearchExcerpts(ed, "-f alpha beta");
    PumpUntilIdle(ed);
    EXPECT_EQ(ed.doc.excerpts.refs.size(), std::size_t{2});
    const std::string text = view(ed);
    EXPECT_TRUE(text.find("alpha widget") != std::string::npos);
    EXPECT_TRUE(text.find("beta") != std::string::npos);
  }

  TEST_CASE("search: quoting, and what the flag rules refuse");
  {
    Editor ed = fresh();
    scratch.Write("dash.txt", "-flag here\n");
    SearchExcerpts(ed, "-f '-flag'");
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("-flag here") != std::string::npos);

    Editor spaced = fresh();
    SearchExcerpts(spaced, "'gamma widget'");
    PumpUntilIdle(spaced);
    EXPECT_EQ(spaced.doc.excerpts.refs.size(), std::size_t{1});

    Editor bad = fresh();
    SearchExcerpts(bad, "-r /a/b/ widget");
    PumpUntilIdle(bad);
    EXPECT_TRUE(bad.status.find("only -f and -e") != std::string::npos);
    EXPECT_FALSE(IsExcerptView(bad.doc));

    Editor broken = fresh();
    SearchExcerpts(broken, "widget(");
    PumpUntilIdle(broken);
    EXPECT_TRUE(broken.status.level() == StatusLevel::kError);
    EXPECT_FALSE(IsExcerptView(broken.doc));

    Editor bare = fresh();
    SearchExcerpts(bare, "-e nope");
    PumpUntilIdle(bare);
    EXPECT_TRUE(bare.status.find("nothing to search for") != std::string::npos);
    EXPECT_FALSE(IsExcerptView(bare.doc));

    Editor missing = fresh();
    SearchExcerpts(missing, "definitely-not-in-any-file");
    PumpUntilIdle(missing);
    EXPECT_TRUE(missing.status.find("no match") != std::string::npos);
    EXPECT_FALSE(IsExcerptView(missing.doc));
  }

  TEST_CASE("search: the view is the references view, so g o and the width knob work");
  {
    Editor ed = fresh();
    SearchExcerpts(ed, "widget");
    PumpUntilIdle(ed);
    EXPECT_FALSE(ed.doc.read_only);

    RunTypableCommand(ed, "set-excerpt-context 1");
    EXPECT_EQ(ed.doc.excerpts.refs.size(), std::size_t{4});
    EXPECT_TRUE(view(ed).find("beta") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
    EXPECT_TRUE(view(ed).starts_with("4 matches for widget"));

    const std::string text = view(ed);
    const Index at = LineAt(ed.doc.table, static_cast<Index>(text.find("gamma widget")));
    ed.doc.selections.Set(Selection{LineStart(ed.doc.table, at), LineStart(ed.doc.table, at), -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    RunCommands(ed, {"goto_excerpt_source"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"one.txt"});
    EXPECT_FALSE(IsExcerptView(ed.doc));
    EXPECT_EQ(LineAt(ed.doc.table, ed.doc.selections.Primary().head), Index{2});

    Editor back = fresh();
    SearchExcerpts(back, "widget");
    PumpUntilIdle(back);
    // front() on an empty vector is how a search that returned nothing became a
    // segfault with no test name rather than a failed assertion.
    EXPECT_FALSE(back.doc.excerpts.blocks.empty());
    if (back.doc.excerpts.blocks.empty()) return;
    const Index header = back.doc.excerpts.blocks.front().header_line;
    back.doc.selections.Set(Selection{LineStart(back.doc.table, header),
                                      LineStart(back.doc.table, header), -1});
    back.doc.selections.EnsureBlockCursors(back.doc.table);
    RunCommands(back, {"goto_excerpt_source"});
    EXPECT_EQ(back.doc.file.filename().string(), std::string{"one.txt"});
    EXPECT_EQ(LineAt(back.doc.table, back.doc.selections.Primary().head), Index{0});
  }

  TEST_CASE("search: the scan runs beside the editor, and esc abandons it");
  {
    Editor ed = fresh();
    SearchExcerpts(ed, "widget");
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    EXPECT_TRUE(ed.pending_commands[0].scan != nullptr);
    EXPECT_EQ(ed.pending_commands[0].pid, -1);
    EXPECT_EQ(ed.pending_commands[0].fd, -1);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
    EXPECT_FALSE(IsExcerptView(ed.doc));
    EXPECT_TRUE(ed.status.find(":from-cancel stops it") != std::string::npos);
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));

    Editor gone = fresh();
    SearchExcerpts(gone, "widget");
    EXPECT_TRUE(CancelCommandJob(gone));
    EXPECT_TRUE(gone.pending_commands.empty());
    EXPECT_TRUE(gone.status.find("cancelled") != std::string::npos);
    PumpUntilIdle(gone);
    EXPECT_FALSE(IsExcerptView(gone.doc));
  }

  TEST_CASE("search: a stale view re-scans in the background and lands fresh");
  {
    Editor ed = fresh();
    SearchExcerpts(ed, "widget");
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));

    scratch.Write("four.txt", "widget again\n");
    ed.doc.excerpts.refs_stale = true;
    EXPECT_TRUE(RebuildExcerptView(ed));
    EXPECT_TRUE(ed.status.find("re-scanning") != std::string::npos);
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("widget again") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("references view: the symbol scan runs on the worker too");
  {
    const std::filesystem::path code = scratch.Write(
        "code.cpp", "int koi_marker() { return 1; }\nint use() { return koi_marker(); }\n");
    Editor ed = fresh();
    EXPECT_TRUE(OpenTarget(ed, code.string()));
    const std::string text = ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
    const Index at = static_cast<Index>(text.find("koi_marker"));
    ed.doc.selections.Set(Selection{at, at + Index{10}, -1});
    ShowReferenceExcerpts(ed);
    EXPECT_TRUE(ed.status.find("scanning for") != std::string::npos);
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_TRUE(ed.doc.excerpts.kind == ExcerptView::Kind::kReferences);
    EXPECT_TRUE(ed.doc.excerpts.refs.size() >= std::size_t{2});
    EXPECT_TRUE(view(ed).find("return koi_marker") != std::string::npos);
  }
}

void ExcerptHeaderLineStaysTrue() {
  TEST_CASE("excerpts: the cached header line never outlives the edits under it");
  const Scratch scratch{"koi-header-line"};
  scratch.Write("a.txt", "widget one\nfiller\n");
  {
    std::string wide;
    for (int i = 1; i <= 10; ++i) wide += "widget b" + std::to_string(i) + "\n";
    scratch.Write("b.txt", wide);
  }

  const auto fresh = [&scratch] {
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    ed.settings.file_filter = "find " + scratch.dir.string() + " -type f -printf '%p\\n'";
    return ed;
  };
  const auto view = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  const auto insert_at = [](Editor& ed, std::size_t at, std::string_view what) {
    const Change change{static_cast<Index>(at), static_cast<Index>(at), what};
    UndoGroup group(ed.doc.table);
    std::ignore = Apply(ed.doc.table, std::span{&change, 1}, CursorState{}, CursorState{}, nullptr);
    ed.doc.modified = true;
  };

  // block.header_line is written when the view is built and refreshed on save,
  // and nothing maps it through the edits in between -- so the one consumer
  // that trusted it, BlockAtCursor's fallback, has to stop.
  //
  // The span has to be wider than one line or std::min(block->last, ...) in
  // GotoExcerptSource clamps the wrong answer back onto the right one and the
  // case passes with the defect still in place.
  {
    Editor ed = fresh();
    SearchExcerpts(ed, "widget");
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{2});
    EXPECT_EQ(ed.doc.excerpts.blocks.back().first, Index{1});
    EXPECT_EQ(ed.doc.excerpts.blocks.back().last, Index{10});

    // Three lines into the first hunk's body: every header below it moves.
    insert_at(ed, view(ed).find("widget one"), "extra 1\nextra 2\nextra 3\n");
    // Break the parse, which is the only way into the fallback.
    const std::size_t header = view(ed).find("a.txt:1");
    EXPECT_TRUE(header != std::string::npos);
    insert_at(ed, header, "MANGLED-");

    const std::size_t body = view(ed).find("widget b3");
    EXPECT_TRUE(body != std::string::npos);
    ed.doc.selections.Set(Selection{static_cast<Index>(body), static_cast<Index>(body), -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    GotoExcerptSource(ed);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string("b.txt"));
    EXPECT_EQ(LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary())), Index{2});
  }

  // The save half: the refresh used to sit inside the per-file loop, so blocks
  // in files the save had no changes for kept their old line numbers.
  {
    Editor ed = fresh();
    SearchExcerpts(ed, "widget");
    PumpUntilIdle(ed);
    // Grow a.txt's hunk, which moves b.txt's header down without touching b.txt.
    const std::size_t at = view(ed).find("widget one");
    EXPECT_TRUE(at != std::string::npos);
    insert_at(ed, at + std::string_view{"widget one"}.size(), "\nwidget grown");
    EXPECT_TRUE(SaveExcerptView(ed));

    const std::vector<std::string_view> lines = [&] {
      std::vector<std::string_view> out;
      static std::string held;
      held = view(ed);
      std::size_t from = 0;
      while (from <= held.size()) {
        const std::size_t eol = held.find('\n', from);
        if (eol == std::string::npos) {
          if (from < held.size()) out.emplace_back(held.data() + from, held.size() - from);
          break;
        }
        out.emplace_back(held.data() + from, eol - from);
        from = eol + 1;
      }
      return out;
    }();
    for (const ExcerptBlock& block : ed.doc.excerpts.blocks) {
      const auto line = static_cast<std::size_t>(block.header_line);
      EXPECT_TRUE(line < lines.size());
      if (line < lines.size()) EXPECT_EQ(std::string{lines[line]}, block.header);
    }
    scratch.Write("a.txt", "widget one\nfiller\n");
  }
}

void ExcerptSaveRoundTripFuzz(Rng& rng) {
  TEST_CASE("excerpts: what a hunk shows is what its file holds, after any :w");
  namespace fs = std::filesystem;

  // The property that found the header-line defect: edit a hunk body at random,
  // save, then check every block against the file it names. A refusal is a
  // legitimate outcome; a save that reports success and writes something else
  // is not.
  for (int round = 0; round < 6; ++round) {
    const Scratch scratch{"koi-excerpt-roundtrip"};
    scratch.Write("a.txt", "one widget here\nplain\nanother widget\nplain2\nwidget tail\n");
    scratch.Write("b.txt", "start\nwidget b1\nmid\nwidget b2\nend\n");
    scratch.Write("c.txt", "nothing interesting\n");

    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = rng.Pick(0, 2);
    ed.settings.file_filter = "find " + scratch.dir.string() + " -type f -printf '%p\\n'";
    SearchExcerpts(ed, "widget");
    PumpUntilIdle(ed);
    if (!IsExcerptView(ed.doc)) continue;

    const auto view = [&ed] {
      return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
    };
    const auto split = [](const std::string& text) {
      std::vector<std::string> out;
      std::size_t at = 0;
      while (at < text.size()) {
        const std::size_t eol = text.find('\n', at);
        if (eol == std::string::npos) { out.push_back(text.substr(at)); break; }
        out.push_back(text.substr(at, eol - at));
        at = eol + 1;
      }
      return out;
    };

    for (int step = 0; step < 10; ++step) {
      const std::string text = view();
      const std::vector<std::string> lines = split(text);
      if (ed.doc.excerpts.blocks.empty()) break;

      // Body lines only: a header is the save contract and editing one is a
      // different test.
      std::set<std::string> headers;
      for (const ExcerptBlock& block : ed.doc.excerpts.blocks) {
        headers.insert(block.header);
        for (const std::string& worn : block.prior_headers) headers.insert(worn);
      }
      std::vector<Index> body;
      bool seen_header = false;
      for (Index i = 0; i < std::ssize(lines); ++i) {
        const std::string& line = lines[static_cast<std::size_t>(i)];
        if (headers.count(line) != 0) { seen_header = true; continue; }
        if (seen_header && !line.empty()) body.push_back(i);
      }
      if (body.empty()) break;

      const Index target = body[static_cast<std::size_t>(rng.Pick(0, std::ssize(body) - 1))];
      const Index start = LineStart(ed.doc.table, target);
      const Index stop = start + std::ssize(lines[static_cast<std::size_t>(target)]);
      static const char* kEdits[] = {"EDIT", "", "x", "two\nlines", "  ", "é世"};
      const std::string put{kEdits[static_cast<std::size_t>(rng.Pick(0, 5))]};
      Index from = start + rng.Pick(0, stop - start);
      while ((from > start) && !IsGraphemeBoundary(ed.doc.table, from)) --from;
      Index to = std::min(stop, from + rng.Pick(0, 3));
      while ((to > from) && (to < stop) && !IsGraphemeBoundary(ed.doc.table, to)) --to;
      if ((from == to) && put.empty()) continue;

      const Change change{from, to, put};
      {
        UndoGroup group(ed.doc.table);
        if (Apply(ed.doc.table, std::span{&change, 1}, CursorState{}, CursorState{}, nullptr)) {
          continue;
        }
      }
      ed.doc.modified = true;
      ed.doc.selections.Normalize(ed.doc.table);
      ed.doc.selections.EnsureBlockCursors(ed.doc.table);
      if (!SaveExcerptView(ed)) continue;
      EXPECT_FALSE(ed.doc.modified);

      const std::vector<std::string> after = split(view());
      std::map<std::string, std::vector<std::string>> disk;
      std::map<std::string, Index> last_end;
      for (std::size_t b = 0; b < ed.doc.excerpts.blocks.size(); ++b) {
        const ExcerptBlock& block = ed.doc.excerpts.blocks[b];
        if (block.no_body) continue;
        if (disk.count(block.path) == 0) {
          std::ifstream in{block.path, std::ios::binary};
          disk[block.path] = split(std::string{std::istreambuf_iterator<char>(in),
                                               std::istreambuf_iterator<char>()});
        }
        const std::vector<std::string>& held = disk[block.path];
        EXPECT_TRUE((block.first >= 1) && (block.last <= std::ssize(held)));
        if ((block.first < 1) || (block.last > std::ssize(held))) continue;

        // Blocks in one file stay ordered and disjoint, or the next save
        // writes one hunk's bytes over another's span.
        if (const auto seen = last_end.find(block.path); seen != last_end.end()) {
          EXPECT_TRUE(block.first > seen->second);
        }
        last_end[block.path] = block.last;

        const auto header = static_cast<std::size_t>(block.header_line);
        EXPECT_TRUE(header < after.size());
        if (header >= after.size()) continue;
        EXPECT_EQ(after[header], block.header);

        std::size_t stop_at = after.size();
        if ((b + 1) < ed.doc.excerpts.blocks.size()) {
          stop_at = static_cast<std::size_t>(ed.doc.excerpts.blocks[b + 1].header_line);
        }
        std::vector<std::string> shown;
        for (std::size_t i = header + 1; (i < stop_at) && (i < after.size()); ++i) {
          const bool separator = ((i + 1) == stop_at) && (stop_at < after.size()) &&
                                 after[i].empty();
          if (separator) break;
          shown.push_back(after[i]);
        }
        const std::vector<std::string> in_file{
            held.begin() + (block.first - 1),
            held.begin() + block.last};
        EXPECT_TRUE(shown == in_file);
      }
    }
  }
}

void ReanchorRefusesToSwallowLinesItNeverShowed() {
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-reanchor-span"};
  const fs::path home = scratch.Write("home.txt", "somewhere to stand\n");

  const auto view = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  const auto file_text = [](const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };

  TEST_CASE("excerpt revert: a span that grew under the view is refused, not overwritten");
  {
    // FindSpanAgain's fallback picks a head line and a tail line by two
    // independent unique-line searches and used to return the pair with no
    // look at what lies between them. Here the file gained seven lines inside
    // the excerpt: the anchors still read HEAD and TAIL, so the block was
    // re-anchored onto the whole eleven-line span, `original` was rewritten to
    // it -- which by itself made an untouched hunk look edited -- and :w wrote
    // the three stale lines over all nine, reporting "wrote 1 hunk into 1
    // file" while n1..n7 went silently into the void.
    const std::string pristine = "top\nHEAD\nmid\nTAIL\nbottom\n";
    const fs::path path = scratch.Write("grow.txt", pristine);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":3");
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_TRUE(view(ed).find("grow.txt:2,4") != std::string::npos);
    EXPECT_TRUE(view(ed).find("mid") != std::string::npos);

    const std::string grown = "top\nHEAD\nn1\nn2\nn3\nn4\nn5\nn6\nn7\nTAIL\nbottom\n";
    scratch.Write("grow.txt", grown);
    MaybeRefreshExcerptView(ed);
    PumpUntilIdle(ed);
    EXPECT_FALSE(ed.doc.modified);

    // The undo takes the view back across the rebuild, which is what arms the
    // re-anchor on the next save. The user has typed nothing.
    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("mid") != std::string::npos);
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("wrote") == std::string::npos);
    EXPECT_TRUE(ed.status.find("moved too far") != std::string::npos);
    EXPECT_EQ(file_text(path), grown);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("excerpt revert: a block that only slid down still re-anchors, and writes nothing");
  {
    // The other half of the same check: refusing a grown span must not refuse
    // a block that merely moved. Lines go in *above* it, so the span still
    // reads line for line -- the re-anchor takes, and a save with no edits
    // has nothing to write rather than something to lose.
    std::string pristine;
    for (int i = 1; i <= 8; ++i) pristine += "b" + std::to_string(i) + "\n";
    const fs::path path = scratch.Write("slide.txt", pristine);
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":4");
    PumpUntilIdle(ed);
    EXPECT_TRUE(view(ed).find("slide.txt:3,5") != std::string::npos);

    const std::string slid = "x1\nx2\n" + pristine;
    scratch.Write("slide.txt", slid);
    MaybeRefreshExcerptView(ed);
    PumpUntilIdle(ed);
    RunCommands(ed, {"undo"});
    EXPECT_TRUE(view(ed).find("b3") != std::string::npos);
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("nothing to write") != std::string::npos);
    EXPECT_EQ(ed.doc.excerpts.blocks[0].first, Index{5});
    EXPECT_EQ(ed.doc.excerpts.blocks[0].last, Index{7});
    EXPECT_EQ(file_text(path), slid);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void ExcerptViewsNeverAdmitIllFormedFiles() {
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-excerpt-utf8"};
  const fs::path home = scratch.Write("home.txt", "somewhere to stand\n");
  // The rebuild below re-scans the project; without this the scan is the
  // built-in `find .` over the working directory, so the case would pass or
  // fail on what the build tree happens to contain.
  const std::string pinned_filter = "find " + scratch.dir.string() + " -type f -printf '%p\\n'";

  const auto view = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  const auto file_text = [](const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };
  const auto in_body = [&view](Editor& ed, std::string_view needle) {
    const std::string text = view(ed);
    return text.find(needle, text.find("\n\n") + 2);
  };
  const auto edit = [](Editor& ed, std::size_t at, std::size_t take, std::string_view put) {
    if (take > 0) {
      std::ignore = Delete(static_cast<Index>(at), static_cast<Index>(at + take), ed.doc.table);
    }
    if (!put.empty()) std::ignore = Insert(put, static_cast<Index>(at), ed.doc.table);
    ed.doc.modified = true;
  };

  TEST_CASE("excerpt view: a file that is not UTF-8 gets a bodyless block, not a poisoned view");
  {
    // BuildExcerpts read every reference file with ReadWholeFile and handed the
    // bytes straight to ResetToOriginal -- the one ingress into a document that
    // was not gated on IsWellFormedUtf8. A latin-1 file the scan happened to
    // match put ill-formed bytes into a live document: the caret teleported
    // through SegmentAround's resync, every edit was refused by Apply, and the
    // rebuild could never take because SwapViewText's Apply refused the text
    // too. One bad file must cost its own block and nothing else.
    const std::string latin1 = "caf\xe9\nlatin two\nlatin three\n";
    const fs::path bad = scratch.Write("bad.txt", latin1);
    const fs::path good = scratch.Write("good.txt", "g1\ng2\ng3\ng4\ng5\n");
    EXPECT_FALSE(IsWellFormedUtf8(latin1));

    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.file_filter = pinned_filter;
    ed.settings.excerpt_context = 1;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(
        ed, {Symbol{bad.string(), 2, 1, "hit"}, Symbol{good.string(), 3, 1, "hit"}}, "hit");
    EXPECT_TRUE(IsExcerptView(ed.doc));

    // The whole point: nothing ill-formed ever reaches the document.
    EXPECT_TRUE(IsWellFormedUtf8(view(ed)));
    EXPECT_TRUE(view(ed).find("latin two") == std::string::npos);
    EXPECT_TRUE(view(ed).find("caf") == std::string::npos);

    // Both files are still named; the bad one carries a header note and no body.
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{2});
    const ExcerptBlock* bad_block = nullptr;
    const ExcerptBlock* good_block = nullptr;
    for (const ExcerptBlock& block : ed.doc.excerpts.blocks) {
      if (block.path == bad.string()) bad_block = &block;
      if (block.path == good.string()) good_block = &block;
    }
    EXPECT_TRUE(bad_block != nullptr);
    EXPECT_TRUE(good_block != nullptr);
    if ((bad_block != nullptr) && (good_block != nullptr)) {
      EXPECT_TRUE(bad_block->no_body);
      EXPECT_TRUE(bad_block->header.find("not valid UTF-8") != std::string::npos);
      EXPECT_FALSE(good_block->no_body);
    }
    EXPECT_TRUE(view(ed).find("bad.txt") != std::string::npos);
    EXPECT_TRUE(view(ed).find("not valid UTF-8") != std::string::npos);
    EXPECT_TRUE(view(ed).find("good.txt:2,4") != std::string::npos);
    EXPECT_TRUE(view(ed).find("g3") != std::string::npos);

    // The clean file's hunk is still a live, writable hunk.
    const std::size_t at = in_body(ed, "g3");
    EXPECT_TRUE(at != std::string::npos);
    edit(ed, at, 2, "g3x");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_EQ(file_text(good), std::string{"g1\ng2\ng3x\ng4\ng5\n"});
    // ...and the save never went near the file it could not decode.
    EXPECT_EQ(file_text(bad), latin1);
    EXPECT_TRUE(IsWellFormedUtf8(view(ed)));
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    // A rebuild is what wedged before -- SwapViewText's Apply refused the text
    // and RefreshLiveExcerptViews had already cleared rebuild_on_focus.
    ed.status.clear();
    EXPECT_TRUE(RebuildExcerptView(ed));
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsWellFormedUtf8(view(ed)));
    EXPECT_TRUE(view(ed).find("g3x") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void ExcerptViewsFollowATrailingNewlineToggle() {
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-excerpt-eof-nl"};
  const fs::path home = scratch.Write("home.txt", "somewhere to stand\n");
  // Keep every scan inside the fixture; the built-in `find .` would otherwise
  // walk whatever the build tree happens to contain.
  const std::string pinned_filter = "find " + scratch.dir.string() + " -type f -printf '%p\\n'";

  const auto view = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  const auto text_of = [](const Document& doc) {
    return ReadDocRange(doc.table, {0, DocLength(doc.table)});
  };
  const auto file_text = [](const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };
  const auto in_body = [](const std::string& text, std::string_view needle) {
    return text.find(needle, text.find("\n\n") + 2);
  };
  const auto edit = [](Editor& ed, std::size_t at, std::size_t take, std::string_view put) {
    if (take > 0) {
      std::ignore = Delete(static_cast<Index>(at), static_cast<Index>(at + take), ed.doc.table);
    }
    if (!put.empty()) std::ignore = Insert(put, static_cast<Index>(at), ed.doc.table);
    ed.doc.modified = true;
  };
  // One hunk, reaching EOF, with the anchor on the last line.
  const auto open_view = [&](Editor& ed, const fs::path& target) {
    ed.theme = BuiltinTheme();
    ed.settings.file_filter = pinned_filter;
    ed.settings.excerpt_context = 0;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    OpenReferenceExcerpts(ed, {Symbol{target.string(), 2, 1, "hit"}}, "hit");
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{1});
  };

  // A hunk that reaches EOF renders the same either way: "alpha\nbravo" gives a
  // body of "bravo" plus a newline the file does not have, "alpha\nbravo\n"
  // gives a body of "bravo\n", and both end up as "bravo\n" in the view under
  // the same `first,last` header. Only ExcerptBlock::synthesized_newline says
  // which one it was, and nothing about it is visible in the text -- so the
  // "text unchanged, keep the model" shortcut in OpenGeneratedView used to
  // carry the old flag across a rebuild. ExpectedSpanBytes then disagreed with
  // the disk by exactly one byte for ever: :w refused the hunk and said to
  // rebuild the view, and the rebuild it advised took this very branch.
  TEST_CASE("excerpts: a trailing newline appearing under an EOF hunk survives a rebuild");
  {
    const fs::path tail = scratch.Write("grew.txt", "alpha\nbravo");
    Editor ed;
    open_view(ed, tail);
    EXPECT_TRUE(ed.doc.excerpts.blocks.front().synthesized_newline);
    EXPECT_EQ(ed.doc.excerpts.blocks.front().first, Index{2});
    EXPECT_EQ(ed.doc.excerpts.blocks.front().last, Index{2});
    const std::string before = view(ed);
    EXPECT_TRUE(before.find("grew.txt:2,2") != std::string::npos);

    // Sit in the hunk, so the rebuild has a block to put the caret back on.
    const std::size_t was_at = in_body(before, "bravo");
    EXPECT_TRUE(was_at != std::string::npos);
    ed.doc.selections.Set(
        Selection{static_cast<Index>(was_at), static_cast<Index>(was_at), -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);

    // A formatter, a hook, another editor -- anything that ends the file with a
    // newline it did not have.
    scratch.Write("grew.txt", "alpha\nbravo\n");
    ed.status.clear();
    EXPECT_TRUE(RebuildExcerptView(ed));

    // The premise: same text, different model. The rebuild has to notice.
    EXPECT_EQ(view(ed), before);
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{1});
    EXPECT_FALSE(ed.doc.excerpts.blocks.front().synthesized_newline);
    // Taking the full-replace branch is not a jump to the top of the view: it
    // re-anchors on the block the caret was in, as it already does whenever the
    // text changed.
    EXPECT_EQ(LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary())),
              ed.doc.excerpts.blocks.front().header_line);

    const std::size_t at = in_body(view(ed), "bravo");
    EXPECT_TRUE(at != std::string::npos);
    edit(ed, at, 5, "bravo edited");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_EQ(file_text(tail), std::string{"alpha\nbravo edited\n"});
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  // The same trace in the other direction: the file loses its trailing newline,
  // so the rebuilt hunk has to start borrowing one, and the save has to put the
  // file back without it.
  TEST_CASE("excerpts: a trailing newline vanishing under an EOF hunk survives a rebuild");
  {
    const fs::path tail = scratch.Write("shrank.txt", "alpha\nbravo\n");
    Editor ed;
    open_view(ed, tail);
    EXPECT_FALSE(ed.doc.excerpts.blocks.front().synthesized_newline);
    const std::string before = view(ed);
    EXPECT_TRUE(before.find("shrank.txt:2,2") != std::string::npos);

    scratch.Write("shrank.txt", "alpha\nbravo");
    ed.status.clear();
    EXPECT_TRUE(RebuildExcerptView(ed));

    EXPECT_EQ(view(ed), before);
    EXPECT_EQ(ed.doc.excerpts.blocks.size(), std::size_t{1});
    EXPECT_TRUE(ed.doc.excerpts.blocks.front().synthesized_newline);

    const std::size_t at = in_body(view(ed), "bravo");
    EXPECT_TRUE(at != std::string::npos);
    edit(ed, at, 5, "bravo edited");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_EQ(file_text(tail), std::string{"alpha\nbravo edited"});
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  // The other copy of the shortcut: a view that is on screen but not the active
  // buffer refreshes through ReplaceViewInPlace, which had the same test.
  TEST_CASE("excerpts: an off-focus view refresh notices the newline toggle too");
  {
    const fs::path tail = scratch.Write("aside.txt", "alpha\nbravo");
    Editor ed;
    open_view(ed, tail);
    const std::size_t excerpt_at = ed.active;
    EXPECT_TRUE(ed.doc.excerpts.blocks.front().synthesized_newline);
    const std::string before = view(ed);

    // Split, then point the focused window at another buffer: the view stays on
    // screen, which is what RefreshLiveExcerptViews refreshes in place.
    RunCommands(ed, {"vsplit"});
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    EXPECT_TRUE(ed.active != excerpt_at);
    EXPECT_TRUE(excerpt_at < ed.buffers.size());

    scratch.Write("aside.txt", "alpha\nbravo\n");
    ed.buffers[excerpt_at].excerpts.rebuild_on_focus = true;
    RefreshLiveExcerptViews(ed);

    EXPECT_EQ(text_of(ed.buffers[excerpt_at]), before);
    EXPECT_EQ(ed.buffers[excerpt_at].excerpts.blocks.size(), std::size_t{1});
    EXPECT_FALSE(ed.buffers[excerpt_at].excerpts.blocks.front().synthesized_newline);

    SwitchToBuffer(ed, excerpt_at);
    const std::size_t at = in_body(view(ed), "bravo");
    EXPECT_TRUE(at != std::string::npos);
    edit(ed, at, 5, "bravo edited");
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_EQ(file_text(tail), std::string{"alpha\nbravo edited\n"});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void AutoPairedKeysGoThroughTheEditFunnel() {
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-auto-pair-funnel"};
  const fs::path home = scratch.Write("home.txt", "somewhere to stand\n");

  const auto view = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  const auto file_text = [](const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };
  const auto in_body = [&view](Editor& ed, std::string_view needle) {
    const std::string text = view(ed);
    return static_cast<Index>(text.find(needle, text.find("\n\n") + 2));
  };
  const auto twelve = [](char letter) {
    std::string body;
    for (int i = 1; i <= 12; ++i) body += std::string{letter} + std::to_string(i) + "\n";
    return body;
  };
  // Typing one key the way the editor does, through the auto-pair branch of
  // FlushPendingAsText.
  const auto type = [](Editor& ed, std::string_view key) {
    std::vector<Key> pending{K(key)};
    FlushPendingAsText(ed, pending);
  };
  const auto caret_at = [](Editor& ed, Index at) {
    ed.doc.selections.Set(Selection{at, at, -1});
    ed.mode = Mode::kInsert;
  };

  TEST_CASE("excerpt revert: an auto-paired keystroke after undo forgets the epoch too");
  {
    // TypeOneKey called Insert directly and set `modified` by hand, so the one
    // thing Edited() does that nothing else does -- DropUnreachableEpochs --
    // never ran for it. Undoing past an epoch boundary leaves
    // `active < boundaries.size()`, which is only legal while the redo branch
    // that owns those epochs is still reachable; this edit destroys that
    // branch. Left behind, the stale entry sends the next :w down the
    // reanchor path, where an untouched-but-renumbered block can be refused
    // with "moved too far" and the user's hunk is not written.
    //
    // The existing test for this drives the edit with delete_char_forward,
    // which goes through Edited(), so the auto-pair path went uncovered.
    const fs::path path = scratch.Write("pair.txt", twelve('p'));
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 1;
    ed.settings.auto_pairs = true;
    EXPECT_TRUE(OpenTarget(ed, home.string()));
    RunTypableCommand(ed, "from echo " + path.string() + ":3");
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_TRUE(view(ed).find("pair.txt:2,4") != std::string::npos);

    RunTypableCommand(ed, "set-excerpt-context 2");
    EXPECT_EQ(ed.doc.excerpt_epochs.boundaries.size(), std::size_t{1});
    EXPECT_EQ(ed.doc.excerpt_epochs.active, std::size_t{1});

    RunCommands(ed, {"undo"});
    EXPECT_EQ(ed.doc.excerpt_epochs.active, std::size_t{0});
    EXPECT_EQ(ed.doc.excerpt_epochs.boundaries.size(), std::size_t{1});
    EXPECT_TRUE(view(ed).find("pair.txt:2,4") != std::string::npos);

    // The edit that abandons the branch: one "(" typed in insert mode.
    caret_at(ed, in_body(ed, "p3"));
    type(ed, "(");
    EXPECT_TRUE(view(ed).find("(p3") != std::string::npos);
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_FALSE(CanRedo(ed.doc.table));
    EXPECT_TRUE(ed.doc.excerpt_epochs.boundaries.empty());
    EXPECT_EQ(ed.doc.excerpt_epochs.active, std::size_t{0});
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    // And so :w takes the ordinary path, not the reanchor one.
    ed.mode = Mode::kNormal;
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    ed.status.clear();
    RunTypableCommand(ed, "w");
    EXPECT_TRUE(ed.status.find("moved too far") == std::string::npos);
    EXPECT_EQ(StatusAbout(ed, "wrote 1 hunk into 1 file"), std::string{"wrote 1 hunk into 1 file"});
    EXPECT_TRUE(file_text(path).find("\n(p3\n") != std::string::npos);
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("an auto-paired keystroke warns about a read-only file like any other edit");
  {
    // Bypassing Edited() also bypassed the warning: typing `z` on a read-only
    // buffer said so, typing `(` said nothing.
    Editor ed;
    ResetToOriginal(ed.doc.table, "hello\n");
    ed.doc.modified = false;
    ed.doc.read_only = true;
    caret_at(ed, 5);
    ed.status.clear();
    type(ed, "(");
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}), std::string("hello()\n"));
    EXPECT_TRUE(ed.status.find("is not writable") != std::string::npos);
    EXPECT_TRUE(ed.doc.modified);
  }

  TEST_CASE("routing auto-pairs through the funnel leaves the pairs and the carets alone");
  {
    const auto text_of = [](const Editor& ed) {
      return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
    };
    const auto heads = [](const Editor& ed) {
      std::vector<Index> out;
      for (const Selection& s : ed.doc.selections.Ranges()) out.push_back(s.head);
      return out;
    };

    // One caret: the pair lands whole and the caret sits between its halves.
    Editor ed;
    ResetToOriginal(ed.doc.table, "\n");
    caret_at(ed, 0);
    type(ed, "(");
    EXPECT_EQ(text_of(ed), std::string("()\n"));
    EXPECT_TRUE(heads(ed) == std::vector<Index>({1}));
    EXPECT_TRUE(ed.doc.modified);

    // Three carets, one per line: each pair lands at its own caret, and the
    // carets below each edit are mapped past it.
    Editor two;
    ResetToOriginal(two.doc.table, "aa\nbb\ncc\n");
    two.mode = Mode::kInsert;
    two.doc.selections.Replace(two.doc.table,
                               std::vector<Selection>{Selection{2, 2, -1}, Selection{5, 5, -1},
                                                      Selection{8, 8, -1}});
    type(two, "(");
    EXPECT_EQ(text_of(two), std::string("aa()\nbb()\ncc()\n"));
    EXPECT_TRUE(heads(two) == std::vector<Index>({3, 8, 13}));
    EXPECT_EQ(EditorInvariants(two), std::string{});

    // The skip-over-the-close leg, and the no-pair leg, still land where they
    // did -- neither runs an edit at all.
    Editor over;
    ResetToOriginal(over.doc.table, "()\n");
    caret_at(over, 1);
    over.doc.modified = false;
    type(over, ")");
    EXPECT_EQ(text_of(over), std::string("()\n"));
    EXPECT_TRUE(heads(over) == std::vector<Index>({2}));
    EXPECT_FALSE(over.doc.modified);

    Editor word;
    ResetToOriginal(word.doc.table, "foo\n");
    caret_at(word, 0);
    type(word, "(");
    EXPECT_EQ(text_of(word), std::string("(foo\n"));
    EXPECT_TRUE(heads(word) == std::vector<Index>({1}));
  }
}

}  // namespace koi
