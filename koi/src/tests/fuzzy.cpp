// Tests for fuzzy.cpp: the smart-jump scorer, its bands, its normalisation and
// the blend on top. Every number below is hand-worked from the table in
// docs/smart-jump.md and written out in the comment above the assertion, so a
// failure says which constant moved rather than only that something did.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {
namespace {

constexpr FuzzyConfig kDefault = FuzzyConfig::Default();
constexpr FuzzyConfig kPath = FuzzyConfig::Path();

int Raw(std::string_view needle, std::string_view candidate, const FuzzyConfig& config) {
  const FuzzyResult got = FuzzyScore(needle, candidate, config);
  return got.matched ? got.score : -1;
}

bool Near(double got, double want) { return std::abs(got - want) < 1e-6; }

std::string SpanText(std::string_view candidate, const std::vector<FuzzySpan>& spans) {
  std::string out;
  for (const FuzzySpan& span : spans) {
    if (!out.empty()) out += '|';
    out += std::string{candidate.substr(static_cast<std::size_t>(span.begin),
                                        static_cast<std::size_t>(span.end - span.begin))};
  }
  return out;
}

}  // namespace

// The doc's worked example, arithmetic and all. `pre` against a symbol.
void CompactMatchesBeatScatteredOnes() {
  TEST_CASE("fuzzy: a run beats letters scattered across a name");

  // P 16 + 10*2 = 36 -- the whitespace boundary at the start of the string,
  // doubled because it is the first needle character. Then r and e each inherit
  // that +10 through the consecutive carry: 16 + 10 twice. 36 + 26 + 26 = 88,
  // which is exactly the self-match, so it normalises to 1.00.
  EXPECT_EQ(Raw("Pre", "PrevNode", kDefault), 88);
  EXPECT_EQ(FuzzySelfScore("pre", kDefault), 88);
  EXPECT_TRUE(Near(NormalizedScore(88, "pre", kDefault), 1.0));

  // The doc writes that example with a lowercase needle, and the two rules it
  // states cannot both hold there: `pre` against `PrevNode` is a matched letter
  // in the wrong case, and the case-mismatch penalty is -2. 88 or `prev` > `Prev`
  // -- pick one. The penalty stays, because it is the rule the doc lists under
  // the tests that must hold, so the lowercase needle scores 86 and normalises
  // to 86/88.
  EXPECT_EQ(Raw("pre", "PrevNode", kDefault), 86);
  EXPECT_TRUE(Near(NormalizedScore(86, "pre", kDefault), 86.0 / 88.0));

  // The doc's second row: p 36, gap -3, r 16, gap -3, e 16 = 62. That is the
  // p-r-e-inside-"parse" alignment, and on the word alone it is what the scorer
  // finds.
  EXPECT_EQ(Raw("pre", "parse", kDefault), 62);

  // On the whole symbol the optimal alignment is not that one, and the doc's 62
  // is a hand-calculation that stopped at the first alignment it saw. Taking the
  // `e` of `_element` instead: r lands at 49 as before, the gap over s, e and _
  // costs -3 -1 -1 = -5, and the +8 word boundary after `_` pays for it twice
  // over. 49 - 5 + 8 + 16 = 68. The scorer is right and the doc is not: a
  // boundary bonus outweighs three characters of gap, which is exactly the
  // calibration the doc argues for two paragraphs earlier.
  EXPECT_EQ(Raw("pre", "parse_element", kDefault), 68);
  EXPECT_TRUE(Near(NormalizedScore(68, "pre", kDefault), 68.0 / 88.0));

  // Which is the property the example is actually about, and it holds with room
  // to spare either way.
  EXPECT_TRUE(Raw("pre", "PrevNode", kDefault) > Raw("pre", "parse_element", kDefault));

  // The acronym guard. Gaps of nine characters cost 3 + 8*1 = 11 each, which no
  // boundary bonus in the table can pay for, so a compact run wins by a mile.
  EXPECT_EQ(Raw("abc", "abcdef", kDefault), 88);
  EXPECT_EQ(Raw("abc", "aXXXXXXXXXbXXXXXXXXXc", kDefault), 46);
  EXPECT_TRUE(Raw("abc", "abcdef", kDefault) > Raw("abc", "aXXXXXXXXXbXXXXXXXXXc", kDefault));

  // Case mismatch, skim's rule: two points per matched letter in the wrong case.
  // `prev` and `Prev` are otherwise the same alignment at the same boundary.
  EXPECT_EQ(Raw("pre", "prev", kDefault), 88);
  EXPECT_EQ(Raw("pre", "Prev", kDefault), 86);
  EXPECT_TRUE(Raw("pre", "prev", kDefault) > Raw("pre", "Prev", kDefault));

  // The backtrace names the alignment the score came from, not the greedy one.
  EXPECT_EQ(SpanText("parse_element", FuzzyPositions("pre", "parse_element", kDefault)),
            std::string{"p|r|e"});
  EXPECT_EQ(FuzzyPositions("pre", "parse_element", kDefault).back().begin, 6);
  EXPECT_EQ(SpanText("PrevNode", FuzzyPositions("pre", "PrevNode", kDefault)),
            std::string{"Pre"});
}

// The camel-at-5 balance property, in the form that holds by construction.
void CamelCaseCostsAndEarnsNothing() {
  TEST_CASE("fuzzy: a camel hump neither breaks a run nor beats a flat one");

  // foobar: 16 + 10*2 = 36, then five characters each inheriting the +10 start
  // bonus through the carry: 5 * 26 = 130. Total 166.
  EXPECT_EQ(Raw("foobar", "foobar", kDefault), 166);
  // FooBar, matched by a needle in the same case so no case penalty enters: the
  // B earns camel +5, but the run already carries +10 from the string start and
  // 5 is below kFuzzyBoundary, so it can neither raise the carry nor break the
  // run. Identical 166. This is what "camel must drop to 5" buys -- at 7 the
  // arithmetic here is the same, and what changes is every alignment where the
  // run starts somewhere weaker than a boundary.
  EXPECT_EQ(Raw("FooBar", "FooBar", kDefault), 166);
  EXPECT_EQ(Raw("FooBar", "FooBar", kDefault), Raw("foobar", "foobar", kDefault));
  // Same candidate through a lowercase needle: two letters in the wrong case.
  EXPECT_EQ(Raw("foobar", "FooBar", kDefault), 162);

  // foo_bar does not join them, and cannot: the `_` costs a gap start (-3) and
  // restarts the run at the +8 word boundary after it rather than the +10 the
  // string start gave, which is 2 points less on each of b, a and r. 166 - 3 - 6
  // = 157. Nothing in the camel constant touches either term of that.
  EXPECT_EQ(Raw("foobar", "foo_bar", kDefault), 157);

  // What camel = 5 does change is the case the doc is worried about: a run that
  // starts somewhere without a boundary at all. Here the camel hump is the only
  // bonus the camel candidate's first character gets -- 16 + 5*2 = 26, then five
  // characters inheriting the +5 carry -- and at 5 the margin over snake is 6
  // points; at fzf's 7 it would be 20.
  EXPECT_EQ(Raw("FooBar", "xFooBar", kDefault), 131);
  EXPECT_EQ(Raw("foobar", "xfoo_bar", kDefault), 125);
  EXPECT_TRUE(Raw("FooBar", "xFooBar", kDefault) - Raw("foobar", "xfoo_bar", kDefault) == 6);
}

// The path config, the bands, and the blend that separates two candidates the
// matcher scores identically.
void BandsSeparateNameMatchesFromPathMatches() {
  TEST_CASE("fuzzy: bands, and priors that reorder inside one but never across");

  // Path config: the start of the string counts as a delimiter, so `k` earns +9
  // and the run carries it. 16 + 9*2 = 34, then e and y at 16 + 9 = 25 each,
  // then nucleo's prefer_prefix at offset 0 for the full +8. 34 + 50 + 8 = 92,
  // and the self-match is the same 92, so both normalise to exactly 1.00.
  EXPECT_EQ(Raw("key", "keymap.cpp", kPath), 92);
  EXPECT_EQ(Raw("key", "keylog.cpp", kPath), 92);
  EXPECT_EQ(FuzzySelfScore("key", kPath), 92);

  const FuzzyMatch keymap = ScoreBanded("key", "keymap.cpp", 0, kPath);
  const FuzzyMatch keylog = ScoreBanded("key", "keylog.cpp", 0, kPath);
  EXPECT_TRUE(keymap.matched && keylog.matched);
  EXPECT_EQ(keymap.band, 3);
  EXPECT_EQ(keylog.band, 3);
  EXPECT_TRUE(Near(keymap.normalized, 1.0));
  EXPECT_TRUE(Near(keylog.normalized, 1.0));
  EXPECT_TRUE(Near(keymap.banded, keylog.banded));

  // Nothing in the match tells them apart, so the priors do. keymap.cpp weighs
  // 343 and keylog.cpp 31 (the doc's corpus); both are on this branch and in its
  // diff, and neither is a confirmed query yet.
  const double hot = FinalScore(keymap.banded, 343, true, true, 0);
  const double cold = FinalScore(keylog.banded, 31, true, true, 0);
  EXPECT_TRUE(hot > cold);
  // 4 + 0.20*343/373 + 0.10 + 0.10 = 4.383914209115281
  EXPECT_TRUE(Near(hot, 4.3839142091152814));
  // 4 + 0.20*31/61 + 0.10 + 0.10 = 4.301639344262295
  EXPECT_TRUE(Near(cold, 4.3016393442622953));

  // The bands themselves, over a real path. The name is what a band talks about.
  const std::string_view path = "koi/src/keymap.cpp";
  const std::size_t name = path.rfind('/') + 1;
  EXPECT_EQ(MatchBand("koi/src/keymap.cpp", path, name), 4);
  EXPECT_EQ(MatchBand("key", path, name), 3);
  // Contiguous-but-not-prefix is its own band, above scattered.
  EXPECT_EQ(MatchBand("eyma", path, name), 2);
  EXPECT_EQ(MatchBand("kymp", path, name), 1);
  EXPECT_EQ(MatchBand("koisrc", path, name), 0);
  // Case-insensitive at every rung.
  EXPECT_EQ(MatchBand("KEY", path, name), 3);

  // A band-2 match on a path scores the basename alone, so the prefix bonus is
  // measured from the basename's first byte: `key` against koi/src/keymap.cpp
  // scores exactly what it scores against keymap.cpp.
  const FuzzyMatch deep = ScoreBanded("key", path, name, kPath);
  EXPECT_EQ(deep.band, 3);
  EXPECT_EQ(deep.score, 92);
  EXPECT_EQ(deep.first, static_cast<int>(name));
  // And a path-only match lands in band 0, where no pile of bonuses can lift it
  // over the band-2 row -- which is the whole point of banding.
  const FuzzyMatch shallow = ScoreBanded("koisrc", path, name, kPath);
  EXPECT_EQ(shallow.band, 0);
  EXPECT_TRUE(shallow.matched);
  EXPECT_TRUE(FinalScore(shallow.banded, 100000, true, true, 10) < deep.banded);

  // The two configs really are two: the path config drops the whitespace
  // boundary to 8 and calls the start of the string a delimiter, so the same
  // needle and candidate score differently through each.
  EXPECT_EQ(Raw("key", "keymap.cpp", kDefault), 88);
  EXPECT_TRUE(Raw("key", "keymap.cpp", kPath) != Raw("key", "keymap.cpp", kDefault));
}

// The blend and the sort key, checked against hand arithmetic.
void TheBlendIsHandCheckable() {
  TEST_CASE("fuzzy: the blend, its constants and the tie-break");

  // The priors sum to 0.90, which is the property the bands rest on: every one
  // of them at full strength is still worth less than a single band step. The
  // header asserts it at compile time; this says the numbers are the ones the
  // design named.
  EXPECT_TRUE(Near(kFrecencyPrior + kSameBranchPrior + kBranchDiffPrior + kAdaptivePrior, 0.90));
  EXPECT_TRUE(kFrecencyPrior + kSameBranchPrior + kBranchDiffPrior + kAdaptivePrior < 1.0);
  // The claim is not that a band always wins -- a band-1 row with a perfect
  // normalised score does beat a band-2 row with a terrible one, and should.
  // It is that on equal normalised scores no amount of history crosses a band:
  // saturate every prior on the lower band and it still loses.
  EXPECT_TRUE(FinalScore(1.0 + 0.5, 1e9, true, true, 1e9) < 2.0 + 0.5);

  // 0.20 * 343/373 = 0.1839142091152815
  EXPECT_TRUE(Near(FinalScore(0, 343, false, false, 0), 0.1839142091152815));
  // 0.50 * 1.9/3.9 = 0.24358974358974358
  EXPECT_TRUE(Near(FinalScore(0, 0, false, false, 1.9), 0.24358974358974358));
  EXPECT_TRUE(Near(FinalScore(0, 0, true, false, 0), 0.10));
  EXPECT_TRUE(Near(FinalScore(0, 0, false, true, 0), 0.10));
  EXPECT_TRUE(Near(FinalScore(2.5, 0, false, false, 0), 2.5));
  // Negative or absent inputs are floors, not sign flips.
  EXPECT_TRUE(Near(FinalScore(1.0, -5, false, false, -5), 1.0));

  // Score desc, then the shorter candidate, then the earlier first match.
  EXPECT_TRUE(RanksBefore(RankKey{2.5, 40, 0}, RankKey{2.4, 4, 0}));
  EXPECT_TRUE(RanksBefore(RankKey{2.5, 10, 5}, RankKey{2.5, 40, 0}));
  EXPECT_TRUE(RanksBefore(RankKey{2.5, 10, 2}, RankKey{2.5, 10, 7}));
  EXPECT_FALSE(RanksBefore(RankKey{2.5, 10, 2}, RankKey{2.5, 10, 2}));

  // The normalisation's load-bearing claim: the closed form FuzzySelfScore
  // computes really is the needle scored against itself. It holds because the
  // start-of-string bonus is the strongest either config hands out, so the carry
  // swallows every bonus inside the needle -- including a camel hump, an
  // underscore and a path separator.
  for (const std::string_view needle :
       {"pre", "key", "FooBar", "foo_bar", "src/foo", "GoToLastEdit", "x", "line42"}) {
    EXPECT_EQ(FuzzyScore(needle, needle, kDefault).score, FuzzySelfScore(needle, kDefault));
    EXPECT_EQ(FuzzyScore(needle, needle, kPath).score, FuzzySelfScore(needle, kPath));
    EXPECT_TRUE(Near(NormalizedScore(FuzzySelfScore(needle, kPath), needle, kPath), 1.0));
  }

  // Degenerate inputs the prompt will hand over while it is being typed into.
  EXPECT_TRUE(FuzzyScore("", "anything", kDefault).matched);
  EXPECT_EQ(FuzzyScore("", "anything", kDefault).score, 0);
  EXPECT_FALSE(FuzzyScore("zz", "anything", kDefault).matched);
  EXPECT_FALSE(FuzzyScore("needle", "", kDefault).matched);
  EXPECT_EQ(FuzzySelfScore("", kDefault), 0);
  EXPECT_TRUE(Near(NormalizedScore(50, "", kDefault), 0.0));
  EXPECT_TRUE(FuzzyPositions("", "anything", kDefault).empty());

  // The caps clamp rather than allocate or overrun: a needle past 32 characters
  // is scored on its first 32, a candidate past 256 bytes on its first 256.
  const std::string long_needle(64, 'a');
  const std::string long_candidate(600, 'a');
  EXPECT_TRUE(FuzzyScore(long_needle, long_candidate, kDefault).matched);
  EXPECT_EQ(FuzzyScore(long_needle, long_candidate, kDefault).score,
            FuzzyScore(long_needle.substr(0, 32), long_candidate, kDefault).score);
  // A needle character living past the candidate cap is simply not found.
  EXPECT_FALSE(FuzzyScore("az", std::string(300, 'a') + "z", kDefault).matched);

  // The band and the score read the same bytes, which is what stops a row
  // disappearing without a reason: `zz` lives past the cap, so a band read off
  // the whole candidate would promise a contiguous match the DP cannot find.
  // ScoreBanded classifies the capped bytes, so the two agree on "no".
  const std::string past_cap = std::string(300, 'x') + "zz";
  EXPECT_EQ(MatchBand("zz", past_cap, 0), 2);
  EXPECT_EQ(MatchBand("zz", past_cap.substr(0, kFuzzyMaxSpan), 0), 0);
  EXPECT_FALSE(ScoreBanded("zz", past_cap, 0, kDefault).matched);
  // A name offset past the cap is clamped rather than read.
  EXPECT_FALSE(ScoreBanded("zz", past_cap, 400, kDefault).matched);
  // Inside the cap nothing moved.
  EXPECT_TRUE(ScoreBanded("xx", past_cap, 0, kDefault).matched);
  EXPECT_EQ(ScoreBanded("xx", past_cap, 0, kDefault).band, 3);

  // Non-ASCII bytes are matched byte for byte and never read as a word boundary
  // in the middle of a sequence.
  EXPECT_FALSE(FuzzyScore("naive", "na\xC3\xAFve_thing", kDefault).matched);
  EXPECT_TRUE(FuzzyScore("nave", "na\xC3\xAFve", kDefault).matched);
}

// The backtrace on the shapes that make it walk a gap chain instead of a
// diagonal: every cell along a long gap saturates to zero, and the walk has to
// stop at the cell that opened the chain rather than at the first zero it meets.
void SpansAlwaysSpellTheNeedle() {
  TEST_CASE("fuzzy: every span spells the needle, gaps and all");

  // Sixteen characters of gap between the two matched bytes.
  const std::string gapped_row = "zaqqqqqqqqqqqqqq_";
  const std::vector<FuzzySpan> gapped = FuzzyPositions("a_", gapped_row, kDefault);
  EXPECT_EQ(SpanText(gapped_row, gapped), std::string{"a|_"});
  EXPECT_EQ(gapped.size(), std::size_t{2});
  EXPECT_EQ(gapped.front().begin, 1);
  EXPECT_EQ(gapped.front().end, 2);
  EXPECT_EQ(gapped.back().begin, 16);
  EXPECT_EQ(gapped.back().end, 17);
  // The spans and the score agree about where the match starts.
  EXPECT_EQ(FuzzyScore("a_", gapped_row, kDefault).first, 1);

  // The same property swept over an alphabet built to saturate the gap row:
  // whatever alignment the DP picks, the bytes it reports back spell the needle
  // and start where FuzzyResult.first says they do.
  std::uint32_t rng = 0x9E3779B9U;
  const auto next = [&rng] {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  };
  static constexpr std::string_view kAlphabet = "abq_/.Q";
  int checked = 0;
  int wrong = 0;
  for (int k = 0; k < 20000; ++k) {
    std::string needle;
    std::string row;
    for (int i = 0; i < 1 + static_cast<int>(next() % 4); ++i) {
      needle += kAlphabet[next() % kAlphabet.size()];
    }
    for (int i = 0; i < 1 + static_cast<int>(next() % 40); ++i) {
      row += kAlphabet[next() % kAlphabet.size()];
    }
    const FuzzyConfig& config = ((k % 2) == 0) ? kDefault : kPath;
    const FuzzyResult scored = FuzzyScore(needle, row, config);
    if (!scored.matched) continue;
    ++checked;
    const std::vector<FuzzySpan> spans = FuzzyPositions(needle, row, config);
    std::string spelled;
    for (const FuzzySpan& span : spans) {
      spelled += row.substr(static_cast<std::size_t>(span.begin),
                            static_cast<std::size_t>(span.end - span.begin));
    }
    // The scorer's own rule: ASCII case folds, anything else is byte equality.
    const auto fold = [](char c) {
      return ((c >= 'A') && (c <= 'Z')) ? static_cast<char>(c - 'A' + 'a') : c;
    };
    const bool same =
        (spelled.size() == needle.size()) &&
        std::equal(spelled.begin(), spelled.end(), needle.begin(),
                   [&fold](char a, char b) { return fold(a) == fold(b); });
    if (!same || spans.empty() || (spans.front().begin != scored.first)) ++wrong;
  }
  EXPECT_TRUE(checked > 5000);
  EXPECT_EQ(wrong, 0);
}

// The budget from docs/smart-jump.md: 1000 rows through the scorer in under a
// millisecond, which is what leaves room inside the 250us keystroke target once
// the real corpus is four hundred rows rather than a thousand.
void ScoringAThousandRowsFitsTheKeystrokeBudget() {
  TEST_CASE("fuzzy: a thousand candidates cost less than a millisecond");

  // Paths and symbols at the shape the store actually holds: about forty
  // characters, a mix of directories, snake and camel.
  std::vector<std::string> corpus;
  corpus.reserve(1000);
  static constexpr std::string_view kDirs[] = {"koi/src/", "koi/src/tests/", "gai/src/",
                                               "tooey/src/", "common/src/"};
  static constexpr std::string_view kStems[] = {"keymap",   "keylog",  "piece_tree", "navigate",
                                                "project",  "syntax",  "selection",  "textobject",
                                                "renderer", "overview"};
  // One needle per row, drawn from that row's own stem: every row gets past the
  // gate and into the DP. Built up front so the timed loop measures the scorer
  // and not std::string.
  std::vector<std::string> needles;
  needles.reserve(1000);
  for (int i = 0; i < 500; ++i) {
    corpus.push_back(std::string{kDirs[i % 5]} + std::string{kStems[i % 10]} + "_" +
                     std::to_string(i) + "_component.cpp");
    needles.push_back(std::string{kStems[i % 10]} + "cpp");
    corpus.push_back("ResolveAnchorFor" + std::string{kStems[i % 10]} + "Row" +
                     std::to_string(i) + "InPlace");
    needles.push_back(std::string{kStems[i % 10]} + "row");
  }
  EXPECT_EQ(corpus.size(), std::size_t{1000});
  EXPECT_EQ(needles.size(), corpus.size());

  double sink = 0;
  // Three passes, best of: the suite runs alongside whatever else the machine is
  // doing, and one descheduled run is not a regression.
  const auto best_of_three = [](auto&& one_pass) {
    long long best_us = -1;
    for (int pass = 0; pass < 3; ++pass) {
      const auto started = std::chrono::steady_clock::now();
      one_pass();
      const long long us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
      if ((best_us < 0) || (us < best_us)) best_us = us;
    }
    return best_us;
  };

  // One typed needle across the whole corpus: the keystroke as it happens, where
  // the subsequence gate throws most rows out before the DP sees them.
  int typed_matches = 0;
  const long long typed_us = best_of_three([&] {
    typed_matches = 0;
    for (const std::string& row : corpus) {
      const std::size_t name = row.rfind('/') + 1;
      const FuzzyMatch got = ScoreBanded("keymapcp", row, name, kPath);
      sink += got.banded;
      if (got.matched) ++typed_matches;
    }
  });
  // The count is the point: this shape is gate cost, not DP cost. Fifty of the
  // thousand rows carry the `keymap` stem and nothing else gets past the gate,
  // so the budget below would still be met by a DP three times slower.
  EXPECT_EQ(typed_matches, 50);

  // The same thousand rows, each against a needle taken from its own stem, so
  // every row runs the DP over its full span. This is what a DP cost regression
  // has to move, and the count says it did run.
  int dp_matches = 0;
  const long long dp_us = best_of_three([&] {
    dp_matches = 0;
    for (std::size_t r = 0; r < corpus.size(); ++r) {
      const std::size_t name = corpus[r].rfind('/') + 1;
      const FuzzyMatch got = ScoreBanded(needles[r], corpus[r], name, kPath);
      sink += got.banded;
      if (got.matched) ++dp_matches;
    }
  });
  EXPECT_EQ(dp_matches, 1000);

  EXPECT_TRUE(sink >= 0);
  std::cout << "fuzzy bench: 1000 candidates x 8-char needle in " << typed_us
            << "us, all-rows-through-the-DP in " << dp_us << "us\n";
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
  // A sanitizer build is optimized and still several times slower; the numbers
  // measure the instrumentation, not the design. Same loose bounds as the
  // unoptimized case.
  EXPECT_TRUE(typed_us < 20000);
  EXPECT_TRUE(dp_us < 60000);
#elif defined(__OPTIMIZE__)
  // The budget as the design states it, for the shape the design describes.
  EXPECT_TRUE(typed_us < 1000);
  // And the DP itself: about 1.3ms for a thousand saturated rows, 1.6ms on a
  // busy machine. The bound is roughly 2x that -- it survives the noise, and a
  // DP three times slower (measured at 3.6ms) fails it while the typed budget
  // above still passes.
  EXPECT_TRUE(dp_us < 3000);
#else
  // Unoptimized, the same work is roughly an order of magnitude slower and the
  // numbers say nothing about the design. Keep bounds anyway -- what they
  // separate is a DP that is linear in the span from one that is not.
  EXPECT_TRUE(typed_us < 20000);
  EXPECT_TRUE(dp_us < 60000);
#endif
}

}  // namespace koi
