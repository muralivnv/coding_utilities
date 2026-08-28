#include "smartjump.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "fuzzy.h"
#include "project.h"
#include "sqlite.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

double Now() {
  const auto since = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration<double>(since).count();
}

bool AllDigits(std::string_view word) {
  if (word.empty()) return false;
  return std::ranges::all_of(word, [](char c) { return (c >= '0') && (c <= '9'); });
}

// Where the basename starts. Bands 1 and 2 are statements about it, and the
// prefix bonus is measured from it, so `key` earns the whole of it against
// `koi/src/keymap.cpp` and none of it against a directory called `key`.
std::size_t BaseNameOffset(std::string_view key) {
  const std::size_t slash = key.find_last_of('/');
  return (slash == std::string_view::npos) ? 0 : (slash + 1);
}

// Cut on a code-point boundary, so a clipped line is still UTF-8 -- this goes
// into a document (the excerpt note) as well as onto the status line.
std::string_view Clip(std::string_view text, std::size_t bytes) {
  if (text.size() <= bytes) return text;
  std::size_t at = bytes;
  while ((at > 0) && ((static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)) --at;
  return text.substr(0, at);
}

// One allocation. This is built for every match, and a chain of operator+ is
// half a dozen -- which at four hundred matches is most of what an evaluation
// costs that is not the DP. `line_at` comes back as the offset of the line
// number inside it, so a healed landing can say the line it went to.
std::string DisplayOf(const SmartRow& row, SmartKind kind, std::size_t& line_at) {
  line_at = 0;
  if (kind == SmartKind::kFile) return row.key;
  const std::string_view text = Clip(row.text, kSmartDisplayBytes);
  const bool cut = text.size() < row.text.size();
  char digits[24];
  const std::to_chars_result end = std::to_chars(digits, digits + sizeof(digits), row.line);
  const std::string_view line{digits, static_cast<std::size_t>(
                                          (end.ec == std::errc{}) ? (end.ptr - digits) : 0)};
  std::string out;
  out.reserve(row.file_key.size() + line.size() + text.size() + 8);
  out += row.file_key;
  out += ':';
  line_at = out.size();
  out += line;
  out += "  ";
  out += text;
  if (cut) out += "...";
  return out;
}

// One typed term and the facets it is allowed to reach. A bare term reaches
// every facet a row has; each keyword cuts one term down to a single facet, and
// a query where every term is cut is the old clause pipeline exactly.
struct Facets {
  const std::string* text{nullptr};
  // The key of the file the row lives in, scored with FuzzyConfig::Path().
  bool path{false};
  // A symbol's name, scored with FuzzyConfig::Default().
  bool name{false};
  // A location's stored line, scored the same way.
  bool content{false};
};

// Which of a row's own text a term may be asked about. Only two kinds have one:
// a file row's text *is* its path, so the path facet is the whole of it and the
// file loop below never calls ScoreRow.
enum class TextFacet : std::uint8_t { kName, kContent };

// The path facet, scored once per (term, file key) and shared by every row in
// that file: a file with twenty locations in it is one DP per term, not twenty.
// Keyed on the corpus's own strings, which outlive the ranking, and stable under
// rehash because the vectors live in the map's nodes.
struct PathFacets {
  const std::vector<Facets>* terms{nullptr};
  std::unordered_map<std::string_view, std::vector<FuzzyMatch>> known;

  const FuzzyMatch* Of(const std::string& key) {
    const auto [at, fresh] = known.try_emplace(std::string_view{key});
    if (!fresh) return at->second.data();
    at->second.resize(terms->size());
    const std::size_t offset = BaseNameOffset(key);
    for (std::size_t i = 0; i < terms->size(); ++i) {
      const Facets& term = (*terms)[i];
      if (term.path) {
        at->second[i] = ScoreBanded(*term.text, key, offset, FuzzyConfig::Path());
      }
    }
    return at->second.data();
  }
};

// One row against the whole query. Terms AND -- every one has to reach a facet
// -- and what the row is worth is the sum of what each term reached. A term that
// matches both facets is worth the better of them and not their total: it has
// said one thing about the row, and counting it twice would let a file whose
// name is in every line of it outrank the line that actually says something.
//
// `first` and the length the tie-break uses are both statements about the row's
// own text, so only the text facet moves it; a row carried by its path alone
// keeps first at 0, which is what the file clause used to leave behind.
// `text_hit` is (†): whether anything found this row by its own text.
bool ScoreRow(const std::vector<Facets>& terms, const FuzzyMatch* path, TextFacet facet,
              std::string_view text, double& banded, int& first, bool& text_hit) {
  banded = 0.0;
  first = 0;
  text_hit = false;
  int earliest = std::numeric_limits<int>::max();
  for (std::size_t i = 0; i < terms.size(); ++i) {
    const Facets& term = terms[i];
    const bool reads_text = (facet == TextFacet::kName) ? term.name : term.content;
    double best = 0.0;
    bool matched = false;
    if (reads_text) {
      // A symbol name and a stored line are each looked up by the whole of
      // themselves, so the name starts at byte 0 for either.
      const FuzzyMatch hit = ScoreBanded(*term.text, text, 0, FuzzyConfig::Default());
      if (hit.matched) {
        best = hit.banded;
        matched = true;
        text_hit = true;
        earliest = std::min(earliest, hit.first);
      }
    }
    if (term.path && path[i].matched) {
      best = std::max(best, path[i].banded);
      matched = true;
    }
    if (!matched) return false;
    banded += best;
  }
  if (earliest != std::numeric_limits<int>::max()) first = earliest;
  return true;
}

// A candidate mid-flight. A pointer and a handful of numbers, deliberately: the
// sort moves these, and a struct with four strings in it turns ranking four
// hundred rows into four hundred string moves per comparison round. The strings
// are built once, for the survivors, in the order they came out in.
struct Candidate {
  const SmartRow* row{nullptr};
  SmartKind kind{SmartKind::kFile};
  double banded{0};
  double score{0};
  int first{0};
  // Where this row came out of the corpus sweep, and the sort's last word: the
  // three keys before it are all query-dependent, and two rows equal on all
  // three must not swap between keystrokes.
  std::size_t order{0};
};

bool Better(const Candidate& a, const Candidate& b) {
  return RanksBefore(RankKey{a.score, a.row->text.size(), a.first, a.order},
                     RankKey{b.score, b.row->text.size(), b.first, b.order});
}

// The store's key, spelled from here, remembered per key rather than per row: a
// file with twenty locations in it is one resolve and one stat, not twenty.
struct PathCache {
  const fs::path* root{nullptr};
  std::unordered_map<std::string, std::string> known;
  // Stats done, which is misses and not calls: the difference between the two is
  // the whole point of the cache, and it is the thing the cost test asserts on.
  std::size_t stats{0};

  // Empty when nothing on disk answers to it -- a deleted file, or an excerpt
  // view's name, which is a location in the store and not a place on disk.
  const std::string& Resolve(const std::string& key) {
    const auto [at, fresh] = known.try_emplace(key);
    if (!fresh) return at->second;
    ++stats;
    std::string path = ResolveStorePath(*root, key);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) path.clear();
    at->second = std::move(path);
    return at->second;
  }
};

}  // namespace

SmartQuery ParseSmartQuery(std::string_view text) {
  SmartQuery out;
  const std::vector<QueryWord> words = SplitQuery(text);

  // The file clause is open to begin with: it is the common first clause, which
  // is why it is bare and why `f` exists only to reopen it after another clause.
  std::vector<std::string>* clause = &out.file_terms;
  // Whether an `f` is what opened it. Terms typed before any keyword are bare --
  // they route themselves -- and terms after an `f` are pinned to paths, which
  // is the only thing that still tells the two apart once both are in
  // `file_terms`.
  bool pinned = false;
  // That `d` was typed at all, which its terms cannot say for it: a digit is
  // taken as a line before any clause records it, so `d 640` would otherwise
  // leave nothing behind to conflict with the line and the `d` would go
  // silently missing.
  bool definitions = false;
  for (const QueryWord& word : words) {
    if (!out.typed.empty()) out.typed += ' ';
    out.typed += word.text;

    // A quoted word is a term whatever it looks like: quoting is how a file
    // called `640` is spelled. It does not buy a shorter term -- the two-byte
    // minimum below applies to quoted words as well, so `'z'` is still an
    // error.
    if (!word.quoted) {
      if (word.text.size() == 1) {
        const char letter = word.text[0];
        if (kSmartKeywords.find(letter) != std::string_view::npos) {
          clause = (letter == 'c')   ? &out.content_terms
                   : (letter == 'd') ? &out.symbol_terms
                                     : &out.file_terms;
          pinned = pinned || (letter == 'f');
          definitions = definitions || (letter == 'd');
          continue;
        }
        if (!AllDigits(word.text)) {
          out.error = "`" + word.text + "` is not a clause -- c content, d definitions, f file";
          return out;
        }
      }
      if (AllDigits(word.text)) {
        out.has_line = true;
        out.line = static_cast<Index>(std::strtoll(word.text.c_str(), nullptr, 10));
        continue;
      }
    }

    if (word.text.size() < kSmartMinTerm) {
      out.error = "`" + word.text + "` is too short -- a term needs two characters";
      return out;
    }
    if (clause == &out.file_terms) out.file_pinned.push_back(pinned ? 1 : 0);
    clause->push_back(word.text);
  }
  // `c` and `d` name different kinds of target -- a visited line, a visited
  // definition -- and a query cannot land on both. `f` narrows either; these
  // two do not narrow each other. A bare number is a line, so it is content
  // for this rule too. Each conflict names what was typed: a query with no `c`
  // in it cannot be told that `c` is the trouble.
  if (definitions && !out.content_terms.empty()) {
    out.error = "`c` and `d` cannot be combined -- content and a definition are different targets";
  } else if (definitions && out.has_line) {
    out.error =
        "a line number and `d` cannot be combined -- a line and a definition are different targets";
  }
  return out;
}

void BuildSmartCorpus(ProjectStore& store, SmartCorpus& out) {
  out.files.clear();
  out.symbols.clear();
  out.locations.clear();
  out.build_ms = 0;
  out.stats = 0;

  sqlite3* db = store.Connection();
  if (db == nullptr) return;

  const auto started = std::chrono::steady_clock::now();
  const fs::path root = ProjectRoot();
  const std::string branch = GitBranch(root);
  // Cached on the branch, so this is a subprocess at most once per branch
  // switch and a vector read every other time (project.h).
  const std::vector<std::string>& changed = BranchDiffFiles();
  const std::unordered_set<std::string_view> in_diff{changed.begin(), changed.end()};
  const double now = Now();
  PathCache paths{&root, {}};

  // What branch each file was last visited on, for the two tables that have no
  // branch of their own to answer with. A symbol is a place in a file, and the
  // file is what was visited on a branch.
  std::unordered_map<std::string, std::string> file_branch;

  {
    Stmt stmt{db, "SELECT path, visits, edits, last_ts, branch, last_line, last_col"
                  " FROM files WHERE last_ts > 0;"};
    while (stmt.Step()) {
      SmartRow row;
      row.key = stmt.Column(0);
      row.path = paths.Resolve(row.key);
      if (row.path.empty()) continue;
      const double weight =
          static_cast<double>(stmt.Integer(1)) + (3.0 * static_cast<double>(stmt.Integer(2)));
      const double last_ts = stmt.Double(3);
      row.frecency = weight * FrecencyMultiplier(now - last_ts);
      const std::string row_branch = stmt.Column(4);
      row.same_branch = !branch.empty() && (row_branch == branch);
      row.in_branch_diff = in_diff.contains(std::string_view{row.key});
      row.line = std::max<Index>(1, static_cast<Index>(stmt.Integer(5)));
      row.col = static_cast<Index>(stmt.Integer(6));
      file_branch.emplace(row.key, row_branch);
      row.file_key = row.key;
      row.text = row.key;
      row.name_offset = BaseNameOffset(row.key);
      out.files.push_back(std::move(row));
    }
  }

  {
    Stmt stmt{db, "SELECT symbol, file, line, visits, last_ts FROM symbols"
                  " WHERE line > 0 AND visits > 0 AND last_ts > 0;"};
    while (stmt.Step()) {
      SmartRow row;
      row.text = stmt.Column(0);
      row.file_key = stmt.Column(1);
      if (row.text.empty()) continue;
      row.path = paths.Resolve(row.file_key);
      if (row.path.empty()) continue;
      row.line = std::max<Index>(1, static_cast<Index>(stmt.Integer(2)));
      row.frecency =
          static_cast<double>(stmt.Integer(3)) * FrecencyMultiplier(now - stmt.Double(4));
      const auto found = file_branch.find(row.file_key);
      row.same_branch =
          !branch.empty() && (found != file_branch.end()) && (found->second == branch);
      row.in_branch_diff = in_diff.contains(std::string_view{row.file_key});
      // A symbol is looked up by name, so that is the whole of the candidate and
      // the name starts at byte 0.
      row.name_offset = 0;
      row.key = row.file_key + "#" + row.text;
      out.symbols.push_back(std::move(row));
    }
  }

  {
    Stmt stmt{db, "SELECT id, path, line, col, content, visits, kind, last_ts, branch"
                  " FROM locations WHERE misses < ?1 AND content IS NOT NULL AND content <> '';"};
    stmt.Int(1, kSmartMaxMisses);
    while (stmt.Step()) {
      SmartRow row;
      row.id = stmt.Integer(0);
      row.file_key = stmt.Column(1);
      row.path = paths.Resolve(row.file_key);
      if (row.path.empty()) continue;
      row.line = std::max<Index>(1, static_cast<Index>(stmt.Integer(2)));
      row.col = static_cast<Index>(stmt.Integer(3));
      row.text = stmt.Column(4);
      // An edit outweighs a visit three to one: "where I edited" out-signals
      // "where I looked", and the kind is what keeps the two apart.
      const double weight =
          static_cast<double>(stmt.Integer(5)) * ((stmt.Integer(6) == 1) ? 3.0 : 1.0);
      row.frecency = weight * FrecencyMultiplier(now - stmt.Double(7));
      const std::string row_branch = stmt.Column(8);
      row.same_branch = !branch.empty() && (row_branch == branch);
      row.in_branch_diff = in_diff.contains(std::string_view{row.file_key});
      row.name_offset = 0;
      row.key = row.file_key + "@" + std::to_string(row.id);
      out.locations.push_back(std::move(row));
    }
  }

  out.stats = paths.stats;
  out.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           started)
                     .count();
}

namespace {

// The query as facet permissions, one entry per typed term, in clause order.
// Everything below reads this and never the clauses again, which is what makes
// the pinned case the general case with three of the four facets switched off.
std::vector<Facets> FacetsOf(const SmartQuery& query) {
  std::vector<Facets> terms;
  terms.reserve(query.file_terms.size() + query.symbol_terms.size() +
                query.content_terms.size());
  for (std::size_t i = 0; i < query.file_terms.size(); ++i) {
    const bool pinned = (i < query.file_pinned.size()) && (query.file_pinned[i] != 0);
    terms.push_back(Facets{&query.file_terms[i], true, !pinned, !pinned});
  }
  for (const std::string& one : query.symbol_terms) {
    terms.push_back(Facets{&one, false, true, false});
  }
  for (const std::string& one : query.content_terms) {
    terms.push_back(Facets{&one, false, false, true});
  }
  return terms;
}

// A destination: the file a row lives in and the line it lands on. Two rows that
// agree on both go to the same place, whatever kind either of them is.
struct Where {
  std::string_view file;
  Index line{0};
};

struct WhereHash {
  std::size_t operator()(const Where& one) const {
    return std::hash<std::string_view>{}(one.file) ^
           (std::hash<Index>{}(one.line) * 0x9e3779b97f4a7c15ULL);
  }
};

struct WhereEq {
  bool operator()(const Where& a, const Where& b) const {
    return (a.line == b.line) && (a.file == b.file);
  }
};

// What occupies a Where. Not one kind but a set of them, because a file row and
// a location can want the same spot and which of the two is already there is
// what decides the other's fate.
struct Held {
  bool file{false};
  bool location{false};
};

// The ranking proper: everything up to the strings. Both entry points run this
// and only this, so the preview and the landing cannot come to different
// conclusions about what the best match is.
std::vector<Candidate> RankCandidates(const SmartCorpus& corpus, const SmartQuery& query,
                                      ProjectStore* store, const SmartHeal& heal) {
  const std::vector<Facets> terms = FacetsOf(query);
  PathFacets paths{&terms, {}};

  // What each kind could possibly produce, answered before a single DP runs. A
  // term that reaches neither a kind's text nor a path can never be satisfied by
  // that kind, and (†) means a kind whose text no term reaches has nothing to be
  // found by -- so `c split` never touches a symbol and `f key cpp` never
  // touches a line, at the price of two passes over the terms.
  bool want_files = true;
  bool want_symbols = true;
  bool want_locations = true;
  bool any_name = false;
  bool any_content = false;
  for (const Facets& term : terms) {
    if (!term.path) want_files = false;
    if (!term.name && !term.path) want_symbols = false;
    if (!term.content && !term.path) want_locations = false;
    any_name = any_name || term.name;
    any_content = any_content || term.content;
  }
  want_symbols = want_symbols && any_name;
  // A line number is content evidence of its own -- it names a line, and a line
  // is what a location row is -- so it satisfies (†) for every location and
  // rules the other two kinds out entirely. That is the v2 behaviour unchanged:
  // digits still mean a line and nothing else.
  want_locations = want_locations && (any_content || query.has_line);
  if (query.has_line) {
    want_files = false;
    want_symbols = false;
  }

  std::vector<Candidate> ranked;
  ranked.reserve((want_files ? corpus.files.size() : 0) +
                 (want_symbols ? corpus.symbols.size() : 0) +
                 (want_locations ? corpus.locations.size() : 0));

  if (want_files) {
    // A file row's text is its path, so it has one facet and every term has to
    // land on it -- the file clause's old AND, said once.
    for (const SmartRow& row : corpus.files) {
      const FuzzyMatch* path = paths.Of(row.file_key);
      double banded = 0;
      int earliest = std::numeric_limits<int>::max();
      bool ok = true;
      for (std::size_t i = 0; i < terms.size(); ++i) {
        if (!path[i].matched) {
          ok = false;
          break;
        }
        banded += path[i].banded;
        earliest = std::min(earliest, path[i].first);
      }
      if (!ok) continue;
      ranked.push_back(Candidate{&row, SmartKind::kFile, banded, 0.0,
                                 (earliest == std::numeric_limits<int>::max()) ? 0 : earliest,
                                 ranked.size()});
    }
  }

  if (want_symbols) {
    for (const SmartRow& row : corpus.symbols) {
      double banded = 0;
      int first = 0;
      bool text_hit = false;
      if (!ScoreRow(terms, paths.Of(row.file_key), TextFacet::kName, row.text, banded, first,
                    text_hit)) {
        continue;
      }
      // (†). A definition nothing named is a definition its file row stands
      // for, and offering both is offering the same evidence twice.
      if (!text_hit) continue;
      ranked.push_back(Candidate{&row, SmartKind::kSymbol, banded, 0.0, first, ranked.size()});
    }
  }

  if (want_locations) {
    const std::size_t from = ranked.size();
    for (const SmartRow& row : corpus.locations) {
      double banded = 0;
      int first = 0;
      bool text_hit = false;
      if (!ScoreRow(terms, paths.Of(row.file_key), TextFacet::kContent, row.text, banded, first,
                    text_hit)) {
        continue;
      }
      if (!text_hit && !query.has_line) continue;
      ranked.push_back(Candidate{&row, SmartKind::kLocation, banded, 0.0, first, ranked.size()});
    }
    if (query.has_line) {
      // |line - N| per file, so `key 640` lands on the one visited line in
      // keymap.cpp nearest 640 rather than on every row of it. Over the
      // survivors and no further: a row the terms threw out cannot be the
      // nearest one to anything.
      std::unordered_map<std::string_view, Index> nearest;
      const auto away = [&query](const SmartRow& row) {
        return (row.line > query.line) ? (row.line - query.line) : (query.line - row.line);
      };
      for (std::size_t at = from; at < ranked.size(); ++at) {
        const SmartRow& row = *ranked[at].row;
        const auto [where, fresh] = nearest.try_emplace(std::string_view{row.file_key}, away(row));
        if (!fresh && (away(row) < where->second)) where->second = away(row);
      }
      const auto not_nearest = [&](const Candidate& one) {
        const auto found = nearest.find(std::string_view{one.row->file_key});
        return (found == nearest.end()) || (away(*one.row) != found->second);
      };
      ranked.erase(std::remove_if(ranked.begin() + static_cast<std::ptrdiff_t>(from),
                                  ranked.end(), not_nearest),
                   ranked.end());
    }
  }

  for (Candidate& one : ranked) {
    one.score = FinalScore(one.banded, one.row->frecency, one.row->same_branch,
                           one.row->in_branch_diff, 0.0);
  }
  std::ranges::sort(ranked, Better);

  // The same place twice, where one of the two is the file row standing for it.
  // A file's row lands raw on the line the file was left on, so a location that
  // *arrives* at that line is the same destination said twice -- the second of
  // them spends a row of five on a card that cannot even tell them apart,
  // because stepping onto it draws the same excerpt. (†) for file rows, and the
  // same reason: offering both is offering the same evidence twice.
  //
  // Arrives, not stores: every consumer heals a location through its anchor
  // before landing on it, so the stored line is a cache and comparing on it
  // collapses pairs that no longer share a destination while missing the pairs
  // that do. `heal` is that same question asked here, and without one -- no
  // buffer, no editor -- the stored line is the best answer there is.
  //
  // File against location and nothing else. A symbol's stored line has no anchor
  // behind it and the landing re-finds the definition by name, so it is not
  // comparable to either of the other two: symbols are never erased here and
  // never occupy a spot. Two locations on one line are two claims about it and
  // both stay. Order-aware, so whichever ranked better keeps its place. Before
  // the probe, so that the rows the probe looks at are the rows that survive.
  //
  // Only a file that has a row of its own can collide, so only locations in one
  // of those are healed at all -- a heal reads the store, and this runs on every
  // keystroke.
  std::unordered_set<std::string_view> has_file_row;
  for (const Candidate& one : ranked) {
    if (one.kind == SmartKind::kFile) has_file_row.insert(std::string_view{one.row->file_key});
  }
  std::unordered_map<Where, Held, WhereHash, WhereEq> seen;
  seen.reserve(ranked.size());
  ranked.erase(std::remove_if(ranked.begin(), ranked.end(),
                              [&](const Candidate& one) {
                                if (one.kind == SmartKind::kSymbol) return false;
                                const std::string_view file{one.row->file_key};
                                if (!has_file_row.contains(file)) return false;
                                Index line = one.row->line;
                                if ((one.kind == SmartKind::kLocation) && heal) {
                                  heal(one.row->id, line);
                                }
                                Held& held = seen[Where{file, line}];
                                if (one.kind == SmartKind::kFile) {
                                  if (held.location) return true;
                                  held.file = true;
                                } else {
                                  if (held.file) return true;
                                  held.location = true;
                                }
                                return false;
                              }),
               ranked.end());

  // The adaptive prior, and the one reason the store is touched on this path at
  // all. It is worth at most kAdaptivePrior, so a row further down than that
  // cannot reach the top however confirmed it is -- which is what makes probing
  // the leading rows exact for the decision Enter makes, at a bounded handful of
  // lookups instead of one per candidate.
  if ((store != nullptr) && !query.typed.empty() && !ranked.empty()) {
    const std::size_t probe = std::min(kSmartAdaptiveProbe, ranked.size());
    bool moved = false;
    for (std::size_t i = 0; i < probe; ++i) {
      Candidate& one = ranked[i];
      const double use = store->AdaptiveUse(query.typed, one.row->key);
      if (use <= 0.0) continue;
      one.score = FinalScore(one.banded, one.row->frecency, one.row->same_branch,
                             one.row->in_branch_diff, use);
      moved = true;
    }
    if (moved) {
      std::ranges::sort(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(probe),
                        Better);
    }
  }

  return ranked;
}

// One candidate, with its strings. Three or four heap allocations, which is why
// the keystroke path asks for one of these and Enter for all of them.
SmartMatch MatchOf(const Candidate& one) {
  const SmartRow& row = *one.row;
  SmartMatch match;
  match.path = row.path;
  match.key = row.key;
  match.line = row.line;
  match.col = (one.kind == SmartKind::kSymbol) ? 0 : row.col;
  match.row_id = (one.kind == SmartKind::kLocation) ? row.id : 0;
  match.score = one.score;
  match.kind = one.kind;
  if (one.kind == SmartKind::kSymbol) match.symbol = row.text;
  match.display = DisplayOf(row, one.kind, match.display_line);
  return match;
}

// The picker's query is a PCRE2 pattern -- the band filters its rows with
// `(?i)` and the query -- so terms join with `.*`: `key zzz` means key and then
// zzz, which is what the terms meant, where a space would mean the literal
// "key zzz". Each term is escaped first, because they are typed as
// abbreviations and `foo(` is a broken pattern rather than a search.
std::string PickerPattern(const std::vector<std::string>& terms) {
  static constexpr std::string_view kMeta = R"(\^$.[]|()?*+{})";
  std::string out;
  for (const std::string& one : terms) {
    if (!out.empty()) out += ".*";
    for (const char c : one) {
      if (kMeta.find(c) != std::string_view::npos) out += '\\';
      out += c;
    }
  }
  return out;
}

// Whether the dead end takes the bare door. Nothing was pinned to a kind and
// nothing named a line, so the query said only "somewhere I have been" and no
// clause is there to name a picker. Every term travels and the widest door
// takes them -- see SmartJumpHandoff for why that door is content.
bool BareHandoff(const SmartQuery& query) {
  if (!query.content_terms.empty() || !query.symbol_terms.empty() || query.has_line) return false;
  return std::ranges::any_of(query.file_pinned, [](std::uint8_t one) { return one == 0; });
}

}  // namespace

std::vector<SmartMatch> RankSmartMatches(const SmartCorpus& corpus, const SmartQuery& query,
                                         ProjectStore* store, const SmartHeal& heal) {
  std::vector<SmartMatch> out;
  if (!query.error.empty() || query.Empty()) return out;
  const std::vector<Candidate> ranked = RankCandidates(corpus, query, store, heal);
  // The strings, once, in the order the ranking put them in.
  out.reserve(ranked.size());
  for (const Candidate& one : ranked) out.push_back(MatchOf(one));
  return out;
}

SmartPreview PreviewSmartMatches(const SmartCorpus& corpus, const SmartQuery& query,
                                 ProjectStore* store, std::size_t want, const SmartHeal& heal) {
  SmartPreview out;
  if (!query.error.empty() || query.Empty()) return out;
  const std::vector<Candidate> ranked = RankCandidates(corpus, query, store, heal);
  out.count = ranked.size();
  const std::size_t keep = std::min(want, ranked.size());
  out.top.reserve(keep);
  for (std::size_t at = 0; at < keep; ++at) out.top.push_back(MatchOf(ranked[at]));
  return out;
}

std::string SmartDisplayAt(const SmartMatch& match, Index line) {
  if ((match.display_line == 0) || (line == match.line)) return match.display;
  std::size_t end = match.display_line;
  while ((end < match.display.size()) && (match.display[end] >= '0') &&
         (match.display[end] <= '9')) {
    ++end;
  }
  std::string out = match.display;
  out.replace(match.display_line, end - match.display_line, std::to_string(line));
  return out;
}

SmartRowText SmartRowAt(const SmartMatch& match, Index line) {
  SmartRowText out;
  // A file row is its path: DisplayOf wrote no line number into it, so there is
  // nothing to split off and nothing to heal.
  if (match.display_line == 0) {
    out.head = match.display;
    return out;
  }
  const std::string whole = SmartDisplayAt(match, line);
  std::size_t end = match.display_line;
  while ((end < whole.size()) && (whole[end] >= '0') && (whole[end] <= '9')) ++end;
  out.head = whole.substr(0, end);
  // Past the two spaces DisplayOf joins the line number to its text with.
  if ((end + 2) < whole.size()) out.text = whole.substr(end + 2);
  return out;
}

std::string SmartJumpFeedback(const SmartQuery& query, const SmartPreview& preview) {
  if (!query.error.empty()) return query.error;
  if (query.Empty()) return {};
  if (preview.top.empty()) return "not been there";
  return std::to_string(preview.count) + "  " + preview.top.front().display;
}

SmartHandoff SmartJumpHandoff(const SmartQuery& query) {
  // A bare query has no deciding clause, so nothing may be left at the door:
  // the whole of it goes to the picker whose rows carry a path, a line and a
  // line's text at once.
  if (BareHandoff(query)) return {SmartPicker::kContent, PickerPattern(query.file_terms)};
  if (!query.content_terms.empty() || query.has_line) {
    return {SmartPicker::kContent, PickerPattern(query.content_terms)};
  }
  if (!query.symbol_terms.empty()) return {SmartPicker::kSymbol, PickerPattern(query.symbol_terms)};
  return {SmartPicker::kFile, PickerPattern(query.file_terms)};
}

std::string SmartDroppedTerms(const SmartQuery& query, SmartPicker picker) {
  // The bare door takes everything, so there is nothing to admit to. Asked
  // about any other picker, the pinned rule below still answers -- the two are
  // read together and have to agree about one query at a time.
  if (BareHandoff(query) && (picker == SmartPicker::kContent)) return {};
  std::string out;
  const auto add = [&out](const std::vector<std::string>& terms) {
    for (const std::string& term : terms) {
      if (!out.empty()) out += ' ';
      out += '`' + term + '`';
    }
  };
  // Whatever the deciding clause did not take. In practice that is the file
  // terms: `c` and `d` cannot both be in a query that parses, and the file
  // picker is chosen only when the file clause is the whole of it.
  if (picker != SmartPicker::kContent) add(query.content_terms);
  if (picker != SmartPicker::kSymbol) add(query.symbol_terms);
  if (picker != SmartPicker::kFile) add(query.file_terms);
  return out;
}

std::string_view SmartPickerName(SmartPicker picker) {
  switch (picker) {
    case SmartPicker::kContent: return "content picker";
    case SmartPicker::kSymbol: return "symbol picker";
    case SmartPicker::kFile: break;
  }
  return "file picker";
}

}  // namespace koi
