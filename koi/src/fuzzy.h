// The smart-jump scorer: Smith-Waterman with affine gaps, nucleo's formulation
// (helix-editor/nucleo, fuzzy_optimal.rs), plus the bands, the normalisation and
// the blend from docs/smart-jump.md.
//
// Nothing here includes a koi header, and nothing here allocates on the scoring
// path. That is deliberate: the whole corpus is re-scored on every keystroke, so
// the DP runs on fixed stack buffers, and the tests drive these functions
// directly rather than through an editor.
#ifndef KOI_FUZZY_H_
#define KOI_FUZZY_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace koi {

// The score table. nucleo's, with one change the design argues for: camelCase
// drops from fzf's 7 (BONUS_BOUNDARY - PENALTY_GAP_EXTENSION) to 5, because the
// per-cell carry below already lets a camel run inherit the bonus of whatever
// boundary started it, and a camel bonus large enough to also *break* a run is
// what makes camel beat snake systematically.
//
// The gap penalties are calibrated against the boundary bonus: about eight
// characters of gap (3 + 7*1) cancel a +8 boundary, which is what stops fuzzy
// matching collapsing into acronym matching. Tighten the gaps if scatter shows
// up; raising the boundary bonuses makes acronym collapse worse, not better.
inline constexpr int kFuzzyMatch = 16;
inline constexpr int kFuzzyGapStart = 3;
inline constexpr int kFuzzyGapExtension = 1;
inline constexpr int kFuzzyBoundary = 8;
inline constexpr int kFuzzyBoundaryWhite = 10;
inline constexpr int kFuzzyBoundaryWhitePath = 8;
inline constexpr int kFuzzyBoundaryDelimiter = 9;
inline constexpr int kFuzzyCamel = 5;
inline constexpr int kFuzzyNonWord = 8;
inline constexpr int kFuzzyConsecutive = 4;
inline constexpr int kFuzzyFirstCharMultiplier = 2;
inline constexpr int kFuzzyCaseMismatch = 2;
// nucleo's prefer_prefix, decaying by a gap start per character the match is
// pushed right: 8 at offset 0, 5 at 1, 2 at 2, nothing from 3 on.
inline constexpr int kFuzzyPreferPrefix = 8;

// Why the camel bonus is absorbed rather than obeyed: a run carries the bonus of
// the character that started it, and only a bonus at least as strong as a word
// boundary may break the run and become the new carry. At 5 a camel hump can do
// neither, which is exactly the "FooBar scores as foobar" property the tests
// pin down.
static_assert(kFuzzyCamel < kFuzzyBoundary,
              "a camel hump must not be able to break a consecutive run");

// The DP runs on stack buffers of exactly these sizes; a longer needle is cut to
// the first 32 characters and a longer candidate scored on its first 256 bytes.
// Stored content is truncated to 200 bytes at record time and paths and symbol
// names are far shorter, so neither cap bites on this corpus.
inline constexpr std::size_t kFuzzyMaxNeedle = 32;
inline constexpr std::size_t kFuzzyMaxSpan = 256;

// fzf's char classes, in fzf's order: the bonus table below tests `> kNonWord`,
// which is what makes a delimiter count as a word character for the purpose of
// earning a boundary bonus. Bytes >= 0x80 are kLetter, so a UTF-8 sequence never
// reads as a word boundary in the middle of itself.
enum class FuzzyClass : std::uint8_t {
  kWhite,
  kNonWord,
  kDelimiter,
  kLower,
  kUpper,
  kLetter,
  kNumber,
};

// Two configs, one matcher (docs/smart-jump.md, Scoring).
struct FuzzyConfig {
  // The characters that count as delimiters -- the strongest boundary short of
  // whitespace.
  std::string_view delimiters;
  // What a match at a word boundary after whitespace is worth. The path config
  // lowers it so that `/` (a delimiter, +9) is the strongest boundary a path
  // has.
  int boundary_white;
  // What precedes byte 0. The default config says whitespace, so the first
  // character of a symbol earns the whitespace boundary; the path config says
  // delimiter, so a leading path component earns the same bonus as one after a
  // `/`.
  FuzzyClass start_class;
  // Whether a match near the start of the candidate earns kFuzzyPreferPrefix.
  // On for paths -- it is exactly `key` -> `keymap.cpp` -- and off for the
  // default config, where the worked examples in docs/smart-jump.md pin the raw
  // scores and a prefix bonus would move all of them.
  bool prefer_prefix;

  // Symbols and content.
  static constexpr FuzzyConfig Default() {
    return FuzzyConfig{"/,:;|", kFuzzyBoundaryWhite, FuzzyClass::kWhite, false};
  }
  // The file clause.
  static constexpr FuzzyConfig Path() {
    return FuzzyConfig{"/:", kFuzzyBoundaryWhitePath, FuzzyClass::kDelimiter, true};
  }
};

struct FuzzyResult {
  int score{0};
  bool matched{false};
  // Byte offset of the first matched character in the candidate, -1 when there
  // is no match. The third tie-break.
  int first{-1};
};

// One run of matched bytes, half-open. Only the displayed rows need these, so
// they come from a separate entry point that pays for a backtrace.
struct FuzzySpan {
  int begin{0};
  int end{0};
};

// The DP. An empty needle matches everything at score 0.
FuzzyResult FuzzyScore(std::string_view needle, std::string_view candidate,
                       const FuzzyConfig& config);

// The same alignment, backtraced into byte runs. Empty when there is no match.
std::vector<FuzzySpan> FuzzyPositions(std::string_view needle, std::string_view candidate,
                                      const FuzzyConfig& config);

// The needle matched against itself: one consecutive run starting at the
// config's start-of-string class, first character doubled, prefix bonus included
// where the config has one. Nobody ships a normalisation, so this is ours --
// query-dependent and candidate-length-independent, which leaves length to the
// tie-breaks.
int FuzzySelfScore(std::string_view needle, const FuzzyConfig& config);

// score / FuzzySelfScore, clamped into [0,1]. The clamp is not decoration: a
// needle whose first character earns a weak bonus (a leading `_`, say) can be
// beaten by a candidate whose run starts at a stronger boundary, and then a real
// match scores over its own self-match.
double NormalizedScore(int score, std::string_view needle, const FuzzyConfig& config);

// VS Code's bands, disjoint by construction so that no pile of small bonuses
// lifts a path-only match over a basename match:
//
//   4  the needle equals the whole candidate (case-insensitive)
//   3  the needle is a prefix of the basename / symbol name
//   2  the needle is a contiguous substring of the basename / symbol name
//   1  the needle matches fuzzily inside the basename / symbol name
//   0  it matches only across the whole path / line
//
// `name_offset` is where the basename or symbol name starts; 0 for a bare name.
int MatchBand(std::string_view needle, std::string_view candidate, std::size_t name_offset);

struct FuzzyMatch {
  bool matched{false};
  int band{0};
  // The raw DP score, and what it was scored against: bands 1 to 3 score the
  // name alone, bands 0 and 4 the whole candidate. "Basename and full path are
  // scored separately" (docs/smart-jump.md, Scoring).
  int score{0};
  double normalized{0.0};
  // band + normalized. A whole band is worth 1.0 and the priors sum to 0.9, so
  // history can reorder inside a band and never across one.
  double banded{0.0};
  int first{-1};
};

// Caps first, then bands and scores the capped bytes: a candidate past
// kFuzzyMaxSpan is classified and scored on its first 256 bytes, name offset
// included. Band and score therefore agree, instead of the band promising a
// match the DP cannot find and the row vanishing for no stated reason; the
// visible edge is that a needle -- or a whole basename -- living past the cap
// does not match at all. Nothing in this corpus is that long: content is
// truncated to 200 bytes at record time and paths are far shorter.
FuzzyMatch ScoreBanded(std::string_view needle, std::string_view candidate,
                       std::size_t name_offset, const FuzzyConfig& config);

// The blend's priors (docs/smart-jump.md, Scoring). Match dominates: every
// shipped system that works runs match:priors around 8:1, and a ranking where a
// frecent file cannot be beaten by a better match is the complaint
// telescope-frecency's issue tracker is made of.
inline constexpr double kFrecencyPrior = 0.20;
inline constexpr double kFrecencySquash = 30.0;
inline constexpr double kSameBranchPrior = 0.10;
inline constexpr double kBranchDiffPrior = 0.10;
inline constexpr double kAdaptivePrior = 0.50;
inline constexpr double kAdaptiveSquash = 2.0;

// The property the bands rest on, asserted rather than asserted-to: every prior
// at once is worth less than one band, so history reorders inside a band and can
// never cross one.
static_assert(kFrecencyPrior + kSameBranchPrior + kBranchDiffPrior + kAdaptivePrior < 1.0,
              "the priors must not be able to cross a band");

// The blend. Inputs are plain numbers so the caller can assemble them from the
// store: `frecency_weight` is the store's f (weight times its recency
// multiplier), `adaptive_use` is ProjectStore::AdaptiveUse for these terms and
// this target.
constexpr double FinalScore(double banded_match, double frecency_weight, bool same_branch,
                            bool in_branch_diff, double adaptive_use) {
  const double f = (frecency_weight > 0.0) ? frecency_weight : 0.0;
  const double u = (adaptive_use > 0.0) ? adaptive_use : 0.0;
  return banded_match                                       //
         + kFrecencyPrior * (f / (f + kFrecencySquash))     //
         + (same_branch ? kSameBranchPrior : 0.0)           //
         + (in_branch_diff ? kBranchDiffPrior : 0.0)        //
         + kAdaptivePrior * (u / (u + kAdaptiveSquash));
}

// The sort key, and the whole tie-break rule in one place: score descending,
// then the shorter candidate, then the earlier first match, then where the row
// sits in the corpus. `score` is the banded match while ranking a single clause,
// and FinalScore once the priors are in -- the ordering is the same either way.
//
// `order` is what makes the rule total. The sorts underneath are unstable, so
// two rows equal on the first three keys could swap between one keystroke and
// the next -- the same query drawing a different band twice. A corpus position
// is the one thing about a row that no query can change, so it is the last word.
struct RankKey {
  double score{0.0};
  std::size_t length{0};
  int first{0};
  std::size_t order{0};
};

bool RanksBefore(const RankKey& a, const RankKey& b);

}  // namespace koi

#endif  // KOI_FUZZY_H_
