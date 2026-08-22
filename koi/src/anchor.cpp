#include "anchor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <ranges>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "editor.h"
#include "navigate.h"
#include "query.h"
#include "sha1.h"
#include "subprocess.h"
#include "symbols.h"
#include "thread_pool.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

}

// -- what a line is -------------------------------------------------------------

std::string_view TrimAnchorLine(std::string_view raw) {
  constexpr std::string_view kSpace = " \t\r\n\f\v";
  const std::size_t from = raw.find_first_not_of(kSpace);
  if (from == std::string_view::npos) return {};
  const std::size_t to = raw.find_last_not_of(kSpace);
  std::string_view kept = raw.substr(from, to - from + 1);
  if (kept.size() > kMaxContentBytes) {
    // The byte just past the cut says whether the cut landed inside a code
    // point: a continuation byte means it did, and the walk back ends on the
    // lead byte that started it. A cut already on a boundary keeps every byte
    // it was allowed.
    std::size_t cut = kMaxContentBytes;
    while ((cut > 0) && ((static_cast<unsigned char>(kept[cut]) & 0xC0) == 0x80)) --cut;
    kept = kept.substr(0, cut);
  }
  return kept;
}

std::string NormalizeAnchorLine(std::string_view raw) { return std::string{TrimAnchorLine(raw)}; }

std::uint64_t AnchorLineHash(std::string_view normalized) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const char c : normalized) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

void SplitAnchorLines(std::string_view text, AnchorFile& out) {
  out.lines.clear();
  out.hashes.clear();
  out.index.clear();
  // One more line than there are newlines, which is what LineCount() counts: a
  // file ending in a newline has a final empty line, and an index here has to
  // be a document line number minus one however the file ends.
  std::size_t at = 0;
  for (;;) {
    const std::size_t eol = std::min(text.find('\n', at), text.size());
    std::string_view line = text.substr(at, eol - at);
    while (!line.empty() && (line.back() == '\r')) line.remove_suffix(1);
    out.lines.emplace_back(TrimAnchorLine(line));
    if (eol == text.size()) break;
    at = eol + 1;
  }
  out.hashes.reserve(out.lines.size());
  out.index.reserve(out.lines.size());
  for (Index i = 0; i < std::ssize(out.lines); ++i) {
    const std::uint64_t hash = AnchorLineHash(out.lines[static_cast<std::size_t>(i)]);
    out.hashes.push_back(hash);
    if (!out.lines[static_cast<std::size_t>(i)].empty()) out.index.emplace_back(hash, i);
  }
  std::ranges::sort(out.index);
}

std::string AnchorContextAt(const AnchorFile& file, Index line0) {
  constexpr Index kContextLines = 2;
  std::string out;
  bool first = true;
  const Index last = std::ssize(file.lines) - 1;
  for (Index at = line0 - kContextLines; at <= (line0 + kContextLines); ++at) {
    if (at == line0) continue;
    // A blank line is context too, and it keeps its place: the entries are
    // positional, so dropping the empty ones would make "two above" mean
    // different lines in different files. A slot off either end of the file is
    // an empty entry for the same reason: a candidate on line 1 has to produce
    // as many entries as one in the middle, or the compare lines "one below"
    // up against "two above" and a mid-file decoy wins on the misalignment.
    if (!first) out += '\n';
    first = false;
    if ((at < 0) || (at > last)) continue;
    out += file.lines[static_cast<std::size_t>(at)];
  }
  return out;
}

// -- patience diff ---------------------------------------------------------------

namespace {

// A pair of lines that appear exactly once in each side of the region under
// consideration. These, and only these, are what the diff is allowed to line up
// -- which is why a stray `}` in one function is never matched to a `}` in
// another.
struct AnchorPair {
  Index old_at{0};
  Index new_at{0};
};

// Longest increasing subsequence of `pairs` by `new_at`, patience-sorting.
// `pairs` arrives sorted by `old_at`, so what survives is the largest set of
// anchors that keeps both sides in order.
std::vector<AnchorPair> LongestIncreasing(const std::vector<AnchorPair>& pairs) {
  std::vector<AnchorPair> out;
  if (pairs.empty()) return out;
  // `tails[k]` is the index into `pairs` of the smallest possible tail of an
  // increasing run of length k + 1; `prev` chains each entry to its predecessor.
  std::vector<std::size_t> tails;
  std::vector<std::size_t> prev(pairs.size(), static_cast<std::size_t>(-1));
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    const auto at = std::ranges::lower_bound(tails, pairs[i].new_at, {},
                                             [&pairs](std::size_t k) { return pairs[k].new_at; });
    const std::size_t slot = static_cast<std::size_t>(at - tails.begin());
    if (slot > 0) prev[i] = tails[slot - 1];
    if (at == tails.end()) {
      tails.push_back(i);
    } else {
      *at = i;
    }
  }
  out.reserve(tails.size());
  for (std::size_t at = tails.back(); at != static_cast<std::size_t>(-1); at = prev[at]) {
    out.push_back(pairs[at]);
  }
  std::ranges::reverse(out);
  return out;
}

// Past this the recursion gives up and calls the rest of the region changed.
// Nothing real reaches it -- each level consumes at least one anchor line -- and
// it is what stops a pathological file from recursing until the stack is gone.
constexpr int kMaxDiffDepth = 64;

void DiffRegion(std::span<const std::uint64_t> old_lines, std::span<const std::uint64_t> new_lines,
                Index old_from, Index old_to, Index new_from, Index new_to,
                std::vector<DiffHunk>& out, int depth) {
  while ((old_from < old_to) && (new_from < new_to) &&
         (old_lines[static_cast<std::size_t>(old_from)] ==
          new_lines[static_cast<std::size_t>(new_from)])) {
    ++old_from;
    ++new_from;
  }
  while ((old_to > old_from) && (new_to > new_from) &&
         (old_lines[static_cast<std::size_t>(old_to - 1)] ==
          new_lines[static_cast<std::size_t>(new_to - 1)])) {
    --old_to;
    --new_to;
  }
  if ((old_from == old_to) && (new_from == new_to)) return;

  const auto give_up = [&] { out.push_back(DiffHunk{old_from, old_to, new_from, new_to}); };
  if ((old_from == old_to) || (new_from == new_to) || (depth >= kMaxDiffDepth)) {
    give_up();
    return;
  }

  // Counted over the region rather than over the file: a line that is unique
  // *here* is a usable anchor even when it repeats elsewhere, which is what
  // makes the recursion find more than the first pass did.
  std::unordered_map<std::uint64_t, std::pair<int, Index>> in_old;
  std::unordered_map<std::uint64_t, std::pair<int, Index>> in_new;
  in_old.reserve(static_cast<std::size_t>(old_to - old_from));
  in_new.reserve(static_cast<std::size_t>(new_to - new_from));
  for (Index i = old_from; i < old_to; ++i) {
    auto& entry = in_old[old_lines[static_cast<std::size_t>(i)]];
    ++entry.first;
    entry.second = i;
  }
  for (Index i = new_from; i < new_to; ++i) {
    auto& entry = in_new[new_lines[static_cast<std::size_t>(i)]];
    ++entry.first;
    entry.second = i;
  }

  std::vector<AnchorPair> pairs;
  for (Index i = old_from; i < old_to; ++i) {
    const std::uint64_t hash = old_lines[static_cast<std::size_t>(i)];
    const auto mine = in_old.find(hash);
    if ((mine == in_old.end()) || (mine->second.first != 1)) continue;
    const auto theirs = in_new.find(hash);
    if ((theirs == in_new.end()) || (theirs->second.first != 1)) continue;
    pairs.push_back(AnchorPair{i, theirs->second.second});
  }
  const std::vector<AnchorPair> anchors = LongestIncreasing(pairs);
  if (anchors.empty()) {
    give_up();
    return;
  }

  Index old_at = old_from;
  Index new_at = new_from;
  for (const AnchorPair& anchor : anchors) {
    DiffRegion(old_lines, new_lines, old_at, anchor.old_at, new_at, anchor.new_at, out, depth + 1);
    old_at = anchor.old_at + 1;
    new_at = anchor.new_at + 1;
  }
  DiffRegion(old_lines, new_lines, old_at, old_to, new_at, new_to, out, depth + 1);
}

}

std::vector<DiffHunk> DiffLineMap(std::span<const std::uint64_t> old_lines,
                                  std::span<const std::uint64_t> new_lines) {
  std::vector<DiffHunk> out;
  DiffRegion(old_lines, new_lines, 0, std::ssize(old_lines), 0, std::ssize(new_lines), out, 0);
  // The recursion emits left to right, so this only ever joins hunks that ended
  // up touching -- which the anchors between them normally prevent.
  std::vector<DiffHunk> merged;
  for (const DiffHunk& hunk : out) {
    if (!merged.empty() && (merged.back().old_to >= hunk.old_from) &&
        (merged.back().new_to >= hunk.new_from)) {
      merged.back().old_to = std::max(merged.back().old_to, hunk.old_to);
      merged.back().new_to = std::max(merged.back().new_to, hunk.new_to);
      continue;
    }
    merged.push_back(hunk);
  }
  return merged;
}

MappedLine MapLineThroughDiff(std::span<const DiffHunk> hunks, Index line) {
  // The first hunk that has not already ended above `line`. Everything before
  // it corresponds line for line, shifted by what that hunk's predecessor did.
  const auto at = std::ranges::lower_bound(hunks, line, {},
                                           [](const DiffHunk& hunk) { return hunk.old_to - 1; });
  Index delta = 0;
  if (at != hunks.begin()) {
    const DiffHunk& before = *(at - 1);
    delta = before.new_to - before.old_to;
  }
  if ((at != hunks.end()) && (at->old_from <= line) && (line < at->old_to)) {
    return MappedLine{at->new_from, false};
  }
  return MappedLine{line + delta, true};
}

// -- banded edit distance ---------------------------------------------------------

namespace {

// Myers 1999 as Hyyrö restated it, one 64-bit word: `pattern` is at most 64
// bytes, `text` is any length, and the cost is a handful of instructions per
// byte of text.
int Myers64(std::string_view pattern, std::string_view text) {
  std::array<std::uint64_t, 256> peq{};
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    peq[static_cast<unsigned char>(pattern[i])] |= (1ull << i);
  }
  std::uint64_t vp = ~0ull;
  std::uint64_t vn = 0;
  int dist = static_cast<int>(pattern.size());
  const std::uint64_t last = 1ull << (pattern.size() - 1);
  for (const char c : text) {
    const std::uint64_t x = peq[static_cast<unsigned char>(c)] | vn;
    const std::uint64_t d0 = (((x & vp) + vp) ^ vp) | x;
    std::uint64_t hp = vn | ~(d0 | vp);
    std::uint64_t hn = d0 & vp;
    // Read off the bottom row before the shift: the shift is what carries the
    // vectors into the next column, and the score belongs to this one.
    dist += ((hp & last) != 0) ? 1 : 0;
    dist -= ((hn & last) != 0) ? 1 : 0;
    hp = (hp << 1) | 1ull;
    hn <<= 1;
    vp = hn | ~(d0 | hp);
    vn = hp & d0;
  }
  return dist;
}

}

int EditErrors(std::string_view a, std::string_view b, int cap) {
  cap = std::max(0, cap);
  const auto clamp = [cap](int errors) { return std::min(errors, cap); };
  if (a.empty()) return clamp(static_cast<int>(b.size()));
  if (b.empty()) return clamp(static_cast<int>(a.size()));
  constexpr std::size_t kWord = 64;
  // Only the pattern has to fit the word, and the distance is symmetric, so a
  // pair with one short side is still exact.
  if (a.size() <= kWord) return clamp(Myers64(a, b));
  if (b.size() <= kWord) return clamp(Myers64(b, a));

  // Both too long: diff-match-patch's Match_MaxBits route. The first word is
  // measured exactly and the tails are charged the cheap upper bound -- bytes
  // that differ over the shared length, plus the length difference. It
  // over-reports a tail edit that shifts the rest along, which is the safe
  // direction: rung 7 accepts on a low score, so an over-report can only refuse
  // a match, never invent one.
  int errors = Myers64(a.substr(0, kWord), b.substr(0, kWord));
  const std::string_view a_tail = a.substr(kWord);
  const std::string_view b_tail = b.substr(kWord);
  const std::size_t shared = std::min(a_tail.size(), b_tail.size());
  for (std::size_t i = 0; (i < shared) && (errors < cap); ++i) {
    errors += (a_tail[i] != b_tail[i]) ? 1 : 0;
  }
  errors += static_cast<int>(std::max(a_tail.size(), b_tail.size()) - shared);
  return clamp(errors);
}

// -- the ladder --------------------------------------------------------------------

namespace {

// Every line of the file whose normalised text hashes to `hash`, in line order.
std::span<const std::pair<std::uint64_t, Index>> LinesWith(const AnchorFile& file,
                                                           std::uint64_t hash) {
  const auto found = std::ranges::equal_range(file.index, hash, {},
                                              [](const auto& entry) { return entry.first; });
  return std::span<const std::pair<std::uint64_t, Index>>{found.data(), found.size()};
}

// How much of the stored context the neighbourhood of `line0` still reads like.
// Positional and entry by entry, against a context built the same way the
// recorder built the stored one -- which is what lets it say anything at all
// about a candidate at the top or the bottom of a file.
//
// This confirms a candidate the content already found. It never finds one.
double ContextMatch(std::string_view stored, const AnchorFile& file, Index line0) {
  if (stored.empty()) return 0.0;
  const std::string have = AnchorContextAt(file, line0);
  std::size_t entries = 0;
  std::size_t same = 0;
  std::size_t want_at = 0;
  std::size_t have_at = 0;
  while ((want_at <= stored.size()) || (have_at <= have.size())) {
    const bool want_left = want_at <= stored.size();
    const bool have_left = have_at <= have.size();
    if (!want_left && !have_left) break;
    std::string_view want_line;
    std::string_view have_line;
    if (want_left) {
      const std::size_t eol = std::min(stored.find('\n', want_at), stored.size());
      want_line = stored.substr(want_at, eol - want_at);
      want_at = eol + 1;
    }
    if (have_left) {
      const std::size_t eol = std::min(have.find('\n', have_at), have.size());
      have_line = std::string_view{have}.substr(have_at, eol - have_at);
      have_at = eol + 1;
    }
    ++entries;
    if (want_left && have_left && (want_line == have_line)) ++same;
  }
  if (entries == 0) return 0.0;
  return static_cast<double>(same) / static_cast<double>(entries);
}

// Rung 6's bar when the line is not unique: the neighbourhood has to agree, and
// it has to agree about *one* candidate. Both, not either. A context that fits
// every candidate equally well -- three copies of the same four-line body --
// has confirmed nothing, and taking the nearest of them is how a bare `}` heals
// onto a neighbour.
constexpr double kContextConfirm = 0.75;
constexpr double kContextMargin = 0.25;

HealResult Hit(Index line0, HealRung rung, bool unique, double similarity) {
  HealResult out;
  out.line = line0 + 1;
  out.rung = static_cast<int>(rung);
  out.similarity = similarity;
  out.unique = unique;
  out.miss = false;
  return out;
}

// Nearest to `seed` among `hits`, restricted to [lo, hi]. -1 when none.
Index NearestIn(std::span<const std::pair<std::uint64_t, Index>> hits, Index seed, Index lo,
                Index hi) {
  Index best = -1;
  for (const auto& [hash, line] : hits) {
    if ((line < lo) || (line > hi)) continue;
    if ((best < 0) || (std::abs(line - seed) < std::abs(best - seed))) best = line;
  }
  return best;
}

}

HealResult ResolveAnchor(const HealInput& in) {
  HealResult missed;
  if ((in.file == nullptr) || in.file->lines.empty()) return missed;
  const AnchorFile& file = *in.file;
  const Index last = std::ssize(file.lines) - 1;
  const std::uint64_t want = AnchorLineHash(in.content);
  const std::span<const std::pair<std::uint64_t, Index>> hits =
      in.content.empty() ? std::span<const std::pair<std::uint64_t, Index>>{}
                         : LinesWith(file, want);
  const bool unique = (hits.size() == 1);

  // Where the content rungs start looking. The cached line, unless rung 2 has
  // something better to say.
  Index seed = std::clamp<Index>(in.line - 1, 0, last);

  // Rung 2: the line through the hunks. A branch switch heals exactly here.
  if (in.hunks != nullptr) {
    const MappedLine at = MapLineThroughDiff(*in.hunks, in.line - 1);
    if (at.exact && (at.line >= 0) && (at.line <= last)) {
      // Verified, not trusted. The diff says this line came through unchanged;
      // the row says what it used to read. When those disagree the blob the row
      // holds is not the text the row holds, and neither is evidence -- fall
      // through to the content rungs rather than believing the position.
      if (in.content.empty() || (file.hashes[static_cast<std::size_t>(at.line)] == want)) {
        return Hit(at.line, HealRung::kDiff, unique || in.content.empty(), 1.0);
      }
      // Still the better place to start, though: the position is where the file
      // carried the row's line to, and only the text disagrees. Searching the
      // pre-diff line number instead is searching a neighbourhood the file has
      // moved out from under. Not for a live line -- that one has already been
      // shifted by the buffer's own edits, and mapping it again would take the
      // same shift twice.
      if (!in.live_line) seed = at.line;
    } else if (!at.exact) {
      // Inside a changed hunk: not an answer, but a better place to start than
      // a line number the file no longer has.
      seed = std::clamp<Index>(at.line, 0, last);
    }
  }

  // Everything below needs text to compare. A row recorded without the buffer
  // has none, and the caller is expected not to count that as a miss.
  if (in.content.empty()) return missed;

  // Rungs 3 to 6 are all "the text is still somewhere, exactly": with no exact
  // match in the file at all there is nothing for them to find, and the fuzzy
  // rung below is the whole of what is left.
  if (!hits.empty()) {
    // Rung 3: exact where it was expected.
    if (file.hashes[static_cast<std::size_t>(seed)] == want) {
      return Hit(seed, HealRung::kAtLine, unique, 1.0);
    }

    // Rung 4: exact nearby.
    if (const Index near = NearestIn(hits, seed, seed - kAnchorSearchWindow,
                                     seed + kAnchorSearchWindow);
        near >= 0) {
      return Hit(near, HealRung::kNearby, unique, 1.0);
    }

    // Rung 5: exact inside the symbol this row was recorded in, re-found by name.
    if (!in.symbol.Empty()) {
      if (const Index inside = NearestIn(hits, seed, in.symbol.from, in.symbol.to); inside >= 0) {
        return Hit(inside, HealRung::kInSymbol, unique, 1.0);
      }
    }

    // Rung 6: exact anywhere, and this is the one that has to be gated. A bare
    // `}` matches everywhere, so the text alone is not permission to move.
    if (unique) return Hit(hits.front().second, HealRung::kAnywhere, true, 1.0);
    {
      double best_score = -1.0;
      Index best = -1;
      for (const auto& [hash, line] : hits) {
        const double score = ContextMatch(in.context, file, line);
        // Higher wins; a tie goes to the nearer line, because the cached position
        // is a tie-breaker and never evidence.
        const bool worse = score < best_score;
        const bool tied = !worse && !(best_score < score);
        if ((best >= 0) && (worse || (tied && (std::abs(line - seed) >= std::abs(best - seed))))) {
          continue;
        }
        best_score = score;
        best = line;
      }
      double runner_up = -1.0;
      for (const auto& [hash, line] : hits) {
        if (line == best) continue;
        runner_up = std::max(runner_up, ContextMatch(in.context, file, line));
      }
      if ((best >= 0) && (best_score >= kContextConfirm) &&
          ((best_score - runner_up) >= kContextMargin)) {
        return Hit(best, HealRung::kAnywhere, false, 1.0);
      }
    }
  }

  // Rung 7: the line itself was edited. diff-match-patch's score, biased toward
  // where the line is expected to be, and refused outright above the threshold.
  {
    const Index lo = in.symbol.Empty() ? std::max<Index>(0, seed - kAnchorSearchWindow)
                                       : std::max<Index>(0, in.symbol.from);
    const Index hi = in.symbol.Empty() ? std::min(last, seed + kAnchorSearchWindow)
                                       : std::min(last, in.symbol.to);
    const double len = static_cast<double>(in.content.size());
    // Hypothesis' maxErrors: half the length is the most a match may be wrong
    // by, and it is the same bound the threshold implies.
    const int cap = static_cast<int>(in.content.size() / 2) + 1;
    double best_score = kFuzzyAccept;
    Index best = -1;
    int best_errors = 0;
    int accepted = 0;
    for (Index line = lo; line <= hi; ++line) {
      const std::string& text = file.lines[static_cast<std::size_t>(line)];
      if (text.empty()) continue;
      const int errors = EditErrors(in.content, text, cap);
      const double score = (static_cast<double>(errors) / len) +
                           (static_cast<double>(std::abs(line - seed)) / kAnchorLineScale);
      if (score > kFuzzyAccept) continue;
      ++accepted;
      // Lowest wins; a tie goes to the nearer line, which the ascending walk
      // and the strict comparison already give -- an equal score at an equal
      // distance keeps the first one seen.
      if ((best < 0) || (score < best_score)) {
        best_score = score;
        best = line;
        best_errors = errors;
      }
    }
    if (best >= 0) {
      return Hit(best, HealRung::kFuzzy, accepted == 1, 1.0 - (best_errors / len));
    }
  }

  return missed;
}

// -- the heal job -------------------------------------------------------------------

namespace {

// Single quotes, so the shell sees one word whatever is in the path.
std::string Quoted(const std::string& text) {
  std::string out{"'"};
  for (const char c : text) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += '\'';
  return out;
}

// The one git-dependent rung. Every way this can fail -- no binary, no
// repository, an object that is not in the odb, a checkout that has since been
// gc'd -- comes back the same way, silently, and the ladder falls to the
// content rungs, which assume nothing. That is the no-git contract
// (docs/smart-jump.md): a project without git behaves exactly like a blob
// missing from the odb.
bool GitBlobText(const fs::path& root, const std::string& oid, std::string& out) {
  // The oid is interpolated into a shell command, and it comes out of a
  // database. Forty hex characters or it is not one.
  if (oid.size() != 40) return false;
  for (const char c : oid) {
    const bool hex = ((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f'));
    if (!hex) return false;
  }
  const common::CmdResult result = common::RunCmdWithCapture(
      "git -C " + Quoted(root.string()) + " cat-file blob " + oid, common::CaptureMode::kPipe,
      common::CaptureMode::kDevNull);
  if (!result.output || (result.exit_status != 0)) return false;
  out.assign(result.output->buffer, result.output->size);
  return true;
}

// The span of the definition called `name`, nearest to where the row thinks it
// is. A file can hold two functions of the same name -- an overload, a static
// and a member -- and the nearer one is the better guess about which was meant.
SymbolSpan SpanNamed(const std::vector<DefinitionSpan>& spans, std::string_view name, Index seed) {
  SymbolSpan out;
  if (name.empty()) return out;
  Index best = 0;
  for (const DefinitionSpan& span : spans) {
    if (span.name != name) continue;
    const Index middle = (span.from_line + span.to_line) / 2;
    const Index away = std::abs(middle - seed);
    if (!out.Empty() && (away >= best)) continue;
    out.from = span.from_line - 1;
    out.to = span.to_line - 1;
    best = away;
  }
  return out;
}

// The innermost definition containing a line, for refilling a null `symbol`.
std::string NameAt(const std::vector<DefinitionSpan>& spans, Index line) {
  const DefinitionSpan* best = nullptr;
  for (const DefinitionSpan& span : spans) {
    if ((line < span.from_line) || (line > span.to_line)) continue;
    if ((best != nullptr) &&
        ((span.to_line - span.from_line) >= (best->to_line - best->from_line))) {
      continue;
    }
    best = &span;
  }
  return (best == nullptr) ? std::string{} : best->name;
}

void RunAnchorJobBody(AnchorJob& job) {
  if (job.rows.empty()) return;

  std::string text;
  if (job.have_text) {
    text = std::move(job.text);
  } else {
    std::error_code ec;
    text = ReadWholeFile(job.path, ec);
    // No file to heal against is not a miss. Half a repository is transiently
    // missing across a branch switch, and a history that counted that against
    // itself would be worth nothing on the way back.
    if (ec) return;
  }

  const std::string blob = GitBlobOid(text);

  // Rung 1. O(1), and it is the reason a focus-in on an untouched file costs a
  // read and a hash rather than a diff and a parse.
  std::vector<const AnchorRow*> todo;
  todo.reserve(job.rows.size());
  bool gated = false;
  for (const AnchorRow& row : job.rows) {
    if (blob.empty() || (row.blob != blob)) {
      todo.push_back(&row);
      continue;
    }
    gated = true;
    job.rungs |= 1u << static_cast<int>(HealRung::kBlob);
    // The row is true as it stands, so there is nothing to write -- unless it
    // was counted against. A file put back to what a row was recorded against
    // is a hit, and a hit clears the counter; without this a generated file
    // that was briefly truncated stays hidden for ever.
    if (row.misses <= 0) continue;
    AnchorHeal heal;
    heal.id = row.id;
    heal.seq = row.seq;
    heal.rung = static_cast<int>(HealRung::kBlob);
    heal.line = row.line;
    heal.blob = blob;
    job.heals.push_back(std::move(heal));
  }
  if (todo.empty()) {
    job.blob_gate = gated;
    return;
  }

  // Everything below splits the whole file into lines and indexes them: three
  // times the file in memory and seconds of a pool thread on a generated one,
  // paid again on every save and every focus-in. Past the cap the recorder uses
  // for the same kind of pass, the rows are left alone. Not a miss -- they were
  // not looked for, and counting it would hide them for a fact about the file's
  // size. The O(1) blob gate above still ran, which is the one cheap answer.
  if (text.size() > static_cast<std::size_t>(kMaxUniqBytes)) {
    job.too_big = true;
    return;
  }

  AnchorFile file;
  SplitAnchorLines(text, file);

  // Rung 2. One subprocess per *distinct* old blob, not per row -- the rows of
  // one file were normally all recorded against the same one.
  std::unordered_map<std::string, std::vector<DiffHunk>> hunks;
  if (!job.git_root.empty()) {
    for (const AnchorRow* row : todo) {
      if (row->blob.empty() || (row->blob == blob)) continue;
      if (hunks.contains(row->blob)) continue;
      job.ran_git = true;
      std::string before;
      if (!GitBlobText(job.git_root, row->blob, before)) {
        // Remembered as an empty map entry so a second row holding the same
        // unreachable oid does not fork again for the same answer.
        hunks.emplace(row->blob, std::vector<DiffHunk>{});
        continue;
      }
      AnchorFile was;
      SplitAnchorLines(before, was);
      hunks.emplace(row->blob, DiffLineMap(was.hashes, file.hashes));
    }
  }

  const auto inputs = [&](const AnchorRow& row) {
    HealInput in;
    in.file = &file;
    in.line = row.line;
    in.content = row.content;
    in.context = row.context;
    in.live_line = row.live_line;
    const auto found = hunks.find(row.blob);
    if ((found != hunks.end()) && !found->second.empty()) in.hunks = &found->second;
    return in;
  };

  std::vector<HealResult> results(todo.size());
  bool parse = false;
  for (std::size_t i = 0; i < todo.size(); ++i) {
    results[i] = ResolveAnchor(inputs(*todo[i]));
    // Rungs 5 and 7 want the symbol's span, and nothing above them does. The
    // parse is therefore paid only by the rows that got that far -- which is
    // what keeps the common heal to one read, one hash pass and no tree.
    if (results[i].miss || (results[i].rung >= static_cast<int>(HealRung::kAnywhere))) parse = true;
  }

  std::vector<DefinitionSpan> spans;
  if (parse) {
    std::string parse_error;
    spans = ScanDefinitionSpans(job.path, text, parse_error);
    job.parsed = true;
    for (std::size_t i = 0; i < todo.size(); ++i) {
      if (!results[i].miss && (results[i].rung < static_cast<int>(HealRung::kAnywhere))) continue;
      HealInput in = inputs(*todo[i]);
      in.symbol = SpanNamed(spans, todo[i]->symbol, std::max<Index>(0, in.line - 1));
      results[i] = ResolveAnchor(in);
    }
  }

  job.heals.reserve(job.heals.size() + todo.size());
  for (std::size_t i = 0; i < todo.size(); ++i) {
    const AnchorRow& row = *todo[i];
    const HealResult& found = results[i];
    job.rungs |= 1u << found.rung;
    AnchorHeal heal;
    heal.id = row.id;
    heal.seq = row.seq;
    heal.rung = found.rung;
    if (found.miss) {
      // A row that never said what it was cannot be looked for, so not finding
      // it is not evidence of anything. Counting it would hide the row at three
      // heals for a fact about the recorder.
      if (row.content.empty()) continue;
      heal.miss = true;
      job.heals.push_back(std::move(heal));
      continue;
    }
    heal.line = found.line;
    heal.blob = blob;
    // The write-back rule: the line always, the text only on a match that was
    // both close and unambiguous.
    if ((found.similarity >= kRefreshSimilarity) && found.unique) {
      heal.refresh_text = true;
      heal.content = file.lines[static_cast<std::size_t>(found.line - 1)];
      heal.context = AnchorContextAt(file, found.line - 1);
    }
    if (row.symbol_null && !spans.empty()) {
      heal.symbol = NameAt(spans, found.line);
      heal.set_symbol = !heal.symbol.empty();
    }
    job.heals.push_back(std::move(heal));
  }
}

}

void RunAnchorJob(AnchorJob& job) {
  // Published from a destructor, so every exit path wakes the pump -- including
  // the throwing one. A job that never publishes is polled for ever.
  struct Publish {
    AnchorJob& target;
    ~Publish() { target.done.store(true, std::memory_order_release); }
  } publish{job};

  try {
    RunAnchorJobBody(job);
  } catch (...) {
    // Nothing to say and nowhere to say it: healing is invisible when it works,
    // and a heal that failed leaves every row exactly as it was.
    job.heals.clear();
  }
}

// -- live shifting -------------------------------------------------------------------

namespace {

void SyncShadow(const PieceTable& table, AnchorShadow& shadow) {
  if (!shadow.valid) return;
  if (shadow.revision == table.revision) return;
  // Ahead of the document. Nothing in normal editing goes backwards -- undo
  // moves the revision forward like everything else -- so this is a table that
  // has been reset under the shadow, and there is no journal to walk back
  // through.
  if ((shadow.revision > table.revision) || (shadow.revision < table.journal_base)) {
    // The journal no longer reaches back to where these lines were true. The
    // shifts cannot be reconstructed and guessing at them is how anchors land
    // on neighbours -- so every row goes to the content ladder instead.
    for (AnchorShadow::Row& row : shadow.rows) row.dirty = true;
    shadow.revision = table.revision;
    return;
  }
  const auto from = static_cast<std::size_t>(shadow.revision - table.journal_base);
  for (std::size_t i = from; i < table.journal.size(); ++i) {
    const Edit& edit = table.journal[i];
    if (edit.IsEmpty()) continue;
    const Index start = edit.start_point.row;
    const Index old_end = edit.old_end_point.row;
    const Index new_end = edit.new_end_point.row;
    for (AnchorShadow::Row& row : shadow.rows) {
      if (row.dirty) continue;
      const Index anchor = row.line - 1;
      // "Above" is about the anchor's *text*, not about its row number. An edit
      // that ends exactly at the start of the anchor line, and leaves the new
      // text ending there too, has taken nothing off it and added nothing to
      // it: whole lines were inserted or removed above, and the anchor moved
      // without changing. Anything else touching the line is a change to it.
      const bool above =
          (old_end < anchor) || ((old_end == anchor) && (edit.old_end_point.column == 0) &&
                                 (edit.new_end_point.column == 0));
      if (above) {
        row.line = std::max<Index>(1, row.line + (new_end - old_end));
        continue;
      }
      if (start > anchor) continue;
      row.dirty = true;
    }
  }
  shadow.revision = table.revision;
}

AnchorShadow::Row* FindShadowRow(AnchorShadow& shadow, std::int64_t id) {
  for (AnchorShadow::Row& row : shadow.rows) {
    if (row.id == id) return &row;
  }
  return nullptr;
}

Document* DocumentWithId(Editor& ed, Index id) {
  if (ed.doc.id == id) return &ed.doc;
  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    Document& doc = (ed.buffers.empty() || (i == ed.active)) ? ed.doc : ed.buffers[i];
    if (doc.id == id) return &doc;
  }
  return nullptr;
}

}

void SyncAnchorShadow(Document& doc) { SyncShadow(doc.table, doc.anchors); }

void AdoptAnchorRows(Editor& ed, Document& doc) {
  if (!ed.project || !HasDiskFile(doc) || IsExcerptView(doc)) return;
  SyncAnchorShadow(doc);
  const std::vector<AnchorRow> stored =
      ed.project->AnchorPositionsFor(LocationKey(doc.file.string()));
  AnchorShadow& shadow = doc.anchors;
  std::vector<AnchorShadow::Row> next;
  next.reserve(stored.size());
  for (const AnchorRow& row : stored) {
    const AnchorShadow::Row* known = FindShadowRow(shadow, row.id);
    // A row whose `seq` has not moved is a row the recorder has not touched, so
    // whatever this shadow has shifted it to is the newer of the two answers. A
    // seq that moved is a fresh record, and the store's line is the place the
    // user was just standing in.
    if ((known != nullptr) && (known->seq == row.seq)) {
      next.push_back(*known);
      continue;
    }
    next.push_back(AnchorShadow::Row{row.id, row.line, row.seq, false});
  }
  shadow.rows = std::move(next);
  shadow.valid = true;
  shadow.revision = doc.table.revision;
}

bool AnchorShadowLine(Editor& ed, Document& doc, std::int64_t id, Index& line) {
  if ((id == 0) || !HasDiskFile(doc) || IsExcerptView(doc)) return false;
  AdoptAnchorRows(ed, doc);
  const AnchorShadow::Row* row = FindShadowRow(doc.anchors, id);
  if ((row == nullptr) || row->dirty) return false;
  line = row->line;
  return true;
}

// -- triggers ----------------------------------------------------------------------

void StartAnchorHeal(Editor& ed, const fs::path& path, std::string text, bool have_text) {
  if (!ed.project || path.empty()) return;
  const std::string key = LocationKey(path.string());
  for (const std::shared_ptr<AnchorJob>& running : ed.anchor_jobs) {
    // One at a time per file. A second job over the same rows would be racing
    // the first to describe one file two ways, and the trigger that queued it
    // will come round again.
    if (running->key == key) return;
  }

  auto job = std::make_shared<AnchorJob>();
  job->rows = ed.project->AnchorsFor(key);
  if (job->rows.empty()) return;
  job->key = key;
  job->path = path;
  job->text = std::move(text);
  job->have_text = have_text;
  // Read here, on the one thread that may: both are memoised process-wide.
  const fs::path root = ProjectRoot();
  if (!GitBranch(root).empty()) job->git_root = root;

  // A document open on this file knows where its rows have drifted to since the
  // store last saw them, and that is the line the ladder should start from.
  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    Document& doc = (ed.buffers.empty() || (i == ed.active)) ? ed.doc : ed.buffers[i];
    if (!HasDiskFile(doc) || (LocationKey(doc.file.string()) != key)) continue;
    AdoptAnchorRows(ed, doc);
    job->doc_id = doc.id;
    job->doc_revision = doc.table.revision;
    for (AnchorRow& row : job->rows) {
      const AnchorShadow::Row* shadow = FindShadowRow(doc.anchors, row.id);
      if ((shadow == nullptr) || shadow->dirty) continue;
      // Flagged when it actually moved: the line is then the buffer's, not the
      // one the row's blob was recorded against, and rung 2 -- which maps
      // stored coordinates -- must not shift it a second time. A shadow line
      // equal to the stored one is the stored one, and needs no flag.
      row.live_line = (shadow->line != row.line);
      row.line = shadow->line;
    }
    break;
  }

  StartScanWorker(ed.settings.scan_workers);
  ed.anchor_jobs.push_back(job);
  std::ignore = ThreadPool::Instance().AddTask([job] { RunAnchorJob(*job); });
}

namespace {

void ApplyAnchorJob(Editor& ed, const AnchorJob& job) {
  if (job.heals.empty() || !ed.project) return;
  // The store decides. A batch that rolled back wrote nothing, and moving the
  // shadow anyway would leave the healed lines living in exactly one place --
  // one that AdoptAnchorRows then keeps in preference to the store, so the two
  // never converge and nothing is ever counted as having failed.
  if (!ed.project->ApplyHeals(job.heals)) return;
  if (job.doc_id < 0) return;
  Document* doc = DocumentWithId(ed, job.doc_id);
  if (doc == nullptr) return;

  // Which rows the write-back actually took. A heal whose row the recorder has
  // touched since the snapshot was skipped on its seq, and the store now holds
  // a newer line than this job computed; putting the job's line in the shadow
  // would reintroduce exactly what the guard threw out.
  const std::vector<AnchorRow> stored = ed.project->AnchorPositionsFor(job.key);

  // The healed lines are true of the text the job was handed, which is the
  // buffer as it stood at `doc_revision`. Anything typed while the job ran is
  // in the journal, so stand them up at that revision and let the journal carry
  // them the rest of the way -- the same catch-up every other reader gets.
  AnchorShadow healed;
  healed.valid = true;
  healed.revision = job.doc_revision;
  for (const AnchorHeal& heal : job.heals) {
    if (heal.miss) continue;
    const auto found = std::ranges::find(stored, heal.id, &AnchorRow::id);
    if ((found == stored.end()) || (found->seq != heal.seq)) continue;
    healed.rows.push_back(AnchorShadow::Row{heal.id, heal.line, 0, false});
  }
  if (healed.rows.empty()) return;
  SyncShadow(doc->table, healed);

  AdoptAnchorRows(ed, *doc);
  for (const AnchorShadow::Row& row : healed.rows) {
    AnchorShadow::Row* mine = FindShadowRow(doc->anchors, row.id);
    if (mine == nullptr) continue;
    mine->line = row.line;
    mine->dirty = row.dirty;
  }
}

}

bool PumpAnchorHeals(Editor& ed) {
  bool running = false;
  for (std::size_t i = 0; i < ed.anchor_jobs.size();) {
    const std::shared_ptr<AnchorJob> job = ed.anchor_jobs[i];
    if (!job->done.load(std::memory_order_acquire)) {
      running = true;
      ++i;
      continue;
    }
    ed.anchor_jobs.erase(ed.anchor_jobs.begin() + static_cast<std::ptrdiff_t>(i));
    ApplyAnchorJob(ed, *job);
  }
  return running;
}

}
