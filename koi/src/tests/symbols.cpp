// Tests for symbols.cpp: extracting symbols from a file, and the bounded,
// cancellable scans that feed the pickers.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

namespace {

std::vector<Symbol> ScanOne(const std::filesystem::path& path, SymbolKind kind,
                            std::string& error) {
  std::vector<Symbol> out;
  for (Symbol&& one : ScanSymbols(path, kind, error)) out.push_back(std::move(one));
  return out;
}

}  // namespace

void SymbolQueriesCompile() {
  TEST_CASE("symbols: every definitions/references query compiles");

  const std::vector<std::string_view> indexable = IndexableLanguages();
  EXPECT_TRUE(indexable.size() >= 10);

  for (const LanguageSample& sample : kLanguageSamples) {
    if (std::ranges::find(indexable, sample.language) == indexable.end()) continue;
    std::string error;
    std::ignore = ScanOne(std::filesystem::path{sample.filename}, SymbolKind::kBoth, error);
    if (!error.empty()) std::cout << "  " << sample.language << ": " << error << "\n";
    EXPECT_TRUE(error.empty());
  }
}

void ScanBudgetAndCancel() {
  TEST_CASE("symbol scan: cancel stops inside a parse; only a timeout is reported");

  const Scratch scratch{"koi-scan-cancel-test"};
  std::string source;
  for (int i = 0; i < 3000; ++i) {
    source += "int fn" + std::to_string(i) + "(int a) { return a + " + std::to_string(i) + "; }\n";
  }
  const std::filesystem::path file = scratch.Write("big.cpp", source);
  const std::array<std::string, 1> paths{file.string()};
  const std::span<const std::string> span{paths};
  std::string error;

  std::atomic<bool> cancel{true};
  std::vector<Symbol> none;
  for (Symbol&& one : ScanSymbols(span, SymbolKind::kDefinitions, error, {}, &cancel)) {
    none.push_back(std::move(one));
  }
  EXPECT_TRUE(none.empty());
  EXPECT_TRUE(error.empty());

  cancel.store(false);
  std::vector<Symbol> defs;
  for (Symbol&& one : ScanSymbols(span, SymbolKind::kDefinitions, error, {}, &cancel)) {
    defs.push_back(std::move(one));
  }
  EXPECT_EQ(defs.size(), std::size_t{3000});
  EXPECT_TRUE(error.empty());

  std::string compile_error;
  constexpr std::array<std::string_view, 1> query_files{"definitions.scm"};
  const std::shared_ptr<CompiledQuery> compiled = CompileQuery("cpp", query_files, compile_error);
  EXPECT_TRUE(compiled != nullptr);
  const std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser{ts_parser_new(),
                                                                      ts_parser_delete};
  const ParsedBuffer starved =
      ParseBuffer(parser.get(), *compiled, "cpp", source, std::chrono::milliseconds{0});
  EXPECT_TRUE(!starved);
  EXPECT_TRUE(starved.timed_out);
  EXPECT_TRUE(starved.grammar_error.empty());

  const ParsedBuffer parsed =
      ParseBuffer(parser.get(), *compiled, "cpp", source, std::chrono::seconds{10});
  EXPECT_TRUE(bool{parsed});
  EXPECT_TRUE(!parsed.timed_out);
}

void CancelStopsTheWalk() {
  TEST_CASE("symbol scan: a cancelled scan stops walking instead of reading the list");

  const Scratch scratch{"koi-scan-walk-cancel-test"};
  const std::filesystem::path a =
      scratch.Write("a.cpp", "int a1(int x) { return x; }\nint a2(int x) { return x; }\n");
  const std::filesystem::path b =
      scratch.Write("b.cpp", "int b1(int x) { return x; }\nint b2(int x) { return x; }\n");

  // A directory wearing a .cpp name: the scan picks a language for it, compiles
  // the query, and only the whole-file read discovers it is not a file --
  // ReadWholeFile answers is_a_directory, which the scan records as "cannot
  // read". That recorded error is the observable this test needs. It appears if
  // and only if the scan actually opened the path, so its *absence* is proof
  // that a cancelled scan never got that far. Nothing else about a skipped file
  // is visible from outside without a production hook.
  const std::filesystem::path probe = scratch.dir / "probe.cpp";
  std::filesystem::create_directory(probe);
  const std::string probe_error = "cannot read " + probe.string() + ": " +
                                  std::make_error_code(std::errc::is_a_directory).message();

  const std::array<std::string, 3> probe_first{probe.string(), a.string(), b.string()};
  const std::span<const std::string> from_probe{probe_first};

  // Pre-cancelled: nothing yielded, and -- the part that matters -- the very
  // first path was never opened, so no error was recorded for it.
  std::atomic<bool> cancel{true};
  std::string error;
  std::vector<Symbol> none;
  for (Symbol&& one : ScanSymbols(from_probe, SymbolKind::kDefinitions, error, {}, &cancel)) {
    none.push_back(std::move(one));
  }
  EXPECT_TRUE(none.empty());
  EXPECT_EQ(error, std::string{});

  // Control: the same list, uncancelled, finds every definition and does record
  // the unreadable path -- so the assertion above is about cancellation and not
  // about the fixture being unreachable.
  cancel.store(false);
  std::vector<Symbol> all;
  for (Symbol&& one : ScanSymbols(from_probe, SymbolKind::kDefinitions, error, {}, &cancel)) {
    all.push_back(std::move(one));
  }
  EXPECT_EQ(all.size(), std::size_t{4});
  EXPECT_EQ(error, probe_error);

  // Cleared by hand from here on: a scan adds to the string it is handed and
  // never wipes it, so what the control run above recorded would otherwise
  // still be sitting there and "this scan said nothing" would be unaskable.
  error.clear();

  // A word filter that matches nothing was the worst case: the file is rejected
  // before anything is yielded, so the generator never suspends and the
  // consumer never gets a turn to notice it was cancelled. Only a check inside
  // the scan can stop this one.
  cancel.store(true);
  std::vector<Symbol> filtered;
  for (Symbol&& one :
       ScanSymbols(from_probe, SymbolKind::kDefinitions, error, "nowhere", &cancel)) {
    filtered.push_back(std::move(one));
  }
  EXPECT_TRUE(filtered.empty());
  EXPECT_EQ(error, std::string{});

  // Cancelled after the first symbol: the generator finishes without yielding
  // again, and never reaches the unreadable path at the end of the list.
  const std::array<std::string, 3> probe_last{a.string(), b.string(), probe.string()};
  const std::span<const std::string> from_files{probe_last};
  cancel.store(false);
  error.clear();
  std::vector<Symbol> stopped_early;
  for (Symbol&& one : ScanSymbols(from_files, SymbolKind::kDefinitions, error, {}, &cancel)) {
    stopped_early.push_back(std::move(one));
    cancel.store(true);
  }
  EXPECT_EQ(stopped_early.size(), std::size_t{1});
  EXPECT_EQ(error, std::string{});

  // And uncancelled over that ordering, so the count above is a stop rather
  // than all this fixture had to give.
  cancel.store(false);
  std::vector<Symbol> whole;
  for (Symbol&& one : ScanSymbols(from_files, SymbolKind::kDefinitions, error, {}, &cancel)) {
    whole.push_back(std::move(one));
  }
  EXPECT_EQ(whole.size(), std::size_t{4});
  EXPECT_EQ(error, probe_error);
}

void SymbolExtraction() {
  TEST_CASE("symbols: extraction");

  const Scratch scratch{"koi-symbol-test"};
  const std::filesystem::path source = scratch.Write("thing.cpp",
                                                     "struct Widget {\n"
                                                     "  int Size() const;\n"
                                                     "};\n"
                                                     "int Widget::Size() const { return 1; }\n"
                                                     "void Use() { Widget w; w.Size(); }\n");

  std::string error;
  const std::vector<Symbol> defs = ScanOne(source, SymbolKind::kDefinitions, error);
  EXPECT_TRUE(error.empty());

  const auto names_include = [](const std::vector<Symbol>& rows, std::string_view name) {
    return std::ranges::any_of(rows, [name](const Symbol& one) { return one.name == name; });
  };
  EXPECT_TRUE(names_include(defs, "Widget"));
  for (const Symbol& one : defs) {
    EXPECT_TRUE(one.line >= 1);
    EXPECT_TRUE(one.column >= 1);
    EXPECT_EQ(one.path, source.string());
  }

  const std::vector<Symbol> refs = ScanOne(source, SymbolKind::kReferences, error);
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(std::ranges::any_of(refs, [](const Symbol& one) {
    return one.name.find("w.Size()") != std::string::npos;
  }));

  const std::vector<Symbol> none =
      ScanOne(scratch.Write("notes.txt", "hello\n"), SymbolKind::kBoth, error);
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(none.empty());

  const std::vector<Symbol> both = ScanOne(source, SymbolKind::kBoth, error);
  EXPECT_TRUE(both.size() > defs.size());

  const std::vector<std::string> paths{source.string(), (scratch.dir / "gone.cpp").string()};
  const std::vector<Symbol> many = CollectSymbols(paths, SymbolKind::kDefinitions, error);
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(many.size(), defs.size());

  const std::vector<Symbol> filtered =
      CollectSymbols(paths, SymbolKind::kBoth, error, "Widget");
  EXPECT_TRUE(!filtered.empty());
  EXPECT_TRUE(std::ranges::all_of(filtered, [](const Symbol& one) {
    return ContainsWord(one.name, "Widget");
  }));

  {
    std::string scan_error;
    auto scan = ScanSymbols(paths, SymbolKind::kBoth, scan_error);
    auto it = scan.begin();

    if (it != scan.end()) {
      const Symbol first = *it;
      EXPECT_EQ(first.path, source.string());
      EXPECT_EQ(first.line, both.front().line);
    } else {
      EXPECT_TRUE(it != scan.end());
    }
  }

  TEST_CASE("symbols: the position points at the name, not at what was stripped");
  {
    // A markdown heading node owns its indentation, and the name has that
    // stripped off it -- so the position has to move with the text that was
    // kept, or the row says line 3, column 1 and lands on the whitespace.
    const std::filesystem::path notes = scratch.Write("notes.md",
                                                      "# Top\n"
                                                      "\n"
                                                      "   ## Indented Two\n"
                                                      "\n"
                                                      "   Setext Indented\n"
                                                      "   ===============\n");
    std::string notes_error;
    const std::vector<Symbol> heads = ScanOne(notes, SymbolKind::kDefinitions, notes_error);
    EXPECT_TRUE(notes_error.empty());

    const auto at = [&heads](std::string_view prefix) {
      const auto hit = std::ranges::find_if(heads, [prefix](const Symbol& one) {
        return std::string_view{one.name}.starts_with(prefix);
      });
      return (hit == heads.end()) ? Symbol{"", -1, -1, ""} : *hit;
    };

    const Symbol top = at("# Top");
    EXPECT_EQ(top.line, Index{1});
    EXPECT_EQ(top.column, Index{1});

    // Three spaces in front of the '#': column 4, one-based, in bytes.
    const Symbol indented = at("## Indented Two");
    EXPECT_EQ(indented.line, Index{3});
    EXPECT_EQ(indented.column, Index{4});

    const Symbol setext = at("Setext Indented");
    EXPECT_EQ(setext.line, Index{5});
    EXPECT_EQ(setext.column, Index{4});

    // The row moves too when the stripped run spans a line. No query that ships
    // captures such a node -- every capture here begins on the name's own line
    // -- so this stays a property of the code, checked by reading it.
  }
}

void SymbolRows() {
  TEST_CASE("symbols: the row form");

  const Symbol one{"src/a.cpp", 42, 7, "Widget"};
  EXPECT_EQ(FormatSymbolRow(one), std::string{"src/a.cpp:42:7:Widget"});
  EXPECT_EQ(FormatSymbolRow({"src/a.cpp", 42, 7, ""}), std::string{"src/a.cpp:42:7"});

  Symbol back;
  EXPECT_TRUE(ParseSymbolRow("src/a.cpp:42:7:Widget", back));
  EXPECT_EQ(back.path, one.path);
  EXPECT_EQ(back.line, one.line);
  EXPECT_EQ(back.column, one.column);
  EXPECT_EQ(back.name, one.name);

  EXPECT_TRUE(ParseSymbolRow("a.cpp:1:1:koi::Widget::Open()", back));
  EXPECT_EQ(back.name, std::string{"koi::Widget::Open()"});

  EXPECT_TRUE(ParseSymbolRow("a.cpp:12:Widget", back));
  EXPECT_EQ(back.line, 12);
  EXPECT_EQ(back.column, 1);
  EXPECT_EQ(back.name, std::string{"Widget"});

  EXPECT_TRUE(ParseSymbolRow("a.cpp:12:7", back));
  EXPECT_EQ(back.column, 7);
  EXPECT_TRUE(back.name.empty());
  EXPECT_TRUE(ParseSymbolRow("a.cpp:12:0", back));
  EXPECT_EQ(back.column, 1);

  EXPECT_TRUE(ParseSymbolRow("src/main.cpp:12", back));
  EXPECT_EQ(back.path, std::string{"src/main.cpp"});
  EXPECT_EQ(back.line, 12);
  EXPECT_EQ(back.column, 1);
  EXPECT_TRUE(back.name.empty());

  EXPECT_TRUE(!ParseSymbolRow("", back));

  EXPECT_TRUE(ParseSymbolRow("odd:name.cpp:12", back));
  EXPECT_EQ(back.path, std::string{"odd:name.cpp"});
  EXPECT_EQ(back.line, 12);

  EXPECT_TRUE(ParseSymbolRow("a.cpp:3:99999999999999999999:f", back));
  EXPECT_EQ(back.line, 3);
  EXPECT_EQ(back.column, 1);
  // No field survives the overflow, so nothing here is a position -- and the
  // whole string is not the name of any file, so it is not a target either.
  EXPECT_TRUE(!ParseSymbolRow("a.cpp:99999999999999999999", back));

  TEST_CASE("symbols: a row without a position has to name a file that is there");
  {
    const Scratch row_scratch{"koi-symbol-row-parse"};
    const std::filesystem::path real = row_scratch.Write("here.txt", "x\n");
    const std::string gone = (row_scratch.dir / "gone.txt").string();

    // The file picker's rows are bare paths; those still open.
    Symbol bare;
    EXPECT_TRUE(ParseSymbolRow(real.string(), bare));
    EXPECT_EQ(bare.path, real.string());
    EXPECT_EQ(bare.line, Index{1});
    EXPECT_EQ(bare.column, Index{1});
    EXPECT_TRUE(bare.name.empty());

    // Everything else a picker prints is not a target, and `out` is left as it
    // was so a caller cannot half-read a row that was refused.
    Symbol junk;
    junk.path = "untouched";
    junk.line = 7;
    EXPECT_FALSE(ParseSymbolRow("  3 matches for Widget", junk));
    EXPECT_EQ(junk.path, std::string{"untouched"});
    EXPECT_EQ(junk.line, Index{7});
    EXPECT_FALSE(ParseSymbolRow("[ Symbols ]", junk));
    EXPECT_FALSE(ParseSymbolRow(":::", junk));
    EXPECT_FALSE(ParseSymbolRow("a.cpp", junk));
    EXPECT_FALSE(ParseSymbolRow(gone, junk));

    // A positioned row is still a target whether or not the file is there: a
    // jump to a file that was just deleted still says where it meant to go.
    Symbol positioned;
    EXPECT_TRUE(ParseSymbolRow(gone + ":4:2:Widget", positioned));
    EXPECT_EQ(positioned.path, gone);
    EXPECT_EQ(positioned.line, Index{4});
    EXPECT_EQ(positioned.column, Index{2});
  }

  TEST_CASE("symbols: a row may be parsed out of the symbol it is written into");
  {
    // `row` views into `out.path`. Publishing the path first freed the buffer
    // the rest of the parse reads -- a use-after-free the parse walked straight
    // into, with the fields it recovered coming out of freed memory.
    Symbol self;
    self.path = "src/some/longer/path/a.cpp:12:3:Widget";
    EXPECT_TRUE(ParseSymbolRow(std::string_view{self.path}, self));
    EXPECT_EQ(self.path, std::string{"src/some/longer/path/a.cpp"});
    EXPECT_EQ(self.line, Index{12});
    EXPECT_EQ(self.column, Index{3});
    EXPECT_EQ(self.name, std::string{"Widget"});
  }

  TEST_CASE("symbols: the picker row form");

  const std::string row = PickerRow("display", "a.cpp:1:1:f");
  EXPECT_EQ(RowPayload(row), std::string_view{"a.cpp:1:1:f"});
  EXPECT_TRUE(row.size() > 300);
  EXPECT_EQ(RowPayload("plain"), std::string_view{"plain"});

  const Symbol widget{"src/a.cpp", 42, 7, "Widget"};
  const std::string rendered = SymbolPickerRow(widget);
  EXPECT_TRUE(rendered.starts_with("Widget\t"));
  EXPECT_TRUE(rendered.find("a.cpp:42") != std::string::npos);
  EXPECT_EQ(std::ranges::count(rendered, '\t'), 2);
  EXPECT_EQ(RowPayload(rendered), std::string_view{"src/a.cpp:42:7:Widget"});
  Symbol parsed;
  EXPECT_TRUE(ParseSymbolRow(RowPayload(rendered), parsed));
  EXPECT_EQ(parsed.path, widget.path);
  EXPECT_EQ(parsed.line, widget.line);
  EXPECT_EQ(parsed.column, widget.column);
  EXPECT_EQ(parsed.name, widget.name);

  TEST_CASE("symbols: a row that cannot say where it points is not made at all");
  {
    // RowPayload takes everything after the last tab, so a tab in the path used
    // to hand back "b.cpp:2:3:Widget" -- a file that is not the one the row was
    // about, and one koi would create on opening.
    const Symbol tabbed{"src/a\tb.cpp", 2, 3, "Widget"};
    EXPECT_TRUE(SymbolPickerRow(tabbed).empty());
    const Symbol lined{"src/a\nb.cpp", 2, 3, "Widget"};
    EXPECT_TRUE(SymbolPickerRow(lined).empty());
    const Symbol returned{"src/a\rb.cpp", 2, 3, "Widget"};
    EXPECT_TRUE(SymbolPickerRow(returned).empty());

    EXPECT_TRUE(PickerRow("display", "src/a\tb.cpp:2:3").empty());
    EXPECT_TRUE(PickerRow("display", "src/a\nb.cpp:2:3").empty());
    EXPECT_TRUE(PickerRow("two\nlines", "a.cpp:1:1").empty());

    // The tab *between* the display's columns is what the row form is made of,
    // and it stays: the payload is what has to be free of them.
    const std::string columns = PickerRow("Widget\ta.cpp:42", "src/a.cpp:42:7:Widget");
    EXPECT_TRUE(!columns.empty());
    EXPECT_EQ(RowPayload(columns), std::string_view{"src/a.cpp:42:7:Widget"});

    // A name with a tab in it is a display, not a path -- still one good row.
    const std::string named = SymbolPickerRow(Symbol{"src/a.cpp", 2, 3, "Widget"});
    EXPECT_TRUE(!named.empty());
    EXPECT_EQ(RowPayload(named), std::string_view{"src/a.cpp:2:3:Widget"});
  }
}

namespace {

std::string SymbolModeOutput(std::span<const std::string> paths, const SymbolModeOptions& options,
                             std::string& error) {
  std::FILE* sink = std::tmpfile();
  if (sink == nullptr) return {};
  std::ignore = WriteSymbols(paths, options, sink, error);
  std::fflush(sink);
  std::rewind(sink);
  std::string text;
  char buffer[8192];
  while (const size_t n = std::fread(buffer, 1, sizeof(buffer), sink)) text.append(buffer, n);
  std::fclose(sink);
  return text;
}

std::vector<std::string> SplitLines(std::string_view text) {
  std::vector<std::string> lines;
  for (size_t at = 0; at < text.size();) {
    const size_t eol = std::min(text.find('\n', at), text.size());
    if (eol > at) lines.emplace_back(text.substr(at, eol - at));
    at = eol + 1;
  }
  return lines;
}

}  // namespace

void ScanErrorOutlivesTheNextScan() {
  TEST_CASE("symbol scan: a diagnostic outlives the next scan's silence");

  const Scratch scratch{"koi-scan-error-carry"};
  const std::filesystem::path good = scratch.Write("good.cpp", "int good(int x) { return x; }\n");

  // The cancellation test's trick: a directory wearing a .cpp name gets a
  // language and a query, and only the read discovers it is not a file --
  // recorded as "cannot read". It is the cheapest diagnostic to seed.
  const std::filesystem::path probe = scratch.dir / "probe.cpp";
  std::filesystem::create_directory(probe);
  const std::string probe_error = "cannot read " + probe.string() + ": " +
                                  std::make_error_code(std::errc::is_a_directory).message();
  const std::filesystem::path second = scratch.dir / "second.cpp";
  std::filesystem::create_directory(second);

  const std::array<std::string, 1> broken{probe.string()};
  const std::array<std::string, 1> clean{good.string()};
  const std::array<std::string, 1> also_broken{second.string()};

  std::string error;
  const std::vector<Symbol> none =
      CollectSymbols(std::span<const std::string>{broken}, SymbolKind::kDefinitions, error);
  EXPECT_TRUE(none.empty());
  EXPECT_EQ(error, probe_error);

  // The scan that follows has nothing to report -- and reporting nothing is not
  // the same as contradicting the scan before it. This is exactly the shape the
  // interactive lookup uses: hot files, then the whole project, one string. The
  // wipe at the head of the scan used to drop the first scan's complaint here,
  // so a grammar that failed to load left an empty picker and no explanation.
  const std::vector<Symbol> found =
      CollectSymbols(std::span<const std::string>{clean}, SymbolKind::kDefinitions, error);
  EXPECT_TRUE(!found.empty());
  EXPECT_EQ(error, probe_error);

  // Including the emptiest scan there is: no paths, nothing read, nothing said.
  std::ignore = CollectSymbols(std::span<const std::string>{}, SymbolKind::kDefinitions, error);
  EXPECT_EQ(error, probe_error);

  // And a scan that does have something to say still does not displace what is
  // already there -- first complaint wins across scans as well as within one.
  const std::vector<Symbol> nothing =
      CollectSymbols(std::span<const std::string>{also_broken}, SymbolKind::kDefinitions, error);
  EXPECT_TRUE(nothing.empty());
  EXPECT_EQ(error, probe_error);

  // A caller that wants a verdict on one scan clears the string first, and gets
  // one. WriteSymbols is such a caller: it clears at its head, so what it hands
  // back is about the run it just did and nothing else.
  error.clear();
  std::ignore = CollectSymbols(std::span<const std::string>{clean}, SymbolKind::kDefinitions, error);
  EXPECT_TRUE(error.empty());

  std::string stale = "left over from something else entirely";
  SymbolModeOptions options;
  options.kind = SymbolKind::kDefinitions;
  const std::vector<std::string> paths{good.string()};
  const std::vector<std::string> rows = SplitLines(SymbolModeOutput(paths, options, stale));
  EXPECT_TRUE(!rows.empty());
  EXPECT_TRUE(stale.empty());
}

void SymbolLookupSaysWhyThePickerIsEmpty() {
  TEST_CASE("go-to-definition: the scan's diagnostic reaches the status line");

  const Scratch scratch{"koi-lookup-diagnostic"};
  const std::filesystem::path caller = scratch.Write("caller.cpp", "void Use() { Widget w; }\n");
  const std::filesystem::path defs = scratch.Write("defs.cpp", "struct Widget { int size; };\n");
  const std::filesystem::path probe = scratch.dir / "probe.cpp";
  std::filesystem::create_directory(probe);
  const std::string probe_error = "cannot read " + probe.string() + ": " +
                                  std::make_error_code(std::errc::is_a_directory).message();

  // Exactly one definition, so the lookup opens it instead of handing a picker
  // a screen this suite does not have. If the query ever captures more, this
  // fails here rather than by launching tooey.
  {
    const std::array<std::string, 1> only{defs.string()};
    std::string count_error;
    EXPECT_EQ(CollectSymbols(std::span<const std::string>{only}, SymbolKind::kDefinitions,
                             count_error, "Widget")
                  .size(),
              std::size_t{1});
    EXPECT_TRUE(count_error.empty());
  }

  const auto fresh = [&caller](const std::vector<std::filesystem::path>& list) {
    Editor ed;
    ed.theme = BuiltinTheme();
    std::string filter = "printf '%s\\n'";
    for (const std::filesystem::path& one : list) filter += " " + one.string();
    ed.settings.file_filter = filter;
    // A lookup refuses outright without a terminal to hand a picker.
    ed.suspend_terminal = [] {};
    EXPECT_TRUE(OpenTarget(ed, caller.string()));
    ed.doc.selections.Set(Selection{13, 19});
    EXPECT_EQ(ReadDocRange(ed.doc.table, ed.doc.selections.Primary().Range()),
              std::string{"Widget"});
    return ed;
  };

  // Nothing found *and* something to say: silence here reads as proof there is
  // no definition, when what happened is that the scan could not read a path.
  {
    Editor ed = fresh({probe});
    GotoDefinition(ed);
    EXPECT_TRUE(ed.status.find("no definition for Widget") != std::string::npos);
    EXPECT_TRUE(ed.status.find(probe_error) != std::string::npos);
  }

  // Found something *and* something to say: the result set is partial, and a
  // partial one that says nothing reads as complete. Said the way the job path
  // says it, appended to whatever opening the definition put on the line.
  {
    Editor ed = fresh({probe, defs});
    GotoDefinition(ed);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"defs.cpp"});
    EXPECT_TRUE(ed.status.find(probe_error) != std::string::npos);
  }

  // The two-scan shape the wipe lived in, and the one the editor actually
  // takes: the hot-file scan is the one that reports, the project scan that
  // follows has nothing to add, and the complaint has to survive the gap
  // between them to be said at all.
  {
    Editor ed = fresh({defs});
    std::string db_error;
    ed.project = ProjectStore::Open(scratch.dir / "hot.db", db_error);
    EXPECT_TRUE(ed.project != nullptr);
    if (ed.project != nullptr) ed.project->RecordSymbolVisit("Anything", probe.string(), 1);
    GotoDefinition(ed);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"defs.cpp"});
    // Loosely spelled: the hot list comes back the project database's way, and
    // what the scan could not read is what it names.
    EXPECT_TRUE(ed.status.find("cannot read") != std::string::npos);
    EXPECT_TRUE(ed.status.find("probe.cpp") != std::string::npos);
  }

  // Nothing to say, nothing said: the parenthetical is the diagnostic's, and a
  // clean scan does not grow one.
  {
    Editor ed = fresh({defs});
    GotoDefinition(ed);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"defs.cpp"});
    EXPECT_TRUE(ed.status.find("cannot read") == std::string::npos);
  }

  // Nothing found, nothing to say: still the plain refusal, with no empty
  // parentheses hung off it.
  {
    Editor ed = fresh({caller});
    GotoDefinition(ed);
    EXPECT_TRUE(ed.status.find("no definition for Widget") != std::string::npos);
    EXPECT_TRUE(ed.status.find("(") == std::string::npos);
  }
}

void HotFirstOrdering() {
  TEST_CASE("symbols: the ranked head is the ranked list");

  const Scratch scratch{"koi-hotfirst-test"};
  const std::string a =
      scratch.Write("a.cpp", "int Alpha() { return 0; }\nint Aleph() { return 1; }\n").string();
  const std::string b =
      scratch.Write("b.cpp", "int Beta() { return 0; }\nint Bravo() { return 1; }\n").string();
  const std::string c =
      scratch.Write("c.cpp", "int Gamma() { return 0; }\nint Delta() { return 1; }\n").string();
  const std::string d =
      scratch.Write("d.cpp", "int Echo() { return 0; }\nint Foxtrot() { return 1; }\n").string();
  const std::vector<std::string> paths{a, b, c, d};

  SetProjectDbPath(scratch.dir / "state.db");

  {
    std::string error;
    const auto store = ProjectStore::Open(ProjectDbPath(), error);
    EXPECT_TRUE(store != nullptr);
    store->RecordSymbolVisit("Beta", b, 1);
    store->RecordSymbolVisit("Beta", b, 1);
    store->RecordCoVisit(a, c);
  }

  SymbolModeOptions options;
  options.kind = SymbolKind::kDefinitions;
  options.order = SymbolOrder::kHotFirst;
  options.from = a;

  std::string error;
  const std::vector<std::string> got = SplitLines(SymbolModeOutput(paths, options, error));
  EXPECT_TRUE(error.empty());

  std::vector<Symbol> all = CollectSymbols(paths, SymbolKind::kDefinitions, error);
  {
    std::string db_error;
    const auto store = ProjectStore::Open(ProjectDbPath(), db_error);
    EXPECT_TRUE(store != nullptr);
    std::ignore = store->RankSymbols(all, a);
  }
  std::vector<std::string> want;
  want.reserve(all.size());
  for (const Symbol& one : all) want.push_back(FormatSymbolRow(one));

  EXPECT_EQ(got.size(), want.size());
  EXPECT_TRUE(got == want);
  EXPECT_TRUE(!got.empty());

  if (!got.empty()) EXPECT_TRUE(got.front().find("a.cpp") != std::string::npos);
  const auto position = [&got](std::string_view name) {
    for (size_t i = 0; i < got.size(); ++i) {
      if (got[i].ends_with(name)) return static_cast<Index>(i);
    }
    return Index{-1};
  };
  EXPECT_TRUE(position("Beta") < position("Gamma"));
  EXPECT_TRUE(position("Gamma") < position("Bravo"));
  EXPECT_TRUE(position("Bravo") < position("Echo"));

  {
    SymbolModeOptions plain = options;
    plain.order = SymbolOrder::kFileOrder;
    std::string plain_error;
    std::vector<std::string> streamed = SplitLines(SymbolModeOutput(paths, plain, plain_error));
    std::vector<std::string> ranked = got;
    std::ranges::sort(streamed);
    std::ranges::sort(ranked);
    EXPECT_TRUE(streamed == ranked);
  }

  {
    std::vector<std::string> twice = paths;
    twice.insert(twice.end(), paths.begin(), paths.end());
    std::string twice_error;
    const std::vector<std::string> rows =
        SplitLines(SymbolModeOutput(twice, options, twice_error));
    EXPECT_EQ(rows.size(), want.size() * 2);
  }

  {
    SymbolModeOptions tight = options;
    tight.hot_limit = 0;
    std::string tight_error;
    std::vector<std::string> rows = SplitLines(SymbolModeOutput(paths, tight, tight_error));
    EXPECT_EQ(rows.size(), want.size());
    EXPECT_TRUE(rows.front().find("a.cpp") != std::string::npos);
    std::ranges::sort(rows);
    std::vector<std::string> sorted_want = want;
    std::ranges::sort(sorted_want);
    EXPECT_TRUE(rows == sorted_want);
  }

  SetProjectDbPath({});
}

void WordMatching() {
  TEST_CASE("symbols: whole-word filtering");

  EXPECT_TRUE(ContainsWord("Open", "Open"));
  EXPECT_TRUE(ContainsWord("w.Open(x)", "Open"));
  EXPECT_TRUE(ContainsWord("a->Open", "Open"));
  EXPECT_TRUE(!ContainsWord("OpenFile", "Open"));
  EXPECT_TRUE(!ContainsWord("ReOpen", "Open"));
  EXPECT_TRUE(!ContainsWord("Open", "OpenFile"));
  EXPECT_TRUE(!ContainsWord("anything", ""));
  EXPECT_TRUE(ContainsWord("operator+(a)", "operator+"));
}

void SymbolNamesAreCappedNotQuadratic() {
  TEST_CASE("symbols: nested calls cannot inflate names into the file itself");

  const Scratch scratch{"koi-symbol-cap"};

  // Reference queries capture whole call expressions by design, and nested
  // calls nest their captures: f1(f2(...)) used to yield names of N, N-1, ...
  // 1 tokens -- O(N^2) bytes, 1.9 GB of RSS from an 85 KB file.
  std::string nested;
  constexpr int kDepth = 300;
  for (int i = 0; i < kDepth; ++i) nested += "f" + std::to_string(i) + "(";
  nested += "1";
  nested.append(static_cast<size_t>(kDepth), ')');
  const std::filesystem::path deep =
      scratch.Write("deep.cpp", "void driver() { " + nested + "; }\n");

  std::string error;
  const std::vector<Symbol> refs = ScanOne(deep, SymbolKind::kReferences, error);
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(refs.size() >= static_cast<size_t>(kDepth));
  size_t total = 0;
  size_t longest = 0;
  for (const Symbol& one : refs) {
    total += one.name.size();
    longest = std::max(longest, one.name.size());
  }
  // 200-byte cap plus a 3-byte ellipsis. Unbounded, `longest` alone is the
  // whole expression (~2 KB here) and `total` is quadratic (~300 KB).
  EXPECT_TRUE(longest <= 203);
  EXPECT_TRUE(total <= refs.size() * 203);

  // The clip lands on a code-point start: a name whose 200th byte falls inside
  // a multi-byte character must come back valid UTF-8, ellipsis and all.
  std::string wide = "void driver2() { fn(";
  wide += "\"";
  for (int i = 0; i < 120; ++i) wide += "\xC3\xA9";  // 240 bytes of e-acute
  wide += "\"); }\n";
  const std::vector<Symbol> wides =
      ScanOne(scratch.Write("wide.cpp", wide), SymbolKind::kReferences, error);
  bool clipped_one = false;
  for (const Symbol& one : wides) {
    EXPECT_TRUE(IsWellFormedUtf8(one.name));
    if (one.name.ends_with("\xE2\x80\xA6")) clipped_one = true;
  }
  EXPECT_TRUE(clipped_one);

  // Ordinary symbols are untouched.
  const std::vector<Symbol> defs = ScanOne(
      scratch.Write("plain.cpp", "int Widget() { return 1; }\n"), SymbolKind::kDefinitions, error);
  bool found_widget = false;
  for (const Symbol& one : defs) {
    if (one.name.find("Widget") != std::string::npos) found_widget = true;
    EXPECT_TRUE(one.name.find("\xE2\x80\xA6") == std::string::npos);
  }
  EXPECT_TRUE(found_widget);
}

void QueryMatchesAreBoundedLikeQueryTime() {
  TEST_CASE("symbol scan: a cursor that runs out of match slots says so");

  // The scan budgeted its parse and left its query unbounded in two ways at
  // once: no deadline, and no cap on how many matches tree-sitter may carry at
  // a time. It keeps a capture list per match still in progress, so a file and
  // a query that between them start matches faster than they finish them make
  // the library allocate without limit -- driven by file contents, inside a
  // scan that had a ten-second budget written on it.
  //
  // The shape that does it: patterns of the form "an identifier, then later a
  // number", over a call whose last argument is the only number in it. Every
  // identifier starts a match that cannot finish until the argument list ends,
  // so the count in flight is the number of identifiers times the number of
  // patterns. Nothing koi ships looks remotely like this -- measured, every
  // source in this repository and six thousand third-party ones peak in the low
  // tens -- which is why the query has to come from a runtime root of the
  // case's own.
  const FakeQueryDir queries{"cpp"};
  EXPECT_TRUE(queries.Ready());
  if (!queries.Ready()) return;

  constexpr int kOver = 128;   // 128 patterns x 24 identifiers: past the cap.
  constexpr int kUnder = 64;   // The same file, half the patterns: inside it.
  constexpr int kArgs = 24;
  const auto repeated = [](int patterns) {
    std::string out;
    for (int p = 0; p < patterns; ++p) {
      const std::string n = std::to_string(p);
      out += "(argument_list (identifier) @a" + n + " (number_literal) @z" + n + ")\n";
    }
    return out;
  };
  queries.Write("definitions.scm", repeated(kOver));
  queries.Write("references.scm", repeated(kUnder));

  const Scratch scratch{"koi-query-match-limit"};
  std::string call = "void driver() { g(";
  for (int i = 0; i < kArgs; ++i) call += "x" + std::to_string(i) + ", ";
  call += "7); }\n";
  const std::filesystem::path wide = scratch.Write("wide.cpp", call);
  const std::filesystem::path small = scratch.Write("small.cpp", "void other() { h(y0, 3); }\n");
  const std::array<std::string, 1> paths{wide.string()};
  const std::span<const std::string> one{paths};
  const std::array<std::string, 2> both{small.string(), wide.string()};

  OnAThreadOfItsOwn([&one, &both] {
    // Under the cap: every match arrives, two captures each, and the scan has
    // nothing to complain about. This is the control that makes the case below
    // about the cap and not about the fixture.
    std::string quiet_error;
    const std::vector<Symbol> quiet = CollectSymbols(one, SymbolKind::kReferences, quiet_error);
    EXPECT_EQ(quiet_error, std::string{});
    EXPECT_EQ(quiet.size(), static_cast<std::size_t>(2 * kUnder * kArgs));

    // Past it: tree-sitter recycles the capture list of the match that began
    // earliest rather than allocating another, so the scan stays bounded -- and
    // says that what it hands back is not all of it. Unbounded, this is the
    // same 6144 symbols the arithmetic above predicts, with no complaint and no
    // ceiling on the allocation behind them.
    std::string loud_error;
    const std::vector<Symbol> loud = CollectSymbols(one, SymbolKind::kDefinitions, loud_error);
    EXPECT_EQ(loud_error, std::string{"wide.cpp: too many query matches -- some symbols are "
                                      "missing"});
    EXPECT_TRUE(!loud.empty());
    EXPECT_TRUE(loud.size() < static_cast<std::size_t>(2 * kOver * kArgs));

    // The complaint names the file that earned it, not the scan. One scan over
    // an ordinary file and the saturated one: the ordinary file still gives up
    // every symbol it has, and what is reported is the other one's name.
    std::string mixed_error;
    const std::vector<Symbol> mixed = CollectSymbols(both, SymbolKind::kDefinitions, mixed_error);
    EXPECT_EQ(mixed_error, std::string{"wide.cpp: too many query matches -- some symbols are "
                                       "missing"});
    EXPECT_EQ(mixed.size(), static_cast<std::size_t>(2 * kOver) + loud.size());

    // And a scan that never saturates says nothing, on the same cursor and the
    // same file: the flag is the cursor's, cleared by every exec, so it is a
    // fact about one file's query and not about the scan that ran it.
    std::string second_error;
    std::ignore = CollectSymbols(one, SymbolKind::kReferences, second_error);
    EXPECT_EQ(second_error, std::string{});
  });
}

void AQueryThatWillNotCompileFailsOncePerRunNotOncePerFile() {
  TEST_CASE("symbol scan: a broken query is compiled once, and remembered");

  // CompileQuery cached what compiled and forgot what did not, so a malformed
  // query in a user's runtime root cost every scanned file a fresh attempt: up
  // to six fs::exists to find the .scm, a whole-file read of it, an inherits
  // splice and a ts_query_new -- per source, on every scan, for an answer that
  // cannot change while koi is running.
  const FakeQueryDir queries{"nix"};
  EXPECT_TRUE(queries.Ready());
  if (!queries.Ready()) return;
  // Nix ships no definitions query, so this file is the only one there is for
  // it: nothing here shadows a query koi ships, and nothing koi ships can stand
  // in for it once it is taken away.
  queries.Write("definitions.scm", "(binding (no_such_node_type) @definition)\n");

  const Scratch scratch{"koi-broken-query"};
  const std::string source = "{ alpha = 1; beta = 2; }\n";
  const std::array<std::string, 1> first{scratch.Write("a.nix", source).string()};
  const std::array<std::string, 1> second{scratch.Write("b.nix", source).string()};
  const std::array<std::string, 1> third{scratch.Write("c.nix", source).string()};
  const std::array<std::string, 1> other{
      scratch.Write("plain.cpp", "int Widget() { return 1; }\n").string()};

  OnAThreadOfItsOwn([&] {
    std::string first_error;
    const std::vector<Symbol> none =
        CollectSymbols(std::span<const std::string>{first}, SymbolKind::kDefinitions, first_error);
    EXPECT_TRUE(none.empty());
    // The message is the first thing the cache must not swallow: a caller that
    // clears its error and asks again -- the highlighter does, per keystroke --
    // has to be told why, every time, and not just the first.
    EXPECT_TRUE(first_error.starts_with("nix definitions.scm: "));
    EXPECT_TRUE(first_error.find("no_such_node_type") != std::string::npos);

    std::string second_error;
    std::ignore =
        CollectSymbols(std::span<const std::string>{second}, SymbolKind::kDefinitions, second_error);
    EXPECT_EQ(second_error, first_error);

    // The observable. With the query file gone, a scan that still went to the
    // filesystem could not produce this message: it would find no query for nix
    // at all and say nothing, or find no source and say "no queries/nix/... on
    // the runtime path". Saying exactly what it said the first time is proof
    // that neither the existence probe nor the read happened again.
    queries.Forget();
    EXPECT_TRUE(!std::filesystem::exists(queries.dir));
    std::string third_error;
    const std::vector<Symbol> still =
        CollectSymbols(std::span<const std::string>{third}, SymbolKind::kDefinitions, third_error);
    EXPECT_TRUE(still.empty());
    EXPECT_EQ(third_error, first_error);

    // Remembered per language and per query file, not globally: a language
    // whose query is fine is scanned as it always was.
    std::string cpp_error;
    const std::vector<Symbol> defs =
        CollectSymbols(std::span<const std::string>{other}, SymbolKind::kDefinitions, cpp_error);
    EXPECT_EQ(cpp_error, std::string{});
    EXPECT_TRUE(!defs.empty());
  });
}

void AProjectScaleScanKeepsItsOrderAndItsFrames() {
  TEST_CASE("symbol scan: thousands of files, one frame's worth of stack");

  // ScanSymbols handed each file's generator over with `elements_of`, which is
  // the better shape and one resume cheaper per symbol -- when the compiler
  // makes the symmetric-transfer tail call. AddressSanitizer's frame-poisoning
  // epilogue runs after the transfer and keeps the caller's frame alive, so the
  // handoff leaked a frame pair per *file* and a project-scale scan overflowed
  // the stack a few thousand files in. Release builds never did, so nothing
  // shipped was wrong -- but the sanitizer build could not run the one workload
  // most worth running under it, which is where the shape had to give.
  //
  // What is asserted here is the equivalence the restructuring had to preserve,
  // at a scale the old shape died at under asan: order, contents, the errors,
  // and the cancellation checks around them. The stack depth itself is measured
  // outside the suite, in a sanitizer build, where it is observable.
  const Scratch scratch{"koi-scan-scale"};
  const std::filesystem::path a =
      scratch.Write("a.cpp", "int Alpha(int x) { return x; }\nint Bravo(int x) { return x; }\n");
  const std::filesystem::path b = scratch.Write("b.cpp", "struct Charlie { int size; };\n");
  const std::filesystem::path absent = scratch.dir / "gone.cpp";
  const std::filesystem::path plain = scratch.Write("notes.txt", "no language, no query\n");
  const std::filesystem::path probe = scratch.dir / "probe.cpp";
  std::filesystem::create_directory(probe);
  const std::string probe_error = "cannot read " + probe.string() + ": " +
                                  std::make_error_code(std::errc::is_a_directory).message();

  // Four kinds of path, interleaved so that no two files of the same kind are
  // adjacent: one that yields two symbols, one that yields one, one that is not
  // there at all, and one with no language. The absent and languageless ones
  // are what a real project walk is mostly made of, and they are the ones that
  // yield nothing and so never suspend the generator.
  constexpr int kRounds = 1000;
  std::vector<std::string> paths;
  paths.reserve(static_cast<std::size_t>(kRounds) * 4);
  for (int i = 0; i < kRounds; ++i) {
    paths.push_back(a.string());
    paths.push_back(absent.string());
    paths.push_back(b.string());
    paths.push_back(plain.string());
  }

  std::string error;
  const std::vector<Symbol> all = CollectSymbols(paths, SymbolKind::kDefinitions, error);
  EXPECT_EQ(error, std::string{});
  EXPECT_EQ(all.size(), static_cast<std::size_t>(kRounds) * 3);

  // In file order, and in capture order within a file: the sequence is exactly
  // the per-file scans concatenated, which is what handing every symbol up
  // through the outer frame has to leave unchanged.
  std::string one_error;
  const std::vector<Symbol> from_a = ScanOne(a, SymbolKind::kDefinitions, one_error);
  const std::vector<Symbol> from_b = ScanOne(b, SymbolKind::kDefinitions, one_error);
  EXPECT_EQ(one_error, std::string{});
  EXPECT_EQ(from_a.size(), std::size_t{2});
  EXPECT_EQ(from_b.size(), std::size_t{1});
  bool same = (all.size() == static_cast<std::size_t>(kRounds) * 3);
  for (int i = 0; same && (i < kRounds); ++i) {
    const std::size_t at = static_cast<std::size_t>(i) * 3;
    same = (FormatSymbolRow(all[at]) == FormatSymbolRow(from_a[0])) &&
           (FormatSymbolRow(all[at + 1]) == FormatSymbolRow(from_a[1])) &&
           (FormatSymbolRow(all[at + 2]) == FormatSymbolRow(from_b[0]));
  }
  EXPECT_TRUE(same);

  // The error still accumulates from wherever in the list it happens, and still
  // first-complaint-wins: one unreadable path a thousand files deep is reported
  // once and does not stop the walk.
  std::vector<std::string> with_probe = paths;
  with_probe.insert(with_probe.begin() + (with_probe.size() / 2), probe.string());
  std::string deep_error;
  const std::vector<Symbol> around = CollectSymbols(with_probe, SymbolKind::kDefinitions,
                                                    deep_error);
  EXPECT_EQ(deep_error, probe_error);
  EXPECT_EQ(around.size(), all.size());

  // And cancellation still stops the walk rather than the yield: cancelled deep
  // into the list, the scan ends at the next path and never reaches the
  // unreadable one at the end.
  std::atomic<bool> cancel{false};
  std::string stopped_error;
  std::size_t seen = 0;
  for (Symbol&& one : ScanSymbols(std::span<const std::string>{with_probe},
                                  SymbolKind::kDefinitions, stopped_error, {}, &cancel)) {
    std::ignore = one;
    if (++seen == 30) cancel.store(true);
  }
  EXPECT_EQ(seen, std::size_t{30});
  EXPECT_EQ(stopped_error, std::string{});
}

}  // namespace koi
