#include "symbols.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>

#include "query.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kDefinitionsFile = "definitions.scm";
constexpr std::string_view kReferencesFile = "references.scm";

std::span<const std::string_view> FilesFor(SymbolKind kind) {
  static constexpr std::array<std::string_view, 1> definitions{kDefinitionsFile};
  static constexpr std::array<std::string_view, 1> references{kReferencesFile};
  static constexpr std::array<std::string_view, 2> both{kDefinitionsFile, kReferencesFile};
  switch (kind) {
    case SymbolKind::kDefinitions: return definitions;
    case SymbolKind::kReferences: return references;
    case SymbolKind::kBoth: return both;
  }
  return definitions;
}

// Memoised, because the scan asks this once per *file* and the answer is a
// property of the runtime tree rather than of the file: on a project of ten
// thousand sources the unmemoised form is up to six fs::exists() per source --
// three runtime roots times two query files -- to re-derive the same "yes" ten
// thousand times. Keyed exactly as CompileQuery's cache is (language, then the
// file names), thread_local for the same reason: the scan runs on worker
// threads, and this way there is no lock on the hot path.
//
// Like the grammar and query caches beside it, this makes a query file added
// while koi is running invisible until it restarts. That is already true of a
// query that has been compiled once, so the answer here does not become staler
// than the thing it is a prelude to.
bool HasAnyQuery(std::string_view language, std::span<const std::string_view> files) {
  std::string key{language};
  for (const std::string_view file : files) {
    key += '\0';
    key += file;
  }
  thread_local std::vector<std::pair<std::string, bool>> cache;
  for (const auto& [known, answer] : cache) {
    if (known == key) return answer;
  }
  const bool found = std::ranges::any_of(files, [language](std::string_view file) {
    return !FindRuntimeFile(fs::path{"queries"} / language / file).empty();
  });
  cache.emplace_back(std::move(key), found);
  return found;
}

std::string_view LStrip(std::string_view text) {
  size_t at = 0;
  while ((at < text.size()) && (std::isspace(static_cast<unsigned char>(text[at])) != 0)) ++at;
  text.remove_prefix(at);
  return text;
}

constexpr auto kScanBudget = std::chrono::seconds{10};

// The longest symbol name a scan will carry. Long enough for any real
// declaration or reference row; short enough that a capture which happens to
// span a whole nested expression stays one row of text, not the file.
constexpr size_t kMaxSymbolNameBytes = 200;

struct Scanner {
  TSParser* parser{ts_parser_new()};
  TSQueryCursor* cursor{ts_query_cursor_new()};

  // Read by the cursor on every ts_query_cursor_next_match, long after the exec
  // that installed them returned, so they live here rather than in ScanFile --
  // whose frame is a coroutine's, suspended and resumed around those very
  // calls.
  Deadline query_deadline;
  TSQueryCursorOptions query_options{};

  Scanner() {
    if (cursor != nullptr) ts_query_cursor_set_match_limit(cursor, kMaxQueryMatchStates);
  }
  ~Scanner() {
    if (cursor != nullptr) ts_query_cursor_delete(cursor);
    if (parser != nullptr) ts_parser_delete(parser);
  }
  Scanner(const Scanner&) = delete;
  Scanner& operator=(const Scanner&) = delete;
};

// Adds a complaint without displacing one -- first-complaint-wins within a scan
// and, because ScanSymbols no longer clears what it is handed, across scans as
// well. It used to clear: a caller threading one string through two scans (the
// interactive lookup does exactly that -- hot files, then the whole project)
// lost the first scan's diagnostic to the second scan's silence, so a grammar
// that failed to load left an empty picker and no explanation.
void Remember(std::string& error, std::string reason) {
  if (error.empty()) error = std::move(reason);
}

// Relaxed because there is nothing to order against: the flag is the whole
// message, and a scan that reads a stale `false` just does one more unit of
// work before the next check sees it.
bool Cancelled(const std::atomic<bool>* cancel) {
  return (cancel != nullptr) && cancel->load(std::memory_order_relaxed);
}

std::generator<Symbol> ScanFile(Scanner& scanner, const fs::path& path, SymbolKind kind,
                                std::string& error, std::string_view containing_word,
                                const std::atomic<bool>* cancel) {
  const std::string_view language = LanguageForPath(path);
  if (language.empty()) co_return;

  const std::span<const std::string_view> files = FilesFor(kind);
  if (!HasAnyQuery(language, files)) co_return;

  std::string compile_error;
  const std::shared_ptr<CompiledQuery> compiled = CompileQuery(language, files, compile_error);
  if (compiled == nullptr) {
    Remember(error, std::move(compile_error));
    co_return;
  }

  // The read is the expensive half of a file that the word prefilter then
  // throws away, and compiling the query above can itself have taken long
  // enough for the flag to flip.
  if (Cancelled(cancel)) co_return;

  std::error_code read_ec;
  const std::string bytes = ReadWholeFile(path, read_ec);
  if ((read_ec == std::errc::no_such_file_or_directory) ||
      (read_ec == std::errc::not_a_directory)) {
    co_return;
  }
  if (read_ec) {
    // With the reason: this is the one line a scan that came back thin leaves
    // behind, and "cannot read" alone does not say whether the file is a
    // directory, locked away, or on a mount that just went sour.
    Remember(error, "cannot read " + path.string() + ": " + read_ec.message());
    co_return;
  }
  if (bytes.empty()) co_return;
  // A file tree-sitter cannot address is not scannable: say so and move on. Out
  // of reach in practice (the read above has already allocated the 4 GiB),
  // which is exactly why it must not be the branch that quietly lies.
  uint32_t scan_end = 0;
  if (!TreeSitterByteRange(bytes.size(), scan_end)) {
    Remember(error, path.filename().string() + " is larger than 4 GiB -- not scanned");
    co_return;
  }
  const std::string_view contents{bytes};

  if (!containing_word.empty() &&
      (contents.find(containing_word) == std::string_view::npos)) {
    co_return;
  }

  // One clock reading for the whole of this file's scan: the budget below is
  // what is *left* of the ten seconds after the parse, not a second ten
  // seconds. A file cannot cost more than kScanBudget no matter how the cost
  // divides between parsing it and matching over it.
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + kScanBudget;

  ParsedBuffer parsed =
      ParseBuffer(scanner.parser, *compiled, language, contents, kScanBudget, cancel);
  if (!parsed.grammar_error.empty()) {
    Remember(error, std::move(parsed.grammar_error));
    co_return;
  }
  if (!parsed) {
    if (parsed.timed_out) {
      Remember(error, path.filename().string() + " exceeded the " +
                          std::to_string(std::chrono::seconds{kScanBudget}.count()) +
                          "s parse budget -- its symbols are missing");
    }
    co_return;
  }

  // Budgeted like the parse, and for the same reason. The parse being cheap
  // says nothing about the match: matching is quadratic in the depth of the
  // tree over a range every ancestor contains, so a file that parses in
  // milliseconds can match for minutes -- and this loop runs on a scan worker
  // that a cancelled lookup is waiting on. The mask inside StopQueryAtDeadline
  // is the one syntax.cpp's painter uses, chosen against what a single cursor
  // operation costs.
  scanner.query_deadline = Deadline{deadline, cancel};
  scanner.query_options.payload = &scanner.query_deadline;
  scanner.query_options.progress_callback = StopQueryAtDeadline;
  ts_query_cursor_set_byte_range(scanner.cursor, 0, scan_end);
  ts_query_cursor_exec_with_options(scanner.cursor, QueryOf(*compiled),
                                    ts_tree_root_node(parsed.tree.get()), &scanner.query_options);

  const std::string path_text = path.string();
  TSQueryMatch match;
  while (ts_query_cursor_next_match(scanner.cursor, &match)) {
    // Every iteration rather than every Nth: the iteration it guards already
    // advances the query cursor, evaluates predicates and copies capture text,
    // so a relaxed load costs nothing measurable next to it, and a counter
    // would only make a cancelled scan on a match-dense file run longer.
    if (Cancelled(cancel)) co_return;
    if (!PredicatesHold(*compiled, match, NodeTextIn, &contents)) continue;
    for (uint16_t i = 0; i < match.capture_count; ++i) {
      const TSNode node = match.captures[i].node;
      const TSPoint start = ts_node_start_point(node);
      std::string scratch;
      const std::string_view raw = NodeTextIn(&contents, node, scratch);
      const std::string_view name = LStrip(raw);
      if (name.empty()) continue;
      // The stripped run has to be paid for in the position too. The point is
      // where the *capture* starts, and for a capture that begins with
      // whitespace -- an ATX heading indented two spaces, an indented setext
      // heading -- that is not where the name starts, so the row said "line 4,
      // column 1" and pointed at the indentation instead of the '#'. A leading
      // run containing a newline would move the line as well. Columns are
      // counted in bytes, which is what ts_node_start_point reports.
      size_t row = start.row;
      size_t column = start.column;
      for (const char c : raw.substr(0, raw.size() - name.size())) {
        if (c == '\n') {
          ++row;
          column = 0;
        } else {
          ++column;
        }
      }
      if (!containing_word.empty() && !ContainsWord(name, containing_word)) continue;
      // Capped: reference queries capture the whole call expression by design,
      // and nested calls nest their captures, so f1(f2(f3(...))) yields names
      // of N, N-1, ... 1 tokens -- O(N^2) bytes in total. An 85 KB file of
      // nested calls drove the picker-staging path to 1.9 GB of RSS. Nothing
      // downstream needs more than a row's worth of text; truncate on a
      // code-point start so a clipped name stays valid UTF-8.
      std::string_view kept = name;
      if (name.size() > kMaxSymbolNameBytes) {
        kept = name.substr(0, kMaxSymbolNameBytes);
        while (!kept.empty() &&
               ((static_cast<unsigned char>(name[kept.size()]) & 0xC0) == 0x80)) {
          kept.remove_suffix(1);
        }
      }
      std::string cleaned{kept};
      if (kept.size() < name.size()) cleaned += "\xE2\x80\xA6";  // U+2026 ellipsis
      for (char& c : cleaned) {
        if ((c == '\n') || (c == '\r') || (c == '\t')) c = ' ';
      }
      co_yield Symbol{path_text, static_cast<Index>(row) + 1, static_cast<Index>(column) + 1,
                      std::move(cleaned)};
    }
  }

  // Both ways this file's symbols can be incomplete, said out loud. The loop
  // above ends the same way whether it ran out of matches, ran out of time, or
  // had matches recycled out from under it, and a picker that quietly shows
  // three of a file's four hundred definitions is worse than one that shows
  // three and says why. Cancellation is not reported: it is what the caller
  // asked for, and it leaves through the co_return above without reaching here.
  if (scanner.query_deadline.expired) {
    Remember(error, path.filename().string() + " exceeded the " +
                        std::to_string(std::chrono::seconds{kScanBudget}.count()) +
                        "s scan budget -- some of its symbols are missing");
  }
  if (ts_query_cursor_did_exceed_match_limit(scanner.cursor)) {
    Remember(error, path.filename().string() +
                        ": too many query matches -- some symbols are missing");
  }
}

}

std::generator<Symbol> ScanSymbols(std::span<const std::string> paths, SymbolKind kind,
                                   std::string& error, std::string_view containing_word,
                                   const std::atomic<bool>* cancel) {
  Scanner scanner;
  for (const std::string& path : paths) {
    // The only check that bounds the *walk*. Cancellation used to reach the
    // scan solely through the parser's progress callback, so a file the word
    // prefilter rejected -- or one whose language has no query -- yielded
    // nothing, never suspended the generator, and never gave the consumer a
    // turn to notice it had been cancelled. A cancelled scan read the project.
    if (Cancelled(cancel)) co_return;
    const fs::path one{path};
    // Each symbol is yielded on through this frame rather than handed over with
    // `elements_of`. The handoff is the better shape and costs one resume less
    // per symbol -- when the compiler makes the symmetric-transfer tail call it
    // is required to. Under AddressSanitizer it does not: the frame-poisoning
    // epilogue runs after the transfer and leaves this frame alive, so a run of
    // files that yield nothing -- which is most of a project scan, and all of
    // one behind a word filter -- stacked one ScanSymbols/ScanFile frame pair
    // per file and overflowed the stack at around 1,200 of them. That is an
    // instrumentation artefact and not a shipping defect, but it made the
    // sanitizer build unable to run the one workload most worth running under
    // it: koi scanning a real project. One extra resume per symbol -- a jump
    // and a switch on the resume index, against a whole-file read, a parse and
    // a query match -- buys a scan whose stack depth is constant in the number
    // of paths under every build.
    for (Symbol&& symbol : ScanFile(scanner, one, kind, error, containing_word, cancel)) {
      co_yield std::move(symbol);
    }
  }
}

std::generator<Symbol> ScanSymbols(fs::path path, SymbolKind kind, std::string& error) {
  const std::array<std::string, 1> one{path.string()};
  co_yield std::ranges::elements_of(ScanSymbols(std::span<const std::string>{one}, kind, error));
}

std::vector<Symbol> CollectSymbols(std::span<const std::string> paths, SymbolKind kind,
                                   std::string& error, std::string_view containing_word) {
  std::vector<Symbol> out;
  for (Symbol&& symbol : ScanSymbols(paths, kind, error, containing_word)) {
    out.push_back(std::move(symbol));
  }
  return out;
}

bool ContainsWord(std::string_view haystack, std::string_view needle) {
  if (needle.empty() || (needle.size() > haystack.size())) return false;
  const auto is_word = [](char c) {
    return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
           ((c >= '0') && (c <= '9')) || (c == '_');
  };
  size_t at = 0;
  while ((at = haystack.find(needle, at)) != std::string_view::npos) {
    const bool left = (at == 0) || !is_word(haystack[at - 1]);
    const size_t after = at + needle.size();
    const bool right = (after >= haystack.size()) || !is_word(haystack[after]);
    if (left && right) return true;
    at = at + 1;
  }
  return false;
}

std::string FormatSymbolRow(const Symbol& symbol) {
  std::string row = symbol.path + ":" + std::to_string(symbol.line) + ":" +
                    std::to_string(symbol.column);
  if (!symbol.name.empty()) row += ":" + symbol.name;
  return row;
}

namespace {

bool ParseIndexField(std::string_view text, Index& out) {
  if (text.empty()) return false;
  Index value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if ((ec != std::errc{}) || (ptr != text.data() + text.size()) || (value < 0)) return false;
  out = value;
  return true;
}

}

// Parsed into locals and published in one block at the end, because `row` is
// allowed to view into `out` -- a caller re-parsing a symbol's own path is the
// obvious way to reach this -- and assigning out.path first freed the buffer
// the rest of the parse was still reading.
bool ParseSymbolRow(std::string_view row, Symbol& out) {
  if (row.empty()) return false;

  std::string path{row};
  Index line = 1;
  Index column = 1;
  std::string name;
  bool positioned = false;

  for (size_t at = row.find(':'); at != std::string_view::npos; at = row.find(':', at + 1)) {
    if (at == 0) continue;
    const std::string_view tail = row.substr(at + 1);
    const size_t line_end = std::min(tail.find(':'), tail.size());
    Index line_no = 0;
    if (!ParseIndexField(tail.substr(0, line_end), line_no)) continue;

    positioned = true;
    path = std::string{row.substr(0, at)};
    line = std::max<Index>(1, line_no);
    if (line_end != tail.size()) {
      const std::string_view rest = tail.substr(line_end + 1);
      const size_t column_end = std::min(rest.find(':'), rest.size());
      Index col = 0;
      if (ParseIndexField(rest.substr(0, column_end), col)) {
        column = std::max<Index>(1, col);
        if (column_end != rest.size()) name = std::string{rest.substr(column_end + 1)};
      } else {
        name = std::string{rest};
      }
    }
    break;
  }

  // A row with no `:<line>` field anywhere is a target only if it names a file
  // that is really there. Everything a picker prints comes back through here --
  // its banner, its echo of the query, its "no matches" line -- and every one of
  // those used to parse into a path of its own, so the caller's "the picker
  // returned nothing koi could open" was unreachable and the pick opened a
  // phantom buffer named after the banner instead. Bare paths still open: the
  // file picker's rows are paths relative to where koi runs, which is where the
  // check runs too.
  std::error_code ec;
  if (!positioned && !fs::exists(fs::path{path}, ec)) return false;

  out.path = std::move(path);
  out.line = line;
  out.column = column;
  out.name = std::move(name);
  return true;
}

namespace {

constexpr std::string_view kDim = "\033[38;5;246m";
constexpr std::string_view kReset = "\033[0m";

}

// Refuses rather than builds a row that decodes to something else. A row is
// display, padding, a tab, then the payload, and RowPayload takes everything
// after the *last* tab: a payload with a tab of its own comes back missing its
// prefix -- "src/a\tb.cpp:2:3:Widget" recovers as "b.cpp:2:3:Widget", which is a
// different file, and one koi will happily create. A newline in either half
// stops the row being one line at all. Neither is recoverable at the far end,
// where the row is just text again, so there is nothing to do but not make it.
// A tab in the *display* is deliberate -- it is how a row lays out its columns.
std::string PickerRow(std::string_view display, std::string_view payload) {
  if (payload.find_first_of("\t\n\r") != std::string_view::npos) return {};
  if (display.find_first_of("\n\r") != std::string_view::npos) return {};

  std::string row{display};
  row.append(kHidePad, ' ');
  row += '\t';
  row += payload;
  return row;
}

std::string_view RowPayload(std::string_view row) {
  const size_t tab = row.rfind('\t');
  return (tab == std::string_view::npos) ? row : row.substr(tab + 1);
}

// Empty when the symbol cannot be said as one row -- see PickerRow. Callers
// stage rows into a picker's input and must drop an empty one rather than
// write a blank line.
std::string SymbolPickerRow(const Symbol& symbol) {
  if (symbol.path.find_first_of("\t\n\r") != std::string::npos) return {};

  std::string display = symbol.name;
  display += '\t';
  display += kDim;
  display += fs::path{symbol.path}.filename().string();
  display += ':';
  display += std::to_string(symbol.line);
  display += kReset;
  return PickerRow(display, FormatSymbolRow(symbol));
}

std::vector<std::string_view> IndexableLanguages() {
  std::vector<std::string_view> out;
  for (const std::string_view language : KnownLanguages()) {
    if (!FindRuntimeFile(fs::path{"queries"} / language / kDefinitionsFile).empty()) {
      out.push_back(language);
    }
  }
  return out;
}

}
