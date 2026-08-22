// Tests for leap: commands.cpp runs the state machine, render.cpp paints the
// matches and labels, and editor.cpp holds what a pick turns into -- so the
// whole feature is tested here.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

namespace {

// A pane with a whole document in it, sized by hand: a leap reads the viewport
// rather than the pane rectangle, so this is the same pane the renderer would
// draw for the same numbers.
void LeapDocument(Editor& ed, std::string text, Index rows = 30, Index columns = 90) {
  ResetToOriginal(ed.doc.table, std::move(text));
  ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
  ed.doc.view.top_line = 0;
  ed.doc.view.top_row = 0;
  ed.doc.view.left_column = 0;
  ed.doc.view.rows = rows;
  ed.doc.view.columns = columns;
}

void PlaceCaret(Editor& ed, Index line, Index column) {
  const Index at = ByteForColumn(ed.doc.table, line, column, ed.doc.tab_width);
  ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
}

// Indexing that reports rather than crashes: a leap that has gone wrong leaves
// these empty, and a segfault halfway through the suite says less about what
// broke than a failed check does.
Index MatchAt(const Editor& ed, std::size_t i) {
  return (i < ed.leap.matches.size()) ? ed.leap.matches[i] : Index{-1};
}

Interval SpanAt(const Editor& ed, std::size_t i) {
  return (i < ed.leap.spans.size()) ? ed.leap.spans[i] : Interval(-1, -1);
}

}  // namespace

void LeapJumpsToATypedPair() {
  Presser press;

  TEST_CASE("leap: `y` arms it, and a pair only one place answers to jumps straight there");
  {
    const Scratch scratch{"koi-leap-jump"};
    // A jump is only recorded for a path the store would keep, and a fixture
    // under the system temp directory is one only when it is the project.
    const AsProjectRoot root{scratch.dir};
    // Twelve lines of filler between the caret and the pair, and no `r` in any
    // of them. The distance is what makes this a jump to somewhere else: a
    // record within kLocationMergeLines of the place it left merges onto that
    // row instead of taking one of its own, and then there is nothing behind
    // the cursor to step back to.
    std::string text = "alpha\n";
    for (int i = 0; i < 12; ++i) text += "one two\n";
    text += "bravo\ncharlie\n";
    const auto pair_at = static_cast<Index>(text.find("bravo")) + 1;
    const std::filesystem::path file = scratch.Write("a.txt", text);
    std::string error;

    Editor ed;
    ed.jumps = OpenJumpStore(scratch.dir / "jumps.db", "pane-leap", error);
    EXPECT_TRUE(ed.jumps != nullptr);
    LeapDocument(ed, text);
    ed.doc.file = std::filesystem::weakly_canonical(file);
    PlaceCaret(ed, 0, 0);

    // The default keymap's binding, not the command name: `y` has to reach it.
    press(ed, "y");
    EXPECT_TRUE(ed.pending_char == PendingChar::kLeapFirst);
    EXPECT_TRUE(ed.status.find("two characters") != std::string::npos);

    press(ed, "r");
    EXPECT_TRUE(ed.pending_char == PendingChar::kLeapSecond);
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kSecond);
    // Both visible r's are lit while the second character is awaited.
    EXPECT_EQ(std::ssize(ed.leap.spans), Index{2});

    // "ra" is in bravo and nowhere else -- charlie has "rl".
    press(ed, "a");
    EXPECT_TRUE(ed.pending_char == PendingChar::kNone);
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(Cur(ed), pair_at);
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{13});
    // The caret lands on the pair's first grapheme, and one cursor holds it.
    EXPECT_EQ(ed.doc.selections.Size(), std::size_t{1});

    // And ctrl-o comes back, which means the jump was recorded before it moved.
    RunCommands(ed, {"jump_backward"});
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{0});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: a pair nothing answers to warns and disarms rather than waiting");
  {
    Editor ed;
    LeapDocument(ed, "alpha\nbravo\ncharlie\n");
    PlaceCaret(ed, 0, 0);
    const Index was = Cur(ed);

    press(ed, "y");
    press(ed, "z");
    // Nothing is lit either: the first character is not there to be found.
    EXPECT_TRUE(ed.leap.spans.empty());
    press(ed, "z");
    EXPECT_TRUE(ed.pending_char == PendingChar::kNone);
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_TRUE(ed.status.find("no match") != std::string::npos);
    EXPECT_TRUE(ed.status.level() == StatusLevel::kWarning);
    EXPECT_EQ(Cur(ed), was);

    // The key after it is a key again.
    press(ed, "k");
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{1});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: overlapping pairs count at every start");
  {
    // "aa" over "aaaa" is a match at 0, 1 and 2 -- not two disjoint ones.
    Editor ed;
    LeapDocument(ed, "aaaa\nzz\n");
    PlaceCaret(ed, 0, 0);

    press(ed, "y");
    press(ed, "a");
    press(ed, "a");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_EQ(std::ssize(ed.leap.matches), Index{3});
    // Nearest first, from a caret at 0.
    EXPECT_EQ(MatchAt(ed, 0), Index{0});
    EXPECT_EQ(MatchAt(ed, 1), Index{1});
    EXPECT_EQ(MatchAt(ed, 2), Index{2});
    // Three overlapping pairs cover one run of four, and the tinted spans are
    // merged into it rather than left overlapping.
    EXPECT_EQ(std::ssize(ed.leap.spans), Index{1});
    EXPECT_EQ(SpanAt(ed, 0).front(), Index{0});
    EXPECT_EQ(SpanAt(ed, 0).back(), Index{3});

    press(ed, "d");
    EXPECT_EQ(Cur(ed), Index{2});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: what the pane does not show is neither lit, labelled nor jumped to");
  {
    // The pair on line 0 and again on line 40, with ten rows of pane.
    std::string text = "xy\n";
    for (int i = 0; i < 39; ++i) text += "....\n";
    text += "xy\n";
    for (int i = 0; i < 10; ++i) text += "....\n";

    Editor ed;
    LeapDocument(ed, text, 10, 90);
    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "x");
    EXPECT_EQ(std::ssize(ed.leap.spans), Index{1});
    press(ed, "y");
    // One match, so it jumped -- the occurrence forty lines down was never a
    // candidate to label.
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{0});

    // Scrolled down, the same query finds the other one and only the other one.
    Editor below;
    LeapDocument(below, text, 10, 90);
    below.doc.view.top_line = 38;
    PlaceCaret(below, 38, 0);
    press(below, "y");
    press(below, "x");
    EXPECT_EQ(std::ssize(below.leap.spans), Index{1});
    press(below, "y");
    EXPECT_EQ(LineAt(below.doc.table, Cur(below)), Index{40});
    EXPECT_EQ(EditorInvariants(below), std::string{});
  }

  TEST_CASE("leap: the columns are a window too");
  {
    // One long line, scrolled right: the pair at column 0 is off the pane.
    Editor ed;
    LeapDocument(ed, "xy" + std::string(100, '.') + "xy\n", 10, 20);
    ed.doc.view.left_column = 60;
    PlaceCaret(ed, 0, 60);
    press(ed, "y");
    press(ed, "x");
    EXPECT_TRUE(ed.leap.spans.empty());
    press(ed, "y");
    EXPECT_TRUE(ed.status.find("no match") != std::string::npos);

    ed.doc.view.left_column = 95;
    PlaceCaret(ed, 0, 95);
    press(ed, "y");
    press(ed, "x");
    press(ed, "y");
    EXPECT_EQ(Cur(ed), Index{102});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: a pair is matched by grapheme, not by the bytes of its first character");
  {
    // Two CJK graphemes, three bytes each: a byte-wise search would find the
    // pair at every byte of it, and a jump would land inside a cluster.
    const std::string one{kCJK};
    const std::string two = "\xE6\x9C\xAC";
    Editor ed;
    LeapDocument(ed, "aaa\n" + one + two + "tail\n");
    PlaceCaret(ed, 0, 0);

    press(ed, "y");
    press(ed, "\xE6\x97\xA5");
    EXPECT_EQ(std::ssize(ed.leap.spans), Index{1});
    press(ed, "\xE6\x9C\xAC");
    EXPECT_EQ(Cur(ed), Index{4});
    EXPECT_TRUE(IsGraphemeBoundary(ed.doc.table, Cur(ed)));
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: a buffer swapped under an armed leap is not jumped around");
  {
    Editor ed;
    LeapDocument(ed, "aaaa\naaaa\n");
    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "a");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_TRUE(LeapIsLive(ed));
    const Index was = Cur(ed);

    // What a finished background job does between two keystrokes. The labels
    // on the screen are byte positions in text that is no longer there.
    ResetToOriginal(ed.doc.table, "wholly different\n");
    EXPECT_TRUE(!LeapIsLive(ed));
    press(ed, "s");
    EXPECT_TRUE(ed.pending_char == PendingChar::kNone);
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_TRUE(ed.status.find("the text changed") != std::string::npos);
    EXPECT_EQ(Cur(ed), was);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void LeapLabelsEveryTargetItOffers() {
  Presser press;

  // Line 0 is "ab" and line 4 is "ab", with the caret between them but nearer
  // the second: distance decides the labels, so the layout has to pin it.
  const std::string kText = "ab..\n....\n..ab\n....\nab..\n";

  TEST_CASE("leap: more than one target is labelled nearest-first");
  {
    Editor ed;
    LeapDocument(ed, kText);
    // Byte 12 is the '.' at the start of line 2 -- one before that line's "ab"
    // at 12+2 = 14, five before line 0's at 0, and twelve before line 4's at 20.
    PlaceCaret(ed, 2, 0);
    EXPECT_EQ(Cur(ed), Index{10});

    press(ed, "y");
    press(ed, "a");
    // Every visible "a", including the one that is not part of an "ab" pair.
    EXPECT_EQ(std::ssize(ed.leap.spans), Index{3});
    press(ed, "b");

    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_TRUE(ed.pending_char == PendingChar::kLeapLabel);
    EXPECT_EQ(std::ssize(ed.leap.matches), Index{3});
    // 12 is two away, 0 is ten away, 20 is ten away -- and equal distances take
    // the earlier position.
    EXPECT_EQ(MatchAt(ed, 0), Index{12});
    EXPECT_EQ(MatchAt(ed, 1), Index{0});
    EXPECT_EQ(MatchAt(ed, 2), Index{20});
    EXPECT_EQ(LeapLabelAt(ed.leap, 12), 'a');
    EXPECT_EQ(LeapLabelAt(ed.leap, 0), 's');
    EXPECT_EQ(LeapLabelAt(ed.leap, 20), 'd');
    EXPECT_EQ(LeapLabelAt(ed.leap, 13), char{0});

    press(ed, "d");
    EXPECT_EQ(Cur(ed), Index{20});
    EXPECT_TRUE(ed.pending_char == PendingChar::kNone);
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: enter takes the nearest target");
  {
    Editor ed;
    LeapDocument(ed, kText);
    PlaceCaret(ed, 2, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    press(ed, "ret");
    EXPECT_EQ(Cur(ed), Index{12});
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: esc leaves the caret exactly where it was");
  {
    Editor ed;
    LeapDocument(ed, kText);
    PlaceCaret(ed, 2, 0);
    const Index was = Cur(ed);
    for (const std::string_view stage : {"first", "second", "label"}) {
      press(ed, "y");
      if (stage != std::string_view{"first"}) press(ed, "a");
      if (stage == std::string_view{"label"}) press(ed, "b");
      press(ed, "esc");
      EXPECT_TRUE(ed.pending_char == PendingChar::kNone);
      EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
      EXPECT_EQ(Cur(ed), was);
    }
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: a key that is not a label cancels quietly instead of doing what it means");
  {
    Editor ed;
    LeapDocument(ed, kText);
    PlaceCaret(ed, 2, 0);
    const Index was = Cur(ed);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    // `1` is not in the label alphabet. It is not a count either -- the leap
    // swallowed it.
    press(ed, "1");
    EXPECT_TRUE(ed.pending_char == PendingChar::kNone);
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(Cur(ed), was);
    EXPECT_EQ(ed.pending_count, Index{0});
    EXPECT_TRUE(ed.status.level() == StatusLevel::kInfo);

    // A named key ends it the same way, and does not also move the caret.
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    press(ed, "left");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(Cur(ed), was);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: more targets than labels are offered a group at a time");
  {
    // Sixty pairs, against twenty-five label keys.
    std::string text;
    for (int i = 0; i < 60; ++i) text += "ab..\n";
    Editor ed;
    LeapDocument(ed, text, 60, 90);
    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    EXPECT_EQ(std::ssize(ed.leap.matches), Index{60});
    const std::vector<Index> order = ed.leap.matches;
    EXPECT_EQ(ed.leap.labels.size(), kLeapKeys.size());
    EXPECT_TRUE(ed.status.find("space labels the other 35") != std::string::npos);
    EXPECT_EQ(LeapLabelAt(ed.leap, 0), 'a');
    EXPECT_EQ(LeapLabelAt(ed.leap, MatchAt(ed, kLeapKeys.size())), char{0});

    press(ed, "space");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_EQ(ed.leap.page, std::size_t{1});
    EXPECT_EQ(LeapLabelAt(ed.leap, MatchAt(ed, kLeapKeys.size())), 'a');
    EXPECT_EQ(LeapLabelAt(ed.leap, 0), char{0});

    // The third group is the short one, and the fourth press comes home.
    press(ed, "space");
    EXPECT_EQ(ed.leap.labels.size(), std::size_t{10});
    press(ed, "space");
    EXPECT_EQ(ed.leap.page, std::size_t{0});
    EXPECT_EQ(LeapLabelAt(ed.leap, 0), 'a');

    // And a label from the group on the screen takes the match it names, which
    // is the one at that offset into the whole order rather than into the page.
    press(ed, "space");
    press(ed, "a");
    EXPECT_EQ(Cur(ed), (kLeapKeys.size() < order.size()) ? order[kLeapKeys.size()] : Index{-1});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void LeapPaintsItsMatchesAndLabels() {
  Presser press;

  TEST_CASE("leap: the first character lights every occurrence the pane shows");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    LeapDocument(ed, "abab\nzzzz\nabab\n", 6, 40);
    PlaceCaret(ed, 0, 0);

    constexpr int kWidth = 40;
    constexpr int kHeight = 8;
    FitFocusedViewport(ed, kWidth, kHeight);
    const int gutter = GutterWidth(ed, kWidth);

    Surface plain;
    RenderTo(ed, plain, kWidth, kHeight);

    press(ed, "y");
    press(ed, "a");
    Surface lit;
    RenderTo(ed, lit, kWidth, kHeight);

    const Style match = ed.theme.Get("ui.cursor.match");
    EXPECT_TRUE(match.bg.set);
    const auto tinted = [&](const Surface& frame, int column, int row) {
      return frame.At(gutter + column, row).bg == static_cast<Attr>(match.bg.rgb);
    };
    // Columns 0 and 2 of lines 0 and 2 are the a's, and nothing else moved.
    for (const int row : {0, 2}) {
      for (const int column : {0, 2}) {
        EXPECT_TRUE(tinted(lit, column, row));
        EXPECT_TRUE(!tinted(plain, column, row));
      }
      EXPECT_TRUE(!tinted(lit, 1, row));
    }
    EXPECT_TRUE(!tinted(lit, 0, 1));
    // The text is still the text: this stage paints colour, not glyphs.
    EXPECT_EQ(lit.Row(0), plain.Row(0));
  }

  TEST_CASE("leap: a label replaces one cell and wears the jump-label style");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    LeapDocument(ed, "abab\nzzzz\nabab\n", 6, 40);
    PlaceCaret(ed, 1, 0);

    constexpr int kWidth = 40;
    constexpr int kHeight = 8;
    FitFocusedViewport(ed, kWidth, kHeight);
    const int gutter = GutterWidth(ed, kWidth);

    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    EXPECT_EQ(std::ssize(ed.leap.matches), Index{4});

    Surface frame;
    RenderTo(ed, frame, kWidth, kHeight);

    const Style label = ed.theme.Get("ui.virtual.jump-label");
    EXPECT_TRUE(label.bg.set);
    for (const LeapLabel& one : ed.leap.labels) {
      const Index line = LineAt(ed.doc.table, one.at);
      const int x = gutter + static_cast<int>(one.at - LineStart(ed.doc.table, line));
      const int y = static_cast<int>(line - ed.doc.view.top_line);
      EXPECT_EQ(frame.At(x, y).text, std::string(1, one.key));
      EXPECT_EQ(frame.At(x, y).bg, static_cast<Attr>(label.bg.rgb));
      // The pair's second character stays where it is, lit but readable.
      EXPECT_EQ(frame.At(x + 1, y).text, std::string{"b"});
    }
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: a label over a double-width cluster leaves no half glyph behind");
  {
    // Every cell of a CJK line, so a label always lands on a cluster that is
    // two columns wide and one of them has to be blanked -- tb_present walks
    // the buffer as `x += wcwidth(cell)`, so a trailing half left with the
    // cluster's own text would print a lone glyph through the label.
    const std::string one{kCJK};
    const std::string two = "\xE6\x9C\xAC";
    std::string text;
    for (int i = 0; i < 6; ++i) text += one + two + one + two + "\n";

    Editor ed;
    ed.theme = BuiltinTheme();
    LeapDocument(ed, text, 6, 40);
    PlaceCaret(ed, 0, 0);

    constexpr int kWidth = 40;
    constexpr int kHeight = 8;
    FitFocusedViewport(ed, kWidth, kHeight);
    const int gutter = GutterWidth(ed, kWidth);

    press(ed, "y");
    press(ed, one);
    press(ed, two);
    EXPECT_TRUE(std::ssize(ed.leap.matches) >= 8);

    Surface frame;
    RenderTo(ed, frame, kWidth, kHeight);

    // tb_present's own walk: a cell it steps over never reaches the terminal.
    const auto arrives = [&](int x, int y) {
      int at = 0;
      while (at < x) at += std::max(1, GraphemeWidth(frame.At(at, y).text));
      return at == x;
    };
    int seen = 0;
    for (const LeapLabel& label : ed.leap.labels) {
      const Index line = LineAt(ed.doc.table, label.at);
      const int y = static_cast<int>(line - ed.doc.view.top_line);
      const int x = gutter + static_cast<int>(ColumnForByteFrom(
                                 ed.doc.table, LineStart(ed.doc.table, line), label.at,
                                 ed.doc.tab_width));
      EXPECT_EQ(frame.At(x, y).text, std::string(1, label.key));
      // One column wide, and the other half of the cluster it stands on is
      // blank rather than a stray copy of the glyph.
      EXPECT_EQ(GraphemeWidth(frame.At(x, y).text), 1);
      EXPECT_EQ(frame.At(x + 1, y).text, std::string{" "});
      EXPECT_TRUE(arrives(x, y));
      // The label cost the terminal nothing downstream: the second half of the
      // pair still starts where it did, on the very next column.
      EXPECT_EQ(frame.At(x + 2, y).text, two);
      ++seen;
    }
    EXPECT_TRUE(seen > 0);
    EXPECT_EQ(seen, static_cast<int>(ed.leap.labels.size()));
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: the overlay is the focused pane's, and only while it describes it");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    LeapDocument(ed, "abab\nzzzz\nabab\n", 6, 40);
    PlaceCaret(ed, 1, 0);
    constexpr int kWidth = 40;
    constexpr int kHeight = 8;
    FitFocusedViewport(ed, kWidth, kHeight);

    Surface plain;
    RenderTo(ed, plain, kWidth, kHeight);

    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    Surface labelled;
    RenderTo(ed, labelled, kWidth, kHeight);
    EXPECT_TRUE(labelled.Row(0) != plain.Row(0));

    // The same frame once the text underneath has been replaced. The label
    // positions are bytes of a document that is no longer there, so the frame
    // goes back to being the one with no leap in it rather than painting keys
    // over text they were never measured in.
    ResetToOriginal(ed.doc.table, "abab\nzzzz\nabab\n");
    PlaceCaret(ed, 1, 0);
    ed.status.clear();
    Surface stale;
    RenderTo(ed, stale, kWidth, kHeight);
    // Text rows only -- the status line is carrying the leap's own hint.
    for (int y = 0; y < (kHeight - 1); ++y) EXPECT_EQ(stale.Row(y), plain.Row(y));
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void LeapAlwaysLandsOnThePairItOffered(Rng& rng) {
  TEST_CASE("leap: over random documents every label offered lands on the pair");

  Presser press;
  static constexpr std::string_view kAlphabet = "ab\t ";
  const std::string cjk{kCJK};

  int warned = 0;
  int jumped = 0;
  int labelled = 0;
  for (int round = 0; round < 120; ++round) {
    std::string text;
    const Index lines = rng.Pick(1, 8);
    for (Index line = 0; line < lines; ++line) {
      const Index width = rng.Pick(0, 10);
      for (Index i = 0; i < width; ++i) {
        const Index which = rng.Pick(0, std::ssize(kAlphabet));
        if (which == std::ssize(kAlphabet)) {
          text += cjk;
        } else {
          text += kAlphabet[static_cast<std::size_t>(which)];
        }
      }
      text += '\n';
    }

    Editor ed;
    LeapDocument(ed, text, rng.Pick(1, 8), rng.Pick(4, 20));
    ed.settings.soft_wrap = (rng.Pick(0, 1) == 1);
    ed.doc.view.top_line = rng.Pick(0, std::max<Index>(0, LineCount(ed.doc.table) - 1));
    const Index start = SnapToGraphemeBoundary(
        ed.doc.table, rng.Pick(0, std::max<Index>(0, DocLength(ed.doc.table) - 1)));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{start, start, -1}));

    const auto pick = [&](Index at) {
      const Index next = NextGraphemeBoundary(ed.doc.table, at);
      return ReadDocRange(ed.doc.table, Interval(at, std::max(next, at + 1)));
    };
    const std::string first = pick(SnapToGraphemeBoundary(
        ed.doc.table, rng.Pick(0, std::max<Index>(0, DocLength(ed.doc.table) - 1))));
    const std::string second = pick(SnapToGraphemeBoundary(
        ed.doc.table, rng.Pick(0, std::max<Index>(0, DocLength(ed.doc.table) - 1))));
    if (first.empty() || second.empty()) continue;
    if ((first == "\n") || (second == "\n")) continue;

    const std::string before = AssembleDocContents(ed.doc.table);
    const Index was = Cur(ed);
    RunCommands(ed, {"leap"});
    ApplyPendingChar(ed, first);
    ApplyPendingChar(ed, second);

    if (ed.leap.stage == LeapState::Stage::kOff) {
      // Either nothing matched, or exactly one did and it jumped.
      if (ed.status.find("no match") != std::string::npos) {
        ++warned;
        EXPECT_EQ(Cur(ed), was);
      } else {
        ++jumped;
        EXPECT_EQ(ReadDocRange(ed.doc.table,
                               Interval(Cur(ed), Cur(ed) + std::ssize(first) +
                                                     std::ssize(second))),
                  first + second);
      }
      EXPECT_EQ(EditorInvariants(ed), std::string{});
      continue;
    }

    // Every label on offer, taken from the same armed state each time.
    ++labelled;
    const LeapState armed = ed.leap;
    for (const LeapLabel& label : armed.labels) {
      Editor again;
      LeapDocument(again, before, ed.doc.view.rows, ed.doc.view.columns);
      again.settings.soft_wrap = ed.settings.soft_wrap;
      again.doc.view.top_line = ed.doc.view.top_line;
      again.doc.selections.Set(MinWidth1(again.doc.table, Selection{was, was, -1}));
      RunCommands(again, {"leap"});
      ApplyPendingChar(again, first);
      ApplyPendingChar(again, second);
      press(again, std::string_view{&label.key, 1});

      EXPECT_TRUE(again.leap.stage == LeapState::Stage::kOff);
      EXPECT_EQ(Cur(again), label.at);
      EXPECT_TRUE(IsGraphemeBoundary(again.doc.table, Cur(again)));
      EXPECT_EQ(ReadDocRange(again.doc.table,
                             Interval(Cur(again), Cur(again) + std::ssize(first) +
                                                      std::ssize(second))),
                first + second);
      EXPECT_EQ(AssembleDocContents(again.doc.table), before);
      EXPECT_EQ(EditorInvariants(again), std::string{});
    }
  }

  // All three outcomes really happened: a run that only ever warned would pass
  // every assertion above without leaping anywhere.
  EXPECT_TRUE(warned > 0);
  EXPECT_TRUE(jumped > 0);
  EXPECT_TRUE(labelled > 0);
}

namespace {

// The document eight labelled `ab` pairs deep, in a pane wide enough for the
// status overlay to fit and narrow enough for a long message to be clipped
// into it.
constexpr int kHintWidth = 60;

constexpr int kHintHeight = 12;

// Verbatim what PumpCommandJobs does when a watched command gives up on its
// deadline: a warning that arrives between two keystrokes, with no keystroke
// of its own for the leap to catch it on.
constexpr std::string_view kBackgroundWarning =
    "grep -rn foo did not finish -- gave up on it, and this message is long enough that "
    "the status bar cannot hold it in one line at all";

bool Shows(const Surface& frame, std::string_view needle) {
  for (int y = 0; y < frame.height; ++y) {
    if (frame.Row(y).find(needle) != std::string::npos) return true;
  }
  return false;
}

// Every cell painted in the jump-label background, which is the one thing on
// the screen that says "this is a key you can press".
int LabelCells(const Surface& frame, const Style& label) {
  int n = 0;
  for (int y = 0; y < frame.height; ++y) {
    for (int x = 0; x < frame.width; ++x) {
      if (frame.At(x, y).bg == static_cast<Attr>(label.bg.rgb)) ++n;
    }
  }
  return n;
}

}  // namespace

void LeapKeepsItsHintUnderABackgroundWarning() {
  Presser press;

  TEST_CASE("leap: a background warning takes neither the leap's line nor its labels");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    std::string text;
    for (int i = 0; i < 8; ++i) text += "ab..\n";
    ResetToOriginal(ed.doc.table, text);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    FitFocusedViewport(ed, kHintWidth, kHintHeight);

    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_EQ(ed.leap.labels.size(), std::size_t{8});

    Surface armed;
    RenderTo(ed, armed, kHintWidth, kHintHeight);
    const Style label = ed.theme.Get("ui.virtual.jump-label");
    const int labels_before = LabelCells(armed, label);
    EXPECT_EQ(labels_before, 8);
    // The bar is the mode's own line, and the popup is not up over the labels.
    EXPECT_TRUE(armed.Row(kHintHeight - 1).find("a label jumps") != std::string::npos);
    EXPECT_TRUE(!ed.status_overlay);

    ed.status.Warn(std::string{kBackgroundWarning});
    Surface warned;
    RenderTo(ed, warned, kHintWidth, kHintHeight);

    // Not one cell moved: the same labels, the same hint, and no popup over
    // the pane -- a covered label's key would still jump.
    for (int y = 0; y < kHintHeight; ++y) EXPECT_EQ(warned.Row(y), armed.Row(y));
    EXPECT_EQ(LabelCells(warned, label), labels_before);
    EXPECT_TRUE(warned.Row(kHintHeight - 1).find("a label jumps") != std::string::npos);
    EXPECT_TRUE(!Shows(warned, "grep -rn foo"));
    EXPECT_TRUE(!ed.status_overlay);
    EXPECT_TRUE(LeapIsLive(ed));
    // Held, not swallowed: the warning is still the editor's status message,
    // and the leap is only borrowing the line it is drawn on.
    EXPECT_TRUE(ed.status.find("grep -rn foo") != std::string::npos);
    EXPECT_TRUE(ed.status.level() == StatusLevel::kWarning);
    EXPECT_EQ(std::string{LeapHint(ed)}, std::string{ed.leap.hint});

    // The mode ends with no keystroke of its own -- a background job rebuilds
    // the buffer under it -- and the warning takes the line back, popup and
    // all. Nothing about it was lost while the labels were up.
    ResetToOriginal(ed.doc.table, text);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    EXPECT_TRUE(!LeapIsLive(ed));
    EXPECT_TRUE(LeapHint(ed).empty());
    Surface after;
    RenderTo(ed, after, kHintWidth, kHintHeight);
    EXPECT_EQ(LabelCells(after, label), 0);
    EXPECT_TRUE(ed.status_overlay);
    EXPECT_TRUE(Shows(after, "grep -rn foo"));
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: the hint is the armed pane's, and no other capture takes it");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    LeapDocument(ed, "abab\nzzzz\nabab\n", 6, 40);
    PlaceCaret(ed, 0, 0);
    EXPECT_TRUE(LeapHint(ed).empty());

    press(ed, "y");
    EXPECT_TRUE(LeapHint(ed).find("two characters") != std::string_view::npos);
    press(ed, "a");
    EXPECT_TRUE(LeapHint(ed).find("character after it") != std::string_view::npos);
    press(ed, "b");
    EXPECT_TRUE(LeapHint(ed).find("a label jumps") != std::string_view::npos);
    // A background warning at every stage leaves the hint where it is.
    ed.status.Warn(std::string{kBackgroundWarning});
    EXPECT_TRUE(LeapHint(ed).find("a label jumps") != std::string_view::npos);
    press(ed, "esc");
    EXPECT_TRUE(LeapHint(ed).empty());

    // `r` is a capture too, and it says what it wants in the ordinary way.
    press(ed, "r");
    EXPECT_TRUE(ed.pending_char == PendingChar::kReplaceChar);
    EXPECT_TRUE(LeapHint(ed).empty());
    press(ed, "z");
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void LeapEndsWhenTheWindowResizes() {
  Presser press;

  TEST_CASE("leap: a resize at the label stage ends the mode rather than arming a dead pane");
  {
    std::string text;
    for (int i = 0; i < 40; ++i) text += "ab..\n";
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, text);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    FitFocusedViewport(ed, 40, 32);

    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_TRUE(ed.leap.matches.size() > 5);
    // 'k' is the eighth label key as well as the motion, so the assertion
    // below tells the two apart rather than only saying the caret moved.
    const Index labelled = MatchAt(ed, 7);
    EXPECT_TRUE(labelled > 0);
    const Index was = Cur(ed);

    // The terminal shrinks. The loop's resize branch runs, then the fit the
    // next frame does -- in that order, as the loop does it.
    HandleResize(ed);
    FitFocusedViewport(ed, 40, 6);

    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_TRUE(ed.pending_char == PendingChar::kNone);
    EXPECT_TRUE(ed.leap.matches.empty());
    EXPECT_TRUE(ed.leap.labels.empty());
    EXPECT_TRUE(ed.leap.spans.empty());
    EXPECT_TRUE(!LeapIsLive(ed));
    EXPECT_EQ(ed.pending_count, Index{0});
    EXPECT_TRUE(ed.status.find("the window resized") != std::string::npos);
    EXPECT_TRUE(ed.status.level() == StatusLevel::kWarning);
    EXPECT_EQ(Cur(ed), was);

    // Nothing is offered on the new pane, and no label of the old one is
    // selectable: the key is the command it usually is.
    Surface frame;
    RenderTo(ed, frame, 40, 6);
    EXPECT_EQ(LabelCells(frame, ed.theme.Get("ui.virtual.jump-label")), 0);
    press(ed, "k");
    EXPECT_TRUE(Cur(ed) != labelled);
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{1});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: a resize between the two characters ends the mode too");
  {
    std::string text;
    for (int i = 0; i < 40; ++i) text += "ab..\n";
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, text);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    FitFocusedViewport(ed, 40, 32);

    press(ed, "y");
    press(ed, "a");
    EXPECT_TRUE(ed.pending_char == PendingChar::kLeapSecond);
    EXPECT_TRUE(!ed.leap.spans.empty());

    HandleResize(ed);
    FitFocusedViewport(ed, 40, 6);
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_TRUE(ed.pending_char == PendingChar::kNone);
    EXPECT_TRUE(ed.leap.spans.empty());
    EXPECT_TRUE(ed.status.find("the window resized") != std::string::npos);

    // The character that would have completed the pair is a command again.
    press(ed, "b");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: a resize with nothing armed says nothing");
  {
    Editor ed;
    LeapDocument(ed, "abab\nzzzz\nabab\n", 6, 40);
    PlaceCaret(ed, 0, 0);
    ed.status.clear();
    HandleResize(ed);
    EXPECT_TRUE(ed.status.empty());

    // A capture that is not a leap was not measured against the pane, and a
    // resize is none of its business.
    press(ed, "r");
    EXPECT_TRUE(ed.pending_char == PendingChar::kReplaceChar);
    HandleResize(ed);
    EXPECT_TRUE(ed.pending_char == PendingChar::kReplaceChar);
    EXPECT_TRUE(ed.status.find("resized") == std::string::npos);
    press(ed, "z");
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void LeapWrapsWhereTheRendererWraps() {
  Presser press;

  // One 400-column line with a single `xy` in it, wrapped in a pane whose
  // continuation indent is the whole question: the renderer measures the wrap
  // indicator one way and a leap that rebuilt the metrics by hand measured it
  // another, so the row the offered range stopped at was not the last row on
  // the screen.
  struct Config {
    std::string_view indicator;
    Index tab_width;
    const char* said;
  };
  static constexpr std::array<Config, 5> kConfigs{
      Config{"\t", 8, "a tab, eight columns wide to anything but the renderer"},
      Config{"\t", 2, "a tab, two columns wide to anything but the renderer"},
      Config{"\xE2\x86\xB3 ", 4, "the shipped indicator"},
      Config{"", 8, "no indicator at all"},
      Config{"\xE2\x86\x92", 3, "one column"},
  };

  TEST_CASE("leap: the offered range ends on the row the renderer stopped drawing");
  for (const Config& config : kConfigs) {
    for (Index target = 250; target < 400; target += 7) {
      Editor ed;
      ed.theme = BuiltinTheme();
      std::string line(400, '.');
      line.replace(static_cast<std::size_t>(target), 2, "xy");
      ResetToOriginal(ed.doc.table, line + "\n");
      ed.settings.soft_wrap = true;
      ed.settings.wrap_indicator = std::string{config.indicator};
      ed.doc.tab_width = config.tab_width;
      ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

      constexpr int kWidth = 40;
      constexpr int kHeight = 11;
      FitFocusedViewport(ed, kWidth, kHeight);

      // The helper really is the renderer's own measure of this pane, not a
      // second copy of it that happens to agree today.
      const WrapMetrics mine = WrapForFocusedViewport(ed);
      const WrapMetrics theirs = WrapOf(ed, GutterWidth(ed, kWidth), kWidth);
      EXPECT_EQ(mine.indent, theirs.indent);
      EXPECT_EQ(mine.width, theirs.width);
      EXPECT_EQ(mine.tab_width, theirs.tab_width);
      EXPECT_EQ(mine.max_wrap, theirs.max_wrap);
      EXPECT_TRUE(mine.enabled == theirs.enabled);

      Surface frame;
      RenderTo(ed, frame, kWidth, kHeight);
      // Both halves of the pair are on the screen, or the pair is not: `x` and
      // `y` occur nowhere else in the document, and a pair the last drawn row
      // cuts in half is one the leap must not offer either.
      const bool drawn = Shows(frame, "x") && Shows(frame, "y");

      press(ed, "y");
      press(ed, "x");
      press(ed, "y");
      const bool jumped = (Cur(ed) == target);
      if (jumped != drawn) {
        std::printf("leap wrap: indicator [%s] tab %lld target %lld drawn %d jumped %d\n",
                    config.said, static_cast<long long>(config.tab_width),
                    static_cast<long long>(target), static_cast<int>(drawn),
                    static_cast<int>(jumped));
      }
      EXPECT_TRUE(jumped == drawn);
      if (!drawn) EXPECT_TRUE(ed.status.find("no match") != std::string::npos);
      EXPECT_EQ(EditorInvariants(ed), std::string{});
    }
  }

  TEST_CASE("leap: a tab in the wrap indicator is one column to the leap as well");
  {
    // The reproduction. The renderer's indent is 1, so ten rows reach byte 341
    // and the pair at 300 is plainly on the screen; an indent of tab_width
    // stopped the offered range at 278 and the leap answered "no match".
    Editor ed;
    ed.theme = BuiltinTheme();
    std::string line(400, '.');
    line.replace(300, 2, "xy");
    ResetToOriginal(ed.doc.table, line + "\n");
    ed.settings.soft_wrap = true;
    ed.settings.wrap_indicator = "\t";
    ed.doc.tab_width = 8;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    constexpr int kWidth = 40;
    constexpr int kHeight = 11;
    FitFocusedViewport(ed, kWidth, kHeight);
    EXPECT_EQ(WrapForFocusedViewport(ed).indent, Index{1});

    Surface frame;
    RenderTo(ed, frame, kWidth, kHeight);
    const int gutter = GutterWidth(ed, kWidth);
    bool adjacent = false;
    for (int y = 0; (y < (kHeight - 1)) && !adjacent; ++y) {
      for (int x = gutter; x < (kWidth - 1); ++x) {
        if ((frame.At(x, y).text == std::string{"x"}) &&
            (frame.At(x + 1, y).text == std::string{"y"})) {
          adjacent = true;
          break;
        }
      }
    }
    EXPECT_TRUE(adjacent);

    press(ed, "y");
    press(ed, "x");
    press(ed, "y");
    EXPECT_EQ(Cur(ed), Index{300});
    EXPECT_TRUE(ed.status.find("no match") == std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void LeapRefusesAClusterTheRightEdgeCutsInHalf() {
  Presser press;

  TEST_CASE("leap: a wide cluster the pane cuts in half is drawn blank and not offered");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    constexpr int kWidth = 30;
    constexpr int kHeight = 5;
    // Built once the gutter is known, so the CJK starts on the last visible
    // column and its second half falls off the pane.
    ResetToOriginal(ed.doc.table, "placeholder\n");
    const int gutter = GutterWidth(ed, kWidth);
    const Index columns = kWidth - gutter;
    std::string line(static_cast<std::size_t>(columns - 2), '.');
    line += "a";
    line += std::string{kCJK};
    line += "tail";
    ResetToOriginal(ed.doc.table, line + "\n");
    ed.settings.soft_wrap = false;
    ed.doc.view.rows = kHeight - 1;
    ed.doc.view.columns = columns;
    ed.doc.view.top_line = 0;
    ed.doc.view.top_row = 0;
    ed.doc.view.left_column = 0;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    Surface frame;
    RenderTo(ed, frame, kWidth, kHeight);
    // The renderer refuses to draw half a glyph: the last column is a space,
    // and the `日` is nowhere on the screen.
    EXPECT_EQ(frame.At(kWidth - 2, 0).text, std::string{"a"});
    EXPECT_EQ(frame.At(kWidth - 1, 0).text, std::string{" "});

    press(ed, "y");
    press(ed, "a");
    press(ed, std::string{kCJK});
    // So the pair is not one the leap may offer either.
    EXPECT_EQ(Cur(ed), Index{0});
    EXPECT_TRUE(ed.status.find("no match") != std::string::npos);
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: a tab at the right edge keeps the column the renderer draws for it");
  {
    // The exception to the rule above, and the reason the clip asks what the
    // cluster is: a tab that runs off the pane still has its first column
    // drawn, so it is visible text however much of its advance is lost.
    Editor ed;
    ed.theme = BuiltinTheme();
    constexpr int kWidth = 30;
    constexpr int kHeight = 5;
    ResetToOriginal(ed.doc.table, "placeholder\n");
    const int gutter = GutterWidth(ed, kWidth);
    const Index columns = kWidth - gutter;
    const Index tab_column = columns - 1;
    // A tab whose advance really does run past the edge, whatever the gutter
    // turned out to be.
    Index tab_width = 0;
    for (const Index candidate : {Index{8}, Index{4}, Index{3}, Index{2}}) {
      if ((candidate - (tab_column % candidate)) > 1) {
        tab_width = candidate;
        break;
      }
    }
    EXPECT_TRUE(tab_width > 0);
    std::string line(static_cast<std::size_t>(columns - 2), '.');
    line += "a\ttail";
    ResetToOriginal(ed.doc.table, line + "\n");
    ed.doc.tab_width = tab_width;
    ed.settings.soft_wrap = false;
    ed.settings.render_tabs = true;
    ed.doc.view.rows = kHeight - 1;
    ed.doc.view.columns = columns;
    ed.doc.view.top_line = 0;
    ed.doc.view.top_row = 0;
    ed.doc.view.left_column = 0;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    Surface frame;
    RenderTo(ed, frame, kWidth, kHeight);
    EXPECT_EQ(frame.At(kWidth - 1, 0).text, ed.settings.tab_glyph);

    press(ed, "y");
    press(ed, "a");
    press(ed, "tab");
    EXPECT_EQ(Cur(ed), columns - 2);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void LeapRefusesANewlineAndKeepsTheTab() {
  Presser press;

  TEST_CASE("leap: enter at either arm stage cancels rather than searching for a line break");
  {
    Editor ed;
    LeapDocument(ed, "alpha\nbravo\ncharlie\n");
    PlaceCaret(ed, 0, 0);
    const Index was = Cur(ed);
    press(ed, "y");
    press(ed, "ret");
    EXPECT_TRUE(ed.pending_char == PendingChar::kNone);
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(std::string{ed.status}, std::string{"leap: cancelled"});
    EXPECT_EQ(Cur(ed), was);
    // And the key after it is the command it usually is, rather than a second
    // character for a search that could never have matched. `k` is this
    // keymap's move-down.
    press(ed, "k");
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{1});

    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "a");
    EXPECT_TRUE(ed.pending_char == PendingChar::kLeapSecond);
    press(ed, "ret");
    EXPECT_TRUE(ed.pending_char == PendingChar::kNone);
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(std::string{ed.status}, std::string{"leap: cancelled"});
    press(ed, "k");
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{1});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: a tab is a character the text can hold, and stays searchable");
  {
    // Deliberately not refused with the newline: a leap never crosses a line
    // break, so `\n` names nothing, but `\t` is ordinary indentation and a
    // pair beginning on one is a target like any other.
    Editor ed;
    LeapDocument(ed, "\tab\n\tab\n", 6, 40);
    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "tab");
    EXPECT_TRUE(ed.pending_char == PendingChar::kLeapSecond);
    EXPECT_EQ(std::ssize(ed.leap.spans), Index{2});
    press(ed, "a");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_EQ(std::ssize(ed.leap.matches), Index{2});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void LeapIgnoresALabelKeyThatNamesNothing() {
  Presser press;

  TEST_CASE("leap: a label key past the end of a short page is ignored, not obeyed");
  {
    // 26 matches: page 0 wears all 25 keys, page 1 wears one.
    std::string text;
    for (int i = 0; i < 26; ++i) text += "ab..\n";
    Editor ed;
    LeapDocument(ed, text, 30, 90);
    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    press(ed, "space");
    EXPECT_EQ(ed.leap.page, std::size_t{1});
    EXPECT_EQ(ed.leap.labels.size(), std::size_t{1});
    const Index was = Cur(ed);
    const std::string hint{LeapHint(ed)};
    EXPECT_TRUE(hint.find("a label jumps") != std::string::npos);

    // `s` is the second label key, and on this page it names nothing.
    press(ed, "s");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_TRUE(ed.pending_char == PendingChar::kLeapLabel);
    EXPECT_EQ(ed.leap.page, std::size_t{1});
    EXPECT_EQ(ed.leap.labels.size(), std::size_t{1});
    EXPECT_EQ(Cur(ed), was);
    // The page is still up and still saying what its keys mean.
    EXPECT_EQ(std::string{LeapHint(ed)}, hint);

    // And the one key this page does carry still jumps.
    press(ed, "a");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(Cur(ed), Index{25 * 5});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void LeapKeepsTheRowsAWideIndicatorWouldPushOffThePane() {
  Presser press;

  TEST_CASE("leap: an indicator as wide as the pane leaves a column for the text");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "abababababab\n");
    ed.settings.soft_wrap = true;
    ed.settings.wrap_indicator = "»»»»»»»»";
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    constexpr int kWidth = 8;
    constexpr int kHeight = 5;
    const int gutter = GutterWidth(ed, kWidth);
    EXPECT_EQ(gutter, 0);
    ed.doc.view.rows = kHeight - 1;
    ed.doc.view.columns = kWidth - gutter;
    ed.doc.view.top_line = 0;
    ed.doc.view.top_row = 0;
    ed.doc.view.left_column = 0;

    // The indicator is eight columns and the pane is eight columns: indenting
    // by all of them would put every continuation row past the right edge.
    EXPECT_EQ(WrapForFocusedViewport(ed).indent, Index{kWidth - 1});

    // A continuation row draws the column it was left, rather than nothing:
    // the indicator fills the pane and the text overwrites its last cell.
    Surface plain;
    RenderTo(ed, plain, kWidth, kHeight);
    EXPECT_EQ(plain.At(kWidth - 1, 1).text, std::string{"a"});
    EXPECT_EQ(plain.At(kWidth - 2, 1).text, std::string{"»"});

    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    EXPECT_TRUE(ed.leap.matches.size() > 4);

    Surface frame;
    RenderTo(ed, frame, kWidth, kHeight);
    const Style label = ed.theme.Get("ui.virtual.jump-label");
    EXPECT_TRUE(label.bg.set);

    // Every key offered is a key on the screen, exactly once -- which is the
    // whole contract, and what an indent of the full pane width broke.
    EXPECT_TRUE(!ed.leap.labels.empty());
    for (const LeapLabel& one : ed.leap.labels) {
      int seen = 0;
      for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
          if ((frame.At(x, y).text == std::string(1, one.key)) &&
              (frame.At(x, y).bg == static_cast<Attr>(label.bg.rgb))) {
            ++seen;
          }
        }
      }
      EXPECT_EQ(seen, 1);
    }
    EXPECT_EQ(LabelCells(frame, label), static_cast<int>(ed.leap.labels.size()));
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void ALabelOnATabIsOneCellNotAWholeTabStop() {
  Presser press;

  TEST_CASE("leap: a label on a tab wears the label style for one cell of it");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    LeapDocument(ed, "\tab\n\tab\n", 6, 40);
    ed.doc.tab_width = 4;
    // The claim here is about the label -- one cell of it, and the rest of the
    // tab left alone -- so the tab is drawn blank on purpose rather than at
    // whatever render_tabs happens to default to. With glyphs on, the columns
    // checked below carry the tab_pad and this case would be asserting the
    // whitespace setting instead of the thing it is named for.
    ed.settings.render_tabs = false;
    // Off the labelled row, so nothing on it is dressed as the caret's line.
    PlaceCaret(ed, 1, 6);

    constexpr int kWidth = 40;
    constexpr int kHeight = 8;
    const int gutter = GutterWidth(ed, kWidth);

    press(ed, "y");
    press(ed, "tab");
    press(ed, "a");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_EQ(std::ssize(ed.leap.matches), Index{2});

    Surface frame;
    RenderTo(ed, frame, kWidth, kHeight);
    const Style label = ed.theme.Get("ui.virtual.jump-label");
    const Style match = ed.theme.Get("ui.cursor.match");
    EXPECT_TRUE(label.bg.set);
    EXPECT_TRUE(match.bg.set);

    // One cell of chrome per label, not one tab stop of it.
    EXPECT_EQ(LabelCells(frame, label), static_cast<int>(ed.leap.labels.size()));
    for (const LeapLabel& one : ed.leap.labels) {
      const int y = static_cast<int>(LineAt(ed.doc.table, one.at) - ed.doc.view.top_line);
      EXPECT_EQ(frame.At(gutter, y).text, std::string(1, one.key));
      EXPECT_EQ(frame.At(gutter, y).bg, static_cast<Attr>(label.bg.rgb));
      // The columns the tab still owns are blanked in the style the text under
      // the label had -- lit as part of the pair, and no wider a key for it.
      for (int x = gutter + 1; x < (gutter + static_cast<int>(ed.doc.tab_width)); ++x) {
        EXPECT_EQ(frame.At(x, y).text, std::string{" "});
        EXPECT_EQ(frame.At(x, y).bg, static_cast<Attr>(match.bg.rgb));
      }
    }
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void LeapReadsOnlyTheColumnsThePaneAsksFor() {
  Presser press;

  TEST_CASE("leap: the horizontal clip lands on cluster boundaries however long a cluster is");
  {
    // One column, sixty-one bytes: the read that finds the pane's edge cannot
    // be bounded by bytes-per-column alone, and a cap that truncated a cluster
    // would hand back a position inside one.
    std::string zalgo = "a";
    for (int i = 0; i < 30; ++i) zalgo += "\xCC\x81";
    const Index cluster_bytes = std::ssize(zalgo);
    EXPECT_EQ(GraphemeWidth(zalgo), 1);

    const auto run = [&](Index count) {
      std::string out;
      for (Index i = 0; i < count; ++i) out += zalgo;
      return out;
    };
    // Columns 0-9 zalgo, 10-11 "xy", 12-111 zalgo, 112-113 "xy", then more.
    const std::string line = run(10) + "xy" + run(100) + "xy" + run(10);

    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, line + "\n");
    ed.settings.soft_wrap = false;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.doc.view.rows = 4;
    ed.doc.view.columns = 30;
    ed.doc.view.top_line = 0;
    ed.doc.view.top_row = 0;
    ed.doc.view.left_column = 0;

    const Index end = std::ssize(line);
    const Index at_ten = ByteForColumnFrom(ed.doc.table, 0, end, 10, ed.doc.tab_width);
    EXPECT_EQ(at_ten, 10 * cluster_bytes);
    EXPECT_EQ(SnapToGraphemeBoundary(ed.doc.table, at_ten), at_ten);
    // Column 30 is the pane's right edge: ten clusters, "xy", eighteen more.
    const Index at_edge = ByteForColumnFrom(ed.doc.table, 0, end, 30, ed.doc.tab_width);
    EXPECT_EQ(at_edge, (28 * cluster_bytes) + 2);
    EXPECT_EQ(SnapToGraphemeBoundary(ed.doc.table, at_edge), at_edge);
    // Past the end of the line stops at the end of the line, not before it.
    EXPECT_EQ(ByteForColumnFrom(ed.doc.table, 0, end, 100000, ed.doc.tab_width), end);

    // And the leap over the same line: the pair inside the pane is the only
    // one it offers, so it jumps straight to it.
    press(ed, "y");
    press(ed, "x");
    press(ed, "y");
    EXPECT_EQ(Cur(ed), 10 * cluster_bytes);
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: a window edge inside a combining mark's encoding is not a boundary");
  {
    // Built so the guessed read window ends one byte into U+0301's two-byte
    // encoding exactly when the column budget runs out on the base before it.
    // The truncated tail cannot join, so a stateless walk sees a boundary
    // after the 'e' that the document does not have -- with spare bytes still
    // in the window, which is why "stopped early" alone must not be trusted.
    // Three zalgo clusters (9 bytes, 1 column each), three 'a's, then e+0301:
    // column 7 stops after the 'e' at byte 31; the 4-bytes-per-column window
    // for column 7 is 32, cutting between the mark's two bytes.
    std::string heavy = "x";
    for (int i = 0; i < 4; ++i) heavy += "\xCC\x81";
    EXPECT_EQ(std::ssize(heavy), Index{9});
    const std::string line = heavy + heavy + heavy + "aaa" + "e\xCC\x81" + "tail";

    PieceTable table;
    ResetToOriginal(table, line + "\n");
    const Index end = std::ssize(line);
    const Index at = ByteForColumnFrom(table, 0, end, 7, 4);
    // The e+0301 cluster spans [30, 33); 31 is inside it.
    EXPECT_EQ(at, Index{33});
    EXPECT_EQ(SnapToGraphemeBoundary(table, at), at);
  }
}

void LeapSaysWhyItEndedInItsOwnWords() {
  Presser press;

  TEST_CASE("leap: a key that is not a character ends the mode in the mode's vocabulary");
  {
    Editor ed;
    LeapDocument(ed, "ab..\n..ab\nab..\n");
    PlaceCaret(ed, 0, 0);
    const Index was = Cur(ed);

    for (const std::string_view key : {"backspace", "left", "home", "C-x"}) {
      press(ed, "y");
      press(ed, "a");
      press(ed, "b");
      EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
      press(ed, key);
      EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
      EXPECT_TRUE(ed.pending_char == PendingChar::kNone);
      EXPECT_EQ(std::string{ed.status}, std::string{"leap: cancelled"});
      EXPECT_EQ(Cur(ed), was);
    }

    // The same at the stage before it, where nothing has been labelled yet.
    press(ed, "y");
    press(ed, "a");
    press(ed, "backspace");
    EXPECT_EQ(std::string{ed.status}, std::string{"leap: cancelled"});

    // Esc keeps the word the whole editor uses for it.
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    press(ed, "esc");
    EXPECT_EQ(std::string{ed.status}, std::string{"cancelled"});

    // And a capture whose argument really is a character still complains that
    // it did not get one.
    RunCommands(ed, {"replace"});
    EXPECT_TRUE(ed.pending_char == PendingChar::kReplaceChar);
    press(ed, "backspace");
    EXPECT_EQ(std::string{ed.status}, std::string{"not a character"});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void LeapPicksGrowIntoCursors() {
  Presser press;
  // Three matches: 'a' at bytes 9, 16, 23, nearest-first from a caret at 0 in
  // exactly that order, so the labels are a/s/d on 9/16/23. The first line has
  // no match, so the primary lands on a different line than the origin.
  const std::string text = "......\n..ab..\n..ab..\n..ab..\n";

  TEST_CASE("leap: * puts a cursor on every match, nearest primary");
  {
    const Scratch scratch{"koi-leap-star"};
    const AsProjectRoot root{scratch.dir};
    // The matches pushed eleven lines further down than the shared fixture, and
    // for the same reason as the single-jump case: a jump shorter than
    // kLocationMergeLines merges onto the row it left, and stepping back then
    // has nowhere to go.
    std::string tall = "......\n";
    for (int i = 0; i < 11; ++i) tall += "......\n";
    tall += "..ab..\n..ab..\n..ab..\n";
    const auto first = static_cast<Index>(tall.find("ab"));
    const std::filesystem::path file = scratch.Write("a.txt", tall);
    std::string error;
    Editor ed;
    ed.jumps = OpenJumpStore(scratch.dir / "jumps.db", "pane-leap-star", error);
    EXPECT_TRUE(ed.jumps != nullptr);
    LeapDocument(ed, tall, 30, 90);
    ed.doc.file = std::filesystem::weakly_canonical(file);
    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    press(ed, "*");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    const auto& all = ed.doc.selections.Ranges();
    EXPECT_EQ(all.size(), std::size_t{3});
    EXPECT_EQ(all[0].From(), first);
    EXPECT_EQ(all[1].From(), first + 7);
    EXPECT_EQ(all[2].From(), first + 14);
    EXPECT_EQ(ed.doc.selections.Primary().From(), first);
    // One jumplist entry for the whole spawn, like the single jump.
    RunCommands(ed, {"jump_backward"});
    EXPECT_EQ(Cur(ed), Index{0});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: capitals pick, and the lowercase label lands them all");
  {
    Editor ed;
    LeapDocument(ed, text, 30, 90);
    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    press(ed, "A-s");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_TRUE(std::string{LeapHint(ed)}.find("1 picked") != std::string::npos);
    press(ed, "A-d");
    EXPECT_TRUE(std::string{LeapHint(ed)}.find("2 picked") != std::string::npos);
    press(ed, "a");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    const auto& all = ed.doc.selections.Ranges();
    EXPECT_EQ(all.size(), std::size_t{3});
    // The lowercase label is the landing, so it is the primary.
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{9});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: enter keeps exactly the picks, and a pick made twice is one cursor");
  {
    Editor ed;
    LeapDocument(ed, text, 30, 90);
    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    press(ed, "A-s");
    press(ed, "A-s");
    press(ed, "ret");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(ed.doc.selections.Ranges().size(), std::size_t{1});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{16});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: esc is still never-mind -- picks and all");
  {
    Editor ed;
    LeapDocument(ed, text, 30, 90);
    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    press(ed, "A-s");
    press(ed, "esc");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(ed.doc.selections.Ranges().size(), std::size_t{1});
    EXPECT_EQ(Cur(ed), Index{0});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: picks survive paging, and a dead capital is ignored like a dead label");
  {
    std::string wide;
    for (int i = 0; i < 26; ++i) wide += "ab..\n";
    Editor ed;
    LeapDocument(ed, wide, 30, 90);
    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    press(ed, "A-s");
    press(ed, "space");
    EXPECT_EQ(ed.leap.page, std::size_t{1});
    // Page 1 carries one label; its second key names nothing, alt or capital.
    press(ed, "A-s");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_TRUE(std::string{LeapHint(ed)}.find("1 picked") != std::string::npos);
    press(ed, "A-a");
    EXPECT_TRUE(std::string{LeapHint(ed)}.find("2 picked") != std::string::npos);
    press(ed, "ret");
    const auto& all = ed.doc.selections.Ranges();
    EXPECT_EQ(all.size(), std::size_t{2});
    EXPECT_EQ(all[0].From(), Index{5});
    EXPECT_EQ(all[1].From(), Index{125});
    // The last pick is what enter leaves primary.
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{125});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void LeapCapitalsSelectToTheMatch() {
  Presser press;
  // 'a' at bytes 9, 16, 23; labels a/s/d in that (nearest-first) order from a
  // caret at 0.
  const std::string text = "......\n..ab..\n..ab..\n..ab..\n";

  TEST_CASE("leap: a capital label selects from here to that match");
  {
    Editor ed;
    LeapDocument(ed, text, 30, 90);
    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    press(ed, "S");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    EXPECT_EQ(ed.doc.selections.Ranges().size(), std::size_t{1});
    const Selection& got = ed.doc.selections.Primary();
    // Anchor held at the origin, head landed on the match's grapheme -- the
    // same [from, match+1) an extend motion would leave.
    EXPECT_EQ(got.From(), Index{0});
    EXPECT_EQ(got.To(), Index{17});
    EXPECT_TRUE(!got.Backward());
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: selecting to a match behind the caret flips like any extend");
  {
    Editor ed;
    LeapDocument(ed, text, 30, 90);
    PlaceCaret(ed, 3, 5);
    const Index origin = Cur(ed);
    EXPECT_EQ(origin, Index{26});
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    // Nearest-first from byte 26: 23, 16, 9 -- so `s` names the middle one.
    press(ed, "S");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    const Selection& got = ed.doc.selections.Primary();
    EXPECT_EQ(got.From(), Index{16});
    EXPECT_EQ(got.To(), Index{27});
    EXPECT_TRUE(got.Backward());
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: a capital ends the mode whole -- picks go the way esc takes them");
  {
    Editor ed;
    LeapDocument(ed, text, 30, 90);
    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    press(ed, "A-s");
    EXPECT_TRUE(std::string{LeapHint(ed)}.find("1 picked") != std::string::npos);
    press(ed, "D");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kOff);
    // One selection to byte 23's grapheme, not the pick at 16 plus a cursor.
    EXPECT_EQ(ed.doc.selections.Ranges().size(), std::size_t{1});
    const Selection& got = ed.doc.selections.Primary();
    EXPECT_EQ(got.From(), Index{0});
    EXPECT_EQ(got.To(), Index{24});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: a dead capital on a short page is ignored, not obeyed");
  {
    std::string wide;
    for (int i = 0; i < 26; ++i) wide += "ab..\n";
    Editor ed;
    LeapDocument(ed, wide, 30, 90);
    PlaceCaret(ed, 0, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    press(ed, "space");
    EXPECT_EQ(ed.leap.page, std::size_t{1});
    press(ed, "S");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_EQ(ed.doc.selections.Ranges().size(), std::size_t{1});
    press(ed, "esc");
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void LeapIdentifiesTheDocumentNotTheSlot() {
  Presser press;

  TEST_CASE("leap: two documents can share a revision, so the gate asks which document");
  {
    Editor ed;
    LeapDocument(ed, "abab\nzzzz\nabab\n", 6, 40);
    PlaceCaret(ed, 1, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_TRUE(LeapIsLive(ed));

    // A different document with the same text, in the same slot, carrying the
    // revision every freshly loaded document carries.
    Document other;
    ResetToOriginal(other.table, "abab\nzzzz\nabab\n");
    other.selections.Set(MinWidth1(other.table, Selection{0, 0, -1}));
    other.view = ed.doc.view;
    EXPECT_EQ(other.table.revision, ed.doc.table.revision);
    EXPECT_TRUE(other.id != ed.doc.id);

    const std::size_t slot = ed.active;
    ed.doc = std::move(other);
    EXPECT_EQ(ed.active, slot);
    EXPECT_TRUE(!LeapIsLive(ed));
    EXPECT_TRUE(LeapHint(ed).empty());
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("leap: a closed buffer's slot coming back is not the document it named");
  {
    const auto make = [](const std::string& text, const Viewport& view) {
      Document doc;
      ResetToOriginal(doc.table, text);
      doc.selections.Set(MinWidth1(doc.table, Selection{0, 0, -1}));
      doc.view = view;
      return doc;
    };

    Editor ed;
    LeapDocument(ed, "zzzz\nzzzz\n", 6, 40);
    const Viewport view = ed.doc.view;
    AddBuffer(ed, make("abab\nzzzz\nabab\n", view));
    EXPECT_EQ(ed.active, std::size_t{1});

    PlaceCaret(ed, 1, 0);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    EXPECT_TRUE(LeapIsLive(ed));

    // Neither of these needs a keystroke of its own, so nothing disarms the
    // leap on the way through: the slot is freed and handed back, and the new
    // document's revision is 1 exactly like the old one's.
    CloseActiveBuffer(ed);
    AddBuffer(ed, make("abab\nzzzz\nabab\n", view));
    EXPECT_EQ(ed.active, std::size_t{1});
    EXPECT_EQ(ed.doc.table.revision, ed.leap.revision);
    EXPECT_TRUE(!LeapIsLive(ed));
    EXPECT_TRUE(LeapHint(ed).empty());
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

}  // namespace koi
