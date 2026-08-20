#include "textobject.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>

#include "query.h"
#include "unicode.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kTextObjectsFile = "textobjects.scm";

// One budget for the whole lookup: the parse, and the match over the tree it
// produced. It is read once, so however the cost divides between the two, a
// keystroke cannot cost more than this.
//
// 500 ms because this runs on the UI thread, under `mi`/`ma`/`]f` -- a typed
// key, not a background scan -- so the ceiling is what someone will sit through
// once, not what a worker may spend. It stays at what the parse alone has
// always been allowed rather than dropping to syntax.cpp's 25 ms frame budget:
// this path is entered once per keystroke and not once per draw, and a smaller
// share would refuse text objects on the large files that work today. The query
// half of it was previously unbounded altogether, which is the thing being
// fixed: matching is quadratic in the depth of the tree over a range every
// ancestor contains, so a file that parses in milliseconds can match for
// minutes -- and the editor is frozen for all of them.
constexpr auto kTextObjectBudget = std::chrono::milliseconds{500};

}

bool TextObjectRanges(const PieceTable& table, const fs::path& path, std::string_view object,
                      std::span<const std::string_view> suffixes, std::vector<ObjectRange>& out,
                      std::string& error, Syntax* syntax, Interval limit) {
  out.clear();
  error.clear();
  const std::string_view language = LanguageForPath(path);
  if (language.empty()) {
    error = "no tree-sitter grammar for this file";
    return false;
  }
  if (FindRuntimeFile(fs::path{"queries"} / language / kTextObjectsFile).empty()) {
    error = "no textobjects.scm for " + std::string{language};
    return false;
  }

  static constexpr std::array<std::string_view, 1> kFiles{kTextObjectsFile};
  const std::shared_ptr<CompiledQuery> compiled = CompileQuery(language, kFiles, error);
  if (compiled == nullptr) return false;

  const auto wants = [&](std::string_view name) {
    for (const std::string_view suffix : suffixes) {
      if ((name.size() == object.size() + 1 + suffix.size()) && name.starts_with(object) &&
          (name[object.size()] == '.') && name.ends_with(suffix)) {
        return true;
      }
    }
    return false;
  };

  const std::span<const std::string> names = CaptureNamesOf(*compiled);
  std::vector<char> wanted(names.size(), 0);
  bool any = false;
  for (size_t i = 0; i < names.size(); ++i) {
    if (!wants(names[i])) continue;
    wanted[i] = 1;
    any = true;
  }
  if (!any) {
    error = std::string{language} + "'s textobjects.scm has no " + std::string{object};
    return false;
  }

  const auto finish = [&] {
    if (!limit.empty()) {
      const Index lo = limit.front();
      const Index hi = limit.back() + 1;
      std::erase_if(out, [lo, hi](const ObjectRange& o) { return (o.to <= lo) || (o.from >= hi); });
    }
    std::ranges::sort(out, [](const ObjectRange& a, const ObjectRange& b) {
      return (a.from != b.from) ? (a.from < b.from) : (a.to < b.to);
    });
    const auto dup = std::ranges::unique(out, [](const ObjectRange& a, const ObjectRange& b) {
      return (a.from == b.from) && (a.to == b.to);
    });
    out.erase(dup.begin(), dup.end());
  };

  if ((syntax != nullptr) && (syntax->Language() == language)) {
    std::vector<Capture> captures;
    bool exhausted = false;
    if (!syntax->Captures(table, kFiles, limit, captures, error, &exhausted)) return false;
    for (const Capture& capture : captures) {
      if (wants(capture.name)) out.push_back(ObjectRange{capture.from, capture.to});
    }
    // The same complaint the standalone path below makes, from the path every
    // `mi`, `ma` and `]f` in an open buffer takes. This one used not to ask at
    // all, so a query the frame budget cut in half came back as a shorter list
    // of objects and nothing else -- and the caller picked the innermost, or the
    // next one along, out of *those*: a selection or a jump that is silently
    // wrong rather than one that is missing. Which of the two ways it fell short
    // is not asked here: Captures reports one state, and the answer either way
    // is that what follows is built from part of the file.
    if (exhausted) error = "the text-object query came back short -- some objects are missing";
    finish();
    return true;
  }

  const std::string text = ReadDocRange(table, Interval(0, DocLength(table)));
  const std::string_view view{text};
  uint32_t view_end = 0;
  if (!TreeSitterByteRange(view.size(), view_end)) {
    error = "buffer is larger than 4 GiB -- no text objects in it";
    return false;
  }

  // One clock reading for both halves below.
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + kTextObjectBudget;

  const std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser{ts_parser_new(),
                                                                     ts_parser_delete};
  ParsedBuffer parsed = ParseBuffer(parser.get(), *compiled, language, view, kTextObjectBudget);
  if (!parsed.grammar_error.empty()) {
    error = std::move(parsed.grammar_error);
    return false;
  }
  if (!parsed) {
    error = "parse gave up -- file too large for textobjects";
    return false;
  }

  // Budgeted like the parse, and capped like symbols.cpp's scan, for the same
  // two reasons: a tree that parsed cheaply can still be matched over for
  // minutes, and a cursor left at tree-sitter's default match limit allocates a
  // capture list per partial match a file starts. `query_deadline` and
  // `query_options` outlive the exec deliberately -- the cursor keeps a pointer
  // to the options and reads the callback out of them on every next_match
  // below, so they cannot be temporaries.
  Deadline query_deadline{deadline, nullptr};
  TSQueryCursorOptions query_options{};
  query_options.payload = &query_deadline;
  query_options.progress_callback = StopQueryAtDeadline;

  TSQueryCursor* cursor = ts_query_cursor_new();
  ts_query_cursor_set_match_limit(cursor, kMaxQueryMatchStates);
  ts_query_cursor_set_byte_range(cursor, 0, view_end);
  ts_query_cursor_exec_with_options(cursor, QueryOf(*compiled),
                                    ts_tree_root_node(parsed.tree.get()), &query_options);

  TSQueryMatch match;
  while (ts_query_cursor_next_match(cursor, &match)) {
    if (!PredicatesHold(*compiled, match, NodeTextIn, &view)) continue;
    for (uint16_t i = 0; i < match.capture_count; ++i) {
      if (wanted[match.captures[i].index] == 0) continue;
      const TSNode node = match.captures[i].node;
      const auto from = static_cast<Index>(ts_node_start_byte(node));
      const auto to = static_cast<Index>(ts_node_end_byte(node));
      if (to > from) out.push_back(ObjectRange{from, to});
    }
  }

  // Both ways this can come back short, said out loud. `out` keeps what was
  // found -- half the objects is a usable answer where none is not -- so the
  // return stays true and the complaint rides in `error`, which is what a
  // caller must show instead of "no function here". Read before the delete:
  // the flag belongs to the cursor.
  if (query_deadline.expired) {
    error = "text objects exceeded the " + std::to_string(kTextObjectBudget.count()) +
            "ms budget -- some are missing";
  } else if (ts_query_cursor_did_exceed_match_limit(cursor)) {
    error = "too many query matches -- some text objects are missing";
  }
  ts_query_cursor_delete(cursor);

  finish();
  return true;
}

}
