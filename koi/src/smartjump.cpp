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

// One clause's terms against one candidate. Terms AND: every one has to match,
// and what the candidate is worth is their sum. `first` is the earliest byte any
// of them landed on, which is the last of the three tie-breaks.
bool ScoreTerms(const std::vector<std::string>& terms, const SmartRow& row,
                const FuzzyConfig& config, double& banded, int& first) {
  banded = 0.0;
  first = 0;
  if (terms.empty()) return true;
  int earliest = std::numeric_limits<int>::max();
  for (const std::string& term : terms) {
    const FuzzyMatch scored = ScoreBanded(term, row.text, row.name_offset, config);
    if (!scored.matched) return false;
    banded += scored.banded;
    earliest = std::min(earliest, scored.first);
  }
  first = earliest;
  return true;
}

// A candidate mid-flight. A pointer and eight numbers, deliberately: the sort
// moves these, and a struct with four strings in it turns ranking four hundred
// rows into four hundred string moves per comparison round. The strings are
// built once, for the survivors, in the order they came out in.
struct Candidate {
  const SmartRow* row{nullptr};
  SmartKind kind{SmartKind::kFile};
  double banded{0};
  double score{0};
  int first{0};
};

bool Better(const Candidate& a, const Candidate& b) {
  return RanksBefore(RankKey{a.score, a.row->text.size(), a.first},
                     RankKey{b.score, b.row->text.size(), b.first});
}

// The store's key, spelled from here, remembered per key rather than per row: a
// file with twenty locations in it is one resolve and one stat, not twenty.
struct PathCache {
  const fs::path* root{nullptr};
  std::unordered_map<std::string, std::string> known;

  // Empty when nothing on disk answers to it -- a deleted file, or an excerpt
  // view's name, which is a location in the store and not a place on disk.
  const std::string& Resolve(const std::string& key) {
    const auto [at, fresh] = known.try_emplace(key);
    if (!fresh) return at->second;
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
    clause->push_back(word.text);
  }
  // `c` and `d` name different kinds of target -- a visited line, a visited
  // definition -- and a query cannot land on both. `f` narrows either; these
  // two do not narrow each other. A bare number is a line, so it is content
  // for this rule too.
  if (!out.symbol_terms.empty() && (!out.content_terms.empty() || out.has_line)) {
    out.error = "`c` and `d` cannot be combined -- a line and a definition are different targets";
  }
  return out;
}

void BuildSmartCorpus(ProjectStore& store, SmartCorpus& out) {
  out.files.clear();
  out.symbols.clear();
  out.locations.clear();
  out.build_ms = 0;

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

  out.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           started)
                     .count();
}

namespace {

// The ranking proper: everything up to the strings. Both entry points run this
// and only this, so the preview and the landing cannot come to different
// conclusions about what the best match is.
std::vector<Candidate> RankCandidates(const SmartCorpus& corpus, const SmartQuery& query,
                                      ProjectStore* store) {
  // The file clause. What it produces is both a filter -- by path, order-free,
  // which is what makes clause order not change the result -- and a bonus every
  // candidate in a matching file carries.
  const bool filter_by_file = !query.file_terms.empty();
  std::unordered_map<std::string_view, double> file_bonus;
  std::unordered_map<std::string_view, int> file_first;
  // Without a file clause every file scores zero and nothing reads the maps:
  // the intersection is skipped, the bonus is zero by lookup miss, and the file
  // tier is only reached when there is a file clause at all.
  if (filter_by_file) {
    for (const SmartRow& row : corpus.files) {
      double banded = 0;
      int first = 0;
      if (!ScoreTerms(query.file_terms, row, FuzzyConfig::Path(), banded, first)) continue;
      file_bonus.emplace(std::string_view{row.key}, banded);
      file_first.emplace(std::string_view{row.key}, first);
    }
  }

  // The symbol clause: `d` terms match definitions and nothing else. It does
  // not filter the content clause -- the parser refuses `c` with `d` -- so all
  // it shares with the file clause is being narrowed by it.
  const bool filter_by_symbol = !query.symbol_terms.empty();
  std::vector<const SmartRow*> symbol_rows;
  std::vector<std::pair<double, int>> symbol_scores;
  for (const SmartRow& row : corpus.symbols) {
    if (filter_by_file && !file_bonus.contains(std::string_view{row.file_key})) continue;
    double banded = 0;
    int first = 0;
    if (!ScoreTerms(query.symbol_terms, row, FuzzyConfig::Default(), banded, first)) continue;
    symbol_rows.push_back(&row);
    symbol_scores.emplace_back(banded, first);
  }

  const auto file_score = [&](std::string_view key) {
    const auto found = file_bonus.find(key);
    return (found == file_bonus.end()) ? 0.0 : found->second;
  };
  std::vector<Candidate> ranked;
  // The clause that is going to be the target, so the vector is sized once
  // rather than doubling five times on the way to four hundred rows.
  ranked.reserve(!query.content_terms.empty() || query.has_line ? corpus.locations.size()
                 : !query.symbol_terms.empty()                  ? symbol_rows.size()
                                                                : file_bonus.size());

  // The most specific clause present decides what you land on: content, then
  // symbol, then file. A bare line number is a content clause -- it names a
  // line, not a file.
  const bool want_content = !query.content_terms.empty() || query.has_line;
  if (want_content) {
    // |line - N| per file, so `key 640` lands on the one visited line in
    // keymap.cpp nearest 640 rather than on every row of it.
    std::unordered_map<std::string_view, Index> nearest;
    if (query.has_line) {
      for (const SmartRow& row : corpus.locations) {
        if (filter_by_file && !file_bonus.contains(std::string_view{row.file_key})) continue;
        double banded = 0;
        int first = 0;
        if (!ScoreTerms(query.content_terms, row, FuzzyConfig::Default(), banded, first)) continue;
        const Index away =
            (row.line > query.line) ? (row.line - query.line) : (query.line - row.line);
        const auto [at, fresh] = nearest.try_emplace(std::string_view{row.file_key}, away);
        if (!fresh && (away < at->second)) at->second = away;
      }
    }

    for (const SmartRow& row : corpus.locations) {
      if (filter_by_file && !file_bonus.contains(std::string_view{row.file_key})) continue;
      double banded = 0;
      int first = 0;
      if (!ScoreTerms(query.content_terms, row, FuzzyConfig::Default(), banded, first)) continue;
      if (query.has_line) {
        const Index away =
            (row.line > query.line) ? (row.line - query.line) : (query.line - row.line);
        const auto found = nearest.find(std::string_view{row.file_key});
        if ((found == nearest.end()) || (away != found->second)) continue;
      }
      ranked.push_back(Candidate{&row, SmartKind::kLocation,
                                 banded + file_score(row.file_key), 0.0, first});
    }
  } else if (filter_by_symbol) {
    for (std::size_t i = 0; i < symbol_rows.size(); ++i) {
      const SmartRow& row = *symbol_rows[i];
      ranked.push_back(Candidate{&row, SmartKind::kSymbol,
                                 symbol_scores[i].first + file_score(row.file_key), 0.0,
                                 symbol_scores[i].second});
    }
  } else {
    for (const SmartRow& row : corpus.files) {
      const auto found = file_bonus.find(std::string_view{row.key});
      if (found == file_bonus.end()) continue;
      ranked.push_back(Candidate{&row, SmartKind::kFile, found->second, 0.0,
                                 file_first[std::string_view{row.key}]});
    }
  }

  for (Candidate& one : ranked) {
    one.score = FinalScore(one.banded, one.row->frecency, one.row->same_branch,
                           one.row->in_branch_diff, 0.0);
  }
  std::ranges::sort(ranked, Better);

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

}  // namespace

std::vector<SmartMatch> RankSmartMatches(const SmartCorpus& corpus, const SmartQuery& query,
                                         ProjectStore* store) {
  std::vector<SmartMatch> out;
  if (!query.error.empty() || query.Empty()) return out;
  const std::vector<Candidate> ranked = RankCandidates(corpus, query, store);
  // The strings, once, in the order the ranking put them in.
  out.reserve(ranked.size());
  for (const Candidate& one : ranked) out.push_back(MatchOf(one));
  return out;
}

SmartSummary SummariseSmartMatches(const SmartCorpus& corpus, const SmartQuery& query,
                                   ProjectStore* store) {
  SmartSummary out;
  if (!query.error.empty() || query.Empty()) return out;
  const std::vector<Candidate> ranked = RankCandidates(corpus, query, store);
  out.count = ranked.size();
  if (!ranked.empty()) {
    std::size_t line_at = 0;
    out.display = DisplayOf(*ranked.front().row, ranked.front().kind, line_at);
  }
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

std::string SmartJumpFeedback(const SmartQuery& query, const SmartSummary& summary) {
  if (!query.error.empty()) return query.error;
  if (query.Empty()) return {};
  if (summary.count == 0) return "not been there";
  return std::to_string(summary.count) + "  " + summary.display;
}

SmartHandoff SmartJumpHandoff(const SmartQuery& query) {
  if (!query.content_terms.empty() || query.has_line) {
    return {SmartPicker::kContent, PickerPattern(query.content_terms)};
  }
  if (!query.symbol_terms.empty()) return {SmartPicker::kSymbol, PickerPattern(query.symbol_terms)};
  return {SmartPicker::kFile, PickerPattern(query.file_terms)};
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
