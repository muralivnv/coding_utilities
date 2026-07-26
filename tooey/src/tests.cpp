// Tests for the parts of tooey that do not need a terminal: text handling and
// layout. Everything here runs without a pty, which is the point of splitting them
// out of tooey.cpp -- the drawing and event loop still need the pty harness.

#include <string>
#include <string_view>
#include <vector>

#include "layout.h"
#include "test_harness.h"
#include "text.h"

using namespace tooey;

namespace {

// Convenience: run AppendCompleteItems over a whole string.
std::vector<std::string_view> SplitComplete(std::string_view s, char delim, std::string_view* tail_out = nullptr) {
  std::vector<std::string_view> out;
  const char* tail = AppendCompleteItems(s.data(), s.data() + s.size(), delim, out);
  if (tail_out != nullptr)
    *tail_out = std::string_view(tail, static_cast<size_t>(s.data() + s.size() - tail));
  return out;
}

std::string Escaped(std::string_view in) {
  std::string s(in);
  ShellEscapeInPlace(s);
  return s;
}

std::string Substituted(std::string_view search, std::string_view replace, std::string_view in) {
  std::string s(in);
  SubstituteInPlace(search, replace, s);
  return s;
}

size_t ParseAnsi(std::string_view s, AnsiColorState& state, uint32_t fg = 0, uint32_t bg = 0) {
  return ParseAnsiSequence(s.data(), s.data() + s.size(), fg, bg, state);
}

}  // namespace

// ============================================================================
// text.h -- UTF-8 decoding and widths
// ============================================================================
static void TestDecodeAndWidth() {
  TEST_CASE("DecodeUtf8");
  {
    constexpr std::string_view ascii = "a";
    const DecodedChar dc = DecodeUtf8(ascii.data(), ascii.data() + ascii.size());
    EXPECT_EQ(dc.cp, 0x61u);
    EXPECT_EQ(dc.len, 1);
  }
  {
    constexpr std::string_view two = "\xC3\xA9";  // U+00E9 e-acute
    const DecodedChar dc = DecodeUtf8(two.data(), two.data() + two.size());
    EXPECT_EQ(dc.cp, 0xE9u);
    EXPECT_EQ(dc.len, 2);
  }
  {
    constexpr std::string_view three = "\xE4\xB8\xAD";  // U+4E2D, wide
    const DecodedChar dc = DecodeUtf8(three.data(), three.data() + three.size());
    EXPECT_EQ(dc.cp, 0x4E2Du);
    EXPECT_EQ(dc.len, 3);
  }
  {
    constexpr std::string_view four = "\xF0\x9F\x8E\x89";  // U+1F389 party popper
    const DecodedChar dc = DecodeUtf8(four.data(), four.data() + four.size());
    EXPECT_EQ(dc.cp, 0x1F389u);
    EXPECT_EQ(dc.len, 4);
  }
  {
    // Malformed input must come back as U+FFFD with a byte count, never a negative
    // length -- callers have no error path and would loop forever on 0.
    constexpr std::string_view bad = "\xFF\xFE";
    const DecodedChar dc = DecodeUtf8(bad.data(), bad.data() + bad.size());
    EXPECT_TRUE(dc.len >= 1);
    EXPECT_EQ(dc.cp, 0xFFFDu);
  }
  {
    // A truncated multi-byte sequence must still advance.
    constexpr std::string_view cut = "\xE4\xB8";
    const DecodedChar dc = DecodeUtf8(cut.data(), cut.data() + cut.size());
    EXPECT_TRUE(dc.len >= 1);
  }

  TEST_CASE("CellWidth");
  EXPECT_EQ(CellWidth(0x61), 1);       // 'a'
  EXPECT_EQ(CellWidth(0x4E2D), 2);     // CJK is double width
  EXPECT_EQ(CellWidth(0x0301), 0);     // combining acute takes no cells
  EXPECT_TRUE(CellWidth(0x1F389) >= 1);

  TEST_CASE("CellWidthUtf8String");
  EXPECT_EQ(CellWidthUtf8String(""), 0);
  EXPECT_EQ(CellWidthUtf8String("hello"), 5);
  EXPECT_EQ(CellWidthUtf8String("\xE4\xB8\xAD\xE6\x96\x87"), 4);  // two wide glyphs
  // "e" + combining acute is one column, not two.
  EXPECT_EQ(CellWidthUtf8String("e\xCC\x81"), 1);
  // \r is skipped so a CRLF-terminated line does not measure an extra column.
  EXPECT_EQ(CellWidthUtf8String("ab\r"), 2);
  // Tabs advance to the next multiple of 8.
  EXPECT_EQ(CellWidthUtf8String("\t"), 8);
  EXPECT_EQ(CellWidthUtf8String("abc\t"), 8);
  EXPECT_EQ(CellWidthUtf8String("abcdefgh\t"), 16);

  TEST_CASE("CellWidthUtf8String skips SGR when ansi is set");
  EXPECT_EQ(CellWidthUtf8String("\x1b[31mred\x1b[m", true), 3);
  EXPECT_EQ(CellWidthUtf8String("\x1b[38;2;255;0;0mx\x1b[m", true), 1);
  // Without the flag the escape bytes count as text, which is why --ansi has to be
  // threaded into every width call rather than guessed.
  EXPECT_TRUE(CellWidthUtf8String("\x1b[31mred\x1b[m", false) > 3);
}

// ============================================================================
// text.h -- grapheme and word movement
// ============================================================================
static void TestGraphemeMovement() {
  TEST_CASE("grapheme steps over combining marks");
  {
    // "e" + U+0301: one grapheme, three bytes... actually 1 + 2 = 3 bytes.
    constexpr std::string_view s = "xe\xCC\x81y";  // x, e-with-acute, y
    EXPECT_EQ(NextGrapheme(s, 0), 1u);             // past 'x'
    EXPECT_EQ(NextGrapheme(s, 1), 4u);             // past the whole cluster
    EXPECT_EQ(PrevGrapheme(s, 4), 1u);             // back over the whole cluster
    EXPECT_EQ(PrevGrapheme(s, 1), 0u);
  }
  {
    // A regional-indicator pair (flag) is one grapheme of 8 bytes.
    constexpr std::string_view flag = "\xF0\x9F\x87\xAE\xF0\x9F\x87\xB3";  // IN
    EXPECT_EQ(flag.size(), 8u);
    EXPECT_EQ(NextGrapheme(flag, 0), 8u);
    EXPECT_EQ(PrevGrapheme(flag, 8), 0u);
  }

  TEST_CASE("grapheme steps clamp at the ends");
  EXPECT_EQ(PrevGrapheme("", 0), 0u);
  EXPECT_EQ(NextGrapheme("", 0), 0u);
  EXPECT_EQ(PrevGrapheme("abc", 0), 0u);
  EXPECT_EQ(NextGrapheme("abc", 3), 3u);
  EXPECT_EQ(NextGrapheme("abc", 99), 3u);  // past the end must not run away

  TEST_CASE("word movement");
  constexpr std::string_view line = "foo bar baz";
  EXPECT_EQ(NextWord(line, 0), 4u);   // start of "bar"
  EXPECT_EQ(NextWord(line, 4), 8u);   // start of "baz"
  EXPECT_EQ(NextWord(line, 8), 11u);  // end of line
  EXPECT_EQ(NextWord(line, 11), 11u);
  EXPECT_EQ(PrevWord(line, 11), 8u);
  EXPECT_EQ(PrevWord(line, 8), 4u);
  EXPECT_EQ(PrevWord(line, 4), 0u);
  EXPECT_EQ(PrevWord(line, 0), 0u);

  TEST_CASE("word movement across runs of spaces");
  constexpr std::string_view spaced = "a   b";
  EXPECT_EQ(NextWord(spaced, 0), 4u);
  EXPECT_EQ(PrevWord(spaced, 5), 4u);
  EXPECT_EQ(PrevWord(spaced, 4), 0u);
  EXPECT_EQ(NextWord("", 0), 0u);
  EXPECT_EQ(PrevWord("", 0), 0u);
  EXPECT_EQ(NextWord("   ", 0), 3u);
}

// ============================================================================
// text.h -- SGR parsing
// ============================================================================
static void TestAnsiParsing() {
  TEST_CASE("ParseAnsiSequence rejects non-CSI input");
  {
    AnsiColorState st{};
    EXPECT_EQ(ParseAnsi("hello", st), 0u);
    EXPECT_EQ(ParseAnsi("", st), 0u);
    EXPECT_EQ(ParseAnsi("\x1b", st), 0u);    // ESC with nothing after it
    EXPECT_EQ(ParseAnsi("\x1bX", st), 0u);   // ESC not followed by '['
  }

  TEST_CASE("ParseAnsiSequence consumed lengths");
  {
    AnsiColorState st{};
    EXPECT_EQ(ParseAnsi("\x1b[31m", st), 5u);
    EXPECT_EQ(ParseAnsi("\x1b[m", st), 3u);
    EXPECT_EQ(ParseAnsi("\x1b[01;31m", st), 8u);
    // A non-SGR sequence is still consumed, so a cursor move cannot leak through as
    // visible bytes.
    EXPECT_EQ(ParseAnsi("\x1b[2J", st), 4u);
    EXPECT_EQ(ParseAnsi("\x1b[10;20H", st), 8u);
    // Unterminated: consume what was scanned rather than looping.
    EXPECT_TRUE(ParseAnsi("\x1b[01;3", st) > 0u);
  }

  TEST_CASE("basic colours map into the palette");
  {
    AnsiColorState st{};
    ParseAnsi("\x1b[31m", st);
    EXPECT_EQ(st.fg, kAnsiColors[1]);
    ParseAnsi("\x1b[42m", st);
    EXPECT_EQ(st.bg, kAnsiColors[2]);
    ParseAnsi("\x1b[91m", st);
    EXPECT_EQ(st.fg, kAnsiBrightColors[1]);
    ParseAnsi("\x1b[102m", st);
    EXPECT_EQ(st.bg, kAnsiBrightColors[2]);
  }

  TEST_CASE("truecolor and 256-colour forms");
  {
    AnsiColorState st{};
    ParseAnsi("\x1b[38;2;18;52;86m", st);
    EXPECT_EQ(st.fg, 0x123456u);
    ParseAnsi("\x1b[48;2;255;255;255m", st);
    EXPECT_EQ(st.bg, 0xFFFFFFu);
    ParseAnsi("\x1b[38;5;1m", st);
    EXPECT_EQ(st.fg, kAnsiColors[1]);
    ParseAnsi("\x1b[38;5;231m", st);
    EXPECT_EQ(st.fg, Ansi256ToRgb(231));
  }

  TEST_CASE("styles set and clear");
  {
    AnsiColorState st{};
    ParseAnsi("\x1b[1m", st);
    EXPECT_TRUE((st.style & TB_BOLD) != 0);
    ParseAnsi("\x1b[22m", st);
    EXPECT_TRUE((st.style & TB_BOLD) == 0);
    ParseAnsi("\x1b[3m", st);
    ParseAnsi("\x1b[4m", st);
    ParseAnsi("\x1b[7m", st);
    EXPECT_TRUE((st.style & TB_ITALIC) != 0);
    EXPECT_TRUE((st.style & TB_UNDERLINE) != 0);
    EXPECT_TRUE((st.style & TB_REVERSE) != 0);
    ParseAnsi("\x1b[23m", st);
    ParseAnsi("\x1b[24m", st);
    ParseAnsi("\x1b[27m", st);
    EXPECT_EQ(st.style, 0u);
  }

  TEST_CASE("reset returns to the caller's defaults, not the terminal's");
  {
    // This is what keeps an item's own \x1b[0m from punching a hole in the selected
    // row: the row's colours are the defaults handed in here.
    AnsiColorState st{};
    ParseAnsi("\x1b[31m", st);
    ParseAnsi("\x1b[0m", st, 0xAABBCC, 0x112233);
    EXPECT_EQ(st.fg, 0xAABBCCu);
    EXPECT_EQ(st.bg, 0x112233u);
    EXPECT_EQ(st.style, 0u);
  }
  {
    // 39/49 reset only one channel each.
    AnsiColorState st{};
    ParseAnsi("\x1b[31;42m", st);
    ParseAnsi("\x1b[39m", st, 0xAABBCC, 0x112233);
    EXPECT_EQ(st.fg, 0xAABBCCu);
    EXPECT_EQ(st.bg, kAnsiColors[2]);
    ParseAnsi("\x1b[49m", st, 0xAABBCC, 0x112233);
    EXPECT_EQ(st.bg, 0x112233u);
  }

  TEST_CASE("Ansi256ToRgb");
  EXPECT_EQ(Ansi256ToRgb(0), kAnsiColors[0]);
  EXPECT_EQ(Ansi256ToRgb(7), kAnsiColors[7]);
  EXPECT_EQ(Ansi256ToRgb(8), kAnsiBrightColors[0]);
  EXPECT_EQ(Ansi256ToRgb(15), kAnsiBrightColors[7]);
  EXPECT_EQ(Ansi256ToRgb(16), 0x000000u);   // first cube entry
  EXPECT_EQ(Ansi256ToRgb(231), 0xFFFFFFu);  // last cube entry
  EXPECT_EQ(Ansi256ToRgb(232), 0x080808u);  // first grey
  EXPECT_EQ(Ansi256ToRgb(255), 0xEEEEEEu);  // last grey
  // The cube is R,G,B major order: index 16 + 36 is the first red step.
  EXPECT_EQ(Ansi256ToRgb(16 + 36), 0x5F0000u);
  EXPECT_EQ(Ansi256ToRgb(16 + 6), 0x005F00u);
  EXPECT_EQ(Ansi256ToRgb(16 + 1), 0x00005Fu);
}

// ============================================================================
// text.h -- shell quoting
// ============================================================================
static void TestShellEscape() {
  TEST_CASE("safe strings are left alone");
  EXPECT_EQ(Escaped("abc"), "abc");
  EXPECT_EQ(Escaped("a1-b_c.d/e"), "a1-b_c.d/e");
  EXPECT_EQ(Escaped("HEAD~1"), "'HEAD~1'");  // ~ is not on the safe list

  TEST_CASE("empty becomes an explicit empty argument");
  EXPECT_EQ(Escaped(""), "''");

  TEST_CASE("anything a shell would interpret gets quoted");
  EXPECT_EQ(Escaped("a b"), "'a b'");
  EXPECT_EQ(Escaped("a;b"), "'a;b'");
  EXPECT_EQ(Escaped("$HOME"), "'$HOME'");
  EXPECT_EQ(Escaped("a|b"), "'a|b'");
  EXPECT_EQ(Escaped("`cmd`"), "'`cmd`'");
  EXPECT_EQ(Escaped("a\nb"), "'a\nb'");
  EXPECT_EQ(Escaped("*"), "'*'");
  EXPECT_EQ(Escaped("a&b"), "'a&b'");
  EXPECT_EQ(Escaped("(x)"), "'(x)'");
  EXPECT_EQ(Escaped("a>b"), "'a>b'");

  TEST_CASE("single quotes use the '\"'\"' idiom");
  EXPECT_EQ(Escaped("'"), "''\"'\"''");
  EXPECT_EQ(Escaped("a'b"), "'a'\"'\"'b'");
  EXPECT_EQ(Escaped("''"), "''\"'\"''\"'\"''");
  // A quote-heavy string must not corrupt: the resulting size is predictable.
  EXPECT_EQ(Escaped("a'b'c").size(), std::string("'a'\"'\"'b'\"'\"'c'").size());

  TEST_CASE("utf-8 payloads survive quoting byte for byte");
  EXPECT_EQ(Escaped("caf\xC3\xA9 x"), "'caf\xC3\xA9 x'");
  EXPECT_EQ(Escaped("\xF0\x9F\x8E\x89"), "'\xF0\x9F\x8E\x89'");
}

// ============================================================================
// text.h -- placeholder substitution and record splitting
// ============================================================================
static void TestSubstituteAndSplit() {
  TEST_CASE("SubstituteInPlace");
  EXPECT_EQ(Substituted("{{@Q@}}", "hi", "echo {{@Q@}}"), "echo hi");
  EXPECT_EQ(Substituted("x", "y", "xxx"), "yyy");
  EXPECT_EQ(Substituted("x", "", "axbxc"), "abc");
  EXPECT_EQ(Substituted("zz", "y", "abc"), "abc");  // no match leaves it alone
  EXPECT_EQ(Substituted("", "y", "abc"), "abc");    // empty needle is a no-op
  EXPECT_EQ(Substituted("a", "b", ""), "");
  // The replacement is not rescanned, so this terminates rather than looping.
  EXPECT_EQ(Substituted("a", "aa", "a"), "aa");
  // Both placeholders appearing twice is the real command-line case.
  EXPECT_EQ(Substituted("{{@S@}}", "f", "cat {{@S@}} && wc {{@S@}}"), "cat f && wc f");

  TEST_CASE("AppendCompleteItems splits on the delimiter");
  {
    auto items = SplitComplete("a\nb\nc\n", '\n');
    EXPECT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0], "a");
    EXPECT_EQ(items[2], "c");
  }
  {
    // Empty records are skipped: a blank line is not an item.
    auto items = SplitComplete("a\n\nb\n", '\n');
    EXPECT_EQ(items.size(), 2u);
    EXPECT_EQ(items[1], "b");
  }
  {
    // NUL delimiter, which is what --read0 uses.
    auto items = SplitComplete(std::string_view("a\0b\0", 4), '\0');
    EXPECT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0], "a");
    // A newline inside a record is payload, not a separator.
    auto multi = SplitComplete(std::string_view("a\nb\0c\0", 6), '\0');
    EXPECT_EQ(multi.size(), 2u);
    EXPECT_EQ(multi[0], "a\nb");
  }

  TEST_CASE("the trailing incomplete record is reported, not emitted");
  {
    std::string_view tail;
    auto items = SplitComplete("a\nb\npartial", '\n', &tail);
    EXPECT_EQ(items.size(), 2u);
    EXPECT_EQ(tail, "partial");
  }
  {
    std::string_view tail;
    auto items = SplitComplete("a\nb\n", '\n', &tail);
    EXPECT_EQ(items.size(), 2u);
    EXPECT_TRUE(tail.empty());  // ended on a delimiter: nothing pending
  }
  {
    std::string_view tail;
    auto items = SplitComplete("", '\n', &tail);
    EXPECT_TRUE(items.empty());
    EXPECT_TRUE(tail.empty());
  }
  {
    // No delimiter at all: everything is still pending.
    std::string_view tail;
    auto items = SplitComplete("abc", '\n', &tail);
    EXPECT_TRUE(items.empty());
    EXPECT_EQ(tail, "abc");
  }

  TEST_CASE("ParseInt");
  EXPECT_EQ(ParseInt("40").value_or(-1), 40);
  EXPECT_EQ(ParseInt("0").value_or(-1), 0);
  EXPECT_EQ(ParseInt("-5").value_or(0), -5);
  // from_chars stops at the first non-digit, which is what makes "40%" work.
  EXPECT_EQ(ParseInt("40%").value_or(-1), 40);
  EXPECT_EQ(ParseInt("12abc").value_or(-1), 12);
  EXPECT_FALSE(ParseInt("").has_value());
  EXPECT_FALSE(ParseInt("abc").has_value());
  EXPECT_FALSE(ParseInt("%40").has_value());
}

// ============================================================================
// layout.h
// ============================================================================
static void TestLayout() {
  TEST_CASE("no preview command gives the list the whole screen");
  {
    Config cfg;
    const Layout l = CalculateLayout(cfg, 80, 24);
    EXPECT_FALSE(l.has_preview);
    EXPECT_EQ(l.main.w, 80);
    EXPECT_EQ(l.main.h, 24);
    EXPECT_EQ(l.main.x, 0);
    EXPECT_EQ(l.main.y, 0);
  }
  {
    // force_preview is how the action window previews a command with none configured.
    Config cfg;
    const Layout l = CalculateLayout(cfg, 80, 24, true);
    EXPECT_TRUE(l.has_preview);
  }

  TEST_CASE("preview split by direction");
  {
    Config cfg;
    cfg.preview_cmd = "cat";
    cfg.preview_size = 50;

    cfg.preview_dir = "right";
    Layout l = CalculateLayout(cfg, 80, 24);
    EXPECT_EQ(l.main.x, 0);
    EXPECT_EQ(l.main.w, 40);
    EXPECT_EQ(l.preview.x, 40);
    EXPECT_EQ(l.preview.w, 40);

    cfg.preview_dir = "left";
    l = CalculateLayout(cfg, 80, 24);
    EXPECT_EQ(l.preview.x, 0);
    EXPECT_EQ(l.preview.w, 40);
    EXPECT_EQ(l.main.x, 40);
    EXPECT_EQ(l.main.w, 40);

    cfg.preview_dir = "top";
    l = CalculateLayout(cfg, 80, 24);
    EXPECT_EQ(l.preview.y, 0);
    EXPECT_EQ(l.preview.h, 12);
    EXPECT_EQ(l.main.y, 12);
    EXPECT_EQ(l.main.h, 12);

    cfg.preview_dir = "bottom";
    l = CalculateLayout(cfg, 80, 24);
    EXPECT_EQ(l.main.y, 0);
    EXPECT_EQ(l.main.h, 12);
    EXPECT_EQ(l.preview.y, 12);
    EXPECT_EQ(l.preview.h, 12);

    // An unrecognised direction falls back to "right" rather than losing the pane.
    cfg.preview_dir = "sideways";
    l = CalculateLayout(cfg, 80, 24);
    EXPECT_EQ(l.preview.x, 40);
  }

  TEST_CASE("the two panes always tile the screen exactly");
  {
    Config cfg;
    cfg.preview_cmd = "cat";
    for (int size : {10, 25, 33, 50, 66, 75, 90}) {
      cfg.preview_size = size;
      for (std::string_view dir : {"right", "left"}) {
        cfg.preview_dir = dir;
        const Layout l = CalculateLayout(cfg, 80, 24);
        EXPECT_EQ(l.main.w + l.preview.w, 80);
        EXPECT_EQ(l.main.h, 24);
      }
      for (std::string_view dir : {"top", "bottom"}) {
        cfg.preview_dir = dir;
        const Layout l = CalculateLayout(cfg, 80, 24);
        EXPECT_EQ(l.main.h + l.preview.h, 24);
        EXPECT_EQ(l.main.w, 80);
      }
    }
  }

  TEST_CASE("out-of-range preview sizes fall back to the default");
  {
    Config cfg;
    cfg.preview_cmd = "cat";
    cfg.preview_dir = "right";
    for (int bad : {0, -10, 100, 1000}) {
      cfg.preview_size = bad;
      const Layout l = CalculateLayout(cfg, 80, 24);
      EXPECT_EQ(l.preview.w, 80 * kDefaultPreviewSize / 100);
    }
  }

  TEST_CASE("tiny terminals do not produce negative geometry");
  {
    Config cfg;
    cfg.preview_cmd = "cat";
    for (int w : {0, 1, 2, 3}) {
      for (int h : {0, 1, 2, 3}) {
        const Layout l = CalculateLayout(cfg, w, h);
        EXPECT_TRUE(l.main.w >= 0);
        EXPECT_TRUE(l.main.h >= 0);
        EXPECT_TRUE(l.preview.w >= 0);
        EXPECT_TRUE(l.preview.h >= 0);
      }
    }
  }

  TEST_CASE("SplitTextRows");
  {
    EXPECT_TRUE(SplitTextRows("").empty());

    auto rows = SplitTextRows("one");
    EXPECT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], "one");

    rows = SplitTextRows("one\ntwo\nthree");
    EXPECT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0], "one");
    EXPECT_EQ(rows[1], "two");
    EXPECT_EQ(rows[2], "three");

    // The form a caller writes first: double quotes keep the backslash, so the two
    // characters \n have to break the line as well. Surrounding spaces are the caller's.
    rows = SplitTextRows("ls \\n ls2");
    EXPECT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0], "ls ");
    EXPECT_EQ(rows[1], " ls2");

    // Both spellings in one string, and repeated breaks giving blank rows.
    rows = SplitTextRows("a\\nb\nc");
    EXPECT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[2], "c");

    rows = SplitTextRows("a\\n\\nb");
    EXPECT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[1], "");

    // A trailing break is a row too: it is a blank line the caller asked for.
    rows = SplitTextRows("one\n");
    EXPECT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[1], "");
    EXPECT_EQ(SplitTextRows("one\\n").size(), 2u);
    EXPECT_EQ(SplitTextRows("\n").size(), 2u);

    // A trailing lone backslash is text, not half a break, and must not read past the end.
    rows = SplitTextRows("one\\");
    EXPECT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], "one\\");
    // Other escapes are left alone; only \n means anything here.
    rows = SplitTextRows("a\\tb");
    EXPECT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], "a\\tb");

    // Rows are views into the argument, so nothing is copied and nothing dangles.
    constexpr std::string_view src{"alpha\\nbeta"};
    rows = SplitTextRows(src);
    EXPECT_TRUE(rows[0].data() == src.data());
    EXPECT_TRUE(rows[1].data() == src.data() + 7);
  }

  TEST_CASE("SplitListPane places the strips below the chrome");
  {
    constexpr Rect main{0, 0, 80, 24};

    // No header or footer: every row below the chrome belongs to the items.
    ListStrips s = SplitListPane(main, 0, 0);
    EXPECT_EQ(s.items.y, kChromeRows);
    EXPECT_EQ(s.items.h, 24 - kChromeRows);
    EXPECT_EQ(s.header.h, 0);
    EXPECT_EQ(s.footer.h, 0);

    // The header sits directly on top of the list and pushes it down; the footer takes
    // the bottom rows and shortens it. Together they still tile the pane exactly.
    s = SplitListPane(main, 2, 1);
    EXPECT_EQ(s.header.y, kChromeRows);
    EXPECT_EQ(s.header.h, 2);
    EXPECT_EQ(s.items.y, kChromeRows + 2);
    EXPECT_EQ(s.items.h, 24 - kChromeRows - 3);
    EXPECT_EQ(s.footer.y, 23);
    EXPECT_EQ(s.footer.h, 1);
    EXPECT_EQ(s.items.y + s.items.h, s.footer.y);

    // An offset pane (preview on top, or the list on the left) carries the strips with it.
    s = SplitListPane(Rect{40, 12, 40, 12}, 1, 1);
    EXPECT_EQ(s.header.x, 40);
    EXPECT_EQ(s.header.y, 12 + kChromeRows);
    EXPECT_EQ(s.items.h, 12 - kChromeRows - 2);
    EXPECT_EQ(s.footer.y, 23);
  }

  TEST_CASE("SplitListPane never trades away the last item row");
  {
    // kMinInlineRows is all the chrome: --height can leave nothing to give.
    ListStrips s = SplitListPane(Rect{0, 0, 80, kMinInlineRows}, 1, 1);
    EXPECT_EQ(s.items.h, 0);
    EXPECT_EQ(s.header.h, 0);
    EXPECT_EQ(s.footer.h, 0);

    // One row to spare: the header takes it only if an item row would survive, which it
    // would not, so the list keeps it.
    s = SplitListPane(Rect{0, 0, 80, kChromeRows + 1}, 1, 1);
    EXPECT_EQ(s.items.h, 1);
    EXPECT_EQ(s.header.h, 0);
    EXPECT_EQ(s.footer.h, 0);

    // Two to spare: the header fits, the footer would empty the list, so it is dropped.
    s = SplitListPane(Rect{0, 0, 80, kChromeRows + 2}, 1, 1);
    EXPECT_EQ(s.header.h, 1);
    EXPECT_EQ(s.items.h, 1);
    EXPECT_EQ(s.footer.h, 0);

    // Strips are all-or-nothing: a 3-row header that does not fit is not half-drawn.
    s = SplitListPane(Rect{0, 0, 80, kChromeRows + 3}, 3, 0);
    EXPECT_EQ(s.header.h, 0);
    EXPECT_EQ(s.items.h, 3);

    // A pane with no room at all, and a degenerate one, stay non-negative.
    for (int h : {0, 1, 2, 3}) {
      s = SplitListPane(Rect{0, 0, 80, h}, 2, 2);
      EXPECT_TRUE(s.items.h >= 0);
      EXPECT_TRUE(s.header.h >= 0);
      EXPECT_TRUE(s.footer.h >= 0);
    }
  }

  TEST_CASE("StepWrapped");
  EXPECT_EQ(StepWrapped(0, 3, true), 1u);
  EXPECT_EQ(StepWrapped(1, 3, true), 2u);
  EXPECT_EQ(StepWrapped(2, 3, true), 0u);   // wraps forward
  EXPECT_EQ(StepWrapped(0, 3, false), 2u);  // wraps backward
  EXPECT_EQ(StepWrapped(2, 3, false), 1u);
  EXPECT_EQ(StepWrapped(0, 1, true), 0u);
  EXPECT_EQ(StepWrapped(0, 1, false), 0u);
  // An empty list must not compute count-1 and wrap to SIZE_MAX.
  EXPECT_EQ(StepWrapped(0, 0, true), 0u);
  EXPECT_EQ(StepWrapped(0, 0, false), 0u);
  EXPECT_EQ(StepWrapped(5, 0, false), 0u);

  TEST_CASE("IsInside");
  {
    constexpr Rect r{10, 5, 20, 10};  // x 10..29, y 5..14
    EXPECT_TRUE(IsInside(r, 10, 5));
    EXPECT_TRUE(IsInside(r, 29, 14));
    EXPECT_FALSE(IsInside(r, 9, 5));
    EXPECT_FALSE(IsInside(r, 30, 5));
    EXPECT_FALSE(IsInside(r, 10, 4));
    EXPECT_FALSE(IsInside(r, 10, 15));
    // A zero-size rect contains nothing, so a click cannot land in a hidden pane.
    constexpr Rect empty{0, 0, 0, 0};
    EXPECT_FALSE(IsInside(empty, 0, 0));
    static_assert(IsInside(Rect{0, 0, 1, 1}, 0, 0));
    static_assert(!IsInside(Rect{0, 0, 1, 1}, 1, 0));
  }

  TEST_CASE("GetMaxPreviewScroll");
  {
    Config cfg;
    cfg.preview_cmd = "cat";
    cfg.preview_dir = "right";
    const Layout l = CalculateLayout(cfg, 80, 24);  // preview.h == 24
    EXPECT_EQ(GetMaxPreviewScroll(l, cfg, 0), 0u);
    EXPECT_EQ(GetMaxPreviewScroll(l, cfg, 10), 0u);  // fits, nothing to scroll
    EXPECT_EQ(GetMaxPreviewScroll(l, cfg, 24), 0u);  // exactly fits
    EXPECT_EQ(GetMaxPreviewScroll(l, cfg, 30), 6u);

    // top/bottom lose a row to the divider, so the ceiling is one higher.
    cfg.preview_dir = "bottom";
    const Layout lb = CalculateLayout(cfg, 80, 24);  // preview.h == 12
    EXPECT_EQ(GetMaxPreviewScroll(lb, cfg, 30), 30u - 11u);

    // No preview means no scrolling regardless of content.
    Config plain;
    const Layout lp = CalculateLayout(plain, 80, 24);
    EXPECT_EQ(GetMaxPreviewScroll(lp, plain, 1000), 0u);
  }

  TEST_CASE("RowsFromPercent");
  EXPECT_EQ(RowsFromPercent("40", 100), 40);
  EXPECT_EQ(RowsFromPercent("40%", 100), 40);
  EXPECT_EQ(RowsFromPercent("100", 50), 50);
  // Clamped at 100%, so an absurd percentage cannot exceed the terminal.
  EXPECT_EQ(RowsFromPercent("500", 50), 50);
  // Never below the floor: a prompt, a counter and one item.
  EXPECT_EQ(RowsFromPercent("1", 24), kMinInlineRows);
  EXPECT_EQ(RowsFromPercent("10", 24), kMinInlineRows);
  EXPECT_EQ(RowsFromPercent("40", 23), 9);
  EXPECT_EQ(RowsFromPercent("40", 38), 15);
  EXPECT_EQ(RowsFromPercent("40", 62), 24);
  // 0 means "stay fullscreen": unparseable, non-positive, or unknown terminal size.
  EXPECT_EQ(RowsFromPercent("", 24), 0);
  EXPECT_EQ(RowsFromPercent("abc", 24), 0);
  EXPECT_EQ(RowsFromPercent("0", 24), 0);
  EXPECT_EQ(RowsFromPercent("-40", 24), 0);
  EXPECT_EQ(RowsFromPercent("40", 0), 0);
  EXPECT_EQ(RowsFromPercent("40", -1), 0);
}

// ============================================================================
// layout.h -- action specs
// ============================================================================
static void TestParseActions() {
  TEST_CASE("name=command");
  {
    const auto acts = ParseActions({"Edit=vim {}"});
    EXPECT_EQ(acts.size(), 1u);
    EXPECT_EQ(acts[0].name, "Edit");
    EXPECT_EQ(acts[0].command, "vim {}");
    EXPECT_EQ(acts[0].key, 0u);
    EXPECT_FALSE(acts[0].is_become);
  }

  TEST_CASE("== marks a become action");
  {
    const auto acts = ParseActions({"Shell==bash"});
    EXPECT_EQ(acts.size(), 1u);
    EXPECT_EQ(acts[0].name, "Shell");
    EXPECT_EQ(acts[0].command, "bash");
    EXPECT_TRUE(acts[0].is_become);
  }

  TEST_CASE("alt- key binding");
  {
    const auto acts = ParseActions({"alt-t:Toggle=echo hi"});
    EXPECT_EQ(acts.size(), 1u);
    EXPECT_EQ(acts[0].key, static_cast<uint32_t>('t'));
    EXPECT_EQ(acts[0].name, "Toggle");
    EXPECT_EQ(acts[0].command, "echo hi");
  }
  {
    // Uppercase is the shifted key, because that is what the terminal sends.
    const auto acts = ParseActions({"alt-T:Toggle=echo hi"});
    EXPECT_EQ(acts[0].key, static_cast<uint32_t>('T'));
  }
  {
    // Binding plus become together, which is what ghatothkacha uses.
    const auto acts = ParseActions({"alt-t:Toggle==_ui next {{@QUERY@}}"});
    EXPECT_EQ(acts.size(), 1u);
    EXPECT_EQ(acts[0].key, static_cast<uint32_t>('t'));
    EXPECT_TRUE(acts[0].is_become);
    EXPECT_EQ(acts[0].command, "_ui next {{@QUERY@}}");
  }

  TEST_CASE("malformed specs are skipped, not fatal");
  {
    EXPECT_TRUE(ParseActions({"no-equals-sign"}).empty());
    EXPECT_TRUE(ParseActions({""}).empty());
    EXPECT_TRUE(ParseActions({}).empty());
    // A good spec still lands when a bad one precedes it.
    const auto acts = ParseActions({"bad", "Good=ls"});
    EXPECT_EQ(acts.size(), 1u);
    EXPECT_EQ(acts[0].name, "Good");
  }

  TEST_CASE("edge shapes");
  {
    // Empty name is accepted; the command is what matters.
    const auto empty_name = ParseActions({"=ls"});
    EXPECT_EQ(empty_name.size(), 1u);
    EXPECT_TRUE(empty_name[0].name.empty());
    // Empty command.
    const auto empty_cmd = ParseActions({"Name="});
    EXPECT_EQ(empty_cmd.size(), 1u);
    EXPECT_TRUE(empty_cmd[0].command.empty());
    // "alt-" without the colon is a name, not a binding.
    const auto not_a_key = ParseActions({"alt-xyz=ls"});
    EXPECT_EQ(not_a_key.size(), 1u);
    EXPECT_EQ(not_a_key[0].key, 0u);
    EXPECT_EQ(not_a_key[0].name, "alt-xyz");
    // An = inside the command must not be mistaken for the separator.
    const auto eq_in_cmd = ParseActions({"Set=export A=1"});
    EXPECT_EQ(eq_in_cmd[0].name, "Set");
    EXPECT_EQ(eq_in_cmd[0].command, "export A=1");
  }

  TEST_CASE("several actions keep their order");
  {
    const auto acts = ParseActions({"alt-a:A=1", "B=2", "alt-c:C==3"});
    EXPECT_EQ(acts.size(), 3u);
    EXPECT_EQ(acts[0].name, "A");
    EXPECT_EQ(acts[1].name, "B");
    EXPECT_EQ(acts[2].name, "C");
    EXPECT_TRUE(acts[2].is_become);
  }
}

int main() {
  TestDecodeAndWidth();
  TestGraphemeMovement();
  TestAnsiParsing();
  TestShellEscape();
  TestSubstituteAndSplit();
  TestLayout();
  TestParseActions();
  return common::TestSummary();
}
