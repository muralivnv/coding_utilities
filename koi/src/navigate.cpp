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
#include <optional>
#include <span>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "anchor.h"
#include "commands.h"
#include "mmap_stream.h"
#include "operation.h"
#include "project.h"
#include "query.h"
#include "regex.h"
#include "search.h"
#include "shell.h"
#include "subprocess.h"
#include "textobject.h"
#include "thread_pool.h"
#include "unicode.h"

namespace koi {

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

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kDefaultFileFilter =
    R"bash(find . -type f -printf '%P\n' | gai -e '.*(build|.git)')bash";

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

// False when the cursor did not move: no path, or a file that would not load --
// in which case OpenFile has already said why.
bool OpenAt(Editor& ed, std::string_view path, Index line, Index column) {
  if (path.empty()) return false;
  if (!OpenTarget(ed, TargetSpec(path, line, column))) return false;
  if (ed.project) {
    ed.project->RecordVisit(CurrentFile(ed), std::max<Index>(1, line), std::max<Index>(0, column));
  }
  return true;
}

// What landing on a symbol tells the store: the symbol to rank higher next
// time, and the pair of files seen together. The tail every symbol pick shares
// -- the lone-hit jump and the in-process band land the same way, so both must
// write the same rows. Called before OpenAt, while the file you came from is
// still the current one -- or after it with that file passed in, where the row
// is owed only to a landing that happened.
void RecordSymbolPick(Editor& ed, std::string_view name, std::string_view path, Index line,
                      std::string_view from = {}) {
  if (ed.project == nullptr) return;
  if (!name.empty()) ed.project->RecordSymbolVisit(name, path, line);
  const std::string here = from.empty() ? CurrentFile(ed) : std::string{from};
  ed.project->RecordCoVisit(here, path);
}

}

void WriteLastPicker(std::string_view name, std::string_view query) {
  const fs::path path = LastPickerStatePath();
  if (path.empty()) return;
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  if (!out) return;
  // The one tab is the separator, so the query cannot spend one: the reader
  // cuts at the next one and would hand back half of what was typed.
  std::string flat{query};
  std::ranges::replace(flat, '\t', ' ');
  out << name << '\t' << flat;
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

std::vector<RankedFile> RankedFiles(Editor& ed) {
  struct Row {
    RankedFile file;
    Index rank{-1};
    double score{0};
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
  // The one call site, and the reason the helper is lazy: a picker opening is
  // the only moment worth spending git subprocesses on, and after the first
  // open on a branch even this is a cache read.
  const std::vector<std::string>& branch_diff = BranchDiffFiles();
  const std::unordered_set<std::string_view> changed{branch_diff.begin(), branch_diff.end()};

  std::vector<Row> rows;
  std::unordered_set<std::string> seen;
  for (const std::string_view path : CapturedLines(ed, FileFilterCommand(ed))) {
    if (path.empty() || !seen.emplace(path).second) continue;
    Row row{RankedFile{std::string{path}, 1, 1, false}, -1, 0};
    const std::string key = ProjectPath(fs::path{path});
    const auto hit = frecent.find(key);
    if (hit != frecent.end()) {
      const FileVisit& visit = visits[static_cast<std::size_t>(hit->second)];
      row.rank = hit->second;
      row.file.visited = true;
      row.file.line = std::max<Index>(1, visit.line);
      row.file.column = std::max<Index>(1, visit.column);
      // The last of the frecency terms, and the only one the store cannot
      // apply: a file you have touched on this branch is likelier to be the one
      // you want next. Small on purpose -- it reorders inside the frecent set
      // and never lifts an unvisited file into it.
      row.score = visit.score * (changed.contains(key) ? 1.1 : 1.0);
    }
    rows.push_back(std::move(row));
  }

  std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    const bool a_seen = (a.rank >= 0);
    const bool b_seen = (b.rank >= 0);
    if (a_seen != b_seen) return a_seen;
    if (!a_seen) return false;
    // The store's own order is the tie-break, so two rows the branch bonus does
    // not separate come out exactly as they went in.
    if (a.score > b.score) return true;
    if (b.score > a.score) return false;
    return a.rank < b.rank;
  });

  std::vector<RankedFile> out;
  out.reserve(rows.size());
  for (Row& row : rows) out.push_back(std::move(row.file));
  return out;
}

namespace {

std::string SelectedWord(const Editor& ed) {
  const Selection& primary = ed.doc.selections.Primary();
  if (primary.IsEmpty()) return {};
  const std::string text = ReadDocRange(ed.doc.table, primary.Range());
  return std::string{Trim(text, " \t\r\n")};
}

// Where jumping to `path` should land, given that the store only learns your
// position at the handful of places that call RecordVisitHere. A buffer that is
// already open knows better than the store does -- you can have moved half the
// file since the last visit was recorded, and switching panes records nothing
// at all -- so the live cursor wins whenever there is one.
//
// `path` is the resolved spelling, which is what Document::file holds.
bool LiveCursorIn(const Editor& ed, std::string_view path, Index& line, Index& column) {
  const std::size_t at = FindFileBuffer(ed, fs::path{path});
  if (at >= BufferCount(ed)) return false;
  const Document& doc = BufferAt(ed, at);
  if (IsExcerptView(doc)) return false;
  const Index cursor = CursorOf(doc.table, doc.selections.Primary());
  const Index at_line = LineAt(doc.table, cursor);
  line = at_line + 1;
  column = cursor - LineStart(doc.table, at_line) + 1;
  return true;
}

void OpenPin(Editor& ed, const Pin& pin, std::string_view what) {
  if (pin.path.empty()) {
    ed.status = std::string{what} + " is not set";
    return;
  }
  // The pin came out of the store, so it is spelled against the project root;
  // opening resolves against the current directory (ResolveStorePath).
  const std::string path = ResolveStorePath(pin.path);
  Index line = pin.line;
  Index column = pin.column;
  std::ignore = LiveCursorIn(ed, path, line, column);
  OpenAt(ed, path, line, column);
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

namespace {

void AppendFragment(std::string& command, std::string_view piece) {
  piece = Trim(piece, " \t\n");
  if (piece.empty()) return;
  if (!command.empty()) command += ' ';
  command += piece;
}

// The two sources a child produces, and the only pickers that spawn one --
// everything else the band builds in process. Each is the file filter piped
// into one producer, and the query rides neither: the child streams every line
// it finds and the band filters what lands.
struct ScanSource {
  std::string_view name;
  std::string_view produce;
};

constexpr std::array<ScanSource, 2> kScanSources{{
    {"content", R"bash(xargs gai --no-color -f '\w' -v -d : --files)bash"},

    {"symbols", R"bash(%{koi} --symbol-mode --picker-rows --definitions \
      --hot-first --from %{buffer_name} --files -)bash"},
}};

}

std::string PickerScanCommand(const Editor& ed, std::string_view name) {
  for (const ScanSource& one : kScanSources) {
    if (one.name != name) continue;
    std::string command;
    AppendFragment(command, FileFilterCommand(ed));
    command += " |";
    AppendFragment(command, one.produce);
    return command;
  }
  return {};
}

std::vector<BufferRow> BufferPickerRows(const Editor& ed) {
  std::vector<BufferRow> rows;
  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    const Document& doc = BufferAt(ed, i);
    const std::string index = "#" + std::to_string(i);
    if (!HasDiskFile(doc) && !IsExcerptView(doc)) {
      rows.push_back(BufferRow{index + " [scratch]", index});
      continue;
    }
    const Index line = LineAt(doc.table, CursorOf(doc.table, doc.selections.Primary()));
    const Index column = CursorOf(doc.table, doc.selections.Primary()) - LineStart(doc.table, line);
    const std::string name = IsExcerptView(doc) ? doc.view_name : DisplayPath(doc.file);
    const std::string where =
        name + ":" + std::to_string(line + 1) + ":" + std::to_string(column + 1);
    // A row is one line on the band and its payload is opened verbatim, so a
    // name holding a tab or a newline can be neither drawn as one row nor
    // reopened as itself. Nothing can encode that here; leave the row out.
    if (where.find_first_of("\t\n\r") != std::string::npos) continue;
    rows.push_back(BufferRow{where, IsExcerptView(doc) ? index : where});
  }
  return rows;
}

bool ChooseBufferRow(Editor& ed, std::string_view payload) {
  if (payload.empty()) return false;
  const std::string_view digits = payload.substr(1);
  const bool indexed = (payload.front() == '#') && !digits.empty() &&
                       std::ranges::all_of(digits, [](char c) { return (c >= '0') && (c <= '9'); });
  if (indexed) {
    std::size_t at = 0;
    for (const char c : digits) {
      at = (at * 10) + static_cast<std::size_t>(c - '0');
      if (at >= BufferCount(ed)) return false;
    }
    RecordJump(ed);
    SwitchToBuffer(ed, at);
    ed.mode = Mode::kNormal;
    ApplyModeInvariants(ed);
    ed.status.clear();
    return true;
  }
  return OpenTarget(ed, payload);
}

bool PickerShowsContext(PickerState::Source source) {
  switch (source) {
    case PickerState::Source::kDefs:
    case PickerState::Source::kRefs:
    case PickerState::Source::kFileSymbols:
    case PickerState::Source::kProjectSymbols:
    case PickerState::Source::kContent: return true;
    case PickerState::Source::kFiles:
    case PickerState::Source::kBuffers: break;
  }
  return false;
}

namespace {

// The corpus a content pick's rows live in: the scan's bytes up to the last
// complete line. `shown` holds offsets into exactly this, so a mapping that
// relocated as it grew costs nothing -- there is no pointer to fix up.
std::string_view PickerCorpus(const PickerState& state) {
  if (!state.scan || (state.scan->out.buffer == nullptr)) return {};
  return std::string_view{state.scan->out.buffer, state.scan->parsed};
}

// The line starting at `from`, without its newline.
std::string_view PickerCorpusLine(std::string_view corpus, std::size_t from) {
  if (from >= corpus.size()) return {};
  const std::size_t eol = std::min(corpus.find('\n', from), corpus.size());
  std::string_view line = corpus.substr(from, eol - from);
  while (!line.empty() && (line.back() == '\r')) line.remove_suffix(1);
  return line;
}

// `path:line:` off the head of a content row -- gai's own `-v -d :` output, so
// the first two fields are the path and the number. The rest of the line is the
// file's, colons and all. The head's length is what the band wants -- it draws
// the two halves apart -- and `out`, where one is passed, is where the head
// points. Zero for a line with no head at all.
std::size_t ContentRowHead(std::string_view line, PickerTarget* out) {
  const std::size_t colon = line.find(':');
  if ((colon == std::string_view::npos) || (colon == 0)) return 0;
  const std::size_t next = line.find(':', colon + 1);
  if (next == std::string_view::npos) return 0;
  const std::string_view digits = line.substr(colon + 1, next - colon - 1);
  // A colon-bearing filename can put a long digit run where the number goes, and
  // running that into an Index is signed overflow. Eighteen fit with room to
  // spare, and no file has 10^18 lines: past that it is not a row.
  constexpr std::size_t kMaxLineDigits = 18;
  if (digits.empty() || (digits.size() > kMaxLineDigits)) return 0;
  Index number = 0;
  for (const char c : digits) {
    if ((c < '0') || (c > '9')) return 0;
    number = (number * 10) + (c - '0');
  }
  if (out != nullptr) {
    out->path = line.substr(0, colon);
    out->line = std::max<Index>(1, number);
    out->column = 1;
  }
  return next + 1;
}

bool ParseContentRow(std::string_view line, PickerTarget& out) {
  return ContentRowHead(line, &out) != 0;
}

}

std::string_view PickerRowText(const PickerState& state, std::size_t at) {
  if (at >= state.shown.size()) return {};
  if (state.source != PickerState::Source::kContent) {
    return PickerRowText(state, state.rows[state.shown[at]]);
  }
  return PickerCorpusLine(PickerCorpus(state), state.shown[at]);
}

std::string_view PickerRowLead(const PickerState& state, std::size_t at) {
  const std::string_view text = PickerRowText(state, at);
  if (state.source != PickerState::Source::kContent) return text;
  // The head is the detail here, so what leads the row is the file's own line,
  // trimmed and capped the way a defs or refs row's line is -- the two read as
  // the same kind of row because they are. A line with no head has none to take
  // off: it is drawn whole, and it opens nothing (PickerAccept says so).
  return TrimAnchorLine(text.substr(ContentRowHead(text, nullptr)));
}

std::string_view PickerRowDetail(const PickerState& state, std::size_t at) {
  if (at >= state.shown.size()) return {};
  if (state.source != PickerState::Source::kContent) return state.rows[state.shown[at]].detail;
  const std::string_view text = PickerRowText(state, at);
  const std::size_t head = ContentRowHead(text, nullptr);
  // Without the colon that joined it to the line: `path:line`, which is what
  // every other source's detail says.
  return (head == 0) ? std::string_view{} : text.substr(0, head - 1);
}

bool PickerRowTarget(const PickerState& state, std::size_t at, PickerTarget& out) {
  out = PickerTarget{};
  if (at >= state.shown.size()) return false;
  if (state.source == PickerState::Source::kContent) {
    return ParseContentRow(PickerRowText(state, at), out);
  }
  const PickerEntry& row = state.rows[state.shown[at]];
  out.path = row.target;
  out.line = row.line;
  out.column = row.column;
  out.name = row.name;
  return true;
}

std::size_t PickerTotal(const PickerState& state) {
  if (state.source != PickerState::Source::kContent) return state.rows.size();
  return state.scan ? state.scan->lines : 0;
}

void PickerScrollToSelected(PickerState& state) {
  const std::size_t window = std::max<std::size_t>(1, state.window);
  if (state.shown.size() <= window) {
    state.offset = 0;
    return;
  }
  if (state.selected < state.offset) state.offset = state.selected;
  if (state.selected >= (state.offset + window)) state.offset = state.selected - window + 1;
  state.offset = std::min(state.offset, state.shown.size() - window);
}

namespace {

// The width `shown[from..]` alone asks for. The append path measures only the
// rows that just landed and widens with them; a scan of thousands must not
// re-measure the whole list on every wake.
int PickerCardWidthFrom(const PickerState& state, std::size_t from) {
  // A card whose rows point into files maximises the line: it takes the screen,
  // and no row is measured to find that out. That is every source with a block
  // -- defs, refs, both symbol lists and content -- so the loop below is files
  // and buffers, whose rows are one short path each. Measuring the others cost a
  // DisplayWidth pass per shown row per keystroke over a list a scan grows
  // without limit, to land on a width the screen could not give anyway.
  if (PickerShowsContext(state.source)) return kPickerCardWide;
  // `i/n/m` and the two slashes, with a column of air either side. The index is
  // drawn padded to the shown count's width, so stepping from row 9 to row 10
  // cannot move what the count's column is kept off.
  const int count = (2 * PickerCountDigits(state.shown.size())) +
                    PickerCountDigits(PickerTotal(state)) + 4 +
                    static_cast<int>(DisplayWidth(PickerCountNote(state), 1));
  int card = kPickerCardMin;
  for (std::size_t at = from; at < state.shown.size(); ++at) {
    const PickerEntry& row = state.rows[state.shown[at]];
    // Text and detail read as one here -- a path with the line it was left on --
    // so they take one column between them.
    const int text = static_cast<int>(DisplayWidth(PickerRowText(state, row), 1));
    const int detail = static_cast<int>(DisplayWidth(row.detail, 1));
    card = std::max(card, 8 + text + detail + count);
  }
  return card;
}

}

// From the shown rows only: the block's lines change with every step, and a
// card that resized as you stepped is what this is here to stop. Pad,
// connector, digit, text, detail, the count's column, pad -- what
// DrawPickerBand lays out.
int PickerCardWidth(const PickerState& state) { return PickerCardWidthFrom(state, 0); }

std::string_view PickerSourceName(PickerState::Source source) {
  switch (source) {
    case PickerState::Source::kBuffers: return "buffers";
    case PickerState::Source::kDefs: return "definition";
    case PickerState::Source::kRefs: return "references";
    case PickerState::Source::kFileSymbols: return "buffer-symbols";
    case PickerState::Source::kProjectSymbols: return "symbols";
    case PickerState::Source::kContent: return "content";
    case PickerState::Source::kFiles: break;
  }
  return "files";
}

namespace {

// Whether what is typed into the band is this source's query. Defs and refs
// were opened for a word the selection named, and reopening them with a half
// of a filter pattern would look that up instead.
bool PickerQueryIsTyped(PickerState::Source source) {
  return (source != PickerState::Source::kDefs) && (source != PickerState::Source::kRefs);
}

// Whose rows lead with the symbol's name rather than the line it points at. In
// a symbol list the name is the information, and every row names a different
// one; in defs and refs they all name the same word, so the line is. The name
// is in hand when the row is built, which is why filtering a symbol list reads
// no files -- the block alone reads the selected target's.
bool PickerRowIsName(PickerState::Source source) {
  return (source == PickerState::Source::kFileSymbols) ||
         (source == PickerState::Source::kProjectSymbols);
}

void ShowAllPickerRows(PickerState& state) {
  state.shown.resize(state.rows.size());
  for (std::size_t i = 0; i < state.shown.size(); ++i) state.shown[i] = i;
  state.selected = 0;
  state.offset = 0;
}

// A query change on content: the pattern is put down and the pump carries it
// over the corpus-so-far, a budget of bytes per wake. Nothing on the band moves
// here -- the last good list stands until the catch-up reaches what the pump has
// parsed, which is what a pattern that will not compile leaves there too. Empty
// is the empty query, which every non-empty line meets. This is what makes each
// byte meet each query exactly once: the bytes before the change were met by the
// old pattern as they landed, the bytes after it are left to the catch-up, and
// here the corpus-so-far meets the new one.
void PickerStartRefilter(PickerState& state, std::string_view pattern) {
  if (!state.scan) {
    // No corpus, so no rows: content without a scan is a picker over nothing.
    state.shown.clear();
    state.selected = 0;
    state.offset = 0;
    return;
  }
  PickerScan& scan = *state.scan;
  scan.refiltering = true;
  scan.refilter_pattern = pattern;
  scan.refilter_at = 0;
  scan.refilter_shown.clear();
}

// Drops the coldest files until the cache is within its cap. Called with the
// file just read already in, so the newest is never the one that goes.
void PickerEvictFiles(PickerState& state) {
  while (state.lines.size() > kPickerCacheFiles) {
    auto coldest = state.lines.begin();
    for (auto at = state.lines.begin(); at != state.lines.end(); ++at) {
      if (at->second.used < coldest->second.used) coldest = at;
    }
    state.lines.erase(coldest);
  }
}

// A file's bytes and line index, read once per stamp. Null when it cannot be
// stat'ed, is past the prompt's `file_max`, or will not read: a row whose file
// went away shows no line rather than a stale one, and one whose file is too
// big to hold is told the same thing. The pointer is good until the next call --
// eviction is the only thing that ends an entry, and nothing keeps one across a
// lookup.
const PickerFileLines* PickerFileCache(PickerState& state, const std::string& path) {
  FileStamp now;
  if (!StampFile(path, now)) return nullptr;
  // The stamp's own size, so the ceiling is judged on the same stat the TOCTOU
  // idiom below is built on rather than on a third look at the file. Before the
  // cache lookup as well as before the read: a file that grew past the ceiling
  // stops being served, and the entry it had ages out.
  if (now.size > state.file_max) return nullptr;
  const std::uint64_t tick = ++state.lines_clock;
  const auto found = state.lines.find(path);
  if ((found != state.lines.end()) && found->second.stamp.SameFile(now)) {
    found->second.used = tick;
    return &found->second;
  }
  std::error_code ec;
  std::string text = ReadWholeFile(path, ec);
  if (ec) return nullptr;
  PickerFileLines entry;
  // Stamped on both sides of the read. Agreeing, the stamp describes the bytes;
  // disagreeing, the read raced a write and neither one does -- the file was
  // replaced between the stat and the read, and caching it under either stamp
  // leaves the wrong bytes there until something moves the file again. So the
  // older of the two is kept, and the oldest stamp there is is the default one:
  // no stat can return it, so the next look reads the file instead.
  FileStamp after;
  const bool settled = StampFile(path, after) && now.SameFile(after);
  entry.stamp = settled ? std::move(now) : FileStamp{};
  entry.used = tick;
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t eol = std::min(text.find('\n', at), text.size());
    entry.eols.push_back(eol);
    at = eol + 1;
  }
  entry.bytes = std::move(text);
  PickerFileLines& slot = state.lines[path];
  slot = std::move(entry);
  // This one carries the newest tick, so eviction cannot take it, and erasing
  // other nodes leaves the reference to this one good.
  PickerEvictFiles(state);
  return &slot;
}

// `want` lines of `path` from `from`, 1-based and clipped to the file. `first`
// comes back the number of the first line handed over, 0 for none. A file open
// as a buffer answers from its table -- unsaved edits are what a jump would
// land in -- and every other file from the cache above. Nothing here opens a
// buffer: reading the lines is what makes stepping free of that.
void PickerLinesOf(Editor& ed, PickerState& state, const std::string& path, Index from, Index want,
                   Index& first, std::vector<std::string>& out) {
  out.clear();
  first = 0;
  if (path.empty() || (want <= 0)) return;
  from = std::max<Index>(1, from);

  const std::size_t at = FindFileBuffer(ed, fs::path{path});
  if (at < BufferCount(ed)) {
    const Document& doc = BufferAt(ed, at);
    const Index count = LineCount(doc.table);
    if (from > count) return;
    first = from;
    for (Index line = from; (line < (from + want)) && (line <= count); ++line) {
      out.push_back(ReadDocRange(doc.table, LineContentRange(doc.table, line - 1)));
    }
    return;
  }

  const PickerFileLines* file = PickerFileCache(state, path);
  if (file == nullptr) return;
  const auto count = static_cast<Index>(file->LineCount());
  if (from > count) return;
  first = from;
  // The only copies the cache ever makes: `want` is three for the block and one
  // for a row, so what is displayed is materialised and the file's bytes stay
  // one buffer.
  for (Index line = from; (line < (from + want)) && (line <= count); ++line) {
    out.emplace_back(file->Line(static_cast<std::size_t>(line - 1)));
  }
}

// A defs or refs row's own text: the line it points at, freshened. The row
// arrived owning that line -- the scan that found it had the file's bytes in
// hand -- so this is not what makes the row showable, it is what keeps it
// honest: a file rewritten under an open picker says what it says now. Only the
// rows the band is showing ask, which is why a keystroke reads nothing. A line
// that will not read -- the file gone, or past the prompt's `file_max` -- leaves
// the row saying what it arrived saying. Nothing to do for a row that is
// already its name.
void PickerReadRow(Editor& ed, PickerState& state, PickerEntry& row, bool again) {
  // Content is the third source with nothing to read: its row is the line, sent
  // by the scan and living in the corpus. Guarded here as well as at every
  // caller, because a read for it would be a file opened per row of a list that
  // has hundreds of thousands.
  if (!PickerShowsContext(state.source) || PickerRowIsName(state.source) ||
      (state.source == PickerState::Source::kContent) || (row.read && !again)) {
    return;
  }
  std::vector<std::string> got;
  Index first = 0;
  PickerLinesOf(ed, state, row.target, row.line, 1, first, got);
  // Trimmed and capped exactly as the scan trimmed and capped what the row
  // arrived with, so a row that is re-read does not change shape under the
  // band; the leading indent is the line's, not the row's, and a row that is
  // most of a minified file is not a row.
  if (!got.empty()) row.text = std::string{TrimAnchorLine(got.front())};
  if (row.text.empty()) row.text = row.name;
  row.read = true;
}

}

// The lines the band and the block need, and no others: a picker over thousands
// of refs reads only the handful of files whose rows the window is on, and
// scrolling is what makes it read the next. A symbol list's rows need none, so
// the loop below is a no-op there and the block is its only read. Outside the
// anonymous namespace for the excerpt-context keys (commands.cpp).
void PickerFillShown(Editor& ed, PickerState& state) {
  state.context.clear();
  state.context_first = 0;
  state.context_target = 0;
  if (!PickerShowsContext(state.source)) return;

  // The band's rows read nothing for content -- a row is the corpus line the
  // scan sent -- so only the block reads, and it reads the file: the corpus
  // holds the matched lines and not their neighbours.
  const bool content = (state.source == PickerState::Source::kContent);
  if (!content) {
    const std::size_t window = std::max<std::size_t>(1, state.window);
    const std::size_t last = std::min(state.shown.size(), state.offset + window);
    for (std::size_t r = state.offset; r < last; ++r) {
      PickerReadRow(ed, state, state.rows[state.shown[r]], true);
    }
  }
  PickerTarget target;
  if (!PickerRowTarget(state, state.selected, target)) return;
  // The block's depth is the excerpt view's setting: excerpt-context lines
  // either side of the target, one knob for both surfaces. A side too short
  // for all of them shows the nearest (FitPromptBox clamps, DrawPickerContext
  // windows around the target).
  const Index around = std::max<Index>(0, ed.settings.excerpt_context);
  PickerLinesOf(ed, state, target.path, target.line - around, (2 * around) + 1,
                state.context_first, state.context);
  state.context_target = target.line;
}

namespace {

// Symbol rows for the band: where the symbol is, dimmed at the card's right
// edge. What leads the row is the source's business (PickerRowIsName): the
// name for a symbol list, settled here, or the line's own content for defs and
// refs, read lazily.
PickerEntry SymbolPickerEntry(Symbol one, PickerState::Source source) {
  PickerEntry row;
  row.detail = one.path + ":" + std::to_string(one.line);
  row.line = one.line;
  row.column = one.column;
  row.name = std::move(one.name);
  row.target = std::move(one.path);
  // A symbol list's row is its name. Defs and refs lead with the line, which
  // the scan captured for them -- so a filter pass over the whole list reads
  // nothing, and only the handful of rows the band is showing go back to the
  // file to say what it says now. A scan that came without a line (a row parsed
  // back off a child's output) falls back to the name, so a row always has
  // something to show and something to be judged by.
  row.text = (PickerRowIsName(source) || one.text.empty()) ? row.name : std::move(one.text);
  return row;
}

std::vector<PickerEntry> SymbolPickerEntries(std::vector<Symbol> found,
                                             PickerState::Source source) {
  std::vector<PickerEntry> rows;
  rows.reserve(found.size());
  for (Symbol& one : found) rows.push_back(SymbolPickerEntry(std::move(one), source));
  return rows;
}

// What the branch row calls a row once the band is gone. A path with the line
// worth naming, the way smart-jump names a destination -- except a buffer row,
// whose target is an index into the buffer list, so the band's own text stands.
std::string WalkLabel(PickerState::Source source, const PickerEntry& row) {
  if (source == PickerState::Source::kBuffers) return row.text;
  // Files carry ":line" as their detail only where the store knows the file;
  // an unvisited one is its path and nothing else, as on the band.
  if (source == PickerState::Source::kFiles) {
    return row.detail.empty() ? row.target : (row.target + ":" + std::to_string(row.line));
  }
  return row.target + ":" + std::to_string(row.line);
}

// One shown row copied out for the walk, resolved per source. Content parses its
// `path:line` off the corpus line here and nowhere else: what survives the
// prompt owns its strings outright, because the corpus does not survive it. A
// content hit is a place, not a symbol, so it records no name. False for a
// corpus line with no parseable head -- it names nowhere, so it is no step.
bool PickerWalkRow(const PickerState& state, std::size_t at, WalkRow& out) {
  if (state.source == PickerState::Source::kContent) {
    PickerTarget target;
    if (!PickerRowTarget(state, at, target)) return false;
    const std::string label = target.path + ":" + std::to_string(target.line);
    out = WalkRow{std::move(target.path), label, {}, target.line, target.column};
    return true;
  }
  const PickerEntry& row = state.rows[state.shown[at]];
  out = WalkRow{row.target, WalkLabel(state.source, row), row.name, row.line, row.column};
  return true;
}

// Both defined with the walk at the bottom of this file, where the smart-jump
// wording they share lives.
bool OpenWalkRow(Editor& ed, PickerState::Source source, const WalkRow& row);
void SayWalk(Editor& ed, const WalkList& list, bool forward, bool wrapped);

// A pattern that will not compile keeps the last good list -- and on the open
// itself there is no last good anything: the card has never been measured, and
// a card of zero is drawn as its connector and its digit with no room left for
// a row. So the list the open made stands, and it gets a width. Only ever the
// first pass: every later one is measured already, and remeasuring here would
// resize the card under a half-typed pattern.
void PickerBadPatternCard(Editor& ed, PickerState& state) {
  if (state.card_w != 0) return;
  PickerFillShown(ed, state);
  state.card_w = PickerCardWidth(state);
  state.lead_w = 0;
  state.detail_w = 0;
}

// Opens the picker prompt over `rows`. A non-empty query arrives typed in: what
// the smart-jump handoff and last_picker carry is a filter, and what defs and
// refs carry is the word the hits were found for -- the search term, which
// belongs in the search box where it can be edited.
void OpenPicker(Editor& ed, PickerState::Source source, std::vector<PickerEntry> rows,
                std::string_view query, std::unique_ptr<PickerScan> scan = nullptr) {
  auto state = std::make_shared<PickerState>();
  state->source = source;
  state->rows = std::move(rows);
  state->query = query;
  // The setting is read here and never again while this prompt lives, so the
  // cache's per-lookup test is against a member and a :config-reload under an
  // open picker changes what the next one reads, not this one.
  state->file_max = ed.settings.picker_file_max;
  // Before the filter pass below, which is what tells a scan the pattern its
  // rows will be judged by as they land.
  state->scan = std::move(scan);
  ShowAllPickerRows(*state);
  ed.picker = std::move(state);
  PromptOpen(ed, PromptKind::kPicker);
  if (!query.empty()) {
    ed.prompt_input = query;
    ed.prompt_cursor = ed.prompt_input.size();
  }
  PickerRefilter(ed);
  // After the filter pass and not before it, so the query recorded is one that
  // compiled. A stored pattern that throws would otherwise be stored again by
  // the reopen it broke, and last_picker would keep handing it back.
  WriteLastPicker(PickerSourceName(source),
                  PickerQueryIsTyped(source) ? std::string_view{ed.picker->good_input} : query);
}

}

void PickerRefilter(Editor& ed) {
  if (!ed.prompt_active || (ed.prompt_kind != PromptKind::kPicker) || !ed.picker) return;
  PickerState& state = *ed.picker;
  // An input that did not change rebuilds nothing. Delete at the end of the
  // input and ctrl-u at column 0 are keys that edit nothing and still land
  // here, and what a rebuild costs is the selection: the list is made again and
  // the band goes back to its top row. What is said stays said too, so a
  // half-typed pattern's reason survives a key that did not finish it.
  if (state.last_input && (*state.last_input == ed.prompt_input)) return;
  state.last_input = ed.prompt_input;
  ed.status.clear();
  // The word defs and refs open with is a search term, not a filter: every row
  // was found for it, so a pass over it can only drop rows it should keep -- a
  // line that spells the name differently, or a name that is not a pattern
  // (`operator[]`). The band opens whole and narrows from the next keystroke.
  const bool content = (state.source == PickerState::Source::kContent);
  if (ed.prompt_input.empty() ||
      (!PickerQueryIsTyped(state.source) && (ed.prompt_input == state.query))) {
    // Nothing was compiled and nothing had to be: an empty filter is the whole
    // list, and a source whose query is not typed opened with the word it holds.
    // Either way it is a query that can be reopened.
    state.good_input = ed.prompt_input;
    if (content) {
      PickerStartRefilter(state, {});
      return;
    }
    ShowAllPickerRows(state);
    if (state.scan) state.scan->filter.clear();
    PickerFillShown(ed, state);
    state.card_w = PickerCardWidth(state);
    state.lead_w = 0;
    state.detail_w = 0;
    return;
  }
  // Compiled fresh each keystroke, and always JIT: content's corpus is
  // megabytes, a streamed list has no ceiling on its rows, and the second
  // compile is tens of microseconds -- never worth branching over. (?i)
  // because what is typed into a band is a filter, not a spelling test.
  std::optional<gai::Pcre2Regex> regex;
  try {
    regex.emplace(gai::Regex(gai::Compile("(?i)" + ed.prompt_input, true, true)));
  } catch (const std::exception& e) {
    // A half-typed pattern keeps the last good list on the band, and the
    // branch row says why instead of flashing the band empty.
    ed.status.Warn(OneLineReason(e.what()));
    PickerBadPatternCard(ed, state);
    return;
  } catch (...) {
    ed.status.Warn("bad pattern");
    PickerBadPatternCard(ed, state);
    return;
  }
  // Written down only now, so what last_picker records is a query that
  // compiled: one that threw filtered nothing, and reopening on it would throw
  // again on the way in.
  state.good_input = ed.prompt_input;
  // Only a pattern that compiled is carried, so rows landing under a half-typed
  // one are filtered by the list's last good pattern -- the band keeps saying
  // what it said, and keeps growing by the same rule. Content puts its pattern
  // down for the pump instead: the corpus is megabytes, and a keystroke that
  // matched all of them before it drew is the one thing this cannot do.
  if (content) {
    PickerStartRefilter(state, ed.prompt_input);
    return;
  }
  if (state.scan) state.scan->filter = ed.prompt_input;
  std::vector<std::size_t> shown;
  // Every row already owns the text it is judged by -- its name for a symbol
  // list, the line the scan captured for defs and refs -- so this sweep touches
  // no file at all. It used to read one per row that had never been on the
  // band: a list of a thousand references is a thousand files read whole,
  // inside one keystroke, with nothing bounding what any of them cost.
  for (std::size_t i = 0; i < state.rows.size(); ++i) {
    // A match-time failure on one row is that row unproven, not the pattern
    // bad: a non-match, and the sweep goes on.
    if (gai::Find(*regex, PickerRowText(state, state.rows[i]))) shown.push_back(i);
  }
  state.shown = std::move(shown);
  state.selected = 0;
  state.offset = 0;
  PickerFillShown(ed, state);
  state.card_w = PickerCardWidth(state);
  state.lead_w = 0;
  state.detail_w = 0;
}

void PickerStep(Editor& ed, bool forward) {
  if (!ed.prompt_active || (ed.prompt_kind != PromptKind::kPicker) || !ed.picker) return;
  PickerState& state = *ed.picker;
  // The whole filtered list, wrapping at its ends and not at the window's: the
  // band is five rows of a list that is as long as it is.
  const std::size_t count = state.shown.size();
  if (count == 0) return;
  state.selected =
      forward ? ((state.selected + 1) % count) : ((state.selected + count - 1) % count);
  PickerScrollToSelected(state);
  // The block follows the selection, and a window that scrolled brought rows
  // onto the band that have not read their line yet.
  PickerFillShown(ed, state);
}

void PickerAccept(Editor& ed, int at) {
  if (!ed.prompt_active || (ed.prompt_kind != PromptKind::kPicker) || !ed.picker) return;
  // The rows the band drew, which is none of them when the screen had no room
  // for a band at all: a digit names a row that is on it, so with no band it
  // names nothing. Enter is still the selection -- what the query in the box
  // was typed for, and a filter leaves the selection on the list's head.
  const std::size_t window = ed.picker->window;
  const std::size_t last = std::min(ed.picker->shown.size(), ed.picker->offset + window);
  // A digit names a row of the window wherever it is scrolled; enter takes the
  // selection, which the scroll keeps inside it.
  const std::size_t pick =
      (at < 0) ? ed.picker->selected : (ed.picker->offset + static_cast<std::size_t>(at));
  // A digit past the band's end, or enter over an empty band: nothing to open,
  // and the prompt stays for the pattern to be fixed.
  if ((pick >= ed.picker->shown.size()) || ((at >= 0) && (pick >= last))) return;
  // A corpus line with no `path:line:` head names nowhere, so there is nothing
  // to open and no walk to build around it: said, and the prompt stays for the
  // pattern to be fixed. Resolved here so the copy-out below can take the pick
  // being in the list it keeps.
  WalkRow picked;
  if (!PickerWalkRow(*ed.picker, pick, picked)) {
    ed.status.Warn("no path in that row");
    return;
  }
  // Out of the editor before PromptCancel frees what is left there.
  const std::shared_ptr<PickerState> taken = std::move(ed.picker);
  // The last query that compiled, not what is in the box: accepting a row out
  // of the last good list with a half-typed pattern still in the prompt must
  // not write that pattern down as the way back to this list.
  WriteLastPicker(PickerSourceName(taken->source),
                  PickerQueryIsTyped(taken->source) ? taken->good_input : taken->query);

  // The filtered list in the order the band showed it, copied into rows the
  // walk owns outright: the prompt's state dies below, and what survives it may
  // not point back into a line cache or a corpus. Capped, so accepting out of a
  // huge list keeps a walk and not the pick.
  auto walk = std::make_shared<WalkList>();
  walk->source = taken->source;
  const std::size_t keep = std::min(taken->shown.size(), kWalkRows);
  // The cap is a window around the pick, not the head of the list: picking the
  // three thousandth row must still leave somewhere to step back to.
  const std::size_t from =
      std::min(pick - std::min(pick, keep / 2), taken->shown.size() - keep);
  walk->rows.reserve(keep);
  // Rows that name nowhere are left out rather than stepped onto, so `at`
  // follows the rows that stayed: the pick is one of them, checked above.
  for (std::size_t r = from; r < (from + keep); ++r) {
    if (r == pick) {
      walk->at = walk->rows.size();
      walk->rows.push_back(std::move(picked));
      continue;
    }
    WalkRow row;
    if (!PickerWalkRow(*taken, r, row)) continue;
    walk->rows.push_back(std::move(row));
  }
  // The walk holds what had arrived, so the scan has nothing left to say: ended
  // here rather than left to `taken` going out of scope, so the child is gone
  // before the pick opens anything. What it had already sent is kept.
  taken->scan.reset();

  PromptCancel(ed);
  // Whichever list was made last owns n/N, so smart-jump's goes here -- its
  // corpus belongs to its own prompt, but the matches are the list this
  // replaces, and a freed one cannot be stepped into by accident.
  if (ed.smart_jump) {
    ed.smart_jump->matches.clear();
    ed.smart_jump->matches.shrink_to_fit();
    ed.smart_jump->typed.clear();
    ed.smart_jump->at = 0;
  }
  ed.walk = std::move(walk);
  // Copied, not referenced: the open runs arbitrary editor machinery, and the
  // rows are what it could move.
  const WalkRow row = ed.walk->rows[ed.walk->at];
  if (!OpenWalkRow(ed, ed.walk->source, row)) {
    // Nothing arrived, so nothing is said about where it arrived: what the open
    // wrote stands, and no branch row is drawn over it. The walk is kept -- the
    // list is still good, and stepping off the bad row is the way out.
    ed.jump_branch = false;
  } else if (ed.walk->rows.size() == 1) {
    // A lone row lands in silence, as a lone smart-jump match does: the cursor
    // arriving is the whole answer.
    ed.status.clear();
    ed.jump_branch = false;
  } else {
    SayWalk(ed, *ed.walk, true, false);
  }
}

void BufferPicker(Editor& ed, std::string_view query) {
  if (BufferCount(ed) <= 1) {
    ed.status.Warn("only one buffer open");
    return;
  }
  std::vector<PickerEntry> rows;
  for (BufferRow& row : BufferPickerRows(ed)) {
    rows.push_back(PickerEntry{std::move(row.text), {}, std::move(row.payload), 1, 1});
  }
  OpenPicker(ed, PickerState::Source::kBuffers, std::move(rows), query);
}

void FilePicker(Editor& ed, std::string_view query) {
  std::vector<PickerEntry> rows;
  for (RankedFile& file : RankedFiles(ed)) {
    PickerEntry row;
    row.detail = file.visited ? (":" + std::to_string(file.line)) : std::string{};
    row.line = file.line;
    row.column = file.column;
    row.text = std::move(file.path);
    row.target = row.text;
    rows.push_back(std::move(row));
  }
  OpenPicker(ed, PickerState::Source::kFiles, std::move(rows), query);
}

void ContentPicker(Editor& ed, std::string_view query) {
  // gai's own `path:line:text`, streamed. The query does not ride the command:
  // the child sends every line it finds and the band filters what lands, so a
  // keystroke never restarts the scan.
  std::ignore = PickerStartScan(ed, PickerState::Source::kContent,
                                ExpandVariables(PickerScanCommand(ed, "content"), ed), query);
}

namespace {

// Ends a child koi owns -- signal the group, reap it, close the pipe. Returns
// the waitpid status, or -1 when there was nothing to reap or the reap gave up:
// the child's stderr goes to /dev/null by design, so for a scan that produced
// nothing this status is the only surviving word on why. Defined with the
// command jobs below, which end theirs the same way and are where the unreapable
// ones are swept from.
int EndChild(int& pid, int& fd);

// Forks `command` under $SHELL with its stdout on a non-blocking pipe, in a
// process group of its own so ending it ends the whole pipeline and not just
// the last stage. A command job wants the child's stderr on the pipe too --
// its diagnostics are half of what it is showing -- while a picker scan does
// not: a warning from the file filter would arrive on the band as a row.
bool SpawnPipedChild(const std::string& command, bool stderr_to_pipe, int& pid, int& fd,
                     std::string& why) {
  int fds[2] = {-1, -1};
  if (pipe2(fds, O_CLOEXEC) != 0) {
    why = std::strerror(errno);
    return false;
  }
  const pid_t forked = fork();
  if (forked < 0) {
    why = std::strerror(errno);
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  if (forked == 0) {
    setpgid(0, 0);
    // A child that cannot set up its stdio does not run: closing fd 0 instead
    // would leave the next open landing on it, and with no /dev/null for fd 2 a
    // warning from the pipeline would be written over the drawn screen. 127 is
    // the exec failure below, which is the same news -- the command did not run.
    const int null = open("/dev/null", O_RDWR);
    if (null < 0) _exit(127);
    dup2(null, 0);
    if (!stderr_to_pipe) dup2(null, 2);
    if (null > 2) close(null);
    dup2(fds[1], 1);
    if (stderr_to_pipe) dup2(fds[1], 2);
    const char* shell = std::getenv("SHELL");
    if ((shell == nullptr) || (*shell == '\0')) shell = "/bin/sh";
    execl(shell, shell, "-c", command.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  setpgid(forked, forked);
  close(fds[1]);
  if (fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0) {
    // Read from the editor thread, so a blocking pipe is the editor waiting on
    // whatever the child feels like writing. Nothing has been handed back yet:
    // end what was just spawned and fail like the arms above.
    why = std::strerror(errno);
    int spawned = static_cast<int>(forked);
    EndChild(spawned, fds[0]);
    return false;
  }
  pid = static_cast<int>(forked);
  fd = fds[0];
  return true;
}

}

// The one cleanup path there is: whatever frees the state ends the child.
PickerScan::~PickerScan() { EndChild(pid, fd); }

// A catch-up counts as live: the child can be reaped and the pipe closed while a
// pattern still has megabytes of corpus to meet, and until it has, the band is
// showing a list that is not the answer to what is typed.
bool PickerScanLive(const PickerState& state) {
  return state.scan && (!state.scan->done || state.scan->refiltering);
}

bool PickerScanning(const Editor& ed) {
  return ed.prompt_active && (ed.prompt_kind == PromptKind::kPicker) && ed.picker &&
         PickerScanLive(*ed.picker);
}

std::string_view PickerCountNote(const PickerState& state) {
  if (PickerScanLive(state)) return kPickerScanNote;
  if (state.scan && state.scan->truncated) return kPickerCutNote;
  return {};
}

namespace {

// The pattern blew a match-time limit, which is the pattern's doing and not the
// line's: every line after it would blow it too, so the pass ends on the first
// one rather than paying the limit out over a hundred thousand of them. The band
// keeps the list it had -- and picks its own pattern up again, because bytes
// landed while this one was being carried and the pump left them to the
// catch-up. A live pattern that blows is the one case nothing can be picked up
// again, so that one stops.
void PickerFailRefilter(Editor& ed, PickerScan& scan, const std::string& why) {
  ed.status.Warn(OneLineReason(why));
  const bool was_live = (scan.refilter_pattern == scan.filter);
  scan.refilter_pattern = scan.filter;
  scan.refilter_at = 0;
  scan.refilter_shown.clear();
  scan.refiltering = !was_live;
}

// One wake of a query change: at most kPickerMatchBudget bytes of the parsed
// corpus met by the new pattern, resuming where the last wake stopped. An empty
// pattern is the empty query, which every non-empty line meets -- empty lines
// are not rows, they carry no path:line, and the pump does not count them
// either, so an empty query shows n out of n. True when the list changed hands.
bool PickerCatchUp(Editor& ed, PickerState& state, PickerScan& scan) {
  if (!scan.refiltering) return false;
  std::optional<gai::Pcre2Regex> regex;
  if (!scan.refilter_pattern.empty()) {
    try {
      // JIT from the start: this runs over a corpus of megabytes, where
      // compiling twice costs less than matching once without it.
      regex.emplace(gai::Regex(gai::Compile("(?i)" + scan.refilter_pattern, true, true)));
    } catch (...) {
      // Only a pattern that already compiled is ever put down, so this is
      // unreachable; reached, the catch-up is dropped and the last good list
      // stands rather than being replaced by a list nobody asked for.
      scan.refiltering = false;
      scan.refilter_shown.clear();
      return false;
    }
  }
  const std::string_view corpus = PickerCorpus(state);
  const std::size_t limit = std::min(scan.parsed, scan.refilter_at + kPickerMatchBudget);
  std::string error;
  while (scan.refilter_at < limit) {
    const std::size_t at = scan.refilter_at;
    const std::size_t eol = std::min(corpus.find('\n', at), corpus.size());
    std::string_view line = corpus.substr(at, eol - at);
    scan.refilter_at = eol + 1;
    while (!line.empty() && (line.back() == '\r')) line.remove_suffix(1);
    if (line.empty()) continue;
    if (regex && !gai::Find(*regex, line, &error)) {
      // A limit failure is not a non-match: reading it as one drops rows in
      // silence, which is a filter that lies about what is in the corpus.
      if (!error.empty()) {
        PickerFailRefilter(ed, scan, error);
        return false;
      }
      continue;
    }
    scan.refilter_shown.push_back(at);
  }
  // Everything the pump has parsed -- the flushed tail included, because pipe
  // close moves `parsed` to the end of the corpus and this chases `parsed`.
  if (scan.refilter_at < scan.parsed) return false;
  state.shown = std::move(scan.refilter_shown);
  scan.refilter_shown.clear();
  scan.filter = std::move(scan.refilter_pattern);
  scan.refilter_pattern.clear();
  scan.refilter_at = 0;
  scan.refiltering = false;
  // The one moment a shown row moves: a new list is not the old one scrolled,
  // so the selection goes back to the top the way a keystroke's did.
  state.selected = 0;
  state.offset = 0;
  PickerFillShown(ed, state);
  state.card_w = PickerCardWidth(state);
  state.lead_w = 0;
  state.detail_w = 0;
  return true;
}

}

bool PickerPumpScan(Editor& ed) {
  if (!ed.prompt_active || (ed.prompt_kind != PromptKind::kPicker) || !ed.picker) return false;
  PickerState& state = *ed.picker;
  if (!state.scan) return false;
  PickerScan& scan = *state.scan;
  // A finished scan still wakes while a catch-up is mid-corpus: the pattern has
  // the rest of the bytes to meet, and the band is showing the last good list
  // until it has.
  if (scan.done && !scan.refiltering) return false;

  const bool content = (state.source == PickerState::Source::kContent);
  const std::size_t was_rows = state.rows.size();
  const std::size_t was_lines = scan.lines;
  const std::size_t was_shown = state.shown.size();
  const std::size_t was_selected = state.selected;
  // Two ways a stream stops, and the tail is the difference. `ended` is the
  // child having said everything: the half-line at `parsed` is the last row and
  // is flushed. `stopped` is the stream being cut -- a read that failed, or the
  // corpus reaching its ceiling -- where that half-line is half of something,
  // and committing it would put a chopped path:line:text on the band.
  bool ended = false;
  bool stopped = false;
  bool failed = false;

  if (!scan.done) {
    ended = (scan.fd < 0);
    if (scan.fd >= 0) {
      // A budget of this wake's own, well under ReadAvailable's ceiling: what
      // the pipe still holds waits for the next wake, the partial tail carries
      // as it always did, and what is parsed and matched below is bounded by
      // what was taken here. Nothing is kept but the byte offset, which is why a
      // mapping that relocated as it grew costs nothing.
      const common::ReadProgress got = scan.out.ReadAvailable(scan.fd, kPickerWakeBudget);
      // EOF and a failed read are not the same answer. A read that failed, or a
      // mapping that would not grow, leaves a corpus that stops mid-project: the
      // rows it did yield stay, and the reader is told, rather than being handed
      // a partial list with the note dropped as if it were the whole one.
      ended = got.eof;
      failed = got.error;
    }
    // The corpus is the row storage, so the project's text is the editor's
    // memory. What landed is kept and the child is ended here; past this nothing
    // is read, and the count says so.
    if (!ended && !failed && (scan.out.size >= scan.corpus_max)) scan.truncated = true;
    stopped = failed || scan.truncated;

    const std::string_view text{(scan.out.buffer == nullptr) ? "" : scan.out.buffer, scan.out.size};
    // The live pattern, compiled once per wake and only where a content line is
    // there to judge: content is filtered as it is parsed, because the corpus is
    // the row storage and there is no second pass over "the new rows" to make.
    std::optional<gai::Pcre2Regex> live;
    bool compiled = false;
    while (scan.parsed < text.size()) {
      // From the high-water mark, not from `parsed`: inside a stretch with no
      // newline in it the two differ, and searching the same megabytes again on
      // every read is what makes one enormous line quadratic.
      const std::size_t eol = text.find('\n', std::max(scan.parsed, scan.searched));
      // Half a row is not a row: it waits for the next read. Pipe close is what
      // flushes an unterminated tail, because nothing more is coming for it.
      if ((eol == std::string_view::npos) && !ended) {
        scan.searched = text.size();
        break;
      }
      const std::size_t end = (eol == std::string_view::npos) ? text.size() : eol;
      const std::size_t from = scan.parsed;
      std::string_view line = text.substr(scan.parsed, end - scan.parsed);
      scan.parsed = (eol == std::string_view::npos) ? text.size() : (eol + 1);
      while (!line.empty() && (line.back() == '\r')) line.remove_suffix(1);
      if (line.empty()) continue;
      if (content) {
        // A content row is where it starts, and nothing else is kept: the whole
        // line stays in the corpus and is read back from `from` whenever
        // anything wants it.
        ++scan.lines;
        // A catch-up is carrying a pattern up towards `parsed`, and these bytes
        // are ahead of it. Judging them here would judge them by the pattern the
        // band is about to stop showing, and the catch-up would judge them
        // again: the line structure is all this loop takes from them, and the
        // catch-up is what decides whether they are rows. Each byte meets each
        // query exactly once.
        if (scan.refiltering) continue;
        if (!scan.filter.empty()) {
          if (!compiled) {
            compiled = true;
            try {
              live.emplace(gai::Regex(gai::Compile("(?i)" + scan.filter, true, true)));
            } catch (...) {
              live.reset();
            }
          }
          // Only a pattern that already compiled is stored, so a reset `live` is
          // unreachable; reached, the line stays off the band rather than
          // joining it unjudged, and the next keystroke rescans it with the
          // rest.
          std::string error;
          if (!live) continue;
          if (!gai::Find(*live, line, &error)) {
            if (error.empty()) continue;
            // A match-time limit is the pattern's doing and not the line's: the
            // rest of what the child is still writing would blow it too, a line
            // at a time, for as long as the scan runs. So the pattern is
            // retired, said out loud, and the band goes back to every line --
            // the query is still in the box, and the next keystroke compiles a
            // new one over it.
            ed.status.Warn(OneLineReason(error));
            scan.filter.clear();
            PickerStartRefilter(state, {});
            continue;
          }
        }
        state.shown.push_back(from);
        continue;
      }
      Symbol one;
      // A `--picker-rows` row: display, padding, tab, payload. The payload is
      // the whole row koi can open; the display half is the child's own
      // formatting.
      if (!ParseSymbolRow(RowPayload(line), one)) continue;
      state.rows.push_back(SymbolPickerEntry(std::move(one), state.source));
    }
  }

  if (state.rows.size() > was_rows) {
    std::optional<gai::Pcre2Regex> regex;
    const bool filtering = !scan.filter.empty();
    if (filtering) {
      try {
        regex.emplace(gai::Regex(gai::Compile("(?i)" + scan.filter, true, true)));
      } catch (...) {
        // Only a pattern that already compiled is stored, so this is unreachable
        // -- and if it is ever reached, the new rows stay off the band rather
        // than joining it unjudged. The next keystroke filters them with the
        // rest.
        regex.reset();
      }
    }
    for (std::size_t i = was_rows; i < state.rows.size(); ++i) {
      if (filtering) {
        if (!regex) continue;
        // A project symbol row is its name and was its name the moment it
        // parsed, so a row lands, is judged and joins the band without a file
        // being opened for it.
        PickerReadRow(ed, state, state.rows[i], false);
        if (!gai::Find(*regex, PickerRowText(state, state.rows[i]))) continue;
      }
      // Appended, never inserted and never sorted: the hot head the child sent
      // first stays first, every shown index keeps its meaning, and the
      // selection is where the eyes left it.
      state.shown.push_back(i);
    }
  }

  if (state.shown.size() > was_shown) {
    // Growth only, and only over what just landed. A card that shrank mid-scan
    // would reflow the list under the reader; a keystroke recomputes it whole.
    // lead_w is not cleared here for the same reason: rows appending is not the
    // filter changing, and clearing it every wake is the wobble it exists to
    // stop. The renderer only ever grows it.
    state.card_w = std::max(state.card_w, PickerCardWidthFrom(state, was_shown));
    // The rows just appended start at was_shown and run to the end, so one of
    // them is on the band only if was_shown is inside the window. Below it they
    // change nothing the window shows, and refilling for them would be five or
    // six stats every wake for the whole life of the scan.
    // kPickerRows, not state.window: window is what the band drew last frame,
    // which is short while the list is shorter than the band, and the rows that
    // just landed are what the next frame grows it with.
    const std::size_t window = std::max(state.window, kPickerRows);
    if ((was_shown < (state.offset + window)) || (state.selected != was_selected)) {
      PickerFillShown(ed, state);
    }
  }

  if (ended || stopped) {
    scan.done = true;
    // The pipe is closed, so the child is done writing; ended here rather than
    // left for the prompt, so a finished scan is not a process still around.
    // What is not ended here is the scan state: a catch-up may still have the
    // corpus to walk, and it walks it out of the same object.
    const int status = EndChild(scan.pid, scan.fd);
    // Content counts lines, because the corpus is the rows and there are none to
    // count; every other source counts rows.
    const bool nothing = content ? (scan.lines == 0) : state.rows.empty();
    if (failed) {
      ed.status.Warn("scan read failed; the list is partial");
    } else if (nothing && (status != -1) && WIFEXITED(status) && (WEXITSTATUS(status) != 0)) {
      // An empty band and a child that failed: the child's stderr goes to
      // /dev/null by design, so its exit code is the only thing left that knows
      // why -- a missing program, a file_filter that will not run, a shell that
      // could not parse the command. Without this a broken scan is a clean 0/0.
      ed.status.Warn((WEXITSTATUS(status) == 127)
                         ? std::string{"a program in the scan command was not found"}
                         : ("the scan exited " + std::to_string(WEXITSTATUS(status))));
    }
  }
  // After the parse, so a catch-up chases `parsed` and reaches it -- with the
  // read budget under the match budget it gains on a live scan, and on a
  // finished one it is all there is left to do.
  const bool caught = PickerCatchUp(ed, state, scan);
  return (state.rows.size() > was_rows) || (scan.lines > was_lines) || ended || stopped || caught;
}

bool PickerStartScan(Editor& ed, PickerState::Source source, const std::string& command,
                     std::string_view query) {
  if (command.empty()) {
    ed.status.Warn("nothing to scan");
    return false;
  }
  auto scan = std::make_unique<PickerScan>();
  // Same rule as the file ceiling: read once, at the spawn, so the pump's
  // per-wake test costs a member load and a reload lands on the next scan.
  scan->corpus_max = ed.settings.picker_corpus_max;
  std::string why;
  if (!SpawnPipedChild(command, false, scan->pid, scan->fd, why)) {
    ed.status.Fail("cannot start the scan: " + why);
    return false;
  }
  OpenPicker(ed, source, {}, query, std::move(scan));
  // Whatever is already there before the first frame: a scan that answers at
  // once opens with rows rather than with an empty band.
  PickerPumpScan(ed);
  return true;
}

void SymbolPicker(Editor& ed, std::string_view query) {
  // The file filter into koi's own symbol mode, with the prompt as the
  // consumer. The query filters what lands, so it rides nothing here either.
  std::ignore = PickerStartScan(ed, PickerState::Source::kProjectSymbols,
                                ExpandVariables(PickerScanCommand(ed, "symbols"), ed), query);
}

void BufferSymbolPicker(Editor& ed, std::string_view query) {
  if (!HasDiskFile(ed.doc)) {
    ed.status.Warn("no file -- :w <path> first");
    return;
  }
  // The scan the `koi --symbol-mode` subprocess ran on this one file, run here
  // instead: koi is already linked against the grammars it would have loaded.
  const std::array<std::string, 1> only{ed.doc.file.string()};
  std::string error;
  std::vector<Symbol> found = CollectSymbols(only, SymbolKind::kDefinitions, error);
  const std::string note = error.empty() ? std::string{} : (" (" + error + ")");
  if (found.empty()) {
    ed.status.Warn("no definitions in " + DisplayPath(ed.doc.file) + note);
    return;
  }
  // Hot head first, the way the project scan orders its rows: what this
  // project's database ranks highest leads, the rest keeps the file's order.
  // Every row is in the current file, so the ranking keeps all of them and
  // there is no unranked tail to sweep in behind it.
  if (ed.project != nullptr) std::ignore = ed.project->RankSymbols(found, CurrentFile(ed));
  OpenPicker(ed, PickerState::Source::kFileSymbols,
             SymbolPickerEntries(std::move(found), PickerState::Source::kFileSymbols), query);
  // Said after the prompt is up, because a partial list that says nothing
  // reads as a complete one; the first keystroke into the band clears it.
  if (!note.empty()) ed.status = "definitions in " + DisplayPath(ed.doc.file) + note;
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

  // Said in every outcome the scan has, the way the job path says it
  // (PresentCommandJob's `note`): "no definition for X" with a grammar quietly
  // unloaded behind it reads as proof there is no definition, and a *partial*
  // result set with symbols missing from a whole language reads as a complete
  // one. The picker path carries it through to whatever the pick opens.
  std::string error;
  std::vector<Symbol> found = FindSymbols(ed, word, kind, error);
  const std::string note = error.empty() ? std::string{} : (" (" + error + ")");
  if (found.empty()) {
    ed.status.Warn("no " + std::string{what} + " for " + word + note);
    return;
  }

  if (found.size() == 1) {
    const Symbol& one = found.front();
    RecordSymbolPick(ed, one.name, one.path, one.line);
    OpenAt(ed, one.path, one.line, one.column);
    if (!note.empty()) ed.status += note;
    return;
  }

  // The rows are already in hand and already hot-head-first: onto the band in
  // that order, with no child and nothing to parse back.
  const PickerState::Source source =
      (name == "definition") ? PickerState::Source::kDefs : PickerState::Source::kRefs;
  OpenPicker(ed, source, SymbolPickerEntries(std::move(found), source), word);
  if (!note.empty()) ed.status = "found " + std::string{what} + "s for " + word + note;
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
  // A ranking alphabetised is not a ranking, and neither is a list of pins
  // sorted by path. Every other kind is a set of matches, where alphabetical is
  // the only order that means anything.
  const bool ordered_by_refs = (out.kind == ExcerptView::Kind::kPins);
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
//
// The line is where the file was last left, not a line the user chose, so this
// view drifts with them: every visit and every edit moves it, and an open
// buffer reports its live cursor. Which is why the pins view is rebuilt from
// here rather than kept -- see RefreshLiveExcerptViews, and the
// MarkLiveViewsStale in RecordHere that tells it something moved.
std::vector<ExcerptRef> PinRefs(Editor& ed) {
  std::vector<ExcerptRef> refs;
  if (!ed.project) return refs;
  const std::vector<Pin> pins = ed.project->Pins();
  const fs::path root = ProjectRoot();
  for (std::size_t at = 0; at < pins.size(); ++at) {
    const Pin& pin = pins[at];
    if (pin.path.empty()) continue;
    const std::string path = ResolveStorePath(root, pin.path);
    Index line = pin.line;
    Index column = pin.column;
    std::ignore = LiveCursorIn(ed, path, line, column);
    refs.push_back(
        ExcerptRef{path, std::max<Index>(1, line), 0, "pin " + std::to_string(at + 1)});
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

// Shared with the picker's streaming scan, which owns its child the same way a
// command job owns one and must end it on every path the prompt can close.
int EndChild(int& pid, int& fd) {
  int status = -1;
  if (pid > 0) {
    // Before the signals, because after them a child that had already exited on
    // its own would be indistinguishable from one koi killed. Reaping the leader
    // does not dissolve the group while a grandchild is still in it, so the kill
    // below still ends the whole pipeline.
    int exited = 0;
    const bool already = (waitpid(pid, &exited, WNOHANG) == pid);
    if (already) status = exited;
    // TERM first and the hammer a nap later, from inside the loop: the two sent
    // back to back are just the SIGKILL, and a child with a handler never gets
    // to run it. Nothing is waited for once the leader is reaped, so there is no
    // nap to hang the escalation on -- what is left of the group is killed at
    // once rather than costing every finished scan five milliseconds.
    kill(-pid, SIGTERM);
    if (already) kill(-pid, SIGKILL);
    // Bounded, not blocking. SIGKILL is not deliverable to a process in
    // uninterruptible sleep -- a child blocked on a stalled NFS or FUSE mount,
    // say -- and this runs on the editor thread, from :from-cancel and from
    // quit, with the terminal in raw mode and nothing drawing. An unbounded
    // wait there froze the editor with no way out.
    //
    // A child that took a signal is reapable within microseconds of it, so the
    // loop below almost always ends a turn or two after whichever one landed;
    // the deadline is only what stops the pathological case. Whatever
    // is left is a zombie, swept by ReapAbandoned on a later pump, and
    // reparented to init if koi exits first.
    constexpr int kReapAttempts = 20;
    constexpr long kReapNapNs = 5L * 1000 * 1000;  // 5 ms; 100 ms in total
    bool reaped = already;
    for (int attempt = 0; (attempt < kReapAttempts) && !reaped; ++attempt) {
      // One nap was the grace; past it the child is not handling anything.
      if (attempt == 1) kill(-pid, SIGKILL);
      int killed = 0;
      const pid_t got = waitpid(pid, &killed, WNOHANG);
      // A pid nobody can wait for is as ended as one that was waited for, but it
      // says nothing about how: only a real reap carries a status.
      if (got == pid) status = killed;
      reaped = (got == pid) || ((got < 0) && (errno != EINTR));
      if (reaped) break;
      // Against the remainder: no handler koi installs sets SA_RESTART, so a
      // resize drag would otherwise cut every nap short and spend the whole
      // budget in microseconds -- the deadline has to be time, not turns.
      timespec nap{0, kReapNapNs};
      timespec left{};
      while ((nanosleep(&nap, &left) == -1) && (errno == EINTR)) nap = left;
    }
    // A zombie until koi exits, and init's after that. Losing the pid to a bad
    // alloc is worth less than the throw would cost: this runs from ~PickerScan,
    // which is noexcept, so the ESC path would be std::terminate.
    if (!reaped) {
      try {
        g_unreaped.push_back(pid);
      } catch (...) {
      }
    }
    pid = -1;
  }
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
  return status;
}

void EndCommandJob(PendingCommand& job) { EndChild(job.pid, job.fd); }

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

  int pid = -1;
  int fd = -1;
  std::string why;
  // The child's stderr on the pipe: a command job shows what its command said
  // as well as what it printed.
  if (!SpawnPipedChild(cmd, true, pid, fd, why)) {
    ed.status.Fail("cannot start " + cmd + ": " + why);
    return false;
  }

  PendingCommand job;
  job.command = cmd;
  job.watched = watched;
  job.with_msg = with_msg;
  job.then = then;
  job.view_name = view_name;
  job.pid = pid;
  job.fd = fd;
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

  // Heals land here too. They have no view to open and nothing to say, but they
  // do write to the store, and store writes stay on this thread -- so the pump
  // every finished job comes back through is where they are applied.
  const bool healing = PumpAnchorHeals(ed);

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
  return running || healing;
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
    if (kind == ExcerptView::Kind::kPins) {
      std::vector<ExcerptRef> fresh = PinRefs(ed);
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
  // A pins view is a view of where you have been, and moving the cursor is not
  // an event anything records -- so between two visits the store's answer can
  // go stale without anything marking it. Focusing one asks the question again
  // rather than trusting the mark. It is a keypress, not a frame, and
  // recomputing costs four rows; only a genuine difference rebuilds.
  if ((doc.excerpts.kind == ExcerptView::Kind::kPins) && !doc.modified &&
      !doc.excerpts.rebuild_on_focus && !SameRefs(PinRefs(ed), doc.excerpts.refs)) {
    doc.excerpts.refs_stale = true;
    RebuildExcerptView(ed);
    return;
  }
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

ExcerptLine ClassifyExcerptLine(const ExcerptView& view, std::string_view line,
                                std::vector<Interval>& spans) {
  spans.clear();
  // A header is a path:line marker, never text from any file, so it is checked
  // first and never painted or selected as a match.
  if (std::ranges::binary_search(view.header_index, line)) return ExcerptLine::kHeader;
  if (std::ranges::binary_search(view.anchor_index, line)) return ExcerptLine::kWholeLineMatch;
  if (!view.paint_line) return ExcerptLine::kPlain;
  view.paint_line(line, spans);
  const auto width = static_cast<Index>(line.size());
  for (Interval& span : spans) {
    if (span.empty()) continue;
    // Both ends clamped, the right one never below the left: a span a painter
    // put past the line must come out empty, not as an inverted iota_view.
    const Index lo = std::clamp<Index>(span.front(), 0, width);
    span = Interval(lo, std::clamp<Index>(span.back() + 1, lo, width));
  }
  const auto gone = std::ranges::remove_if(spans, [](const Interval& s) { return s.empty(); });
  spans.erase(gone.begin(), gone.end());
  return spans.empty() ? ExcerptLine::kPlain : ExcerptLine::kSpanMatches;
}

void SelectExcerptMatches(Editor& ed) {
  if (!IsExcerptView(ed.doc)) {
    ed.status.Warn("not an excerpt view");
    return;
  }
  // No AlignExcerptModel here, unlike the excerpt commands that read `blocks`:
  // this reads only the paint model, and reads it exactly as the renderer does.
  // Undo and redo align it already, so aligning again could only move it away
  // from what is on screen.
  std::string text;
  ReadDocRangeInto(ed.doc.table, Interval(0, DocLength(ed.doc.table)), text);
  std::vector<Selection> found;
  std::vector<Interval> spans;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t eol = text.find('\n', at);
    if (eol == std::string::npos) eol = text.size();
    const std::string_view line{text.data() + at, eol - at};
    const auto start = static_cast<Index>(at);
    // The newline is never part of a match: a selection that swallowed it would
    // join two body lines on the first change typed over it.
    switch (ClassifyExcerptLine(ed.doc.excerpts, line, spans)) {
      case ExcerptLine::kWholeLineMatch:
        if (!line.empty()) {
          found.push_back(CoveringSelection(ed.doc.table, start,
                                            start + static_cast<Index>(line.size())));
        }
        break;
      case ExcerptLine::kSpanMatches:
        for (const Interval& span : spans) {
          found.push_back(
              CoveringSelection(ed.doc.table, start + span.front(), start + span.back() + 1));
        }
        break;
      case ExcerptLine::kHeader:
      case ExcerptLine::kPlain:
        break;
    }
    at = eol + 1;
  }

  if (found.empty()) {
    ed.status.Warn("no matches");
    return;
  }
  RecordJump(ed);
  const std::size_t count = found.size();
  ed.doc.selections.Replace(ed.doc.table, std::move(found));
  ed.status = "selected " + std::to_string(count) + " match" + ((count == 1) ? "" : "es");
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
  const std::vector<Pin> before = ed.project->Pins();
  const bool replacing =
      (slot >= 1) && (slot <= std::ssize(before)) && !before[static_cast<size_t>(slot - 1)].path.empty();
  ed.project->SetPin(slot, CurrentFile(ed));
  MarkLiveViewsStale(ed, ExcerptView::Kind::kPins);
  const std::vector<Pin> after = ed.project->Pins();
  // Read back, so spelled the way the store spells it. The line is not checked
  // because the store no longer holds one -- it reports where the file was last
  // left, which is a different question from whether the pin was written.
  const bool landed = (slot >= 1) && (slot <= std::ssize(after)) &&
                      (after[static_cast<size_t>(slot - 1)].path == ProjectPath(ed.doc.file));
  if (!landed) {
    ed.status.Fail("pin " + std::to_string(slot) + " was not written to the project database");
    return;
  }
  ed.status = "pinned " + DisplayPath(ed.doc.file) + " to " + std::to_string(slot) +
              (replacing ? " -- replaced the old pin" : "");
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

// The most recent edit anywhere, which is a question the revision trees already
// answer -- so this stores nothing and there is nothing to keep in sync.
//
// Every open buffer's `revisions[current]` is the step that produced the text
// it is showing, `stamp_seq` is where that step falls in the order every edit
// in the process was written in, and `cursors_after` is where the cursors were
// when it was. Coalescing keeps both current (piece_doc.cpp), so a burst of
// typing is one step counted at its last keystroke rather than a hundred. Undo
// moves `current` back, which is right: after undoing, the last edit standing
// really is the older one.
//
// Deliberately not built on the project store. `files.last_line` is bumped by
// every visit as well as every edit, and RecordEditHere only runs on save, so
// "the last place an edit happened" read from there is really "wherever the
// cursor was when something was last written to disk".
//
// The cost is that this is session-scoped and covers open buffers only, and
// that history is budget-trimmed (PieceTable::history_budget_bytes), so a long
// enough session forgets its oldest steps. Persisting it would mean a new table
// written on every buffer leave, for a question that is almost always about the
// last few minutes.
void GoToLastEdit(Editor& ed) {
  const std::size_t count = BufferCount(ed);
  std::size_t best = count;
  std::uint64_t newest = 0;  // stamp_seq counts from 1, so 0 is "none yet"
  for (std::size_t i = 0; i < count; ++i) {
    const Document& doc = BufferAt(ed, i);
    // A view's offsets are positions in a text the view assembled, not in any
    // file, so an edit inside one has no location to go to.
    if (IsExcerptView(doc) || !HasDiskFile(doc)) continue;
    const PieceTable& table = doc.table;
    // revisions[0] is the file as loaded and no edit made it.
    if ((table.current <= 0) || (table.current >= std::ssize(table.revisions))) continue;
    const Revision& rev = table.revisions[static_cast<std::size_t>(table.current)];
    if (rev.forward.empty() || (rev.stamp_seq <= newest)) continue;
    newest = rev.stamp_seq;
    best = i;
  }
  if (best == count) {
    ed.status.Warn("nothing has been edited yet");
    return;
  }

  // Everything is read out before OpenAt, because opening moves documents:
  // SwitchToBuffer moves ed.doc into the buffer list and the target out of it,
  // so `doc` and anything pointing into it are stale from that call onwards.
  const Document& doc = BufferAt(ed, best);
  const PieceTable& table = doc.table;
  const Revision& rev = table.revisions[static_cast<std::size_t>(table.current)];
  const CursorState& after = rev.cursors_after;
  // The primary cursor, as asked. A revision whose cursors were never noted
  // falls back to the start of its first edit, which is where the change is
  // even when it is not where the cursor ended up.
  Index at = rev.forward.front().start_byte;
  if (after.primary < after.spans.size()) {
    at = after.spans[static_cast<std::size_t>(after.primary)].head;
  }
  at = std::clamp<Index>(at, 0, DocLength(table));
  const Index line = LineAt(table, at);
  const Index column = at - LineStart(table, line) + 1;
  const std::string path = doc.file.string();
  OpenAt(ed, path, line + 1, column);
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

double NowSeconds() {
  const auto since = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration<double>(since).count();
}

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
  // Either way the store's idea of where this file was last left has just
  // moved, and that is the line the pins view excerpts around -- so an open one
  // is out of date the moment this returns. Marking is cheap: the rebuild only
  // happens for a view that is on screen, and PinRefs comparing equal skips it
  // (RefreshLiveExcerptViews).
  MarkLiveViewsStale(ed, ExcerptView::Kind::kPins);
}

// -- what a location row says about a place -----------------------------------

// The line rule itself lives in anchor.h, and has to: what a record stores here
// is compared, later and elsewhere, against what a heal reads back out of the
// file. Two spellings of "the same line" would not fail loudly, they would make
// every row unhealable. TrimAnchorLine is the view-returning form, which is
// what the per-line census below wants.
std::string LineTextAt(const PieceTable& table, Index line, std::string& scratch) {
  ReadDocRangeInto(table, LineContentRange(table, line), scratch);
  return NormalizeAnchorLine(scratch);
}

// Past either this or kMaxUniqBytes (editor.h, shared with the heal job) a
// record leaves `uniq` at 0, "unknown". The census is one pass over the buffer
// and it runs on the input loop: measured at about 7 us per kilobyte, so koi's
// own largest source (140 KB) costs a millisecond and the cap costs fifteen. A
// generated file bigger than that is not worth a visible pause for a number
// that only sharpens a heal.
constexpr Index kMaxUniqLines = 100000;

// What a record costs the second time in one revision: nothing. The enclosing
// range and the line-hash census are both properties of (document, revision),
// and a boundary record, a jump record and a linger record at one place all ask
// for them in the same breath.
//
// One entry, because there is one cursor: the editor records where it is, and
// where it is is in one document. A buffer switch or a keystroke drops it.
struct RecordCache {
  Index document{0};
  Index revision{-1};

  // The enclosing definition of one cursor position, and the position it was
  // resolved for. Keyed on the position and not on the range it landed in: a
  // nested named function is a strict subset of its parent's range, so "still
  // inside the cached range" would answer with the parent for every cursor
  // inside the child. An empty `symbol` is a resolved "none", cached too.
  bool symbol_valid{false};
  Index symbol_cursor{-1};
  Index symbol_from{0};
  Index symbol_to{0};
  std::string symbol;

  bool uniq_valid{false};
  std::unordered_map<std::uint64_t, std::int64_t> lines;
};

RecordCache& CacheFor(const Document& doc) {
  static RecordCache cache;
  if ((cache.document != doc.id) || (cache.revision != doc.table.revision)) {
    cache = RecordCache{};
    cache.document = doc.id;
    cache.revision = doc.table.revision;
  }
  return cache;
}

// The name of the definition that opens `range`, from the live tree.
//
// The definitions query over the range, and the earliest capture in it wins:
// the outermost pattern that starts inside a function's own span is the
// function's own declarator, and anything nested -- a lambda, a local class --
// starts after it. It is the same query the symbol picker's rows come from,
// which is what makes the name here and the name in `symbols` the same string.
std::string DefinitionNameIn(Document& doc, Index from, Index to) {
  static constexpr std::array<std::string_view, 1> kDefinitions{"definitions.scm"};
  std::vector<Capture> captures;
  std::string error;
  if (!doc.syntax->Captures(doc.table, kDefinitions, Interval(from, to), captures, error)) {
    return {};
  }
  const Capture* first = nullptr;
  for (const Capture& one : captures) {
    if ((one.from < from) || (one.to > to)) continue;
    if ((first == nullptr) || (one.from < first->from)) first = &one;
  }
  if (first == nullptr) return {};

  std::string text = ReadDocRange(doc.table, Interval(first->from, first->to));
  std::string_view kept = TrimAnchorLine(text);
  std::string name{kept};
  for (char& c : name) {
    if ((c == '\n') || (c == '\r') || (c == '\t')) c = ' ';
  }
  return name;
}

// The innermost definition the cursor is inside, or nothing.
//
// Only ever from a tree that already exists. TextObjectRanges parses the buffer
// itself when it is handed no live Syntax -- 21 ms, on the input loop, per
// record -- so a document with no syntax attached, or one whose Syntax is for
// another language than the file now has, records a null symbol and healing
// fills it in later (docs/smart-jump.md, Recording).
std::string EnclosingSymbol(Editor& ed, Index cursor) {
  Document& doc = ed.doc;
  if ((doc.syntax == nullptr) || (doc.syntax->Language() != LanguageForPath(doc.file))) return {};

  RecordCache& cache = CacheFor(doc);
  // The cursor itself, not the range it was in. The query below returns the
  // whole chain of functions the cursor sits in, so "inside the last answer"
  // says nothing about a function nested deeper that the last cursor happened
  // to be outside of -- and answering the parent for a cursor in a nested `def`
  // is a wrong name on the row and a wrong name credited in `symbols`.
  if (cache.symbol_valid && (cache.symbol_cursor == cursor)) return cache.symbol;

  static constexpr std::array<std::string_view, 1> kAround{"around"};
  std::vector<ObjectRange> ranges;
  std::string error;
  if (!TextObjectRanges(doc.table, doc.file, "function", kAround, ranges, error, doc.syntax.get(),
                        Interval(cursor, cursor + 1))) {
    return {};
  }
  // Innermost first, and the first one that has a name is the answer: a lambda
  // is a `function.around` with no declarator, and the place to call that is
  // the function the lambda is written in.
  std::ranges::sort(ranges, [](const ObjectRange& a, const ObjectRange& b) {
    return (a.to - a.from) < (b.to - b.from);
  });
  cache.symbol_valid = true;
  cache.symbol_cursor = cursor;
  cache.symbol_from = 0;
  cache.symbol_to = 0;
  cache.symbol.clear();
  for (const ObjectRange& one : ranges) {
    if ((cursor < one.from) || (cursor >= one.to)) continue;
    std::string name = DefinitionNameIn(doc, one.from, one.to);
    if (name.empty()) continue;
    cache.symbol_from = one.from;
    cache.symbol_to = one.to;
    cache.symbol = std::move(name);
    break;
  }
  return cache.symbol;
}

// How many lines of the buffer read the same as this one, from a census built
// once per revision. Zero is "not counted", which is what an empty line and an
// oversized buffer both get.
std::int64_t UniqOf(Document& doc, std::string_view content) {
  if (content.empty()) return 0;
  RecordCache& cache = CacheFor(doc);
  if (!cache.uniq_valid) {
    const PieceTable& table = doc.table;
    const Index lines = LineCount(table);
    if ((lines > kMaxUniqLines) || (DocLength(table) > kMaxUniqBytes)) return 0;
    // One read and one pass. Per-line reads down the piece tree would be a
    // descent each; this is a copy and a walk. Sized up front, because a
    // rehash halfway through is a third of what this costs.
    cache.lines.reserve(static_cast<std::size_t>(lines));
    const std::string text = ReadDocRange(table, Interval(0, DocLength(table)));
    std::string_view rest{text};
    while (!rest.empty()) {
      const std::size_t end = rest.find('\n');
      const std::string_view line = rest.substr(0, end);
      const std::string_view normalized = TrimAnchorLine(line);
      if (!normalized.empty()) ++cache.lines[AnchorLineHash(normalized)];
      if (end == std::string_view::npos) break;
      rest.remove_prefix(end + 1);
    }
    cache.uniq_valid = true;
  }
  const auto found = cache.lines.find(AnchorLineHash(content));
  return (found == cache.lines.end()) ? 0 : found->second;
}

// The lines on either side, which is what tells one `}` from another. Not part
// of what a row is matched on -- it confirms a candidate the content found.
std::string ContextAround(const PieceTable& table, Index line, std::string& scratch) {
  constexpr Index kContextLines = 2;
  std::string out;
  bool first = true;
  const Index last = LineCount(table) - 1;
  for (Index at = line - kContextLines; at <= (line + kContextLines); ++at) {
    if (at == line) continue;
    // A blank line is context too, and it keeps its place: the entries are
    // positional, so dropping the empty ones would make "two above" mean
    // different lines in different files. A slot off either end of the buffer
    // is an empty entry for the same reason -- AnchorContextAt() pads the same
    // way, and a heal compares the two entry by entry.
    if (!first) out += '\n';
    first = false;
    if ((at < 0) || (at > last)) continue;
    out += LineTextAt(table, at, scratch);
  }
  return out;
}

// The symbols table, from the place just recorded. RecordSymbolVisit used to
// fire only on an arrival through a symbol mechanism -- a picker row, a hot
// symbol -- so a function worked on for days had no row and `d pin` found
// nothing. Debounced on the symbol rather than on the clock: staying inside one
// function is one visit to it, however many locations inside it get recorded.
void NoteSymbolVisit(Editor& ed, std::string_view symbol, Index cursor_line) {
  if (!ed.project || symbol.empty()) return;
  if ((ed.record.symbol_document == ed.doc.id) && (ed.record.symbol == symbol)) return;
  ed.record.symbol_document = ed.doc.id;
  ed.record.symbol = symbol;
  // Where the definition starts, not where the cursor was inside it: this row
  // is what a hot-symbol jump lands on, and landing halfway down a function is
  // not landing on it. The cursor's line is the fallback for a resolution the
  // cache no longer holds the range of.
  const RecordCache& cache = CacheFor(ed.doc);
  const Index line = (cache.symbol_valid && (cache.symbol == symbol))
                         ? LineAt(ed.doc.table, cache.symbol_from) + 1
                         : cursor_line;
  // The path is built here and not taken from a row, because the boundary this
  // is reached from may have decided there is no row to build.
  ed.project->RecordSymbolVisit(symbol, ed.doc.file.native(), line);
}

// Whether this buffer's file is one the store will keep rows for, from an
// answer held per file rather than reached per boundary: LocationKey
// canonicalises, which is a stat down every component of the path.
bool StorableHere(Editor& ed) {
  const std::string& path = ed.doc.file.native();
  if (ed.record.storable_path != path) {
    ed.record.storable_path = path;
    ed.record.storable = StorablePath(LocationKey(path));
  }
  return ed.record.storable;
}

// One record of the current place, with everything that goes with it: the row,
// the `files` bump behind it, the symbol, and the bookkeeping that keeps the
// next few keystrokes from writing it all again.
//
// The in-memory debounce is not the store's. The store holds a row's counters
// still for thirty seconds (kLocationVisitDebounce) so that a burst of typing
// is one edit rather than forty; this one holds the *write* still over the same
// window. It is decided before the place is described and not after: describing
// one is a read of the whole buffer (the uniq census), and on a large file that
// is milliseconds per keystroke thrown away -- an edit boundary is one record
// per revision by construction, so the census cache never once hits on the path
// that pays for it. Moving more than a merge's worth of lines away, or changing
// kind, is a new place and writes at once.
//
// `force` is for the save: it is the moment the file on disk becomes something
// a row can name (the blob), and a save inside the window would otherwise leave
// the row describing the dirty buffer it was recorded from. A save is a human
// action; the store's own debounce still keeps it from counting twice.
void RecordBoundary(Editor& ed, int kind, bool force = false) {
  if (!ed.project) return;
  Document& doc = ed.doc;
  // The two kinds of buffer that are not places -- a view assembles text that
  // is in no file, a scratch buffer has nothing to come back to -- and then the
  // one that is a place the store has been told not to keep. All three before
  // anything is read out of the buffer.
  if (IsExcerptView(doc) || !HasDiskFile(doc)) return;
  if (!StorableHere(ed)) {
    ed.record.recorded = true;
    return;
  }

  const Index cursor = CursorOf(doc.table, doc.selections.Primary());
  const Index line = LineAt(doc.table, cursor) + 1;
  const double now = NowSeconds();
  const bool same = (ed.record.wrote_kind == kind) && (ed.record.wrote_document == doc.id) &&
                    (std::abs(line - ed.record.wrote_line) <= kLocationMergeLines);
  if (force || !same || ((now - ed.record.wrote_at) >= kLocationVisitDebounce)) {
    LocationRecord row;
    if (!LocationHere(ed, row)) return;
    row.kind = kind;
    ed.project->WriteLocation(row);
    // The row that has just been written names this cursor at this revision,
    // which is the one moment its stored line is certainly true. Taking it into
    // the shadow now is what makes every later edit above it a shift instead of
    // a stale number -- and it is why jump_backward lands on the text it left
    // rather than on the line number it left.
    AdoptAnchorRows(ed, ed.doc);
    ed.record.wrote_document = doc.id;
    ed.record.wrote_line = row.line;
    ed.record.wrote_kind = kind;
    ed.record.wrote_at = now;

    // `files` moves with the row: since v4 an edit is recorded where it
    // happens rather than when the file is saved, so `files.edits` counts edit
    // bursts, and a burst is what the debounce defines. `same` is in the
    // condition and not just the window, or the first edit at a place a linger
    // has just recorded would be swallowed by that linger's window -- and an
    // edit is the signal worth three visits.
    if (!same || ((now - ed.record.files_at) >= kLocationVisitDebounce)) {
      ed.record.files_at = now;
      RecordHere(ed, kind != 0);
    }
    ed.record.note_document = doc.id;
    ed.record.note_line = line;
    NoteSymbolVisit(ed, row.symbol, row.line);
  } else if ((ed.record.note_document != doc.id) || (ed.record.note_line != line)) {
    // The row is debounced, the symbol is not: ten lines is inside the merge
    // window and can still be inside a different function. Only the tree query
    // runs here, and only when the cursor has left the line the last note was
    // made from -- typing on one line cannot walk into another definition, and
    // typing a definition's own name should not credit every prefix of it.
    ed.record.note_document = doc.id;
    ed.record.note_line = line;
    NoteSymbolVisit(ed, EnclosingSymbol(ed, cursor), line);
  }

  ed.record.recorded = true;
}

// A smart-jump arrival, armed rather than recorded. Everything else in this
// file writes where the cursor is the moment it gets there; this one waits to
// find out whether the jump was right, because a mis-jump that records itself
// makes the next mis-jump likelier -- zoxide's documented trust-killer, and the
// reason this corpus can be believed.
void ArmSmartJumpBounce(Editor& ed, Index line) {
  ed.record.pending = true;
  ed.record.pending_document = ed.doc.id;
  ed.record.pending_line = line;
  ed.record.pending_revision = ed.doc.table.revision;
  ed.record.pending_since = NowSeconds();
  ed.record.pending_typed.clear();
  ed.record.pending_target.clear();
}

// The arrival is dropped: no row, no credit, no trace. Both halves go together
// -- a `queries` accept left behind here would be spent on the next arrival.
void DropSmartJumpArrival(Editor& ed) {
  ed.record.pending = false;
  ed.record.pending_typed.clear();
  ed.record.pending_target.clear();
}

// The other half, run at both boundaries: the arrival is written once it has
// been stood in for kBounceSeconds or edited, and dropped without a trace if
// the cursor has gone anywhere else first.
//
// Same window as a merge (kLocationMergeLines): moving a few lines inside the
// function you landed in is not leaving, and the row a record would land on is
// the same one either way.
void CheckSmartJumpArrival(Editor& ed) {
  if (!ed.record.pending) return;
  Document& doc = ed.doc;
  if (doc.id != ed.record.pending_document) {
    DropSmartJumpArrival(ed);
    return;
  }
  // Where the cursor is now decides both paths, the edited one included: one
  // command can edit and move at once -- undo at a distance, a multi-line
  // paste, a bound list of commands -- and an edit that leaves the cursor
  // elsewhere is not an edit at the arrival. Recording it would name a place
  // never stood in and credit the query for having found it.
  const Index line = LineAt(doc.table, CursorOf(doc.table, doc.selections.Primary())) + 1;
  if (std::abs(line - ed.record.pending_line) > kLocationMergeLines) {
    DropSmartJumpArrival(ed);
    return;
  }
  const bool edited = doc.table.revision != ed.record.pending_revision;
  if (!edited && ((NowSeconds() - ed.record.pending_since) < kBounceSeconds)) return;
  ed.record.pending = false;
  // Forced: the arrival is a place in its own right, and the debounce window
  // the jump's own departure record opened would otherwise swallow it.
  RecordBoundary(ed, edited ? 1 : 0, true);
  // The credit, on the same evidence as the row. A bounced mis-jump reaches
  // neither the store nor `queries`.
  if (!ed.record.pending_typed.empty() && ed.project) {
    ed.project->RecordQueryAccept(ed.record.pending_typed, ed.record.pending_target);
  }
  ed.record.pending_typed.clear();
  ed.record.pending_target.clear();
}

}

bool LocationHere(Editor& ed, LocationRecord& out) {
  Document& doc = ed.doc;
  // The two kinds of buffer that are not places: a view assembles text that is
  // in no file, and a scratch buffer has nothing to come back to.
  if (IsExcerptView(doc) || !HasDiskFile(doc)) return false;

  const PieceTable& table = doc.table;
  const Index cursor = CursorOf(table, doc.selections.Primary());
  Index line = 0;
  Index start = 0;
  LineAtAndStart(table, cursor, line, start);

  out = LocationRecord{};
  out.path = doc.file.string();
  out.line = line + 1;
  out.col = cursor - start + 1;
  out.has_text = true;

  std::string scratch;
  out.content = LineTextAt(table, line, scratch);
  out.context = ContextAround(table, line, scratch);
  out.uniq = UniqOf(doc, out.content);
  out.symbol = EnclosingSymbol(ed, cursor);
  // The blob names the file on disk, so it is only true of a buffer that is
  // still that file. ExternallyModified is two stat()s and catches the case the
  // dirty flag cannot: somebody else wrote the file while it sat here clean.
  if (!doc.modified && !doc.disk_blob.empty() && !ExternallyModified(doc)) {
    out.blob = doc.disk_blob;
  }
  return true;
}

void NoteRecordedHere(Editor& ed, const LocationRecord& row) {
  // Written by the jump list rather than by RecordBoundary, and the shadow
  // wants it for the same reason -- see there.
  AdoptAnchorRows(ed, ed.doc);
  ed.record.wrote_document = ed.doc.id;
  ed.record.wrote_line = row.line;
  ed.record.wrote_kind = row.kind;
  ed.record.wrote_at = NowSeconds();
  ed.record.recorded = true;
  ed.record.note_document = ed.doc.id;
  ed.record.note_line = row.line;
  NoteSymbolVisit(ed, row.symbol, row.line);
}

void NoteInputBoundary(Editor& ed) {
  // Linger: three seconds at one place, and the place is recorded once.
  //
  // There is no timer and no thread. The loop this hangs off blocks in
  // tb_poll_event with no timeout when nothing is happening, so the record
  // lands on the *next* event after the three seconds rather than at the
  // instant they pass -- which is what the design allows for, and the only
  // difference it makes is to a place the user walked away from the terminal
  // in: it is recorded when they come back.
  CheckSmartJumpArrival(ed);
  if (ed.record.recorded || (ed.record.document != ed.doc.id) || (ed.record.line < 0)) return;
  if ((NowSeconds() - ed.record.since) < kLingerSeconds) return;
  RecordBoundary(ed, 0);
}

void NoteCommandBoundary(Editor& ed) {
  // Before the place tracking below moves on: this is the last moment the
  // arrival's own line and revision can be compared with where the command
  // that just ran has left the cursor.
  CheckSmartJumpArrival(ed);

  Document& doc = ed.doc;
  const Index revision = doc.table.revision;
  // An edit is a command boundary where the buffer's revision moved. Only
  // within one document: across a buffer switch the two numbers are unrelated.
  const bool edited = (ed.record.document == doc.id) && (ed.record.revision >= 0) &&
                      (revision != ed.record.revision);

  // The document id and the line are what a stay is: this runs once per turn of
  // the input loop, so nothing here reads a path or allocates.
  const Index line = LineAt(doc.table, CursorOf(doc.table, doc.selections.Primary()));
  if ((doc.id != ed.record.document) || (line != ed.record.line)) {
    ed.record.document = doc.id;
    ed.record.line = line;
    ed.record.since = NowSeconds();
    ed.record.recorded = false;
  }
  ed.record.revision = revision;

  if (edited) RecordBoundary(ed, 1);
}

void RecordVisitHere(Editor& ed) { RecordHere(ed, false); }

void RecordEditHere(Editor& ed) { RecordBoundary(ed, 1, true); }

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

// -- smart jump --------------------------------------------------------------
//
// The editor half of docs/smart-jump.md: the prompt, the landing rules and the
// step-through. The parser, the snapshot and the ranking are in smartjump.cpp,
// which knows nothing about an Editor; what is left here is what has to drive
// the recorder, the excerpt views and the pickers, and all three of those live
// in this file already.

namespace {

SmartJumpState& SmartJumpStateOf(Editor& ed) {
  if (!ed.smart_jump) ed.smart_jump = std::make_shared<SmartJumpState>();
  return *ed.smart_jump;
}

// Everything a smart-jump landing does that an ordinary open does not.
//
// The place being left goes into the jump list, as it does for every jump
// motion. The place being arrived at does not go into the store at all -- it is
// armed instead, and earns its row by being stood in. And the stored line is
// corrected by the open buffer's shadow before it is used, because a line in
// the store is a cache and every edit above it since has moved the text it
// names.
// `landed` comes back as the line the cursor is actually on, which is the
// stored line healed, re-found and clamped -- what the status has to name.
bool LandSmartJump(Editor& ed, const SmartMatch& match, Index column, Index& landed) {
  if (match.path.empty()) return false;
  const fs::path path{match.path};
  RecordJump(ed);

  const bool already_here = !IsExcerptView(ed.doc) && HasDiskFile(ed.doc) &&
                            (CanonicalOf(ed.doc.file) == CanonicalOf(path));
  if (!already_here && !OpenFile(ed, path)) return false;

  Target target;
  target.path = path;
  target.line = std::max<Index>(1, match.line);
  target.has_line = true;
  target.column = (match.col > 0) ? match.col : column;
  target.has_column = target.column > 0;
  if (Index live = target.line; AnchorShadowLine(ed, ed.doc, match.row_id, live)) {
    target.line = live;
  }
  // The symbol tier heals by name, not by line: `symbols.line` has no anchor
  // behind it, so re-find the definition in the file as it stands and prefer
  // that. One parse per symbol landing -- a jump can afford what a keystroke
  // cannot -- and the store row is refreshed so the next list shows the truth.
  if (!match.symbol.empty()) {
    std::string scan_error;
    const std::string text = ReadDocRange(ed.doc.table, Interval(0, DocLength(ed.doc.table)));
    for (const DefinitionSpan& span : ScanDefinitionSpans(ed.doc.file, text, scan_error)) {
      if (span.name != match.symbol) continue;
      target.line = span.from_line;
      if (ed.project) {
        ed.project->RecordSymbolVisit(match.symbol, ed.doc.file.string(), span.from_line);
      }
      break;
    }
  }
  GoToTarget(ed.doc, target);
  // Armed from where the cursor actually is rather than from what was asked
  // for: GoToTarget clamps to the last line, and a stored line past the end of
  // a file that shrank between sessions would otherwise cancel its own arrival
  // on the disparity -- such a row could never record, credit, or even miss.
  landed = LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary())) + 1;
  ArmSmartJumpBounce(ed, landed);
  return true;
}

// What one more press in this direction would give, for the status line. The
// row just landed on is under the cursor already, so naming it answers nothing;
// the only question stepping asks is whether to press again, and that is a
// question about the row after this one. Nothing to say for a list of one.
//
// The line is healed the way the landing heals it: from the open buffer's
// shadow when the row belongs to it -- which is the common case, a second line
// in the file just opened -- and the stored line otherwise, which is the best
// that is known without opening the file.
// The stepping status wears the rune the prompt does, spelled out only with
// icons off.
std::string SmartJumpBadge(const Editor& ed) {
  return ed.settings.icons ? "ᛃ " : "jump ";
}

std::string SmartNextSaid(Editor& ed, const std::vector<SmartMatch>& matches, std::size_t at,
                          bool forward, std::size_t* target_from = nullptr) {
  const auto count = static_cast<std::ptrdiff_t>(matches.size());
  if (count < 2) return {};
  const std::ptrdiff_t step = static_cast<std::ptrdiff_t>(at) + (forward ? 1 : -1);
  const bool wraps = (step < 0) || (step >= count);
  const SmartMatch& next = matches[static_cast<std::size_t>(((step % count) + count) % count)];
  Index line = next.line;
  AnchorShadowLine(ed, ed.doc, next.row_id, line);
  // The end of the list, said as what it costs: one more press is the top of
  // the list again, not a new place. Bare otherwise -- the branch row's whole
  // meaning is "the next press", so the word would only repeat the glyph.
  // `target_from` gets the offset of the destination itself, for the caller
  // to mark.
  const std::string head = wraps ? "wraps to " : "";
  if (target_from != nullptr) *target_from = head.size();
  return head + SmartDisplayAt(next, line);
}

// One accepted arrival: the jump, the query credited for having found it, and
// the cursor into the ranked list moved so that next and prev step from here.
bool AcceptSmartJump(Editor& ed, std::size_t at, Index& landed, Index column = 0) {
  SmartJumpState& state = SmartJumpStateOf(ed);
  if (at >= state.matches.size()) return false;
  const SmartMatch match = state.matches[at];
  if (!LandSmartJump(ed, match, column, landed)) {
    ed.status.Warn("cannot open " + match.path);
    return false;
  }
  state.at = at;
  // Held with the armed arrival, not written here: see CheckSmartJumpArrival.
  if (ed.project && !state.typed.empty()) {
    ed.record.pending_typed = state.typed;
    ed.record.pending_target = match.key;
  }
  return true;
}

}

void SmartJumpPrompt(Editor& ed) {
  if (!RequireProject(ed)) return;
  // Asking again is rejecting the last answer. An arrival still unconfirmed
  // when this prompt reopens is one the user is walking away from without
  // having moved the cursor to say so, and the clock would otherwise run on
  // through the typing of its replacement -- prompt keys move no cursor, so
  // nothing would cancel it -- and credit the very pair being abandoned. An
  // arrival that had already earned its row earned it at the boundary that ran
  // on the keystroke opening this prompt, so nothing confirmed is taken back.
  DropSmartJumpArrival(ed);
  PromptOpen(ed, PromptKind::kSmartJump);
  // The caret box hangs whatever ed.status holds off itself as its feedback
  // row, so a message left over from before the prompt would read as an answer
  // to a query not yet typed. An empty query previews to nothing; start there.
  ed.status.clear();
  // The whole corpus, once, here. Every keystroke after this re-scores it in
  // full and touches nothing else -- no disk, no parser, no subprocess.
  SmartJumpState& state = SmartJumpStateOf(ed);
  BuildSmartCorpus(*ed.project, state.corpus);
}

void SmartJumpPreview(Editor& ed) {
  if (!ed.prompt_active || (ed.prompt_kind != PromptKind::kSmartJump) || !ed.smart_jump) return;
  SmartJumpState& state = *ed.smart_jump;
  const SmartQuery query = ParseSmartQuery(ed.prompt_input);
  // The count and the best target with its line text, so that where Enter goes
  // is visible before it is pressed. Into ed.status, which the caret box hangs
  // off itself as its feedback row -- and the bar leaves alone while the
  // prompt is up. The summary and not the list: this runs on every keystroke
  // and the rest of the ranking is strings nothing here reads.
  const SmartSummary summary = SummariseSmartMatches(state.corpus, query, ed.project.get());
  const std::string said = SmartJumpFeedback(query, summary);
  if (said.empty()) {
    ed.status.clear();
  } else {
    ed.status = said;
    // Where enter would land wears its own colour in the branch row, the way
    // the stepping status paints it on the bar.
    if (query.error.empty() && (summary.count > 0) && !summary.display.empty() &&
        said.ends_with(summary.display)) {
      ed.status.Highlight(said.size() - summary.display.size(), summary.display.size());
    }
  }
}

namespace {

// Where a handoff lands. Every picker opens in process at the caret: the box the
// query was typed in grows a band under it rather than the screen swapping. The
// query is already a PCRE2 pattern (PickerPattern), which is what the band
// filters with, so it goes in typed.
void OpenHandoffPicker(Editor& ed, const SmartHandoff& handoff) {
  switch (handoff.picker) {
    case SmartPicker::kContent: ContentPicker(ed, handoff.query); return;
    case SmartPicker::kSymbol: SymbolPicker(ed, handoff.query); return;
    case SmartPicker::kFile: break;
  }
  FilePicker(ed, handoff.query);
}

}

void SmartJumpSubmit(Editor& ed, std::string_view line) {
  if (!RequireProject(ed)) return;
  SmartJumpState& state = SmartJumpStateOf(ed);
  // Reached without the prompt having been opened -- a bound query, a test --
  // so there is no snapshot to score against yet.
  if (state.corpus.files.empty() && state.corpus.symbols.empty() &&
      state.corpus.locations.empty()) {
    BuildSmartCorpus(*ed.project, state.corpus);
  }

  const SmartQuery query = ParseSmartQuery(line);
  if (!query.error.empty()) {
    ed.status.Warn(query.error);
    return;
  }
  std::vector<SmartMatch> matches = RankSmartMatches(state.corpus, query, ed.project.get());
  // Either way this query is what n/N answer to now, so the picker walk that
  // held them is freed here -- one list at a time, whichever was made last.
  ed.walk.reset();
  if (matches.empty()) {
    // Said plainly -- nothing found means you have not been there, and this
    // list never widens on its own -- and then handed straight to the picker
    // that does search the project, with the deciding clause's terms in it.
    // Tab does the same before Enter; this is the same door at the dead end.
    // And the last list goes with it: stepping after this would otherwise walk
    // the previous query's matches, which is not what was asked for.
    state.matches.clear();
    state.typed.clear();
    state.at = 0;
    const SmartHandoff handoff = SmartJumpHandoff(query);
    const std::string said = "not been there -- " + std::string{SmartPickerName(handoff.picker)};
    ed.status = said;
    // Terms and all, so the picker opens with them typed: this has to be the
    // same place you would have got to by choosing the picker yourself.
    OpenHandoffPicker(ed, handoff);
    // Opening a prompt clears the status, so the sentence explaining the band is
    // said again over it -- and only where a band opened, so a picker that
    // refused keeps its own reason on the line.
    if (ed.prompt_active && (ed.prompt_kind == PromptKind::kPicker)) ed.status = said;
    return;
  }

  state.matches = std::move(matches);
  state.typed = query.typed;
  state.at = 0;

  // Enter lands on the best match, always -- the way a search lands on its
  // first hit -- and stepping is the disambiguator, not a list view. A wrong
  // landing costs one keypress (picker_jump_next) and teaches nothing: both the
  // row and the query credit wait out the bounce.
  const std::size_t count = state.matches.size();
  Index landed = 0;
  if (AcceptSmartJump(ed, 0, landed)) {
    // A lone match lands in silence: the cursor arriving is the whole answer,
    // and a box naming the place it already stands would be noise. Several
    // leave the walk's box at the caret, naming the second: the first is under
    // the cursor, and what is worth saying is where picker_jump_next would take
    // you from it.
    if (count == 1) {
      ed.status.clear();
      ed.jump_branch = false;
    } else {
      std::size_t dest = 0;
      const std::string said = SmartNextSaid(ed, state.matches, 0, true, &dest);
      const std::string head = SmartJumpBadge(ed) + "1/" + std::to_string(count) + "  ";
      ed.status = head + said;
      if (!said.empty()) ed.status.Highlight(head.size() + dest, said.size() - dest);
      ed.jump_branch = true;
    }
  }
}

void SmartJumpToPicker(Editor& ed) {
  // The same door the dead end opens, one keystroke earlier: the most specific
  // clause names the picker and hands over its own terms, escaped and joined the
  // way a picker query is a pattern.
  const SmartHandoff handoff = SmartJumpHandoff(ParseSmartQuery(ed.prompt_input));
  // Read before the cancel clears it, opened after: both halves are this one
  // keystroke, so the box at the caret is never drawn without a prompt in it.
  PromptCancel(ed);
  OpenHandoffPicker(ed, handoff);
}

void SmartJumpStep(Editor& ed, bool forward) {
  SmartJumpState& state = SmartJumpStateOf(ed);
  if (state.matches.empty()) {
    ed.status.Warn("no smart jump to step through");
    return;
  }
  const auto count = static_cast<std::ptrdiff_t>(state.matches.size());
  const std::ptrdiff_t target = static_cast<std::ptrdiff_t>(state.at) + (forward ? 1 : -1);
  const bool wrapped = (target < 0) || (target >= count);
  const auto landed = static_cast<std::size_t>(((target % count) + count) % count);

  const SmartMatch match = state.matches[landed];
  Index at_line = 0;
  if (!LandSmartJump(ed, match, 0, at_line)) {
    ed.status.Warn("cannot open " + match.path);
    return;
  }
  state.at = landed;
  // Not an accept: stepping is how a wrong landing gets corrected, and teaching
  // `queries` every row walked past on the way is how the correction would make
  // the next query worse.
  // Where the next press goes, not where this one landed -- and for a list of
  // one there is no next, so that one names itself. The wrap note is about the
  // step just taken; "next wraps to" is about the one after it.
  std::size_t dest = 0;
  const std::string said = (count == 1) ? SmartDisplayAt(match, at_line)
                                        : SmartNextSaid(ed, state.matches, landed, forward, &dest);
  const std::string head =
      SmartJumpBadge(ed) + std::to_string(landed + 1) + "/" + std::to_string(count) + "  ";
  ed.status = head + said +
              ((!wrapped || (count == 1)) ? ""
               : forward                  ? " -- wrapped to the top"
                                          : " -- wrapped to the bottom");
  if (!said.empty()) ed.status.Highlight(head.size() + dest, said.size() - dest);
  ed.jump_branch = true;
}

// -- the walk -----------------------------------------------------------------
//
// The other list n/N step: a picker's own rows, kept past the prompt that
// filtered them. Nothing here reaches into smart-jump's state -- no
// jump_cursor, no arrival credit, no bounce, because a picker row is not a
// store location and has none of that behind it. What is shared is the wording:
// the same badge, the same count, the same "where the next press goes".

namespace {

// One walk landing, and what accept does for the same row: a symbol records a
// visit -- a landing is a visit, whichever key produced it -- and a file or
// buffer row records nothing beyond what the open itself writes. False when the
// cursor did not move, so that the caller leaves the walk where it was instead
// of saying it went somewhere: SmartJumpStep's contract, on a picker's rows.
bool OpenWalkRow(Editor& ed, PickerState::Source source, const WalkRow& row) {
  if (source == PickerState::Source::kBuffers) {
    if (ChooseBufferRow(ed, row.target)) return true;
    // The only failure a path row does not already announce: an `#index` with
    // no buffer under it any more.
    ed.status.Warn("cannot open " + row.display);
    return false;
  }
  // The pair a symbol visit records is where you were and where you went, so
  // the file you came from is read before the open moves it -- and written only
  // once the open has happened, because an arrival that did not happen is not a
  // visit.
  const std::string from = CurrentFile(ed);
  if (!OpenAt(ed, row.target, row.line, row.column)) return false;
  if (!row.name.empty()) RecordSymbolPick(ed, row.name, row.target, row.line, from);
  return true;
}

// The branch row for where the walk now stands. Where the next press in this
// direction goes, not the row under the cursor, and no next at all for a list
// of one -- so that one names itself. `wrapped` is the step just taken; the
// "wraps to" note is about the one after it.
void SayWalk(Editor& ed, const WalkList& list, bool forward, bool wrapped) {
  const auto count = static_cast<std::ptrdiff_t>(list.rows.size());
  std::size_t dest = 0;
  std::string said = list.rows[list.at].display;
  if (count > 1) {
    const std::ptrdiff_t step = static_cast<std::ptrdiff_t>(list.at) + (forward ? 1 : -1);
    const std::string ahead = ((step < 0) || (step >= count)) ? "wraps to " : "";
    dest = ahead.size();
    said = ahead + list.rows[static_cast<std::size_t>(((step % count) + count) % count)].display;
  }
  const std::string head =
      SmartJumpBadge(ed) + std::to_string(list.at + 1) + "/" + std::to_string(count) + "  ";
  ed.status = head + said +
              ((!wrapped || (count == 1)) ? ""
               : forward                  ? " -- wrapped to the top"
                                          : " -- wrapped to the bottom");
  if (!said.empty()) ed.status.Highlight(head.size() + dest, said.size() - dest);
  ed.jump_branch = true;
}

}

void PickerJumpStep(Editor& ed, bool forward) {
  // One pair of keys, whichever list was made last. A pick clears smart-jump's
  // matches and a submit frees the walk, so there is never a choice to make.
  if (!ed.walk || ed.walk->rows.empty()) {
    SmartJumpStep(ed, forward);
    return;
  }
  WalkList& list = *ed.walk;
  const auto count = static_cast<std::ptrdiff_t>(list.rows.size());
  const std::ptrdiff_t target = static_cast<std::ptrdiff_t>(list.at) + (forward ? 1 : -1);
  const bool wrapped = (target < 0) || (target >= count);
  const auto landed = static_cast<std::size_t>(((target % count) + count) % count);
  // Copied, not referenced: the open runs arbitrary editor machinery between
  // here and the say below.
  const WalkRow row = list.rows[landed];
  // Committed only once the cursor is there. A step that could not open leaves
  // the walk where it stood and what the open said standing with it, which is
  // what a smart-jump step onto an unopenable match does.
  if (!OpenWalkRow(ed, list.source, row)) return;
  list.at = landed;
  SayWalk(ed, list, forward, wrapped);
}

void DropBuffersWalk(Editor& ed) {
  if (ed.walk && (ed.walk->source == PickerState::Source::kBuffers)) ed.walk.reset();
}

}
