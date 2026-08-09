// Tests for unicode.cpp: grapheme cluster boundaries, display widths, and the
// column mapping the editor lays out with.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

namespace {

constexpr std::string_view kAcute = "\xCC\x81";

constexpr std::string_view kFlagJP = "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5";

constexpr std::string_view kDevanagari = "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7";

}  // namespace

void UnicodeBoundaries() {
  TEST_CASE("grapheme boundaries");
  const std::string doc =
      std::string("ab") + std::string(kEAcute) + std::string(kCJK) + std::string(kFlagJP) +
      std::string(kFamily) + std::string(kDevanagari) + "z";
  PieceTable table = MakeTable(doc);

  const Index e_acute = 2;
  const Index cjk = e_acute + std::ssize(kEAcute);
  const Index flag = cjk + std::ssize(kCJK);
  const Index family = flag + std::ssize(kFlagJP);
  const Index devanagari = family + std::ssize(kFamily);
  const Index z = devanagari + std::ssize(kDevanagari);

  for (Index at : {Index{0}, Index{1}, e_acute, cjk, flag, family, devanagari, z, z + 1}) {
    EXPECT_TRUE(IsGraphemeBoundary(table, at));
  }
  for (Index at : {e_acute + 1,
                   cjk + 1, cjk + 2,
                   flag + 4,
                   family + 4,
                   family + 7,
                   devanagari + 3}) {
    EXPECT_FALSE(IsGraphemeBoundary(table, at));
  }

  EXPECT_EQ(NextGraphemeBoundary(table, e_acute), cjk);
  EXPECT_EQ(NextGraphemeBoundary(table, flag), family);
  EXPECT_EQ(NextGraphemeBoundary(table, family), devanagari);
  EXPECT_EQ(PrevGraphemeBoundary(table, cjk), e_acute);
  EXPECT_EQ(PrevGraphemeBoundary(table, devanagari), family);
  EXPECT_EQ(NextGraphemeBoundary(table, z + 1), z + 1);
  EXPECT_EQ(PrevGraphemeBoundary(table, 0), Index{0});

  EXPECT_EQ(CountGraphemes(table, {0, DocLength(table)}), Index{8});
  EXPECT_EQ(DocLength(table), std::ssize(doc));
  EXPECT_EQ(ReadDocRange(table, {0, DocLength(table)}), doc);
}

void UnicodeEditsAreRejectedInsideClusters() {
  TEST_CASE("edits inside a cluster are refused");
  const std::string doc = std::string("ab") + std::string(kFamily) + "z";
  PieceTable table = MakeTable(doc);
  const std::string before = AssembleDocContents(table);

  const auto is_boundary_error = [](const ErrorCtx& e) {
    return e.ec.value() == static_cast<int>(PieceTableErrorCode::kDocPosNotOnGraphemeBoundary);
  };

  EXPECT_TRUE(is_boundary_error(Insert("x", 4, table)));
  EXPECT_TRUE(is_boundary_error(Delete({4, 6}, table)));
  EXPECT_TRUE(is_boundary_error(Delete({2, 6}, table)));
  EXPECT_TRUE(is_boundary_error(Replace("xx", {4, 6}, table)));
  ExpectSameDoc(table, before, "a refused edit must not mutate the document");

  EXPECT_TRUE(Insert("\xFF\xFE", 2, table).ec.value() ==
              static_cast<int>(PieceTableErrorCode::kMalformedUtf8Input));
  EXPECT_TRUE(Insert("\xE6\x97", 2, table).ec.value() ==
              static_cast<int>(PieceTableErrorCode::kMalformedUtf8Input));
  ExpectSameDoc(table, before, "malformed input must not mutate the document");

  ExpectOk(Insert(kCJK, 2, table), "insert before a cluster");
  ExpectOk(Delete({2, 2 + std::ssize(kCJK)}, table), "delete a whole cluster");
  ExpectSameDoc(table, before, "insert then delete of a whole cluster round-trips");
  EXPECT_TRUE(IsWellFormedUtf8(AssembleDocContents(table)));
}

void UnicodeClusterSpanningPieces() {
  TEST_CASE("a cluster split across two pieces");
  PieceTable table = MakeTable("abc");
  ExpectOk(Insert(kAcute, 3, table), "append a combining acute");

  EXPECT_TRUE(PieceCount(table) >= 2);
  EXPECT_EQ(AssembleDocContents(table), std::string("abc") + std::string(kAcute));

  EXPECT_FALSE(IsGraphemeBoundary(table, 3));
  EXPECT_TRUE(IsGraphemeBoundary(table, 2));
  EXPECT_EQ(PrevGraphemeBoundary(table, DocLength(table)), Index{2});
  EXPECT_EQ(CountGraphemes(table, {0, DocLength(table)}), Index{3});

  EXPECT_TRUE(Insert("x", 3, table).ec.value() ==
              static_cast<int>(PieceTableErrorCode::kDocPosNotOnGraphemeBoundary));

  ExpectOk(Delete({2, DocLength(table)}, table), "delete the spanning cluster");
  EXPECT_EQ(AssembleDocContents(table), std::string("ab"));
}

void UnicodeEditingRoundTrip(Rng& rng) {
  TEST_CASE("random editing keeps the document well-formed");
  std::string doc;
  for (int i = 0; i < 200; ++i) {
    doc += "ab";
    doc += kEAcute;
    doc += kCJK;
    doc += kFamily;
    doc += kFlagJP;
    doc += kDevanagari;
    doc += '\n';
  }
  PieceTable table = MakeTable(doc);
  std::string brute_force = doc;

  const std::string_view inserts[] = {kCJK, kFamily, kFlagJP, kDevanagari, "hello", kEAcute};
  for (int step = 0; step < 400; ++step) {
    const Index len = DocLength(table);
    if (len < 32) break;
    const Index raw = rng.Pick(0, len - 1);
    const Index at = SnapToGraphemeBoundary(table, raw);
    EXPECT_TRUE(IsGraphemeBoundary(table, at));

    if (rng.Pick(0, 1) == 0) {
      const std::string_view txt = inserts[rng.Pick(0, std::ssize(inserts) - 1)];
      ExpectOk(InsertFunctor(txt, {at, at + std::ssize(txt)}, table, brute_force), "unicode insert");
    } else {
      const Index end = NextGraphemeBoundary(table, at);
      if (end <= at) continue;
      ExpectOk(DeleteFunctor("", {at, end}, table, brute_force), "unicode delete");
    }
  }
  EXPECT_TRUE(IsWellFormedUtf8(AssembleDocContents(table)));
  ExpectSameDoc(table, brute_force, "after random unicode editing");
}

void DisplayWidths() {
  TEST_CASE("display width");
  EXPECT_EQ(GraphemeWidth("a"), 1);
  EXPECT_EQ(GraphemeWidth(kEAcute), 1);
  EXPECT_EQ(GraphemeWidth(kCJK), 2);
  EXPECT_EQ(GraphemeWidth(kFlagJP), 2);
  EXPECT_EQ(GraphemeWidth(kFamily), 2);
  EXPECT_EQ(GraphemeWidth(kAcute), 1);
  EXPECT_EQ(GraphemeWidth(kDevanagari), 1);
  EXPECT_EQ(GraphemeWidth(""), 0);
  EXPECT_EQ(GraphemeWidth("\t"), 0);

  EXPECT_EQ(DisplayWidth("abc", 4), Index{3});
  EXPECT_EQ(DisplayWidth("\t", 4), Index{4});
  EXPECT_EQ(DisplayWidth("a\t", 4), Index{4});
  EXPECT_EQ(DisplayWidth("abc\t", 4), Index{4});
  EXPECT_EQ(DisplayWidth("abcd\t", 4), Index{8});
  EXPECT_EQ(DisplayWidth("\t", 4, 1), Index{3});
  EXPECT_EQ(DisplayWidth(std::string(kCJK) + "a", 4), Index{3});
  EXPECT_EQ(DisplayWidth(kFamily, 4), Index{2});
}

void ColumnMapping() {
  TEST_CASE("column mapping");
  const std::string doc = std::string("ab\t") + std::string(kCJK) + std::string(kFamily) + "z\nnext\n";
  PieceTable table = MakeTable(doc);
  constexpr Index kTab = 4;

  EXPECT_EQ(ColumnForByte(table, 0, kTab), Index{0});
  EXPECT_EQ(ColumnForByte(table, 2, kTab), Index{2});
  EXPECT_EQ(ColumnForByte(table, 3, kTab), Index{4});
  EXPECT_EQ(ColumnForByte(table, 3 + std::ssize(kCJK), kTab), Index{6});
  EXPECT_EQ(ColumnForByte(table, 3 + std::ssize(kCJK) + std::ssize(kFamily), kTab), Index{8});

  EXPECT_EQ(ByteForColumn(table, 0, 4, kTab), Index{3});
  EXPECT_EQ(ByteForColumn(table, 0, 5, kTab), Index{3});
  EXPECT_EQ(ByteForColumn(table, 0, 6, kTab), 3 + std::ssize(kCJK));
  EXPECT_EQ(ByteForColumn(table, 0, 0, kTab), Index{0});
  EXPECT_EQ(ByteForColumn(table, 0, 999, kTab), LineContentRange(table, 0).back() + 1);

  const Interval line0 = LineContentRange(table, 0);
  for (Index at = line0.front(); at <= line0.back() + 1;) {
    EXPECT_EQ(ByteForColumn(table, 0, ColumnForByte(table, at, kTab), kTab), at);
    const Index next = NextGraphemeBoundary(table, at);
    if (next <= at) break;
    at = next;
  }
}

void CrlfLines() {
  TEST_CASE("CRLF line content");
  PieceTable table = MakeTable("alpha\r\nbravo\ncharlie");
  EXPECT_EQ(LineCount(table), Index{3});
  EXPECT_EQ(LineRange(table, 0).back() + 1, Index{7});
  EXPECT_EQ(LineContentRange(table, 0).back() + 1, Index{5});
  EXPECT_EQ(ReadDocRange(table, LineContentRange(table, 0)), std::string("alpha"));
  EXPECT_EQ(ReadDocRange(table, LineContentRange(table, 1)), std::string("bravo"));
  EXPECT_EQ(ReadDocRange(table, LineContentRange(table, 2)), std::string("charlie"));

  EXPECT_FALSE(IsGraphemeBoundary(table, 6));

  std::string reused = "leftover";
  ReadDocRangeInto(table, LineContentRange(table, 1), reused);
  EXPECT_EQ(reused, std::string("bravo"));
}

void GraphemeQueriesMatchAFullWalk() {
  TEST_CASE("grapheme queries agree with a full walk");

  const std::string text = std::string("ab\r\ncd\n") + std::string(kEAcute) + "f\r\n" +
                           std::string(kCJK) + "\r\n" + std::string(kFlagJP) + "x\n" +
                           std::string(kFamily) + "\r\n" + "z\r" + "\n" + "tail";
  PieceTable table = MakeTable(text);
  const Index len = DocLength(table);
  EXPECT_EQ(len, std::ssize(text));

  std::vector<bool> boundary(static_cast<size_t>(len) + 1, false);
  boundary[0] = true;
  for (std::size_t at = 0; at < text.size(); at = NextGraphemeInString(text, at)) {
    boundary[at] = true;
  }
  boundary[static_cast<size_t>(len)] = true;

  EXPECT_FALSE(boundary[3]);
  EXPECT_TRUE(boundary[2]);

  for (Index pos = 0; pos <= len; ++pos) {
    const auto at = static_cast<size_t>(pos);
    EXPECT_EQ(IsGraphemeBoundary(table, pos), boundary[at]);

    Index expect_snap = pos;
    while ((expect_snap < len) && !boundary[static_cast<size_t>(expect_snap)]) ++expect_snap;
    EXPECT_EQ(SnapToGraphemeBoundary(table, pos), expect_snap);

    Index expect_next = pos + 1;
    while ((expect_next < len) && !boundary[static_cast<size_t>(expect_next)]) ++expect_next;
    EXPECT_EQ(NextGraphemeBoundary(table, pos), std::min(expect_next, len));

    Index expect_prev = std::max<Index>(0, pos - 1);
    while ((expect_prev > 0) && !boundary[static_cast<size_t>(expect_prev)]) --expect_prev;
    EXPECT_EQ(PrevGraphemeBoundary(table, pos), expect_prev);
  }
}

void ClustersLongerThanTheReadWindowStillSegment() {
  // SegmentAround reads a window of kGraphemeContextBytes (128) either side of
  // the position and grew it when the *left* edge landed inside a cluster, but
  // never when the right one did: u8_grapheme_next answers `end` both for a
  // cluster that ends there and for one it ran out of bytes to segment, so a
  // cluster running past the window was reported as ending at the window edge.
  // NextGraphemeBoundary and SnapToGraphemeBoundary handed that truncated
  // position back, IsGraphemeBoundary -- which re-centres its own window and
  // sees the cluster's real start -- rejected it, and Apply, which checks every
  // change endpoint with IsGraphemeBoundary, then refused every edit at it. The
  // caret could sit somewhere from which nothing could be typed or deleted, and
  // nothing said so.
  const auto marks = [](int n) {
    std::string s = "x";
    for (int i = 0; i < n; ++i) s += kAcute;
    return s;
  };

  TEST_CASE("a grapheme cluster wider than the read window is not cut at the window edge");
  // 63 marks is 127 bytes and fit inside the old window; 64 is 129 and did not.
  for (const int n : {63, 64, 100, 200}) {
    const std::string cluster = marks(n);
    const auto cluster_end = static_cast<Index>(std::ssize(cluster));
    PieceTable table = MakeTable(cluster + "y\n");

    const Index next = NextGraphemeBoundary(table, 0);
    EXPECT_EQ(next, cluster_end);
    EXPECT_TRUE(IsGraphemeBoundary(table, next));
    EXPECT_EQ(CountGraphemes(table, {0, DocLength(table)}), Index{3});
    // Stepping right from 0 crosses the whole cluster in one step.
    EXPECT_EQ(NextGraphemeBoundary(table, next), cluster_end + 1);
    EXPECT_EQ(PrevGraphemeBoundary(table, cluster_end), Index{0});
  }

  TEST_CASE("a caret past a window-sized cluster can still edit");
  {
    const std::string cluster = marks(200);
    Editor ed;
    ResetToOriginal(ed.doc.table, cluster + "y\n");
    ed.doc.view.rows = 10;
    ed.doc.selections.Set(Selection{0, 0, -1});
    ApplyModeInvariants(ed);

    const std::string before = AssembleDocContents(ed.doc.table);
    RunCommands(ed, {"move_char_right"});
    EXPECT_TRUE(IsGraphemeBoundary(ed.doc.table, Cur(ed)));
    RunCommands(ed, {"delete_selection"});
    const std::string after = AssembleDocContents(ed.doc.table);
    EXPECT_TRUE(std::ssize(after) < std::ssize(before));
    EXPECT_EQ(after, cluster + "\n");
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("snapping from inside a window-sized cluster reaches a real boundary");
  {
    const std::string cluster = marks(200);
    const auto cluster_end = static_cast<Index>(std::ssize(cluster));
    PieceTable table = MakeTable(cluster + "y\n");
    // Odd offsets inside the cluster are code-point starts, even ones are
    // continuation bytes; both must snap out to the same real boundary.
    for (const Index mid : {Index{51}, Index{52}, cluster_end - 1}) {
      const Index snapped = SnapToGraphemeBoundary(table, mid);
      EXPECT_EQ(snapped, cluster_end);
      EXPECT_TRUE(IsGraphemeBoundary(table, snapped));

      const Index prev = PrevGraphemeBoundary(table, mid);
      const Index next = NextGraphemeBoundary(table, mid);
      EXPECT_EQ(prev, Index{0});
      EXPECT_EQ(next, cluster_end);
      EXPECT_TRUE(prev <= mid && mid <= next);
      EXPECT_TRUE(IsGraphemeBoundary(table, prev));
      EXPECT_TRUE(IsGraphemeBoundary(table, next));
    }
  }

  TEST_CASE("a window-sized cluster at the end of the document");
  {
    const std::string cluster = marks(200);
    const auto len = static_cast<Index>(std::ssize(cluster));
    PieceTable table = MakeTable(cluster);
    EXPECT_EQ(DocLength(table), len);
    EXPECT_EQ(NextGraphemeBoundary(table, 0), len);
    EXPECT_TRUE(IsGraphemeBoundary(table, NextGraphemeBoundary(table, 0)));
    EXPECT_EQ(SnapToGraphemeBoundary(table, 51), len);
    EXPECT_EQ(PrevGraphemeBoundary(table, len), Index{0});
    EXPECT_EQ(CountGraphemes(table, {0, len}), Index{1});
  }

  TEST_CASE("a cluster past the context cap terminates with an in-range answer");
  {
    // kMaxGraphemeContextBytes is 1 << 16, so 40,000 combining marks (80,001
    // bytes with the base) outruns the cap. Past it the window stops growing
    // and the answer is a guess again -- the residual noted in unicode.cpp. All
    // that is asserted here is that every query terminates and stays in range.
    const std::string cluster = marks(40000);
    PieceTable table = MakeTable(cluster + "y\n");
    const Index len = DocLength(table);
    EXPECT_TRUE(len > Index{1} << 16);

    const Index next = NextGraphemeBoundary(table, 0);
    EXPECT_TRUE(next > Index{0} && next <= len);
    const Index snapped = SnapToGraphemeBoundary(table, 51);
    EXPECT_TRUE(snapped > Index{0} && snapped <= len);
    const Index prev = PrevGraphemeBoundary(table, len / 2);
    EXPECT_TRUE(prev >= Index{0} && prev <= len / 2);
  }
}

}  // namespace koi
