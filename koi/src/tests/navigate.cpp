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

}  // namespace koi
