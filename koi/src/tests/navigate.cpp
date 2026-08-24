// Tests for navigate.cpp: the pickers -- files, buffers, symbols -- their
// ranking, and the commands that drive them.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

#include <fcntl.h>

#include <cerrno>
#include <chrono>

namespace koi {

void SelfIsRunnableByName() {
  TEST_CASE("pickers: a bare `koi` in a command finds this koi");

  PutSelfOnPath();
  EXPECT_TRUE(std::system("command -v koi >/dev/null 2>&1") == 0);

  const char* before = std::getenv("PATH");
  const std::string saved{(before == nullptr) ? "" : before};
  PutSelfOnPath();
  const char* after = std::getenv("PATH");
  EXPECT_EQ(std::string{(after == nullptr) ? "" : after}, saved);
}

void FilePickerRanking() {
  TEST_CASE("pickers: the file picker offers the files you actually use first");

  namespace fs = std::filesystem;
  const Scratch scratch{"koi-rank-test"};
  const fs::path dir = scratch.dir;
  for (const char* name : {"alpha.txt", "bravo.txt", "charlie.txt", "delta.txt"}) {
    scratch.Write(name, "x\n");
  }

  Editor ed;
  std::string err;
  ed.project = ProjectStore::Open(dir / "state.db", err);
  EXPECT_TRUE(ed.project != nullptr);
  if (ed.project == nullptr) return;

  ed.settings.file_filter = "find . -maxdepth 1 -name '*.txt' -printf '%P\n'";
  const fs::path previous = fs::current_path();
  fs::current_path(dir);
  SetProjectRoot(dir);

  const auto first_row = [](const std::vector<RankedFile>& rows) {
    return rows.empty() ? std::string{} : rows.front().path;
  };
  const auto holds = [](const std::vector<RankedFile>& rows, std::string_view path) {
    return std::ranges::any_of(rows, [path](const RankedFile& one) { return one.path == path; });
  };

  const std::vector<RankedFile> cold = RankedFiles(ed);
  EXPECT_EQ(std::ssize(cold), Index{4});

  ed.project->RecordVisit("charlie.txt", 7, 2);
  EXPECT_EQ(first_row(RankedFiles(ed)), std::string{"charlie.txt"});
  ed.project->RecordEdit("delta.txt", 3, 1);
  EXPECT_EQ(first_row(RankedFiles(ed)), std::string{"delta.txt"});

  const std::vector<RankedFile> warm = RankedFiles(ed);
  EXPECT_EQ(std::ssize(warm), Index{4});
  for (const char* name : {"alpha.txt", "bravo.txt", "charlie.txt", "delta.txt"}) {
    EXPECT_TRUE(holds(warm, name));
  }

  // The store knows a file the filter does not offer: the rows are the filter's,
  // ranked, and never the store's list.
  ed.project->RecordVisit("vanished.txt", 1, 1);
  EXPECT_EQ(std::ssize(RankedFiles(ed)), Index{4});
  EXPECT_FALSE(holds(RankedFiles(ed), "vanished.txt"));

  // Where the pick opens: the store's own line and column ride the row.
  EXPECT_TRUE(warm.front().visited);
  EXPECT_EQ(warm.front().line, Index{3});
  EXPECT_EQ(warm.front().column, Index{1});

  SetProjectRoot({});
  fs::current_path(previous);
}

// The last frecency term, and the only one the store cannot apply: a file
// changed on this branch is likelier to be the one you want next. It is a
// multiplier on the score, so it reorders inside the frecent set and never
// lifts a file that has never been visited into it.
void TheFilePickerLiftsFilesChangedOnThisBranch() {
  TEST_CASE("pickers: a file changed on this branch outranks a slightly hotter one");

  namespace fs = std::filesystem;
  const Scratch scratch{"koi-branch-rank"};
  const fs::path dir = scratch.dir;
  scratch.Write("alpha.txt", "a\n");
  scratch.Write("bravo.txt", "b\n");

  Editor ed;
  std::string err;
  ed.project = ProjectStore::Open(dir / "state.db", err);
  EXPECT_TRUE(ed.project != nullptr);
  if (ed.project == nullptr) return;

  ed.settings.file_filter = "find . -maxdepth 1 -name '*.txt' -printf '%P\n'";
  const fs::path previous = fs::current_path();
  fs::current_path(dir);
  SetProjectRoot(dir);
  struct Restore {
    fs::path was;
    ~Restore() {
      SetProjectRoot({});
      fs::current_path(was);
    }
  } restore{previous};

  const auto first_row = [](const std::vector<RankedFile>& rows) {
    return rows.empty() ? std::string{} : rows.front().path;
  };

  // 84 against 80, both in the same recency bucket, so alpha is ahead by more
  // than a rounding wobble and by less than the 10% the bonus is worth.
  for (int i = 0; i < 21; ++i) ed.project->RecordVisit("alpha.txt", 1, 0);
  for (int i = 0; i < 20; ++i) ed.project->RecordVisit("bravo.txt", 1, 0);
  EXPECT_EQ(first_row(RankedFiles(ed)), std::string{"alpha.txt"});

  // Gated on a git that runs; without one the assertion above is the whole
  // contract, since the branch diff is empty and the ranking is unchanged.
  if (std::system("git --version >/dev/null 2>&1") != 0) return;
  const std::string at = "git -C '" + dir.string() + "' -c user.email=koi@test -c user.name=koi ";
  if (std::system(("git -c init.defaultBranch=main init -q '" + dir.string() + "' >/dev/null 2>&1")
                      .c_str()) != 0) {
    return;
  }
  EXPECT_EQ(std::system((at + "add alpha.txt bravo.txt >/dev/null 2>&1").c_str()), 0);
  EXPECT_EQ(std::system((at + "commit -q -m init >/dev/null 2>&1").c_str()), 0);
  EXPECT_EQ(std::system((at + "checkout -q -b feature >/dev/null 2>&1").c_str()), 0);
  scratch.Write("bravo.txt", "b changed\n");

  EXPECT_EQ(first_row(RankedFiles(ed)), std::string{"bravo.txt"});
}

void FileFilter() {
  TEST_CASE("pickers: file-filter feeds both scans, and only those two spawn one");

  Editor ed;
  ed.doc.file = "a.cpp";
  EXPECT_TRUE(FileFilterCommand(ed).find("find .") != std::string::npos);

  ed.settings.file_filter = "git ls-files";
  EXPECT_EQ(FileFilterCommand(ed), std::string{"git ls-files"});
  // Spelled out whole: the two commands are the wire between koi-the-editor and
  // its scan children, and a change to either is a change to the rows the band
  // reads. Whitespace and all -- the filter is a shell fragment, so where the
  // pipes and the continuations fall is the contract.
  EXPECT_EQ(PickerScanCommand(ed, "content"),
            std::string{"git ls-files | xargs gai --no-color -f '\\w' -v -d : --files"});
  EXPECT_EQ(PickerScanCommand(ed, "symbols"),
            std::string{"git ls-files | %{koi} --symbol-mode --picker-rows --definitions \\\n"
                        "      --hot-first --from %{buffer_name} --files -"});
  // And every variable in them is koi's own, so the child is this koi against
  // this buffer and nothing is left for the shell to see.
  const std::string expanded = ExpandVariables(PickerScanCommand(ed, "symbols"), ed);
  EXPECT_TRUE(expanded.find("%{") == std::string::npos);
  EXPECT_TRUE(expanded.find("--from a.cpp") != std::string::npos);

  // Every other picker builds its rows in process, so it has no command at all.
  for (const std::string_view name : {"files", "buffers", "buffer-symbols", "definition",
                                      "references", "nonesuch", ""}) {
    EXPECT_TRUE(PickerScanCommand(ed, name).empty());
  }

  ed.settings.file_filter = "find . -type f |\n  grep -v build |\n";
  EXPECT_TRUE(PickerScanCommand(ed, "symbols").starts_with("find . -type f |\n  grep -v build |"));
  EXPECT_TRUE(PickerScanCommand(ed, "symbols").find("| |") == std::string::npos);
}

void BufferPickerRows() {
  TEST_CASE("buffer picker: refuses when there is nothing to choose between");
  {
    const Scratch scratch{"koi-buffer-picker"};
    Editor ed;
    ResetToOriginal(ed.doc.table, "x\n");
    ed.status.clear();

    BufferPicker(ed);
    EXPECT_TRUE(ed.status.empty() || (ed.status.find("only one buffer") != std::string::npos));
  }

  TEST_CASE("buffer picker: every open buffer is reachable, named or not");
  {
    const Scratch scratch{"koi-buffer-picker-rows"};
    const std::filesystem::path a = scratch.Write("one.txt", "alpha\n");
    const std::filesystem::path b = scratch.Write("two.txt", "bravo\n");
    Editor ed;
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    EXPECT_TRUE(OpenTarget(ed, b.string()));
    RunTypableCommand(ed, "new");
    EXPECT_EQ(BufferCount(ed), std::size_t{3});

    const std::size_t before = BufferCount(ed);
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    EXPECT_EQ(BufferCount(ed), before);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"one.txt"});

    std::size_t unnamed = BufferCount(ed);
    for (std::size_t i = 0; i < BufferCount(ed); ++i) {
      if (BufferAt(ed, i).file.empty()) unnamed = i;
    }
    EXPECT_TRUE(unnamed < BufferCount(ed));
    SwitchToBuffer(ed, unnamed);
    EXPECT_TRUE(ed.doc.file.empty());
  }

  TEST_CASE("buffer picker: a generated view is chosen as a buffer, not reopened as a file");
  {
    const Scratch scratch{"koi-buffer-picker-views"};
    const std::filesystem::path a = scratch.Write("one.txt", "alpha\nbravo\ncharlie\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    OpenReferenceExcerpts(ed, {Symbol{a.string(), 2, 1, "bravo"}}, "bravo");
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_EQ(BufferCount(ed), std::size_t{2});
    const std::string view_name = ed.doc.view_name;

    std::size_t view_at = BufferCount(ed);
    std::size_t file_at = BufferCount(ed);
    for (std::size_t i = 0; i < BufferCount(ed); ++i) {
      (IsExcerptView(BufferAt(ed, i)) ? view_at : file_at) = i;
    }
    EXPECT_TRUE((view_at < BufferCount(ed)) && (file_at < BufferCount(ed)));

    const std::vector<BufferRow> rows = BufferPickerRows(ed);
    EXPECT_EQ(rows.size(), BufferCount(ed));
    if (rows.size() != BufferCount(ed)) return;
    EXPECT_EQ(rows[view_at].payload, "#" + std::to_string(view_at));
    EXPECT_TRUE(rows[file_at].payload.find("one.txt") != std::string::npos);
    EXPECT_TRUE(rows[view_at].text.starts_with(view_name + ":"));

    SwitchToBuffer(ed, view_at);
    ChooseBufferRow(ed, rows[file_at].payload);
    EXPECT_FALSE(IsExcerptView(ed.doc));
    EXPECT_EQ(BufferCount(ed), std::size_t{2});

    ChooseBufferRow(ed, rows[view_at].payload);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_EQ(ed.doc.view_name, view_name);
    EXPECT_EQ(BufferCount(ed), std::size_t{2});
    EXPECT_EQ(FindFileBuffer(ed, std::filesystem::path{view_name}), BufferCount(ed));
    EXPECT_TRUE(DocLength(ed.doc.table) > 0);
  }

  TEST_CASE("buffer picker: a name that cannot survive the row form is left out of it");
  {
    // A row is one line on the band and its payload is opened verbatim, so a
    // name holding a tab or a newline can be neither drawn nor reopened as
    // itself. It is left out rather than shown as something it is not.
    const Scratch scratch{"koi-buffer-picker-tab"};
    const std::filesystem::path odd = scratch.Write("a\tb.txt", "alpha\n");
    const std::filesystem::path plain = scratch.Write("plain.txt", "bravo\n");
    Editor ed;
    EXPECT_TRUE(OpenTarget(ed, odd.string()));
    EXPECT_TRUE(OpenTarget(ed, plain.string()));

    const std::vector<BufferRow> rows = BufferPickerRows(ed);
    EXPECT_EQ(rows.size(), std::size_t{1});
    for (const BufferRow& row : rows) {
      EXPECT_TRUE(row.text.find("plain.txt") != std::string::npos);
      EXPECT_TRUE(row.text.find_first_of("\t\n\r") == std::string::npos);
      EXPECT_TRUE(row.payload.find_first_of("\t\n\r") == std::string::npos);
    }
  }

  TEST_CASE("buffer picker: a scratch buffer keeps its index, and a bad row does nothing");
  {
    const Scratch scratch{"koi-buffer-picker-scratch"};
    const std::filesystem::path a = scratch.Write("one.txt", "alpha\n");
    Editor ed;
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    RunTypableCommand(ed, "new");
    EXPECT_EQ(BufferCount(ed), std::size_t{2});

    std::size_t unnamed = BufferCount(ed);
    for (std::size_t i = 0; i < BufferCount(ed); ++i) {
      if (BufferAt(ed, i).file.empty()) unnamed = i;
    }
    EXPECT_TRUE(unnamed < BufferCount(ed));
    const std::vector<BufferRow> rows = BufferPickerRows(ed);
    EXPECT_EQ(rows.size(), BufferCount(ed));
    if (rows.size() != BufferCount(ed)) return;
    EXPECT_EQ(rows[unnamed].text, "#" + std::to_string(unnamed) + " [scratch]");

    SwitchToBuffer(ed, (unnamed == 0) ? 1 : 0);
    ChooseBufferRow(ed, "#" + std::to_string(unnamed));
    EXPECT_TRUE(ed.doc.file.empty());

    ChooseBufferRow(ed, "#99");
    EXPECT_TRUE(ed.doc.file.empty());
    EXPECT_EQ(BufferCount(ed), std::size_t{2});
    ChooseBufferRow(ed, "");
    EXPECT_EQ(BufferCount(ed), std::size_t{2});
  }
}

// The in-process picker (IN_PROCESS_PICKER.md): the filter pass, the band's
// keys and what accepting a row does. Buffers is the pilot source, so these
// drive that one -- its rows come from BufferPickerRows with no subprocess and
// no scan behind them.
void InProcessPicker() {
  namespace fs = std::filesystem;

  // More buffers than the band has rows, named so a pattern can pick a
  // scattered few of them.
  const auto six = [](Editor& ed, const Scratch& scratch) {
    for (const char* name : {"alpha.txt", "bravo.txt", "charlie.txt", "delta.txt", "echo.txt",
                             "foxtrot.txt"}) {
      EXPECT_TRUE(OpenTarget(ed, scratch.Write(name, "x\n").string()));
    }
    EXPECT_EQ(BufferCount(ed), std::size_t{6});
  };

  TEST_CASE("picker: the filter selects rows and never reorders them");
  {
    const Scratch scratch{"koi-picker-filter"};
    Editor ed;
    six(ed, scratch);
    const std::vector<BufferRow> rows = BufferPickerRows(ed);

    BufferPicker(ed);
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.prompt_kind == PromptKind::kPicker);
    EXPECT_TRUE(ed.picker != nullptr);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->rows.size(), rows.size());
    EXPECT_EQ(ed.picker->shown.size(), rows.size());

    const auto row_at = [&rows](std::string_view needle) {
      for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].text.find(needle) != std::string::npos) return i;
      }
      return rows.size();
    };
    const std::size_t alpha = row_at("alpha.txt");
    const std::size_t foxtrot = row_at("foxtrot.txt");
    EXPECT_TRUE((alpha < rows.size()) && (foxtrot < rows.size()) && (alpha < foxtrot));

    // The alternation names the last row first; what comes back is the rows'
    // own order, because the filter selects and never sorts.
    PromptInsert(ed, "foxtrot|alpha");
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{2});
    if (ed.picker->shown.size() != 2) return;
    EXPECT_EQ(ed.picker->shown[0], alpha);
    EXPECT_EQ(ed.picker->shown[1], foxtrot);
    EXPECT_EQ(ed.picker->selected, std::size_t{0});

    // Case-folded -- what is typed is a filter, not a spelling test -- and an
    // empty input is the whole ranked list back.
    ed.prompt_input = "CHARLIE";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    ed.prompt_input.clear();
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), rows.size());
    PromptCancel(ed);
  }

  TEST_CASE("picker: a half-typed pattern keeps the last good list and says why");
  {
    const Scratch scratch{"koi-picker-bad-pattern"};
    Editor ed;
    six(ed, scratch);
    BufferPicker(ed);
    if (ed.picker == nullptr) return;

    ed.prompt_input = "charlie";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    const std::vector<std::size_t> good = ed.picker->shown;

    // One keystroke further and the pattern is half a character class. The
    // band keeps what it had rather than flashing empty, and the branch row
    // says why in gai's one line rather than its whole complaint.
    ed.prompt_input = "charlie[";
    PickerRefilter(ed);
    EXPECT_TRUE(ed.picker->shown == good);
    EXPECT_TRUE(ed.status.level() == StatusLevel::kWarning);
    EXPECT_TRUE(!ed.status.text().empty());
    EXPECT_TRUE(ed.status.text().find('\n') == std::string::npos);

    // Finished, and the list moves again with nothing left said.
    ed.prompt_input = "charlie[.]";
    PickerRefilter(ed);
    EXPECT_TRUE(ed.status.empty());
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    PromptCancel(ed);
  }

  TEST_CASE("picker: a pattern that will not compile is not a card of nothing, and is not stored");
  {
    const Scratch scratch{"koi-picker-bad-seed"};
    const AsProjectRoot root{scratch.dir};
    Editor ed;
    six(ed, scratch);

    // The pattern arrives as the opening query, which is how last_picker hands
    // one back. The band keeps the last good list -- on the open that is the
    // whole one -- and the card is measured for it: a card of zero is drawn as
    // a connector and a digit with no columns left for a row.
    BufferPicker(ed, "charlie(");
    EXPECT_TRUE(ed.picker != nullptr);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{6});
    EXPECT_TRUE(ed.picker->card_w >= kPickerCardMin);
    EXPECT_TRUE(ed.status.level() == StatusLevel::kWarning);

    // Nothing compiled, so nothing is written down: the way back is the
    // unfiltered list rather than the same throw, every reopen, for good.
    std::string name;
    std::string query;
    EXPECT_TRUE(ReadLastPicker(name, query));
    EXPECT_EQ(name, std::string{"buffers"});
    EXPECT_TRUE(query.empty());

    // A good pattern, then a half-typed one over it, then a pick out of the
    // list the good one made: that is the query this list is reopened by.
    ed.prompt_input = "charlie";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    ed.prompt_input = "charlie(";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    PickerAccept(ed, 0);
    EXPECT_TRUE(ReadLastPicker(name, query));
    EXPECT_EQ(name, std::string{"buffers"});
    EXPECT_EQ(query, std::string{"charlie"});
  }

  TEST_CASE("picker: a key that edits nothing keeps the selection where it is");
  {
    const Scratch scratch{"koi-picker-noop"};
    Editor ed;
    six(ed, scratch);
    BufferPicker(ed);
    if (ed.picker == nullptr) return;

    PickerStep(ed, true);
    PickerStep(ed, true);
    EXPECT_EQ(ed.picker->selected, std::size_t{2});
    // Delete at the end of the input and ctrl-u at column 0 edit nothing and
    // still come through the filter pass. Rebuilding the list there is the
    // selection back at the top: two keys that did nothing, undoing the walk.
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->selected, std::size_t{2});
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{6});

    // An input that did change is a new list, and a new list starts at its top.
    ed.prompt_input = "bravo";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->selected, std::size_t{0});
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    PromptCancel(ed);
  }

  TEST_CASE("picker: tab and the arrows walk the whole list, and the window follows");
  {
    const Scratch scratch{"koi-picker-walk"};
    Editor ed;
    six(ed, scratch);
    BufferPicker(ed);
    if (ed.picker == nullptr) return;

    // Six rows behind a five-row band: the selection walks all six and the
    // window scrolls to keep it on the band, rather than wrapping at five.
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{6});
    EXPECT_EQ(ed.picker->window, kPickerRows);
    Presser press;
    for (std::size_t i = 1; i < kPickerRows; ++i) {
      press(ed, "tab");
      EXPECT_EQ(ed.picker->selected, i);
      // Inside the band, so nothing scrolled: the rows stay where the eyes
      // left them.
      EXPECT_EQ(ed.picker->offset, std::size_t{0});
    }
    // The sixth is one past the window, so the window moves by one and the
    // selection is its last row.
    press(ed, "tab");
    EXPECT_EQ(ed.picker->selected, kPickerRows);
    EXPECT_EQ(ed.picker->offset, std::size_t{1});

    // Off the end and round to the top, window with it.
    press(ed, "tab");
    EXPECT_EQ(ed.picker->selected, std::size_t{0});
    EXPECT_EQ(ed.picker->offset, std::size_t{0});
    press(ed, "S-tab");
    EXPECT_EQ(ed.picker->selected, std::size_t{5});
    EXPECT_EQ(ed.picker->offset, std::size_t{1});
    press(ed, "up");
    EXPECT_EQ(ed.picker->selected, std::size_t{4});
    EXPECT_EQ(ed.picker->offset, std::size_t{1});
    press(ed, "down");
    EXPECT_EQ(ed.picker->selected, std::size_t{5});
    EXPECT_TRUE(ed.prompt_active);

    // A filter short enough for the band puts the window back at the top: the
    // offset belongs to a list, and this is a new one.
    ed.prompt_input = "alpha|bravo";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{2});
    EXPECT_EQ(ed.picker->offset, std::size_t{0});
    EXPECT_EQ(ed.picker->selected, std::size_t{0});
    PromptCancel(ed);
  }

  TEST_CASE("picker: a digit opens the window's row, wherever it is scrolled");
  {
    const Scratch scratch{"koi-picker-digit-window"};
    Editor ed;
    six(ed, scratch);
    BufferPicker(ed);
    if (ed.picker == nullptr) return;

    // Scrolled by one, so the band shows rows 1..5 and `1` is the second row
    // of the list -- the digit names what is drawn beside it, not the list's
    // own numbering.
    Presser press;
    for (std::size_t i = 0; i < kPickerRows; ++i) press(ed, "tab");
    EXPECT_EQ(ed.picker->offset, std::size_t{1});
    const std::string second = ed.picker->rows[ed.picker->shown[1]].target;
    press(ed, "1");
    EXPECT_FALSE(ed.prompt_active);
    EXPECT_TRUE(ed.walk != nullptr);
    if (ed.walk == nullptr) return;
    EXPECT_EQ(ed.walk->rows[ed.walk->at].target, second);
  }

  TEST_CASE("picker: a digit opens that row, and one past the band does nothing");
  {
    const Scratch scratch{"koi-picker-digits"};
    Editor ed;
    six(ed, scratch);
    BufferPicker(ed);
    if (ed.picker == nullptr) return;

    Presser press;
    for (const std::string_view key : {"c", "h", "a", "r"}) press(ed, key);
    EXPECT_EQ(ed.prompt_input, std::string{"char"});
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});

    // The accelerators end at the band height, so 9 is still pattern text.
    press(ed, "9");
    EXPECT_EQ(ed.prompt_input, std::string{"char9"});
    EXPECT_TRUE(ed.picker->shown.empty());
    press(ed, "backspace");
    EXPECT_EQ(ed.prompt_input, std::string{"char"});

    // One row shown, so 2 is past the band's end: nothing opens, nothing is
    // typed, and the prompt stays for the pattern to be fixed.
    press(ed, "2");
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.picker != nullptr);
    EXPECT_EQ(ed.prompt_input, std::string{"char"});

    press(ed, "1");
    EXPECT_FALSE(ed.prompt_active);
    EXPECT_TRUE(ed.picker == nullptr);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"charlie.txt"});
  }

  TEST_CASE("picker: esc closes the picker and frees its rows");
  {
    const Scratch scratch{"koi-picker-esc"};
    Editor ed;
    six(ed, scratch);
    const fs::path was = ed.doc.file;
    const std::size_t buffers = BufferCount(ed);
    BufferPicker(ed);
    if (ed.picker == nullptr) return;

    Presser press;
    for (const std::string_view key : {"c", "h", "a", "r"}) press(ed, key);

    // Backspacing the query away does not take the picker with it: a scan and
    // a filter are too expensive to lose to one keystroke past the end, and
    // esc is the only way out of any prompt.
    for (int i = 0; i < 4; ++i) press(ed, "backspace");
    EXPECT_TRUE(ed.prompt_input.empty());
    press(ed, "backspace");
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.picker != nullptr);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->shown.size(), ed.picker->rows.size());

    press(ed, "esc");
    EXPECT_FALSE(ed.prompt_active);
    EXPECT_TRUE(ed.picker == nullptr);
    EXPECT_TRUE(ed.prompt_input.empty());
    EXPECT_EQ(ed.doc.file, was);
    EXPECT_EQ(BufferCount(ed), buffers);
  }

  TEST_CASE("picker: a view row is accepted as its buffer, not reopened as a file");
  {
    const Scratch scratch{"koi-picker-payload"};
    const fs::path a = scratch.Write("one.txt", "alpha\nbravo\ncharlie\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    OpenReferenceExcerpts(ed, {Symbol{a.string(), 2, 1, "bravo"}}, "bravo");
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_EQ(BufferCount(ed), std::size_t{2});
    const std::string view_name = ed.doc.view_name;

    std::size_t view_at = BufferCount(ed);
    std::size_t file_at = BufferCount(ed);
    for (std::size_t i = 0; i < BufferCount(ed); ++i) {
      (IsExcerptView(BufferAt(ed, i)) ? view_at : file_at) = i;
    }
    EXPECT_TRUE((view_at < BufferCount(ed)) && (file_at < BufferCount(ed)));
    if (view_at >= BufferCount(ed)) return;
    SwitchToBuffer(ed, file_at);

    BufferPicker(ed);
    if (ed.picker == nullptr) return;
    // The view's row carries "#index" rather than a path, so accepting it goes
    // through ChooseBufferRow to that buffer rather than opening a file named
    // after the view.
    EXPECT_EQ(ed.picker->rows[view_at].target, "#" + std::to_string(view_at));
    PickerAccept(ed, static_cast<int>(view_at));
    EXPECT_FALSE(ed.prompt_active);
    EXPECT_TRUE(ed.picker == nullptr);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_EQ(ed.doc.view_name, view_name);
    EXPECT_EQ(BufferCount(ed), std::size_t{2});
  }

  TEST_CASE("picker: a long list filters to the same rows a short one does");
  {
    const Scratch scratch{"koi-picker-jit-rows"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    // The filter pass JIT-compiles its pattern for every list. RankedFiles
    // ranks paths and never stats them, so a list this long costs no files at
    // all; what is asserted is that a big list finds the same rows a small one
    // would -- the JIT being a matter of speed only.
    constexpr std::size_t kRows = 10'001;
    ed.settings.file_filter = "i=1; while [ $i -le " + std::to_string(kRows) +
                              " ]; do echo f$i.txt; i=$((i+1)); done";
    FilePicker(ed);
    EXPECT_TRUE(ed.picker != nullptr);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->rows.size(), kRows);

    ed.prompt_input = "f9999";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    if (ed.picker->shown.size() != 1) return;
    EXPECT_EQ(ed.picker->rows[ed.picker->shown[0]].text, std::string{"f9999.txt"});
    PromptCancel(ed);
  }
}

// Step 2: the file rows are the frecency pass left structured -- the sort and
// the branch bonus are the store's, and what the band gets is the order.
void InProcessFilePickerRows() {
  TEST_CASE("picker: the file rows come out in frecency order, unvisited last");

  namespace fs = std::filesystem;
  const Scratch scratch{"koi-picker-file-rows"};
  const fs::path dir = scratch.dir;
  for (const char* name : {"alpha.txt", "bravo.txt", "charlie.txt", "delta.txt"}) {
    scratch.Write(name, "x\n");
  }

  Editor ed;
  std::string err;
  ed.project = ProjectStore::Open(dir / "state.db", err);
  EXPECT_TRUE(ed.project != nullptr);
  if (ed.project == nullptr) return;

  // The filter's own order, spelled out rather than however the tree is
  // walked, so what the unvisited rows keep is something to compare against.
  ed.settings.file_filter = "printf '%s\\n' delta.txt charlie.txt bravo.txt alpha.txt";
  const fs::path previous = fs::current_path();
  fs::current_path(dir);
  SetProjectRoot(dir);
  struct Restore {
    fs::path was;
    ~Restore() {
      SetProjectRoot({});
      fs::current_path(was);
    }
  } restore{previous};

  const std::vector<RankedFile> cold = RankedFiles(ed);
  EXPECT_EQ(cold.size(), std::size_t{4});
  if (cold.size() != 4) return;
  EXPECT_EQ(cold[0].path, std::string{"delta.txt"});
  EXPECT_EQ(cold[3].path, std::string{"alpha.txt"});
  for (const RankedFile& file : cold) {
    EXPECT_FALSE(file.visited);
    EXPECT_EQ(file.line, Index{1});
    EXPECT_EQ(file.column, Index{1});
  }

  // An edit is worth three visits, so bravo takes the head and the two the
  // store has never heard of stay where the filter put them.
  ed.project->RecordVisit("charlie.txt", 7, 2);
  ed.project->RecordEdit("bravo.txt", 3, 1);

  const std::vector<RankedFile> warm = RankedFiles(ed);
  EXPECT_EQ(warm.size(), std::size_t{4});
  if (warm.size() != 4) return;
  EXPECT_EQ(warm[0].path, std::string{"bravo.txt"});
  EXPECT_EQ(warm[1].path, std::string{"charlie.txt"});
  EXPECT_EQ(warm[2].path, std::string{"delta.txt"});
  EXPECT_EQ(warm[3].path, std::string{"alpha.txt"});

  // The store's line and column ride the row: the pick opens where you left.
  EXPECT_TRUE(warm[0].visited);
  EXPECT_EQ(warm[0].line, Index{3});
  EXPECT_EQ(warm[0].column, Index{1});
  EXPECT_TRUE(warm[1].visited);
  EXPECT_EQ(warm[1].line, Index{7});
  EXPECT_FALSE(warm[3].visited);
}

// Step 3: defs, refs and this file's symbols on the same band. What is new
// here is what the rows carry -- a symbol list's name, or a def's or ref's own
// line read lazily -- and the context block that follows the selection.
void InProcessSymbolPickers() {
  namespace fs = std::filesystem;

  TEST_CASE("picker: this file's symbols lead with what the store ranks highest");
  {
    const Scratch scratch{"koi-picker-file-symbols"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    scratch.Write("syms.cpp",
                  "int Alpha() { return 0; }\n"
                  "int Bravo() { return 1; }\n"
                  "int Charlie() { return 2; }\n");

    Editor ed;
    std::string err;
    ed.project = ProjectStore::Open(scratch.dir / "state.db", err);
    EXPECT_TRUE(ed.project != nullptr);
    if (ed.project == nullptr) return;
    EXPECT_TRUE(OpenTarget(ed, "syms.cpp"));

    // Cold: the file's own order, which is what the subprocess printed.
    BufferSymbolPicker(ed);
    EXPECT_TRUE(ed.picker != nullptr);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{3});
    EXPECT_EQ(ed.picker->rows[0].name, std::string{"Alpha"});
    EXPECT_EQ(ed.picker->rows[2].name, std::string{"Charlie"});
    // The row is the name, with where it is dimmed at the right edge. Nothing
    // read it off anything: a symbol list's rows are complete at parse time.
    EXPECT_EQ(ed.picker->rows[0].detail, std::string{"syms.cpp:1"});
    EXPECT_EQ(ed.picker->rows[0].text, std::string{"Alpha"});
    EXPECT_EQ(ed.picker->rows[2].text, std::string{"Charlie"});
    EXPECT_FALSE(ed.picker->rows[0].read);
    EXPECT_TRUE(ed.picker->lines.empty());
    // The block still previews the selection's lines -- off the open buffer's
    // table here, which is why the line cache stays empty.
    EXPECT_EQ(ed.picker->context_first, Index{1});
    EXPECT_EQ(ed.picker->context[0], std::string{"int Alpha() { return 0; }"});
    PromptCancel(ed);

    // Warm: the head is what the database ranks, the rest keeps file order.
    ed.project->RecordSymbolVisit("Charlie", "syms.cpp", 3);
    ed.project->RecordSymbolVisit("Charlie", "syms.cpp", 3);
    BufferSymbolPicker(ed);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{3});
    EXPECT_EQ(ed.picker->rows[0].name, std::string{"Charlie"});
    EXPECT_EQ(ed.picker->rows[1].name, std::string{"Alpha"});
    EXPECT_EQ(ed.picker->rows[2].name, std::string{"Bravo"});
    // Typing filters the names, which is what the rows are: looking a symbol up
    // by its name is the whole point of this list.
    ed.prompt_input = "Bravo";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    EXPECT_EQ(ed.picker->rows[ed.picker->shown[0]].name, std::string{"Bravo"});
    // The line the row points at is not what was matched against: filtering a
    // symbol list opens no file.
    ed.prompt_input = "return 1";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{0});
    EXPECT_TRUE(ed.picker->lines.empty());
    PromptCancel(ed);

    // A symbol picker opened on a pattern that throws: the card is measured and
    // the block is filled, because the open's own pass is what does both and a
    // query that threw used to leave before either. A block-drawing source, so
    // an eight-column card would be a card with a block and no rows in it.
    BufferSymbolPicker(ed, "Alpha(");
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});
    EXPECT_TRUE(ed.picker->card_w >= kPickerCardMin);
    EXPECT_TRUE(ed.status.level() == StatusLevel::kWarning);
    EXPECT_FALSE(ed.picker->context.empty());
    EXPECT_EQ(ed.picker->context_target, Index{3});
    PromptCancel(ed);
  }

  // One definition per file, one open buffer looking at none of them.
  const auto defs_in = [](const Scratch& scratch, Editor& ed, int count) {
    std::string filter = "printf '%s\\n'";
    for (int i = 0; i < count; ++i) {
      const std::string name = "def" + std::to_string(i) + ".cpp";
      filter += " " + scratch
                          .Write(name, "// above\nstruct Widget { int a" + std::to_string(i) +
                                           "; };\n// below\n")
                          .string();
    }
    ed.settings.file_filter = filter;
    EXPECT_TRUE(OpenTarget(ed, scratch.Write("caller.cpp", "void Use() { Widget w; }\n").string()));
    ed.doc.selections.Set(Selection{13, 19});
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range()),
              std::string{"Widget"});
  };

  // Six of them: the band shows five, so five files are read.
  const auto six_defs = [&defs_in](const Scratch& scratch, Editor& ed) {
    defs_in(scratch, ed, 6);
  };

  TEST_CASE("picker: only the rows on the band read their file");
  {
    const Scratch scratch{"koi-picker-lazy-lines"};
    Editor ed;
    six_defs(scratch, ed);

    GotoDefinition(ed);
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.picker != nullptr);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{6});

    // Every row arrives owning the line it points at: the scan that found the
    // hit had the file's bytes open, so the line came back with the row.
    for (std::size_t i = 0; i < 6; ++i) {
      EXPECT_EQ(ed.picker->rows[i].text, "struct Widget { int a" + std::to_string(i) + "; };");
    }
    // The sixth is a row like any other -- it is just not on the band, so its
    // file was never opened, never stat'ed and never read here. The word the
    // picker opened with is the search term, not a filter, so it drove no pass
    // over the rows either.
    EXPECT_EQ(ed.prompt_input, std::string{"Widget"});
    EXPECT_FALSE(ed.picker->rows[5].read);
    EXPECT_EQ(ed.picker->lines.size(), kPickerRows);
    EXPECT_TRUE(!ed.picker->lines.contains(ed.picker->rows[5].target));

    // Typing past that word is a filter, and a filter matches the line -- on
    // the line the row already has. The sixth row is judged and dropped without
    // its file being touched, and the only file the pass leaves warm is the one
    // the band landed on, which was warm already.
    ed.prompt_input = "int a1";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    EXPECT_EQ(ed.picker->shown[0], std::size_t{1});
    EXPECT_FALSE(ed.picker->rows[5].read);
    EXPECT_EQ(ed.picker->lines.size(), kPickerRows);
    EXPECT_TRUE(!ed.picker->lines.contains(ed.picker->rows[5].target));
    PromptCancel(ed);
  }

  TEST_CASE("picker: a filter judges rows whose files are gone");
  {
    namespace fs = std::filesystem;
    const Scratch scratch{"koi-picker-read-free-filter"};
    Editor ed;
    six_defs(scratch, ed);

    GotoDefinition(ed);
    EXPECT_TRUE(ed.picker != nullptr);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{6});

    // Every file the hits came from, deleted under the open picker. A filter
    // pass that read them could not match on their lines at all; this one has
    // the lines and never asks.
    for (std::size_t i = 0; i < ed.picker->rows.size(); ++i) {
      std::error_code ec;
      fs::remove(ed.picker->rows[i].target, ec);
      EXPECT_FALSE(static_cast<bool>(ec));
    }
    ed.picker->lines.clear();

    ed.prompt_input = "int a5";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    EXPECT_EQ(ed.picker->shown[0], std::size_t{5});
    EXPECT_TRUE(ed.picker->lines.empty());
    // The band still says what the row came with -- the re-read found nothing,
    // and a row that cannot be freshened keeps what it had rather than blanking.
    EXPECT_EQ(ed.picker->rows[5].text, std::string{"struct Widget { int a5; };"});
    // Nothing to read means nothing to show around it.
    EXPECT_TRUE(ed.picker->context.empty());
    PromptCancel(ed);
  }

  TEST_CASE("picker: scrolling the window reads the rows it brings onto the band");
  {
    const Scratch scratch{"koi-picker-scroll-lines"};
    Editor ed;
    six_defs(scratch, ed);

    GotoDefinition(ed);
    if (ed.picker == nullptr) return;
    EXPECT_FALSE(ed.picker->rows[5].read);

    // Five steps take the selection past the window's end; the window scrolls
    // by one, and the row it brought on reads its line then and not before.
    for (std::size_t i = 0; i < kPickerRows; ++i) PickerStep(ed, true);
    EXPECT_EQ(ed.picker->offset, std::size_t{1});
    EXPECT_EQ(ed.picker->selected, kPickerRows);
    EXPECT_TRUE(ed.picker->rows[5].read);
    EXPECT_EQ(ed.picker->rows[5].text, std::string{"struct Widget { int a5; };"});
    EXPECT_EQ(ed.picker->lines.size(), std::size_t{6});
    // The block follows the selection wherever the window is.
    EXPECT_EQ(ed.picker->context[1], std::string{"struct Widget { int a5; };"});
    PromptCancel(ed);
  }

  TEST_CASE("picker: a file rewritten under the picker is read again");
  {
    const Scratch scratch{"koi-picker-stamp"};
    Editor ed;
    six_defs(scratch, ed);

    GotoDefinition(ed);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->rows[1].text, std::string{"struct Widget { int a1; };"});

    // Same line, other bytes: the stamp moves, so the cached lines are not the
    // file's any more and the next fill reads it instead of believing itself.
    scratch.Write("def1.cpp", "// above\nstruct Widget { int a1; int more; };\n// below\n");
    PickerStep(ed, true);
    EXPECT_EQ(ed.picker->rows[1].text, std::string{"struct Widget { int a1; int more; };"});
    EXPECT_EQ(ed.picker->lines.size(), kPickerRows);
    // The card was measured when the filter last changed, so a row growing
    // under the band does not widen it.
    const int card = ed.picker->card_w;
    PickerStep(ed, true);
    EXPECT_EQ(ed.picker->card_w, card);
    PromptCancel(ed);
  }

  TEST_CASE("picker: the line cache is flat bytes, and its views outlive a lookup");
  {
    const Scratch scratch{"koi-picker-flat-lines"};
    Editor ed;
    six_defs(scratch, ed);

    GotoDefinition(ed);
    if (ed.picker == nullptr) return;
    const std::string& path = ed.picker->rows[0].target;
    EXPECT_TRUE(ed.picker->lines.contains(path));
    if (!ed.picker->lines.contains(path)) return;
    const PickerFileLines& file = ed.picker->lines.at(path);
    // One buffer and an index into it, not a string per line.
    EXPECT_EQ(file.LineCount(), std::size_t{3});
    EXPECT_EQ(file.bytes.size(), std::size_t{9 + 27 + 9});
    EXPECT_EQ(file.Line(0), std::string_view{"// above"});
    EXPECT_EQ(file.Line(1), std::string_view{"struct Widget { int a0; };"});
    EXPECT_EQ(file.Line(2), std::string_view{"// below"});
    // Past the end is nothing, not a read off the end of the bytes.
    EXPECT_TRUE(file.Line(3).empty());

    // A view handed out now must still read the same bytes after the cache has
    // been asked again -- a step is a hit on this file, and reading the sixth
    // file inserts a node the map may rehash for.
    const std::string_view line = file.Line(1);
    const char* at = line.data();
    PickerStep(ed, true);
    ed.prompt_input = "int a5";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->lines.size(), std::size_t{6});
    EXPECT_EQ(line, std::string_view{"struct Widget { int a0; };"});
    EXPECT_TRUE(line.data() == at);

    // The line ends at its newline and the \r before it is the file's, not the
    // line's -- rewritten with CRLF, the re-read says the same thing.
    scratch.Write("def0.cpp", "// above\r\nstruct Widget { int a0; };\r\n// below\r\n");
    ed.prompt_input.clear();
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->rows[0].text, std::string{"struct Widget { int a0; };"});
    EXPECT_EQ(ed.picker->context[0], std::string{"// above"});
    PromptCancel(ed);
  }

  TEST_CASE("picker: the line cache is capped, and a file it dropped is read again");
  {
    const Scratch scratch{"koi-picker-cache-cap"};
    Editor ed;
    // More files than the cap, so a walk that visits every row must drop some.
    const int count = static_cast<int>(kPickerCacheFiles) + 4;
    defs_in(scratch, ed, count);

    GotoDefinition(ed);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->rows.size(), static_cast<std::size_t>(count));
    EXPECT_EQ(ed.picker->lines.size(), kPickerRows);

    // Walking the list is what reads: every step brings a row onto the band and
    // takes the block with it, so twenty files are read -- and the cache holds
    // its cap of them and no more, however long the session runs.
    for (int i = 1; i < count; ++i) PickerStep(ed, true);
    EXPECT_EQ(ed.picker->selected, static_cast<std::size_t>(count - 1));
    EXPECT_EQ(ed.picker->lines.size(), kPickerCacheFiles);
    // The band's own files are the warmest, so they are never what goes.
    const std::size_t last = ed.picker->rows.size() - 1;
    for (std::size_t i = (last + 1) - kPickerRows; i <= last; ++i) {
      EXPECT_TRUE(ed.picker->lines.contains(ed.picker->rows[i].target));
    }

    // Whichever file the cap dropped: stepping the window onto its row reads it
    // again, and says what the file says.
    std::size_t dropped = ed.picker->rows.size();
    for (std::size_t i = 0; i < ed.picker->rows.size(); ++i) {
      if (!ed.picker->lines.contains(ed.picker->rows[i].target)) {
        dropped = i;
        break;
      }
    }
    EXPECT_TRUE(dropped < ed.picker->rows.size());
    if (dropped >= ed.picker->rows.size()) return;
    // One step wraps off the end onto the head, then down to the row itself.
    for (std::size_t i = 0; i <= dropped; ++i) PickerStep(ed, true);
    EXPECT_EQ(ed.picker->selected, dropped);
    EXPECT_TRUE(ed.picker->lines.contains(ed.picker->rows[dropped].target));
    EXPECT_EQ(ed.picker->rows[dropped].text,
              "struct Widget { int a" + std::to_string(dropped) + "; };");
    EXPECT_EQ(ed.picker->lines.size(), kPickerCacheFiles);
    PromptCancel(ed);
  }

  TEST_CASE("picker: a file past the ceiling is not read, and the row still says something");
  {
    const Scratch scratch{"koi-picker-file-cap"};
    Editor ed;
    six_defs(scratch, ed);

    GotoDefinition(ed);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->rows[0].text, std::string{"struct Widget { int a0; };"});
    EXPECT_FALSE(ed.picker->context.empty());

    // The same file with a generated blob appended -- a bundled script, a
    // dump, a minified checkout -- which is what a picker steps into most and
    // what reading a whole file to show three lines of it costs the most on.
    // The definition is where it was; only the size moved.
    std::string big = "// above\nstruct Widget { int a0; };\n// below\n";
    big.append(static_cast<std::size_t>(ed.picker->file_max) + 1, 'x');
    scratch.Write("def0.cpp", big);
    ed.picker->lines.clear();

    // Off the row and back, so the band and the block both ask for it again.
    PickerStep(ed, true);
    PickerStep(ed, false);
    EXPECT_EQ(ed.picker->selected, std::size_t{0});
    // Not read and not cached: the other four band files are the whole cache.
    EXPECT_TRUE(!ed.picker->lines.contains(ed.picker->rows[0].target));
    EXPECT_EQ(ed.picker->lines.size(), kPickerRows - 1);
    // The row keeps the line the scan gave it rather than blanking, and the
    // block draws nothing -- what a row whose file went missing already does.
    EXPECT_EQ(ed.picker->rows[0].text, std::string{"struct Widget { int a0; };"});
    EXPECT_TRUE(ed.picker->context.empty());
    PromptCancel(ed);
  }

  TEST_CASE("picker: the context block follows the selection");
  {
    const Scratch scratch{"koi-picker-context"};
    Editor ed;
    six_defs(scratch, ed);

    GotoDefinition(ed);
    if (ed.picker == nullptr) return;
    EXPECT_TRUE(PickerShowsContext(ed.picker->source));
    EXPECT_EQ(ed.picker->context.size(), kPickerContext);
    EXPECT_EQ(ed.picker->context_first, Index{1});
    EXPECT_EQ(ed.picker->context[1], std::string{"struct Widget { int a0; };"});
    EXPECT_EQ(ed.picker->context[2], std::string{"// below"});

    PickerStep(ed, true);
    EXPECT_EQ(ed.picker->selected, std::size_t{1});
    EXPECT_EQ(ed.picker->context[1], std::string{"struct Widget { int a1; };"});

    // Back to the top, and nothing about the rows moved with the block.
    PickerStep(ed, false);
    EXPECT_EQ(ed.picker->context[1], std::string{"struct Widget { int a0; };"});
    EXPECT_EQ(ed.picker->rows[0].name, std::string{"Widget"});
    PromptCancel(ed);
  }

  TEST_CASE("picker: a band over file content takes the screen; a file band is measured");
  {
    const Scratch scratch{"koi-picker-card-width"};
    Editor ed;
    // Three definitions on lines of wildly different lengths: what the card is
    // drawn at must be one answer for all of them, or stepping resizes it.
    std::string filter = "printf '%s\\n'";
    const std::array<std::string, 3> tails{"", " // and a much longer tail than the others have",
                                           " // short"};
    for (std::size_t i = 0; i < tails.size(); ++i) {
      const std::string name = "w" + std::to_string(i) + ".cpp";
      filter += " " + scratch.Write(name, "struct Widget { int a; };" + tails[i] + "\n").string();
    }
    // A fourth with no definition in it and a much longer name, so that a file
    // band measured over the four narrows when it is filtered out.
    filter += " " + scratch.Write("a-file-with-a-considerably-longer-name.cpp", "x\n").string();
    ed.settings.file_filter = filter;
    EXPECT_TRUE(OpenTarget(ed, scratch.Write("caller.cpp", "void Use() { Widget w; }\n").string()));
    ed.doc.selections.Set(Selection{13, 19});

    GotoDefinition(ed);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});
    // Rows that are lines of source: the card is the screen, and no row was
    // measured to say so. Nothing a step or a filter does moves it.
    EXPECT_EQ(ed.picker->card_w, kPickerCardWide);
    for (std::size_t i = 0; i < 4; ++i) {
      PickerStep(ed, true);
      EXPECT_EQ(ed.picker->card_w, kPickerCardWide);
    }
    ed.prompt_input = "short";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    EXPECT_EQ(ed.picker->card_w, kPickerCardWide);
    PromptCancel(ed);

    // A file band is the other rule: one path per row, so it is measured to the
    // widest of them and narrows with the list -- never past the minimum.
    FilePicker(ed, "");
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{4});
    const int card = ed.picker->card_w;
    EXPECT_TRUE((card > kPickerCardMin) && (card < kPickerCardWide));
    for (std::size_t i = 0; i < 4; ++i) {
      PickerStep(ed, true);
      EXPECT_EQ(ed.picker->card_w, card);
    }
    ed.prompt_input = "w0";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    EXPECT_TRUE(ed.picker->card_w < card);
    PromptCancel(ed);
  }

  TEST_CASE("picker: a file or buffer row has no line and no block");
  {
    const Scratch scratch{"koi-picker-no-context"};
    Editor ed;
    for (const char* name : {"alpha.txt", "bravo.txt"}) {
      EXPECT_TRUE(OpenTarget(ed, scratch.Write(name, "x\n").string()));
    }
    BufferPicker(ed);
    if (ed.picker == nullptr) return;
    EXPECT_FALSE(PickerShowsContext(ed.picker->source));
    EXPECT_TRUE(ed.picker->context.empty());
    EXPECT_FALSE(ed.picker->rows[0].read);
    EXPECT_TRUE(ed.picker->lines.empty());
    PromptCancel(ed);
  }

  TEST_CASE("picker: one definition still jumps without a prompt");
  {
    const Scratch scratch{"koi-picker-lone-hit"};
    Editor ed;
    const fs::path defs = scratch.Write("defs.cpp", "struct Widget { int size; };\n");
    ed.settings.file_filter = "printf '%s\\n' " + defs.string();
    EXPECT_TRUE(OpenTarget(ed, scratch.Write("caller.cpp", "void Use() { Widget w; }\n").string()));
    ed.doc.selections.Set(Selection{13, 19});

    GotoDefinition(ed);
    EXPECT_FALSE(ed.prompt_active);
    EXPECT_TRUE(ed.picker == nullptr);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"defs.cpp"});
  }

  TEST_CASE("picker: accepting a symbol row records the visit, and space-space comes back");
  {
    const Scratch scratch{"koi-picker-symbol-visit"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    six_defs(scratch, ed);
    std::string err;
    ed.project = ProjectStore::Open(scratch.dir / "state.db", err);
    EXPECT_TRUE(ed.project != nullptr);
    if (ed.project == nullptr) return;
    SetProjectDbPath(scratch.dir / "state.db");

    GotoDefinition(ed);
    if (ed.picker == nullptr) return;
    const std::string target = ed.picker->rows[0].target;
    PickerAccept(ed, 0);
    EXPECT_FALSE(ed.prompt_active);
    EXPECT_EQ(ed.doc.file.filename().string(), fs::path{target}.filename().string());

    const std::vector<SymbolVisit> hot = ed.project->HotSymbols(10);
    EXPECT_EQ(hot.size(), std::size_t{1});
    if (hot.empty()) return;
    EXPECT_EQ(hot.front().symbol, std::string{"Widget"});

    // The word is what was recorded, not what the band was filtered by, so
    // reopening looks the same thing up -- in process, on the same band.
    std::string name;
    std::string query;
    EXPECT_TRUE(ReadLastPicker(name, query));
    EXPECT_EQ(name, std::string{"definition"});
    EXPECT_EQ(query, std::string{"Widget"});
    LastPicker(ed);
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.prompt_kind == PromptKind::kPicker);
    EXPECT_TRUE(ed.picker != nullptr);
    if (ed.picker != nullptr) {
      EXPECT_TRUE(ed.picker->source == PickerState::Source::kDefs);
      // The word is typed in as the search term it is, and the band still opens
      // whole: every row was found for that word, so it filters nothing out.
      EXPECT_EQ(ed.prompt_input, std::string{"Widget"});
      EXPECT_EQ(ed.picker->shown.size(), ed.picker->rows.size());

      // Even for a word that is not a pattern: the term is not compiled while
      // it is still the term, so a row cannot vanish over how a name spells.
      ed.picker->query = "operator[]";
      ed.prompt_input = "operator[]";
      PickerRefilter(ed);
      EXPECT_EQ(ed.picker->shown.size(), ed.picker->rows.size());
      EXPECT_TRUE(ed.status.empty());
    }
    PromptCancel(ed);
  }
}

// Step 4: the walk. picker_jump_next/_prev are one pair of keys over two lists
// -- smart-jump's matches and a picker's accepted rows -- and what is tested
// here is the picker half of that, plus the handoff between the two.
void PickerWalk() {
  namespace fs = std::filesystem;

  // Six buffers, named so a pattern can pick a scattered three of them.
  const auto six = [](Editor& ed, const Scratch& scratch) {
    for (const char* name : {"alpha.txt", "bravo.txt", "charlie.txt", "delta.txt", "echo.txt",
                             "foxtrot.txt"}) {
      EXPECT_TRUE(OpenTarget(ed, scratch.Write(name, "x\n").string()));
    }
    EXPECT_EQ(BufferCount(ed), std::size_t{6});
  };

  // Six buffers filtered to alpha, charlie and echo, and the first of them
  // accepted: the shape every case below walks.
  const auto three = [&six](Editor& ed, const Scratch& scratch) {
    six(ed, scratch);
    BufferPicker(ed);
    if (ed.picker == nullptr) return;
    ed.prompt_input = "alpha|charlie|echo";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});
    PickerAccept(ed, 0);
  };

  TEST_CASE("walk: the accepted list survives the prompt and steps in filtered order");
  {
    const Scratch scratch{"koi-walk-survives"};
    Editor ed;
    six(ed, scratch);
    BufferPicker(ed);
    if (ed.picker == nullptr) return;

    ed.prompt_input = "alpha|charlie|echo";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});
    std::vector<std::string> want;
    for (const std::size_t i : ed.picker->shown) want.push_back(ed.picker->rows[i].target);

    PickerAccept(ed, 0);
    // The prompt's state died with the prompt; what the band was showing did
    // not, and the rows it kept are its own.
    EXPECT_TRUE(ed.picker == nullptr);
    EXPECT_TRUE(ed.walk != nullptr);
    if (ed.walk == nullptr) return;
    EXPECT_EQ(ed.walk->rows.size(), std::size_t{3});
    EXPECT_EQ(ed.walk->at, std::size_t{0});
    EXPECT_TRUE(ed.walk->source == PickerState::Source::kBuffers);
    for (std::size_t r = 0; r < want.size(); ++r) EXPECT_EQ(ed.walk->rows[r].target, want[r]);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"alpha.txt"});

    // Shown order, not row order: the three the pattern dropped are not in the
    // list at all, so stepping never passes through bravo.
    PickerJumpStep(ed, true);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"charlie.txt"});
    EXPECT_EQ(ed.walk->at, std::size_t{1});
    PickerJumpStep(ed, true);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"echo.txt"});
    PickerJumpStep(ed, false);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"charlie.txt"});

    // Esc buries the branch row and nothing else: only a newer list replaces
    // the walk, which is smart-jump's own contract.
    Presser press;
    press(ed, "esc");
    EXPECT_FALSE(ed.jump_branch);
    EXPECT_TRUE(ed.walk != nullptr);
    PickerJumpStep(ed, true);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"echo.txt"});
  }

  TEST_CASE("walk: it wraps at both ends in smart-jump's own words");
  {
    const Scratch scratch{"koi-walk-wrap"};
    Editor ed;
    three(ed, scratch);
    if (ed.walk == nullptr) return;

    // The landing names where the next press goes, not the row under the
    // cursor, and marks the destination for the box to dress apart.
    EXPECT_TRUE(ed.status.text().starts_with("ᛃ 1/3  "));
    EXPECT_TRUE(ed.status.text().find("charlie.txt") != std::string::npos);
    EXPECT_TRUE(ed.status.target().find("charlie.txt") != std::string::npos);
    EXPECT_TRUE(ed.jump_branch);

    PickerJumpStep(ed, true);
    EXPECT_TRUE(ed.status.text().starts_with("ᛃ 2/3  "));
    PickerJumpStep(ed, true);
    // The end of the list, said as what the next press costs. The wrap words
    // stay outside the mark, as they do for a smart-jump step.
    EXPECT_TRUE(ed.status.text().starts_with("ᛃ 3/3  wraps to "));
    EXPECT_TRUE(ed.status.target().find("alpha.txt") != std::string::npos);

    PickerJumpStep(ed, true);
    EXPECT_TRUE(ed.status.text().starts_with("ᛃ 1/3  "));
    EXPECT_TRUE(ed.status.text().find("wrapped to the top") != std::string::npos);

    PickerJumpStep(ed, false);
    EXPECT_TRUE(ed.status.text().starts_with("ᛃ 3/3  "));
    EXPECT_TRUE(ed.status.text().find("wrapped to the bottom") != std::string::npos);
  }

  TEST_CASE("walk: a lone row lands in silence and then names itself");
  {
    const Scratch scratch{"koi-walk-lone"};
    Editor ed;
    six(ed, scratch);
    BufferPicker(ed);
    if (ed.picker == nullptr) return;
    ed.prompt_input = "charlie";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});

    PickerAccept(ed, 0);
    // Nothing to say: the cursor arriving is the whole answer, exactly as for a
    // smart jump that matched once.
    EXPECT_TRUE(ed.status.empty());
    EXPECT_FALSE(ed.jump_branch);
    if (ed.walk == nullptr) return;
    EXPECT_EQ(ed.walk->rows.size(), std::size_t{1});

    // A step has no next to name, so the row names itself and no wrap is
    // announced -- there was nowhere else to go.
    PickerJumpStep(ed, true);
    EXPECT_TRUE(ed.status.text().starts_with("ᛃ 1/1  "));
    EXPECT_TRUE(ed.status.text().find("wrapped") == std::string::npos);
  }

  TEST_CASE("walk: a pick takes the keys off smart-jump, and frees its list");
  {
    const Scratch scratch{"koi-walk-replaces-jump"};
    Editor ed;

    // A smart-jump list standing, the way a submit would have left it.
    ed.smart_jump = std::make_shared<SmartJumpState>();
    ed.smart_jump->matches.push_back(SmartMatch{.path = "a.txt", .display = "a.txt"});
    ed.smart_jump->matches.push_back(SmartMatch{.path = "b.txt", .display = "b.txt"});
    ed.smart_jump->typed = "a";
    ed.smart_jump->at = 1;

    three(ed, scratch);
    if (ed.walk == nullptr) return;
    // Whichever list was made last owns the keys, and the one it replaced is
    // freed rather than left where a stray press could reach it.
    EXPECT_TRUE(ed.smart_jump->matches.empty());
    EXPECT_EQ(ed.smart_jump->matches.capacity(), std::size_t{0});
    EXPECT_TRUE(ed.smart_jump->typed.empty());
    EXPECT_EQ(ed.smart_jump->at, std::size_t{0});

    // And the dispatch proves it: n walks the pick, not the two paths above,
    // neither of which exists to be opened.
    PickerJumpStep(ed, true);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"charlie.txt"});
    EXPECT_EQ(ed.walk->at, std::size_t{1});
  }

  TEST_CASE("walk: past kWalkRows the list is capped rather than kept whole");
  {
    const Scratch scratch{"koi-walk-cap"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    const fs::path real = scratch.Write("real.txt", "x\n");
    // One file that exists, then kWalkRows + 1 names that do not: RankedFiles
    // ranks paths and never stats them, so this is the cheap way to a list
    // longer than anyone would step through.
    ed.settings.file_filter = "echo " + real.string() + "; i=0; while [ $i -le " +
                              std::to_string(kWalkRows) + " ]; do echo f$i.txt; i=$((i+1)); done";

    FilePicker(ed);
    EXPECT_TRUE(ed.picker != nullptr);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->shown.size(), kWalkRows + 2);

    // The selection can walk the whole list, so a pick can come from past the
    // cap: the rows kept are a window around it, and the row picked is in them.
    ed.picker->selected = ed.picker->shown.size() - 1;
    const std::string deep = ed.picker->rows[ed.picker->shown.back()].target;
    PickerAccept(ed);
    EXPECT_TRUE(ed.walk != nullptr);
    if (ed.walk == nullptr) return;
    EXPECT_EQ(ed.walk->rows.size(), kWalkRows);
    EXPECT_EQ(ed.walk->rows[ed.walk->at].target, deep);
    EXPECT_EQ(ed.walk->at, kWalkRows - 1);

    FilePicker(ed);
    if (ed.picker == nullptr) return;
    PickerAccept(ed, 0);
    if (ed.walk == nullptr) return;
    // From the top the window is the head of the list, and the pick is its
    // first row -- stepping forward from a pick is the common case.
    EXPECT_EQ(ed.walk->rows.size(), kWalkRows);
    EXPECT_EQ(ed.walk->at, std::size_t{0});
    EXPECT_EQ(ed.walk->rows.front().target, real.string());
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"real.txt"});
  }

  TEST_CASE("walk: a symbol step records the visit, a file step records no symbol");
  {
    const Scratch scratch{"koi-walk-visits"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    std::string filter = "printf '%s\\n'";
    for (int i = 0; i < 3; ++i) {
      filter += " " + scratch
                          .Write("def" + std::to_string(i) + ".cpp",
                                 "struct Widget { int a" + std::to_string(i) + "; };\n")
                          .string();
    }
    ed.settings.file_filter = filter;
    std::string err;
    ed.project = ProjectStore::Open(scratch.dir / "state.db", err);
    EXPECT_TRUE(ed.project != nullptr);
    if (ed.project == nullptr) return;
    SetProjectDbPath(scratch.dir / "state.db");
    EXPECT_TRUE(OpenTarget(ed, scratch.Write("caller.cpp", "void Use() { Widget w; }\n").string()));
    ed.doc.selections.Set(Selection{13, 19});

    GotoDefinition(ed);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{3});
    PickerAccept(ed, 0);
    if (ed.walk == nullptr) return;
    EXPECT_TRUE(ed.walk->source == PickerState::Source::kDefs);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"def0.cpp"});

    // A landing is a visit wherever the press came from, so the step writes the
    // row the accept would have written for the same target.
    PickerJumpStep(ed, true);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"def1.cpp"});
    bool named = false;
    for (const SymbolVisit& one : ed.project->HotSymbols(10)) {
      named = named || ((one.symbol == "Widget") && one.file.ends_with("def1.cpp"));
    }
    EXPECT_TRUE(named);
  }

  TEST_CASE("walk: a file step opens without writing a symbol row");
  {
    const Scratch scratch{"koi-walk-file-visits"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    std::string err;
    ed.project = ProjectStore::Open(scratch.dir / "state.db", err);
    EXPECT_TRUE(ed.project != nullptr);
    if (ed.project == nullptr) return;
    SetProjectDbPath(scratch.dir / "state.db");
    std::string filter = "printf '%s\\n'";
    for (const char* name : {"one.txt", "two.txt"}) {
      filter += " " + scratch.Write(name, "x\n").string();
    }
    ed.settings.file_filter = filter;

    FilePicker(ed);
    if (ed.picker == nullptr) return;
    PickerAccept(ed, 0);
    PickerJumpStep(ed, true);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"two.txt"});
    // A path is not a symbol: the open records where you are, and the symbol
    // table hears nothing about it.
    EXPECT_TRUE(ed.project->HotSymbols(10).empty());
  }

  // What an unopenable target is, here and below: a missing path is not one --
  // opening a name with no file behind it is how a new file is started -- so it
  // takes a file that exists and will not load.
  const std::string_view unreadable = "\xff\xfe not utf-8\n";

  TEST_CASE("walk: a step onto a file that will not load says so and stays where it is");
  {
    const Scratch scratch{"koi-walk-step-bad"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    std::vector<fs::path> defs;
    std::string filter = "printf '%s\\n'";
    for (int i = 0; i < 3; ++i) {
      defs.push_back(scratch.Write("def" + std::to_string(i) + ".cpp",
                                   "struct Widget { int a" + std::to_string(i) + "; };\n"));
      filter += " " + defs.back().string();
    }
    ed.settings.file_filter = filter;
    std::string err;
    ed.project = ProjectStore::Open(scratch.dir / "state.db", err);
    EXPECT_TRUE(ed.project != nullptr);
    if (ed.project == nullptr) return;
    SetProjectDbPath(scratch.dir / "state.db");
    EXPECT_TRUE(OpenTarget(ed, scratch.Write("caller.cpp", "void Use() { Widget w; }\n").string()));
    ed.doc.selections.Set(Selection{13, 19});

    GotoDefinition(ed);
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{3});
    PickerAccept(ed, 0);
    if (ed.walk == nullptr) return;
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"def0.cpp"});

    // The row the next press goes to, made unreadable under the standing walk.
    scratch.Write("def1.cpp", unreadable);

    const Index was = LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary()));
    PickerJumpStep(ed, true);
    // Nowhere to land, so nothing moved -- and the reason is still on the line
    // rather than painted over with an n/m that would have lied about it.
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"def0.cpp"});
    EXPECT_EQ(LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary())), was);
    EXPECT_EQ(ed.walk->at, std::size_t{0});
    EXPECT_TRUE(ed.status.text().find("cannot open") != std::string::npos);
    EXPECT_TRUE(ed.status.text().find("2/3") == std::string::npos);
    // An arrival that did not happen is not a visit: nothing was credited.
    for (const SymbolVisit& one : ed.project->HotSymbols(10)) {
      EXPECT_FALSE(one.file.ends_with("def1.cpp"));
    }

    // The list is still good: stepping the other way lands on the row that is
    // still there, and says so.
    PickerJumpStep(ed, false);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"def2.cpp"});
    EXPECT_EQ(ed.walk->at, std::size_t{2});
    EXPECT_TRUE(ed.status.text().starts_with("ᛃ 3/3  "));
  }

  TEST_CASE("walk: a lone pick that will not open keeps the failure, not the silence");
  {
    const Scratch scratch{"koi-walk-lone-bad"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    const fs::path good = scratch.Write("good.txt", "x\n");
    const fs::path raw = scratch.Write("raw.bin", unreadable);
    ed.settings.file_filter = "printf '%s\\n' " + good.string() + " " + raw.string();

    FilePicker(ed);
    if (ed.picker == nullptr) return;
    ed.prompt_input = "raw";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});

    PickerAccept(ed, 0);
    // The lone-row arm goes silent for a landing that happened. This one did
    // not, so what the open said stands and no branch row is drawn over it.
    EXPECT_TRUE(ed.status.text().find("cannot open") != std::string::npos);
    EXPECT_FALSE(ed.jump_branch);
    EXPECT_TRUE(ed.doc.file.empty());
    // Kept, so the list is still there to step off.
    EXPECT_TRUE(ed.walk != nullptr);
  }

  TEST_CASE("walk: a buffers walk goes when a buffer does");
  {
    const Scratch scratch{"koi-walk-buffer-close"};
    Editor ed;
    three(ed, scratch);
    if (ed.walk == nullptr) return;
    EXPECT_TRUE(ed.walk->source == PickerState::Source::kBuffers);

    // A row is a position in the buffer list, so a close re-points every row
    // past the one that went: the list is dropped rather than repaired.
    RunTypableCommand(ed, "bc!");
    EXPECT_TRUE(ed.walk == nullptr);
    // And the keys fall back to smart-jump, which has nothing to step.
    PickerJumpStep(ed, true);
    EXPECT_TRUE(ed.status.text().find("no smart jump") != std::string::npos);
  }

  TEST_CASE("walk: a files walk survives a buffer close");
  {
    const Scratch scratch{"koi-walk-files-close"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    std::string filter = "printf '%s\\n'";
    for (const char* name : {"one.txt", "two.txt"}) {
      filter += " " + scratch.Write(name, "x\n").string();
    }
    ed.settings.file_filter = filter;

    FilePicker(ed);
    if (ed.picker == nullptr) return;
    PickerAccept(ed, 0);
    if (ed.walk == nullptr) return;

    // A file row names a path, which a close does not move.
    RunTypableCommand(ed, "bc!");
    EXPECT_TRUE(ed.walk != nullptr);
    PickerJumpStep(ed, true);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"two.txt"});
  }
}

// Step 5: the project-symbol scan stays a subprocess and the prompt consumes
// its stdout. Everything here drives the pump by hand -- most of it over a pipe
// the test writes into, which is the only way an append is a fact rather than a
// race -- and the last two over a real child, which is what has to be killed.
namespace {

// A picker prompt with a pipe where its child would be. Rows go in one write at
// a time and the pump is called where the band can be counted.
struct PipedPicker {
  Editor& ed;
  int in{-1};

  PipedPicker(Editor& editor, PickerState::Source source) : ed(editor) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(pipe2(fds, O_NONBLOCK), 0);
    auto state = std::make_shared<PickerState>();
    state->source = source;
    state->scan = std::make_unique<PickerScan>();
    state->scan->fd = fds[0];
    ed.picker = std::move(state);
    PromptOpen(ed, PromptKind::kPicker);
    in = fds[1];
  }
  ~PipedPicker() { Close(); }
  PipedPicker(const PipedPicker&) = delete;
  PipedPicker& operator=(const PipedPicker&) = delete;

  void Send(std::string_view bytes) const {
    EXPECT_EQ(write(in, bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
  }
  void Close() {
    if (in >= 0) close(in);
    in = -1;
  }
};

// A row in the format `--picker-rows` prints: the display, the padding, a tab,
// then the payload the picker opens. Built by the same function the scan uses.
std::string ScanRow(const std::filesystem::path& path, Index line, std::string_view name) {
  return SymbolPickerRow(Symbol{.path = path.string(), .line = line, .column = 1,
                                .name = std::string{name}}) +
         "\n";
}

// Gone, and waited for: ECHILD is what waitpid says about a pid this process
// has already reaped, and anything else is a child or a zombie left behind.
bool ChildIsGone(int pid) {
  int status = 0;
  errno = 0;
  const pid_t got = waitpid(pid, &status, WNOHANG);
  return (got < 0) && (errno == ECHILD);
}

// A content query change rides the pump: the pattern is put down and carried
// over the corpus a budget of bytes per wake, so a test that types has to let
// the catch-up land before it counts the band.
void PumpRefilter(Editor& ed) {
  for (int tries = 0; tries < 500; ++tries) {
    if (!ed.picker || !ed.picker->scan || !ed.picker->scan->refiltering) return;
    PickerPumpScan(ed);
  }
}

// A child that says nothing still has to be waited for: pump until the pipe
// closes, which is all that ends a scan with no rows in it.
bool PumpUntilDone(Editor& ed) {
  for (int tries = 0; tries < 500; ++tries) {
    PickerPumpScan(ed);
    if (!ed.picker || !ed.picker->scan || ed.picker->scan->done) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  return false;
}

// A real child answers when it answers: pump until the rows are there or give
// up, so a slow machine fails the assertion rather than the schedule.
bool PumpUntilRows(Editor& ed, std::size_t want) {
  for (int tries = 0; tries < 500; ++tries) {
    PickerPumpScan(ed);
    if (ed.picker && (ed.picker->rows.size() >= want)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  return false;
}

}

void StreamingSymbolPicker() {
  namespace fs = std::filesystem;

  TEST_CASE("picker: the symbol scan is the file filter into koi's own symbol mode");
  {
    Editor ed;
    ed.doc.file = "src/a.cpp";
    const std::string scan = PickerScanCommand(ed, "symbols");
    EXPECT_TRUE(!scan.empty());
    // `--picker-rows` is the wire between the editor and this child: display,
    // padding, tab, payload, which is what PickerPumpScan parses back.
    EXPECT_TRUE(scan.find("--symbol-mode --picker-rows --definitions") != std::string::npos);
    // The hot head lands first and never moves, which is what lets the band
    // append what follows it without ever reordering.
    EXPECT_TRUE(scan.find("--hot-first --from") != std::string::npos);
    EXPECT_TRUE(scan.find("--files -") != std::string::npos);
    // Nothing dangling for a shell to wait on the other side of.
    EXPECT_TRUE(scan.back() != '|');
    const std::string expanded = ExpandVariables(scan, ed);
    EXPECT_TRUE(expanded.find("%{") == std::string::npos);
    EXPECT_TRUE(expanded.find("--from src/a.cpp") != std::string::npos);
  }

  // Three files, one line each: a row's text is its name, but the block reads
  // the selection's target, so the rows the scan sends point at something real.
  const auto lines = [](const Scratch& scratch, int count) {
    std::vector<fs::path> paths;
    for (int i = 0; i < count; ++i) {
      const std::string name = "s" + std::to_string(i) + ".cpp";
      paths.push_back(scratch.Write(name, "int Sym" + std::to_string(i) + "() { return 0; }\n"));
    }
    return paths;
  };

  TEST_CASE("picker: rows join at the bottom, and nothing already on the band moves");
  {
    const Scratch scratch{"koi-scan-append"};
    Editor ed;
    const std::vector<fs::path> paths = lines(scratch, 6);
    PipedPicker piped{ed, PickerState::Source::kProjectSymbols};

    for (int i = 0; i < 3; ++i) piped.Send(ScanRow(paths[i], 1, "Sym" + std::to_string(i)));
    EXPECT_TRUE(PickerPumpScan(ed));
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{3});
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});
    EXPECT_EQ(ed.picker->rows[0].name, std::string{"Sym0"});
    // The row is the name the scan sent, with where it is at the right edge.
    EXPECT_EQ(ed.picker->rows[0].text, std::string{"Sym0"});
    EXPECT_EQ(ed.picker->rows[0].detail, paths[0].string() + ":1");

    // Somewhere in the middle of the list, which is where an insert would show.
    PickerStep(ed, true);
    PickerStep(ed, true);
    EXPECT_EQ(ed.picker->selected, std::size_t{2});
    const std::size_t was = ed.picker->shown[2];

    for (int i = 3; i < 6; ++i) piped.Send(ScanRow(paths[i], 1, "Sym" + std::to_string(i)));
    EXPECT_TRUE(PickerPumpScan(ed));
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{6});
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{6});
    // The head the child sent first is still the head, the selection is still
    // on the row it was on, and every index it was standing on means what it
    // meant. That is the whole promise of appending.
    EXPECT_EQ(ed.picker->rows[0].name, std::string{"Sym0"});
    EXPECT_EQ(ed.picker->selected, std::size_t{2});
    EXPECT_EQ(ed.picker->shown[2], was);
    for (std::size_t i = 0; i < 6; ++i) EXPECT_EQ(ed.picker->shown[i], i);
    PromptCancel(ed);
  }

  TEST_CASE("picker: half a row waits for the rest of it");
  {
    const Scratch scratch{"koi-scan-carry"};
    Editor ed;
    const std::vector<fs::path> paths = lines(scratch, 1);
    PipedPicker piped{ed, PickerState::Source::kProjectSymbols};

    const std::string row = ScanRow(paths[0], 1, "Sym0");
    piped.Send(row.substr(0, row.size() / 2));
    PickerPumpScan(ed);
    // Nothing complete, so nothing parsed -- and the bytes are not lost either.
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{0});
    EXPECT_EQ(ed.picker->scan->parsed, std::size_t{0});

    piped.Send(row.substr(row.size() / 2));
    EXPECT_TRUE(PickerPumpScan(ed));
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{1});
    EXPECT_EQ(ed.picker->rows[0].name, std::string{"Sym0"});
    EXPECT_EQ(ed.picker->rows[0].target, paths[0].string());
    PromptCancel(ed);
  }

  TEST_CASE("picker: pipe close flushes the tail and drops the scanning note");
  {
    const Scratch scratch{"koi-scan-eof"};
    Editor ed;
    const std::vector<fs::path> paths = lines(scratch, 2);
    PipedPicker piped{ed, PickerState::Source::kProjectSymbols};

    std::string last = ScanRow(paths[1], 1, "Sym1");
    last.pop_back();  // no newline: the child stopped mid-row
    piped.Send(ScanRow(paths[0], 1, "Sym0") + last);
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{1});
    EXPECT_TRUE(PickerScanLive(*ed.picker));
    EXPECT_TRUE(PickerScanning(ed));

    // Close is what says the tail is all there is of that row.
    piped.Close();
    EXPECT_TRUE(PickerPumpScan(ed));
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{2});
    EXPECT_EQ(ed.picker->rows[1].name, std::string{"Sym1"});
    EXPECT_FALSE(PickerScanLive(*ed.picker));
    EXPECT_FALSE(PickerScanning(ed));
    // Nothing left to poll for, and nothing left to read.
    EXPECT_TRUE(ed.picker->scan->done);
    EXPECT_FALSE(PickerPumpScan(ed));
    PromptCancel(ed);
  }

  TEST_CASE("picker: a scan far bigger than the mapping keeps every row");
  {
    const Scratch scratch{"koi-scan-big"};
    Editor ed;
    const std::vector<fs::path> paths = lines(scratch, 1);
    PipedPicker piped{ed, PickerState::Source::kProjectSymbols};

    // Enough rows to grow the mapping many times over, written in pieces that
    // land mid-row: what survives that is offsets into the corpus, which is why
    // nothing here keeps a pointer.
    constexpr int kRows = 4000;
    std::string all;
    for (int i = 0; i < kRows; ++i) all += ScanRow(paths[0], i + 1, "Sym" + std::to_string(i));
    for (std::size_t at = 0; at < all.size();) {
      const std::size_t take = std::min<std::size_t>(3000, all.size() - at);
      piped.Send(std::string_view{all}.substr(at, take));
      at += take;
      PickerPumpScan(ed);
    }
    piped.Close();
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->rows.size(), static_cast<std::size_t>(kRows));
    EXPECT_EQ(ed.picker->shown.size(), static_cast<std::size_t>(kRows));
    EXPECT_EQ(ed.picker->rows.back().name, "Sym" + std::to_string(kRows - 1));
    EXPECT_EQ(ed.picker->rows.back().line, Index{kRows});
    PromptCancel(ed);
  }

  TEST_CASE("picker: a keystroke filters the list, a new row only itself");
  {
    const Scratch scratch{"koi-scan-filter"};
    Editor ed;
    const std::vector<fs::path> paths = lines(scratch, 4);
    PipedPicker piped{ed, PickerState::Source::kProjectSymbols};

    for (int i = 0; i < 2; ++i) piped.Send(ScanRow(paths[i], 1, "Sym" + std::to_string(i)));
    PickerPumpScan(ed);
    ed.prompt_input = "Sym1";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    EXPECT_EQ(ed.picker->scan->filter, std::string{"Sym1"});

    // Two more land: the one the pattern keeps joins the band, the other is a
    // row of the list all the same -- the count says so, and a wider pattern
    // finds it without the scan being re-run.
    for (int i = 2; i < 4; ++i) piped.Send(ScanRow(paths[i], 1, "Sym" + std::to_string(i)));
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{4});
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    ed.prompt_input = "Sym[123]";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});
    // Three rows on the band, and only the selected one's file was ever read:
    // the rows are names, so nothing about drawing them or judging them opens
    // anything. The block is the one reader.
    EXPECT_FALSE(ed.picker->lines.contains(paths[2].string()));
    EXPECT_FALSE(ed.picker->lines.contains(paths[3].string()));

    // And the pattern is held against the name alone: what every one of these
    // files says on the line the rows point at keeps nothing.
    ed.prompt_input = "return 0";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{0});
    PromptCancel(ed);
  }

  TEST_CASE("picker: a pasted query filters the list the same as a typed one");
  {
    const Scratch scratch{"koi-scan-paste"};
    Editor ed;
    const std::vector<fs::path> paths = lines(scratch, 4);
    PipedPicker piped{ed, PickerState::Source::kProjectSymbols};

    for (int i = 0; i < 3; ++i) piped.Send(ScanRow(paths[i], 1, "Sym" + std::to_string(i)));
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});

    // A paste is a query change, so the band answers for it -- the box and the
    // rows under it say the same thing, and enter would open from this list.
    ApplyPaste(ed, "Sym1");
    EXPECT_EQ(ed.prompt_input, std::string{"Sym1"});
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    EXPECT_EQ(ed.picker->rows[ed.picker->shown[0]].name, std::string{"Sym1"});
    // And the pattern is down for the pump, or a row landing after the paste
    // would be judged by the one the paste replaced.
    EXPECT_EQ(ed.picker->scan->filter, std::string{"Sym1"});
    piped.Send(ScanRow(paths[3], 1, "Sym3"));
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{4});
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});

    // Newlines and tabs cannot reach a prompt that is one line: they arrive as
    // spaces, and the pattern they make is the one the band is filtered by.
    ed.prompt_input.clear();
    PickerRefilter(ed);
    ApplyPaste(ed, "Sym1\tor\nSym3");
    EXPECT_EQ(ed.prompt_input, std::string{"Sym1 or Sym3"});
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{0});
    PromptCancel(ed);
  }

  TEST_CASE("picker: a row landing below the window does not refill it");
  {
    const Scratch scratch{"koi-scan-refill"};
    Editor ed;
    const std::vector<fs::path> paths = lines(scratch, 12);
    PipedPicker piped{ed, PickerState::Source::kProjectSymbols};

    // Two rows, a band that fits five: the window is not full, so what lands
    // joins it and the block is filled for the selection.
    for (int i = 0; i < 2; ++i) piped.Send(ScanRow(paths[i], 1, "Sym" + std::to_string(i)));
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{2});
    EXPECT_FALSE(ed.picker->context.empty());

    // Still not full: rows three to five land on the band, so the fill runs.
    ed.picker->context.clear();
    for (int i = 2; i < 5; ++i) piped.Send(ScanRow(paths[i], 1, "Sym" + std::to_string(i)));
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->shown.size(), kPickerRows);
    EXPECT_FALSE(ed.picker->context.empty());

    // Full now. What joins below the window cannot change what the window
    // shows, so nothing is re-read for it -- but the count is the whole list's
    // and moves with every row.
    ed.picker->context.clear();
    const int card = ed.picker->card_w;
    for (int i = 5; i < 12; ++i) {
      piped.Send(ScanRow(paths[i], 1, "Sym" + std::to_string(i) + "WithAMuchLongerNameThanTheRest"));
    }
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{12});
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{12});
    EXPECT_TRUE(ed.picker->context.empty());
    // The card is the screen for a band over file content, so a longer row
    // landing does not widen it -- there is nothing wider to grow to.
    EXPECT_EQ(ed.picker->card_w, card);

    // Skipped, not stopped: the next thing that moves the window fills it.
    PickerStep(ed, true);
    EXPECT_FALSE(ed.picker->context.empty());
    PromptCancel(ed);
  }

  TEST_CASE("picker: rows arriving under a half-typed pattern join the last good list");
  {
    const Scratch scratch{"koi-scan-bad-pattern"};
    Editor ed;
    const std::vector<fs::path> paths = lines(scratch, 4);
    PipedPicker piped{ed, PickerState::Source::kProjectSymbols};

    for (int i = 0; i < 2; ++i) piped.Send(ScanRow(paths[i], 1, "Sym" + std::to_string(i)));
    PickerPumpScan(ed);
    ed.prompt_input = "Sym";
    PickerRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{2});

    // Half a group: the band keeps what it had and says why.
    ed.prompt_input = "Sym(";
    PickerRefilter(ed);
    EXPECT_TRUE(!ed.status.empty());
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{2});
    EXPECT_EQ(ed.picker->scan->filter, std::string{"Sym"});

    // And rows landing meanwhile are judged by that same last good pattern, so
    // the list goes on meaning one thing all the way down.
    piped.Send(ScanRow(paths[2], 1, "Sym2"));
    piped.Send(ScanRow(scratch.Write("other.cpp", "int Nope() { return 0; }\n"), 1, "Nope"));
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{4});
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});
    EXPECT_EQ(ed.picker->rows[ed.picker->shown[2]].name, std::string{"Sym2"});
    PromptCancel(ed);
  }

  TEST_CASE("picker: esc mid-scan kills the child and reaps it");
  {
    const Scratch scratch{"koi-scan-esc"};
    Editor ed;
    // A child that will never end on its own: only the teardown can end it.
    EXPECT_TRUE(PickerStartScan(ed, PickerState::Source::kProjectSymbols, "sleep 120", ""));
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.picker != nullptr);
    if (ed.picker == nullptr) return;
    const int pid = ed.picker->scan->pid;
    EXPECT_TRUE(pid > 0);
    EXPECT_TRUE(PickerScanning(ed));

    // The same path esc takes, and the only cleanup there is.
    PromptCancel(ed);
    EXPECT_TRUE(ed.picker == nullptr);
    EXPECT_TRUE(ChildIsGone(pid));
  }

  TEST_CASE("picker: a symbol scan that exited non-zero with no rows says the code");
  {
    Editor ed;
    ed.status.clear();
    // Not 127, so it is not the missing-program wording that answers -- a
    // file_filter that will not run exits however it exits.
    EXPECT_TRUE(PickerStartScan(ed, PickerState::Source::kProjectSymbols, "exit 3", ""));
    if (ed.picker == nullptr) return;
    EXPECT_TRUE(PumpUntilDone(ed));
    EXPECT_TRUE(ed.picker->rows.empty());
    EXPECT_TRUE(std::string{ed.status}.find("exited 3") != std::string::npos);
    PromptCancel(ed);
  }

  TEST_CASE("picker: accepting mid-scan keeps what arrived and ends the child");
  {
    const Scratch scratch{"koi-scan-accept"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    const std::vector<fs::path> paths = lines(scratch, 2);
    const fs::path rows =
        scratch.Write("rows.txt", ScanRow(paths[0], 1, "Sym0") + ScanRow(paths[1], 1, "Sym1"));
    // Two rows, then a child that goes on forever: the pick happens mid-scan.
    EXPECT_TRUE(PickerStartScan(ed, PickerState::Source::kProjectSymbols,
                                "cat " + rows.string() + "; sleep 120", ""));
    if (ed.picker == nullptr) return;
    const int pid = ed.picker->scan->pid;
    EXPECT_TRUE(PumpUntilRows(ed, 2));
    EXPECT_EQ(ed.picker->rows.size(), std::size_t{2});

    PickerAccept(ed, 0);
    EXPECT_FALSE(ed.prompt_active);
    EXPECT_TRUE(ed.picker == nullptr);
    EXPECT_TRUE(ChildIsGone(pid));
    // The walk holds what had arrived, off in its own rows -- the scan is gone
    // and nothing the pick kept points back at it.
    EXPECT_TRUE(ed.walk != nullptr);
    if (ed.walk == nullptr) return;
    EXPECT_EQ(ed.walk->rows.size(), std::size_t{2});
    EXPECT_EQ(ed.doc.file.filename().string(), paths[0].filename().string());
    PickerJumpStep(ed, true);
    EXPECT_EQ(ed.doc.file.filename().string(), paths[1].filename().string());
  }

  TEST_CASE("picker: the project scan opens in process, and last_picker resumes it");
  {
    const Scratch scratch{"koi-scan-symbols"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    EXPECT_TRUE(OpenTarget(ed, scratch.Write("a.cpp", "int Alpha() { return 0; }\n").string()));
    ed.settings.file_filter = "printf '%s\\n' a.cpp";

    SymbolPicker(ed);
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.prompt_kind == PromptKind::kPicker);
    EXPECT_TRUE(ed.picker != nullptr);
    if (ed.picker == nullptr) return;
    EXPECT_TRUE(ed.picker->source == PickerState::Source::kProjectSymbols);
    // A child of its own, and a band that shows a context block the way every
    // other symbol picker does.
    EXPECT_TRUE(ed.picker->scan != nullptr);
    EXPECT_TRUE(ed.picker->scan->pid > 0);
    EXPECT_TRUE(PickerShowsContext(ed.picker->source));
    const int pid = ed.picker->scan->pid;
    PromptCancel(ed);
    EXPECT_TRUE(ChildIsGone(pid));

    std::string name;
    std::string query;
    EXPECT_TRUE(ReadLastPicker(name, query));
    EXPECT_EQ(name, std::string{"symbols"});

    // Reopened with the query typed in -- and here the query is a filter, so
    // rows arriving under it are judged by it from the first byte.
    WriteLastPicker("symbols", "Alpha");
    LastPicker(ed);
    EXPECT_TRUE(ed.prompt_active);
    if (ed.picker == nullptr) return;
    EXPECT_TRUE(ed.picker->source == PickerState::Source::kProjectSymbols);
    EXPECT_EQ(ed.prompt_input, std::string{"Alpha"});
    EXPECT_EQ(ed.picker->scan->filter, std::string{"Alpha"});
    PromptCancel(ed);
  }
}

// Step 7: content, whose candidates are unbounded. The corpus IS the row
// storage -- `rows` stays empty and `shown` holds byte offsets into the scan's
// bytes -- so most of what is asserted here is that an offset still means what
// it meant: after a relocating grow, after a read that split a line, and after
// the prompt that owned the corpus is gone.
namespace {

// What the content pipeline emits: gai's own `path:line:text`, one per line.
std::string ContentRow(std::string_view path, Index line, std::string_view text) {
  return std::string{path} + ":" + std::to_string(line) + ":" + std::string{text} + "\n";
}

// The shown list spelled as the lines it names, which is what two filter passes
// have to agree on -- the offsets themselves only agree if the corpus does.
std::vector<std::string> ShownLines(const PickerState& state) {
  std::vector<std::string> out;
  for (std::size_t at = 0; at < state.shown.size(); ++at) {
    out.emplace_back(PickerRowText(state, at));
  }
  return out;
}

}

void StreamingContentPicker() {
  namespace fs = std::filesystem;

  TEST_CASE("content: the scan command is the file filter into gai, and nothing else");
  {
    Editor ed;
    ed.doc.file = "src/a.cpp";
    const std::string scan = PickerScanCommand(ed, "content");
    EXPECT_TRUE(!scan.empty());
    // gai's own `path:line:text`, unreformatted: the band parses only the rows
    // it draws, so nothing stands between the child and the corpus.
    EXPECT_TRUE(scan.find("gai --no-color -f '\\w' -v -d : --files") != std::string::npos);
    EXPECT_TRUE(scan.find("awk") == std::string::npos);
    EXPECT_TRUE(scan.back() != '|');
    // The query cannot ride the scan -- the command is built without one, and
    // the child sends every line it finds for the band to filter as it lands.
    EXPECT_TRUE(ExpandVariables(scan, ed).find("%{") == std::string::npos);
  }

  TEST_CASE("content: an empty query shows every complete line, and the count is the corpus");
  {
    Editor ed;
    PipedPicker piped{ed, PickerState::Source::kContent};
    const std::string first = ContentRow("src/a.cpp", 1, "alpha");
    piped.Send(first + ContentRow("src/b.cpp", 2, "bravo") + ContentRow("src/c.cpp", 3, "charlie"));
    EXPECT_TRUE(PickerPumpScan(ed));

    // No PickerEntry per match: the corpus holds the rows and shown holds where
    // each one starts in it.
    EXPECT_TRUE(ed.picker->rows.empty());
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});
    EXPECT_EQ(PickerTotal(*ed.picker), std::size_t{3});
    EXPECT_EQ(ed.picker->shown[0], std::size_t{0});
    EXPECT_EQ(ed.picker->shown[1], first.size());
    // The row is the whole line, path:line: head and all -- that head is what
    // lets the query filter by path. The band draws it in two: the line leads,
    // and the head rides the card's right edge as the detail, the way a symbol
    // row's path does.
    EXPECT_EQ(std::string{PickerRowText(*ed.picker, 0)}, std::string{"src/a.cpp:1:alpha"});
    EXPECT_EQ(std::string{PickerRowLead(*ed.picker, 0)}, std::string{"alpha"});
    EXPECT_EQ(std::string{PickerRowDetail(*ed.picker, 0)}, std::string{"src/a.cpp:1"});

    // And the keystroke path says the same thing the landing path did: an empty
    // query is every line, whichever pass produced the list.
    const std::vector<std::size_t> landed = ed.picker->shown;
    ed.prompt_input.clear();
    PickerRefilter(ed);
    PumpRefilter(ed);
    EXPECT_TRUE(ed.picker->shown == landed);
    PromptCancel(ed);
  }

  TEST_CASE("content: a query change rescans the corpus, matching a from-scratch filter");
  {
    const std::string corpus = ContentRow("src/a.cpp", 1, "alpha widget") +
                               ContentRow("src/b.cpp", 2, "bravo") +
                               ContentRow("docs/c.md", 3, "widget notes") +
                               ContentRow("src/d.cpp", 4, "delta WIDGET");

    // One picker takes the bytes with nothing typed and then types the query --
    // the rescan. Another takes the same bytes with the query already standing
    // -- each byte met by the pattern as it landed. The two lists have to be the
    // same list, which is what "each byte meets each query exactly once" means.
    Editor typed;
    PipedPicker after{typed, PickerState::Source::kContent};
    after.Send(corpus);
    PickerPumpScan(typed);
    typed.prompt_input = "widget";
    PickerRefilter(typed);
    // The pattern is put down, not run: the band still has the list it had, and
    // the catch-up is what replaces it.
    EXPECT_TRUE(typed.picker->scan->refiltering);
    EXPECT_EQ(typed.picker->shown.size(), std::size_t{4});
    PumpRefilter(typed);

    Editor standing;
    PipedPicker during{standing, PickerState::Source::kContent};
    standing.picker->scan->filter = "widget";
    during.Send(corpus);
    PickerPumpScan(standing);

    EXPECT_EQ(typed.picker->shown.size(), std::size_t{3});
    EXPECT_TRUE(typed.picker->shown == standing.picker->shown);
    EXPECT_TRUE(ShownLines(*typed.picker) == ShownLines(*standing.picker));
    // (?i), as everywhere else: WIDGET is widget.
    EXPECT_EQ(ShownLines(*typed.picker).back(), std::string{"src/d.cpp:4:delta WIDGET"});
    // The count still says the whole corpus: n of m, not n of n.
    EXPECT_EQ(PickerTotal(*typed.picker), std::size_t{4});

    // A pattern that will not compile keeps the last good list, exactly as a
    // list of rows does -- and the last good pattern is what the corpus is
    // still judged by.
    typed.prompt_input = "widget(";
    PickerRefilter(typed);
    EXPECT_TRUE(!typed.status.empty());
    EXPECT_EQ(typed.picker->shown.size(), std::size_t{3});
    EXPECT_EQ(typed.picker->scan->filter, std::string{"widget"});
    PromptCancel(typed);
    PromptCancel(standing);
  }

  TEST_CASE("content: the query filters by path as much as by what the line says");
  {
    Editor ed;
    PipedPicker piped{ed, PickerState::Source::kContent};
    piped.Send(ContentRow("koi/src/a.cpp", 1, "alpha") + ContentRow("docs/one.md", 2, "alpha") +
               ContentRow("docs/two.md", 3, "bravo") + ContentRow("koi/src/b.cpp", 4, "bravo"));
    PickerPumpScan(ed);

    // A directory is a pattern like any other, because the row keeps its head.
    ed.prompt_input = "^docs/";
    PickerRefilter(ed);
    PumpRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{2});
    EXPECT_EQ(std::string{PickerRowText(*ed.picker, 0)}, std::string{"docs/one.md:2:alpha"});
    EXPECT_EQ(std::string{PickerRowText(*ed.picker, 1)}, std::string{"docs/two.md:3:bravo"});

    // Path and content together, which is the whole point of keeping the head.
    ed.prompt_input = R"(^koi/.*bravo)";
    PickerRefilter(ed);
    PumpRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    EXPECT_EQ(std::string{PickerRowText(*ed.picker, 0)}, std::string{"koi/src/b.cpp:4:bravo"});
    PromptCancel(ed);
  }

  TEST_CASE("content: bytes landing under a live query join at the bottom, and nothing moves");
  {
    Editor ed;
    PipedPicker piped{ed, PickerState::Source::kContent};
    for (int i = 0; i < 3; ++i) {
      piped.Send(ContentRow("src/a.cpp", i + 1, "widget " + std::to_string(i)));
    }
    PickerPumpScan(ed);
    ed.prompt_input = "widget";
    PickerRefilter(ed);
    PumpRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});

    // Somewhere in the middle, which is where an insert would show.
    PickerStep(ed, true);
    EXPECT_EQ(ed.picker->selected, std::size_t{1});
    const std::vector<std::size_t> was = ed.picker->shown;
    const std::size_t offset = ed.picker->offset;

    // Three more land: two the live query keeps, one it does not. The one it
    // does not is still a line of the corpus, and the count says so.
    piped.Send(ContentRow("src/b.cpp", 9, "nothing here"));
    for (int i = 3; i < 5; ++i) {
      piped.Send(ContentRow("src/a.cpp", i + 1, "widget " + std::to_string(i)));
    }
    EXPECT_TRUE(PickerPumpScan(ed));
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{5});
    EXPECT_EQ(PickerTotal(*ed.picker), std::size_t{6});
    // Append-only: every offset that was there is still there, in place.
    for (std::size_t i = 0; i < was.size(); ++i) EXPECT_EQ(ed.picker->shown[i], was[i]);
    EXPECT_EQ(ed.picker->selected, std::size_t{1});
    EXPECT_EQ(ed.picker->offset, offset);
    EXPECT_EQ(std::string{PickerRowText(*ed.picker, 4)}, std::string{"src/a.cpp:5:widget 4"});

    // And a wider pattern finds the line that never joined, without the scan
    // being re-run: the corpus kept it.
    ed.prompt_input = "nothing";
    PickerRefilter(ed);
    PumpRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    EXPECT_EQ(std::string{PickerRowText(*ed.picker, 0)}, std::string{"src/b.cpp:9:nothing here"});
    PromptCancel(ed);
  }

  TEST_CASE("content: a match spanning a read boundary is not lost");
  {
    Editor ed;
    PipedPicker piped{ed, PickerState::Source::kContent};
    ed.picker->scan->filter = "widget";

    // Split inside the word the query is looking for: neither half matches it,
    // so only the carry can keep the row.
    const std::string row = ContentRow("src/a.cpp", 7, "the widget here");
    const std::size_t cut = row.find("get here");
    piped.Send(row.substr(0, cut));
    PickerPumpScan(ed);
    // Half a line is not a line: nothing parsed, nothing matched, nothing lost.
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{0});
    EXPECT_EQ(PickerTotal(*ed.picker), std::size_t{0});
    EXPECT_EQ(ed.picker->scan->parsed, std::size_t{0});

    piped.Send(row.substr(cut));
    EXPECT_TRUE(PickerPumpScan(ed));
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    EXPECT_EQ(ed.picker->shown[0], std::size_t{0});
    EXPECT_EQ(std::string{PickerRowText(*ed.picker, 0)},
              std::string{"src/a.cpp:7:the widget here"});

    // Pipe close flushes an unterminated tail, and the note goes with it.
    std::string last = ContentRow("src/a.cpp", 8, "widget last");
    last.pop_back();
    piped.Send(last);
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    piped.Close();
    EXPECT_TRUE(PickerPumpScan(ed));
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{2});
    EXPECT_EQ(std::string{PickerRowText(*ed.picker, 1)}, std::string{"src/a.cpp:8:widget last"});
    EXPECT_FALSE(PickerScanLive(*ed.picker));
    PromptCancel(ed);
  }

  TEST_CASE("content: a line number too long to hold is no target, and the guard is not tight");
  {
    Editor ed;
    PipedPicker piped{ed, PickerState::Source::kContent};
    // A filename with a colon in it puts the rest of the name where the number
    // belongs. Twenty digits do not fit an Index, and running them into one is
    // signed overflow, so the row is refused rather than parsed.
    piped.Send(std::string{"src/od:99999999999999999999:alpha\n"} +
               "src/wide:999999999999999999:bravo\n" + ContentRow("src/a.cpp", 7, "charlie"));
    PickerPumpScan(ed);
    // All three are rows on the band -- only what they resolve to differs.
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});

    PickerTarget target{};
    EXPECT_FALSE(PickerRowTarget(*ed.picker, 0, target));
    // Eighteen digits is the longest run that fits, and it still parses: the
    // guard refuses overflow, not large numbers.
    EXPECT_TRUE(PickerRowTarget(*ed.picker, 1, target));
    EXPECT_EQ(target.line, Index{999999999999999999});
    EXPECT_TRUE(PickerRowTarget(*ed.picker, 2, target));
    EXPECT_EQ(target.line, Index{7});
    // And a row that names nowhere has nothing to split: it leads with the whole
    // line and carries no path at the edge.
    EXPECT_EQ(std::string{PickerRowLead(*ed.picker, 0)},
              std::string{PickerRowText(*ed.picker, 0)});
    EXPECT_TRUE(PickerRowDetail(*ed.picker, 0).empty());
    PromptCancel(ed);
  }

  TEST_CASE("content: offsets survive a corpus that relocates as it grows");
  {
    Editor ed;
    PipedPicker piped{ed, PickerState::Source::kContent};

    // Enough to grow the mapping many times over, written in pieces that land
    // mid-line: what survives that is offsets, which is why nothing keeps a
    // pointer into the corpus.
    constexpr int kRows = 4000;
    std::string all;
    for (int i = 0; i < kRows; ++i) {
      all += ContentRow("src/f" + std::to_string(i % 7) + ".cpp", i + 1,
                        "row " + std::to_string(i) + " of the corpus");
    }
    for (std::size_t at = 0; at < all.size();) {
      const std::size_t take = std::min<std::size_t>(3000, all.size() - at);
      piped.Send(std::string_view{all}.substr(at, take));
      at += take;
      PickerPumpScan(ed);
    }
    piped.Close();
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->shown.size(), static_cast<std::size_t>(kRows));
    EXPECT_EQ(PickerTotal(*ed.picker), static_cast<std::size_t>(kRows));
    // The first and the last offset both still name their own line, which they
    // could not if a grow had left either behind.
    EXPECT_EQ(std::string{PickerRowText(*ed.picker, 0)},
              std::string{"src/f0.cpp:1:row 0 of the corpus"});
    EXPECT_EQ(std::string{PickerRowText(*ed.picker, kRows - 1)},
              "src/f" + std::to_string((kRows - 1) % 7) + ".cpp:" + std::to_string(kRows) +
                  ":row " + std::to_string(kRows - 1) + " of the corpus");

    // And a rescan over the grown corpus finds what the incremental pass would.
    ed.prompt_input = "^src/f3\\.cpp:";
    PickerRefilter(ed);
    // The child is gone and the pipe is closed, and the catch-up still has the
    // corpus to walk: the scan outlives its own EOF while one is pending.
    EXPECT_TRUE(PickerScanLive(*ed.picker));
    PumpRefilter(ed);
    EXPECT_FALSE(PickerScanLive(*ed.picker));
    EXPECT_EQ(ed.picker->shown.size(), static_cast<std::size_t>(kRows / 7));
    EXPECT_TRUE(PickerRowText(*ed.picker, 0).starts_with("src/f3.cpp:"));
    PromptCancel(ed);
  }

  TEST_CASE("content: accept opens path:line and the walk outlives the corpus");
  {
    const Scratch scratch{"koi-content-accept"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    const fs::path a = scratch.Write("a.cpp", "one\ntwo widget\nthree\n");
    const fs::path b = scratch.Write("b.cpp", "alpha\nbravo\nwidget again\n");

    PipedPicker piped{ed, PickerState::Source::kContent};
    piped.Send(ContentRow(a.string(), 2, "two widget") +
               ContentRow(b.string(), 3, "widget again"));
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{2});

    // The block reads the file, not the corpus: the corpus holds the matched
    // line and not the two around it.
    EXPECT_EQ(ed.picker->context.size(), kPickerContext);
    EXPECT_EQ(ed.picker->context_first, Index{1});
    EXPECT_EQ(ed.picker->context_target, Index{2});
    // The row names the file at the card's right edge, so the block carries no
    // heading of its own -- what every other source with a block does.
    EXPECT_EQ(std::string{PickerRowDetail(*ed.picker, 0)}, a.string() + ":2");
    EXPECT_EQ(ed.picker->context[1], std::string{"two widget"});

    PickerAccept(ed, 0);
    EXPECT_FALSE(ed.prompt_active);
    EXPECT_TRUE(ed.picker == nullptr);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.cpp"});
    EXPECT_EQ(LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary())), Index{1});

    std::string name;
    std::string query;
    EXPECT_TRUE(ReadLastPicker(name, query));
    EXPECT_EQ(name, std::string{"content"});

    // The corpus died with the prompt, and the walk did not: its rows own their
    // strings, and stepping them opens the next file.
    EXPECT_TRUE(ed.walk != nullptr);
    if (ed.walk == nullptr) return;
    EXPECT_EQ(ed.walk->rows.size(), std::size_t{2});
    EXPECT_EQ(ed.walk->rows[0].display, a.string() + ":2");
    EXPECT_EQ(ed.walk->rows[1].display, b.string() + ":3");
    // A content hit is a place, not a symbol: nothing to record under a name.
    EXPECT_TRUE(ed.walk->rows[0].name.empty());
    PickerJumpStep(ed, true);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"b.cpp"});
    EXPECT_EQ(LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary())), Index{2});
  }

  TEST_CASE("content: a row with no path:line head is no destination and no step");
  {
    const Scratch scratch{"koi-content-headless"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    const fs::path a = scratch.Write("a.cpp", "one\ntwo widget\nthree\n");
    const fs::path b = scratch.Write("b.cpp", "alpha\nbravo\nwidget again\n");

    PipedPicker piped{ed, PickerState::Source::kContent};
    // Two hits with the producer's own complaint between them: it filters like
    // any other line and names nowhere at all.
    piped.Send(ContentRow(a.string(), 2, "two widget") + "grep: widget: no such file\n" +
               ContentRow(b.string(), 3, "widget again"));
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});

    // Accepting it opens nothing, and the prompt stays for the pattern to be
    // fixed -- the same as a digit off the band.
    PickerAccept(ed, 1);
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.picker != nullptr);
    EXPECT_TRUE(ed.walk == nullptr);
    EXPECT_TRUE(ed.status.text().find("no path") != std::string::npos);

    // And it is left out of the walk a real pick builds, with `at` counting the
    // rows that stayed rather than the rows the band showed.
    PickerAccept(ed, 2);
    EXPECT_TRUE(ed.walk != nullptr);
    if (ed.walk == nullptr) return;
    EXPECT_EQ(ed.walk->rows.size(), std::size_t{2});
    EXPECT_EQ(ed.walk->at, std::size_t{1});
    EXPECT_EQ(ed.walk->rows[0].display, a.string() + ":2");
    EXPECT_EQ(ed.walk->rows[1].display, b.string() + ":3");
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"b.cpp"});

    // Stepping wraps onto the first hit, not onto the row between them.
    PickerJumpStep(ed, true);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.cpp"});
    EXPECT_EQ(ed.walk->at, std::size_t{0});
  }

  TEST_CASE("content: a pick out of a huge list keeps a walk, not the pick");
  {
    const Scratch scratch{"koi-content-walkcap"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    std::string file;
    for (int i = 0; i < 4; ++i) file += "line " + std::to_string(i + 1) + "\n";
    const fs::path only = scratch.Write("big.cpp", file);

    PipedPicker piped{ed, PickerState::Source::kContent};
    std::string all;
    for (std::size_t i = 0; i < (kWalkRows + 100); ++i) {
      all += ContentRow(only.string(), 1 + static_cast<Index>(i % 4), "hit " + std::to_string(i));
    }
    // In pieces, with a pump between: more than a pipe holds at once.
    for (std::size_t at = 0; at < all.size();) {
      const std::size_t take = std::min<std::size_t>(32768, all.size() - at);
      piped.Send(std::string_view{all}.substr(at, take));
      at += take;
      PickerPumpScan(ed);
    }
    EXPECT_EQ(ed.picker->shown.size(), kWalkRows + 100);

    PickerAccept(ed, 0);
    EXPECT_TRUE(ed.walk != nullptr);
    if (ed.walk == nullptr) return;
    EXPECT_EQ(ed.walk->rows.size(), kWalkRows);
    EXPECT_EQ(ed.walk->rows[0].target, only.string());
  }

  TEST_CASE("content: esc mid-scan kills the child and frees the corpus");
  {
    Editor ed;
    EXPECT_TRUE(PickerStartScan(ed, PickerState::Source::kContent, "sleep 120", ""));
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.picker != nullptr);
    if (ed.picker == nullptr) return;
    const int pid = ed.picker->scan->pid;
    EXPECT_TRUE(pid > 0);
    EXPECT_TRUE(PickerScanning(ed));

    PromptCancel(ed);
    EXPECT_TRUE(ed.picker == nullptr);
    EXPECT_TRUE(ChildIsGone(pid));
  }

  TEST_CASE("content: the picker opens in process, and last_picker resumes it");
  {
    const Scratch scratch{"koi-content-open"};
    const AsProjectRoot root{scratch.dir};
    const InDirectory here{scratch.dir};
    Editor ed;
    EXPECT_TRUE(OpenTarget(ed, scratch.Write("a.cpp", "int Alpha() { return 0; }\n").string()));
    ed.settings.file_filter = "printf '%s\\n' a.cpp";

    ContentPicker(ed);
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.prompt_kind == PromptKind::kPicker);
    EXPECT_TRUE(ed.picker != nullptr);
    if (ed.picker == nullptr) return;
    EXPECT_TRUE(ed.picker->source == PickerState::Source::kContent);
    // A child of its own, feeding the band -- no terminal is handed over.
    EXPECT_TRUE(ed.picker->scan != nullptr);
    EXPECT_TRUE(ed.picker->scan->pid > 0);
    EXPECT_TRUE(PickerShowsContext(ed.picker->source));
    const int pid = ed.picker->scan->pid;
    PromptCancel(ed);
    EXPECT_TRUE(ChildIsGone(pid));

    std::string name;
    std::string query;
    EXPECT_TRUE(ReadLastPicker(name, query));
    EXPECT_EQ(name, std::string{"content"});

    // Reopened with the query typed in, and a typed query is a filter here: the
    // lines the child sends are judged by it from the first byte.
    WriteLastPicker("content", "Alpha");
    LastPicker(ed);
    EXPECT_TRUE(ed.prompt_active);
    if (ed.picker == nullptr) return;
    EXPECT_TRUE(ed.picker->source == PickerState::Source::kContent);
    EXPECT_EQ(ed.prompt_input, std::string{"Alpha"});
    EXPECT_EQ(ed.picker->scan->filter, std::string{"Alpha"});
    PromptCancel(ed);
  }

  TEST_CASE("content: a query change is carried a budget at a time, not run at the keystroke");
  {
    Editor ed;
    PipedPicker piped{ed, PickerState::Source::kContent};

    // Past the match budget, so the catch-up cannot finish in one wake -- which
    // is the whole point: a keystroke waits behind one budget of matching and
    // never behind the corpus.
    std::string all;
    std::size_t rows = 0;
    while (all.size() <= (kPickerMatchBudget + (64u << 10))) {
      all += ContentRow("src/f.cpp", static_cast<Index>(rows + 1),
                        ((rows % 1000) == 0) ? ("needle " + std::to_string(rows))
                                             : ("row " + std::to_string(rows) + " of the corpus"));
      ++rows;
    }
    for (std::size_t at = 0; at < all.size();) {
      const std::size_t take = std::min<std::size_t>(32768, all.size() - at);
      piped.Send(std::string_view{all}.substr(at, take));
      at += take;
      PickerPumpScan(ed);
    }
    piped.Close();
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->shown.size(), rows);
    EXPECT_TRUE(ed.picker->scan->done);

    const std::vector<std::size_t> was = ed.picker->shown;
    ed.prompt_input = "needle";
    PickerRefilter(ed);
    // Nothing matched yet: the band is still showing every line, which is what
    // a bad pattern leaves there too.
    EXPECT_TRUE(ed.picker->scan->refiltering);
    EXPECT_TRUE(ed.picker->shown == was);
    // The child is reaped and the pipe closed, and the count still says the
    // list is not the answer yet -- so the input loop keeps waking for it.
    EXPECT_TRUE(PickerScanLive(*ed.picker));
    EXPECT_TRUE(PickerScanning(ed));

    // One wake takes a budget of the corpus and no more, and the band does not
    // move for it.
    PickerPumpScan(ed);
    EXPECT_TRUE(ed.picker->scan->refiltering);
    EXPECT_TRUE(ed.picker->scan->refilter_at > 0);
    EXPECT_TRUE(ed.picker->scan->refilter_at <= (kPickerMatchBudget + (all.size() / rows)));
    EXPECT_TRUE(ed.picker->shown == was);

    // And when it lands, the list changes hands whole: the same list a pass
    // over the corpus in one go would have built.
    PumpRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), (rows + 999) / 1000);
    EXPECT_EQ(ed.picker->scan->filter, std::string{"needle"});
    EXPECT_EQ(ed.picker->selected, std::size_t{0});
    EXPECT_EQ(ed.picker->offset, std::size_t{0});
    EXPECT_TRUE(PickerRowText(*ed.picker, 0).find("needle 0") != std::string_view::npos);
    // Nothing left to carry, so nothing left to wake for.
    EXPECT_FALSE(PickerScanLive(*ed.picker));
    EXPECT_FALSE(PickerPumpScan(ed));
    PromptCancel(ed);
  }

  TEST_CASE("content: a pattern that blows the match limit ends the pass and keeps the list");
  {
    Editor ed;
    PipedPicker piped{ed, PickerState::Source::kContent};
    // The middle line makes `(a+)+$` backtrack exponentially and never match:
    // it ends in something the anchor will not take. That is a match-time
    // limit, not a non-match, and reading it as one would drop the row in
    // silence.
    piped.Send(ContentRow("src/a.cpp", 1, "alpha") +
               ContentRow("src/b.cpp", 2, std::string(64, 'a') + "b") +
               ContentRow("src/c.cpp", 3, "charlie"));
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});

    ed.prompt_input = "(a+)+$";
    PickerRefilter(ed);
    // It compiles, so the catch-up starts -- what it costs is only findable by
    // running it.
    EXPECT_TRUE(ed.status.empty());
    EXPECT_TRUE(ed.picker->scan->refiltering);

    PickerPumpScan(ed);
    // One blown line ends the pass: the rest of the corpus would blow it too,
    // and the band keeps the list it had with the reason on the branch row.
    EXPECT_TRUE(!ed.status.empty());
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});
    EXPECT_TRUE(ed.picker->scan->refilter_pattern != std::string{"(a+)+$"});

    // What the list was built with is picked up again, so bytes that landed
    // while the bad pattern was being carried are still judged by something.
    PumpRefilter(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{3});
    EXPECT_TRUE(ed.picker->scan->filter.empty());
    PromptCancel(ed);
  }

  TEST_CASE("content: a read that fails is not eof -- its tail is no row, and it says so");
  {
    Editor ed;
    ed.status.clear();
    PipedPicker piped{ed, PickerState::Source::kContent};
    // One whole line and the start of another: the half-line is what eof would
    // flush as the last row and what a failure must leave alone.
    piped.Send(ContentRow("src/a.cpp", 1, "alpha") + "src/b.cpp:2:brav");
    PickerPumpScan(ed);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});

    // A directory fd reads EISDIR: a real mid-stream read failure, which is the
    // one thing a pipe cannot be asked to produce.
    const int bad = open(".", O_RDONLY);
    EXPECT_TRUE(bad >= 0);
    if (bad < 0) return;
    close(ed.picker->scan->fd);
    ed.picker->scan->fd = bad;
    EXPECT_TRUE(PickerPumpScan(ed));

    // The scan is over, but not the way eof ends one: the half-line stays off
    // the band rather than landing there chopped, and the count drops its note
    // because the warning is where the reason belongs.
    EXPECT_TRUE(ed.picker->scan->done);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    EXPECT_FALSE(ed.picker->scan->truncated);
    EXPECT_TRUE(PickerCountNote(*ed.picker).empty());
    EXPECT_TRUE(std::string{ed.status}.find("partial") != std::string::npos);
    PromptCancel(ed);
  }

  TEST_CASE("content: a corpus at its ceiling keeps what landed and the count says it stopped");
  {
    Editor ed;
    PipedPicker piped{ed, PickerState::Source::kContent};
    piped.Send(ContentRow("src/a.cpp", 1, "alpha") + ContentRow("src/b.cpp", 2, "bravo"));
    PickerPumpScan(ed);
    EXPECT_EQ(PickerCountNote(*ed.picker), kPickerScanNote);

    // The scan's ceiling defaults to a quarter of a gigabyte, so what is driven
    // here is the flag rather than the read that sets it -- that read is in
    // PickerCeilingsFollowTheSettings, which lowers the ceiling to reach it.
    // Everything that landed stays on the band and the count stops reading as
    // the whole project.
    ed.picker->scan->truncated = true;
    ed.picker->scan->done = true;
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{2});
    EXPECT_EQ(PickerCountNote(*ed.picker), kPickerCutNote);
    // Same width as the note it replaces, so the card the scan sized does not
    // move when the scan ends.
    EXPECT_EQ(DisplayWidth(kPickerCutNote, 1), DisplayWidth(kPickerScanNote, 1));

    // And a query typed after it still lands: truncation ends the scan through
    // the same door eof does, so the catch-up has a corpus that holds still.
    ed.prompt_input = "bravo";
    PickerRefilter(ed);
    PumpRefilter(ed);
    EXPECT_FALSE(ed.picker->scan->refiltering);
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    EXPECT_EQ(PickerCountNote(*ed.picker), kPickerCutNote);
    PromptCancel(ed);
  }

  TEST_CASE("content: a scan whose command failed says why instead of drawing a clean 0/0");
  {
    Editor ed;
    ed.status.clear();
    // What a missing program leaves behind. The child's stderr goes to
    // /dev/null by design, so the exit code is the only word on it there is.
    EXPECT_TRUE(PickerStartScan(ed, PickerState::Source::kContent, "exit 127", ""));
    if (ed.picker == nullptr) return;
    EXPECT_TRUE(PumpUntilDone(ed));
    EXPECT_EQ(ed.picker->scan->lines, std::size_t{0});
    EXPECT_TRUE(std::string{ed.status}.find("not found") != std::string::npos);
    PromptCancel(ed);
  }

  TEST_CASE("content: a scan that produced lines and then failed keeps quiet");
  {
    Editor ed;
    ed.status.clear();
    // The exit code is only worth saying when there is nothing on the band to
    // say it about -- a scan that answered and then tripped over its last file
    // has already shown its work.
    EXPECT_TRUE(PickerStartScan(ed, PickerState::Source::kContent,
                                "printf 'src/a.cpp:1:alpha\\n'; exit 3", ""));
    if (ed.picker == nullptr) return;
    EXPECT_TRUE(PumpUntilDone(ed));
    EXPECT_EQ(ed.picker->shown.size(), std::size_t{1});
    EXPECT_TRUE(ed.status.empty());
    PromptCancel(ed);
  }
}

// The two picker ceilings are settings, and settings that are snapshotted: the
// pump and the line cache weigh against a member rather than reading
// `ed.settings` per byte or per row, which is what makes :config-reload a thing
// the next scan and the next prompt see rather than a live one.
void PickerCeilingsFollowTheSettings() {
  TEST_CASE("config: the picker ceilings parse as bytes and clamp at both ends");
  {
    // Clamped rather than complained about, like scan-workers: a number is what
    // was asked for, just not that one.
    KeyMaps maps = DefaultKeyMaps();
    Settings settings;
    std::vector<std::string> errors;
    std::ignore = ParseKeyMapConfig(
        "[editor]\npicker-corpus-max-bytes = 536870912\npicker-file-max-bytes = 33554432\n", maps,
        settings, errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(settings.picker_corpus_max, std::uintmax_t{512} << 20);
    EXPECT_EQ(settings.picker_file_max, std::uintmax_t{32} << 20);

    std::ignore = ParseKeyMapConfig(
        "[editor]\npicker-corpus-max-bytes = 0\npicker-file-max-bytes = 0\n", maps, settings,
        errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(settings.picker_corpus_max, std::uintmax_t{4} << 20);
    EXPECT_EQ(settings.picker_file_max, std::uintmax_t{1} << 20);

    // A number past the ceiling comes out clamped, not wrapped: the clamp runs
    // in the signed type the TOML reader hands over.
    std::ignore =
        ParseKeyMapConfig("[editor]\npicker-corpus-max-bytes = 9223372036854775807\n"
                          "picker-file-max-bytes = 9223372036854775807\n",
                          maps, settings, errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(settings.picker_corpus_max, std::uintmax_t{64} << 30);
    EXPECT_EQ(settings.picker_file_max, std::uintmax_t{1} << 30);

    // Wrong type is the other half of the contract: reported, default kept.
    Settings typed;
    std::vector<std::string> complaints;
    std::ignore = ParseKeyMapConfig("[editor]\npicker-corpus-max-bytes = \"lots\"\n", maps, typed,
                                    complaints);
    EXPECT_EQ(complaints.size(), std::size_t{1});
    EXPECT_EQ(typed.picker_corpus_max, kDefaultPickerCorpusMax);
  }

  TEST_CASE("config: a ceiling is captured where it is spent, so a reload is the next one's");
  {
    Editor ed;
    ed.settings.picker_corpus_max = std::uintmax_t{7} << 20;
    ed.settings.picker_file_max = std::uintmax_t{3} << 20;
    EXPECT_TRUE(PickerStartScan(ed, PickerState::Source::kContent,
                                "printf 'src/a.cpp:1:alpha\\n'", ""));
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->scan->corpus_max, std::uintmax_t{7} << 20);
    EXPECT_EQ(ed.picker->file_max, std::uintmax_t{3} << 20);

    // What :config-reload does under an open picker. The live scan and the live
    // prompt keep what they captured, however many wakes they have left.
    ed.settings.picker_corpus_max = std::uintmax_t{11} << 20;
    ed.settings.picker_file_max = std::uintmax_t{5} << 20;
    EXPECT_TRUE(PumpUntilDone(ed));
    EXPECT_EQ(ed.picker->scan->corpus_max, std::uintmax_t{7} << 20);
    EXPECT_EQ(ed.picker->file_max, std::uintmax_t{3} << 20);
    PromptCancel(ed);

    // The next scan and the next prompt are where the new numbers land.
    EXPECT_TRUE(PickerStartScan(ed, PickerState::Source::kContent,
                                "printf 'src/a.cpp:1:alpha\\n'", ""));
    if (ed.picker == nullptr) return;
    EXPECT_EQ(ed.picker->scan->corpus_max, std::uintmax_t{11} << 20);
    EXPECT_EQ(ed.picker->file_max, std::uintmax_t{5} << 20);
    EXPECT_TRUE(PumpUntilDone(ed));
    PromptCancel(ed);
  }

  TEST_CASE("config: the pump weighs the corpus against the scan's own ceiling");
  {
    Editor ed;
    PipedPicker piped{ed, PickerState::Source::kContent};
    // Low enough that one write crosses it, so what is driven here is the read
    // that sets the flag rather than the flag itself.
    ed.picker->scan->corpus_max = 1024;
    std::string corpus;
    for (Index line = 1; line <= 100; ++line) corpus += ContentRow("src/a.cpp", line, "alpha");
    EXPECT_TRUE(corpus.size() > 1024);
    piped.Send(corpus);
    PickerPumpScan(ed);

    // The child is ended and everything that landed stays on the band, with the
    // count saying it is not the whole project.
    EXPECT_TRUE(ed.picker->scan->truncated);
    EXPECT_TRUE(!ed.picker->shown.empty());
    EXPECT_EQ(PickerCountNote(*ed.picker), kPickerCutNote);
    PromptCancel(ed);
  }
}

void PickerCommandShape() {
  TEST_CASE("picker: the assembled scan is one shell command, not several");
  Editor ed;
  ResetToOriginal(ed.doc.table, "x\n");
  for (const char* which : {"content", "symbols"}) {
    const std::string command = PickerScanCommand(ed, which);
    EXPECT_TRUE(!command.empty());

    std::size_t at = 0;
    bool broken = false;
    while ((at = command.find('\n', at)) != std::string::npos) {
      std::size_t back = at;
      while ((back > 0) && ((command[back - 1] == ' ') || (command[back - 1] == '\t'))) --back;

      const char prev = (back > 0) ? command[back - 1] : '\n';
      const bool continued = (prev == '\\') || (prev == '|') || (prev == '&') ||
                             (prev == ';') || (prev == '(') || (prev == '{');

      const auto quotes = static_cast<std::size_t>(
          std::count(command.begin(), command.begin() + static_cast<std::ptrdiff_t>(at), '\''));
      if (!continued && ((quotes % 2) == 0)) broken = true;
      ++at;
    }
    if (broken) std::cerr << "    broken scan command for " << which << "\n";
    EXPECT_FALSE(broken);
  }
  TEST_CASE("picker: every picker that records a name can be reopened");
  {
    // Driven off PickerSourceName over every source rather than a list written
    // out here, so adding a source without teaching LastPicker about it fails
    // this instead of shipping a picker that records itself as the last one and
    // is then unreachable -- which is what "buffers" did.
    const Scratch scratch{"koi-lastpicker"};
    const std::filesystem::path was_root = ProjectRoot();
    SetProjectRoot(scratch.dir);
    struct Restore {
      std::filesystem::path back;
      ~Restore() { SetProjectRoot(back); }
    } restore{was_root};

    const std::filesystem::path state = LastPickerStatePath();
    EXPECT_FALSE(state.empty());

    constexpr std::array<PickerState::Source, 7> kSources{
        {PickerState::Source::kFiles, PickerState::Source::kBuffers, PickerState::Source::kDefs,
         PickerState::Source::kRefs, PickerState::Source::kFileSymbols,
         PickerState::Source::kProjectSymbols, PickerState::Source::kContent}};
    // And no two sources answer to the same name, or one of them reopens as the
    // other.
    std::set<std::string_view> distinct;
    for (const PickerState::Source source : kSources) distinct.insert(PickerSourceName(source));
    EXPECT_EQ(distinct.size(), kSources.size());

    for (const PickerState::Source source : kSources) {
      const std::string_view name = PickerSourceName(source);
      std::error_code ec;
      std::filesystem::create_directories(state.parent_path(), ec);
      {
        std::ofstream out{state, std::ios::binary | std::ios::trunc};
        out << name << '\t' << "the-query";
      }

      // Read back first: the writer and the reader have to agree before the
      // dispatch below means anything.
      std::string got_name;
      std::string got_query;
      EXPECT_TRUE(ReadLastPicker(got_name, got_query));
      EXPECT_EQ(got_name, std::string{name});
      EXPECT_EQ(got_query, std::string{"the-query"});

      Editor reopened;
      ResetToOriginal(reopened.doc.table, "x\n");
      reopened.status.clear();
      LastPicker(reopened);
      // No terminal is attached, so each of these stops early for its own
      // reason. The one answer that means the name was not recognised is the
      // one that must not appear.
      const std::string said{reopened.status};
      if (said.find("no picker called") != std::string::npos) {
        std::cerr << "    LastPicker does not handle " << name << "\n";
      }
      EXPECT_TRUE(said.find("no picker called") == std::string::npos);
    }
  }

  TEST_CASE("picker: a recorded query comes back whole, tab or no tab");
  {
    const Scratch scratch{"koi-lastpicker-tab"};
    const std::filesystem::path was_root = ProjectRoot();
    SetProjectRoot(scratch.dir);
    struct Restore {
      std::filesystem::path back;
      ~Restore() { SetProjectRoot(back); }
    } restore{was_root};

    // The separator is a tab, so a query holding one used to come back cut at
    // it. Nothing types a tab into a picker prompt -- that is the step key --
    // but a paste can carry one in.
    WriteLastPicker("content", "alpha\tbravo");
    std::string name;
    std::string query;
    EXPECT_TRUE(ReadLastPicker(name, query));
    EXPECT_EQ(name, std::string{"content"});
    EXPECT_EQ(query, std::string{"alpha bravo"});

    WriteLastPicker("symbols", "alpha bravo");
    EXPECT_TRUE(ReadLastPicker(name, query));
    EXPECT_EQ(name, std::string{"symbols"});
    EXPECT_EQ(query, std::string{"alpha bravo"});
  }
}


// Pins name files, not positions, so what a jump has to get right is the
// position it derives -- and there are two sources for it, in this order: the
// live cursor of an open buffer, and `files.last_line` for one that is closed.
void FilePinsLandWhereYouLeft() {
  TEST_CASE("pins: a jump lands where you last were, not where you pinned");

  const Scratch scratch{"koi-file-pins"};
  const AsProjectRoot root{scratch.dir};
  const std::filesystem::path a = scratch.Write("fa.cpp", NumberedLines(40));
  const std::filesystem::path b = scratch.Write("fb.cpp", NumberedLines(40));
  Editor ed;
  ed.theme = BuiltinTheme();
  std::string db_error;
  ed.project = ProjectStore::Open(scratch.dir / "fp.db", db_error);
  EXPECT_TRUE(ed.project != nullptr);
  if (ed.project == nullptr) return;

  EXPECT_TRUE(OpenTarget(ed, a.string() + ":5"));
  RunTypableCommand(ed, "pin 1");
  EXPECT_TRUE(OpenTarget(ed, b.string() + ":9"));
  RunTypableCommand(ed, "pin 2");

  // The pin was placed at line 5 and the cursor has moved a long way since.
  // A pinned *position* would still say 5.
  EXPECT_TRUE(OpenTarget(ed, a.string() + ":30"));
  RunTypableCommand(ed, "jump-pin 2");
  EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{8});
  RunTypableCommand(ed, "jump-pin 1");
  EXPECT_EQ(ed.doc.file.filename().string(), std::string{"fa.cpp"});
  EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{29});

  // And it keeps following. Nothing is re-pinned here.
  RunCommands(ed, {"move_line_down", "move_line_down"});
  RunTypableCommand(ed, "jump-pin 2");
  RunTypableCommand(ed, "jump-pin 1");
  EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{31});

  TEST_CASE("pins: a closed file falls back to what the store last recorded");
  {
    // The live cursor is the better answer only while there is one. A fresh
    // editor sharing the same database has no buffers, so this is the store's
    // half of the same jump -- and it has to agree with where the other editor
    // left off.
    Editor cold;
    cold.theme = BuiltinTheme();
    cold.project = ed.project;
    ResetToOriginal(cold.doc.table, "");
    cold.doc.selections.Set(MinWidth1(cold.doc.table, Selection{0, 0, -1}));
    RunTypableCommand(cold, "jump-pin 1");
    EXPECT_EQ(cold.doc.file.filename().string(), std::string{"fa.cpp"});
    EXPECT_EQ(LineAt(cold.doc.table, Cur(cold)), Index{31});
  }

  TEST_CASE("pins: an unset slot says so and moves nothing");
  {
    const std::filesystem::path was = ed.doc.file;
    const Index at = Cur(ed);
    RunTypableCommand(ed, "jump-pin 4");
    EXPECT_TRUE(ed.status.find("pin 4 is not set") != std::string::npos);
    EXPECT_EQ(ed.doc.file, was);
    EXPECT_EQ(Cur(ed), at);
  }
}

// goto_last_edit reads the revision trees rather than any stored position, so
// what it has to get right is which buffer holds the newest step and where that
// step left the primary cursor.
void LastEditIsFoundAcrossFiles() {
  TEST_CASE("goto_last_edit: the newest edit wins, whichever file it is in");

  const Scratch scratch{"koi-last-edit"};
  // The store keeps the paths of its own project, so the fixture is one.
  const AsProjectRoot root{scratch.dir};
  const std::filesystem::path a = scratch.Write("ea.cpp", NumberedLines(30));
  const std::filesystem::path b = scratch.Write("eb.cpp", NumberedLines(30));
  Editor ed;
  ed.theme = BuiltinTheme();

  RunTypableCommand(ed, "goto_last_edit");
  EXPECT_TRUE(OpenTarget(ed, a.string()));
  RunCommands(ed, {"goto_last_edit"});
  EXPECT_TRUE(ed.status.find("nothing has been edited yet") != std::string::npos);

  EXPECT_TRUE(OpenTarget(ed, a.string() + ":6"));
  TypeInto(ed, 'X');
  EXPECT_TRUE(OpenTarget(ed, b.string() + ":20"));
  TypeInto(ed, 'Y');

  // Back to a, well away from either edit, and then home again. The newest
  // edit is b's, so that is where this goes -- across the file boundary,
  // without either file having been pinned or recorded anywhere.
  EXPECT_TRUE(OpenTarget(ed, a.string() + ":2"));
  RunCommands(ed, {"goto_last_edit"});
  EXPECT_EQ(ed.doc.file.filename().string(), std::string{"eb.cpp"});
  EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{19});

  TEST_CASE("goto_last_edit: an older edit takes over once the newest is undone");
  {
    // `current` walks back with the undo, and the step it lands on is a's --
    // which is the right answer, because after the undo b's edit no longer
    // exists to go back to.
    RunCommands(ed, {"undo"});
    RunCommands(ed, {"goto_last_edit"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"ea.cpp"});
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{5});
  }

  TEST_CASE("goto_last_edit: it is the primary cursor that is followed");
  {
    EXPECT_TRUE(OpenTarget(ed, b.string() + ":3"));
    RunCommands(ed, {"copy_selection_on_next_line", "copy_selection_on_next_line"});
    EXPECT_TRUE(ed.doc.selections.Ranges().size() >= 3u);
    const std::size_t primary = ed.doc.selections.PrimaryIndex();
    TypeInto(ed, 'Z');
    const Index want = LineAt(ed.doc.table, ed.doc.selections.Ranges()[primary].head);
    EXPECT_TRUE(OpenTarget(ed, a.string() + ":28"));
    RunCommands(ed, {"goto_last_edit"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"eb.cpp"});
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), want);
  }

  TEST_CASE("goto_last_edit: a jump back to it is a jump, not a scroll");
  {
    // It goes through the same door every cross-file move does, so `[[` comes
    // back from it -- which needs a jump store to have anywhere to record.
    std::string jump_error;
    ed.jumps = OpenJumpStore(scratch.dir / "jumps.db", "pane-edit", jump_error);
    EXPECT_TRUE(ed.jumps != nullptr);
    EXPECT_TRUE(OpenTarget(ed, a.string() + ":12"));
    RunCommands(ed, {"goto_last_edit"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"eb.cpp"});
    RunCommands(ed, {"jump_backward"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"ea.cpp"});
    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{11});
  }
}

namespace {

// A source file with two functions and a line that appears twice, which is
// what the symbol, content and uniq assertions below are made of.
// The two functions are more than kLocationMergeLines apart, so a record in
// each is a record in two places.
constexpr std::string_view kRecordSample =
    "#include <cstdio>\n"    // 1
    "\n"                     // 2
    "int Alpha(int n) {\n"   // 3
    "  int total = 0;\n"     // 4
    "  total += n;\n"        // 5
    "  return total;\n"      // 6
    "}\n"                    // 7
    "\n"                     // 8
    "// filler 1\n"          // 9
    "// filler 2\n"          // 10
    "// filler 3\n"          // 11
    "// filler 4\n"          // 12
    "// filler 5\n"          // 13
    "// filler 6\n"          // 14
    "// filler 7\n"          // 15
    "// filler 8\n"          // 16
    "// filler 9\n"          // 17
    "\n"                     // 18
    "int Beta(int n) {\n"    // 19
    "  int total = 0;\n"     // 20  -- the same line as 4
    "  return total * 2;\n"  // 21
    "}\n";                   // 22

void PutCursorOnLine(Editor& ed, Index line) {
  const Index at = LineStart(ed.doc.table, line - 1);
  ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
}

}

void BoundaryRecording() {
  TEST_CASE("recording: what a location row says about the place it was made at");

  const Scratch scratch{"koi-record-boundary"};
  const AsProjectRoot root{scratch.dir};
  const std::filesystem::path source = scratch.Write("rec.cpp", kRecordSample);
  const std::filesystem::path db = scratch.dir / "rec.db";

  Editor ed;
  ed.theme = BuiltinTheme();
  std::string error;
  ed.project = ProjectStore::Open(db, error);
  EXPECT_TRUE(ed.project != nullptr);
  if (ed.project == nullptr) return;
  EXPECT_FALSE(static_cast<bool>(LoadDocument(source, ed.doc)));
  ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
  AttachSyntax(ed);
  EXPECT_TRUE(ed.doc.syntax != nullptr);

  {
    // Inside Alpha: the enclosing definition comes off the live tree, and the
    // duplicated line is not unique in this buffer.
    PutCursorOnLine(ed, 4);
    LocationRecord row;
    EXPECT_TRUE(LocationHere(ed, row));
    EXPECT_EQ(row.line, Index{4});
    EXPECT_EQ(row.symbol, std::string{"Alpha"});
    EXPECT_EQ(row.content, std::string{"int total = 0;"});
    // Two lines either side, in order, and the blank one keeps its place.
    EXPECT_EQ(row.context, std::string{"\nint Alpha(int n) {\ntotal += n;\nreturn total;"});
    EXPECT_EQ(row.uniq, std::int64_t{2});

    // The other function, and a line only it has.
    PutCursorOnLine(ed, 21);
    EXPECT_TRUE(LocationHere(ed, row));
    EXPECT_EQ(row.symbol, std::string{"Beta"});
    EXPECT_EQ(row.content, std::string{"return total * 2;"});
    EXPECT_EQ(row.uniq, std::int64_t{1});

    // Outside every function there is no enclosing definition to name.
    PutCursorOnLine(ed, 1);
    EXPECT_TRUE(LocationHere(ed, row));
    EXPECT_EQ(row.symbol, std::string{});
  }

  TEST_CASE("recording: the blob is git's, and only for a buffer that is still the file");
  {
    // Computed by hand from the rule git uses -- sha1 of "blob <len>\0" and the
    // bytes -- so this is checked against the definition rather than against
    // this implementation of it.
    EXPECT_EQ(GitBlobOid(""), std::string{"e69de29bb2d1d6434b8b29ae775ad8c2e48c5391"});
    EXPECT_EQ(GitBlobOid("hello"), std::string{"b6fc4c620b67d95f953a5c1c1230aaab5db5a1b0"});
    EXPECT_EQ(GitBlobOid("what is up, doc?"),
              std::string{"bd9dbf5aae1a3862dd1526723246b20206e5fc37"});

    PutCursorOnLine(ed, 5);
    LocationRecord row;
    EXPECT_TRUE(LocationHere(ed, row));
    EXPECT_EQ(row.blob, GitBlobOid(kRecordSample));

    // Dirty against disk: the blob would name bytes this buffer no longer
    // holds, so there is none.
    ed.doc.modified = true;
    EXPECT_TRUE(LocationHere(ed, row));
    EXPECT_EQ(row.blob, std::string{});
    ed.doc.modified = false;
  }

  TEST_CASE("recording: content and context are cut on a code point");
  {
    const std::filesystem::path wide = scratch.Write("wide.txt", "");
    std::string line;
    // Two hundred and one bytes of three-byte code points, so the cut lands
    // inside one and has to walk back off it.
    for (int i = 0; i < 67; ++i) line += "\xE4\xB8\xAD";
    WriteFixtureFile(wide, "  " + line + "  \n" + line + "\ntail\n");

    Editor other;
    other.theme = BuiltinTheme();
    other.project = ed.project;
    EXPECT_FALSE(static_cast<bool>(LoadDocument(wide, other.doc)));
    other.doc.selections.Set(MinWidth1(other.doc.table, Selection{0, 0, -1}));
    PutCursorOnLine(other, 2);
    LocationRecord row;
    EXPECT_TRUE(LocationHere(other, row));
    // Whitespace off both ends, 198 bytes kept of 201, and still valid UTF-8.
    EXPECT_EQ(row.content.size(), std::size_t{198});
    EXPECT_TRUE(IsWellFormedUtf8(row.content));
    EXPECT_TRUE(IsWellFormedUtf8(row.context));
    // Four entries, always: the slot off the top of the file, the line above
    // cut the same way, the short one below, and the empty last line of the
    // file after it. Positional, so empties and off-the-end slots both count.
    EXPECT_EQ(row.context.size(), std::size_t{1 + 198 + 1 + 4 + 1});
    // Both wide lines normalise to the same text, so neither is unique.
    EXPECT_EQ(row.uniq, std::int64_t{2});

    // And a cut that lands on a boundary keeps every byte it is allowed.
    WriteFixtureFile(wide, std::string(250, 'x') + "\n");
    EXPECT_FALSE(static_cast<bool>(LoadDocument(wide, other.doc)));
    other.doc.selections.Set(MinWidth1(other.doc.table, Selection{0, 0, -1}));
    EXPECT_TRUE(LocationHere(other, row));
    EXPECT_EQ(row.content.size(), std::size_t{200});
  }

  TEST_CASE("recording: linger and edit boundaries, and what they debounce");
  {
    sqlite3* reader = nullptr;
    EXPECT_EQ(sqlite3_open(db.c_str(), &reader), SQLITE_OK);
    const auto scalar = [&reader](const char* sql) {
      Stmt stmt{reader, sql};
      return (stmt && stmt.Step()) ? stmt.Integer(0) : std::int64_t{-1};
    };
    const auto rows = [&scalar] { return scalar("SELECT COUNT(*) FROM locations;"); };

    PutCursorOnLine(ed, 5);
    NoteCommandBoundary(ed);
    // Nothing yet: the cursor has only just arrived.
    NoteInputBoundary(ed);
    EXPECT_EQ(rows(), std::int64_t{0});

    // Three seconds of sitting still, which the test reaches by ageing the
    // arrival rather than by waiting for it.
    ed.record.since -= kLingerSeconds + 1;
    NoteInputBoundary(ed);
    EXPECT_EQ(rows(), std::int64_t{1});
    EXPECT_EQ(scalar("SELECT kind FROM locations;"), std::int64_t{0});
    EXPECT_EQ(scalar("SELECT line FROM locations;"), std::int64_t{5});
    EXPECT_EQ(scalar("SELECT uniq FROM locations;"), std::int64_t{1});

    // And once only, however many events go by while it sits there.
    ed.record.since -= kLingerSeconds + 1;
    NoteInputBoundary(ed);
    EXPECT_EQ(scalar("SELECT visits FROM locations;"), std::int64_t{1});

    // The symbol the linger was inside is a visit to that symbol, which is the
    // row `d <name>` reads and which nothing but a symbol jump used to write.
    EXPECT_EQ(scalar("SELECT visits FROM symbols WHERE symbol='Alpha';"), std::int64_t{1});
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM symbols;"), std::int64_t{1});
    // The line is where the definition starts, not where the cursor was in it.
    EXPECT_EQ(scalar("SELECT line FROM symbols WHERE symbol='Alpha';"), std::int64_t{3});

    // An edit is a boundary of its own, and this one is at the place the
    // linger recorded: one row, which the edit takes over. A place edited is
    // an edit row from then on -- the kind never walks back to visit.
    TypeInto(ed, 'X');
    NoteCommandBoundary(ed);
    EXPECT_EQ(rows(), std::int64_t{1});
    EXPECT_EQ(scalar("SELECT kind FROM locations;"), std::int64_t{1});
    // The file's own row counts the burst, not the keystrokes -- and the edit
    // is counted even though the linger's window has not run out, because a
    // visit and an edit at one place are two different things to record.
    EXPECT_EQ(scalar("SELECT edits FROM files;"), std::int64_t{1});

    // Typing on: one burst, one record. Neither the row nor the file's edit
    // count moves again inside the window.
    for (int i = 0; i < 20; ++i) {
      TypeInto(ed, 'y');
      NoteCommandBoundary(ed);
    }
    EXPECT_EQ(rows(), std::int64_t{1});
    EXPECT_EQ(scalar("SELECT visits FROM locations;"), std::int64_t{1});
    EXPECT_EQ(scalar("SELECT edits FROM files;"), std::int64_t{1});

    // Far enough away is a new place even inside the window, and a second
    // symbol to have been in.
    PutCursorOnLine(ed, 21);
    NoteCommandBoundary(ed);
    TypeInto(ed, 'Z');
    NoteCommandBoundary(ed);
    EXPECT_EQ(rows(), std::int64_t{2});
    EXPECT_EQ(scalar("SELECT edits FROM files;"), std::int64_t{2});
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM symbols;"), std::int64_t{2});
    EXPECT_EQ(scalar("SELECT visits FROM symbols WHERE symbol='Beta';"), std::int64_t{1});

    sqlite3_close(reader);
  }

  TEST_CASE("recording: a cold buffer records no symbol rather than parsing for one");
  {
    // No syntax attached is the shape a record takes on a buffer nothing has
    // painted yet. TextObjectRanges would parse the whole file to answer --
    // 21 ms on the input loop -- so the answer is that there is no answer.
    Editor cold;
    cold.project = ed.project;
    EXPECT_FALSE(static_cast<bool>(LoadDocument(source, cold.doc)));
    cold.doc.selections.Set(MinWidth1(cold.doc.table, Selection{0, 0, -1}));
    PutCursorOnLine(cold, 4);
    EXPECT_TRUE(cold.doc.syntax == nullptr);
    LocationRecord row;
    EXPECT_TRUE(LocationHere(cold, row));
    EXPECT_EQ(row.symbol, std::string{});
    EXPECT_EQ(row.content, std::string{"int total = 0;"});

    // And neither an excerpt view nor a buffer with no file is a place at all.
    Editor view;
    view.doc.view_name = "references: total";
    EXPECT_FALSE(LocationHere(view, row));
    Editor scratch_buffer;
    EXPECT_FALSE(LocationHere(scratch_buffer, row));
  }

  TEST_CASE("recording: a nested definition names itself, not the one it is written in");
  {
    // Python nests named functions, which is what the enclosing-symbol cache
    // has to survive. `inner`'s range is a strict subset of `outer`'s, so a
    // cache kept on "the cursor is still inside the range we answered from"
    // answers `outer` for every cursor in the body of `inner` -- a wrong name
    // on the row, a wrong name credited in `symbols`, and rows inside `inner`
    // merged onto the `outer` row whatever the distance between them.
    const std::filesystem::path nested = scratch.Write("nest.py",
                                                       "def outer(n):\n"          // 1
                                                       "    total = 0\n"          // 2
                                                       "    def inner(k):\n"      // 3
                                                       "        return k * 2\n"   // 4
                                                       "    total += inner(n)\n"  // 5
                                                       "    return total\n");     // 6

    Editor py;
    py.theme = BuiltinTheme();
    py.project = ed.project;
    EXPECT_FALSE(static_cast<bool>(LoadDocument(nested, py.doc)));
    py.doc.selections.Set(MinWidth1(py.doc.table, Selection{0, 0, -1}));
    AttachSyntax(py);
    EXPECT_TRUE(py.doc.syntax != nullptr);

    // No edit anywhere below, so all three answers come out of one revision --
    // which is the whole of the question being asked.
    LocationRecord row;
    PutCursorOnLine(py, 2);
    EXPECT_TRUE(LocationHere(py, row));
    EXPECT_EQ(row.symbol, std::string{"outer"});

    PutCursorOnLine(py, 4);
    EXPECT_TRUE(LocationHere(py, row));
    EXPECT_EQ(row.symbol, std::string{"inner"});

    // And back out of it again, in the same revision still.
    PutCursorOnLine(py, 6);
    EXPECT_TRUE(LocationHere(py, row));
    EXPECT_EQ(row.symbol, std::string{"outer"});
  }

  TEST_CASE("recording: a debounced boundary does not read the buffer");
  {
    // Describing a place is a read of the whole document -- the uniq census --
    // and an edit boundary is one record per revision by construction, so the
    // per-revision cache behind that census never once hits on the typing path.
    // The debounce therefore has to be decided before the place is described:
    // otherwise every keystroke of a burst pays for a description that is
    // thrown away, and what it pays scales with the file.
    std::string big;
    big.reserve(1900000);
    for (int i = 0; big.size() < 1800000; ++i) {
      big += "  const int value_" + std::to_string(i) + " = compute(" + std::to_string(i % 97) +
             ");\n";
    }
    const std::filesystem::path large = scratch.Write("large.cpp", big);

    Editor fat;
    fat.theme = BuiltinTheme();
    fat.project = ed.project;
    EXPECT_FALSE(static_cast<bool>(LoadDocument(large, fat.doc)));
    fat.doc.selections.Set(MinWidth1(fat.doc.table, Selection{0, 0, -1}));
    // Under both of the census's own ceilings, so it is a buffer the census
    // would run on if it were reached at all.
    EXPECT_TRUE(DocLength(fat.doc.table) < Index{2 * 1024 * 1024});
    EXPECT_TRUE(LineCount(fat.doc.table) < Index{100000});
    PutCursorOnLine(fat, 200);

    // The first two boundaries are the place arriving and then being edited:
    // both write. Everything after is the same place inside the same window.
    NoteCommandBoundary(fat);
    TypeInto(fat, 'x');
    NoteCommandBoundary(fat);

    constexpr int kKeystrokes = 100;
    long long best_us = -1;
    // Three passes, best of: the suite runs alongside whatever else the machine
    // is doing, and one descheduled run is not a regression.
    for (int pass = 0; pass < 3; ++pass) {
      const auto started = std::chrono::steady_clock::now();
      for (int i = 0; i < kKeystrokes; ++i) {
        TypeInto(fat, 'y');
        NoteCommandBoundary(fat);
      }
      const long long us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
      if ((best_us < 0) || (us < best_us)) best_us = us;
    }
    const long long per_us = best_us / kKeystrokes;
    std::cout << "boundary bench: " << kKeystrokes << " debounced boundaries at "
              << (DocLength(fat.doc.table) / 1024) << "KB in " << best_us << "us (" << per_us
              << "us each)\n";
    // The whole burst wrote one row and counted one edit burst, whatever it
    // cost: the measurement above is of the path that does nothing.
    EXPECT_EQ(fat.record.wrote_line, Index{200});

    // What the bound separates is a boundary whose cost is the buffer's size
    // from one whose cost is a handful of comparisons. Describing this place
    // costs about 4 ms, so a hundred of them is four hundred thousand
    // microseconds; deciding not to describe it is about two.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    // A sanitizer build is optimized and still several times slower; the number
    // measures the instrumentation, not the design.
    EXPECT_TRUE(best_us < 100000);
#elif defined(__OPTIMIZE__)
    EXPECT_TRUE(best_us < 5000);
#else
    EXPECT_TRUE(best_us < 100000);
#endif
  }
}
}  // namespace koi
