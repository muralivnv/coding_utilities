#include "navigate.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <new>
#include <span>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "commands.h"
#include "mmap_stream.h"
#include "operation.h"
#include "project.h"
#include "query.h"
#include "regex.h"
#include "shell.h"
#include "subprocess.h"
#include "thread_pool.h"
#include "unicode.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kDefaultFileFilter =
    R"bash(find . -type f -printf '%P\n' | gai -e '.*(build|.git)')bash";

constexpr std::string_view kTooey = R"bash(
    tooey --ansi --print-query --query %{user_query} \
    --preview-dir right --preview-size 55 \
    --query-process-command 'gai --no-color -f "(?i)"{{@QUERY@}}' \
    --width 100 --height 50 --position centered \
    --preview-command '
      payload=$(printf %s {{@SELECTION@}} | sed "s/.*\t//")
      f=$payload; l=1
      case $payload in (*:*)
        f=${payload%%:*}
        l=${payload#*:}; l=${l%%:*};;
      esac
      [ -n "$l" ] || l=1
      %{koi} --render-mode "$f" --no-syntax --line-numbers --highlight-line "$l" \
          --line-range "$(( l > 15 ? l - 15 : 1 )):$(( l + 25 ))"')bash";

enum class Candidates {
  kRankedFiles,
  kFromFileList,
  kSelfContained,
};

struct Pipeline {
  std::string_view name;
  Candidates candidates;
  std::string_view produce;
  std::string_view flags;
};

constexpr std::array<Pipeline, 7> kPipelines{{
    {"files", Candidates::kRankedFiles, "", R"bash( --prompt '[ Files ] ❯ ')bash"},

    {"content", Candidates::kFromFileList,
     R"bash(xargs gai --no-color -f '\w' -v -d : --files |
  awk -F: -v w=300 '{
    c = ""; for (i = 3; i <= NF; i++) { if (i > 3) c = c ":"; c = c $i }
    gsub(/^[[:space:]]+/, "", c); gsub(/[[:space:]]+$/, "", c)
    n = split($1, a, "/")
    printf "%s\t\033[38;5;246m%s:%s\033[0m%*s\t%s:%s\n", c, a[n], $2, w, "", $1, $2
  }' |
  )bash",
     R"bash( --prompt '[ Search ] ❯ ')bash"},

    {"symbols", Candidates::kFromFileList,
     R"bash(%{koi} --symbol-mode --picker-rows --definitions \
      --hot-first --from %{buffer_name} --files - |
  )bash",
     R"bash( --prompt '[ Symbols ] ❯ ')bash"},

    {"buffer-symbols", Candidates::kSelfContained,
     R"bash(%{koi} --symbol-mode --picker-rows --definitions --files %{buffer_name} |
  )bash",
     R"bash( --prompt '[ Symbols ] ❯ ')bash"},

    {"buffers", Candidates::kSelfContained, "", R"bash( --prompt '[ Buffers ] ❯ ')bash"},
    {"definition", Candidates::kSelfContained, "", R"bash( --prompt '[ Defs ] ❯ ')bash"},
    {"references", Candidates::kSelfContained, "", R"bash( --prompt '[ Refs ] ❯ ')bash"},
}};

std::generator<std::string_view> CapturedLines(Editor& ed, std::string command) {
  if (command.empty()) co_return;
  const common::CmdResult result =
      common::RunCmdWithCapture(command, common::CaptureMode::kPipe, common::CaptureMode::kDevNull);
  if (!result.output) {
    ed.status.Fail("could not run: " + command);
    co_return;
  }
  const std::string_view text{result.output->buffer, result.output->size};
  size_t at = 0;
  while (at < text.size()) {
    const size_t eol = std::min(text.find('\n', at), text.size());
    std::string_view line = text.substr(at, eol - at);
    while (!line.empty() && (line.back() == '\r')) line.remove_suffix(1);
    if (!line.empty()) co_yield line;
    at = eol + 1;
  }
}

std::vector<std::string> CaptureLines(Editor& ed, std::string command) {
  std::vector<std::string> out;
  for (const std::string_view line : CapturedLines(ed, std::move(command))) out.emplace_back(line);
  return out;
}

std::string SelfExe() {
  std::error_code ec;
  const fs::path self = fs::read_symlink("/proc/self/exe", ec);
  return ec ? std::string{} : ShellQuote(self.string());
}

bool HasScreen(Editor& ed) {
  if (ed.suspend_terminal != nullptr) return true;
  ed.status.Warn("no terminal to hand to a picker");
  return false;
}

std::string TargetSpec(std::string_view path, Index line, Index column) {
  std::string spec{path};
  if (line > 0) {
    spec += ':';
    spec += std::to_string(line);
    if (column > 0) {
      spec += ':';
      spec += std::to_string(column);
    }
  }
  return spec;
}

// The key a path is stored under in the project database -- the same rule
// ProjectStore applies to everything it is handed (ProjectKey, project.cpp).
// It is needed out here only to read *back*: matching a path against something
// the store already returned means spelling it the store's way.
std::string ProjectPath(const fs::path& path) {
  if (path.empty()) return {};
  std::error_code ec;
  const fs::path absolute = fs::weakly_canonical(fs::absolute(path, ec), ec);
  if (ec) return path.string();
  const fs::path root = ProjectRoot();
  if (root.empty()) return absolute.string();
  const fs::path relative = fs::relative(absolute, root, ec);
  if (ec || relative.empty() || relative.string().starts_with("..")) return absolute.string();
  return relative.string();
}

// What the store gets handed: the path this session opened, untouched.
//
// It used to be ProjectPath(ed.doc.file) -- the editor keying the path itself,
// because nothing else would. Now the store keys every path that reaches it,
// and keying here as well would key an already-keyed path: a relative spelling
// is resolved against the current directory, so re-keying "sub/file.cpp" from
// inside "sub" lands on "sub/sub/file.cpp". `ed.doc.file` is whatever opened
// this buffer, which is by construction a path valid from koi's current
// directory -- exactly what the store's rule expects.
std::string CurrentFile(const Editor& ed) { return ed.doc.file.string(); }

// The files worth scanning first, spelled the way a scan can open them.
//
// The list comes out of the store, so it is keyed against the project root;
// the paths it is merged with come from the file filter, which runs here. The
// two are the same strings when koi was started at the root and different ones
// when it was not -- and "different" meant both a scan of paths that do not
// exist from here and, for whatever did resolve, a second copy of every hot
// symbol under a second spelling.
std::vector<std::string> HotFilesFromHere(ProjectStore& store, std::string_view from) {
  std::vector<std::string> hot = store.HotFiles(kDefaultHotFileLimit, from);
  const fs::path root = ProjectRoot();
  for (std::string& path : hot) path = ResolveStorePath(root, path);
  return hot;
}

void OpenAt(Editor& ed, std::string_view path, Index line, Index column) {
  if (path.empty()) return;
  if (!OpenTarget(ed, TargetSpec(path, line, column))) return;
  if (ed.project) {
    ed.project->RecordVisit(CurrentFile(ed), std::max<Index>(1, line), std::max<Index>(0, column));
  }
}

}

void WriteLastPicker(std::string_view name, std::string_view query) {
  const fs::path path = LastPickerStatePath();
  if (path.empty()) return;
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  if (!out) return;
  out << name << '\t' << query;
}

bool ReadLastPicker(std::string& name, std::string& query) {
  const fs::path path = LastPickerStatePath();
  if (path.empty()) return false;
  std::ifstream in{path, std::ios::binary};
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  const size_t first = text.find('\t');
  name = text.substr(0, first);
  query.clear();
  if (first != std::string::npos) {
    query = text.substr(first + 1, text.find('\t', first + 1) - first - 1);
  }
  return !name.empty();
}

std::string RankedFileRows(Editor& ed) {
  struct Row {
    std::string path;
    Index line{1};
    Index column{1};
    Index rank{-1};
  };

  std::unordered_map<std::string, Index> frecent;
  std::vector<FileVisit> visits;
  if (ed.project) {
    // Every row, not a page of them: this is not a list of files to show, it is
    // the rank each of the file filter's paths gets. A cap here would not
    // shorten the picker, it would drop everything past it out of frecency
    // order and back into the order find(1) happened to walk the tree.
    visits = ed.project->FrecentFiles(0);
    for (Index i = 0; i < std::ssize(visits); ++i) frecent.emplace(visits[i].path, i);
  }

  std::vector<Row> rows;
  std::unordered_set<std::string> seen;
  for (const std::string_view path : CapturedLines(ed, FileFilterCommand(ed))) {
    if (path.empty() || !seen.emplace(path).second) continue;
    Row row{std::string{path}, 1, 1, -1};
    const auto hit = frecent.find(ProjectPath(fs::path{path}));
    if (hit != frecent.end()) {
      row.rank = hit->second;
      row.line = std::max<Index>(1, visits[static_cast<std::size_t>(hit->second)].line);
      row.column = std::max<Index>(1, visits[static_cast<std::size_t>(hit->second)].column);
    }
    rows.push_back(std::move(row));
  }

  std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    const bool a_seen = (a.rank >= 0);
    const bool b_seen = (b.rank >= 0);
    if (a_seen != b_seen) return a_seen;
    return a_seen ? (a.rank < b.rank) : false;
  });

  std::string out;
  for (const Row& row : rows) {
    const Symbol one{.path = row.path, .line = row.line, .column = row.column, .name = {}};
    // Empty when the path holds a tab or a newline: the row could not come back
    // naming the same file, so it is left out rather than made to point
    // somewhere else.
    const std::string rendered = PickerRow(row.path, FormatSymbolRow(one));
    if (rendered.empty()) continue;
    out += rendered;
    out += '\n';
  }
  return out;
}

namespace {

// `note` is what the caller wants said alongside whatever the pick turns into
// -- a scan diagnostic, say. Appended only once something was opened, because
// the picker owns the screen while it runs and a person who backs out of it
// would otherwise be handed a parenthetical about a scan they abandoned.
void RunPickerNamed(Editor& ed, std::string_view name, std::string_view query,
                    std::string staged = {}, std::string_view note = {}) {
  if (!HasScreen(ed)) return;
  const std::string command = ExpandPickerCommand(ed, PickerCommand(ed, name), query);

  WriteLastPicker(name, query);

  const std::string items = staged.empty() && (name == "files") ? RankedFileRows(ed)
                                                                : std::move(staged);
  std::string chosen_query;
  std::vector<std::string> rows;
  if (!RunPickerLines(ed, command, items, chosen_query, rows)) return;
  if (rows.empty()) return;

  WriteLastPicker(name, chosen_query);

  std::vector<Symbol> chosen;
  for (const std::string& row : rows) {
    Symbol parsed;
    if (ParseSymbolRow(RowPayload(row), parsed)) chosen.push_back(std::move(parsed));
  }
  if (chosen.empty()) {
    ed.status = "the picker returned nothing koi could open";
    return;
  }

  if (ed.project) {
    const std::string from = CurrentFile(ed);
    for (const Symbol& one : chosen) {
      if (one.name.empty()) continue;
      ed.project->RecordSymbolVisit(one.name, one.path, one.line);
      ed.project->RecordCoVisit(from, one.path);
    }
  }

  const Symbol& first = chosen.front();
  OpenAt(ed, first.path, first.line, first.column);
  if (!note.empty()) ed.status += note;
  if (chosen.size() > 1) {
    ed.status += "  (" + std::to_string(chosen.size() - 1) + " more not opened)";
  }
}

std::string SelectedWord(const Editor& ed) {
  const Selection& primary = ed.doc.selections.Primary();
  if (primary.IsEmpty()) return {};
  const std::string text = ReadDocRange(ed.doc.table, primary.Range());
  return std::string{Trim(text, " \t\r\n")};
}

void OpenPin(Editor& ed, const Pin& pin, std::string_view what) {
  if (pin.path.empty()) {
    ed.status = std::string{what} + " is not set";
    return;
  }
  // The pin came out of the store, so it is spelled against the project root;
  // opening resolves against the current directory (ResolveStorePath).
  OpenAt(ed, ResolveStorePath(pin.path), pin.line, pin.column);
}

bool RequireProject(Editor& ed) {
  if (ed.project) return true;
  ed.status.Warn("no project database");
  return false;
}

}

std::string FileFilterCommand(const Editor& ed) {
  std::string_view filter = TrimBack(ed.settings.file_filter, " \t\r\n");
  filter = TrimBack(TrimBack(filter, "|"), " \t");
  return filter.empty() ? std::string{kDefaultFileFilter} : std::string{filter};
}

void AppendFragment(std::string& command, std::string_view piece) {
  piece = Trim(piece, " \t\n");
  if (piece.empty()) return;
  if (!command.empty()) command += ' ';
  command += piece;
}

std::vector<std::string_view> PickerNames() {
  std::vector<std::string_view> out;
  out.reserve(kPipelines.size());
  for (const Pipeline& one : kPipelines) out.push_back(one.name);
  return out;
}

std::string PickerCommand(const Editor& ed, std::string_view name) {
  for (const Pipeline& one : kPipelines) {
    if (one.name != name) continue;
    std::string command;
    if (one.candidates == Candidates::kFromFileList) {
      AppendFragment(command, FileFilterCommand(ed));
      command += " |";
    }
    AppendFragment(command, one.produce);
    AppendFragment(command, kTooey);
    AppendFragment(command, one.flags);
    return command;
  }
  return {};
}

std::string ExpandPickerCommand(const Editor& ed, std::string_view command,
                                std::string_view query) {
  return ExpandVariables(command, ed, query);
}

std::string BufferPickerItems(const Editor& ed) {
  std::string items;
  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    const Document& doc = BufferAt(ed, i);
    const std::string index = "#" + std::to_string(i);
    std::string item;
    if (!HasDiskFile(doc) && !IsExcerptView(doc)) {
      item = PickerRow(index + " [scratch]", index);
    } else {
      const Index line = LineAt(doc.table, CursorOf(doc.table, doc.selections.Primary()));
      const Index column =
          CursorOf(doc.table, doc.selections.Primary()) - LineStart(doc.table, line);
      const std::string name = IsExcerptView(doc) ? doc.view_name : DisplayPath(doc.file);
      const std::string where =
          name + ":" + std::to_string(line + 1) + ":" + std::to_string(column + 1);
      // A file row is its own payload, so a tab in the name would have
      // RowPayload hand back a suffix of it and the pick would open a file that
      // is not this buffer's. Nothing can encode that here; leave the row out.
      if (where.find_first_of("\t\n\r") != std::string::npos) continue;
      item = IsExcerptView(doc) ? PickerRow(where, index) : where;
    }
    if (item.empty()) continue;
    items += item;
    items += '\n';
  }
  return items;
}

void ChooseBufferRow(Editor& ed, std::string_view payload) {
  if (payload.empty()) return;
  const std::string_view digits = payload.substr(1);
  const bool indexed = (payload.front() == '#') && !digits.empty() &&
                       std::ranges::all_of(digits, [](char c) { return (c >= '0') && (c <= '9'); });
  if (indexed) {
    std::size_t at = 0;
    for (const char c : digits) {
      at = (at * 10) + static_cast<std::size_t>(c - '0');
      if (at >= BufferCount(ed)) return;
    }
    RecordJump(ed);
    SwitchToBuffer(ed, at);
    ed.mode = Mode::kNormal;
    ApplyModeInvariants(ed);
    ed.status.clear();
    return;
  }
  OpenTarget(ed, payload);
}

void BufferPicker(Editor& ed, std::string_view query) {

  if (BufferCount(ed) <= 1) {
    ed.status.Warn("only one buffer open");
    return;
  }
  if (!HasScreen(ed)) return;
  const std::string command = ExpandPickerCommand(ed, PickerCommand(ed, "buffers"), query);

  // Recorded like every other picker, so reopening the last one can land here.
  // This does not go through RunPickerNamed -- it opens a buffer rather than a
  // file at a position -- which is how it came to be the one picker that never
  // wrote its name.
  WriteLastPicker("buffers", query);

  std::string chosen_query;
  std::vector<std::string> rows;
  if (!RunPickerLines(ed, command, BufferPickerItems(ed), chosen_query, rows)) return;
  if (rows.empty()) return;

  WriteLastPicker("buffers", chosen_query);
  ChooseBufferRow(ed, RowPayload(rows.front()));
}

void FilePicker(Editor& ed, std::string_view query) { RunPickerNamed(ed, "files", query); }

void ContentPicker(Editor& ed, std::string_view query) { RunPickerNamed(ed, "content", query); }

void SymbolPicker(Editor& ed, std::string_view query) { RunPickerNamed(ed, "symbols", query); }

void BufferSymbolPicker(Editor& ed, std::string_view query) {
  if (!HasDiskFile(ed.doc)) {
    ed.status.Warn("no file -- :w <path> first");
    return;
  }
  RunPickerNamed(ed, "buffer-symbols", query);
}

namespace {

void SayWhileWorking(Editor& ed, std::string message) {
  ed.status = std::move(message);
  if (ed.draw_now != nullptr) ed.draw_now(ed);
}

// `error` comes back holding the first complaint either scan had -- a grammar
// that would not load, a file that blew the parse budget, a path that would not
// read. It is the caller's to say out loud, not this function's: an empty
// result set is announced by the caller, and a status line written here would
// be overwritten by that announcement a moment later.
std::vector<Symbol> FindSymbols(Editor& ed, std::string_view word, SymbolKind kind,
                                std::string& error) {
  SayWhileWorking(ed, "scanning the project for " + std::string{word} + "…");
  std::vector<std::string> paths;
  for (const std::string_view path : CapturedLines(ed, FileFilterCommand(ed))) {
    paths.emplace_back(path);
  }

  std::vector<Symbol> found;
  std::unordered_set<std::string> seen;
  const auto take = [&](std::vector<Symbol>&& rows) {
    for (Symbol& one : rows) {
      if (!seen.insert(FormatSymbolRow(one)).second) continue;
      found.push_back(std::move(one));
    }
  };

  const std::string from = CurrentFile(ed);
  if (ed.project != nullptr) {
    std::vector<std::string> hot = HotFilesFromHere(*ed.project, from);
    if (!hot.empty()) {
      std::vector<Symbol> head = CollectSymbols(hot, kind, error, word);
      head.resize(std::min(ed.project->RankSymbols(head, from), head.size()));
      take(std::move(head));
    }
  }
  take(CollectSymbols(paths, kind, error, word));
  return found;
}

// `word_override` is what reopening the last picker passes back in: the stored
// query is the word that was looked up, and re-deriving it from the selection
// instead would reopen the picker for whatever happens to be selected now, or
// refuse outright when nothing is.
void LookUpSymbol(Editor& ed, std::string_view name, SymbolKind kind, std::string_view what,
                  std::string_view word_override = {}) {
  const std::string word =
      word_override.empty() ? SelectedWord(ed) : std::string{word_override};
  if (word.empty()) {
    ed.status.Warn("nothing selected");
    return;
  }
  if (!HasScreen(ed)) return;

  // Said in every outcome the scan has, the way the job path says it
  // (PresentCommandJob's `note`): "no definition for X" with a grammar quietly
  // unloaded behind it reads as proof there is no definition, and a *partial*
  // result set with symbols missing from a whole language reads as a complete
  // one. The picker path carries it through to whatever the pick opens.
  std::string error;
  const std::vector<Symbol> found = FindSymbols(ed, word, kind, error);
  const std::string note = error.empty() ? std::string{} : (" (" + error + ")");
  if (found.empty()) {
    ed.status.Warn("no " + std::string{what} + " for " + word + note);
    return;
  }

  if (found.size() == 1) {
    const Symbol& one = found.front();
    if (ed.project != nullptr) {
      if (!one.name.empty()) ed.project->RecordSymbolVisit(one.name, one.path, one.line);
      ed.project->RecordCoVisit(CurrentFile(ed), one.path);
    }
    OpenAt(ed, one.path, one.line, one.column);
    if (!note.empty()) ed.status += note;
    return;
  }

  std::string staged;
  for (const Symbol& one : found) {
    const std::string row = SymbolPickerRow(one);
    if (row.empty()) continue;
    staged += row;
    staged += '\n';
  }
  RunPickerNamed(ed, name, word, std::move(staged), note);
}

}

void GotoDefinition(Editor& ed) {
  LookUpSymbol(ed, "definition", SymbolKind::kDefinitions, "definition");
}

void ShowReferences(Editor& ed) {
  LookUpSymbol(ed, "references", SymbolKind::kBoth, "reference");
}

namespace {

constexpr std::string_view kExcerptMarker = "▸ ";

bool IsWordBounded(std::string_view text, std::size_t at, std::size_t length) {
  const auto is_word = [](char c) {
    return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
           ((c >= '0') && (c <= '9')) || (c == '_');
  };
  const bool left = (at == 0) || !is_word(text[at - 1]);
  const std::size_t after = at + length;
  const bool right = (after >= text.size()) || !is_word(text[after]);
  return left && right;
}

void RebuildHeaderIndex(ExcerptView& view) {
  view.header_index.clear();
  std::size_t total = 0;
  for (const ExcerptBlock& block : view.blocks) total += 1 + block.prior_headers.size();
  for (const DroppedExcerpt& gone : view.dropped) total += 1 + gone.block.prior_headers.size();
  view.header_index.reserve(total);
  const auto take = [&view](const ExcerptBlock& block) {
    view.header_index.push_back(block.header);
    view.header_index.insert(view.header_index.end(), block.prior_headers.begin(),
                             block.prior_headers.end());
  };
  for (const ExcerptBlock& block : view.blocks) take(block);
  for (const DroppedExcerpt& gone : view.dropped) take(gone.block);
  std::ranges::sort(view.header_index);
}

std::string DeriveHeader(const ExcerptBlock& block, Index first, Index last) {
  char span[48];
  const std::to_chars_result a = std::to_chars(span, span + 20, first);
  char* p = (a.ec == std::errc{}) ? a.ptr : span;
  *p++ = ',';
  const std::to_chars_result b = std::to_chars(p, p + 20, last);
  if (b.ec == std::errc{}) p = b.ptr;
  std::string out;
  out.reserve(block.header_prefix.size() + block.path.size() + 1 +
              static_cast<std::size_t>(p - span) + block.header_note.size());
  out += block.header_prefix;
  out += block.path;
  out += ':';
  out.append(span, p);
  out += block.header_note;
  return out;
}

std::string BuildExcerpts(const std::vector<ExcerptRef>& refs, std::string_view title,
                          Index context, std::string_view marker, ExcerptView& out) {
  out.blocks.clear();
  out.dropped.clear();
  out.header_index.clear();
  out.capture_names.clear();
  out.stamps.clear();
  out.capture_names.emplace_back(kExcerptHeaderScope);
  out.capture_names.emplace_back(kExcerptMatchScope);

  struct Mark {
    Index line{1};
    Index col{0};
    const std::string* msg{nullptr};
  };
  std::vector<std::pair<std::string, std::vector<Mark>>> by_path;
  std::unordered_map<std::string, std::size_t> seen_path;
  for (const ExcerptRef& one : refs) {
    const auto [at, fresh] = seen_path.try_emplace(one.path, by_path.size());
    if (fresh) by_path.emplace_back(one.path, std::vector<Mark>{});
    by_path[at->second].second.push_back(Mark{std::max<Index>(1, one.line), one.col, &one.msg});
  }
  const bool ordered_by_refs =
      (out.kind == ExcerptView::Kind::kPins) || (out.kind == ExcerptView::Kind::kTrail);
  const bool anchors_are_the_point = ordered_by_refs;
  out.anchor_index.clear();
  if (!ordered_by_refs) {
    std::ranges::sort(by_path, {}, [](const auto& one) -> const std::string& { return one.first; });
  }

  std::string text;
  Index line_no = 0;
  const auto append = [&](std::string_view chunk) {
    text += chunk;
    line_no += std::ranges::count(chunk, '\n');
  };

  append(title);
  append("\n");

  for (auto& [path, lines] : by_path) {
    std::ranges::sort(lines, {}, &Mark::line);
    const auto extra = std::ranges::unique(lines, {}, &Mark::line);
    lines.erase(extra.begin(), extra.end());

    if (FileStamp stamp; StampFile(path, stamp)) out.stamps.push_back(std::move(stamp));

    PieceTable file;
    std::error_code read_ec;
    std::string bytes = ReadWholeFile(path, read_ec);
    // A file that could not be read used to arrive here as an empty string and
    // build exactly the block an empty file builds: bodyless, unremarkable, and
    // silent about the fact that the bytes exist and koi simply failed to get
    // them. The block is still bodyless -- there is no body to show -- but it
    // says so and says why, so a transient read failure is legible in the view
    // instead of looking like a file that has nothing in it.
    if (read_ec) bytes.clear();
    // The same gate every other way text enters a document goes through
    // (editor.cpp's open, koi.cpp's stdin, shell.cpp's command output). Without
    // it a latin-1 or binary file that a scan happened to match puts ill-formed
    // UTF-8 into a live document, and every grapheme query over it -- the
    // resync in SegmentAround above all -- is undefined by its own contract.
    // Per file, so one such file degrades to a bodyless block instead of
    // poisoning the whole view.
    const bool decodable = IsWellFormedUtf8(bytes);
    if (!decodable) bytes.clear();
    ResetToOriginal(file, std::move(bytes));
    const Index file_lines = LineCount(file);
    const bool readable = DocLength(file) > 0;
    const bool phantom =
        readable && (ReadDocRange(file, Interval(DocLength(file) - 1, DocLength(file))) == "\n");
    const Index cap = std::max<Index>(1, phantom ? (file_lines - 1) : file_lines);

    for (std::size_t i = 0; i < lines.size();) {
      const Index anchor = lines[i].line;
      const Index anchor_col = lines[i].col;
      Index last = anchor + context;
      std::string note;
      const auto add_note = [&note](const std::string* msg) {
        if ((msg == nullptr) || msg->empty() || (note.find(*msg) != std::string::npos)) return;
        note += note.empty() ? "  " : " · ";
        note += *msg;
      };
      add_note(lines[i].msg);
      std::size_t j = i + 1;
      while ((j < lines.size()) && ((lines[j].line - context) <= (last + 1))) {
        last = lines[j].line + context;
        add_note(lines[j].msg);
        ++j;
      }
      i = j;
      if (!decodable) {
        note += note.empty() ? "  " : " · ";
        note += "not valid UTF-8 -- no body shown";
      }
      if (read_ec) {
        note += note.empty() ? "  " : " · ";
        note += "cannot read -- " + read_ec.message();
      }

      const Index anchor_in = readable ? std::min(anchor, cap) : anchor;
      const Index first = readable ? std::max<Index>(1, anchor_in - context) : anchor;
      const Index shown_last = readable ? std::min(last, cap) : anchor;

      append("\n");
      ExcerptBlock block;
      block.header_line = line_no;
      block.path = path;
      block.line = anchor_in;
      block.col = anchor_col;
      block.first = first;
      block.last = shown_last;
      block.header_prefix = std::string{marker};
      block.header_note = std::move(note);
      block.header = DeriveHeader(block, first, shown_last);
      append(block.header);
      append("\n");

      const Index begin = readable ? LineStart(file, first - 1) : 0;
      const Index end = !readable ? 0
                        : (std::min(last, file_lines) < file_lines)
                            ? LineStart(file, std::min(last, file_lines))
                            : DocLength(file);
      if (end <= begin) {
        block.no_body = true;
        out.blocks.push_back(std::move(block));
        continue;
      }

      std::string body = ReadDocRange(file, Interval(begin, end));
      if (body.empty() || (body.back() != '\n')) {
        body += '\n';
        block.synthesized_newline = true;
      }
      if (anchors_are_the_point) {
        for (const Mark& mark : lines) {
          if ((mark.line < block.first) || (mark.line > block.last)) continue;
          const Interval span = LineContentRange(file, mark.line - 1);
          if (span.empty()) continue;
          std::string marked = ReadDocRange(file, span);
          if (!marked.empty()) out.anchor_index.push_back(std::move(marked));
        }
      }
      append(body);
      block.original = std::move(body);
      out.blocks.push_back(std::move(block));
    }
  }

  RebuildHeaderIndex(out);
  std::ranges::sort(out.anchor_index);
  const auto extra = std::ranges::unique(out.anchor_index);
  out.anchor_index.erase(extra.begin(), extra.end());
  return text;
}

ErrorCtx SwapViewText(Document& doc, std::string_view text) {
  BreakUndoCoalescing(doc.table);
  CursorState before;
  before.primary = static_cast<std::uint32_t>(doc.selections.PrimaryIndex());
  for (const Selection& s : doc.selections.Ranges()) {
    before.spans.push_back(CursorSpan{s.anchor, s.head});
  }
  const std::vector<Change> whole{Change{0, DocLength(doc.table), text}};
  if (const ErrorCtx err = Apply(doc.table, whole, std::move(before), CursorState{}); err) {
    return err;
  }
  BreakUndoCoalescing(doc.table);
  ExcerptEpochs& epochs = doc.excerpt_epochs;
  if (epochs.store.empty()) epochs.store.emplace_back();
  epochs.store.resize(epochs.active + 1);
  epochs.boundaries.resize(epochs.active);
  epochs.store[epochs.active] = std::move(doc.excerpts);
  epochs.boundaries.push_back(CurrentUndoSerial(doc.table));
  epochs.store.emplace_back();
  epochs.active = epochs.boundaries.size();
  if (constexpr std::size_t kMaxEpochs = 8; epochs.boundaries.size() > kMaxEpochs) {
    epochs.store.erase(epochs.store.begin());
    epochs.boundaries.erase(epochs.boundaries.begin());
    --epochs.active;
  }
  MarkUndoSavePoint(doc.table);
  doc.saved_undo_serial = CurrentUndoSerial(doc.table);
  return Success();
}

void ClampScrollToText(Editor& ed, std::size_t buffer, Document& doc) {
  const Index last = std::max<Index>(0, LineCount(doc.table) - 1);
  doc.view.top_line = std::clamp<Index>(doc.view.top_line, 0, last);
  for (WindowNode& node : ed.windows) {
    if (node.dead || (node.kind != WindowNode::Kind::kLeaf)) continue;
    if (node.buffer != buffer) continue;
    node.view.top_line = std::clamp<Index>(node.view.top_line, 0, last);
  }
}

// Two builds can produce byte-identical view text and still disagree about the
// model underneath it, so "the text did not change, keep the model we have" is
// only sound once the parts of the model the text does not carry are compared
// too. `synthesized_newline` is the one that bites: it records whether the last
// line of a hunk borrowed a newline the file does not have, and a hunk that
// reaches EOF renders identically either way -- "…\nbravo" gives body "bravo"
// plus a synthetic newline, "…\nbravo\n" gives body "bravo\n", and both put
// "bravo\n" in the view under the same first,last header. Keeping a stale flag
// makes ExpectedSpanBytes disagree with the disk by exactly one byte forever:
// :w refuses the hunk and tells the user to rebuild the view, and the rebuild
// it advises lands right back on this branch.
//
// Only fields that are invisible in the text are worth testing. `path`,
// `first`, `last`, `header_prefix` and `header_note` are all spelled out in the
// header line (DeriveHeader), `original` is the body as it was appended, and
// `header_line` is that header's position in the text -- equal text means equal
// values for all of them. `first`/`last` are compared anyway because they are
// what a save writes through, and the comparison is free when the header
// matched. `prior_headers` is deliberately left out: a fresh build never has
// any, and the held ones record headers the user has since renamed, which is
// exactly the state keeping the model is for.
bool SameShape(const std::vector<ExcerptBlock>& a, const std::vector<ExcerptBlock>& b) {
  return std::ranges::equal(a, b, [](const ExcerptBlock& x, const ExcerptBlock& y) {
    return (x.synthesized_newline == y.synthesized_newline) && (x.no_body == y.no_body) &&
           (x.line == y.line) && (x.col == y.col) && (x.first == y.first) && (x.last == y.last);
  });
}

bool OpenGeneratedView(Editor& ed, std::string name, std::string text, ExcerptView built,
                       Index put_on_line) {
  const std::size_t existing = FindViewBuffer(ed, name);
  bool transactional = false;
  bool keep_model = false;
  if (existing < BufferCount(ed)) {
    if (BufferAt(ed, existing).modified) {
      ed.status.Warn(name + " has unsaved edits -- :w them or :bc! the view first");
      return false;
    }
    SwitchToBuffer(ed, existing);
    if ((ReadDocRange(ed.doc.table, Interval(0, DocLength(ed.doc.table))) == text) &&
        SameShape(ed.doc.excerpts.blocks, built.blocks)) {
      keep_model = true;
      ExcerptView& held = ed.doc.excerpts;
      held.kind = built.kind;
      held.word = std::move(built.word);
      held.watched = built.watched;
      held.with_msg = built.with_msg;
      held.refs = std::move(built.refs);
      held.refs_stale = false;
      held.stamps = std::move(built.stamps);
      held.paint_line = std::move(built.paint_line);
    } else {
      if (const ErrorCtx err = SwapViewText(ed.doc, text); err) {
        ed.status.Fail("the view could not be rebuilt: " + FormatErrorCtx(err));
        return false;
      }
      transactional = true;
    }
  } else {
    Document fresh;
    fresh.tab_width = ed.doc.tab_width;
    fresh.insert_spaces = ed.doc.insert_spaces;
    fresh.view.rows = ed.doc.view.rows;
    fresh.view.columns = ed.doc.view.columns;
    fresh.view_name = std::move(name);
    ResetToOriginal(fresh.table, std::move(text));
    AddBuffer(ed, std::move(fresh));
  }
  ed.doc.read_only = false;
  if (!keep_model) ed.doc.excerpts = std::move(built);
  ed.doc.modified = false;

  if (!keep_model) {
    const Index line =
        std::clamp<Index>(put_on_line, 0, std::max<Index>(0, LineCount(ed.doc.table) - 1));
    const Index at = LineStart(ed.doc.table, line);
    ed.doc.selections.Set(Selection{at, at, -1});
    if (transactional) {
      NoteCursorsAfter(ed.doc.table, CursorState{.spans = {CursorSpan{at, at}}, .primary = 0});
    }
  }
  if (!keep_model) {
    ed.doc.view.top_line = 0;
    ed.doc.view.top_row = 0;
    ed.doc.view.left_column = 0;
  }
  ClampScrollToText(ed, ed.active, ed.doc);
  ed.mode = Mode::kNormal;
  ApplyModeInvariants(ed);
  AttachSyntax(ed);
  return true;
}

struct ParsedHunk {
  std::size_t block{0};
  Index header_line{0};
  std::string header_text;
  std::string body;
};

bool IsHeaderOf(const ExcerptBlock& block, std::string_view line) {
  return (line == block.header) || std::ranges::contains(block.prior_headers, line);
}

std::vector<std::string_view> SplitViewLines(const std::string& text) {
  std::vector<std::string_view> lines;
  for (std::size_t at = 0; at <= text.size();) {
    const std::size_t eol = text.find('\n', at);
    if (eol == std::string::npos) {
      if (at < text.size()) lines.emplace_back(text.data() + at, text.size() - at);
      break;
    }
    lines.emplace_back(text.data() + at, eol - at);
    at = eol + 1;
  }
  return lines;
}

bool ParseExcerptView(const PieceTable& table, const std::vector<ExcerptBlock>& blocks,
                      std::vector<ParsedHunk>& out, std::string& error) {
  out.clear();
  error.clear();
  const std::string text = ReadDocRange(table, Interval(0, DocLength(table)));
  const std::vector<std::string_view> lines = SplitViewLines(text);

  std::vector<Index> header_at(blocks.size(), 0);
  std::size_t line = 0;
  for (std::size_t b = 0; b < blocks.size(); ++b) {
    while ((line < lines.size()) && !IsHeaderOf(blocks[b], lines[line])) ++line;
    if (line >= lines.size()) {
      error = "header missing or edited: " + blocks[b].header +
              " -- headers are the save contract; undo the edit or rebuild the view";
      return false;
    }
    header_at[b] = static_cast<Index>(line);
    ++line;
  }

  std::vector<std::string_view> known;
  known.reserve(blocks.size() * 2);
  for (const ExcerptBlock& block : blocks) {
    known.push_back(block.header);
    for (const std::string& worn : block.prior_headers) known.push_back(worn);
  }
  std::ranges::sort(known);
  known.erase(std::ranges::unique(known).begin(), known.end());
  // `header_at` comes out of the scan above strictly increasing -- each block's
  // search starts past the previous block's header -- so this is a binary
  // search, not a linear one. It runs once per line of the view and there is
  // one entry per block, which made the check quietly O(lines x blocks).
  for (std::size_t at = 0; at < lines.size(); ++at) {
    if (std::ranges::binary_search(header_at, static_cast<Index>(at))) continue;
    if (!std::ranges::binary_search(known, lines[at])) continue;
    error = "line " + std::to_string(at + 1) +
            " reads as a hunk header -- the save cannot tell it apart from one; change it or "
            "rebuild the view";
    return false;
  }

  for (std::size_t b = 0; b < blocks.size(); ++b) {
    ParsedHunk hunk;
    hunk.block = b;
    hunk.header_line = header_at[b];
    hunk.header_text = std::string{lines[static_cast<std::size_t>(header_at[b])]};
    const std::size_t from = static_cast<std::size_t>(header_at[b]) + 1;
    std::size_t to = ((b + 1) < blocks.size()) ? static_cast<std::size_t>(header_at[b + 1])
                                               : lines.size();
    if (((b + 1) < blocks.size()) && (to > from) && lines[to - 1].empty()) --to;
    for (std::size_t i = from; i < to; ++i) {
      hunk.body.append(lines[i]);
      if ((i + 1 < to) || (b + 1 < blocks.size()) || text.ends_with('\n')) hunk.body += '\n';
    }
    out.push_back(std::move(hunk));
  }
  return true;
}

void ReadoptDroppedExcerpts(Document& doc) {
  ExcerptView& view = doc.excerpts;
  if (view.dropped.empty()) return;
  const std::string text = ReadDocRange(doc.table, Interval(0, DocLength(doc.table)));
  const std::vector<std::string_view> lines = SplitViewLines(text);

  struct Slot {
    bool resurrected{false};
    std::size_t index{0};
  };
  std::vector<Slot> order;
  std::vector<bool> found(view.dropped.size(), false);
  std::size_t next = 0;
  for (const std::string_view line : lines) {
    if ((next < view.blocks.size()) && IsHeaderOf(view.blocks[next], line)) {
      order.push_back(Slot{false, next++});
      continue;
    }
    for (std::size_t d = 0; d < view.dropped.size(); ++d) {
      if (found[d] || !IsHeaderOf(view.dropped[d].block, line)) continue;
      found[d] = true;
      order.push_back(Slot{true, d});
      break;
    }
  }
  if (next < view.blocks.size()) return;
  if (order.size() == view.blocks.size()) return;

  std::vector<ExcerptBlock> merged;
  merged.reserve(order.size());
  for (const Slot slot : order) {
    if (!slot.resurrected) {
      merged.push_back(std::move(view.blocks[slot.index]));
      continue;
    }
    DroppedExcerpt& gone = view.dropped[slot.index];
    merged.push_back(std::move(gone.block));
    std::ranges::move(gone.refs, std::back_inserter(view.refs));
  }
  view.blocks = std::move(merged);
  std::vector<DroppedExcerpt> still_gone;
  for (std::size_t d = 0; d < view.dropped.size(); ++d) {
    if (!found[d]) still_gone.push_back(std::move(view.dropped[d]));
  }
  view.dropped = std::move(still_gone);
  RebuildHeaderIndex(view);
}

Index LineCountOf(std::string_view body) {
  if (body.empty()) return 0;
  Index n = 0;
  for (const char c : body) n += static_cast<Index>(c == '\n');
  if (!body.ends_with('\n')) ++n;
  return n;
}

std::string_view ExpectedSpanBytes(const ExcerptBlock& block) {
  std::string_view expected{block.original};
  if (block.synthesized_newline && expected.ends_with('\n')) expected.remove_suffix(1);
  return expected;
}

constexpr Index kReanchorWindow = 400;

struct FoundSpan {
  Index first{0};
  Index last{0};
  bool ok{false};
};

Index LinesApart(Index a, Index b) { return (a > b) ? (a - b) : (b - a); }

FoundSpan FindSpanAgain(const std::vector<std::string_view>& lines, Index first, Index last,
                        const std::vector<std::string_view>& want) {
  const Index count = std::ssize(lines);
  const Index n = std::ssize(want);
  if ((n == 0) || (count == 0)) return {};
  const Index lo = std::max<Index>(1, first - kReanchorWindow);
  const Index hi = std::min<Index>(count, last + kReanchorWindow);
  if (lo > hi) return {};

  for (Index step = 0; step <= kReanchorWindow; ++step) {
    for (const Index at : {first + step, first - step}) {
      if ((at >= lo) && ((at + n - 1) <= hi)) {
        bool same = true;
        for (Index k = 0; same && (k < n); ++k) same = (lines[at + k - 1] == want[k]);
        if (same) return FoundSpan{at, at + n - 1, true};
      }
      if (step == 0) break;
    }
  }

  const auto unique_line = [&](std::string_view text, Index& at) {
    Index seen = 0;
    for (Index l = lo; l <= hi; ++l) {
      if (lines[l - 1] != text) continue;
      if (++seen > 1) return false;
      at = l;
    }
    return seen == 1;
  };

  Index head = 0;
  Index head_k = 0;
  bool head_found = false;
  for (Index k = 0; (k < n) && !head_found; ++k) {
    if (!unique_line(want[k], head)) continue;
    head_k = k;
    head_found = true;
  }
  Index tail = 0;
  Index tail_k = 0;
  bool tail_found = false;
  for (Index k = 0; (k < n) && !tail_found; ++k) {
    if (!unique_line(want[n - 1 - k], tail)) continue;
    tail_k = k;
    tail_found = true;
  }
  if (!head_found || !tail_found) return {};
  while ((head_k > 0) && (head > 1) && (lines[head - 2] == want[head_k - 1])) {
    --head;
    --head_k;
  }
  while ((tail_k > 0) && (tail < count) && (lines[tail] == want[n - tail_k])) {
    ++tail;
    --tail_k;
  }
  if ((head > tail) || (head < 1) || (tail > count)) return {};
  // The head and the tail were found by two independent searches, so the span
  // they bracket is only a candidate: nothing so far has looked at what is
  // between them, or even at how many lines that is. Re-anchoring hands that
  // whole span to the view's body, so a span wider than the block is the file
  // having *gained* lines between the two anchors -- lines this view has never
  // shown, that no undo of the user's put there, and that the write would
  // delete without ever naming them. A span no wider than the block is the
  // revert this fallback exists for: the file lost or altered lines inside a
  // region the user is looking at, and :w puts back what they see.
  if ((tail - head + 1) > n) return {};
  if ((LinesApart(head, first) > kReanchorWindow) || (LinesApart(tail, last) > kReanchorWindow)) {
    return {};
  }
  return FoundSpan{head, tail, true};
}

bool ReanchorFile(const std::vector<ExcerptBlock*>& blocks, const std::string& content) {
  const std::vector<std::string_view> lines = SplitViewLines(content);
  std::vector<FoundSpan> found;
  found.reserve(blocks.size());
  for (const ExcerptBlock* block : blocks) {
    const std::vector<std::string_view> want = SplitViewLines(block->original);
    const FoundSpan at = FindSpanAgain(lines, block->first, block->last, want);
    if (!at.ok) return false;
    if (!found.empty() && (at.first <= found.back().last)) return false;
    found.push_back(at);
  }

  for (std::size_t i = 0; i < blocks.size(); ++i) {
    ExcerptBlock& block = *blocks[i];
    const FoundSpan at = found[i];
    std::string fresh;
    for (Index l = at.first; l <= at.last; ++l) {
      fresh += lines[static_cast<std::size_t>(l - 1)];
      fresh += '\n';
    }
    block.synthesized_newline = (at.last == std::ssize(lines)) && !content.ends_with('\n');
    block.line = std::clamp(block.line + (at.first - block.first), at.first, at.last);
    block.first = at.first;
    block.last = at.last;
    block.original = std::move(fresh);
  }
  return true;
}

bool FileMovedUnderModel(const ExcerptView& view, const std::string& path) {
  FileStamp now;
  if (!StampFile(path, now)) return false;
  for (const FileStamp& then : view.stamps) {
    if (then.path == path) return !now.SameFile(then);
  }
  return true;
}

// `read_failure` is set, and re-anchoring abandoned, when a file that moved
// under the view could not be read. Skipping it silently -- which an empty
// return from ReadWholeFile used to do -- leaves the blocks on their old line
// numbers and lets the save go on to write through anchors nobody re-checked.
std::vector<std::string> ReanchorMovedFiles(Document& doc, const std::vector<ParsedHunk>& hunks,
                                            std::string& read_failure) {
  ExcerptView& view = doc.excerpts;
  std::map<std::string, std::vector<ExcerptBlock*>> by_path;
  for (const ParsedHunk& hunk : hunks) {
    ExcerptBlock& block = view.blocks[hunk.block];
    if (block.no_body) continue;
    by_path[block.path].push_back(&block);
  }
  std::vector<std::string> lost;
  for (auto& [path, blocks] : by_path) {
    if (!FileMovedUnderModel(view, path)) continue;
    std::error_code read_ec;
    const std::string content = ReadWholeFile(path, read_ec);
    if (read_ec) {
      if (read_failure.empty()) {
        read_failure = "cannot read " + path + ": " + read_ec.message();
      }
      return {};
    }
    if (content.empty()) continue;
    if (!ReanchorFile(blocks, content)) lost.push_back(path);
  }
  return lost;
}

inline constexpr std::size_t kNoOpenBuffer = static_cast<std::size_t>(-1);

struct PlannedFile {
  std::string canon;
  std::string path;
  std::vector<std::size_t> hunks;
  std::string content;
  std::size_t buffer{kNoOpenBuffer};
  std::vector<Change> edits;
  std::vector<std::string> bodies;
};

std::string SpliceIntoOpenBuffer(Editor& ed, const PlannedFile& file) {
  if ((file.buffer == kNoOpenBuffer) || (file.buffer >= ed.buffers.size())) return {};
  Document& held = ed.buffers[file.buffer];
  if (file.edits.empty()) return {};

  BreakUndoCoalescing(held.table);
  CursorState before;
  before.primary = static_cast<std::uint32_t>(held.selections.PrimaryIndex());
  for (const Selection& s : held.selections.Ranges()) {
    before.spans.push_back(CursorSpan{s.anchor, s.head});
  }

  std::vector<Edit> edits;
  if (const ErrorCtx err = Apply(held.table, file.edits, before, CursorState{}, &edits); err) {
    return file.path + ": " + FormatErrorCtx(err);
  }
  held.selections.MapThroughEdits(held.table, edits);
  held.selections.Normalize(held.table);
  CursorState after;
  after.primary = static_cast<std::uint32_t>(held.selections.PrimaryIndex());
  for (const Selection& s : held.selections.Ranges()) {
    after.spans.push_back(CursorSpan{s.anchor, s.head});
  }
  NoteCursorsAfter(held.table, std::move(after));
  BreakUndoCoalescing(held.table);

  const Index lines = std::max<Index>(0, LineCount(held.table) - 1);
  held.view.top_line = std::clamp<Index>(held.view.top_line, 0, lines);
  for (WindowNode& node : ed.windows) {
    if (node.dead || (node.kind != WindowNode::Kind::kLeaf)) continue;
    if (node.buffer != file.buffer) continue;
    node.view.top_line = std::clamp<Index>(node.view.top_line, 0, lines);
  }

  MarkUndoSavePoint(held.table);
  held.saved_undo_serial = CurrentUndoSerial(held.table);
  held.modified = false;
  std::ignore = StampFile(file.path, held.disk_stamp);
  return {};
}

struct CursorBlock {
  const ExcerptBlock* block{nullptr};
  Index header_line{0};
};

CursorBlock BlockAtCursor(const Editor& ed) {
  const Index here = LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary()));
  CursorBlock found;
  std::vector<ParsedHunk> hunks;
  std::string error;
  if (ParseExcerptView(ed.doc.table, ed.doc.excerpts.blocks, hunks, error)) {
    for (const ParsedHunk& hunk : hunks) {
      if (hunk.header_line > here) break;
      found = CursorBlock{&ed.doc.excerpts.blocks[hunk.block], hunk.header_line};
    }
    return found;
  }
  // The parse failed, which means a header was edited or a body line now reads
  // as one -- exactly the state in which `block.header_line` is least likely to
  // be true. It is a cache written when the view was built and refreshed on
  // save, and nothing maps it through the edits in between, so trusting it here
  // put the cursor's block off by however many lines the edits added above it
  // and sent goto_excerpt_source to the wrong line, or the wrong block's file.
  //
  // Scanning the text costs one pass over a view the user is looking at, and it
  // is right by construction: it asks where the headers *are*, which is the
  // same question ParseExcerptView asks, minus the ordering it insists on.
  // Indexed rather than a block-per-line sweep: ParseExcerptView learned the
  // same lesson (see its `known` table) because one entry per block over one
  // entry per line is quietly O(lines x blocks) on a big view.
  std::unordered_map<std::string_view, const ExcerptBlock*> by_header;
  by_header.reserve(ed.doc.excerpts.blocks.size() * 2);
  for (const ExcerptBlock& block : ed.doc.excerpts.blocks) {
    by_header.emplace(block.header, &block);
    for (const std::string& worn : block.prior_headers) by_header.emplace(worn, &block);
  }

  const std::string text = ReadDocRange(ed.doc.table, Interval(0, DocLength(ed.doc.table)));
  const std::vector<std::string_view> lines = SplitViewLines(text);
  const Index scan_to = std::min<Index>(here, std::ssize(lines) - 1);
  for (Index at = 0; at <= scan_to; ++at) {
    const auto it = by_header.find(lines[static_cast<std::size_t>(at)]);
    if (it != by_header.end()) found = CursorBlock{it->second, at};
  }
  return found;
}

struct SearchQuery {
  std::vector<gai::Pcre2Regex> filters;
  std::vector<gai::Pcre2Regex> excludes;
  std::string error;
};

struct QueryWord {
  std::string text;
  bool quoted{false};
};

std::vector<QueryWord> SplitQuery(std::string_view query) {
  std::vector<QueryWord> words;
  std::size_t at = 0;
  while (at < query.size()) {
    while ((at < query.size()) && ((query[at] == ' ') || (query[at] == '\t'))) ++at;
    if (at >= query.size()) break;

    QueryWord word;
    if ((query[at] == '\'') || (query[at] == '"')) {
      const char quote = query[at];
      ++at;
      word.quoted = true;
      while ((at < query.size()) && (query[at] != quote)) word.text += query[at++];
      if (at < query.size()) ++at;
    } else {
      while ((at < query.size()) && (query[at] != ' ') && (query[at] != '\t')) {
        word.text += query[at++];
      }
    }
    words.push_back(std::move(word));
  }
  return words;
}

SearchQuery ParseSearchQuery(std::string_view query) {
  SearchQuery out;
  const std::vector<QueryWord> words = SplitQuery(query);

  std::vector<std::string_view> filters;
  std::vector<std::string_view> excludes;
  bool excluding = false;
  for (const QueryWord& word : words) {
    const bool flag = !word.quoted && (word.text.size() > 1) && word.text.starts_with('-');
    if (flag) {
      if ((word.text == "-f") || (word.text == "--filter")) {
        excluding = false;
      } else if ((word.text == "-e") || (word.text == "--exclude")) {
        excluding = true;
      } else {
        out.error = "only -f and -e are understood here, not " + word.text;
        return out;
      }
      continue;
    }
    (excluding ? excludes : filters).push_back(word.text);
  }

  if (filters.empty()) {
    out.error = "nothing to search for";
    return out;
  }
  try {
    out.filters = gai::ParseFilters(filters, true, false);
    out.excludes = gai::ParseFilters(excludes, true, false);
  } catch (const std::exception& err) {
    out.filters.clear();
    out.excludes.clear();
    out.error = err.what();
  }
  return out;
}

bool LineMatches(const SearchQuery& query, std::string_view line) {
  const auto hits = [line](const gai::Pcre2Regex& re) { return gai::Find(re, line); };
  if (!query.filters.empty() && !std::ranges::any_of(query.filters, hits)) return false;
  if (!query.excludes.empty() && std::ranges::any_of(query.excludes, hits)) return false;
  return true;
}

void PaintQueryMatches(const SearchQuery& query, std::string_view line,
                       std::vector<Interval>& out) {
  std::vector<gai::MatchSpan> spans;
  for (const gai::Pcre2Regex& re : query.filters) gai::FindSpans(re, line, spans);
  gai::MergeSpans(spans);
  for (const gai::MatchSpan& span : spans) out.push_back(Interval(span.start, span.end));
}

inline constexpr std::size_t kScanChunk = std::size_t{256} * 1024;
inline constexpr std::size_t kScanLineMax = std::size_t{8} * 1024 * 1024;

std::vector<char>& ScanBuffer() {
  thread_local std::vector<char> buffer(kScanChunk);
  return buffer;
}

template <typename OnLine>
bool ScanFileLines(const std::string& path, OnLine&& on_line) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  std::vector<char>& buffer = ScanBuffer();
  std::size_t held = 0;
  bool discarding = false;
  Index line_no = 0;
  for (;;) {
    if (held == buffer.size()) {
      if (buffer.size() < kScanLineMax) {
        buffer.resize(std::min(buffer.size() * 2, kScanLineMax));
      } else {
        ++line_no;
        on_line(std::string_view{buffer.data(), held}, line_no);
        held = 0;
        discarding = true;
      }
    }
    const ssize_t n = read(fd, buffer.data() + held, buffer.size() - held);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    const std::size_t filled = held + static_cast<std::size_t>(n);
    const std::string_view text{buffer.data(), filled};
    std::size_t at = 0;
    if (discarding) {
      const std::size_t eol = text.find('\n');
      if (eol == std::string_view::npos) {
        held = 0;
        if (n == 0) break;
        continue;
      }
      discarding = false;
      at = eol + 1;
    }
    for (;;) {
      const std::size_t eol = text.find('\n', at);
      if (eol == std::string_view::npos) break;
      ++line_no;
      on_line(text.substr(at, eol - at), line_no);
      at = eol + 1;
    }
    if (n == 0) {
      if (at < filled) {
        ++line_no;
        on_line(text.substr(at, filled - at), line_no);
      }
      break;
    }
    held = filled - at;
    if ((held > 0) && (at > 0)) std::memmove(buffer.data(), buffer.data() + at, held);
  }
  close(fd);
  if (buffer.size() > kScanChunk) {
    buffer.resize(kScanChunk);
    buffer.shrink_to_fit();
  }
  return true;
}

inline constexpr std::size_t kMaxCommandHits = 20000;

// The same ceiling for the scan jobs. `:from <command>` capped its hits and the
// search/symbol scans did not, which is the asymmetry rather than the intent:
// one ExcerptRef per matching line, each copying the whole path, is ~300 bytes
// of resident memory that nothing bounded. A broad pattern over a large tree
// walked that into hundreds of megabytes, on the worker thread, where a
// bad_alloc is reported rather than thrown into anything that can act on it.
inline constexpr std::size_t kMaxScanHits = kMaxCommandHits;

// How long PumpCommandJobs will keep polling a scan that has not published its
// result. Generous on purpose -- a real scan over a large tree must never trip
// it -- and finite on purpose, because the alternative is an editor that polls
// forever and a status line that lies about it.
inline constexpr long kScanDeadlineSeconds = 600;

std::vector<ExcerptRef> ParseCommandHits(std::string_view output, bool with_msg) {
  std::vector<ExcerptRef> hits;
  struct PathHits {
    bool regular{false};
    std::map<Index, std::size_t> at_line;
  };
  std::map<std::string, PathHits, std::less<>> by_path;
  for (std::size_t at = 0; at < output.size();) {
    std::size_t eol = output.find('\n', at);
    if (eol == std::string_view::npos) eol = output.size();
    std::string_view line = output.substr(at, eol - at);
    at = eol + 1;
    if (line.ends_with('\r')) line.remove_suffix(1);
    const std::size_t colon = line.find(':');
    if ((colon == std::string_view::npos) || (colon == 0)) continue;
    Index number = 0;
    const char* digits = line.data() + colon + 1;
    const auto [end, err] = std::from_chars(digits, line.data() + line.size(), number);
    if ((err != std::errc{}) || (end == digits) || (number < 1)) continue;
    const std::string_view path_view = line.substr(0, colon);
    auto probe = by_path.find(path_view);
    if (probe == by_path.end()) {
      std::error_code ec;
      const bool regular = fs::is_regular_file(fs::path{path_view}, ec);
      probe = by_path.emplace(std::string{path_view}, PathHits{regular, {}}).first;
    }
    if (!probe->second.regular) continue;

    Index col = 0;
    std::string msg;
    if (with_msg) {
      const char* scan = end;
      const char* const stop = line.data() + line.size();
      if ((scan < stop) && (*scan == ':')) {
        Index c = 0;
        const auto [cend, cerr] = std::from_chars(scan + 1, stop, c);
        if ((cerr == std::errc{}) && (cend != scan + 1) && (c >= 1)) {
          col = c;
          scan = cend;
        }
      }
      if ((scan < stop) && (*scan == ':')) ++scan;
      msg = std::string{Trim({scan, static_cast<std::size_t>(stop - scan)}, " \t")};
    }

    const auto [line_it, fresh] = probe->second.at_line.try_emplace(number, hits.size());
    if (!fresh) {
      ExcerptRef& have = hits[line_it->second];
      if (!msg.empty() && (have.msg.find(msg) == std::string::npos)) {
        have.msg += have.msg.empty() ? msg : (" · " + msg);
      }
      continue;
    }
    if (hits.size() >= kMaxCommandHits) {
      probe->second.at_line.erase(line_it);
      break;
    }
    hits.push_back(ExcerptRef{probe->first, number, col, std::move(msg)});
  }
  return hits;
}

// A ref is a path the excerpt machinery reads (BuildExcerpts) and that
// :goto-source later opens, so what these two build from store rows is the
// resolved spelling, not the stored one. The root is derived once for the loop,
// the way the store's own row reads derive it.
std::vector<ExcerptRef> PinRefs(Editor& ed) {
  std::vector<ExcerptRef> refs;
  if (!ed.project) return refs;
  const std::vector<Pin> pins = ed.project->Pins();
  const fs::path root = ProjectRoot();
  for (std::size_t at = 0; at < pins.size(); ++at) {
    const Pin& pin = pins[at];
    if (pin.path.empty()) continue;
    refs.push_back(ExcerptRef{ResolveStorePath(root, pin.path), std::max<Index>(1, pin.line), 0,
                              "pin " + std::to_string(at + 1)});
  }
  return refs;
}

std::vector<ExcerptRef> TrailRefs(Editor& ed) {
  std::vector<ExcerptRef> refs;
  if (!ed.project) return refs;
  constexpr int kMostRecent = 20;
  const Trail trail = TrailOf(*ed.project, kMostRecent);
  const fs::path root = ProjectRoot();
  for (const FileVisit& visit : trail.entries) {
    refs.push_back(ExcerptRef{ResolveStorePath(root, visit.path), std::max<Index>(1, visit.line)});
  }
  return refs;
}

std::string ExcerptTitle(ExcerptView::Kind kind, std::size_t count, std::string_view word) {
  using Kind = ExcerptView::Kind;
  const bool plural = (count != 1);
  std::string title = std::to_string(count);
  switch (kind) {
    case Kind::kReferences:
      title += plural ? " references to " : " reference to ";
      title += word;
      break;
    case Kind::kDefinitions:
      title += plural ? " definitions of " : " definition of ";
      title += word;
      break;
    case Kind::kSearch:
      title += plural ? " matches for " : " match for ";
      title += word;
      break;
    case Kind::kCommand:
      title += plural ? " hits from " : " hit from ";
      title += word;
      break;
    case Kind::kPins:
      title += plural ? " pins" : " pin";
      break;
    case Kind::kTrail:
      title += plural ? " recent locations" : " recent location";
      break;
  }
  return title;
}

struct ComposedView {
  std::string text;
  ExcerptView model;
  std::string title;
  std::string name;
};

std::string FromViewName(std::string_view command, bool watched) {
  return (watched ? "from!: " : "from: ") + std::string{command};
}

ComposedView ComposeExcerptView(Editor& ed, ExcerptView::Kind kind, std::vector<ExcerptRef> refs,
                                std::string_view word, bool watched, bool with_msg) {
  ExcerptView built;
  built.kind = kind;
  built.watched = watched;
  built.with_msg = with_msg;
  built.word = std::string{word};
  built.refs = std::move(refs);

  using Kind = ExcerptView::Kind;
  if (kind == Kind::kSearch) {
    const auto query = std::make_shared<const SearchQuery>(ParseSearchQuery(word));
    built.paint_line = [query](std::string_view line, std::vector<Interval>& out) {
      PaintQueryMatches(*query, line, out);
    };
  } else if ((kind == Kind::kReferences) || (kind == Kind::kDefinitions)) {
    built.paint_line = [word = std::string{word}](std::string_view line,
                                                  std::vector<Interval>& out) {
      for (std::size_t at = line.find(word); at != std::string_view::npos;
           at = line.find(word, at + 1)) {
        if (!IsWordBounded(line, at, word.size())) continue;
        out.push_back(Interval(static_cast<Index>(at), static_cast<Index>(at + word.size())));
      }
    };
  }

  std::string title = ExcerptTitle(kind, built.refs.size(), word);

  const std::string marker = (ed.settings.icons && !ed.settings.icon_file.empty())
                                 ? (ed.settings.icon_file + " ")
                                 : std::string{kExcerptMarker};
  std::string text = BuildExcerpts(built.refs, title,
                                   std::max<Index>(0, ed.settings.excerpt_context), marker, built);

  std::string name;
  switch (kind) {
    case Kind::kReferences: name = "references: " + std::string{word}; break;
    case Kind::kDefinitions: name = "definitions: " + std::string{word}; break;
    case Kind::kSearch: name = "search: " + std::string{word}; break;
    case Kind::kCommand: name = FromViewName(word, watched); break;
    case Kind::kPins: name = "pins"; break;
    case Kind::kTrail: name = "trail"; break;
  }
  return ComposedView{std::move(text), std::move(built), std::move(title), std::move(name)};
}

void BuildAndOpen(Editor& ed, ExcerptView::Kind kind, std::vector<ExcerptRef> refs,
                  std::string_view word, Index put_on_line, bool watched = false,
                  bool with_msg = false) {
  ComposedView made = ComposeExcerptView(ed, kind, std::move(refs), word, watched, with_msg);
  const std::size_t blocks = made.model.blocks.size();
  const std::string title = made.title;
  if (!OpenGeneratedView(ed, std::move(made.name), std::move(made.text), std::move(made.model),
                         put_on_line)) {
    return;
  }
  ed.status.clear();
}

void ReplaceViewInPlace(Editor& ed, Document& doc, std::size_t buffer, ComposedView made) {
  if ((ReadDocRange(doc.table, Interval(0, DocLength(doc.table))) == made.text) &&
      SameShape(doc.excerpts.blocks, made.model.blocks)) {
    ExcerptView& held = doc.excerpts;
    held.refs = std::move(made.model.refs);
    held.refs_stale = false;
    held.stamps = std::move(made.model.stamps);
    return;
  }
  if (const ErrorCtx err = SwapViewText(doc, made.text); err) {
    ed.status.Fail("the view could not be rebuilt: " + FormatErrorCtx(err));
    return;
  }
  doc.excerpts = std::move(made.model);
  doc.modified = false;
  doc.selections.Normalize(doc.table);
  if (ed.mode != Mode::kInsert) doc.selections.EnsureBlockCursors(doc.table);
  ClampScrollToText(ed, buffer, doc);
}

// Children signalled but not yet reaped. Deliberately not a `waitpid(-1, ...)`
// sweep: RunCmdWithCapture forks and waits for its own child, on the scan
// worker as well as here, and a blanket reap on this thread would steal that
// status out from under it and turn a working file-filter into a failure.
std::vector<pid_t> g_unreaped;

void ReapAbandoned() {
  std::erase_if(g_unreaped, [](pid_t pid) {
    int status = 0;
    const pid_t got = waitpid(pid, &status, WNOHANG);
    // Gone, or never ours to begin with. Still running means keep it.
    return (got == pid) || ((got < 0) && (errno != EINTR));
  });
}

void EndCommandJob(PendingCommand& job) {
  if (job.pid > 0) {
    kill(-job.pid, SIGTERM);
    kill(-job.pid, SIGKILL);
    // Bounded, not blocking. SIGKILL is not deliverable to a process in
    // uninterruptible sleep -- a child blocked on a stalled NFS or FUSE mount,
    // say -- and this runs on the editor thread, from :from-cancel and from
    // quit, with the terminal in raw mode and nothing drawing. An unbounded
    // wait there froze the editor with no way out.
    //
    // A child that took the SIGKILL is reapable within microseconds, so the
    // loop below almost always ends on its first or second turn; the deadline
    // is only what stops the pathological case from being unbounded. Whatever
    // is left is a zombie, swept by ReapAbandoned on a later pump, and
    // reparented to init if koi exits first.
    constexpr int kReapAttempts = 20;
    constexpr long kReapNapNs = 5L * 1000 * 1000;  // 5 ms; 100 ms in total
    bool reaped = false;
    for (int attempt = 0; (attempt < kReapAttempts) && !reaped; ++attempt) {
      int status = 0;
      const pid_t got = waitpid(job.pid, &status, WNOHANG);
      reaped = (got == job.pid) || ((got < 0) && (errno != EINTR));
      if (reaped) break;
      const timespec nap{0, kReapNapNs};
      nanosleep(&nap, nullptr);
    }
    if (!reaped) g_unreaped.push_back(job.pid);
    job.pid = -1;
  }
  if (job.fd >= 0) {
    close(job.fd);
    job.fd = -1;
  }
}

void AbandonJob(PendingCommand& job) {
  if (job.scan != nullptr) {
    job.scan->stop.store(true);
    return;
  }
  EndCommandJob(job);
}

bool CommandJobPending(const Editor& ed, std::string_view view_name) {
  return std::ranges::any_of(ed.pending_commands, [view_name](const PendingCommand& job) {
    return job.view_name == view_name;
  });
}

bool DrainCommandJob(PendingCommand& job) {
  constexpr std::size_t kOutputCap = std::size_t{16} << 20;
  char buf[16384];
  while (job.fd >= 0) {
    const ssize_t n = read(job.fd, buf, sizeof(buf));
    if (n > 0) {
      job.output.append(buf, static_cast<std::size_t>(n));
      if (!job.overflowed && (job.output.size() > kOutputCap)) {
        job.overflowed = true;
        // Guarded on both counts. `job.pid` is -1 for a job already cleaned up
        // and for every scan job, and kill(-(-1), ...) is kill(1, ...) -- a
        // SIGKILL aimed at init. And PumpCommandJobs drains a second time after
        // waitpid has already reaped the child, where the pid names nothing;
        // after pid wraparound it would name somebody else's process group.
        if (job.pid > 0) kill(-job.pid, SIGKILL);
      }
      continue;
    }
    if (n == 0) return true;
    if (errno == EINTR) continue;
    return (errno != EAGAIN) && (errno != EWOULDBLOCK);
  }
  return true;
}

ComposedView ComposeFailureView(const PendingCommand& job, std::string_view why) {
  ExcerptView built;
  built.kind = ExcerptView::Kind::kCommand;
  built.watched = job.watched;
  built.with_msg = job.with_msg;
  built.word = job.command;
  built.capture_names.emplace_back(kExcerptHeaderScope);
  built.capture_names.emplace_back(kExcerptMatchScope);

  constexpr std::size_t kTailBytes = std::size_t{64} << 10;
  constexpr Index kTailLines = 200;
  std::string_view tail{job.output};
  bool trimmed = false;
  if (tail.size() > kTailBytes) {
    tail.remove_prefix(tail.size() - kTailBytes);
    const std::size_t partial = tail.find('\n');
    tail.remove_prefix((partial == std::string_view::npos) ? tail.size() : (partial + 1));
    trimmed = true;
  }
  Index lines = static_cast<Index>(std::ranges::count(tail, '\n'));
  if (!tail.empty() && !tail.ends_with('\n')) ++lines;
  if (lines > kTailLines) {
    Index skip = lines - kTailLines;
    std::size_t at = 0;
    while ((skip > 0) && (at < tail.size())) {
      if (tail[at] == '\n') --skip;
      ++at;
    }
    tail.remove_prefix(at);
    trimmed = true;
  }

  std::string title = job.command + " -- " + std::string{why};
  std::string text = title;
  text += '\n';
  if (trimmed) text += "(earlier output dropped -- this is the tail)\n";
  text += '\n';
  text += tail;
  if (!text.ends_with('\n')) text += '\n';

  std::string name = FromViewName(job.command, job.watched);
  return ComposedView{std::move(text), std::move(built), std::move(title), std::move(name)};
}

void PresentCommandFailure(Editor& ed, const PendingCommand& job, std::string_view why,
                           const std::string& note) {
  const std::string said = job.command + " failed" + note + " -- its output is in the view";
  if (job.then == PendingCommand::Then::kOpen) {
    ComposedView made = ComposeFailureView(job, why);
    RecordJump(ed);
    RecordVisitHere(ed);
    if (!OpenGeneratedView(ed, std::move(made.name), std::move(made.text), std::move(made.model),
                           0)) {
      ed.status += note;
      return;
    }
    ed.status.Warn(said);
    return;
  }
  const std::size_t at = FindViewBuffer(ed, job.view_name);
  if (at >= BufferCount(ed)) return;
  Document& target = (ed.buffers.empty() || (at == ed.active)) ? ed.doc : ed.buffers[at];
  if (target.modified) {
    ed.status.Warn(job.command + " failed" + note +
                   " -- the view has unsaved edits, so the output is not shown");
    return;
  }
  AlignExcerptModel(target);
  ReplaceViewInPlace(ed, target, at, ComposeFailureView(job, why));
  ed.status.Warn(said);
}

void PresentRebuild(Editor& ed, const PendingCommand& job, std::size_t at,
                    std::vector<ExcerptRef> hits, const std::string& note) {
  Document& target = (ed.buffers.empty() || (at == ed.active)) ? ed.doc : ed.buffers[at];
  if (hits.empty()) {
    if (job.scan != nullptr) {
      ed.status.Warn("no longer any match for " + job.command + note);
      return;
    }
    if (target.modified) {
      ed.status = job.command + " reports nothing now -- shown after the view's edits are saved" +
                  note;
      return;
    }
    AlignExcerptModel(target);
    ReplaceViewInPlace(
        ed, target, at,
        ComposeExcerptView(ed, job.kind, {}, job.command, job.watched, job.with_msg));
    ed.status = job.command + " reports nothing now" + note;
    return;
  }
  AlignExcerptModel(target);
  target.excerpts.refs = std::move(hits);
  target.excerpts.refs_stale = false;
  if ((at == ed.active) && !target.modified) {
    RebuildExcerptView(ed);
    if (!note.empty()) ed.status += note;
  } else {
    target.excerpts.rebuild_on_focus = true;
  }
}

void PresentCommandJob(Editor& ed, PendingCommand& job, int status) {
  std::string note;
  std::vector<ExcerptRef> hits;
  if (job.scan != nullptr) {
    ScanJob& scan = *job.scan;
    if (!scan.error.empty()) note = " (" + scan.error + ")";
    if (job.kind == ExcerptView::Kind::kSearch) {
      hits = std::move(scan.hits);
    } else {
      std::vector<Symbol> found;
      std::unordered_set<std::string> seen;
      const auto take = [&found, &seen](std::vector<Symbol>&& rows) {
        for (Symbol& one : rows) {
          if (!seen.insert(FormatSymbolRow(one)).second) continue;
          found.push_back(std::move(one));
        }
      };
      if ((ed.project != nullptr) && !scan.hot.empty()) {
        scan.hot.resize(
            std::min(ed.project->RankSymbols(scan.hot, CurrentFile(ed)), scan.hot.size()));
        take(std::move(scan.hot));
      }
      take(std::move(scan.symbols));
      hits.reserve(found.size());
      for (const Symbol& one : found) hits.push_back(ExcerptRef{one.path, one.line});
    }
  } else {
    std::string why;
    if (job.overflowed) {
      why = "output capped, command killed";
    } else if (WIFEXITED(status) && (WEXITSTATUS(status) != 0)) {
      why = "exit " + std::to_string(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      why = "killed by signal " + std::to_string(WTERMSIG(status));
    }
    if (!why.empty()) note = " (" + why + ")";
    hits = ParseCommandHits(job.output, job.with_msg);
    if (hits.size() >= kMaxCommandHits) {
      note += " (first " + std::to_string(kMaxCommandHits) + " hits)";
    }
    if (hits.empty() && !why.empty()) {
      PresentCommandFailure(ed, job, why, note);
      return;
    }
  }

  const std::size_t at = FindViewBuffer(ed, job.view_name);

  if (job.then == PendingCommand::Then::kOpen) {
    if (hits.empty()) {
      switch (job.kind) {
        case ExcerptView::Kind::kSearch:
          ed.status.Warn("no match for " + job.command + note);
          break;
        case ExcerptView::Kind::kReferences:
          ed.status.Warn("no reference for " + job.command + note);
          break;
        case ExcerptView::Kind::kDefinitions:
          ed.status.Warn("no definition for " + job.command + note);
          break;
        default:
          ed.status.Warn("no file:line in the output of " + job.command + note);
          break;
      }
      return;
    }
    if (at < BufferCount(ed)) {
      Document& held = (ed.buffers.empty() || (at == ed.active)) ? ed.doc : ed.buffers[at];
      if (held.modified) {
        AlignExcerptModel(held);
        held.excerpts.refs = std::move(hits);
        held.excerpts.refs_stale = false;
        held.excerpts.rebuild_on_focus = true;
        ed.status.Warn(job.command + " finished -- the view has unsaved edits, so the "
                       "results wait for :w" + note);
        return;
      }
    }
    RecordJump(ed);
    RecordVisitHere(ed);
    BuildAndOpen(ed, job.kind, std::move(hits), job.command, 0, job.watched, job.with_msg);
    if (!note.empty()) ed.status += note;
    return;
  }

  if (at >= BufferCount(ed)) return;

  const StatusMessage said = ed.status;
  PresentRebuild(ed, job, at, std::move(hits), note);
  if (!said.empty() && (ed.status.level() != StatusLevel::kError) &&
      (ed.status.text() != said.text())) {
    ed.status.Log(ed.status.text(), ed.status.level());
    ed.status = said;
  }
}

}

bool StartCommandJob(Editor& ed, std::string_view command, bool watched, bool with_msg,
                     PendingCommand::Then then) {
  const std::string cmd{command};
  const std::string view_name = FromViewName(cmd, watched);
  std::erase_if(ed.pending_commands, [&view_name](PendingCommand& job) {
    if (job.view_name != view_name) return false;
    AbandonJob(job);
    return true;
  });

  int fds[2] = {-1, -1};
  if (pipe2(fds, O_CLOEXEC) != 0) {
    ed.status.Fail("cannot start " + cmd + ": " + std::strerror(errno));
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    ed.status.Fail("cannot start " + cmd + ": " + std::strerror(errno));
    return false;
  }
  if (pid == 0) {
    setpgid(0, 0);
    const int null = open("/dev/null", O_RDONLY);
    if (null >= 0) {
      dup2(null, 0);
      if (null > 2) close(null);
    } else {
      close(0);
    }
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    const char* shell = std::getenv("SHELL");
    if ((shell == nullptr) || (*shell == '\0')) shell = "/bin/sh";
    execl(shell, shell, "-c", cmd.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  setpgid(pid, pid);
  close(fds[1]);
  fcntl(fds[0], F_SETFL, O_NONBLOCK);

  PendingCommand job;
  job.command = cmd;
  job.watched = watched;
  job.with_msg = with_msg;
  job.then = then;
  job.view_name = view_name;
  job.pid = static_cast<int>(pid);
  job.fd = fds[0];
  job.started = std::chrono::steady_clock::now();
  ed.pending_commands.push_back(std::move(job));
  return true;
}

struct ScanInputs {
  std::shared_ptr<ScanJob> slot;
  std::string filter;
  std::string word;
  SearchQuery query;
  SymbolKind symbols{SymbolKind::kBoth};
  bool search{false};
  std::vector<std::string> hot;
};

std::vector<std::string> FilterFileList(const std::string& filter, std::string& error) {
  std::vector<std::string> paths;
  if (filter.empty()) return paths;
  const common::CmdResult result = common::RunCmdWithCapture(
      filter, common::CaptureMode::kPipe, common::CaptureMode::kDevNull);
  if (!result.output) {
    error = "could not run: " + filter;
    return paths;
  }
  const std::string_view text{result.output->buffer, result.output->size};
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t eol = std::min(text.find('\n', at), text.size());
    std::string_view line = text.substr(at, eol - at);
    while (!line.empty() && (line.back() == '\r')) line.remove_suffix(1);
    if (!line.empty()) paths.emplace_back(line);
    at = eol + 1;
  }
  return paths;
}

void RunScanJobBody(const std::shared_ptr<ScanInputs>& in) {
  ScanJob& slot = *in->slot;
  const auto stopped = [&slot] { return slot.stop.load(std::memory_order_relaxed); };

  // Stamped here rather than where the job was queued, and published with a
  // release so a reader that sees `begun` sees the time with it. Everything
  // that measures the job's age -- the watchdog, :from-cancel, the spinner --
  // wants how long it has been *running*, which on a shared pool is not how
  // long ago someone asked for it.
  slot.begun_at.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                      std::memory_order_relaxed);
  slot.begun.store(true, std::memory_order_release);

  std::string error;
  bool truncated = false;
  // Cancelled while it waited for a worker. Worth its own check because the
  // next line is the one that cannot be interrupted: FilterFileList blocks in
  // waitpid on a `find` that no stop flag reaches, so without this a job the
  // person already cancelled still costs a worker its full run.
  if (stopped()) return;
  const std::vector<std::string> paths = FilterFileList(in->filter, error);
  if (stopped()) return;
  if (in->search) {
    for (const std::string& path : paths) {
      if (stopped() || (slot.hits.size() >= kMaxScanHits)) break;
      // The inner test as well as the outer one: ScanFileLines has no way to be
      // told to stop, so without it a single enormous file keeps appending long
      // after the ceiling. Reading the rest of the *current* file is the most
      // this can now waste, and it costs no memory.
      ScanFileLines(path, [&in, &path, &slot](std::string_view line, Index line_no) {
        if (slot.hits.size() >= kMaxScanHits) return;
        if (LineMatches(in->query, line)) slot.hits.push_back(ExcerptRef{path, line_no});
      });
    }
    truncated = slot.hits.size() >= kMaxScanHits;
  } else {
    const auto collect = [&](std::span<const std::string> from, std::vector<Symbol>& into) {
      std::string scan_error;
      for (Symbol&& one : ScanSymbols(from, in->symbols, scan_error, in->word, &slot.stop)) {
        if (stopped() || (into.size() >= kMaxScanHits)) break;
        into.push_back(std::move(one));
      }
      if (error.empty()) error = std::move(scan_error);
      return into.size() >= kMaxScanHits;
    };
    if (!stopped() && collect(in->hot, slot.hot)) truncated = true;
    if (!stopped() && collect(paths, slot.symbols)) truncated = true;
  }
  // Said out loud, the way the command path says "(first N hits)". A result
  // that silently stops short is worse than a slow one, because the absence of
  // a match reads as proof there is none.
  if (truncated) {
    const std::string note = "first " + std::to_string(kMaxScanHits) + " matches";
    error = error.empty() ? note : (error + "; " + note);
  }
  slot.error = std::move(error);
}

// `done` is the only thing PumpCommandJobs polls, and the body above was the
// only writer -- so a throw anywhere in it (a bad_alloc out of the hit vector,
// out of ScanSymbols, out of the file list) simply never published. The pool
// wraps every task in a packaged_task, which swallows the exception into a
// future that AddTask's caller discards, so the process carried on and the
// editor polled "searching for ..." at 80 ms for the rest of the session with
// nothing reported anywhere. Publishing from a destructor makes every exit
// path, including the throwing one, wake the poll.
void RunScanJob(const std::shared_ptr<ScanInputs>& in) {
  ScanJob& job = *in->slot;
  struct Publish {
    ScanJob& target;
    ~Publish() { target.done.store(true, std::memory_order_release); }
  } publish{job};

  try {
    RunScanJobBody(in);
  } catch (const std::bad_alloc&) {
    // Short enough to live in the string's own buffer. A handler that
    // concatenates has to allocate, the allocator is what just failed, and the
    // second throw leaves RunScanJob entirely -- at which point the pool's
    // packaged_task tries to store it, which allocates again, and the process
    // terminates instead of the scan.
    job.error = "out of memory";
  } catch (const std::exception& e) {
    job.error = e.what();
  } catch (...) {
    job.error = "scan failed";
  }
}

void EnsureScanWorker(Index want) {
  // More than one, so a slow scan does not hold every later one behind it: the
  // file filter alone is a `find` over the tree, and with a single worker
  // refining a search ("wid" -> "widg") queued the new scan behind the old
  // one's whole life -- four seconds of a status line claiming to be searching.
  // The count comes from settings (scan-workers) and stays bounded low by its
  // parse range: these are I/O bound over a file list, and the point is
  // concurrency rather than throughput.
  //
  // Asks the pool for the count, rather than remembering that we once asked it
  // to start. StopScanWorker joins every worker, so a "spawned" flag goes stale
  // the moment it runs and every scan after that queued against a pool with no
  // threads -- polled forever by PumpCommandJobs, with nothing to run it.
  //
  // Grows and never shrinks: taking a worker away means joining it, which
  // blocks on whatever scan it is running -- not a price a config reload on
  // the UI thread should pay. A lowered setting takes effect on the next
  // start, and says so in the reference config.
  const std::size_t workers = static_cast<std::size_t>(std::max<Index>(1, want));
  const std::size_t have = ThreadPool::Instance().WorkerCount();
  if (have < workers) {
    ThreadPool::Instance().Init(static_cast<int>(workers - have));
  }
}

bool StartScanJob(Editor& ed, ExcerptView::Kind kind, std::string_view word,
                  PendingCommand::Then then) {
  auto in = std::make_shared<ScanInputs>();
  std::string view_name;
  switch (kind) {
    case ExcerptView::Kind::kSearch:
      in->query = ParseSearchQuery(word);
      if (!in->query.error.empty()) {
        ed.status.Fail(in->query.error);
        return false;
      }
      in->search = true;
      view_name = "search: ";
      break;
    case ExcerptView::Kind::kReferences:
      in->symbols = SymbolKind::kBoth;
      view_name = "references: ";
      break;
    case ExcerptView::Kind::kDefinitions:
      in->symbols = SymbolKind::kDefinitions;
      view_name = "definitions: ";
      break;
    default: return false;
  }
  view_name += word;

  std::erase_if(ed.pending_commands, [&view_name](PendingCommand& job) {
    if (job.view_name != view_name) return false;
    AbandonJob(job);
    return true;
  });

  in->slot = std::make_shared<ScanJob>();
  in->filter = FileFilterCommand(ed);
  in->word = word;
  if (!in->search && (ed.project != nullptr)) {
    in->hot = HotFilesFromHere(*ed.project, CurrentFile(ed));
  }

  PendingCommand job;
  job.command = word;
  job.then = then;
  job.kind = kind;
  job.view_name = view_name;
  job.scan = in->slot;
  job.started = std::chrono::steady_clock::now();
  ed.pending_commands.push_back(std::move(job));

  EnsureScanWorker(ed.settings.scan_workers);
  std::ignore = ThreadPool::Instance().AddTask([in] { RunScanJob(in); });
  return true;
}

namespace {

// How long a job has been *working*, and whether it has started working at all.
// For a command that is the same thing as how long ago it was asked for -- the
// fork happens in StartCommandJob -- but a scan is queued against a pool of a
// few workers, so it can wait behind other scans first. Everything that judges
// a job by its age has to ask, or a busy pool turns queue time into "this has
// been running for 4s" and eventually into a watchdog that gives up on a job
// which never ran.
struct JobAge {
  bool begun{true};
  std::chrono::steady_clock::duration ran{};
};

JobAge AgeOfJob(const PendingCommand& job) {
  const auto now = std::chrono::steady_clock::now();
  if (job.scan == nullptr) return JobAge{true, now - job.started};
  // Acquire pairs with the worker's release, so the timestamp read below is the
  // one that worker wrote and not whatever the slot was built with.
  if (!job.scan->begun.load(std::memory_order_acquire)) {
    return JobAge{false, now - job.started};
  }
  const std::chrono::steady_clock::time_point at{std::chrono::steady_clock::duration{
      job.scan->begun_at.load(std::memory_order_relaxed)}};
  return JobAge{true, now - at};
}

long JobSeconds(const JobAge& age) {
  return static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(age.ran).count());
}

}

bool PumpCommandJobs(Editor& ed, bool defer_present) {
  // Children EndCommandJob signalled but could not reap without blocking.
  // Without this they stay zombies for the life of the session, one per
  // :from-cancel of a command that would not die promptly.
  ReapAbandoned();

  std::erase_if(ed.pending_commands, [&ed](PendingCommand& job) {
    if (job.then != PendingCommand::Then::kRebuild) return false;
    for (std::size_t i = 0; i < BufferCount(ed); ++i) {
      const Document& doc = BufferAt(ed, i);
      if (doc.view_name == job.view_name) return false;
    }
    AbandonJob(job);
    return true;
  });

  bool running = false;
  for (std::size_t i = 0; i < ed.pending_commands.size();) {
    PendingCommand& job = ed.pending_commands[i];
    if (!job.done && (job.scan != nullptr)) {
      if (!job.scan->done.load(std::memory_order_acquire)) {
        // A scan that never publishes -- a worker that died, a task the pool
        // dropped -- used to be polled forever, at 80 ms, with a status line
        // claiming work was in progress. RunScanJob now publishes on every path
        // including a throw, so reaching the deadline is a bug rather than a
        // hazard; it is here so the next one is visible instead of eternal.
        //
        // A job still waiting for a worker is exempt rather than given a
        // deadline of its own: the deadline is a statement about a scan that
        // has stopped making progress, and one that has not started has made
        // none to stop. Waiting is bounded by the scans ahead of it -- every
        // queued task is one a worker will reach, because Shutdown drains the
        // queue and StartScanJob will not enqueue against a pool with no
        // workers -- so exempting it cannot strand the poll indefinitely.
        const JobAge age = AgeOfJob(job);
        if (!age.begun || (JobSeconds(age) < kScanDeadlineSeconds)) {
          running = true;
          ++i;
          continue;
        }
        // Dropped, not presented: without `done` there is no release to pair
        // with, so the worker may still be writing `hits` and reading them here
        // would be a race. The slot outlives us on the worker's own shared_ptr.
        const std::string gave_up = job.command;
        AbandonJob(job);
        ed.pending_commands.erase(ed.pending_commands.begin() +
                                  static_cast<std::ptrdiff_t>(i));
        ed.status.Warn(gave_up + " did not finish -- gave up on it");
        continue;
      }
      job.done = true;
      job.exit_status = 0;
    }
    if (!job.done) {
      DrainCommandJob(job);
      int status = 0;
      const pid_t got = waitpid(job.pid, &status, WNOHANG);
      if ((got == 0) || ((got < 0) && (errno == EINTR))) {
        running = true;
        ++i;
        continue;
      }
      // Cleared before the second drain, not after it: waitpid has reaped the
      // child, so the pid names nothing, and the drain below is the one that
      // can cross the output cap and reach for kill(-pid).
      job.pid = -1;
      DrainCommandJob(job);
      if (job.fd >= 0) {
        close(job.fd);
        job.fd = -1;
      }
      job.done = true;
      job.exit_status = status;
    }
    if (defer_present) {
      ++i;
      continue;
    }
    PendingCommand done = std::move(job);
    ed.pending_commands.erase(ed.pending_commands.begin() + static_cast<std::ptrdiff_t>(i));
    PresentCommandJob(ed, done, done.exit_status);
    i = 0;
    running = false;
  }
  return running;
}

bool CancelCommandJob(Editor& ed) {
  std::vector<PendingCommand>& jobs = ed.pending_commands;
  if (jobs.empty()) return false;
  auto it = std::ranges::find(jobs, ed.doc.view_name, &PendingCommand::view_name);
  if (it == jobs.end()) it = std::prev(jobs.end());
  const JobAge age = AgeOfJob(*it);
  const bool finished = it->done;
  AbandonJob(*it);
  // "after 4s" for a scan that spent those four seconds waiting for a worker
  // was a lie about where the time went, and the one place a person would look
  // to find out. A job that never started says so.
  std::string tail;
  if (!finished) {
    tail = age.begun ? (" after " + std::to_string(JobSeconds(age)) + "s")
                     : std::string{" while it was queued"};
  }
  ed.status = (finished ? "discarded the finished " : "cancelled ") + it->command + tail;
  jobs.erase(it);
  return true;
}

void KillAllCommandJobs(Editor& ed) {
  for (PendingCommand& job : ed.pending_commands) AbandonJob(job);
  ed.pending_commands.clear();
}

void AlignExcerptModel(Document& doc) {
  ExcerptEpochs& epochs = doc.excerpt_epochs;
  if (epochs.boundaries.empty()) return;
  std::size_t desired = 0;
  for (std::size_t i = epochs.boundaries.size(); i > 0; --i) {
    if (SerialApplied(doc.table, epochs.boundaries[i - 1])) {
      desired = i;
      break;
    }
  }
  if (desired != epochs.active) {
    epochs.store[epochs.active] = std::move(doc.excerpts);
    doc.excerpts = std::move(epochs.store[desired]);
    epochs.active = desired;
  }
  DropUnreachableEpochs(doc);
}

void DropUnreachableEpochs(Document& doc) {
  ExcerptEpochs& epochs = doc.excerpt_epochs;
  if (CanRedo(doc.table)) return;
  if (epochs.boundaries.size() <= epochs.active) return;
  epochs.boundaries.resize(epochs.active);
  epochs.store.resize(epochs.active + 1);
}

void OpenReferenceExcerpts(Editor& ed, const std::vector<Symbol>& found, std::string_view word) {
  if (found.empty()) {
    ed.status.Warn("no reference for " + std::string{word});
    return;
  }
  std::vector<ExcerptRef> refs;
  refs.reserve(found.size());
  for (const Symbol& one : found) refs.push_back(ExcerptRef{one.path, one.line});

  RecordJump(ed);
  RecordVisitHere(ed);
  BuildAndOpen(ed, ExcerptView::Kind::kReferences, std::move(refs), word, 0);
}

void SearchExcerpts(Editor& ed, std::string_view query) {
  if (StartScanJob(ed, ExcerptView::Kind::kSearch, query, PendingCommand::Then::kOpen)) {
    ed.status = "searching for " + std::string{query} + " -- :from-cancel stops it";
  }
}

bool RebuildExcerptView(Editor& ed) {
  if (!IsExcerptView(ed.doc)) return false;
  AlignExcerptModel(ed.doc);
  const bool may_be_empty = (ed.doc.excerpts.kind == ExcerptView::Kind::kPins);
  if (ed.doc.excerpts.refs.empty() && !may_be_empty) return false;
  if (ed.doc.modified) {
    ed.status.Warn("the view has unsaved edits -- :w them before rebuilding");
    return true;
  }
  std::string keep_path;
  Index keep_line = 0;
  std::size_t keep_nth = 0;
  if (const CursorBlock at = BlockAtCursor(ed); at.block != nullptr) {
    keep_path = at.block->path;
    keep_line = at.block->line;
    for (const ExcerptBlock& block : ed.doc.excerpts.blocks) {
      if (&block == at.block) break;
      if (block.path == keep_path) ++keep_nth;
    }
  }

  const ExcerptView::Kind kind = ed.doc.excerpts.kind;
  const bool watched = ed.doc.excerpts.watched;
  const bool with_msg = ed.doc.excerpts.with_msg;
  const std::string word = ed.doc.excerpts.word;
  std::vector<ExcerptRef> refs;
  if (ed.doc.excerpts.refs_stale) {
    switch (kind) {
      case ExcerptView::Kind::kSearch:
      case ExcerptView::Kind::kReferences:
      case ExcerptView::Kind::kDefinitions:
        if (StartScanJob(ed, kind, word, PendingCommand::Then::kRebuild)) {
          ed.status = "re-scanning for " + word + " -- :from-cancel stops it";
        }
        return true;
      case ExcerptView::Kind::kCommand:
        if (watched) {
          if (StartCommandJob(ed, word, true, with_msg, PendingCommand::Then::kRebuild)) {
            ed.status = "re-running " + word + " -- :from-cancel stops it";
          }
          return true;
        }
        refs = ed.doc.excerpts.refs;
        break;
      case ExcerptView::Kind::kPins:
        refs = PinRefs(ed);
        break;
      case ExcerptView::Kind::kTrail:
        refs = TrailRefs(ed);
        break;
    }
    if (refs.empty() && !may_be_empty) {
      ed.status.Warn("no longer any match for " +
                     (word.empty() ? std::string{"the view"} : word));
      return true;
    }
  } else {
    refs = ed.doc.excerpts.refs;
  }
  const std::size_t was_active = ed.active;
  const Index was_revision = ed.doc.table.revision;
  BuildAndOpen(ed, kind, std::move(refs), word, 0, watched, with_msg);
  if ((ed.active == was_active) && (ed.doc.table.revision == was_revision)) return true;

  if (keep_path.empty()) return true;
  const ExcerptBlock* exact = nullptr;
  const ExcerptBlock* nth = nullptr;
  const ExcerptBlock* nearest = nullptr;
  std::size_t seen = 0;
  for (const ExcerptBlock& block : ed.doc.excerpts.blocks) {
    if (block.path != keep_path) continue;
    if (block.line == keep_line) {
      exact = &block;
      break;
    }
    if (seen == keep_nth) nth = &block;
    if ((nearest == nullptr) ||
        (LinesApart(block.line, keep_line) < LinesApart(nearest->line, keep_line))) {
      nearest = &block;
    }
    ++seen;
  }
  const ExcerptBlock* land = (exact != nullptr) ? exact : ((nth != nullptr) ? nth : nearest);
  if (land != nullptr) {
    const Index at = LineStart(ed.doc.table, land->header_line);
    ed.doc.selections.Set(Selection{at, at, -1});
    ApplyModeInvariants(ed);
    ed.doc.view.top_line =
        std::max<Index>(0, land->header_line - std::max<Index>(1, ed.doc.view.rows) / 2);
    ed.align_view_once = true;
  }
  return true;
}

void ShowReferenceExcerpts(Editor& ed) {
  const std::string word = SelectedWord(ed);
  if (word.empty()) {
    ed.status.Warn("nothing selected");
    return;
  }
  if (StartScanJob(ed, ExcerptView::Kind::kReferences, word, PendingCommand::Then::kOpen)) {
    ed.status = "scanning for " + word + " -- :from-cancel stops it";
  }
}

bool SaveExcerptView(Editor& ed) {
  Document& doc = ed.doc;
  AlignExcerptModel(doc);
  ReadoptDroppedExcerpts(doc);
  std::vector<ExcerptBlock>& blocks = doc.excerpts.blocks;

  std::vector<ParsedHunk> hunks;
  std::string error;
  if (!ParseExcerptView(doc.table, blocks, hunks, error)) {
    ed.status.Fail(error);
    return false;
  }

  // Every block, not only the ones whose file ends up being written. The
  // refresh used to live inside the per-file loop below, which skips any file
  // the save had no changes for -- so an edit that added a line to one file's
  // hunk moved every later header in the view and left the blocks belonging to
  // untouched files pointing at their old line numbers.
  for (const ParsedHunk& hunk : hunks) blocks[hunk.block].header_line = hunk.header_line;

  std::vector<std::string> lost;
  if (doc.excerpt_epochs.active < doc.excerpt_epochs.boundaries.size()) {
    std::string read_failure;
    lost = ReanchorMovedFiles(doc, hunks, read_failure);
    if (!read_failure.empty()) {
      ed.status.Fail(read_failure + " -- this view could not be re-anchored, so nothing was "
                                    "written; try again");
      return false;
    }
  }

  std::vector<bool> changed(hunks.size(), false);
  bool any = false;
  for (std::size_t i = 0; i < hunks.size(); ++i) {
    const ExcerptBlock& block = blocks[hunks[i].block];
    if (block.no_body) {
      if (!hunks[i].body.empty()) {
        ed.status.Fail("there are no file bytes under " + block.header +
                       " -- edits there cannot be written back");
        return false;
      }
      continue;
    }
    changed[i] = (hunks[i].body != block.original);
    any = any || changed[i];
  }
  if (!any && !lost.empty()) {
    ed.status.Warn(DisplayPath(lost.front()) +
                   " has moved too far under this view -- redo, or rebuild it, to take the "
                   "file as it stands");
    return false;
  }
  if (!any) {
    MarkUndoSavePoint(doc.table);
    doc.saved_undo_serial = CurrentUndoSerial(doc.table);
    doc.modified = false;
    ed.status = "nothing to write -- every hunk matches its file";
    return true;
  }

  std::vector<std::string> hunk_canon(hunks.size());
  for (std::size_t i = 0; i < hunks.size(); ++i) {
    hunk_canon[i] = CanonicalOf(blocks[hunks[i].block].path).string();
  }
  std::vector<std::string> dropped_canon;
  dropped_canon.reserve(doc.excerpts.dropped.size());
  for (const DroppedExcerpt& gone : doc.excerpts.dropped) {
    dropped_canon.push_back(CanonicalOf(gone.block.path).string());
  }

  std::map<std::string, PlannedFile> plan;
  for (std::size_t i = 0; i < hunks.size(); ++i) {
    if (!changed[i]) continue;
    const ExcerptBlock& block = blocks[hunks[i].block];
    PlannedFile& file = plan[hunk_canon[i]];
    file.canon = hunk_canon[i];
    file.path = block.path;
    file.hunks.push_back(i);
  }

  for (auto& [canon, file] : plan) {
    PieceTable disk;
    std::error_code read_ec;
    ResetToOriginal(disk, ReadWholeFile(file.path, read_ec));
    // The reason, and only for a read that actually failed. This branch used to
    // stand for both "the read failed" and "the file is empty", and -- worse --
    // a read that died part-way did not reach it at all: the truncated image
    // went on to be spliced and written, silently dropping everything past the
    // failure. The truncation is an error now; these two just have to stay
    // told apart, since only one of them is worth retrying.
    if (read_ec) {
      ed.status.Fail("cannot read " + file.path + " to write it back: " + read_ec.message());
      return false;
    }
    if (DocLength(disk) == 0) {
      ed.status.Fail(file.path + " is empty -- rebuild the view");
      return false;
    }
    const Index disk_lines = LineCount(disk);

    std::ranges::sort(file.hunks, {}, [&](std::size_t i) { return blocks[hunks[i].block].first; });
    Index prev_last = 0;
    for (const std::size_t i : file.hunks) {
      const ExcerptBlock& block = blocks[hunks[i].block];
      if (block.first <= prev_last) {
        ed.status.Fail("two excerpts overlap in " + file.path + " -- rebuild the view");
        return false;
      }
      prev_last = block.last;
      if ((block.first < 1) || (block.last > disk_lines)) {
        ed.status.Fail(file.path + " shrank under " + block.header + " -- rebuild the view");
        return false;
      }
      const Index begin = LineStart(disk, block.first - 1);
      const Index end =
          (block.last < disk_lines) ? LineStart(disk, block.last) : DocLength(disk);
      if (ReadDocRange(disk, Interval(begin, end)) != ExpectedSpanBytes(block)) {
        ed.status.Fail(file.path + " changed under " + block.header + " -- rebuild the view");
        return false;
      }
    }

    file.content = ReadDocRange(disk, Interval(0, DocLength(disk)));

    if (const std::size_t at = FindFileBuffer(ed, file.path); at < BufferCount(ed)) {
      if ((at == ed.active) || (at >= ed.buffers.size())) {
        ed.status.Fail(file.path + " is open as this view -- it cannot be written into itself");
        return false;
      }
      const Document& held = ed.buffers[at];
      if (held.modified) {
        ed.status.Fail(file.path + " has unsaved edits in a buffer -- :w or undo them there first");
        return false;
      }
      if (ReadDocRange(held.table, Interval(0, DocLength(held.table))) != file.content) {
        ed.status.Fail(file.path + " differs from the buffer holding it -- :reload there first");
        return false;
      }
      file.buffer = at;
    }

    file.bodies.reserve(file.hunks.size());
    for (const std::size_t i : file.hunks) {
      const ExcerptBlock& block = blocks[hunks[i].block];
      const Index begin = LineStart(disk, block.first - 1);
      const Index end =
          (block.last < disk_lines) ? LineStart(disk, block.last) : DocLength(disk);
      std::string replacement = hunks[i].body;
      if (block.synthesized_newline && (end == DocLength(disk)) &&
          replacement.ends_with('\n')) {
        replacement.pop_back();
      }
      file.bodies.push_back(std::move(replacement));
      file.edits.push_back(Change{begin, end, {}});
    }
    for (std::size_t at = 0; at < file.edits.size(); ++at) {
      file.edits[at].text = file.bodies[at];
    }
    for (std::size_t at = file.edits.size(); at > 0; --at) {
      const Change& change = file.edits[at - 1];
      file.content.replace(static_cast<std::size_t>(change.from),
                           static_cast<std::size_t>(change.to - change.from), change.text);
    }
  }

  int wrote_files = 0;
  int wrote_hunks = 0;
  std::string failure;
  std::string buffer_note;
  std::vector<Change> header_edits;
  std::vector<std::string> new_headers;
  for (auto& [canon, file] : plan) {
    if (const ErrorCtx err = AtomicWriteFile(file.path, file.content); err) {
      failure = file.path + ": " + FormatErrorCtx(err);
      break;
    }
    ++wrote_files;
    if (std::string broke = SpliceIntoOpenBuffer(ed, file); !broke.empty()) {
      buffer_note = " -- but the buffer on " + broke;
    }
    for (FileStamp& stamp : doc.excerpts.stamps) {
      if (stamp.path != file.path) continue;
      // Re-read after our own write, so what the model holds is what the file
      // now is -- not what we hoped writing it would make it.
      std::ignore = StampFile(file.path, stamp);
      break;
    }

    Index delta = 0;
    struct SpanShift {
      Index first{0};
      Index lines{0};
      Index moved{0};
    };
    std::vector<SpanShift> shifts;
    for (std::size_t i = 0; i < hunks.size(); ++i) {
      ExcerptBlock& block = blocks[hunks[i].block];
      if (hunk_canon[i] != canon) continue;
      const Index old_first = block.first;
      const Index old_lines = block.no_body ? 0 : (block.last - block.first + 1);
      const Index new_lines = changed[i] ? LineCountOf(hunks[i].body) : old_lines;
      if (new_lines != old_lines) {
        shifts.push_back(SpanShift{old_first, old_lines, new_lines - old_lines});
      }
      block.first += delta;
      if (changed[i]) {
        block.last = block.first + new_lines - 1;
        block.original = hunks[i].body;
        block.synthesized_newline =
            block.synthesized_newline && hunks[i].body.ends_with('\n');
        ++wrote_hunks;
      } else {
        block.last += delta;
      }
      block.line = std::clamp(block.line + delta, block.first, std::max(block.first, block.last));
      delta += new_lines - old_lines;

      std::string fresh = DeriveHeader(block, block.first, block.last);
      if (fresh != block.header) {
        if (!std::ranges::contains(block.prior_headers, block.header)) {
          block.prior_headers.push_back(std::move(block.header));
        }
      }
      if (fresh != hunks[i].header_text) {
        const Index start = LineStart(doc.table, block.header_line);
        header_edits.push_back(Change{start, start + std::ssize(hunks[i].header_text), {}});
        new_headers.push_back(fresh);
      }
      block.header = std::move(fresh);
    }

    for (std::size_t d = 0; d < doc.excerpts.dropped.size(); ++d) {
      DroppedExcerpt& gone = doc.excerpts.dropped[d];
      if (dropped_canon[d] != canon) continue;
      Index shift = 0;
      for (const SpanShift& moved : shifts) {
        if (moved.first < gone.block.first) shift += moved.moved;
      }
      if (shift == 0) continue;
      ExcerptBlock& block = gone.block;
      block.first += shift;
      block.last += shift;
      block.line = std::clamp(block.line + shift, block.first, std::max(block.first, block.last));
      std::string fresh = DeriveHeader(block, block.first, block.last);
      if (fresh != block.header) {
        if (!std::ranges::contains(block.prior_headers, block.header)) {
          block.prior_headers.push_back(std::move(block.header));
        }
        block.header = std::move(fresh);
      }
    }

    for (ExcerptRef& ref : doc.excerpts.refs) {
      if (ref.path != file.path) continue;
      Index shift = 0;
      for (const SpanShift& moved : shifts) {
        if ((moved.first + moved.lines) <= ref.line) {
          shift += moved.moved;
        } else if (moved.first <= ref.line) {
          const Index new_last = moved.first + moved.lines + moved.moved - 1;
          ref.line = std::clamp(ref.line, moved.first, std::max(moved.first, new_last));
        }
      }
      ref.line += shift;
    }
  }

  if (!header_edits.empty()) {
    std::size_t at = 0;
    for (Change& change : header_edits) change.text = new_headers[at++];
    std::ranges::sort(header_edits, {}, &Change::from);
    std::vector<Edit> edits;
    if (const ErrorCtx err =
            Apply(doc.table, header_edits, CursorState{}, CursorState{}, &edits);
        err) {
      ed.status.Fail("view headers could not be refreshed: " + FormatErrorCtx(err));
    } else {
      doc.selections.MapThroughEdits(doc.table, edits);
    }
  }
  RebuildHeaderIndex(doc.excerpts);

  if (wrote_files > 0) doc.excerpts.refs_stale = true;
  if (!failure.empty()) {
    ed.status.Fail("wrote " + std::to_string(wrote_files) + " of " +
                   std::to_string(plan.size()) + " files, then " + failure +
                   " -- :w retries the rest");
    return false;
  }
  MarkUndoSavePoint(doc.table);
  doc.saved_undo_serial = CurrentUndoSerial(doc.table);
  doc.modified = false;
  const std::string wrote = "wrote " + std::to_string(wrote_hunks) + " hunk" +
                            ((wrote_hunks == 1) ? "" : "s") + " into " +
                            std::to_string(wrote_files) + " file" +
                            ((wrote_files == 1) ? "" : "s");
  if (buffer_note.empty()) {
    ed.status = wrote;
  } else {
    ed.status.Warn(wrote + buffer_note);
  }

  if (const int rerun = RerunWatchedViews(ed); rerun > 0) {
    ed.status = wrote + " -- re-running " + std::to_string(rerun) +
                ((rerun == 1) ? " watched command" : " watched commands");
  }
  return true;
}

void ShowDefinitionExcerpts(Editor& ed) {
  const std::string word = SelectedWord(ed);
  if (word.empty()) {
    ed.status.Warn("nothing selected");
    return;
  }
  if (StartScanJob(ed, ExcerptView::Kind::kDefinitions, word, PendingCommand::Then::kOpen)) {
    ed.status = "scanning for " + word + " -- :from-cancel stops it";
  }
}

void CommandExcerpts(Editor& ed, std::string_view command, bool watched, bool with_msg) {
  command = Trim(command, " \t");
  if (command.empty()) {
    ed.status.Warn(":from <command> -- its file:line output becomes excerpts");
    return;
  }
  if (StartCommandJob(ed, command, watched, with_msg, PendingCommand::Then::kOpen)) {
    ed.status = "running " + std::string{command} + " -- :from-cancel stops it";
  }
}

void WatchView(Editor& ed) {
  AlignExcerptModel(ed.doc);
  if (!IsExcerptView(ed.doc) || (ed.doc.excerpts.kind != ExcerptView::Kind::kCommand)) {
    ed.status.Warn("this is not a :from view");
    return;
  }
  if (ed.doc.excerpts.watched) {
    ed.status.Warn("already watched -- :w re-runs " + ed.doc.excerpts.word);
    return;
  }
  const std::string marked = FromViewName(ed.doc.excerpts.word, true);
  if (const std::size_t clash = FindViewBuffer(ed, marked);
      (clash < BufferCount(ed)) && (clash != ed.active)) {
    ed.status.Warn(marked + " is already open -- watch that view, or :bc it first");
    return;
  }
  ed.doc.excerpts.watched = true;
  ed.doc.view_name = marked;
  ed.status = "watching -- :w re-runs " + ed.doc.excerpts.word;
}

void UnwatchView(Editor& ed) {
  AlignExcerptModel(ed.doc);
  if (!IsExcerptView(ed.doc) || (ed.doc.excerpts.kind != ExcerptView::Kind::kCommand) ||
      !ed.doc.excerpts.watched) {
    ed.status.Warn("this is not a watched view");
    return;
  }
  const std::string plain = FromViewName(ed.doc.excerpts.word, false);
  if (const std::size_t clash = FindViewBuffer(ed, plain);
      (clash < BufferCount(ed)) && (clash != ed.active)) {
    ed.status.Warn(plain + " is already open -- :bc it first");
    return;
  }
  ed.doc.excerpts.watched = false;
  std::erase_if(ed.pending_commands, [&ed](PendingCommand& job) {
    if (job.view_name != ed.doc.view_name) return false;
    AbandonJob(job);
    return true;
  });
  ed.doc.view_name = plain;
  ed.status = "unwatched -- :w no longer re-runs " + ed.doc.excerpts.word;
}

void PinExcerpts(Editor& ed) {
  std::vector<ExcerptRef> refs = PinRefs(ed);
  if (refs.empty()) {
    ed.status.Warn(ed.project ? "no pins" : "no project database");
    return;
  }
  RecordJump(ed);
  RecordVisitHere(ed);
  BuildAndOpen(ed, ExcerptView::Kind::kPins, std::move(refs), "", 0);
}

void TrailExcerpts(Editor& ed) {
  std::vector<ExcerptRef> refs = TrailRefs(ed);
  if (refs.empty()) {
    ed.status.Warn(ed.project ? "nothing on the trail yet" : "no project database");
    return;
  }
  RecordJump(ed);
  RecordVisitHere(ed);
  BuildAndOpen(ed, ExcerptView::Kind::kTrail, std::move(refs), "", 0);
}

void MessagesView(Editor& ed) {
  std::vector<StatusRecord> entries = ed.status.log();
  if (!ed.status.empty() &&
      (entries.empty() || (entries.back().text != ed.status.text()))) {
    entries.push_back(StatusRecord{ed.status.text(), ed.status.level()});
  }
  if (entries.empty()) {
    ed.status.Warn("no messages yet");
    return;
  }

  const auto lines_in = [](const std::string& text) {
    Index n = 1;
    for (const char c : text) n += (c == '\n') ? 1 : 0;
    return n;
  };
  std::string text = std::to_string(entries.size()) + " message(s), oldest first\n";
  Index line = 1;
  Index newest = 1;
  for (const StatusRecord& entry : entries) {
    switch (entry.level) {
      case StatusLevel::kError: text += "error: "; break;
      case StatusLevel::kWarning: text += "warn: "; break;
      case StatusLevel::kInfo: break;
    }
    text += entry.text;
    text += '\n';
    newest = line;
    line += lines_in(entry.text);
  }

  ExcerptView model;
  model.kind = ExcerptView::Kind::kCommand;
  model.word = "messages";
  RecordJump(ed);
  RecordVisitHere(ed);
  if (OpenGeneratedView(ed, "messages", std::move(text), std::move(model), newest)) {
    ed.status.clear();
  }
}

namespace {

bool SameRefs(const std::vector<ExcerptRef>& a, const std::vector<ExcerptRef>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if ((a[i].line != b[i].line) || (a[i].col != b[i].col) || (a[i].path != b[i].path)) {
      return false;
    }
  }
  return true;
}

}

void MarkLiveViewsStale(Editor& ed, ExcerptView::Kind kind) {
  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    Document& doc = (ed.buffers.empty() || (i == ed.active)) ? ed.doc : ed.buffers[i];
    if (!IsExcerptView(doc)) continue;
    AlignExcerptModel(doc);
    if (doc.excerpts.kind != kind) continue;
    doc.excerpts.refs_stale = true;
    doc.excerpts.rebuild_on_focus = true;
  }
}

void RefreshLiveExcerptViews(Editor& ed) {
  const StatusMessage said = ed.status;
  bool spoke = false;

  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    Document& doc = (ed.buffers.empty() || (i == ed.active)) ? ed.doc : ed.buffers[i];
    if (!IsExcerptView(doc) || !doc.excerpts.rebuild_on_focus) continue;
    AlignExcerptModel(doc);
    if (!doc.excerpts.rebuild_on_focus) continue;
    if (doc.modified) continue;
    if (!BufferOnScreen(ed, i)) continue;

    const ExcerptView::Kind kind = doc.excerpts.kind;
    const bool from_project =
        (kind == ExcerptView::Kind::kPins) || (kind == ExcerptView::Kind::kTrail);
    if (from_project) {
      std::vector<ExcerptRef> fresh =
          (kind == ExcerptView::Kind::kPins) ? PinRefs(ed) : TrailRefs(ed);
      doc.excerpts.rebuild_on_focus = false;
      if (SameRefs(fresh, doc.excerpts.refs)) {
        doc.excerpts.refs_stale = false;
        continue;
      }
      doc.excerpts.refs = std::move(fresh);
      doc.excerpts.refs_stale = false;
    } else {
      doc.excerpts.rebuild_on_focus = false;
    }
    if (doc.excerpts.refs.empty() && (kind != ExcerptView::Kind::kPins)) continue;

    if (i == ed.active) {
      RebuildExcerptView(ed);
    } else {
      ReplaceViewInPlace(ed, doc, i,
                         ComposeExcerptView(ed, kind, doc.excerpts.refs, doc.excerpts.word,
                                            doc.excerpts.watched, doc.excerpts.with_msg));
    }
    spoke = true;
  }

  if (spoke && (ed.status.level() != StatusLevel::kError)) ed.status = said;
}

int RerunWatchedViews(Editor& ed) {
  int started = 0;
  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    Document& doc = (ed.buffers.empty() || (i == ed.active)) ? ed.doc : ed.buffers[i];
    if (!IsExcerptView(doc)) continue;
    AlignExcerptModel(doc);
    if ((doc.excerpts.kind != ExcerptView::Kind::kCommand) || !doc.excerpts.watched) continue;
    if (StartCommandJob(ed, doc.excerpts.word, true, doc.excerpts.with_msg,
                        PendingCommand::Then::kRebuild)) {
      ++started;
    }
  }
  return started;
}

void MaybeRefreshExcerptView(Editor& ed) {
  Document& doc = ed.doc;
  if (!IsExcerptView(doc)) return;
  AlignExcerptModel(doc);
  if (doc.excerpts.rebuild_on_focus && !doc.modified) {
    doc.excerpts.rebuild_on_focus = false;
    RebuildExcerptView(ed);
    return;
  }
  if (CommandJobPending(ed, doc.view_name)) return;
  if (doc.excerpts.stamps.empty()) return;
  bool moved = false;
  for (const FileStamp& then : doc.excerpts.stamps) {
    FileStamp now;
    if (!StampFile(then.path, now) || !now.SameFile(then)) {
      moved = true;
      break;
    }
  }
  if (!moved) return;
  if (doc.modified) {
    ed.status.Warn("files changed under the view -- :w your edits, or rebuild to see them");
    for (FileStamp& stamp : doc.excerpts.stamps) std::ignore = StampFile(stamp.path, stamp);
    return;
  }
  doc.excerpts.refs_stale = true;
  RebuildExcerptView(ed);
}

bool CoversRef(const ExcerptBlock& block, const ExcerptRef& ref) {
  return (ref.path == block.path) && (ref.line >= block.first) &&
         (ref.line <= std::max(block.first, block.last));
}

void DropExcerptHunk(Editor& ed) {
  if (!IsExcerptView(ed.doc)) {
    ed.status.Warn("not an excerpt view");
    return;
  }
  AlignExcerptModel(ed.doc);
  ReadoptDroppedExcerpts(ed.doc);
  std::vector<ParsedHunk> hunks;
  std::string error;
  if (!ParseExcerptView(ed.doc.table, ed.doc.excerpts.blocks, hunks, error)) {
    ed.status.Fail(error);
    return;
  }

  std::vector<std::size_t> marked;
  for (const Selection& s : ed.doc.selections.Ranges()) {
    const Index here = LineAt(ed.doc.table, CursorOf(ed.doc.table, s));
    std::size_t at = hunks.size();
    for (std::size_t i = 0; i < hunks.size(); ++i) {
      if (hunks[i].header_line > here) break;
      at = i;
    }
    if (at < hunks.size()) marked.push_back(at);
  }
  std::ranges::sort(marked);
  const auto dup = std::ranges::unique(marked);
  marked.erase(dup.begin(), dup.end());
  if (marked.empty()) {
    ed.status.Warn("no excerpt here");
    return;
  }

  const auto region_start = [&](std::size_t i) {
    Index line = hunks[i].header_line;
    if ((line > 0) && LineContentRange(ed.doc.table, line - 1).empty()) --line;
    return LineStart(ed.doc.table, line);
  };
  std::vector<Change> cuts;
  for (const std::size_t at : marked) {
    const Index from = region_start(at);
    const Index to = ((at + 1) < hunks.size()) ? region_start(at + 1) : DocLength(ed.doc.table);
    if (to > from) cuts.push_back(Change{from, to, {}});
  }

  std::string retitled;
  {
    const ExcerptView& view = ed.doc.excerpts;
    const Interval span = LineContentRange(ed.doc.table, 0);
    const Index title_to = span.empty() ? 0 : (span.back() + 1);
    if (cuts.empty() || (cuts.front().from >= title_to)) {
      std::size_t left = view.refs.size();
      for (const std::size_t at : marked) {
        const ExcerptBlock& block = view.blocks[hunks[at].block];
        const auto covered = std::ranges::count_if(
            view.refs, [&block](const ExcerptRef& ref) { return CoversRef(block, ref); });
        left -= std::min(left, static_cast<std::size_t>(covered));
      }
      if (ReadDocRange(ed.doc.table, Interval(0, title_to)) ==
          ExcerptTitle(view.kind, view.refs.size(), view.word)) {
        retitled = ExcerptTitle(view.kind, left, view.word);
        cuts.insert(cuts.begin(), Change{0, title_to, retitled});
      }
    }
  }

  std::vector<Edit> edits;
  if (!cuts.empty()) {
    if (const ErrorCtx err = Apply(ed.doc.table, cuts, CursorState{}, CursorState{}, &edits);
        err) {
      ed.status.Fail(FormatErrorCtx(err));
      return;
    }
  }

  std::vector<ExcerptBlock>& blocks = ed.doc.excerpts.blocks;
  for (auto it = marked.rbegin(); it != marked.rend(); ++it) {
    DroppedExcerpt gone;
    gone.block = std::move(blocks[hunks[*it].block]);
    blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(hunks[*it].block));
    std::vector<ExcerptRef>& refs = ed.doc.excerpts.refs;
    const auto covered = [&gone](const ExcerptRef& ref) { return CoversRef(gone.block, ref); };
    const auto kept = std::stable_partition(refs.begin(), refs.end(), std::not_fn(covered));
    std::move(kept, refs.end(), std::back_inserter(gone.refs));
    refs.erase(kept, refs.end());
    ed.doc.excerpts.dropped.push_back(std::move(gone));
  }
  RebuildHeaderIndex(ed.doc.excerpts);

  ed.doc.modified = true;
  ed.doc.selections.MapThroughEdits(ed.doc.table, edits);
  ed.doc.selections.EnsureBlockCursors(ed.doc.table);
  ed.status = "dropped " + std::to_string(marked.size()) + " excerpt" +
              ((marked.size() == 1) ? "" : "s") +
              (ed.doc.excerpts.watched ? " -- :w re-arms the watch" : "");
}

void GotoExcerptSource(Editor& ed) {
  if (!IsExcerptView(ed.doc)) {
    ed.status.Warn("not an excerpt view");
    return;
  }
  AlignExcerptModel(ed.doc);
  ReadoptDroppedExcerpts(ed.doc);
  const auto [block, header_line] = BlockAtCursor(ed);
  if (block == nullptr) {
    ed.status.Warn("no excerpt here");
    return;
  }
  const Index cursor = CursorOf(ed.doc.table, ed.doc.selections.Primary());
  const Index here = LineAt(ed.doc.table, cursor);
  const Index into = here - header_line;
  const Index line = (into <= 0) ? block->line
                                 : std::min(block->last, block->first + into - 1);
  const Index column = (into <= 0) ? std::max<Index>(1, block->col)
                                   : (cursor - LineStart(ed.doc.table, here) + 1);
  OpenAt(ed, block->path, line, column);
}

void LastPicker(Editor& ed) {
  std::string name;
  std::string query;
  if (!ReadLastPicker(name, query)) {
    ed.status.Warn("no picker to reopen");
    return;
  }
  if (name == "files") {
    FilePicker(ed, query);
  } else if (name == "content") {
    ContentPicker(ed, query);
  } else if (name == "symbols") {
    SymbolPicker(ed, query);
  } else if (name == "buffer-symbols") {
    BufferSymbolPicker(ed, query);
  } else if (name == "buffers") {
    BufferPicker(ed, query);
  } else if (name == "definition") {
    LookUpSymbol(ed, "definition", SymbolKind::kDefinitions, "definition", query);
  } else if (name == "references") {
    LookUpSymbol(ed, "references", SymbolKind::kBoth, "reference", query);
  } else {
    ed.status.Warn("no picker called " + name);
  }
}

void ToggleSidebar(Editor& ed) {
  const std::string self = SelfExe();
  if (self.empty()) {
    ed.status.Fail("cannot find koi to run the sidebar");
    return;
  }
  const std::string command = self + " --sidebar";

  const char* pane = std::getenv("TMUX_PANE");
  if ((pane == nullptr) || (*pane == '\0')) {
    ed.status.Warn("no tmux -- the sidebar is a pane");
    return;
  }
  const fs::path record = SidebarPanePath();

  std::error_code ec;
  if (fs::exists(record, ec)) {
    std::ifstream in{record, std::ios::binary};
    std::string cached;
    std::getline(in, cached);
    in.close();
    fs::remove(record, ec);

    const std::vector<std::string> alive = CaptureLines(ed, "tmux list-panes -a -F '#{pane_id}'");
    if (!cached.empty() && (std::ranges::find(alive, cached) != alive.end())) {
      RunShellCommand(ed, "tmux kill-pane -t " + ShellQuote(cached), ShellMode::kDiscard);
      ed.status.clear();
      return;
    }
  }

  const std::vector<std::string> opened =
      CaptureLines(ed, "tmux split-window -h -l 42 -P -F '#{pane_id}' -t " + ShellQuote(pane) +
                           " " + ShellQuote("exec " + command));
  if (opened.empty()) {
    ed.status.Fail("could not open the sidebar");
    return;
  }
  std::error_code dir_ec;
  fs::create_directories(record.parent_path(), dir_ec);
  std::ofstream out{record, std::ios::binary | std::ios::trunc};
  if (out) out << opened.front();
  RunShellCommand(ed, "tmux select-pane -t " + ShellQuote(pane), ShellMode::kDiscard);
  ed.status.clear();
}

void SetPinHere(Editor& ed, int slot) {
  if (!RequireProject(ed)) return;
  if (IsExcerptView(ed.doc)) {
    ed.status.Warn("nothing to pin here -- this view is not a file");
    return;
  }
  if (!HasDiskFile(ed.doc)) {
    ed.status.Warn("no file to pin");
    return;
  }
  const Index cursor = CursorOf(ed.doc.table, ed.doc.selections.Primary());
  const Index line = LineAt(ed.doc.table, cursor);
  const std::vector<Pin> before = ed.project->Pins();
  const bool replacing =
      (slot >= 1) && (slot <= std::ssize(before)) && !before[static_cast<size_t>(slot - 1)].path.empty();
  ed.project->SetPin(slot, CurrentFile(ed), line + 1,
                     cursor - LineStart(ed.doc.table, line) + 1);
  MarkLiveViewsStale(ed, ExcerptView::Kind::kPins);
  const std::vector<Pin> after = ed.project->Pins();
  const bool landed = (slot >= 1) && (slot <= std::ssize(after)) &&
                      // Read back, so spelled the way the store spells it.
                      (after[static_cast<size_t>(slot - 1)].path == ProjectPath(ed.doc.file)) &&
                      (after[static_cast<size_t>(slot - 1)].line == line + 1);
  if (!landed) {
    ed.status.Fail("pin " + std::to_string(slot) + " was not written to the project database");
    return;
  }
  ed.status = "pinned " + DisplayPath(ed.doc.file) + ":" + std::to_string(line + 1) + " to " +
              std::to_string(slot) + (replacing ? " -- replaced the old pin" : "");
}

void ClearPinSlot(Editor& ed, int slot) {
  if (!RequireProject(ed)) return;
  const std::vector<Pin> before = ed.project->Pins();
  const bool had =
      (slot >= 1) && (slot <= std::ssize(before)) && !before[static_cast<size_t>(slot - 1)].path.empty();
  ed.project->ClearPin(slot);
  MarkLiveViewsStale(ed, ExcerptView::Kind::kPins);
  if (!had) {
    ed.status.Warn("pin " + std::to_string(slot) + " was already empty");
    return;
  }
  const std::vector<Pin> after = ed.project->Pins();
  const bool gone =
      (slot >= 1) && (slot <= std::ssize(after)) && after[static_cast<size_t>(slot - 1)].path.empty();
  if (!gone) {
    ed.status.Fail("pin " + std::to_string(slot) + " was not cleared from the project database");
    return;
  }
  ed.status = "cleared pin " + std::to_string(slot);
}

void JumpToPin(Editor& ed, int slot) {
  if (!RequireProject(ed)) return;
  const std::vector<Pin> pins = ed.project->Pins();
  if ((slot < 1) || (slot > static_cast<int>(pins.size()))) {
    ed.status.Warn("no pin " + std::to_string(slot));
    return;
  }
  OpenPin(ed, pins[static_cast<size_t>(slot - 1)], "pin " + std::to_string(slot));
}

void JumpToTrail(Editor& ed, int index) {
  if (!RequireProject(ed)) return;
  // :jump-trail takes any number, not just the five labelled slots, so the
  // trail is read exactly as deep as this index reaches. The clamp is what
  // keeps a typed-in INT_MAX from overflowing the +1; nothing that deep can
  // exist anyway, since the visit table is pruned at open.
  constexpr int kDeepest = 1 << 20;
  const Trail trail = TrailOf(*ed.project, std::min(std::max(index, 0), kDeepest) + 1);
  if ((index < 0) || (index >= static_cast<int>(trail.entries.size()))) {
    ed.status.Warn("nothing at trail " + std::to_string(index));
    return;
  }
  const FileVisit& visit = trail.entries[static_cast<size_t>(index)];
  OpenAt(ed, ResolveStorePath(visit.path), visit.line, visit.column);
}

void JumpToHotSymbol(Editor& ed, int index) {
  if (!RequireProject(ed)) return;
  const std::vector<SymbolVisit> hot = ed.project->HotSymbols(kHotSymbolSlots);
  if ((index < 0) || (index >= static_cast<int>(hot.size()))) {
    ed.status.Warn("nothing at symbol " + std::to_string(index));
    return;
  }
  const SymbolVisit& visit = hot[static_cast<size_t>(index)];
  // Resolved before either use: the store keys what it is handed, and handing
  // back a key it already produced would key it a second time (ProjectKey is
  // not idempotent below the root), so the visit would be recorded against
  // "src/src/main.cpp" and then opened from there.
  const std::string file = ResolveStorePath(visit.file);
  ed.project->RecordSymbolVisit(visit.symbol, file, visit.line);
  OpenAt(ed, file, visit.line, 0);
}

namespace {

void RecordHere(Editor& ed, bool edited) {
  if (!ed.project || !HasDiskFile(ed.doc)) return;
  const std::string path = CurrentFile(ed);
  const Index cursor = CursorOf(ed.doc.table, ed.doc.selections.Primary());
  const Index line = LineAt(ed.doc.table, cursor);
  const Index column = cursor - LineStart(ed.doc.table, line) + 1;
  if (edited) {
    ed.project->RecordEdit(path, line + 1, column);
  } else {
    ed.project->RecordVisit(path, line + 1, column);
  }
  MarkLiveViewsStale(ed, ExcerptView::Kind::kTrail);
}

}

void RecordVisitHere(Editor& ed) { RecordHere(ed, false); }
void RecordEditHere(Editor& ed) { RecordHere(ed, true); }

void StartScanWorker(Index workers) { EnsureScanWorker(workers); }

void StopScanWorker() { ThreadPool::Instance().Shutdown(); }

void RestoreLastPosition(Editor& ed) {
  if (!ed.project || !HasDiskFile(ed.doc)) return;
  Index line = 0;
  Index column = 0;
  if (!ed.project->LastVisit(CurrentFile(ed), line, column)) return;
  Target target;
  target.path = ed.doc.file;
  target.line = line;
  target.has_line = true;
  target.column = column;
  target.has_column = column > 0;
  GoToTarget(ed.doc, target);
}

}
