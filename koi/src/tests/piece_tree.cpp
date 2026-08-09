// Tests for piece_tree.cpp: the B+tree under the piece table -- bulk-load
// shape, offset arithmetic, and the walks the document layer cannot reach.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

// The tree layer's own arithmetic, exercised where the document layer cannot
// reach it: bulk-load shape, outputs that alias the input, and the zero-line
// advance.
void PieceTreeArithmetic() {
  // Smallest number of children anywhere below the root. Build's whole promise
  // is that this is never 1: a node holding a single child is a level of
  // descent that discriminates nothing.
  const std::function<int(const pt::Node*, bool)> min_fill = [&](const pt::Node* n,
                                                                 bool is_root) -> int {
    if ((n == nullptr) || (n->height == 0)) {
      return (is_root || (n == nullptr)) ? 1000 : static_cast<int>(n->n);
    }
    const pt::Branch* b = static_cast<const pt::Branch*>(n);
    int worst = is_root ? 1000 : static_cast<int>(n->n);
    for (int i = 0; i < b->n; ++i) worst = std::min(worst, min_fill(b->child[i], false));
    return worst;
  };

  TEST_CASE("piece tree: a bulk load never strands a single child in the last node");
  // Rounding the group size up is greedy filling in disguise, and it fails at
  // exactly the sizes one past a full row of groups. 993 pieces is that size
  // for the leaf row (32 groups, 31 full leaves, one piece left over); 7712
  // pieces is 241 full leaves, which is that size for the branch row above it.
  for (const std::size_t count : {std::size_t{993}, std::size_t{7712}}) {
    std::string text(count, 'x');
    for (std::size_t i = 7; i < count; i += 13) text[i] = '\n';
    std::vector<pt::Piece> pieces;
    pieces.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      pieces.emplace_back(static_cast<std::int64_t>(i), 1, (text[i] == '\n') ? 1 : 0, true);
    }
    const pt::TextSource src{text, std::string_view{}};
    const pt::Tree tree = pt::Build(pieces);

    EXPECT_EQ(pt::Validate(tree), std::string_view{});
    EXPECT_EQ(tree.Bytes(), std::ssize(text));
    EXPECT_EQ(tree.Newlines(), static_cast<Index>(std::ranges::count(text, '\n')));
    std::string got;
    pt::ReadInto(tree, 0, tree.Bytes(), src, got);
    EXPECT_EQ(got, text);
    EXPECT_TRUE(min_fill(tree.Root(), true) >= 2);
  }

  TEST_CASE("piece tree: an output that aliases the input keeps the document");
  {
    const std::string text = "alpha\nbravo\ncharlie\n";
    const pt::TextSource src{text, std::string_view{}};
    const auto contents = [&src](const pt::Tree& t) {
      std::string out;
      pt::ReadInto(t, 0, t.Bytes(), src, out);
      return out;
    };

    // `left` aliases `in`, on the branch that clears `left` before reading it.
    pt::Tree head = pt::BuildFromText(text, true);
    pt::Tree tail;
    pt::Split(head, 0, src, head, tail);
    EXPECT_EQ(contents(head), std::string{});
    EXPECT_EQ(contents(tail), text);

    // `right` aliases `in`, at the other end.
    pt::Tree whole = pt::BuildFromText(text, true);
    pt::Tree front;
    pt::Split(whole, std::ssize(text), src, front, whole);
    EXPECT_EQ(contents(front), text);
    EXPECT_EQ(contents(whole), std::string{});

    // And the general path, which cuts inside the tree.
    pt::Tree both = pt::BuildFromText(text, true);
    pt::Tree rest;
    pt::Split(both, 6, src, both, rest);
    EXPECT_EQ(contents(both), std::string{"alpha\n"});
    EXPECT_EQ(contents(rest), std::string{"bravo\ncharlie\n"});
    EXPECT_EQ(pt::Validate(both), std::string_view{});
    EXPECT_EQ(pt::Validate(rest), std::string_view{});
  }

  TEST_CASE("piece tree: advancing zero lines lands on the start of the line it is on");
  {
    const std::string text = "alpha\nbravo\ncharlie\n";
    const pt::TextSource src{text, std::string_view{}};
    const pt::Tree tree = pt::BuildFromText(text, true);
    // Every offset on line 1 answers 6, not itself.
    for (Index at = 6; at <= 11; ++at) EXPECT_EQ(pt::AdvanceLines(tree, at, 0, src, 8), Index{6});
    EXPECT_EQ(pt::AdvanceLines(tree, 0, 0, src, 8), Index{0});
    EXPECT_EQ(pt::AdvanceLines(tree, 3, 0, src, 8), Index{0});
    EXPECT_EQ(pt::AdvanceLines(tree, 15, 0, src, 8), Index{12});
    // Past the end is the last line, and one line on is still one line on.
    EXPECT_EQ(pt::AdvanceLines(tree, std::ssize(text), 0, src, 8), Index{20});
    EXPECT_EQ(pt::AdvanceLines(tree, 8, 1, src, 8), Index{12});
  }

  TEST_CASE("piece table: a negative line does not poison the line memo");
  {
    PieceTable t = MakeTable("aa\nbb\ncc\n");
    EXPECT_EQ(LineStart(t, -1), Index{0});
    // This is the check that mattered: the memo used to remember -1, so the
    // next query walked line - (-1) lines and answered with line 1's offset.
    EXPECT_EQ(LineStart(t, 0), Index{0});
    EXPECT_EQ(LineStart(t, -7), Index{0});
    EXPECT_EQ(LineStart(t, 1), Index{3});
    EXPECT_EQ(LineStart(t, -3), Index{0});
    EXPECT_EQ(LineStart(t, 2), Index{6});
  }
}

}  // namespace koi
