#include <tree_sitter/api.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "cli.h"
#include "query.h"
#include "symbols.h"
#include "syntax.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

constexpr std::array<std::string_view, 5> kOverviewQueries{
    "includes.scm", "functions.scm", "calls.scm", "classes.scm", "class_members.scm"};

struct Named {
  std::string name;
  Index line{0};
};

struct Scope {
  std::string name;
  std::string bases;
  Index line{0};
  Index first{0};
  Index last{0};
  std::vector<Named> calls;
  std::vector<Named> members;
  std::vector<Named> methods;
};

void SortAndDedupe(std::vector<Named>& items) {
  std::ranges::stable_sort(items, [](const Named& a, const Named& b) { return a.line < b.line; });
  std::unordered_set<std::string> seen;
  std::erase_if(items, [&seen](const Named& item) { return !seen.insert(item.name).second; });
}

std::string BaseClauseOf(TSNode node, std::string_view contents) {
  const uint32_t count = ts_node_child_count(node);
  for (uint32_t i = 0; i < count; ++i) {
    const TSNode child = ts_node_child(node, i);
    const char* type = ts_node_type(child);
    if ((type == nullptr) || (std::string_view{type} != "base_class_clause")) continue;
    const auto from = static_cast<size_t>(ts_node_start_byte(child));
    const auto to = static_cast<size_t>(ts_node_end_byte(child));
    if ((to <= from) || (to > contents.size())) return {};
    return std::string{contents.substr(from, to - from)};
  }
  return {};
}

std::string_view WithoutTemplateArgs(std::string_view text) {
  const size_t open = text.find('<');
  if ((open == std::string_view::npos) || (open == 0)) return text;
  if (text.substr(0, open).ends_with("operator")) return text;
  return text.substr(0, open);
}

std::string Squashed(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) out += ((c == '\n') || (c == '\r') || (c == '\t')) ? ' ' : c;
  std::string tidy;
  bool space = false;
  for (const char c : out) {
    if (c == ' ') {
      space = true;
      continue;
    }
    if (space && !tidy.empty()) tidy += ' ';
    space = false;
    tidy += c;
  }
  return tidy;
}

Scope* Owner(std::vector<Scope>& scopes, Index line) {
  Scope* best = nullptr;
  for (Scope& scope : scopes) {
    if ((line < scope.first) || (line > scope.last)) continue;
    if ((best == nullptr) || ((scope.last - scope.first) < (best->last - best->first))) {
      best = &scope;
    }
  }
  return best;
}

void AppendNamed(std::string& out, std::string_view label, const std::vector<Named>& items) {
  if (items.empty()) return;
  out += label;
  for (const Named& item : items) {
    out += ' ';
    out += item.name;
    out += '@';
    out += std::to_string(item.line);
  }
  out += '\n';
}

// One budget per file, covering the parse and the match over the tree it
// produced -- read once, so however the cost divides between the two, no file
// can cost more than this. Two seconds because `--overview` is a batch tool a
// script waits on rather than a keystroke, so it can afford four times what a
// text-object lookup gets, and it is still a bound: an unbudgeted query over a
// generated file blocks the script for as long as the file wants.
constexpr auto kOverviewBudget = std::chrono::milliseconds{2000};

// What a section says when it is not the whole file. Its own key, so the marker
// is as parseable as every other line the overview emits, and on stdout rather
// than in `error` -- a file that came back short is not a run that failed, and
// the exit code is what tells a script whether the tool worked. Mirrors the
// symbol scan, whose incompleteness likewise reaches the user without turning
// into a non-zero exit.
void MarkPartial(std::string& out, std::string_view reason) {
  out += "partial: ";
  out += reason;
  out += '\n';
}

bool OverviewOne(const fs::path& path, const std::unordered_set<std::string>& filter,
                 std::string& out, std::string& error) {
  const std::string_view language = LanguageForPath(path);
  if (language.empty()) return true;
  if (FindRuntimeFile(fs::path{"queries"} / language / "functions.scm").empty()) {
    return true;
  }

  std::string compile_error;
  const std::shared_ptr<CompiledQuery> compiled =
      CompileQuery(language, kOverviewQueries, compile_error);
  if (compiled == nullptr) {
    error = compile_error;
    return false;
  }

  std::error_code read_ec;
  const std::string bytes = ReadWholeFile(path, read_ec);
  if (read_ec) {
    error = "cannot read " + path.string() + ": " + read_ec.message();
    return false;
  }
  if (bytes.empty()) return true;
  // A file tree-sitter cannot address at all. Out of reach in practice -- the
  // read above has already allocated the 4 GiB -- but the truncating cast it
  // replaces would have summarised an arbitrary prefix and called it the file.
  uint32_t scan_end = 0;
  if (!TreeSitterByteRange(bytes.size(), scan_end)) {
    out += "file: " + path.string() + "\n";
    MarkPartial(out, path.filename().string() + " is larger than 4 GiB -- not read");
    return true;
  }
  const std::string_view contents{bytes};

  // One clock reading for both halves below.
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + kOverviewBudget;

  ParsedBuffer parsed;
  {
    const std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser{ts_parser_new(),
                                                                       ts_parser_delete};
    parsed = ParseBuffer(parser.get(), *compiled, language, contents, kOverviewBudget);
  }
  if (!parsed.grammar_error.empty()) {
    error = std::move(parsed.grammar_error);
    return false;
  }
  if (!parsed) {
    // A parse that gave up used to leave no section at all, which reads exactly
    // like a file with nothing in it worth summarising.
    if (parsed.timed_out) {
      out += "file: " + path.string() + "\n";
      MarkPartial(out, path.filename().string() + " exceeded the " +
                           std::to_string(kOverviewBudget.count()) +
                           "ms parse budget -- nothing was read from it");
    }
    return true;
  }

  // Budgeted with what the parse left, and capped, exactly as symbols.cpp's
  // scan is: a tree that parsed inside its budget says nothing about what
  // matching over it costs, and a cursor at tree-sitter's default match limit
  // allocates a capture list per partial match the file starts. `deadline` and
  // `options` outlive the exec deliberately -- the cursor keeps a pointer to
  // the options and reads the callback out of them on every next_match below.
  Deadline query_deadline{deadline, nullptr};
  TSQueryCursorOptions query_options{};
  query_options.payload = &query_deadline;
  query_options.progress_callback = StopQueryAtDeadline;

  TSQueryCursor* cursor = ts_query_cursor_new();
  ts_query_cursor_set_match_limit(cursor, kMaxQueryMatchStates);
  ts_query_cursor_set_byte_range(cursor, 0, scan_end);
  ts_query_cursor_exec_with_options(cursor, QueryOf(*compiled),
                                    ts_tree_root_node(parsed.tree.get()), &query_options);

  std::vector<std::string> includes;
  std::vector<Scope> functions;
  std::vector<Scope> classes;
  std::vector<Named> calls;
  std::vector<Named> members;
  std::vector<Named> methods;

  const std::span<const std::string> names = CaptureNamesOf(*compiled);
  TSQueryMatch match;
  while (ts_query_cursor_next_match(cursor, &match)) {
    if (!PredicatesHold(*compiled, match, NodeTextIn, &contents)) continue;

    std::optional<Named> def_name;
    std::optional<std::pair<Index, Index>> def_span;
    std::optional<TSNode> def_node;
    bool is_function = false;
    bool is_class = false;

    for (uint16_t i = 0; i < match.capture_count; ++i) {
      const TSNode node = match.captures[i].node;
      const uint32_t index = match.captures[i].index;
      if (index >= names.size()) continue;
      const std::string& capture = names[index];
      std::string scratch;
      const std::string_view text = NodeTextIn(&contents, node, scratch);
      const Index start = static_cast<Index>(ts_node_start_point(node).row) + 1;
      const Index end = static_cast<Index>(ts_node_end_point(node).row) + 1;

      if ((capture == "include") || (capture == "import")) {
        std::string_view path_text = text;
        if ((path_text.size() >= 2) && path_text.starts_with('"') && path_text.ends_with('"')) {
          path_text = path_text.substr(1, path_text.size() - 2);
        }
        std::string value = Squashed(path_text);
        if (!value.empty() && (std::ranges::find(includes, value) == includes.end())) {
          includes.push_back(std::move(value));
        }
      } else if (capture == "function.name") {
        def_name = Named{Squashed(text), start};
        is_function = true;
      } else if (capture == "function.def") {
        def_span = std::pair{start, end};
        is_function = true;
      } else if (capture == "class.name") {
        def_name = Named{Squashed(text), start};
        is_class = true;
      } else if (capture == "class.def") {
        def_span = std::pair{start, end};
        def_node = node;
        is_class = true;
      } else if (capture == "call") {
        calls.push_back(Named{Squashed(WithoutTemplateArgs(text)), start});
      } else if (capture == "member") {
        members.push_back(Named{Squashed(text), start});
      } else if (capture == "method") {
        methods.push_back(Named{Squashed(text), start});
      }
    }

    if (def_name && def_span && !def_name->name.empty()) {
      Scope scope;
      scope.name = std::move(def_name->name);
      scope.line = def_name->line;
      scope.first = def_span->first;
      scope.last = def_span->second;
      if (is_class) {
        if (def_node) scope.bases = BaseClauseOf(*def_node, contents);
        classes.push_back(std::move(scope));
      } else if (is_function) {
        functions.push_back(std::move(scope));
      }
    }
  }
  // Read before the delete: the flag belongs to the cursor. Both ways the
  // section can be short of what the file holds, and the marker goes out with
  // whatever was collected rather than in place of it.
  std::string short_of;
  if (query_deadline.expired) {
    short_of = path.filename().string() + " exceeded the " +
               std::to_string(kOverviewBudget.count()) +
               "ms budget -- some of its contents are missing";
  } else if (ts_query_cursor_did_exceed_match_limit(cursor)) {
    short_of = path.filename().string() + ": too many query matches -- some of its contents are "
                                          "missing";
  }
  ts_query_cursor_delete(cursor);

  for (const Named& call : calls) {
    if (filter.contains(call.name)) continue;
    if (Scope* owner = Owner(functions, call.line); owner != nullptr) owner->calls.push_back(call);
  }
  for (const Named& member : members) {
    if (Scope* owner = Owner(classes, member.line); owner != nullptr) {
      owner->members.push_back(member);
    }
  }
  for (const Named& method : methods) {
    if (Scope* owner = Owner(classes, method.line); owner != nullptr) {
      owner->methods.push_back(method);
    }
  }

  for (Scope& scope : classes) {
    SortAndDedupe(scope.members);
    SortAndDedupe(scope.methods);
  }
  for (Scope& scope : functions) SortAndDedupe(scope.calls);
  const auto by_line = [](const Scope& a, const Scope& b) { return a.line < b.line; };
  std::ranges::stable_sort(functions, by_line);
  std::ranges::stable_sort(classes, by_line);

  out += "file: " + path.string() + "\n";
  if (!short_of.empty()) MarkPartial(out, short_of);
  if (!includes.empty()) {
    out += "includes:";
    for (const std::string& include : includes) {
      out += ' ';
      out += include;
    }
    out += '\n';
  }
  if (!classes.empty()) {
    out += "classes:\n";
    for (const Scope& scope : classes) {
      out += "  " + scope.name + "@" + std::to_string(scope.line) + ":\n";
      std::string body;
      if (!scope.bases.empty()) body += "    bases: " + scope.bases + "\n";
      AppendNamed(body, "    members:", scope.members);
      AppendNamed(body, "    methods:", scope.methods);
      out += body;
    }
  }
  if (!functions.empty()) {
    out += "functions:\n";
    for (const Scope& scope : functions) {
      out += "  " + scope.name + "@" + std::to_string(scope.line) + ":";
      for (const Named& call : scope.calls) {
        out += ' ';
        out += call.name;
        out += '@';
        out += std::to_string(call.line);
      }
      out += '\n';
    }
  }
  return true;
}

}

std::generator<std::string_view> OverviewSections(std::span<const std::string> paths,
                                                  std::span<const std::string> filter,
                                                  std::string& error) {
  error.clear();
  const std::unordered_set<std::string> excluded{filter.begin(), filter.end()};
  std::string section;
  for (const std::string& path : paths) {
    section.clear();
    std::string one_error;
    if (!OverviewOne(fs::path{path}, excluded, section, one_error) && error.empty()) {
      error = std::move(one_error);
    }
    if (!section.empty()) co_yield section;
  }
}

bool OverviewOf(std::span<const std::string> paths, std::span<const std::string> filter,
                std::string& out, std::string& error) {
  out.clear();
  for (const std::string_view section : OverviewSections(paths, filter, error)) out += section;
  return error.empty();
}

}
