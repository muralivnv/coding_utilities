#include "search.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>

#include "regex.h"
#include "unicode.h"

namespace koi {

// gai names the reason a compile failed in a field of a multi-line message that
// echoes the pattern first:
//
//   "PCRE2 compilation failed on pattern.\nPattern: %s\nError Offset: %d\nError: %s\n"
//
// so a pattern is perfectly able to contain the field marker itself, and
// anchoring on the first one handed the status line a slice of what the user
// typed instead of what PCRE2 objected to -- `Error: (` reported `(`.
//
// The reason is always the last such field: gai's marker is the final one in
// the format string, and PCRE2's message never contains a newline, so nothing
// line-anchored can follow it. Searching backwards therefore lands on gai's own
// marker no matter what the pattern smuggles in, including a newline of its
// own. Anchoring on the newline as well keeps a mid-line "Error: " from
// counting at all.
std::string OneLineReason(std::string_view what) {
  constexpr std::string_view kMarker = "\nError: ";
  std::string_view tail = what;
  if (const std::size_t at = what.rfind(kMarker); at != std::string_view::npos) {
    tail = what.substr(at + kMarker.size());
    // gai asks PCRE2 to write its message one byte into a buffer it filled with
    // '.', so exactly one dot sits in front of it. Strip that one and no more:
    // a run-stripper eats the leading dots of whatever it is handed.
    if (tail.starts_with('.')) tail.remove_prefix(1);
  }
  if (const std::size_t nl = tail.find('\n'); nl != std::string_view::npos) {
    tail = tail.substr(0, nl);
  }
  if (tail.empty()) return "bad pattern";
  return std::string{tail};
}

namespace {

// gai caps what one line can cost, but the cap is per subject and these loops
// run the pattern once per line of the document -- on the main thread, and for
// FindFirstInDocument once per keystroke typed into the search prompt. A cost
// small enough to be invisible on one line is still a frozen editor at 60k of
// them, so the sweep needs an end of its own.
//
// 150 ms is over the threshold where a keystroke stops feeling immediate and
// well under the point where a person reaches for the kill signal; every
// honest search over a large document finishes far inside it.
constexpr auto kSearchBudget = std::chrono::milliseconds{150};

std::string BudgetExhausted() {
  return "too slow -- gave up after " + std::to_string(kSearchBudget.count()) + " ms";
}

struct LineBudget {
  std::chrono::steady_clock::time_point until{std::chrono::steady_clock::now() + kSearchBudget};
  Index steps{0};

  // Reading the clock costs more than matching a short line against a literal,
  // so amortise it over 64 lines: the overshoot is 63 lines of whatever the
  // per-line cost is, which gai's match limit already bounds.
  bool Expired() {
    return (((++steps & 0x3F) == 0) && (std::chrono::steady_clock::now() >= until));
  }
};

struct Compiled {
  std::string pattern;
  std::optional<gai::Pcre2Regex> regex;
  std::string error;
  bool primed{false};
};

Compiled& Cached(std::string_view pattern) {
  static thread_local Compiled cache;
  if (cache.primed && (cache.pattern == pattern)) return cache;

  // What a primed entry promises is "regex engaged XOR error non-empty", and
  // everything below can throw: the pattern copy allocates, so does building
  // the reason string, and gai is not the only thing that could raise. An entry
  // marked primed on the way in is left claiming that promise by any escape,
  // with neither half of it true -- and the next call for the same pattern
  // would take the cache hit and read through a disengaged optional.
  //
  // So poison first and promise last. A throw out of here leaves primed false,
  // which costs one recompile and nothing else.
  cache.primed = false;
  cache.error.clear();
  cache.regex.reset();
  cache.pattern.assign(pattern);
  try {
    cache.regex.emplace(gai::Regex(gai::Compile(pattern,  true,  true)));
  } catch (const std::exception& e) {
    // The reason is worth having, but not at the price of the invariant: if
    // shaping it is what fails, a literal short enough to need no allocation
    // still satisfies "error non-empty".
    try {
      cache.error = OneLineReason(e.what());
    } catch (...) {
      cache.error = "bad pattern";
    }
  } catch (...) {
    cache.error = "bad pattern";
  }
  cache.primed = true;
  return cache;
}

}

bool FindInText(std::string_view pattern, std::string_view text, std::vector<Interval>& out,
                std::string& error) {
  if (pattern.empty()) return true;

  Compiled& compiled = Cached(pattern);
  // An empty error is Cached's word that the regex is there; the dereference
  // below is what actually needs the regex to be there. Ask about the thing
  // being dereferenced, so a broken invariant costs a bad search instead of the
  // process.
  if (!compiled.regex || !compiled.error.empty()) {
    error = compiled.error.empty() ? std::string{"bad pattern"} : compiled.error;
    return false;
  }

  static thread_local std::vector<gai::MatchSpan> spans;
  spans.clear();
  // A match-time failure is not "nothing here": the spans collected so far are a
  // prefix of the truth, and reporting them as the whole answer is how a search
  // comes back empty with no word about why. Same out-param as a compile
  // failure, same false -- the caller cannot use a partial line either way.
  std::string match_error;
  gai::FindSpans(*compiled.regex, text, spans, &match_error);
  if (!match_error.empty()) {
    error = match_error;
    return false;
  }
  for (const gai::MatchSpan& span : spans) {
    if (span.end <= span.start) continue;
    out.push_back(Interval(static_cast<Index>(span.start), static_cast<Index>(span.end)));
  }
  return true;
}

bool FindInDocument(const PieceTable& table, std::string_view pattern, Interval range,
                    std::vector<Interval>& out, std::string& error) {
  if (pattern.empty() || range.empty()) return true;

  const Index lo = range.front();
  const Index hi = range.back() + 1;
  const Index first_line = LineAt(table, lo);
  const Index last_line = LineAt(table, std::max<Index>(lo, hi - 1));

  std::string scratch;
  std::vector<Interval> hits;
  LineBudget budget;
  for (Index line = first_line; line <= last_line; ++line) {
    if (budget.Expired()) {
      error = BudgetExhausted();
      return false;
    }
    const Index base = LineStart(table, line);
    ReadDocRangeInto(table, LineContentRange(table, line), scratch);

    hits.clear();
    if (!FindInText(pattern, scratch, hits, error)) return false;
    for (const Interval& hit : hits) {
      const Index from = base + hit.front();
      const Index to = base + hit.back() + 1;
      if ((from >= lo) && (to <= hi)) out.push_back(Interval(from, to));
    }
  }
  return true;
}

bool FindFirstInDocument(const PieceTable& table, std::string_view pattern, Index from,
                         std::optional<Interval>& out, std::string& error) {
  out.reset();
  if (pattern.empty()) return true;

  bool bad_pattern = false;
  std::string scratch;
  std::vector<Interval> hits;
  // One budget across both sweeps: together they walk the document once, and
  // two budgets would let a pathological pattern cost twice the promise.
  LineBudget budget;
  const auto sweep = [&](Index from_line, Index to_line,
                         Index at_or_after) -> std::optional<Interval> {
    for (Index line = from_line; line < to_line; ++line) {
      if (budget.Expired()) {
        error = BudgetExhausted();
        bad_pattern = true;
        return std::nullopt;
      }
      const Index base = LineStart(table, line);
      ReadDocRangeInto(table, LineContentRange(table, line), scratch);
      hits.clear();
      if (!FindInText(pattern, scratch, hits, error)) {
        bad_pattern = true;
        return std::nullopt;
      }
      for (const Interval& hit : hits) {
        const Index at = base + hit.front();
        if (at >= at_or_after) return Interval(at, base + hit.back() + 1);
      }
    }
    return std::nullopt;
  };

  const Index lines = LineCount(table);
  const Index cursor_line = std::clamp(LineAt(table, from), Index{0}, std::max<Index>(0, lines - 1));
  out = sweep(cursor_line, lines, from);
  if (bad_pattern) return false;
  if (out) return true;
  out = sweep(0, cursor_line + 1, 0);
  return !bad_pattern;
}

}
