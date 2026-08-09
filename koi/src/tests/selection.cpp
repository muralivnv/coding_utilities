// Tests for selection.cpp: selections and the cursors in them -- movement,
// normalisation, mapping through edits, and multi-cursor behaviour.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void SelectionMapping() {
  TEST_CASE("selection mapping");
  Edit ins;
  ins.start_byte = 10; ins.old_end_byte = 10; ins.new_end_byte = 13;
  EXPECT_EQ(MapPosition(5, ins), Index{5});
  EXPECT_EQ(MapPosition(10, ins), Index{10});
  EXPECT_EQ(MapPosition(11, ins), Index{14});

  Edit del;
  del.start_byte = 10; del.old_end_byte = 20; del.new_end_byte = 10;
  EXPECT_EQ(MapPosition(5, del), Index{5});
  EXPECT_EQ(MapPosition(15, del), Index{10});
  EXPECT_EQ(MapPosition(25, del), Index{15});

  Edit rep;
  rep.start_byte = 10; rep.old_end_byte = 20; rep.new_end_byte = 14;
  EXPECT_EQ(MapPosition(30, rep), Index{24});
  EXPECT_EQ(MapPosition(18, rep), Index{14});
}

namespace {

// What MapThroughEdits used to do, kept as the reference its composed sweep has
// to reproduce position for position.
Index FoldThroughEdits(Index pos, const std::vector<Edit>& edits) {
  for (const Edit& e : edits) pos = MapPosition(pos, e);
  return pos;
}

// One edit as the generator thinks of it: `removed` bytes at `at` (pre-batch
// coordinates) replaced by `inserted` bytes.
struct PlannedEdit {
  Index at{0};
  Index removed{0};
  Index inserted{0};
};

std::vector<PlannedEdit> PlanEdits(Rng& rng, Index span, int count) {
  std::vector<PlannedEdit> plan;
  Index at = rng.Pick(0, 3);
  for (int i = 0; i < count; ++i) {
    // Zero-length removals, zero-length insertions and zero-length gaps all on
    // purpose: pure insertions and edits that touch are where the tie rules of
    // MapPosition live, and they are what a composed map can get wrong.
    const Index removed = rng.Pick(0, 4);
    const Index inserted = rng.Pick(0, 4);
    if ((at + removed) > span) break;
    plan.push_back(PlannedEdit{at, removed, inserted});
    at += removed + rng.Pick(0, 3);
  }
  return plan;
}

// Front to back, each edit in the coordinates the ones before it left behind:
// what one Apply call reports.
std::vector<Edit> AscendingEdits(const std::vector<PlannedEdit>& plan) {
  std::vector<Edit> edits;
  Index delta = 0;
  for (const PlannedEdit& p : plan) {
    Edit e;
    e.start_byte = p.at + delta;
    e.old_end_byte = e.start_byte + p.removed;
    e.new_end_byte = e.start_byte + p.inserted;
    edits.push_back(e);
    delta += p.inserted - p.removed;
  }
  return edits;
}

// Back to front, every edit still in pre-batch coordinates because the ones
// applied before it all sat above it: what a command that edits once per cursor
// reports.
std::vector<Edit> DescendingEdits(const std::vector<PlannedEdit>& plan) {
  std::vector<Edit> edits;
  for (auto it = plan.rbegin(); it != plan.rend(); ++it) {
    Edit e;
    e.start_byte = it->at;
    e.old_end_byte = it->at + it->removed;
    e.new_end_byte = it->at + it->inserted;
    edits.push_back(e);
  }
  return edits;
}

}  // namespace

void SelectionMappingComposes(Rng& rng) {
  TEST_CASE("selection mapping composes");
  // ASCII, and far longer than anything the generator reaches, so Normalize's
  // snapping and clamping are the identity here and only the arithmetic is on
  // test.
  PieceTable table = MakeTable(std::string(4096, 'x'));
  constexpr Index kSpan = 200;

  for (int round = 0; round < 120; ++round) {
    const std::vector<PlannedEdit> plan = PlanEdits(rng, kSpan, static_cast<int>(rng.Pick(1, 12)));
    if (plan.empty()) continue;

    const std::vector<Edit> ascending = AscendingEdits(plan);
    const std::vector<Edit> descending = DescendingEdits(plan);
    std::vector<Edit> scrambled = descending;
    std::shuffle(scrambled.begin(), scrambled.end(), rng.gen);

    const std::vector<Edit>* shapes[] = {&ascending, &descending, &scrambled};
    for (const std::vector<Edit>* edits : shapes) {
      // One selection at a time first, so that a difference cannot hide inside
      // a merge: both endpoints move independently and are checked as such.
      for (int probe = 0; probe < 6; ++probe) {
        const Index anchor = rng.Pick(0, kSpan + 4);
        const Index head = rng.Pick(0, kSpan + 4);
        SelectionSet got;
        got.Set(Selection{anchor, head, 7});
        got.MapThroughEdits(table, *edits);
        EXPECT_EQ(got.Size(), size_t{1});
        EXPECT_EQ(got.Ranges()[0].anchor, FoldThroughEdits(anchor, *edits));
        EXPECT_EQ(got.Ranges()[0].head, FoldThroughEdits(head, *edits));
        EXPECT_EQ(got.Ranges()[0].goal_column, Index{-1});
      }

      // Then the whole set at once, which is the shape the callers pass: the
      // same ranges folded by hand and put through the same normalization.
      std::vector<Selection> before;
      for (Index at = 0; at <= kSpan; at += rng.Pick(1, 9)) {
        before.push_back(Selection{at, at + rng.Pick(0, 3), -1});
      }
      SelectionSet got;
      got.Replace(table, before);
      std::vector<Selection> want = got.Ranges();
      got.MapThroughEdits(table, *edits);
      for (Selection& s : want) {
        s.anchor = FoldThroughEdits(s.anchor, *edits);
        s.head = FoldThroughEdits(s.head, *edits);
      }
      SelectionSet expected;
      expected.Replace(table, want);
      EXPECT_EQ(got.Ranges().size(), expected.Ranges().size());
      for (size_t i = 0; (i < got.Ranges().size()) && (i < expected.Ranges().size()); ++i) {
        EXPECT_TRUE(got.Ranges()[i] == expected.Ranges()[i]);
      }
    }
  }

  // The sweep and the fold agree by construction, so nothing above can tell
  // which one ran. The fallback counter can: ordered batches must compose,
  // overlapping ones must not.
  {
    const std::vector<PlannedEdit> plan = {{4, 2, 5}, {12, 0, 3}, {20, 4, 0}};
    SelectionSet sel;
    sel.Set(Selection{30, 31, -1});

    const Index base = MapThroughEditsFallbacks();
    sel.MapThroughEdits(table, AscendingEdits(plan));
    EXPECT_EQ(MapThroughEditsFallbacks(), base);
    sel.MapThroughEdits(table, DescendingEdits(plan));
    EXPECT_EQ(MapThroughEditsFallbacks(), base);

    Edit outer;
    outer.start_byte = 10; outer.old_end_byte = 20; outer.new_end_byte = 15;
    Edit inner;
    inner.start_byte = 12; inner.old_end_byte = 18; inner.new_end_byte = 14;
    sel.MapThroughEdits(table, std::vector<Edit>{outer, inner});
    EXPECT_EQ(MapThroughEditsFallbacks(), base + 1);

    // Applied top down: a deletion, a pure insertion touching its start, and a
    // third edit lower down that rules out reading the batch as an ascending
    // one. A position the deletion collapses onto that byte stops at the
    // insertion's start, where a composed map would already have carried it
    // past -- the one ordered shape the sweep cannot reproduce, so it must not
    // try.
    Edit cut;
    cut.start_byte = 20; cut.old_end_byte = 26; cut.new_end_byte = 20;
    Edit add;
    add.start_byte = 20; add.old_end_byte = 20; add.new_end_byte = 23;
    Edit below;
    below.start_byte = 10; below.old_end_byte = 12; below.new_end_byte = 10;
    const std::vector<Edit> tie_batch{cut, add, below};
    sel.MapThroughEdits(table, tie_batch);
    EXPECT_EQ(MapThroughEditsFallbacks(), base + 2);
    SelectionSet tie;
    tie.Set(Selection{23, 24, -1});
    tie.MapThroughEdits(table, tie_batch);
    EXPECT_EQ(tie.Ranges()[0].anchor, FoldThroughEdits(23, tie_batch));
    EXPECT_EQ(tie.Ranges()[0].anchor, Index{18});
  }
}

void SelectionNormalize() {
  TEST_CASE("selection normalize");
  PieceTable table = MakeTable("0123456789abcdefghij");
  SelectionSet sel;
  sel.Replace(table, {{5, 9}, {0, 3}, {7, 12}});
  EXPECT_EQ(sel.Size(), size_t{2});
  EXPECT_EQ(sel.Ranges()[0].From(), Index{0});
  EXPECT_EQ(sel.Ranges()[0].To(), Index{3});
  EXPECT_EQ(sel.Ranges()[1].From(), Index{5});
  EXPECT_EQ(sel.Ranges()[1].To(), Index{12});

  SelectionSet carets;
  carets.Replace(table, {{4, 4}, {4, 4}, {8, 8}});
  EXPECT_EQ(carets.Size(), size_t{2});
}

void SelectionMovement() {
  TEST_CASE("selection movement");
  const std::string doc = "long line here\nsh\nanother long line\n";
  PieceTable table = MakeTable(doc);
  constexpr Index kTab = 4;

  SelectionSet sel;
  sel.Set(Selection{10, 10});

  Move(table, sel, Motion::kDown, false, kTab);
  EXPECT_EQ(LineAt(table, Cur(table, sel)), Index{1});
  EXPECT_EQ(ColumnForByte(table, Cur(table, sel), kTab), Index{2});
  Move(table, sel, Motion::kDown, false, kTab);
  EXPECT_EQ(ColumnForByte(table, Cur(table, sel), kTab), Index{10});
  Move(table, sel, Motion::kUp, false, kTab);
  Move(table, sel, Motion::kUp, false, kTab);
  EXPECT_EQ(LineAt(table, Cur(table, sel)), Index{0});
  EXPECT_EQ(ColumnForByte(table, Cur(table, sel), kTab), Index{10});

  Move(table, sel, Motion::kLeft, false, kTab);
  EXPECT_EQ(sel.Primary().goal_column, Index{-1});
  EXPECT_EQ(Cur(table, sel), Index{9});
  EXPECT_FALSE(sel.Primary().IsEmpty());
  EXPECT_EQ(sel.Primary().anchor, Index{9});
  EXPECT_EQ(sel.Primary().head, Index{10});

  Move(table, sel, Motion::kRight, true, kTab);
  EXPECT_EQ(sel.Primary().anchor, Index{9});
  EXPECT_EQ(Cur(table, sel), Index{10});
  EXPECT_EQ(sel.Primary().To(), Index{11});

  Move(table, sel, Motion::kLeft, true, kTab);
  Move(table, sel, Motion::kLeft, true, kTab);
  EXPECT_EQ(Cur(table, sel), Index{8});
  EXPECT_TRUE(sel.Primary().Backward());
  EXPECT_EQ(sel.Primary().From(), Index{8});
  EXPECT_EQ(sel.Primary().To(), Index{10});

  Move(table, sel, Motion::kLineEnd, false, kTab);
  EXPECT_EQ(Cur(table, sel), Index{13});
  Move(table, sel, Motion::kRight, false, kTab);
  EXPECT_EQ(Cur(table, sel), Index{14});
  Move(table, sel, Motion::kLineStart, false, kTab);
  EXPECT_EQ(Cur(table, sel), Index{0});
  Move(table, sel, Motion::kDocEnd, false, kTab);
  EXPECT_EQ(Cur(table, sel), std::ssize(doc));

  sel.Set(Selection{0, 0});
  Move(table, sel, Motion::kRight, false, kTab, 5);
  EXPECT_EQ(Cur(table, sel), Index{5});
  Move(table, sel, Motion::kLeft, false, kTab, 3);
  EXPECT_EQ(Cur(table, sel), Index{2});
  Move(table, sel, Motion::kLeft, false, kTab, 999);
  EXPECT_EQ(Cur(table, sel), Index{0});
  Move(table, sel, Motion::kDown, false, kTab, 2);
  EXPECT_EQ(LineAt(table, Cur(table, sel)), Index{2});

  PieceTable words = MakeTable("hello world again\n");
  SelectionSet wsel;

  const auto expect_range = [&](const char* what, Index from, Index to) {
    if ((wsel.Primary().From() != from) || (wsel.Primary().To() != to)) {
      ++common::g_test_failures;
      std::cerr << "FAIL [" << common::g_test_case << "] " << what << " got ["
                << wsel.Primary().From() << "," << wsel.Primary().To() << ") want [" << from << ","
                << to << ")" << std::endl;
    } else {
      ++common::g_test_checks;
    }
  };

  wsel.Set(MinWidth1(words, Selection{0, 0}));
  Move(words, wsel, Motion::kWordNext, false, kTab);
  expect_range("w", 0, 6);
  EXPECT_EQ(Cur(words, wsel), Index{5});
  Move(words, wsel, Motion::kWordNext, false, kTab);
  expect_range("ww", 6, 12);
  EXPECT_EQ(Cur(words, wsel), Index{11});
  Move(words, wsel, Motion::kWordNext, false, kTab);
  expect_range("www", 12, 17);

  wsel.Set(MinWidth1(words, Selection{0, 0}));
  Move(words, wsel, Motion::kWordEnd, false, kTab);
  expect_range("e", 0, 5);
  EXPECT_EQ(Cur(words, wsel), Index{4});
  Move(words, wsel, Motion::kWordEnd, false, kTab);
  expect_range("ee", 5, 11);
  Move(words, wsel, Motion::kWordEnd, false, kTab);
  expect_range("eee", 11, 17);

  wsel.Set(MinWidth1(words, Selection{1, 1}));
  Move(words, wsel, Motion::kWordEnd, false, kTab);
  expect_range("le", 1, 5);
  wsel.Set(MinWidth1(words, Selection{1, 1}));
  Move(words, wsel, Motion::kWordNext, false, kTab);
  expect_range("lw", 1, 6);

  wsel.Set(MinWidth1(words, Selection{6, 6}));
  Move(words, wsel, Motion::kWordPrev, false, kTab);
  expect_range("wlb", 0, 6);
  EXPECT_TRUE(wsel.Primary().Backward());
  EXPECT_EQ(Cur(words, wsel), Index{0});

  PieceTable two = MakeTable("one two three four\nalpha beta gamma\n");
  SelectionSet bsel;
  const auto expect_sel = [&](const char* what, Index anchor, Index head) {
    if ((bsel.Primary().anchor != anchor) || (bsel.Primary().head != head)) {
      ++common::g_test_failures;
      std::cerr << "FAIL [" << common::g_test_case << "] " << what << " got ("
                << bsel.Primary().anchor << "," << bsel.Primary().head << ") want (" << anchor
                << "," << head << ")" << std::endl;
    } else {
      ++common::g_test_checks;
    }
  };

  bsel.Set(MinWidth1(two, Selection{10, 10}));
  Move(two, bsel, Motion::kWordPrev, false, kTab);
  expect_sel("b", 11, 8);
  Move(two, bsel, Motion::kWordPrev, false, kTab);
  expect_sel("bb", 8, 4);

  bsel.Set(MinWidth1(two, Selection{8, 8}));
  Move(two, bsel, Motion::kWordPrev, false, kTab);
  expect_sel("t|b", 8, 4);

  bsel.Set(MinWidth1(two, Selection{10, 10}));
  Move(two, bsel, Motion::kWordPrev, false, kTab, 2);
  expect_sel("2b", 8, 4);

  bsel.Set(MinWidth1(two, Selection{4, 4}));
  Move(two, bsel, Motion::kWordNext, false, kTab);
  expect_sel("t|w", 4, 8);
  Move(two, bsel, Motion::kWordPrev, false, kTab);
  expect_sel("t|wb", 8, 4);

  Move(two, bsel, Motion::kWordNext, false, kTab);
  expect_sel("t|wbw", 4, 8);

  bsel.Set(MinWidth1(two, Selection{10, 10}));
  Move(two, bsel, Motion::kWordPrev, false, kTab);
  Move(two, bsel, Motion::kWordEnd, false, kTab);
  expect_sel("be", 8, 13);

  bsel.Set(MinWidth1(two, Selection{4, 4}));
  Move(two, bsel, Motion::kWordEnd, false, kTab);
  expect_sel("t|e", 4, 7);
  Move(two, bsel, Motion::kWordPrev, false, kTab);
  expect_sel("t|eb", 7, 4);

  bsel.Set(MinWidth1(two, Selection{19, 19}));
  Move(two, bsel, Motion::kWordPrev, false, kTab);
  expect_sel("a|b", 18, 14);

  bsel.Set(MinWidth1(two, Selection{0, 0}));
  Move(two, bsel, Motion::kWordPrev, false, kTab);
  expect_sel("^b", 1, 0);

  bsel.Set(MinWidth1(two, Selection{10, 10}));
  Move(two, bsel, Motion::kWordPrevEnd, false, kTab);
  expect_sel("gE", 11, 7);
  Move(two, bsel, Motion::kWordPrevEnd, false, kTab);
  expect_sel("gEgE", 7, 3);
  bsel.Set(MinWidth1(two, Selection{6, 6}));
  Move(two, bsel, Motion::kWordPrevEnd, false, kTab);
  expect_sel("o|gE", 7, 3);
  bsel.Set(MinWidth1(two, Selection{19, 19}));
  Move(two, bsel, Motion::kWordPrevEnd, false, kTab);
  expect_sel("a|gE", 18, 13);

  {
    SelectionSet walk;
    walk.Set(MinWidth1(two, Selection{17, 17}));
    Index previous = Cur(two, walk);
    for (int step = 0; step < 12; ++step) {
      Move(two, walk, Motion::kWordPrev, false, kTab);
      const Index now = Cur(two, walk);
      if (now == previous) {
        EXPECT_TRUE(now == 0);
        break;
      }
      EXPECT_TRUE(now < previous);
      previous = now;
    }
    EXPECT_EQ(Cur(two, walk), Index{0});
  }

  wsel.Set(MinWidth1(words, Selection{0, 0}));
  Move(words, wsel, Motion::kWordNext, false, kTab, 2);
  expect_range("2w", 6, 12);

  PieceTable lead = MakeTable("   ab cd\n");
  SelectionSet lsel;
  lsel.Set(MinWidth1(lead, Selection{0, 0}));
  Move(lead, lsel, Motion::kWordNext, false, kTab);
  EXPECT_EQ(lsel.Primary().From(), Index{0});
  EXPECT_EQ(lsel.Primary().To(), Index{3});
  lsel.Set(MinWidth1(lead, Selection{0, 0}));
  Move(lead, lsel, Motion::kWordEnd, false, kTab);
  EXPECT_EQ(lsel.Primary().From(), Index{0});
  EXPECT_EQ(lsel.Primary().To(), Index{5});

  for (const Motion motion : {Motion::kWordNext, Motion::kWordEnd, Motion::kLongWordNext,
                              Motion::kLongWordEnd}) {
    SelectionSet walk;
    walk.Set(MinWidth1(words, Selection{0, 0}));
    Index previous = Cur(words, walk);
    for (int step = 0; step < 12; ++step) {
      Move(words, walk, motion, false, kTab);
      const Index now = Cur(words, walk);
      if (now == previous) {
        EXPECT_TRUE(now >= DocLength(words) - 1);
        break;
      }
      EXPECT_TRUE(now > previous);
      previous = now;
    }
  }

  PieceTable uni = MakeTable(std::string("ab") + std::string(kFamily) + "cd");
  SelectionSet usel;
  usel.Set(Selection{2, 2});
  Move(uni, usel, Motion::kRight, false, kTab);
  EXPECT_EQ(Cur(uni, usel), 2 + std::ssize(kFamily));
  Move(uni, usel, Motion::kLeft, false, kTab);
  EXPECT_EQ(Cur(uni, usel), Index{2});
  EXPECT_EQ(usel.Primary().To(), 2 + std::ssize(kFamily));
}

void BlockCursorInvariant() {
  TEST_CASE("block cursor invariant");
  PieceTable table = MakeTable("abc\ndef\n");

  SelectionSet sel;
  sel.Set(Selection{0, 0});
  EXPECT_TRUE(sel.Primary().IsEmpty());
  sel.EnsureBlockCursors(table);
  EXPECT_FALSE(sel.Primary().IsEmpty());
  EXPECT_EQ(sel.Primary().Range().front(), Index{0});
  EXPECT_EQ(sel.Primary().To(), Index{1});

  for (Index at = 0; at <= DocLength(table); ++at) {
    SelectionSet probe;
    probe.Set(Selection{at, at});
    probe.EnsureBlockCursors(table);
    const Selection& s = probe.Primary();
    const Index cursor = CursorOf(table, s);
    if (s.IsEmpty()) {
      EXPECT_EQ(at, DocLength(table));
      continue;
    }
    EXPECT_TRUE((cursor >= s.From()) && (cursor < s.To()));
  }

  SelectionSet many;
  many.Replace(table, {{0, 0}, {1, 1}, {2, 2}});
  many.EnsureBlockCursors(table);
  EXPECT_EQ(many.Size(), size_t{3});
  SelectionSet same;
  same.Replace(table, {{1, 1}, {1, 1}});
  same.EnsureBlockCursors(table);
  EXPECT_EQ(same.Size(), size_t{1});

  many.SetPrimary(0);
  many.RotatePrimary(1);
  EXPECT_EQ(many.PrimaryIndex(), size_t{1});
  many.RotatePrimary(-1);
  many.RotatePrimary(-1);
  EXPECT_EQ(many.PrimaryIndex(), size_t{2});
  many.RotatePrimary(1);
  EXPECT_EQ(many.PrimaryIndex(), size_t{0});
}

void MultiCursorEditing() {
  TEST_CASE("multi-cursor editing");
  PieceTable table = MakeTable("aaa\nbbb\nccc\nddd\n");
  const std::string original = AssembleDocContents(table);

  SelectionSet sel;
  sel.Replace(table, {{0, 0}, {4, 4}, {8, 8}, {12, 12}});
  EXPECT_EQ(sel.Size(), size_t{4});

  ExpectOk(InsertAtCursors(">> ", table, sel), "insert at four cursors");
  EXPECT_EQ(AssembleDocContents(table), std::string(">> aaa\n>> bbb\n>> ccc\n>> ddd\n"));
  EXPECT_EQ(sel.Size(), size_t{4});
  for (size_t i = 0; i < sel.Size(); ++i) {
    EXPECT_EQ(sel.Ranges()[i].head, static_cast<Index>(i) * 7 + 3);
    EXPECT_TRUE(sel.Ranges()[i].IsEmpty());
  }

  std::vector<Edit> edits;
  ExpectOk(Undo(table, &edits), "undo a multi-cursor insert");
  EXPECT_EQ(AssembleDocContents(table), original);
  EXPECT_EQ(edits.size(), size_t{4});

  ExpectOk(Redo(table, &edits), "redo a multi-cursor insert");
  EXPECT_EQ(AssembleDocContents(table), std::string(">> aaa\n>> bbb\n>> ccc\n>> ddd\n"));
  EXPECT_EQ(edits.size(), size_t{4});

  SelectionSet back;
  back.Replace(table, {{3, 3}, {10, 10}, {17, 17}, {24, 24}});
  ExpectOk(DeleteBackwardAtCursors(table, back), "backspace at four cursors");
  EXPECT_EQ(AssembleDocContents(table), std::string(">>aaa\n>>bbb\n>>ccc\n>>ddd\n"));
  ExpectOk(Undo(table, &edits), "undo a multi-cursor backspace");
  EXPECT_EQ(AssembleDocContents(table), std::string(">> aaa\n>> bbb\n>> ccc\n>> ddd\n"));

  SelectionSet ranges;
  ranges.Replace(table, {{0, 2}, {7, 9}, {14, 16}, {21, 23}});
  ExpectOk(InsertAtCursors("##", table, ranges), "replace four selections");
  EXPECT_EQ(AssembleDocContents(table), std::string("## aaa\n## bbb\n## ccc\n## ddd\n"));
  ExpectOk(Undo(table, &edits), "undo a multi-cursor replace");
  EXPECT_EQ(AssembleDocContents(table), std::string(">> aaa\n>> bbb\n>> ccc\n>> ddd\n"));

  {
    PieceTable t = MakeTable("0123456789");
    SelectionSet s;
    s.Replace(t, {{3, 5}, {7, 9}});
    ExpectOk(DeleteSelections(t, s), "two-cursor delete");
    EXPECT_EQ(AssembleDocContents(t), std::string{"012569"});
    ExpectOk(Undo(t), "undo two-cursor delete");
    EXPECT_EQ(AssembleDocContents(t), std::string{"0123456789"});
  }
  {
    PieceTable t = MakeTable("0123456789");
    SelectionSet s;
    s.Replace(t, {{4, 4}, {6, 6}});
    ExpectOk(DeleteBackwardAtCursors(t, s), "two-cursor backspace");
    EXPECT_EQ(AssembleDocContents(t), std::string{"01246789"});
    ExpectOk(Undo(t), "undo two-cursor backspace");
    EXPECT_EQ(AssembleDocContents(t), std::string{"0123456789"});
  }
  {
    PieceTable t = MakeTable("01234XYZ");
    ExpectOk(Insert("a", 5, t), "insert high");
    ExpectOk(Insert("b", 4, t), "insert low");
    EXPECT_EQ(AssembleDocContents(t), std::string{"0123b4aXYZ"});
    ExpectOk(Undo(t), "undo the low insert");
    ExpectOk(Undo(t), "undo the high insert");
    EXPECT_EQ(AssembleDocContents(t), std::string{"01234XYZ"});
  }
  {
    PieceTable t = MakeTable("abcdef");
    ExpectOk(Delete(Interval(4, 5), t), "backspace once");
    ExpectOk(Delete(Interval(3, 4), t), "backspace twice");
    ExpectOk(Delete(Interval(2, 3), t), "backspace thrice");
    EXPECT_EQ(AssembleDocContents(t), std::string{"abf"});
    EXPECT_EQ(UndoDepth(t), Index{1});
    ExpectOk(Undo(t), "undo the merged backspaces");
    EXPECT_EQ(AssembleDocContents(t), std::string{"abcdef"});
  }
}

void MultiCursorFuzz(Rng& rng) {
  TEST_CASE("multi-cursor fuzz");
  std::string brute;
  for (int i = 0; i < 40; ++i) brute += "line " + std::to_string(i) + " content\n";
  PieceTable table = MakeTable(brute);

  SelectionSet sel;
  std::vector<Selection> carets;
  for (Index line = 0; line < LineCount(table); line += 3) {
    carets.push_back(Selection{LineStart(table, line), LineStart(table, line)});
  }
  sel.Replace(table, carets);

  for (int step = 0; step < 60; ++step) {
    const std::string_view txt = (rng.Pick(0, 1) == 0) ? "x" : "yz";
    std::vector<Index> at;
    for (const Selection& s : sel.Ranges()) at.push_back(s.head);
    ExpectOk(InsertAtCursors(txt, table, sel), "fuzz insert at cursors");
    for (auto it = at.rbegin(); it != at.rend(); ++it) {
      brute.insert(static_cast<size_t>(*it), txt);
    }
    ExpectSameDoc(table, brute, "multi-cursor fuzz");
  }

  for (int step = 0; step < 60; ++step) {
    std::vector<Edit> edits;
    if (!CanUndo(table)) break;
    ExpectOk(Undo(table, &edits), "fuzz undo");
  }
  EXPECT_EQ(LineCount(table), Index{41});
}

// One cursor per line and one edit per line at once -- the shape that used to
// cost cursors x edits. Where every cursor lands is checked against the fold
// MapThroughEdits used to perform, edit by edit, so a batch that the composed
// map handles differently shows up here as a moved cursor.
void MultiCursorCommentMappingIsUnchanged() {
  TEST_CASE("multi-cursor comment mapping");
  constexpr Index kLines = 200;
  constexpr Index kLineBytes = 3;  // "a;\n"

  Editor tc;
  tc.doc.file = "probe.cpp";
  tc.doc.view.rows = 10;
  tc.doc.view.columns = 60;
  std::string doc;
  for (Index line = 0; line < kLines; ++line) doc += "a;\n";
  ResetToOriginal(tc.doc.table, doc);

  std::vector<Selection> cursors;
  for (Index line = 0; line < kLines; ++line) {
    cursors.push_back(Selection{line * kLineBytes, (line * kLineBytes) + 1, -1});
  }
  tc.doc.selections.Replace(tc.doc.table, cursors);
  const std::vector<Selection> before = tc.doc.selections.Ranges();

  // What ToggleComments applies: "// " at the start of every line, highest
  // line first, so each edit is in the coordinates of the document as it stood
  // before the command.
  std::vector<Edit> edits;
  for (Index line = kLines; line-- > 0;) {
    Edit e;
    e.start_byte = line * kLineBytes;
    e.old_end_byte = e.start_byte;
    e.new_end_byte = e.start_byte + 3;
    edits.push_back(e);
  }
  std::vector<Selection> want = before;
  for (Selection& s : want) {
    s.anchor = FoldThroughEdits(s.anchor, edits);
    s.head = FoldThroughEdits(s.head, edits);
  }

  const Index fallbacks = MapThroughEditsFallbacks();
  RunCommands(tc, {"toggle_comments"});

  std::string commented;
  for (Index line = 0; line < kLines; ++line) commented += "// a;\n";
  EXPECT_EQ(AssembleDocContents(tc.doc.table), commented);

  SelectionSet expected;
  expected.Replace(tc.doc.table, want);
  EXPECT_EQ(tc.doc.selections.Size(), expected.Size());
  for (std::size_t i = 0; (i < tc.doc.selections.Size()) && (i < expected.Size()); ++i) {
    EXPECT_TRUE(tc.doc.selections.Ranges()[i] == expected.Ranges()[i]);
  }
  // And it went through the composed map, not the fold it is measured against.
  EXPECT_EQ(MapThroughEditsFallbacks(), fallbacks);
}

// PutCursor re-holds the anchor grapheme when the cursor crosses behind it --
// a forward selection covers [anchor, anchor+1), so a direction flip has to
// take that grapheme from the other side or it silently drops out. A
// zero-width insert-mode caret (selection.h) holds no grapheme at all, so
// there is nothing to re-hold and the step annexed the one *in front* of the
// caret: `i` then A-I (extend_line_up) selected one grapheme too many, and the
// following `d` deleted a character that was never highlighted.
void ExtendingFromAnInsertCaretDoesNotAnnexTheGraphemeInFront() {
  TEST_CASE("extending backwards from an insert caret holds no extra grapheme");

  auto text_of = [](const Editor& ed, const Selection& s) {
    return AssembleDocContents(ed.doc.table)
        .substr(static_cast<size_t>(s.From()), static_cast<size_t>(s.To() - s.From()));
  };

  // "abcd\nefgh\n": a0 b1 c2 d3 \n4 e5 f6 g7 h8 \n9.
  const std::string kDoc = "abcd\nefgh\n";

  // (a) The end-to-end trace from the report: block cursor on `g`, then
  // `i`, A-I, `d`.
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, kDoc);
    ed.doc.view.rows = 5;
    ed.doc.view.columns = 20;
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{7, 7, -1}));
    EXPECT_TRUE(ed.doc.selections.Primary() == (Selection{7, 8, -1}));

    RunCommands(ed, {"insert_mode"});
    EXPECT_TRUE(ed.doc.selections.Primary().IsEmpty());
    EXPECT_EQ(Cur(ed), Index{7});

    // A-I is ["extend_line_up", "normal_mode"]; extend_line_up runs while the
    // mode is still insert, so it sees the zero-width caret.
    RunCommands(ed, {"extend_line_up"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{2});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{7});
    EXPECT_EQ(text_of(ed, ed.doc.selections.Primary()), std::string("cd\nef"));

    RunCommands(ed, {"normal_mode"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{2});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{7});

    RunCommands(ed, {"delete_selection"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("abgh\n"));
  }

  // (b) The primitive underneath it: a caret at 7, extended one grapheme left.
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, kDoc);
    SelectionSet sel;
    sel.Set(Selection{7, 7, -1});
    Move(ed.doc.table, sel, Motion::kLeft, true, ed.doc.tab_width, 1);
    EXPECT_TRUE(sel.Primary() == (Selection{7, 6, -1}));
    EXPECT_EQ(sel.Primary().From(), Index{6});
    EXPECT_EQ(sel.Primary().To(), Index{7});
  }

  // (c) Controls: from a *block* cursor the anchor step is still right, in
  // both directions.
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, kDoc);

    // Forward selection [7,8) on `g`, extended left: the anchor grapheme has
    // to be re-held from the far side, so the result stays two wide.
    SelectionSet sel;
    sel.Set(Selection{7, 8, -1});
    Move(ed.doc.table, sel, Motion::kLeft, true, ed.doc.tab_width, 1);
    EXPECT_TRUE(sel.Primary() == (Selection{8, 6, -1}));

    // Same block cursor, the report's A-I motion: [2,8), not [2,7).
    SelectionSet up;
    up.Set(Selection{7, 8, -1});
    Move(ed.doc.table, up, Motion::kUp, true, ed.doc.tab_width, 1);
    EXPECT_EQ(up.Primary().From(), Index{2});
    EXPECT_EQ(up.Primary().To(), Index{8});

    // Backward selection [6,8) crossing its anchor going right: the mirror
    // branch, untouched.
    SelectionSet back;
    back.Set(Selection{8, 6, -1});
    Move(ed.doc.table, back, Motion::kRight, true, ed.doc.tab_width, 1);
    EXPECT_TRUE(back.Primary() == (Selection{8, 7, -1}));
    Move(ed.doc.table, back, Motion::kRight, true, ed.doc.tab_width, 1);
    EXPECT_TRUE(back.Primary() == (Selection{7, 9, -1}));
  }

  // (d) The annexed grapheme is taken with NextGraphemeBoundary, so the byte
  // count it stole depended on what sat in front of the caret. Both a
  // multi-byte scalar and a multi-scalar cluster.
  {
    // "ab" + U+1F600 (4 bytes, [2,6)) + "cd\n".
    Editor ed;
    ResetToOriginal(ed.doc.table, "ab\xF0\x9F\x98\x80" "cd\n");
    SelectionSet sel;
    sel.Set(Selection{2, 2, -1});
    Move(ed.doc.table, sel, Motion::kLeft, true, ed.doc.tab_width, 1);
    EXPECT_TRUE(sel.Primary() == (Selection{2, 1, -1}));
    EXPECT_EQ(AssembleDocContents(ed.doc.table)
                  .substr(static_cast<size_t>(sel.Primary().From()),
                          static_cast<size_t>(sel.Primary().To() - sel.Primary().From())),
              std::string("b"));

    // The block cursor sitting on that same emoji still re-holds all 4 bytes.
    SelectionSet block;
    block.Set(Selection{2, 6, -1});
    Move(ed.doc.table, block, Motion::kLeft, true, ed.doc.tab_width, 1);
    EXPECT_TRUE(block.Primary() == (Selection{6, 1, -1}));
  }
  {
    // "ab" + "e" + U+0301 (one cluster, [2,5)) + "cd\n".
    Editor ed;
    ResetToOriginal(ed.doc.table, "abe\xCC\x81" "cd\n");
    SelectionSet sel;
    sel.Set(Selection{2, 2, -1});
    Move(ed.doc.table, sel, Motion::kLeft, true, ed.doc.tab_width, 1);
    EXPECT_TRUE(sel.Primary() == (Selection{2, 1, -1}));

    SelectionSet block;
    block.Set(Selection{2, 5, -1});
    Move(ed.doc.table, block, Motion::kLeft, true, ed.doc.tab_width, 1);
    EXPECT_TRUE(block.Primary() == (Selection{5, 1, -1}));
  }
}

}  // namespace koi
