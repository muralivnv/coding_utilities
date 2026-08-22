// Tests for navigate.cpp: the pickers -- files, buffers, symbols -- their
// ranking, and the commands that drive them.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void PickerCommands() {
  TEST_CASE("pickers: expanding a configured command");

  Editor ed;
  ed.doc.file = "src/a.cpp";
  ResetToOriginal(ed.doc.table, "operator<<\n");
  ed.doc.selections.Set(Selection{0, 10, -1});

  const std::string all = ExpandPickerCommand(
      ed, "scan --from %{buffer_name} --containing %{selection} | pick --query %{user_query}",
      "(?i)x");
  EXPECT_TRUE(all.find("--from src/a.cpp") != std::string::npos);
  EXPECT_TRUE(all.find("--containing 'operator<<'") != std::string::npos);
  EXPECT_TRUE(all.find("--query '(?i)x'") != std::string::npos);

  EXPECT_TRUE(ExpandPickerCommand(ed, "pick --query %{user_query}", "; rm -rf /")
                  .find("'; rm -rf /'") != std::string::npos);

  const std::string foreign =
      ExpandPickerCommand(ed, "pick --preview {{@SELECTION@}} --nope %{nonsense}", "q");
  EXPECT_TRUE(foreign.find("{{@SELECTION@}}") != std::string::npos);
  EXPECT_TRUE(foreign.find("%{nonsense}") != std::string::npos);

  EXPECT_TRUE(ExpandVariables("pick --query %{user_query}", ed) ==
              std::string{"pick --query ''"});
}

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

void PickerPipelines() {
  TEST_CASE("pickers: the built-in pipelines keep their own contract");

  constexpr std::array<std::string_view, 6> kNames{
      {"files", "content", "symbols", "buffer-symbols", "definition", "references"}};
  Editor ed;
  ed.doc.file = "a.cpp";
  for (const std::string_view name : kNames) {
    const std::string command = PickerCommand(ed, name);
    EXPECT_TRUE(!command.empty());
    EXPECT_TRUE(command.find("%{user_query}") != std::string::npos);
  }
  EXPECT_TRUE(PickerCommand(ed, "nonesuch").empty());

  const auto first_stage_is_the_picker = [](std::string_view command) {
    while (!command.empty() && ((command.front() == '\n') || (command.front() == ' ') ||
                                (command.front() == '\t'))) {
      command.remove_prefix(1);
    }
    return command.starts_with("tooey");
  };
  for (const std::string_view name : {"files", "definition", "references"}) {
    EXPECT_TRUE(first_stage_is_the_picker(PickerCommand(ed, name)));
    EXPECT_TRUE(PickerCommand(ed, name).find("koi --symbol-mode") == std::string::npos);
  }
  EXPECT_TRUE(PickerCommand(ed, "definition").find("--select-1") == std::string::npos);

  EXPECT_TRUE(PickerCommand(ed, "symbols").find("%{koi} --symbol-mode") != std::string::npos);
  for (const std::string_view name : {"symbols", "buffer-symbols", "files"}) {
    const std::string expanded = ExpandPickerCommand(ed, PickerCommand(ed, name), "q");
    EXPECT_TRUE(expanded.find("%{koi}") == std::string::npos);
  }

  EXPECT_TRUE(PickerCommand(ed, "files").find(R"Q(-f "(?i)"{{@QUERY@}})Q") != std::string::npos);

  for (const std::string_view name : kNames) {
    const std::string expanded = ExpandPickerCommand(ed, PickerCommand(ed, name), "q");
    EXPECT_TRUE(expanded.find("%{") == std::string::npos);
    EXPECT_TRUE(expanded.find("{{@SELECTION@}}") != std::string::npos);
    EXPECT_TRUE(expanded.find("{{@QUERY@}}") != std::string::npos);
  }
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

  const auto first_row = [](const std::string& rows) {
    std::string name = rows.substr(0, rows.find('\n'));
    name = name.substr(0, name.find('\t'));
    while (!name.empty() && (name.back() == ' ')) name.pop_back();
    return name;
  };
  const auto row_count = [](const std::string& rows) {
    return static_cast<Index>(std::ranges::count(rows, '\n'));
  };

  const std::string cold = RankedFileRows(ed);
  EXPECT_EQ(row_count(cold), Index{4});

  ed.project->RecordVisit("charlie.txt", 7, 2);
  EXPECT_EQ(first_row(RankedFileRows(ed)), std::string{"charlie.txt"});
  ed.project->RecordEdit("delta.txt", 3, 1);
  EXPECT_EQ(first_row(RankedFileRows(ed)), std::string{"delta.txt"});

  const std::string warm = RankedFileRows(ed);
  EXPECT_EQ(row_count(warm), Index{4});
  for (const char* name : {"alpha.txt", "bravo.txt", "charlie.txt", "delta.txt"}) {
    EXPECT_TRUE(warm.find(name) != std::string::npos);
  }

  ed.project->RecordVisit("vanished.txt", 1, 1);
  EXPECT_EQ(row_count(RankedFileRows(ed)), Index{4});
  EXPECT_TRUE(RankedFileRows(ed).find("vanished.txt") == std::string::npos);

  EXPECT_TRUE(RankedFileRows(ed).find("delta.txt:3:1") != std::string::npos);

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

  const auto first_row = [](const std::string& rows) {
    std::string name = rows.substr(0, rows.find('\n'));
    name = name.substr(0, name.find('\t'));
    while (!name.empty() && (name.back() == ' ')) name.pop_back();
    return name;
  };

  // 84 against 80, both in the same recency bucket, so alpha is ahead by more
  // than a rounding wobble and by less than the 10% the bonus is worth.
  for (int i = 0; i < 21; ++i) ed.project->RecordVisit("alpha.txt", 1, 0);
  for (int i = 0; i < 20; ++i) ed.project->RecordVisit("bravo.txt", 1, 0);
  EXPECT_EQ(first_row(RankedFileRows(ed)), std::string{"alpha.txt"});

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

  EXPECT_EQ(first_row(RankedFileRows(ed)), std::string{"bravo.txt"});
}

void FileFilter() {
  TEST_CASE("pickers: file-filter feeds every project-wide pipeline");

  Editor ed;
  ed.doc.file = "a.cpp";
  EXPECT_TRUE(FileFilterCommand(ed).find("find .") != std::string::npos);

  ed.settings.file_filter = "git ls-files";
  EXPECT_EQ(FileFilterCommand(ed), std::string{"git ls-files"});
  for (const std::string_view name : {"content", "symbols"}) {
    EXPECT_TRUE(PickerCommand(ed, name).starts_with("git ls-files |"));
  }
  for (const std::string_view name : {"files", "buffer-symbols", "definition", "references"}) {
    EXPECT_TRUE(PickerCommand(ed, name).find("git ls-files") == std::string::npos);
  }

  ed.settings.file_filter = "find . -type f |\n  grep -v build |\n";
  EXPECT_TRUE(PickerCommand(ed, "symbols").starts_with("find . -type f |\n  grep -v build |"));
  EXPECT_TRUE(PickerCommand(ed, "symbols").find("| |") == std::string::npos);
}

void BufferPickerRows() {
  TEST_CASE("buffer picker: the command exists and is one shell command");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "x\n");
    const std::string command = PickerCommand(ed, "buffers");
    EXPECT_TRUE(!command.empty());
    EXPECT_TRUE(command.find("tooey") != std::string::npos);
    EXPECT_TRUE(command.find("[ Buffers ]") != std::string::npos);
  }

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

    const std::string items = BufferPickerItems(ed);
    std::vector<std::string> payloads;
    for (std::size_t at = 0; at < items.size();) {
      const std::size_t end = items.find('\n', at);
      payloads.emplace_back(RowPayload(std::string_view{items}.substr(at, end - at)));
      at = end + 1;
    }
    EXPECT_EQ(payloads.size(), BufferCount(ed));
    EXPECT_EQ(payloads[view_at], "#" + std::to_string(view_at));
    EXPECT_TRUE(payloads[file_at].find("one.txt") != std::string::npos);
    EXPECT_TRUE(items.find(view_name + ":") != std::string::npos);

    SwitchToBuffer(ed, view_at);
    ChooseBufferRow(ed, payloads[file_at]);
    EXPECT_FALSE(IsExcerptView(ed.doc));
    EXPECT_EQ(BufferCount(ed), std::size_t{2});

    ChooseBufferRow(ed, payloads[view_at]);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_EQ(ed.doc.view_name, view_name);
    EXPECT_EQ(BufferCount(ed), std::size_t{2});
    EXPECT_EQ(FindFileBuffer(ed, std::filesystem::path{view_name}), BufferCount(ed));
    EXPECT_TRUE(DocLength(ed.doc.table) > 0);
  }

  TEST_CASE("buffer picker: a name that cannot survive the row form is left out of it");
  {
    // A file row is its own payload and RowPayload keeps what follows the last
    // tab, so a tab in the name would come back as a shorter path -- a row that
    // opens a different file than the buffer it names.
    const Scratch scratch{"koi-buffer-picker-tab"};
    const std::filesystem::path odd = scratch.Write("a\tb.txt", "alpha\n");
    const std::filesystem::path plain = scratch.Write("plain.txt", "bravo\n");
    Editor ed;
    EXPECT_TRUE(OpenTarget(ed, odd.string()));
    EXPECT_TRUE(OpenTarget(ed, plain.string()));

    const std::string items = BufferPickerItems(ed);
    EXPECT_TRUE(items.find("plain.txt") != std::string::npos);
    EXPECT_TRUE(items.find("a\tb.txt") == std::string::npos);
    for (std::size_t at = 0; at < items.size();) {
      const std::size_t end = std::min(items.find('\n', at), items.size());
      const std::string_view payload = RowPayload(std::string_view{items}.substr(at, end - at));
      EXPECT_TRUE(!payload.starts_with("b.txt"));
      at = end + 1;
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
    const std::string items = BufferPickerItems(ed);
    EXPECT_TRUE(items.find("#" + std::to_string(unnamed) + " [scratch]") != std::string::npos);

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

void PickerCommandShape() {
  TEST_CASE("picker: the assembled command is one shell command, not several");
  Editor ed;
  ResetToOriginal(ed.doc.table, "x\n");
  for (const char* which : {"files", "content", "symbols", "buffer-symbols"}) {
    const std::string command = PickerCommand(ed, which);
    if (command.empty()) continue;

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
    if (broken) std::cerr << "    broken picker command for " << which << "\n";
    EXPECT_FALSE(broken);
  }
  TEST_CASE("picker: every picker that records a name can be reopened");
  {
    // Driven off PickerNames() rather than a list written out here, so adding a
    // pipeline without teaching LastPicker about it fails this instead of
    // shipping a picker that records itself as the last one and is then
    // unreachable -- which is what "buffers" did.
    const Scratch scratch{"koi-lastpicker"};
    const std::filesystem::path was_root = ProjectRoot();
    SetProjectRoot(scratch.dir);
    struct Restore {
      std::filesystem::path back;
      ~Restore() { SetProjectRoot(back); }
    } restore{was_root};

    const std::filesystem::path state = LastPickerStatePath();
    EXPECT_FALSE(state.empty());

    const std::vector<std::string_view> names = PickerNames();
    EXPECT_TRUE(names.size() >= 7);
    for (const std::string_view name : names) {
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
