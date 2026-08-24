// smart-jump: the parser, the corpus snapshot and the clause pipeline
// (docs/smart-jump.md, Syntax / Scoring / "Parser and pipeline").
//
// Not a search tool, a re-visit tool. The corpus is the store -- files, symbols
// and locations actually visited -- read once when the prompt opens and
// re-scored in full on every keystroke. Nothing in here touches disk, a parser
// or a subprocess after the snapshot is built.
//
// Everything below is a free function over plain data. The Editor-facing half --
// the prompt, the landing rules, the bounce timer -- lives in navigate.cpp
// beside the excerpt views and the recorder it has to drive.
#ifndef KOI_SMARTJUMP_H_
#define KOI_SMARTJUMP_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "piece_doc.h"

namespace koi {

struct ProjectStore;

// -- query words ----------------------------------------------------------------

// One word of a typed query, with the quotes already taken off. Shared with the
// search-query parser in navigate.cpp rather than spelled twice: two ideas of
// what a term is would mean `"two words"` splitting one way in one prompt and
// another way in the next.
struct QueryWord {
  std::string text;
  bool quoted{false};
};

std::vector<QueryWord> SplitQuery(std::string_view query);

// -- the parser ----------------------------------------------------------------

// The clause keywords: `c` content, `d` definitions, `f` file. A bare single
// letter is one of these and applies to every term after it until the next
// keyword; terms before any keyword are the file clause.
inline constexpr std::string_view kSmartKeywords = "cdf";

// What that costs, and the whole of it: a match term needs at least two
// characters, so a lone letter is always a keyword. One letter cannot
// discriminate four hundred rows anyway.
inline constexpr std::size_t kSmartMinTerm = 2;

// One query, parsed. Clause order does not reach here -- `key cpp c split` and
// `c split f key cpp` produce the same three vectors -- which is what makes the
// clauses order-free by construction rather than by the pipeline being careful.
struct SmartQuery {
  std::vector<std::string> file_terms;
  std::vector<std::string> symbol_terms;
  std::vector<std::string> content_terms;
  bool has_line{false};
  Index line{0};
  // Non-empty means the query says nothing usable and this is what to show.
  std::string error;
  // Every word as typed, joined by single spaces. `queries` is keyed on this,
  // so a lookup builds the same string the write did.
  std::string typed;

  bool Empty() const {
    return file_terms.empty() && symbol_terms.empty() && content_terms.empty() && !has_line;
  }
};

SmartQuery ParseSmartQuery(std::string_view text);

// -- the corpus ------------------------------------------------------------------

enum class SmartKind : std::uint8_t { kFile, kSymbol, kLocation };

// One candidate, with everything the blend needs already on it. Built once; the
// keystroke path reads and never writes.
struct SmartRow {
  // What the store holds, and the same thing spelled from here. `key` is also
  // the file half of the adaptive target.
  std::string key;
  std::string path;
  // The key of the file this row lives in. The file clause intersects on it, so
  // a file row carries its own.
  std::string file_key;
  // What the matcher scores, and where the name inside it starts -- the
  // basename for a path, byte 0 for a symbol or a line of text.
  std::string text;
  std::size_t name_offset{0};
  Index line{1};
  Index col{0};
  // `locations.id`, and 0 for the other two kinds: it is what asks an open
  // buffer where the row has drifted to (AnchorShadowLine).
  std::int64_t id{0};
  // weight x recency multiplier, and deliberately *without* the branch factor
  // FrecentFiles folds in -- here the branch is its own prior in the blend, and
  // multiplying it in as well would count it twice.
  double frecency{0};
  bool same_branch{false};
  bool in_branch_diff{false};
};

struct SmartCorpus {
  std::vector<SmartRow> files;
  std::vector<SmartRow> symbols;
  std::vector<SmartRow> locations;
  // What the read and the stat sweep cost, in milliseconds. The budget is 5ms
  // (docs/smart-jump.md, Implementation) and a budget nobody can see is a budget
  // nobody keeps.
  double build_ms{0};
};

// The store's own recency multiplier (project.cpp's KOI_MULT_SQL), in C++ so
// that one expression serves all three tables and so that the branch factor
// FrecentFiles multiplies in stays out of it.
inline double FrecencyMultiplier(double age_seconds) {
  const double hours = age_seconds / 3600.0;
  if (hours < 1.0) return 4.0;
  if (hours < 24.0) return 2.0;
  if (hours < 168.0) return 1.0;
  if (hours < 720.0) return 0.5;
  return 0.25;
}

// A location that has missed this many heals in a row is hidden, never deleted
// (docs/smart-jump.md, "Never delete on a miss").
inline constexpr std::int64_t kSmartMaxMisses = 3;

// Three SELECTs and one stat sweep over the distinct paths they name. Rows whose
// file is gone are dropped here rather than being offered and then failing to
// open.
void BuildSmartCorpus(ProjectStore& store, SmartCorpus& out);

// -- the pipeline -------------------------------------------------------------------

struct SmartMatch {
  std::string path;
  // What `queries` credits when this one is accepted. A file is its key; a
  // symbol and a location are that key plus what distinguishes them inside it,
  // so accepting `d pin` does not teach the whole file.
  std::string key;
  Index line{1};
  Index col{0};
  std::int64_t row_id{0};
  double score{0};
  SmartKind kind{SmartKind::kFile};
  // The definition's name, for symbol targets only: the landing re-finds it in
  // the file as it is now, because `symbols.line` is not healed and a stored
  // line goes stale the moment the definition moves.
  std::string symbol;
  // `path:line  text`, ready for the status line and for the excerpt note.
  std::string display;
  // Where the line number starts inside `display`, and 0 for a file row, which
  // carries none. The landing heals the stored line before it goes anywhere, and
  // what the status says has to be the line it healed to.
  std::size_t display_line{0};
};

// `display` with the line number in it replaced by `line`. The stored line is a
// cache; the landing corrects it, and this is how the announcement says the same
// thing the cursor does.
std::string SmartDisplayAt(const SmartMatch& match, Index line);

// How many leading rows get an AdaptiveUse lookup. The prior is worth at most
// kAdaptivePrior, so a row further down than that in the no-adaptive order
// cannot climb to the top however confirmed it is -- which is what lets the
// store be hit O(rows shown) instead of O(corpus) on every keystroke.
inline constexpr std::size_t kSmartAdaptiveProbe = 16;

// How much of a stored line the display carries.
inline constexpr std::size_t kSmartDisplayBytes = 60;

// One evaluation: score every clause against its corpus, intersect by file, and
// rank what the most specific clause present produced. `store` may be null --
// then no row gets the adaptive prior, which is exactly what a corpus with no
// query history scores anyway.
std::vector<SmartMatch> RankSmartMatches(const SmartCorpus& corpus, const SmartQuery& query,
                                         ProjectStore* store);

// How many rows a query matches and what the best of them looks like -- the
// whole of what the keystroke path reads. Same corpus, same blend, same order as
// RankSmartMatches, so the preview and the landing cannot disagree about what is
// best; what it skips is the strings per survivor that nothing reads while
// typing.
struct SmartSummary {
  std::size_t count{0};
  std::string display;
};

SmartSummary SummariseSmartMatches(const SmartCorpus& corpus, const SmartQuery& query,
                                   ProjectStore* store);

// The status line while typing: the count, the best target and its line text --
// or the parse error, or "not been there". docs/smart-jump.md, Landing: Enter is
// never a surprise.
std::string SmartJumpFeedback(const SmartQuery& query, const SmartSummary& summary);

// -- the dead end ---------------------------------------------------------------

// Which picker a query that matched nothing is handed to. Smart-jump never
// widens its own list -- the corpus is places you have been, and that is the
// whole of the answer -- but a dead end is not an answer either, so the terms
// go on to the thing that does search the project.
enum class SmartPicker : std::uint8_t { kFile, kSymbol, kContent };

struct SmartHandoff {
  SmartPicker picker{SmartPicker::kFile};
  // The deciding clause's terms as the picker takes them: escaped and joined
  // with `.*`, because a picker query is a PCRE2 pattern and the terms are
  // abbreviations that only have to appear in that order. Never the clause
  // keywords -- the picker would search for the letters.
  std::string query;
};

// The most specific clause present decides, the same rule the landing lands by:
// content > symbol > file. A bare line number is a content clause with no terms
// of its own -- the content picker opens empty rather than searching the
// project for a number.
SmartHandoff SmartJumpHandoff(const SmartQuery& query);

// What the status line calls it: "not been there -- content picker".
std::string_view SmartPickerName(SmartPicker picker);


// What the editor holds between two smart jumps. The corpus belongs to the open
// prompt; the ranked list outlives it, because picker_jump_next steps through
// that list long after the prompt has closed and a new query replaces it.
struct SmartJumpState {
  SmartCorpus corpus;
  std::vector<SmartMatch> matches;
  // Which of them the last landing was, so next and prev step from where you
  // are rather than from the top of the list.
  std::size_t at{0};
  // The terms that produced `matches`, spelled the way `queries` is keyed.
  std::string typed;
};

}  // namespace koi

#endif  // KOI_SMARTJUMP_H_
