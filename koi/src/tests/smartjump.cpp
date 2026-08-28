// Tests for smartjump.cpp and the prompt half of it in navigate.cpp: the
// clause parser, the corpus snapshot, the ranking pipeline, the landing rules,
// the step-through and the bounce timer.
//
// The corpus every pipeline case runs against is the worked example in
// docs/smart-jump.md -- keymap.cpp at weight 343, keylog.cpp at 31,
// piece_tree.cpp at 140 and tooey.cpp at 1053 three weeks old -- so a failure
// here says which of the doc's claims stopped being true.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

double Now() {
  const auto since = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration<double>(since).count();
}

constexpr double kHour = 3600.0;
constexpr double kDay = 24 * kHour;

// Seeding goes through the connection rather than through RecordVisit: what
// these cases are about is a corpus of a particular shape and age, and the
// recording API can produce neither a visit count nor a timestamp of its own.
void SeedFile(sqlite3* db, std::string_view key, int visits, int edits, double last_ts) {
  Stmt stmt{db, "INSERT OR REPLACE INTO files(path,visits,edits,last_ts,last_line,last_col,branch)"
                " VALUES(?,?,?,?,1,0,NULL);"};
  stmt.Text(1, key);
  stmt.Int(2, visits);
  stmt.Int(3, edits);
  stmt.Real(4, last_ts);
  EXPECT_TRUE(stmt.Run());
}

void SeedSymbol(sqlite3* db, std::string_view file, std::string_view symbol, Index line,
                int visits, double last_ts) {
  Stmt stmt{db, "INSERT OR REPLACE INTO symbols(file,symbol,visits,last_ts,line)"
                " VALUES(?,?,?,?,?);"};
  stmt.Text(1, file);
  stmt.Text(2, symbol);
  stmt.Int(3, visits);
  stmt.Real(4, last_ts);
  stmt.Int(5, line);
  EXPECT_TRUE(stmt.Run());
}

void SeedLocation(sqlite3* db, std::string_view path, Index line, std::string_view content,
                  int visits, int kind, double last_ts, int misses = 0) {
  Stmt stmt{db, "INSERT INTO locations(path,line,col,symbol,content,context,blob,uniq,kind,"
                "visits,misses,last_ts,branch,seq)"
                " VALUES(?,?,1,NULL,?,NULL,NULL,1,?,?,?,?,NULL,"
                " (SELECT COALESCE(MAX(seq),0)+1 FROM locations));"};
  stmt.Text(1, path);
  stmt.Int(2, line);
  stmt.Text(3, content);
  stmt.Int(4, kind);
  stmt.Int(5, visits);
  stmt.Int(6, misses);
  stmt.Real(7, last_ts);
  EXPECT_TRUE(stmt.Run());
}

std::int64_t Scalar(sqlite3* db, const char* sql, std::string_view arg = {}) {
  Stmt stmt{db, sql};
  if (!arg.empty()) stmt.Text(1, arg);
  return stmt.Step() ? stmt.Integer(0) : -1;
}

// The doc's corpus, on disk and in a store, with an editor pointed at both.
struct Fixture {
  Scratch scratch;
  AsProjectRoot root;
  Editor ed;
  sqlite3* db{nullptr};

  explicit Fixture(std::string_view name) : scratch{name}, root{scratch.dir} {
    std::error_code ec;
    fs::create_directories(scratch.dir / "koi" / "src", ec);
    Write("koi/src/keymap.cpp", "// keymap\n");
    Write("koi/src/keylog.cpp", "// keylog\n");
    Write("koi/src/piece_tree.cpp", NumberedLines(300));
    Write("koi/src/tooey.cpp", "// tooey\n");
    Write("koi/src/navigate.cpp", NumberedLines(3400));

    ed.theme = BuiltinTheme();
    std::string error;
    ed.project = ProjectStore::Open(scratch.dir / "state.db", error);
    EXPECT_TRUE(ed.project != nullptr);
    if (ed.project == nullptr) return;
    db = ed.project->Connection();

    const double now = Now();
    // Weight is visits + 3*edits, and the multiplier is what the row's age is
    // worth: under a day is x2, three weeks is x0.5.
    SeedFile(db, "koi/src/keymap.cpp", 343, 0, now - (2 * kHour));
    SeedFile(db, "koi/src/keylog.cpp", 31, 0, now - (2 * kHour));
    SeedFile(db, "koi/src/piece_tree.cpp", 140, 0, now - (2 * kHour));
    SeedFile(db, "koi/src/tooey.cpp", 1053, 0, now - (21 * kDay));
    SeedFile(db, "koi/src/navigate.cpp", 90, 0, now - (2 * kHour));

    SeedSymbol(db, "koi/src/navigate.cpp", "SetPinHere", 3300, 5, now - kHour);
    SeedSymbol(db, "koi/src/navigate.cpp", "JumpToPin", 3320, 2, now - kHour);
    SeedSymbol(db, "koi/src/keymap.cpp", "Lookup", 50, 1, now - kHour);

    SeedLocation(db, "koi/src/piece_tree.cpp", 212, "SplitNode(&node);", 4, 0, now - kHour);
    SeedLocation(db, "koi/src/keymap.cpp", 640, "return Lookup(keys);", 2, 0, now - kHour);
    SeedLocation(db, "koi/src/keymap.cpp", 100, "void SplitBinding();", 1, 0, now - kHour);
  }

  fs::path Write(std::string_view name, std::string_view text) const {
    const fs::path path = scratch.dir / name;
    WriteFixtureFile(path, text);
    return path;
  }

  SmartCorpus Snapshot() {
    SmartCorpus corpus;
    BuildSmartCorpus(*ed.project, corpus);
    return corpus;
  }

  // `heal` is what the same-spot dedup compares a location on. Empty is the
  // shape every case but the dedup's own wants: no buffer, so stored lines.
  std::vector<SmartMatch> Rank(const SmartCorpus& corpus, std::string_view text,
                               const SmartHeal& heal = {}) {
    return RankSmartMatches(corpus, ParseSmartQuery(text), ed.project.get(), heal);
  }

  SmartPreview Preview(const SmartCorpus& corpus, std::string_view text) {
    return PreviewSmartMatches(corpus, ParseSmartQuery(text), ed.project.get(), kPickerRows);
  }
};

std::string Keys(const std::vector<SmartMatch>& matches) {
  std::string out;
  for (const SmartMatch& one : matches) {
    if (!out.empty()) out += ' ';
    out += one.key;
  }
  return out;
}

std::string ViewText(const Editor& ed) {
  return ReadDocRange(ed.doc.table, Interval(0, DocLength(ed.doc.table)));
}

// Where `needle` sits in the view text, or npos. Ordering assertions compare
// two of these, which is what "in ranked order" means in a document.
std::size_t At(const std::string& text, std::string_view needle) { return text.find(needle); }

// One key through the real key path. Presser cannot be used here: it clears the
// chord prefix before every key, and these sequences carry one across.
struct PressKey {
  KeyMaps maps{DefaultKeyMaps()};
  std::vector<Key> pending;
  void operator()(Editor& ed, std::string_view key) { HandleKeyInput(ed, maps, K(key), pending); }
};

}  // namespace

void SmartJumpParsing() {
  TEST_CASE("smart jump: a bare letter is a clause, and it claims what follows");

  {
    // No keyword at all: everything is a file term, which is the common case
    // and the reason the file clause is the bare one.
    const SmartQuery q = ParseSmartQuery("key cpp");
    EXPECT_EQ(q.file_terms.size(), std::size_t{2});
    if (q.file_terms.size() < 2) return;
    EXPECT_EQ(q.file_terms[0], std::string{"key"});
    EXPECT_EQ(q.file_terms[1], std::string{"cpp"});
    EXPECT_TRUE(q.symbol_terms.empty());
    EXPECT_TRUE(q.content_terms.empty());
    EXPECT_FALSE(q.has_line);
    EXPECT_TRUE(q.error.empty());
    // What `queries` is keyed on: the words as typed, one space between them.
    EXPECT_EQ(q.typed, std::string{"key cpp"});
  }

  {
    const SmartQuery q = ParseSmartQuery("key cpp c split node");
    EXPECT_EQ(q.file_terms.size(), std::size_t{2});
    // The keyword claims every term after it, not only the next one.
    EXPECT_EQ(q.content_terms.size(), std::size_t{2});
    if (q.content_terms.size() < 2) return;
    EXPECT_EQ(q.content_terms[1], std::string{"node"});
  }

  {
    // `f` reopens the file clause once another one has been opened, which is
    // the only thing it is for.
    const SmartQuery q = ParseSmartQuery("c split f key cpp");
    EXPECT_EQ(q.content_terms.size(), std::size_t{1});
    EXPECT_EQ(q.file_terms.size(), std::size_t{2});
    if (q.file_terms.size() < 2) return;
    EXPECT_EQ(q.file_terms[1], std::string{"cpp"});
  }

  {
    // Different kinds of target do not combine; `f` with either is fine.
    EXPECT_TRUE(!ParseSmartQuery("d pin c split").error.empty());
    EXPECT_TRUE(!ParseSmartQuery("c split d pin").error.empty());
    EXPECT_TRUE(!ParseSmartQuery("d pin 42").error.empty());
    EXPECT_TRUE(ParseSmartQuery("key cpp c split").error.empty());
    EXPECT_TRUE(ParseSmartQuery("key cpp d pin").error.empty());
  }
  {
    // Each conflict names what was typed. A query with no `c` anywhere in it
    // cannot be told that `c` is the trouble: it sends you looking for a clause
    // you never wrote.
    const SmartQuery line = ParseSmartQuery("d pin 640");
    EXPECT_TRUE(line.error.find("a line number and `d`") != std::string::npos);
    EXPECT_TRUE(line.error.find("`c`") == std::string::npos);

    const SmartQuery both = ParseSmartQuery("d pin c split");
    EXPECT_TRUE(both.error.starts_with("`c` and `d` cannot be combined"));

    // The `d` counts even with nothing under it yet: digits are taken as a line
    // before any clause records a term, so a parser that only looked at the
    // terms let `d 640` quietly become a plain line query.
    const SmartQuery bare = ParseSmartQuery("d 640");
    EXPECT_TRUE(bare.error.find("a line number and `d`") != std::string::npos);
    // Which shows from the first digit typed, the way every other error here
    // shows as you type.
    EXPECT_TRUE(!ParseSmartQuery("d 6").error.empty());
    // And neither half alone is a conflict.
    EXPECT_TRUE(ParseSmartQuery("d pin").error.empty());
    EXPECT_TRUE(ParseSmartQuery("key 640").error.empty());
  }
  {
    const SmartQuery q = ParseSmartQuery("nav d pin");
    EXPECT_EQ(q.file_terms.size(), std::size_t{1});
    EXPECT_EQ(q.symbol_terms.size(), std::size_t{1});
    if (q.symbol_terms.empty()) return;
    EXPECT_EQ(q.symbol_terms[0], std::string{"pin"});
  }

  TEST_CASE("smart jump: digits are a line, and an unknown letter is an error");
  {
    const SmartQuery q = ParseSmartQuery("key 640");
    EXPECT_TRUE(q.has_line);
    EXPECT_EQ(q.line, Index{640});
    EXPECT_EQ(q.file_terms.size(), std::size_t{1});
  }
  {
    // A lone letter is always a keyword, so an unknown one is a mistake rather
    // than a term -- and the message says which letters are not.
    const SmartQuery q = ParseSmartQuery("key z");
    EXPECT_TRUE(!q.error.empty());
    EXPECT_TRUE(q.error.find("c content") != std::string::npos);
    EXPECT_TRUE(q.error.find("d definitions") != std::string::npos);
    EXPECT_TRUE(q.error.find("f file") != std::string::npos);
  }
  {
    // Quoted, the same letter is a term -- and then it is too short to be one.
    const SmartQuery q = ParseSmartQuery("'z'");
    EXPECT_TRUE(!q.error.empty());
    EXPECT_TRUE(q.error.find("two characters") != std::string::npos);
  }
  {
    // Quoting is also how a term with a space in it is spelled, and it survives
    // the classifier untouched.
    const SmartQuery q = ParseSmartQuery("'two words' key");
    EXPECT_EQ(q.file_terms.size(), std::size_t{2});
    if (q.file_terms.size() < 2) return;
    EXPECT_EQ(q.file_terms[0], std::string{"two words"});
  }
  {
    // And how a file called 640 is asked for.
    const SmartQuery q = ParseSmartQuery("\"640\"");
    EXPECT_FALSE(q.has_line);
    EXPECT_EQ(q.file_terms.size(), std::size_t{1});
  }
}

void SmartJumpPipeline() {
  TEST_CASE("smart jump: the worked example, band by band");

  Fixture fix{"koi-smartjump-pipeline"};
  if (fix.ed.project == nullptr) return;
  const SmartCorpus corpus = fix.Snapshot();
  EXPECT_EQ(corpus.files.size(), std::size_t{5});
  EXPECT_EQ(corpus.symbols.size(), std::size_t{3});
  EXPECT_EQ(corpus.locations.size(), std::size_t{3});

  {
    // The doc's first row, handed to the pipeline rather than typed: `k` is a
    // lone letter, and under v2's syntax a lone letter is a keyword, so the
    // query cannot be spelled. The claim it makes is about the bands, which is
    // the pipeline's business and not the parser's.
    SmartQuery one_letter;
    one_letter.file_terms.emplace_back("k");
    one_letter.typed = "k";
    // Seven, not four: every path here starts `koi/`, so navigate.cpp matches on
    // the path too, and the term is bare, so the one symbol and the one stored
    // line with a `k` in their own text answer as well. What the doc's claim is
    // about holds -- the two basename prefixes come first however heavy the
    // path-only rows are.
    const std::vector<SmartMatch> got =
        RankSmartMatches(corpus, one_letter, fix.ed.project.get());
    EXPECT_EQ(got.size(), std::size_t{7});
    if (got.size() < 5) return;
    EXPECT_EQ(got[0].key, std::string{"koi/src/keymap.cpp"});
    EXPECT_EQ(got[1].key, std::string{"koi/src/keylog.cpp"});
    // tooey is the heaviest row in the store -- weight 1053 -- and it is still
    // below both band-2 rows. Frecency cannot save a row across a band.
    EXPECT_TRUE(At(Keys(got), "tooey") > At(Keys(got), "keylog"));
    // Two full bands between them, which no pile of priors can close.
    EXPECT_TRUE((got[1].score - got.back().score) > 1.0);
  }

  {
    // Same band, same fuzzy: priors decide, and the only one in play here is
    // frecency. keymap at 343 over keylog at 31. Four rows, not three: `key` is
    // bare, and the stored line `return Lookup(keys);` has it in its own text,
    // so the line answers beside the files it lives among.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "key");
    EXPECT_EQ(got.size(), std::size_t{4});
    if (got.size() < 2) return;
    EXPECT_EQ(got[0].key, std::string{"koi/src/keymap.cpp"});
    EXPECT_EQ(got[1].key, std::string{"koi/src/keylog.cpp"});
    // 0.2 * 686/716 - 0.2 * 62/92 = 0.0568, and nothing else separates them.
    EXPECT_TRUE(std::abs((got[0].score - got[1].score) - 0.05684) < 1e-3);
  }

  {
    // Both terms AND, whatever facet each of them landed on. The doc says two;
    // this corpus gives four, because `cpp` matches tooey.cpp as well, `key`
    // already did, and the keymap line `return Lookup(keys);` answers `key` by
    // its text and `cpp` by the path it lives at.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "key cpp");
    EXPECT_EQ(got.size(), std::size_t{4});
    if (got.empty()) return;
    EXPECT_EQ(got[0].key, std::string{"koi/src/keymap.cpp"});
    // ... and one more term does narrow it, which is the property that matters:
    // more typing always narrows.
    EXPECT_EQ(fix.Rank(corpus, "key map cpp").size(), std::size_t{2});
  }

  TEST_CASE("smart jump: the most specific clause decides what you land on");
  {
    // A content term makes the target a line, not a file.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "c split");
    EXPECT_EQ(got.size(), std::size_t{2});
    if (got.empty()) return;
    for (const SmartMatch& one : got) EXPECT_TRUE(one.kind == SmartKind::kLocation);
    EXPECT_TRUE(got[0].row_id != 0);
    // SplitNode is a prefix of its line, SplitBinding is not the start of
    // "void SplitBinding();" -- band 2 against band 1.
    EXPECT_EQ(got[0].line, Index{212});

    // ... and the file clause narrows it to one.
    const std::vector<SmartMatch> narrowed = fix.Rank(corpus, "key cpp c split");
    EXPECT_EQ(narrowed.size(), std::size_t{1});
    if (narrowed.empty()) return;
    EXPECT_EQ(narrowed[0].line, Index{100});
    EXPECT_TRUE(narrowed[0].display.find("SplitBinding") != std::string::npos);

    // Clause order does not change the result set.
    const std::vector<SmartMatch> flipped = fix.Rank(corpus, "c split f key cpp");
    EXPECT_EQ(Keys(flipped), Keys(narrowed));
  }

  {
    // A definition clause makes the target a symbol.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "nav d pin");
    EXPECT_EQ(got.size(), std::size_t{2});
    for (const SmartMatch& one : got) EXPECT_TRUE(one.kind == SmartKind::kSymbol);
    EXPECT_TRUE(Keys(got).find("SetPinHere") != std::string::npos);
    EXPECT_TRUE(Keys(got).find("JumpToPin") != std::string::npos);
    // The file clause is a filter as well as a bonus: Lookup is in keymap.cpp
    // and never reaches the list.
    EXPECT_TRUE(Keys(got).find("Lookup") == std::string::npos);
  }

  {
    // A bare number is a line clause, and it is the nearest visited line in
    // each surviving file that wins -- not every row of it.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "key 640");
    EXPECT_EQ(got.size(), std::size_t{1});
    if (got.empty()) return;
    EXPECT_EQ(got[0].line, Index{640});
    EXPECT_TRUE(got[0].kind == SmartKind::kLocation);

    const std::vector<SmartMatch> near_top = fix.Rank(corpus, "key 90");
    EXPECT_EQ(near_top.size(), std::size_t{1});
    if (near_top.empty()) return;
    EXPECT_EQ(near_top[0].line, Index{100});
  }

  TEST_CASE("smart jump: a row nothing can find any more is not offered");
  {
    SeedLocation(fix.db, "koi/src/keymap.cpp", 900, "gone entirely;", 1, 0, Now(), 3);
    SeedLocation(fix.db, "koi/src/keymap.cpp", 901, "nearly gone;", 1, 0, Now(), 2);
    const SmartCorpus after = fix.Snapshot();
    EXPECT_EQ(after.locations.size(), std::size_t{4});
    EXPECT_TRUE(fix.Rank(after, "c gone entirely").empty());
    EXPECT_EQ(fix.Rank(after, "c nearly").size(), std::size_t{1});
  }

  TEST_CASE("smart jump: a row whose file is gone is dropped at snapshot time");
  {
    std::error_code ec;
    fs::remove(fix.scratch.dir / "koi/src/tooey.cpp", ec);
    const SmartCorpus after = fix.Snapshot();
    EXPECT_EQ(after.files.size(), std::size_t{4});
    // Nothing tooey answers any more. `toy` still reaches a stored line whose
    // text spells it out, which is the bare term doing its job and not the
    // deleted file coming back.
    EXPECT_TRUE(Keys(fix.Rank(after, "toy")).find("tooey") == std::string::npos);
  }
}

// A file row lands on the line the file was left on, so it can name the very
// place a stored line names. Two rows for one destination is a row of five spent
// on nothing: stepping onto the second draws the same excerpt as the first.
//
// File against location only, and the location on the line it would arrive at
// rather than the one it stores: the dedup takes a heal hook and every consumer
// heals before landing. A symbol is neither -- its stored line has no anchor and
// the landing re-finds it by name -- so it is never eaten and never in the way.
void SmartJumpOneSpotOneRow() {
  Fixture fix{"smart-one-spot"};
  if (fix.ed.project == nullptr) return;

  // How many rows of the ranking point into one file, which is the whole of what
  // every case here asks.
  const auto rows_in = [](const std::vector<SmartMatch>& out, std::string_view name) {
    std::size_t rows = 0;
    for (const SmartMatch& one : out) {
      if (one.path.find(name) != std::string::npos) ++rows;
    }
    return rows;
  };

  TEST_CASE("smart jump: a file row and a line row on one spot come back as one");
  {
    // A line stored where keylog.cpp was last left, and named by the same term
    // the path is: both kinds survive the evaluation, and both point at line 4.
    SeedLocation(fix.db, "koi/src/keylog.cpp", 4, "#include \"keylog.h\"", 3, 0, Now() - kHour);
    // First with the file still on the line SeedFile left it (1): two places,
    // so both rows come back. Without it the count below could as well be a
    // location the query never reached, which is not what this case is about.
    const SmartCorpus apart = fix.Snapshot();
    EXPECT_EQ(rows_in(fix.Rank(apart, "keylog"), "keylog.cpp"), std::size_t{2});
    {
      Stmt set{fix.db, "UPDATE files SET last_line=4 WHERE path='koi/src/keylog.cpp';"};
      EXPECT_TRUE(set.Run());
    }
    const SmartCorpus corpus = fix.Snapshot();
    EXPECT_EQ(rows_in(fix.Rank(corpus, "keylog"), "keylog.cpp"), std::size_t{1});
  }

  TEST_CASE("smart jump: a file left somewhere else is a second place and keeps its row");
  {
    {
      Stmt set{fix.db, "UPDATE files SET last_line=9 WHERE path='koi/src/keylog.cpp';"};
      EXPECT_TRUE(set.Run());
    }
    const SmartCorpus corpus = fix.Snapshot();
    EXPECT_EQ(rows_in(fix.Rank(corpus, "keylog"), "keylog.cpp"), std::size_t{2});
  }

  TEST_CASE("smart jump: a definition on the file's own line is not the file said twice");
  {
    // A symbol stored at exactly the line keylog.cpp was left on, named by the
    // same term the path is. Nothing about a symbol's stored line survives an
    // edit -- the landing goes back to the file and finds the name again -- so
    // the two are different claims however equal the numbers look, and the third
    // row is the location from the case above, now at a line of its own.
    SeedSymbol(fix.db, "koi/src/keylog.cpp", "keylog", 7, 4, Now() - kHour);
    {
      Stmt set{fix.db, "UPDATE files SET last_line=7 WHERE path='koi/src/keylog.cpp';"};
      EXPECT_TRUE(set.Run());
    }
    const SmartCorpus corpus = fix.Snapshot();
    const std::vector<SmartMatch> out = fix.Rank(corpus, "keylog");
    EXPECT_EQ(rows_in(out, "keylog.cpp"), std::size_t{3});
    std::size_t symbols = 0;
    for (const SmartMatch& one : out) {
      if (one.kind == SmartKind::kSymbol) ++symbols;
    }
    EXPECT_EQ(symbols, std::size_t{1});
  }

  TEST_CASE("smart jump: the dedup compares a location on the line it heals to");
  {
    // A file of its own, because the cases above have left keylog.cpp with rows
    // this one does not want to count. Stored line and last line agree, so
    // without a heal the two collapse the way case one does.
    fix.Write("koi/src/hooked.cpp", NumberedLines(40));
    SeedFile(fix.db, "koi/src/hooked.cpp", 20, 0, Now() - kHour);
    SeedLocation(fix.db, "koi/src/hooked.cpp", 12, "hooked the line;", 3, 0, Now() - kHour);
    {
      Stmt set{fix.db, "UPDATE files SET last_line=12 WHERE path='koi/src/hooked.cpp';"};
      EXPECT_TRUE(set.Run());
    }
    const std::int64_t id =
        Scalar(fix.db, "SELECT id FROM locations WHERE path=?;", "koi/src/hooked.cpp");
    EXPECT_TRUE(id > 0);
    // The hook contract, stood in for: given a row's id, overwrite the line and
    // say so. A real one asks the open buffer's anchor shadow.
    const auto heals_to = [id](Index to) {
      return SmartHeal{[id, to](std::int64_t ask, Index& line) {
        if (ask != id) return false;
        line = to;
        return true;
      }};
    };

    const SmartCorpus corpus = fix.Snapshot();
    EXPECT_EQ(rows_in(fix.Rank(corpus, "hooked"), "hooked.cpp"), std::size_t{1});
    // Drifted away from the file's line: two places again, and the stored line
    // says otherwise.
    EXPECT_EQ(rows_in(fix.Rank(corpus, "hooked", heals_to(30)), "hooked.cpp"), std::size_t{2});

    // The other direction, which is the one the stored lines cannot see: the
    // file is left at 30, the location still stores 12, and healing it onto 30
    // makes them one destination.
    {
      Stmt set{fix.db, "UPDATE files SET last_line=30 WHERE path='koi/src/hooked.cpp';"};
      EXPECT_TRUE(set.Run());
    }
    const SmartCorpus moved = fix.Snapshot();
    EXPECT_EQ(rows_in(fix.Rank(moved, "hooked"), "hooked.cpp"), std::size_t{2});
    EXPECT_EQ(rows_in(fix.Rank(moved, "hooked", heals_to(30)), "hooked.cpp"), std::size_t{1});
  }
}

// The third prior: 0.10 for a row whose file this branch has touched. It has to
// reach all three kinds -- the two the feature exists for are the definition and
// the visited line, and neither carries the signal any other way.
void SmartJumpBranchDiffPrior() {
  // A git that runs is the whole of the setup: without one the helper answers
  // empty, which is a supported shape and not this case.
  if (std::system("git --version >/dev/null 2>&1") != 0) return;

  TEST_CASE("smart jump: a row the branch has touched leads its twin that it has not");

  const Scratch scratch{"koi-smartjump-branch-diff"};
  const AsProjectRoot root{scratch.dir};
  WriteFixtureFile(scratch.dir / "alpha.cpp", "int Handler();\n");
  WriteFixtureFile(scratch.dir / "beta.cpp", "int Handler();\n");

  const std::string dir = "'" + scratch.dir.string() + "'";
  const auto git = [&dir](const std::string& args) {
    return std::system(("git -C " + dir + " -c user.email=koi@test -c user.name=koi " + args +
                        " >/dev/null 2>&1")
                           .c_str());
  };
  if (std::system(("git -c init.defaultBranch=main init -q " + dir + " >/dev/null 2>&1").c_str()) !=
      0) {
    return;
  }
  EXPECT_EQ(git("add -A"), 0);
  EXPECT_EQ(git("commit -q -m init"), 0);
  // alpha is what the branch has changed since the merge base; beta is not.
  WriteFixtureFile(scratch.dir / "alpha.cpp", "int Handler(int);\n");
  // A setup that did not take is a failure, not a quiet pass: everything below
  // is a statement about this one name being in the diff.
  const std::vector<std::string>& changed = BranchDiffFiles();
  EXPECT_EQ(changed.size(), std::size_t{1});
  if (changed.size() != 1) return;
  EXPECT_EQ(changed.front(), std::string{"alpha.cpp"});

  Editor ed;
  ed.theme = BuiltinTheme();
  std::string error;
  ed.project = ProjectStore::Open(scratch.dir / "state.db", error);
  EXPECT_TRUE(ed.project != nullptr);
  if (ed.project == nullptr) return;
  sqlite3* db = ed.project->Connection();

  // Twins in everything the blend reads: same weight, same age, same branch,
  // same name and the same length of it. What is left between them is the diff.
  const double now = Now();
  SeedFile(db, "alpha.cpp", 10, 0, now - kHour);
  SeedFile(db, "beta.cpp", 10, 0, now - kHour);
  SeedSymbol(db, "alpha.cpp", "Handler", 1, 3, now - kHour);
  SeedSymbol(db, "beta.cpp", "Handler", 1, 3, now - kHour);
  SeedLocation(db, "alpha.cpp", 1, "int Handler();", 3, 0, now - kHour);
  SeedLocation(db, "beta.cpp", 1, "int Handler();", 3, 0, now - kHour);

  SmartCorpus corpus;
  BuildSmartCorpus(*ed.project, corpus);
  for (const SmartRow& row : corpus.files) {
    EXPECT_EQ(row.in_branch_diff, row.file_key == std::string{"alpha.cpp"});
  }
  for (const SmartRow& row : corpus.symbols) {
    EXPECT_EQ(row.in_branch_diff, row.file_key == std::string{"alpha.cpp"});
  }
  for (const SmartRow& row : corpus.locations) {
    EXPECT_EQ(row.in_branch_diff, row.file_key == std::string{"alpha.cpp"});
  }

  // And the prior is worth exactly what the doc says, on the two kinds that had
  // been losing it.
  const auto leads = [&ed, &corpus](std::string_view typed) {
    const std::vector<SmartMatch> got =
        RankSmartMatches(corpus, ParseSmartQuery(typed), ed.project.get());
    EXPECT_EQ(got.size(), std::size_t{2});
    if (got.size() != 2) return;
    EXPECT_TRUE(got[0].key.starts_with("alpha.cpp"));
    EXPECT_TRUE(std::abs((got[0].score - got[1].score) - kBranchDiffPrior) < 1e-9);
  };
  leads("c handler");
  leads("d handler");
}

void SmartJumpFeedbackText() {
  TEST_CASE("smart jump: what the status line says while you type");

  Fixture fix{"koi-smartjump-feedback"};
  if (fix.ed.project == nullptr) return;
  const SmartCorpus corpus = fix.Snapshot();

  // Nothing typed yet says nothing: the prompt is not a list of everything.
  EXPECT_EQ(SmartJumpFeedback(ParseSmartQuery(""), {}), std::string{});

  {
    const SmartQuery q = ParseSmartQuery("c split");
    const std::string said = SmartJumpFeedback(q, fix.Preview(corpus, "c split"));
    // The count, then the best target with its line text.
    EXPECT_TRUE(said.starts_with("2  "));
    EXPECT_TRUE(said.find("piece_tree.cpp:212") != std::string::npos);
    EXPECT_TRUE(said.find("SplitNode(&node);") != std::string::npos);
  }

  {
    const SmartQuery q = ParseSmartQuery("zqx");
    EXPECT_EQ(SmartJumpFeedback(q, fix.Preview(corpus, "zqx")), std::string{"not been there"});
  }

  // The summary is the ranking, minus the strings for rows nothing reads: the
  // count and the rows the band draws have to be the ones enter and alt+letter
  // would use, or the prompt promises one landing and makes another.
  for (const std::string_view typed : {"key", "c split", "d pin", "key 640", "zqx", ""}) {
    const std::vector<SmartMatch> full = fix.Rank(corpus, typed);
    const SmartPreview preview = fix.Preview(corpus, typed);
    EXPECT_EQ(preview.count, full.size());
    EXPECT_EQ(preview.top.size(), std::min(full.size(), kPickerRows));
    for (std::size_t at = 0; at < preview.top.size(); ++at) {
      EXPECT_EQ(preview.top[at].display, full[at].display);
      EXPECT_EQ(preview.top[at].key, full[at].key);
    }
  }

  {
    const SmartQuery q = ParseSmartQuery("key z");
    EXPECT_EQ(SmartJumpFeedback(q, {}), q.error);
  }

  TEST_CASE("smart jump: the prompt answers on every keystroke, through the ordinary one");
  {
    PressKey press;
    press(fix.ed, "h");
    EXPECT_TRUE(fix.ed.prompt_active);
    EXPECT_TRUE(fix.ed.prompt_kind == PromptKind::kSmartJump);
    EXPECT_EQ(std::string{PromptSigil(fix.ed)}, std::string{"ᛃ "});
    fix.ed.settings.icons = false;
    EXPECT_EQ(std::string{PromptSigil(fix.ed)}, std::string{"jump:"});
    fix.ed.settings.icons = true;
    // The snapshot is taken once, when the prompt opens.
    EXPECT_TRUE(fix.ed.smart_jump != nullptr);
    if (fix.ed.smart_jump == nullptr) return;
    EXPECT_EQ(fix.ed.smart_jump->corpus.files.size(), std::size_t{5});

    press(fix.ed, "k");
    press(fix.ed, "e");
    press(fix.ed, "y");
    EXPECT_EQ(fix.ed.prompt_input, std::string{"key"});
    EXPECT_TRUE(fix.ed.status.text().find("keymap.cpp") != std::string::npos);
    EXPECT_TRUE(fix.ed.status.text().starts_with("4  "));

    // And narrows as more arrives.
    press(fix.ed, "m");
    EXPECT_TRUE(fix.ed.status.text().starts_with("1  "));

    press(fix.ed, "esc");
    EXPECT_FALSE(fix.ed.prompt_active);
  }

  TEST_CASE("smart jump: one text one severity, and esc takes the answer with the question");
  {
    PressKey press;
    press(fix.ed, "h");
    press(fix.ed, "z");
    // A query that will not parse is a warning here for the same reason it is
    // one on submit: it is the same sentence about the same mistake.
    EXPECT_TRUE(fix.ed.status.text().find("not a clause") != std::string::npos);
    EXPECT_TRUE(fix.ed.status.level() == StatusLevel::kWarning);

    // And fixing the word takes the warning's colour off the line with it.
    press(fix.ed, "backspace");
    press(fix.ed, "k");
    press(fix.ed, "e");
    press(fix.ed, "y");
    EXPECT_TRUE(fix.ed.status.text().starts_with("4  "));
    EXPECT_TRUE(fix.ed.status.level() == StatusLevel::kInfo);

    // Esc answers nothing, so the feedback about what enter would have done
    // does not outlive the prompt it was written for.
    press(fix.ed, "esc");
    EXPECT_FALSE(fix.ed.prompt_active);
    EXPECT_TRUE(fix.ed.status.empty());

    // The accept paths cancel the prompt too, and they write after: what they
    // say survives.
    press(fix.ed, "h");
    press(fix.ed, "k");
    press(fix.ed, "e");
    press(fix.ed, "y");
    press(fix.ed, "enter");
    EXPECT_FALSE(fix.ed.prompt_active);
    EXPECT_TRUE(fix.ed.status.text().find("/4") != std::string::npos);
  }

  TEST_CASE("smart jump: the band under the box is the head of the ranking, in its order");
  {
    PressKey press;
    press(fix.ed, "h");
    // A file-tier query: five visited .cpp files, which is the whole band.
    for (const std::string_view key : {"c", "p", "p"}) press(fix.ed, key);

    EXPECT_TRUE(fix.ed.smart_band != nullptr);
    if (fix.ed.smart_band == nullptr) return;
    const std::vector<SmartMatch> full = fix.Rank(corpus, "cpp");
    EXPECT_EQ(full.size(), std::size_t{5});
    EXPECT_EQ(fix.ed.smart_band->shown.size(), std::size_t{5});
    // The band's order is the ranking's, and its rows are the destinations --
    // a file row is its path, and it carries no second copy of it.
    for (std::size_t at = 0; at < fix.ed.smart_band->shown.size(); ++at) {
      EXPECT_EQ(std::string{PickerRowLead(*fix.ed.smart_band, at)}, full[at].display);
      EXPECT_TRUE(PickerRowDetail(*fix.ed.smart_band, at).empty());
    }
    // The count says five of five here; a query with more matches than the band
    // holds says five of all of them (render.cpp draws it).
    EXPECT_EQ(PickerTotal(*fix.ed.smart_band), std::size_t{5});
    // The block is the lines around the row enter would land on -- row 0's
    // file, at row 0's line -- and it follows nothing else.
    EXPECT_TRUE(!fix.ed.smart_band->context.empty());
    EXPECT_EQ(fix.ed.smart_band->context_target, fix.ed.smart_band->rows[0].line);

    // A query that matched nothing, and one that will not parse, are no band at
    // all: the feedback row says which of the two it was.
    for (const std::string_view key : {"z", "q", "x"}) press(fix.ed, key);
    EXPECT_EQ(fix.ed.prompt_input, std::string{"cppzqx"});
    EXPECT_TRUE(fix.ed.smart_band == nullptr);
    EXPECT_EQ(fix.ed.status.text(), std::string{"not been there"});

    for (int i = 0; i < 6; ++i) press(fix.ed, "backspace");
    for (const std::string_view key : {"k", "e", "y", "space", "z"}) press(fix.ed, key);
    EXPECT_TRUE(fix.ed.smart_band == nullptr);
    EXPECT_EQ(fix.ed.status.text(), ParseSmartQuery("key z").error);

    press(fix.ed, "esc");
    EXPECT_TRUE(fix.ed.smart_band == nullptr);
  }

  TEST_CASE("smart jump: the band's lines are healed, and one match is a band of one");
  {
    Fixture heal{"koi-smartjump-band-heal"};
    if (heal.ed.project == nullptr) return;
    // Landed in piece_tree.cpp, then three lines pushed in above the anchor:
    // the store still says 212 and the band has to say 215, because 215 is
    // where enter would put the cursor.
    SmartJumpSubmit(heal.ed, "c splitnode");
    EXPECT_EQ(heal.ed.doc.file.filename().string(), std::string{"piece_tree.cpp"});
    Edit edit;
    ExpectOk(
        Insert("one\ntwo\nthree\n", LineStart(heal.ed.doc.table, 0), heal.ed.doc.table, &edit),
        "three lines above the anchor");

    PressKey press;
    press(heal.ed, "h");
    for (const std::string_view key : {"c", "space", "s", "p", "l", "i", "t", "n", "o", "d", "e"}) {
      press(heal.ed, key);
    }
    EXPECT_TRUE(heal.ed.smart_band != nullptr);
    if (heal.ed.smart_band == nullptr) return;
    // One match is a band of one -- no gate on ambiguity, no key to summon it.
    EXPECT_EQ(heal.ed.smart_band->shown.size(), std::size_t{1});
    EXPECT_EQ(heal.ed.smart_band->rows[0].line, Index{215});
    EXPECT_EQ(std::string{PickerRowDetail(*heal.ed.smart_band, 0)},
              std::string{"koi/src/piece_tree.cpp:215"});
    EXPECT_EQ(std::string{PickerRowLead(*heal.ed.smart_band, 0)}, std::string{"SplitNode(&node);"});
    // And the block is drawn around the healed line, out of the open buffer's
    // own table rather than off the disk the edit never reached.
    EXPECT_EQ(heal.ed.smart_band->context_target, Index{215});
    EXPECT_TRUE(!heal.ed.smart_band->context.empty());
    press(heal.ed, "esc");
  }

  TEST_CASE("smart jump: alt+letter lands on that band row, and past the band it does nothing");
  {
    Fixture pick{"koi-smartjump-band-letter"};
    if (pick.ed.project == nullptr) return;
    std::string error;
    pick.ed.jumps = JumpStore::Open(pick.ed.project, "pane", error);
    const std::vector<SmartMatch> full = pick.Rank(pick.Snapshot(), "key");
    EXPECT_EQ(full.size(), std::size_t{4});

    PressKey press;
    press(pick.ed, "h");
    for (const std::string_view key : {"k", "e", "y"}) press(pick.ed, key);
    EXPECT_EQ(pick.ed.smart_band->shown.size(), std::size_t{4});

    // A letter past the rows the band drew names nothing: the prompt stands and
    // the query is still there to be fixed.
    press(pick.ed, "A-a");
    EXPECT_TRUE(pick.ed.prompt_active);
    EXPECT_EQ(pick.ed.prompt_input, std::string{"key"});

    // A bare letter is query text, not an accelerator; so is a bare digit,
    // which is the line clause.
    press(pick.ed, "2");
    EXPECT_TRUE(pick.ed.prompt_active);
    EXPECT_EQ(pick.ed.prompt_input, std::string{"key2"});
    press(pick.ed, "backspace");
    press(pick.ed, "j");
    EXPECT_EQ(pick.ed.prompt_input, std::string{"keyj"});
    press(pick.ed, "backspace");
    // An alt chord that names no row does nothing at all -- neither typed nor
    // taken.
    press(pick.ed, "A-z");
    EXPECT_TRUE(pick.ed.prompt_active);
    EXPECT_EQ(pick.ed.prompt_input, std::string{"key"});

    // The second row, which is not what enter would have taken. The prompt
    // closes exactly as an enter landing closes it, the walk starts from the
    // row chosen, and the arrival is armed for that row.
    press(pick.ed, "A-j");
    EXPECT_FALSE(pick.ed.prompt_active);
    EXPECT_TRUE(pick.ed.smart_band == nullptr);
    EXPECT_EQ(pick.ed.doc.file.filename().string(),
              fs::path{full[1].path}.filename().string());
    EXPECT_EQ(pick.ed.smart_jump->at, std::size_t{1});
    EXPECT_TRUE(pick.ed.status.text().starts_with("ᛃ 2/4"));
    EXPECT_EQ(pick.ed.record.pending_target, full[1].key);
    EXPECT_EQ(pick.ed.record.pending_typed, std::string{"key"});
    EXPECT_TRUE(pick.ed.record.pending_line > 0);

    // And the credit the bounce was holding is the row the letter chose, not
    // the row enter would have taken.
    pick.ed.record.pending_since -= kBounceSeconds + 1;
    NoteInputBoundary(pick.ed);
    Stmt stmt{pick.db, "SELECT target FROM queries WHERE prefix='key';"};
    EXPECT_TRUE(stmt.Step());
    EXPECT_EQ(stmt.Column(0), full[1].key);
  }

  TEST_CASE("smart jump: the arrows walk the band and enter lands on what they left");
  {
    Fixture walk{"koi-smartjump-band-arrows"};
    if (walk.ed.project == nullptr) return;
    std::string error;
    walk.ed.jumps = JumpStore::Open(walk.ed.project, "pane", error);
    const std::vector<SmartMatch> full = walk.Rank(walk.Snapshot(), "key");
    EXPECT_EQ(full.size(), std::size_t{4});

    PressKey press;
    press(walk.ed, "h");
    for (const std::string_view key : {"k", "e", "y"}) press(walk.ed, key);
    EXPECT_TRUE(walk.ed.smart_band != nullptr);
    if (walk.ed.smart_band == nullptr) return;
    EXPECT_EQ(walk.ed.smart_band->shown.size(), std::size_t{4});
    // Row 0 until the arrows move it, and typing does not.
    EXPECT_EQ(walk.ed.smart_band->selected, std::size_t{0});

    // Down the list and back up over its top, wrapping at the ends the way a
    // picker's arrows wrap -- and the input is untouched: an arrow is not text.
    press(walk.ed, "down");
    press(walk.ed, "down");
    EXPECT_EQ(walk.ed.smart_band->selected, std::size_t{2});
    EXPECT_EQ(walk.ed.prompt_input, std::string{"key"});
    press(walk.ed, "up");
    press(walk.ed, "up");
    press(walk.ed, "up");
    EXPECT_EQ(walk.ed.smart_band->selected, std::size_t{3});
    press(walk.ed, "down");
    EXPECT_EQ(walk.ed.smart_band->selected, std::size_t{0});

    // The block follows the selection: what it draws is the selected row's
    // target, not the head of the ranking.
    press(walk.ed, "down");
    EXPECT_EQ(walk.ed.smart_band->selected, std::size_t{1});
    if (!walk.ed.smart_band->context.empty()) {
      EXPECT_EQ(walk.ed.smart_band->context_target,
                walk.ed.smart_band->rows[walk.ed.smart_band->shown[1]].line);
    }

    // A keystroke rebuilds the band, and a new list starts at its top.
    press(walk.ed, "backspace");
    press(walk.ed, "y");
    EXPECT_TRUE(walk.ed.smart_band != nullptr);
    if (walk.ed.smart_band == nullptr) return;
    EXPECT_EQ(walk.ed.smart_band->selected, std::size_t{0});

    // Enter lands on the row the arrows left selected, exactly as the letter
    // for that row would have: same walk position, same status, same arrival.
    press(walk.ed, "down");
    press(walk.ed, "down");
    EXPECT_EQ(walk.ed.smart_band->selected, std::size_t{2});
    press(walk.ed, "ret");
    EXPECT_FALSE(walk.ed.prompt_active);
    EXPECT_TRUE(walk.ed.smart_band == nullptr);
    EXPECT_EQ(walk.ed.doc.file.filename().string(), fs::path{full[2].path}.filename().string());
    EXPECT_EQ(walk.ed.smart_jump->at, std::size_t{2});
    EXPECT_TRUE(walk.ed.status.text().starts_with("ᛃ 3/4"));
    EXPECT_EQ(walk.ed.record.pending_target, full[2].key);

    // And the credit the bounce was holding is that row, not the top one.
    walk.ed.record.pending_since -= kBounceSeconds + 1;
    NoteInputBoundary(walk.ed);
    Stmt stmt{walk.db, "SELECT target FROM queries WHERE prefix='key';"};
    EXPECT_TRUE(stmt.Step());
    EXPECT_EQ(stmt.Column(0), full[2].key);

    // With no band under the box the arrows are the history again: the query
    // just submitted comes back, and the band it builds takes the arrows over
    // from there.
    press(walk.ed, "h");
    EXPECT_TRUE(walk.ed.smart_band == nullptr);
    press(walk.ed, "up");
    EXPECT_EQ(walk.ed.prompt_input, std::string{"key"});
    EXPECT_TRUE(walk.ed.smart_band != nullptr);
    if (walk.ed.smart_band == nullptr) return;
    press(walk.ed, "up");
    EXPECT_EQ(walk.ed.prompt_input, std::string{"key"});
    EXPECT_EQ(walk.ed.smart_band->selected, walk.ed.smart_band->shown.size() - 1);
    press(walk.ed, "esc");
  }
}

void SmartJumpLanding() {
  TEST_CASE("smart jump: one match jumps, and credits the query for it");

  Fixture fix{"koi-smartjump-landing"};
  if (fix.ed.project == nullptr) return;
  std::string error;
  fix.ed.jumps = JumpStore::Open(fix.ed.project, "pane", error);

  SmartJumpSubmit(fix.ed, "piece");
  EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"piece_tree.cpp"});
  // The credit waits with the bounce: nothing in `queries` until the arrival
  // has been stood in, then both the row and the credit land together.
  EXPECT_EQ(Scalar(fix.db, "SELECT COUNT(*) FROM queries WHERE prefix='piece';"), std::int64_t{0});
  fix.ed.record.pending_since -= kBounceSeconds + 1;
  NoteInputBoundary(fix.ed);
  EXPECT_EQ(Scalar(fix.db, "SELECT COUNT(*) FROM queries WHERE prefix='piece';"), std::int64_t{1});
  {
    Stmt stmt{fix.db, "SELECT target FROM queries WHERE prefix='piece';"};
    EXPECT_TRUE(stmt.Step());
    EXPECT_EQ(stmt.Column(0), std::string{"koi/src/piece_tree.cpp"});
  }

  TEST_CASE("smart jump: nothing found falls through to the respective picker");
  {
    // The decision itself, which is all of the contract: the most specific
    // clause names the picker and hands over its own terms, never the keywords.
    const SmartHandoff content = SmartJumpHandoff(ParseSmartQuery("c zzz"));
    EXPECT_TRUE(content.picker == SmartPicker::kContent);
    EXPECT_EQ(content.query, std::string{"zzz"});
    EXPECT_EQ(std::string{SmartPickerName(content.picker)}, std::string{"content picker"});

    const SmartHandoff symbol = SmartJumpHandoff(ParseSmartQuery("d zzz"));
    EXPECT_TRUE(symbol.picker == SmartPicker::kSymbol);
    EXPECT_EQ(symbol.query, std::string{"zzz"});
    EXPECT_EQ(std::string{SmartPickerName(symbol.picker)}, std::string{"symbol picker"});

    // A pinned file clause still names the file picker.
    const SmartHandoff file = SmartJumpHandoff(ParseSmartQuery("f zzz"));
    EXPECT_TRUE(file.picker == SmartPicker::kFile);
    EXPECT_EQ(file.query, std::string{"zzz"});
    EXPECT_EQ(std::string{SmartPickerName(file.picker)}, std::string{"file picker"});

    // A bare one names no clause at all, so nothing may be left behind: the
    // whole query goes to the picker whose rows are `path:line:text` and can
    // therefore still mean any of the three.
    const SmartHandoff bare = SmartJumpHandoff(ParseSmartQuery("zzz"));
    EXPECT_TRUE(bare.picker == SmartPicker::kContent);
    EXPECT_EQ(bare.query, std::string{"zzz"});
    EXPECT_TRUE(SmartDroppedTerms(ParseSmartQuery("zzz"), bare.picker).empty());
    const SmartHandoff bare_two = SmartJumpHandoff(ParseSmartQuery("key zzz"));
    EXPECT_TRUE(bare_two.picker == SmartPicker::kContent);
    EXPECT_EQ(bare_two.query, std::string{"key.*zzz"});
    EXPECT_TRUE(SmartDroppedTerms(ParseSmartQuery("key zzz"), bare_two.picker).empty());
    // One bare term is enough: a query part-pinned to paths is still a query
    // that never said which kind it wanted.
    const SmartHandoff half = SmartJumpHandoff(ParseSmartQuery("key f zzz"));
    EXPECT_TRUE(half.picker == SmartPicker::kContent);
    EXPECT_EQ(half.query, std::string{"key.*zzz"});

    // Two clauses: the more specific one decides, and only its terms travel.
    const SmartHandoff both = SmartJumpHandoff(ParseSmartQuery("key c zzz"));
    EXPECT_TRUE(both.picker == SmartPicker::kContent);
    EXPECT_EQ(both.query, std::string{"zzz"});

    // Two terms are two terms, not one string with a space in it: the picker
    // matches its rows with a regex, so they join the way they were meant.
    const SmartHandoff defs = SmartJumpHandoff(ParseSmartQuery("key d zzz vvv"));
    EXPECT_TRUE(defs.picker == SmartPicker::kSymbol);
    EXPECT_EQ(defs.query, std::string{"zzz.*vvv"});

    // And a term that would be a pattern is a term. `foo(` searches for foo(.
    const SmartHandoff meta = SmartJumpHandoff(ParseSmartQuery("c foo( b.r"));
    EXPECT_EQ(meta.query, std::string{R"(foo\(.*b\.r)"});

    // What the handoff could not take with it, for the sentence that has to
    // admit it. The deciding clause's terms are never in the list.
    EXPECT_EQ(SmartDroppedTerms(ParseSmartQuery("nav d pin"), SmartPicker::kSymbol),
              std::string{"`nav`"});
    EXPECT_EQ(SmartDroppedTerms(ParseSmartQuery("key cpp c split"), SmartPicker::kContent),
              std::string{"`key` `cpp`"});
    EXPECT_TRUE(SmartDroppedTerms(ParseSmartQuery("d pin"), SmartPicker::kSymbol).empty());
    EXPECT_TRUE(SmartDroppedTerms(ParseSmartQuery("f key cpp"), SmartPicker::kFile).empty());

    // A bare line is a content clause with nothing to search for: the picker
    // opens empty rather than grepping the project for a number.
    const SmartHandoff line = SmartJumpHandoff(ParseSmartQuery("key 640"));
    EXPECT_TRUE(line.picker == SmartPicker::kContent);
    EXPECT_TRUE(line.query.empty());

    // And the picker each one names is the picker the dead end says it is
    // opening -- one mapping, so the message and the band cannot disagree. The
    // blocks below open all three with the terms typed in.
    EXPECT_EQ(std::string{SmartPickerName(content.picker)}, std::string{"content picker"});
    EXPECT_EQ(std::string{SmartPickerName(symbol.picker)}, std::string{"symbol picker"});
    EXPECT_EQ(std::string{SmartPickerName(file.picker)}, std::string{"file picker"});
    EXPECT_EQ(std::string{SmartPickerName(defs.picker)}, std::string{"symbol picker"});
    EXPECT_EQ(std::string{SmartPickerName(meta.picker)}, std::string{"content picker"});
    EXPECT_EQ(std::string{SmartPickerName(line.picker)}, std::string{"content picker"});
  }
  {
    // And the glue asks for it: the box the query was typed in grows a band,
    // filtered by the terms it carried. `zqxvw` is in no store row, so it is a
    // dead end -- and it is a file on disk, so the picker that does search the
    // project has something to show for it. Pinned, because a bare query takes
    // the content door instead (the block after the three keyworded ones).
    const fs::path stranger = fix.Write("zqxvw_notes.txt", "x\n");
    fix.ed.settings.file_filter = "printf '%s\\n' " + stranger.string() + " " +
                                  (fix.scratch.dir / "koi/src/keymap.cpp").string();
    const std::string was = fix.ed.doc.file.string();
    const std::size_t buffers = BufferCount(fix.ed);

    SmartJumpSubmit(fix.ed, "f zqxvw");
    EXPECT_TRUE(fix.ed.prompt_active);
    EXPECT_TRUE(fix.ed.prompt_kind == PromptKind::kPicker);
    EXPECT_TRUE(fix.ed.picker != nullptr);
    if (fix.ed.picker == nullptr) return;
    EXPECT_TRUE(fix.ed.picker->source == PickerState::Source::kFiles);
    // Typed in, and a typed query is a filter: both rows were offered, one of
    // them matches.
    EXPECT_EQ(fix.ed.prompt_input, std::string{"zqxvw"});
    EXPECT_EQ(fix.ed.picker->rows.size(), std::size_t{2});
    EXPECT_EQ(fix.ed.picker->shown.size(), std::size_t{1});
    if (fix.ed.picker->shown.size() != 1) return;
    EXPECT_EQ(fix.ed.picker->rows[fix.ed.picker->shown[0]].target, stranger.string());
    // Said over the band it explains, not lost to the prompt that opened.
    EXPECT_TRUE(fix.ed.status.text().find("not been there -- file picker") != std::string::npos);
    // Nothing was opened on the way there: the band is the answer, not a jump.
    EXPECT_EQ(fix.ed.doc.file.string(), was);
    EXPECT_EQ(BufferCount(fix.ed), buffers);
    // And the open path records which picker ran, so last_picker reopens it.
    std::string name;
    std::string query;
    EXPECT_TRUE(ReadLastPicker(name, query));
    EXPECT_EQ(name, std::string{"files"});
    EXPECT_EQ(query, std::string{"zqxvw"});
    PromptCancel(fix.ed);
  }
  {
    // The symbol clause takes the same door: the project scan opens in process
    // with the query installed as the filter its rows land under.
    SmartJumpSubmit(fix.ed, "d zqxvw");
    EXPECT_TRUE(fix.ed.prompt_active);
    EXPECT_TRUE(fix.ed.prompt_kind == PromptKind::kPicker);
    EXPECT_TRUE(fix.ed.picker != nullptr);
    if (fix.ed.picker == nullptr) return;
    EXPECT_TRUE(fix.ed.picker->source == PickerState::Source::kProjectSymbols);
    EXPECT_EQ(fix.ed.prompt_input, std::string{"zqxvw"});
    EXPECT_TRUE(fix.ed.picker->scan != nullptr);
    if (fix.ed.picker->scan == nullptr) return;
    EXPECT_EQ(fix.ed.picker->scan->filter, std::string{"zqxvw"});
    EXPECT_TRUE(fix.ed.status.text().find("not been there -- symbol picker") != std::string::npos);
    PromptCancel(fix.ed);
  }
  {
    // The dead end says what it left at the door too: the file terms narrowed
    // the query and the symbol picker cannot take them.
    SmartJumpSubmit(fix.ed, "nav d zqxvw");
    EXPECT_TRUE(fix.ed.prompt_kind == PromptKind::kPicker);
    EXPECT_TRUE(fix.ed.status.text().find("not been there -- symbol picker, dropped `nav`") !=
                std::string::npos);
    PromptCancel(fix.ed);
  }
  {
    // Content takes the same door, and it needs no terminal to: the scan streams
    // into the band with the terms installed as the filter its lines land under.
    SmartJumpSubmit(fix.ed, "c zqxvw");
    EXPECT_TRUE(fix.ed.prompt_active);
    EXPECT_TRUE(fix.ed.prompt_kind == PromptKind::kPicker);
    EXPECT_TRUE(fix.ed.picker != nullptr);
    if (fix.ed.picker == nullptr) return;
    EXPECT_TRUE(fix.ed.picker->source == PickerState::Source::kContent);
    EXPECT_EQ(fix.ed.prompt_input, std::string{"zqxvw"});
    EXPECT_TRUE(fix.ed.picker->scan != nullptr);
    if (fix.ed.picker->scan == nullptr) return;
    EXPECT_EQ(fix.ed.picker->scan->filter, std::string{"zqxvw"});
    EXPECT_TRUE(fix.ed.status.text().find("not been there -- content picker") !=
                std::string::npos);
    PromptCancel(fix.ed);
  }
  {
    // And a bare query -- no keyword, no line -- takes that same door with the
    // whole of itself. Nothing is dropped, because nothing was pinned for the
    // door to refuse.
    SmartJumpSubmit(fix.ed, "zqxvw");
    EXPECT_TRUE(fix.ed.prompt_kind == PromptKind::kPicker);
    EXPECT_TRUE(fix.ed.picker != nullptr);
    if (fix.ed.picker == nullptr) return;
    EXPECT_TRUE(fix.ed.picker->source == PickerState::Source::kContent);
    EXPECT_EQ(fix.ed.prompt_input, std::string{"zqxvw"});
    EXPECT_TRUE(fix.ed.status.text().find("not been there -- content picker") !=
                std::string::npos);
    EXPECT_TRUE(fix.ed.status.text().find("dropped") == std::string::npos);
    PromptCancel(fix.ed);
  }

  TEST_CASE("smart jump: Tab opens the same door the dead end does");
  {
    // Tab is the escape hatch before Enter, and it goes through the same
    // handoff: the deciding clause names the picker and its own terms travel,
    // escaped and joined the way a picker query is a pattern.
    const SmartHandoff files = SmartJumpHandoff(ParseSmartQuery("f key cpp"));
    EXPECT_TRUE(files.picker == SmartPicker::kFile);
    EXPECT_EQ(files.query, std::string{"key.*cpp"});

    const SmartHandoff content = SmartJumpHandoff(ParseSmartQuery("c split"));
    EXPECT_TRUE(content.picker == SmartPicker::kContent);
    EXPECT_EQ(content.query, std::string{"split"});

    // A term that would be a pattern is a term wherever it travels.
    const SmartHandoff meta = SmartJumpHandoff(ParseSmartQuery("f foo("));
    EXPECT_TRUE(meta.picker == SmartPicker::kFile);
    EXPECT_EQ(meta.query, std::string{R"(foo\()"});

    // And the key reaches it, in the box already open: one prompt closes and
    // the next opens on the same keystroke, so what is at the caret goes from a
    // query to a band without ever being nothing.
    PressKey press;
    press(fix.ed, "h");
    for (const std::string_view key : {"f", "space", "k", "e", "y"}) press(fix.ed, key);
    EXPECT_TRUE(fix.ed.prompt_active);
    press(fix.ed, "tab");
    EXPECT_TRUE(fix.ed.prompt_active);
    EXPECT_TRUE(fix.ed.prompt_kind == PromptKind::kPicker);
    EXPECT_TRUE(fix.ed.picker != nullptr);
    if (fix.ed.picker == nullptr) return;
    EXPECT_TRUE(fix.ed.picker->source == PickerState::Source::kFiles);
    // The keyword pinned the clause; it never travels itself.
    EXPECT_EQ(fix.ed.prompt_input, std::string{"key"});
    // Filtering, not just prefilled: keymap.cpp of the two rows the filter ran.
    EXPECT_EQ(fix.ed.picker->shown.size(), std::size_t{1});
    press(fix.ed, "esc");
    EXPECT_FALSE(fix.ed.prompt_active);
  }

  TEST_CASE("smart jump: Tab refuses a bad parse, keeps the query and owns what it drops");
  {
    PressKey press;
    press(fix.ed, "h");
    press(fix.ed, "z");
    press(fix.ed, "tab");
    // Half a query is not the query: the terms the parse reached are not the
    // ones the bad word was going to narrow, so nothing is handed over and the
    // prompt stays with the reason on it.
    EXPECT_TRUE(fix.ed.prompt_active);
    EXPECT_TRUE(fix.ed.prompt_kind == PromptKind::kSmartJump);
    EXPECT_TRUE(fix.ed.picker == nullptr);
    EXPECT_EQ(fix.ed.prompt_input, std::string{"z"});
    EXPECT_TRUE(fix.ed.status.text().find("not a clause") != std::string::npos);
    EXPECT_TRUE(fix.ed.status.level() == StatusLevel::kWarning);
    press(fix.ed, "esc");

    press(fix.ed, "h");
    for (const std::string_view key : {"n", "a", "v", "space", "d", "space", "p", "i", "n"}) {
      press(fix.ed, key);
    }
    EXPECT_EQ(fix.ed.prompt_input, std::string{"nav d pin"});
    press(fix.ed, "tab");
    EXPECT_TRUE(fix.ed.prompt_kind == PromptKind::kPicker);
    EXPECT_TRUE(fix.ed.picker != nullptr);
    // Only the deciding clause's terms travel, and the line says which half
    // stayed behind: a band filtered by `pin` alone looks exactly like a band
    // filtered by the whole query.
    EXPECT_EQ(fix.ed.prompt_input, std::string{"pin"});
    EXPECT_TRUE(fix.ed.status.text().find("symbol picker, dropped `nav`") != std::string::npos);
    press(fix.ed, "esc");

    // And the query earned its place in the history: tab is a way of using it,
    // not of throwing it away, so up-arrow in the next jump prompt has it.
    press(fix.ed, "h");
    press(fix.ed, "up");
    EXPECT_EQ(fix.ed.prompt_input, std::string{"nav d pin"});
    press(fix.ed, "esc");
  }

  TEST_CASE("smart jump: an ambiguous query lands the best match, not a list");
  {
    SmartJumpSubmit(fix.ed, "key");
    // No view: Enter drops on the top of the ranking the way a search drops on
    // its first hit, and stepping is the disambiguator.
    EXPECT_FALSE(IsExcerptView(fix.ed.doc));
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keymap.cpp"});
    EXPECT_TRUE(fix.ed.status.text().starts_with("ᛃ 1/"));
    // Nothing is learnt until the arrival is stood in.
    EXPECT_EQ(Scalar(fix.db, "SELECT COUNT(*) FROM queries WHERE prefix='key'"), std::int64_t{0});
    fix.ed.record.pending_since -= kBounceSeconds + 1;
    NoteInputBoundary(fix.ed);
    Stmt stmt{fix.db, "SELECT target FROM queries WHERE prefix='key';"};
    EXPECT_TRUE(stmt.Step());
    EXPECT_EQ(stmt.Column(0), std::string{"koi/src/keymap.cpp"});
  }

  TEST_CASE("smart jump: a step that cannot open says why, and leaves no row behind it");
  {
    Fixture bad{"koi-smartjump-bad-open"};
    if (bad.ed.project == nullptr) return;
    // The cwd is the project root, as it is when koi runs in one: what the
    // failure names has to be the spelling every other smart-jump surface uses.
    const InDirectory here{bad.scratch.dir};
    std::string jump_error;
    bad.ed.jumps = JumpStore::Open(bad.ed.project, "pane", jump_error);
    EXPECT_TRUE(bad.ed.jumps != nullptr);
    if (bad.ed.jumps == nullptr) return;

    const auto trail_rows = [&bad](std::string_view key) {
      return Scalar(bad.db, "SELECT COUNT(*) FROM locations WHERE path=? AND line=1;", key);
    };

    SmartJumpSubmit(bad.ed, "key");
    EXPECT_EQ(bad.ed.doc.file.filename().string(), std::string{"keymap.cpp"});
    if (bad.ed.smart_jump == nullptr) return;
    EXPECT_TRUE(bad.ed.smart_jump->matches.size() > 1);
    if (bad.ed.smart_jump->matches.size() < 2) return;

    // The row the next press goes to, made unreadable under the standing list.
    const std::string next_key = bad.ed.smart_jump->matches[1].key;
    const std::string good = "// still here\n";
    bad.Write(next_key, "\xff\xfe not utf-8\n");

    EXPECT_EQ(trail_rows("koi/src/keymap.cpp"), std::int64_t{0});
    SmartJumpStep(bad.ed, true);
    // Nothing moved, and what the open said is what stands: the reason it gave,
    // at the weight it gave it, spelled from the project root -- not a bare
    // warning naming an absolute path.
    EXPECT_EQ(bad.ed.doc.file.filename().string(), std::string{"keymap.cpp"});
    EXPECT_EQ(bad.ed.smart_jump->at, std::size_t{0});
    EXPECT_TRUE(bad.ed.status.text().starts_with("cannot open " + next_key + ":"));
    EXPECT_TRUE(bad.ed.status.text().find(bad.scratch.dir.string()) == std::string::npos);
    EXPECT_TRUE(bad.ed.status.level() == StatusLevel::kError);
    // And the departure stayed out of the trail: a jump that did not happen is
    // not a place to step back to.
    EXPECT_EQ(trail_rows("koi/src/keymap.cpp"), std::int64_t{0});

    // The same row, readable again: the step lands, the trail gets its row, and
    // the line comes back plain rather than wearing the failure's colour.
    bad.Write(next_key, good);
    SmartJumpStep(bad.ed, true);
    EXPECT_EQ(bad.ed.doc.file.filename().string(), fs::path{next_key}.filename().string());
    EXPECT_TRUE(bad.ed.status.text().starts_with("ᛃ 2/"));
    EXPECT_TRUE(bad.ed.status.level() == StatusLevel::kInfo);
    EXPECT_EQ(trail_rows("koi/src/keymap.cpp"), std::int64_t{1});
  }

  TEST_CASE("smart jump: stepping reaches the runner-up without crediting it");
  {
    SmartJumpStep(fix.ed, true);
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keylog.cpp"});
    // Bounced away before standing: the step taught nothing.
    NoteCommandBoundary(fix.ed);
    Stmt stmt{fix.db, "SELECT COUNT(*) FROM queries WHERE target LIKE '%keylog%';"};
    EXPECT_TRUE(stmt.Step());
    EXPECT_EQ(stmt.Integer(0), std::int64_t{0});
  }

  TEST_CASE("smart jump: the status names the line the landing healed to");
  {
    // The store says 212 and the landing goes where the anchor has drifted to.
    // Whatever names the row has to say the number the cursor is on, or the
    // status is a surprise dressed up as a promise. A lone match lands in
    // silence, so the naming is asked for with a step -- a walk of one names
    // itself.
    SmartJumpSubmit(fix.ed, "c splitnode");
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"piece_tree.cpp"});
    EXPECT_EQ(fix.ed.smart_jump->matches.size(), std::size_t{1});
    EXPECT_TRUE(fix.ed.status.empty());
    EXPECT_FALSE(fix.ed.jump_branch);
    SmartJumpStep(fix.ed, true);
    EXPECT_TRUE(fix.ed.status.text().find("piece_tree.cpp:212") != std::string::npos);

    Edit edit;
    ExpectOk(Insert("one\ntwo\nthree\n", LineStart(fix.ed.doc.table, 0), fix.ed.doc.table, &edit),
             "three lines above the anchor");

    SmartJumpSubmit(fix.ed, "c splitnode");
    const Index landed =
        LineAt(fix.ed.doc.table, CursorOf(fix.ed.doc.table, fix.ed.doc.selections.Primary())) + 1;
    EXPECT_EQ(landed, Index{215});
    EXPECT_TRUE(fix.ed.status.empty());

    // And the step names the healed line, text and all.
    SmartJumpStep(fix.ed, true);
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"piece_tree.cpp"});
    EXPECT_TRUE(fix.ed.status.text().find("piece_tree.cpp:215") != std::string::npos);
    EXPECT_TRUE(fix.ed.status.text().find("SplitNode(&node);") != std::string::npos);
  }
}

void SmartJumpAdaptiveLoop() {
  TEST_CASE("smart jump: confirmed pairs reorder the top of the ranking");

  Fixture fix{"koi-smartjump-adaptive"};
  if (fix.ed.project == nullptr) return;
  // keylog starts as the runner-up on frecency; enough confirmations of the
  // (key -> keylog) pair lift it over keymap, so Enter lands there directly.
  {
    const SmartCorpus corpus = fix.Snapshot();
    const std::vector<SmartMatch> got = fix.Rank(corpus, "key");
    EXPECT_TRUE(!got.empty());
    if (got.empty()) return;
    EXPECT_EQ(got[0].key, std::string{"koi/src/keymap.cpp"});
  }
  for (int i = 0; i < 4; ++i) {
    fix.ed.project->RecordQueryAccept("key", "koi/src/keylog.cpp");
  }
  {
    const SmartCorpus corpus = fix.Snapshot();
    const std::vector<SmartMatch> got = fix.Rank(corpus, "key");
    EXPECT_TRUE(!got.empty());
    if (got.empty()) return;
    EXPECT_EQ(got[0].key, std::string{"koi/src/keylog.cpp"});
  }
  SmartJumpSubmit(fix.ed, "key");
  EXPECT_FALSE(IsExcerptView(fix.ed.doc));
  EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keylog.cpp"});

  TEST_CASE("smart jump: the prior cannot cross a band");
  {
    // tooey is confirmed to the asymptote against a query it matches two whole
    // bands worse than keymap does, and it still cannot climb. The priors sum
    // to less than one band by construction (fuzzy.h), and this is that
    // property with a store behind it.
    for (int i = 0; i < 40; ++i) {
      fix.ed.project->RecordQueryAccept("key", "koi/src/tooey.cpp");
    }
    const SmartCorpus corpus = fix.Snapshot();
    const std::vector<SmartMatch> got = fix.Rank(corpus, "key");
    EXPECT_TRUE(!got.empty());
    if (got.empty()) return;
    // keylog carries its own confirmations from above; the property under test
    // is only that tooey, two bands down, cannot climb however confirmed.
    EXPECT_TRUE(got[0].key != std::string{"koi/src/tooey.cpp"});
    EXPECT_EQ(got.back().key, std::string{"koi/src/tooey.cpp"});
  }
}

void SmartJumpStepping() {
  TEST_CASE("smart jump: next and prev walk the last query's list, and wrap");

  // Driven through picker_jump_next/_prev rather than SmartJumpStep itself:
  // with no picker walk standing, the dispatch is smart-jump's stepping, and
  // that is the half of the contract these cases are about.
  Fixture fix{"koi-smartjump-step"};
  if (fix.ed.project == nullptr) return;
  // Nothing yet.
  PickerJumpStep(fix.ed, true);
  EXPECT_TRUE(fix.ed.status.text().find("no smart jump") != std::string::npos);

  // A picker walk standing, and a submit made after it: the submit's list wins
  // and the walk is freed, the other half of the handoff.
  fix.ed.walk = std::make_shared<WalkList>();
  fix.ed.walk->rows.push_back(WalkRow{"a.txt", "a.txt", {}, 1, 1});

  // Pinned to paths, so the list is three files and the walk below is about the
  // walk rather than about what a bare `key` also reaches.
  SmartJumpSubmit(fix.ed, "f key");
  EXPECT_TRUE(fix.ed.walk == nullptr);
  // Enter landed match 1 of 3; the list survives the landing for stepping.
  EXPECT_FALSE(IsExcerptView(fix.ed.doc));
  EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keymap.cpp"});
  EXPECT_EQ(fix.ed.smart_jump->matches.size(), std::size_t{3});
  // What the status names is where one more press goes, not the row under the
  // cursor: match 2, from a landing on match 1. The destination alone carries
  // the highlight mark, so the bar can dress it apart from the file name.
  EXPECT_TRUE(fix.ed.status.text().starts_with("ᛃ 1/3  koi/src/keylog.cpp"));
  EXPECT_TRUE(fix.ed.status.target().starts_with("koi/src/keylog.cpp"));
  // An arrival hangs its feedback off the caret rather than the bar.
  EXPECT_TRUE(fix.ed.jump_branch);

  PickerJumpStep(fix.ed, true);
  EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keylog.cpp"});
  EXPECT_TRUE(fix.ed.status.text().starts_with("ᛃ 2/3  koi/src/tooey.cpp"));

  PickerJumpStep(fix.ed, true);
  EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"tooey.cpp"});
  // The end of the list, said as what the next press costs. The wrap words
  // stay outside the mark.
  EXPECT_TRUE(fix.ed.status.text().starts_with("ᛃ 3/3  wraps to koi/src/keymap.cpp"));
  EXPECT_TRUE(fix.ed.status.target().starts_with("koi/src/keymap.cpp"));

  PickerJumpStep(fix.ed, true);
  EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keymap.cpp"});
  EXPECT_TRUE(fix.ed.status.text().starts_with("ᛃ 1/3  koi/src/keylog.cpp"));
  EXPECT_TRUE(fix.ed.status.text().find("wrapped to the top") != std::string::npos);

  PickerJumpStep(fix.ed, false);
  EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"tooey.cpp"});
  // Direction-aware: prev from the last row names the row behind it, which is
  // the one prev would give again.
  EXPECT_TRUE(fix.ed.status.text().starts_with("ᛃ 3/3  koi/src/keylog.cpp"));
  EXPECT_TRUE(fix.ed.status.text().find("wrapped to the bottom") != std::string::npos);

  // And prev from the top wraps the other way.
  PickerJumpStep(fix.ed, false);
  PickerJumpStep(fix.ed, false);
  EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keymap.cpp"});
  EXPECT_TRUE(fix.ed.status.text().starts_with("ᛃ 1/3  wraps to koi/src/tooey.cpp"));

  // A new query replaces the list outright.
  SmartJumpSubmit(fix.ed, "piece");
  EXPECT_EQ(fix.ed.smart_jump->matches.size(), std::size_t{1});
  PickerJumpStep(fix.ed, true);
  EXPECT_TRUE(fix.ed.status.text().starts_with("ᛃ 1/1"));

  // The branch lives for exactly one decision: the first key that is not a
  // step buries it, message and all.
  EXPECT_TRUE(fix.ed.jump_branch);
  {
    PressKey press;
    press(fix.ed, "esc");
  }
  EXPECT_FALSE(fix.ed.jump_branch);
  EXPECT_TRUE(fix.ed.status.empty());

  TEST_CASE("smart jump: the next destination's line is healed too");
  {
    Fixture heal{"koi-smartjump-next-line"};
    if (heal.ed.project == nullptr) return;
    // A second visited line in the same file. Landing on the first leaves the
    // second in the open buffer, where the shadow knows where it has drifted
    // to -- and the status has to name that, not the stored line.
    SeedLocation(heal.db, "koi/src/piece_tree.cpp", 250, "SplitNodeTwo(&node);", 1, 0,
                 Now() - kHour);

    SmartJumpSubmit(heal.ed, "c splitnode");
    EXPECT_EQ(heal.ed.doc.file.filename().string(), std::string{"piece_tree.cpp"});
    EXPECT_TRUE(heal.ed.status.text().starts_with("ᛃ 1/2  koi/src/piece_tree.cpp:250"));

    Edit edit;
    ExpectOk(Insert("one\ntwo\nthree\n", LineStart(heal.ed.doc.table, 0), heal.ed.doc.table, &edit),
             "three lines above both anchors");

    SmartJumpSubmit(heal.ed, "c splitnode");
    EXPECT_TRUE(heal.ed.status.text().starts_with("ᛃ 1/2  koi/src/piece_tree.cpp:253"));
  }

  TEST_CASE("smart jump: a dead end takes the list with it");
  // A query that matched nothing went to a picker, and there is nothing to
  // step through afterwards -- stepping the query before last is not what was
  // asked for.
  SmartJumpSubmit(fix.ed, "zqxvw");
  EXPECT_TRUE(fix.ed.smart_jump->matches.empty());
  EXPECT_TRUE(fix.ed.smart_jump->typed.empty());
  PickerJumpStep(fix.ed, true);
  EXPECT_TRUE(fix.ed.status.text().find("no smart jump") != std::string::npos);
}

void SmartJumpTypedCommand() {
  TEST_CASE("smart jump: :smart-jump lands where the prompt's enter would");
  {
    // Two stores of the same shape, one query each, one door each: whatever the
    // prompt does with enter is the whole contract of the typed command.
    Fixture prompt{"koi-smartjump-cmd-prompt"};
    Fixture typed{"koi-smartjump-cmd-typed"};
    if ((prompt.ed.project == nullptr) || (typed.ed.project == nullptr)) return;
    std::string error;
    prompt.ed.jumps = JumpStore::Open(prompt.ed.project, "pane", error);
    typed.ed.jumps = JumpStore::Open(typed.ed.project, "pane", error);

    SmartJumpPrompt(prompt.ed);
    PromptInsert(prompt.ed, "key cpp");
    PromptSubmit(prompt.ed);

    RunTypableCommand(typed.ed, "smart-jump key cpp");

    EXPECT_EQ(typed.ed.doc.file.filename().string(), prompt.ed.doc.file.filename().string());
    EXPECT_EQ(typed.ed.doc.file.filename().string(), std::string{"keymap.cpp"});
    EXPECT_EQ(Cur(typed.ed), Cur(prompt.ed));
    EXPECT_EQ(typed.ed.status.text(), prompt.ed.status.text());
    EXPECT_EQ(typed.ed.smart_jump->matches.size(), prompt.ed.smart_jump->matches.size());
    // The bounce is armed the same way, so the credit that follows it is the
    // same credit.
    EXPECT_EQ(typed.ed.record.pending_typed, std::string{"key cpp"});
    EXPECT_EQ(typed.ed.record.pending_target, prompt.ed.record.pending_target);
    EXPECT_EQ(typed.ed.record.pending_target, std::string{"koi/src/keymap.cpp"});
    // Two prompts, two histories: the query went in at the `:` prompt, and the
    // smart-jump prompt's own history is no place for it.
    EXPECT_TRUE(typed.ed.jump_history.empty());
    EXPECT_EQ(prompt.ed.jump_history.size(), std::size_t{1});

    // Nothing until the arrival is stood in, then a row per progressive prefix,
    // exactly as the prompt's landing writes them.
    EXPECT_EQ(Scalar(typed.db, "SELECT COUNT(*) FROM queries;"), std::int64_t{0});
    typed.ed.record.pending_since -= kBounceSeconds + 1;
    NoteInputBoundary(typed.ed);
    EXPECT_EQ(Scalar(typed.db, "SELECT COUNT(*) FROM queries;"), std::int64_t{2});
    EXPECT_EQ(Scalar(typed.db, "SELECT COUNT(*) FROM queries WHERE prefix='key'"
                               " AND target='koi/src/keymap.cpp';"),
              std::int64_t{1});
    EXPECT_EQ(Scalar(typed.db, "SELECT COUNT(*) FROM queries WHERE prefix='key cpp'"
                               " AND target='koi/src/keymap.cpp';"),
              std::int64_t{1});
  }

  TEST_CASE("smart jump: a typed query leaves the same steppable list");
  {
    Fixture fix{"koi-smartjump-cmd-step"};
    if (fix.ed.project == nullptr) return;

    RunTypableCommand(fix.ed, "smart-jump f key");
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keymap.cpp"});
    EXPECT_EQ(fix.ed.smart_jump->matches.size(), std::size_t{3});
    EXPECT_TRUE(fix.ed.status.text().starts_with("ᛃ 1/3  koi/src/keylog.cpp"));
    EXPECT_TRUE(fix.ed.jump_branch);

    PickerJumpStep(fix.ed, true);
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keylog.cpp"});
    EXPECT_TRUE(fix.ed.status.text().starts_with("ᛃ 2/3  koi/src/tooey.cpp"));

    // And one match lands in silence, the same as it does from the prompt.
    RunTypableCommand(fix.ed, "smart-jump piece");
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"piece_tree.cpp"});
    EXPECT_EQ(fix.ed.smart_jump->matches.size(), std::size_t{1});
    EXPECT_TRUE(fix.ed.status.empty());
    EXPECT_FALSE(fix.ed.jump_branch);
  }

  TEST_CASE("smart jump: a typed query with no matches takes the same dead end");
  {
    Fixture fix{"koi-smartjump-cmd-deadend"};
    if (fix.ed.project == nullptr) return;
    const fs::path stranger = fix.Write("zqxvw_notes.txt", "x\n");
    fix.ed.settings.file_filter = "printf '%s\\n' " + stranger.string();
    const std::string was = fix.ed.doc.file.string();

    RunTypableCommand(fix.ed, "smart-jump f zqxvw");
    EXPECT_TRUE(fix.ed.status.text().find("not been there -- file picker") != std::string::npos);
    EXPECT_TRUE(fix.ed.prompt_active);
    EXPECT_TRUE(fix.ed.prompt_kind == PromptKind::kPicker);
    EXPECT_EQ(fix.ed.prompt_input, std::string{"zqxvw"});
    EXPECT_EQ(fix.ed.doc.file.string(), was);
    PromptCancel(fix.ed);
  }

  TEST_CASE("smart jump: a typed query that does not parse warns and goes nowhere");
  {
    Fixture fix{"koi-smartjump-cmd-parse"};
    if (fix.ed.project == nullptr) return;
    const std::string was = fix.ed.doc.file.string();

    RunTypableCommand(fix.ed, "smart-jump z key");
    EXPECT_TRUE(fix.ed.status.text().find("is not a clause") != std::string::npos);
    EXPECT_EQ(fix.ed.doc.file.string(), was);
    EXPECT_FALSE(fix.ed.prompt_active);
    EXPECT_TRUE(!fix.ed.smart_jump || fix.ed.smart_jump->matches.empty());
  }

  TEST_CASE("smart jump: a typed query ranks against a fresh snapshot");
  {
    Fixture fix{"koi-smartjump-cmd-fresh"};
    if (fix.ed.project == nullptr) return;
    // A prompt session, which takes a snapshot and leaves it behind.
    SmartJumpPrompt(fix.ed);
    PromptInsert(fix.ed, "key");
    PromptCancel(fix.ed);

    // The store moves on afterwards: a place visited since is a row that
    // snapshot does not have.
    fix.Write("koi/src/watch.cpp", NumberedLines(40));
    SeedFile(fix.db, "koi/src/watch.cpp", 20, 0, Now() - kHour);
    SeedLocation(fix.db, "koi/src/watch.cpp", 12, "if (disk_deadline)", 3, 0, Now() - kHour);

    RunTypableCommand(fix.ed, "smart-jump c disk_deadline");
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"watch.cpp"});
    EXPECT_EQ(fix.ed.smart_jump->matches.size(), std::size_t{1});
  }

  TEST_CASE("smart jump: a key bound to :smart-jump runs the query");
  {
    Fixture fix{"koi-smartjump-cmd-bound"};
    if (fix.ed.project == nullptr) return;
    constexpr std::string_view kBound = R"TOML(
[keys.normal."space"]
g = ":smart-jump piece"
)TOML";
    PressKey press;
    std::vector<std::string> errors;
    std::ignore = ParseKeyMapConfig(kBound, press.maps, errors);
    EXPECT_TRUE(errors.empty());

    press(fix.ed, "space");
    press(fix.ed, "g");
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"piece_tree.cpp"});
    EXPECT_EQ(fix.ed.record.pending_typed, std::string{"piece"});
  }

  TEST_CASE("smart jump: :smart-jump with nothing to run says so");
  {
    Fixture fix{"koi-smartjump-cmd-blank"};
    if (fix.ed.project == nullptr) return;
    const std::string was = fix.ed.doc.file.string();

    RunTypableCommand(fix.ed, "smart-jump");
    EXPECT_TRUE(fix.ed.status.text().find(":smart-jump wants a query") != std::string::npos);
    EXPECT_EQ(fix.ed.doc.file.string(), was);
    EXPECT_FALSE(fix.ed.prompt_active);

    // Spaces are not a query either.
    fix.ed.status.clear();
    RunTypableCommand(fix.ed, "smart-jump   ");
    EXPECT_TRUE(fix.ed.status.text().find(":smart-jump wants a query") != std::string::npos);
    EXPECT_EQ(fix.ed.doc.file.string(), was);

    // And no project is no corpus, the same refusal the prompt makes.
    Editor bare;
    RunTypableCommand(bare, "smart-jump key");
    EXPECT_TRUE(bare.status.text().find("no project database") != std::string::npos);
  }
}

void SmartJumpBounceRule() {
  TEST_CASE("smart jump: an arrival abandoned inside two seconds records nothing");

  Fixture fix{"koi-smartjump-bounce"};
  if (fix.ed.project == nullptr) return;
  const auto rows_in = [&fix](std::string_view key) {
    return Scalar(fix.db, "SELECT COUNT(*) FROM locations WHERE path=?;", key);
  };
  const std::int64_t before = rows_in("koi/src/piece_tree.cpp");

  SmartJumpSubmit(fix.ed, "piece");
  EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"piece_tree.cpp"});
  EXPECT_TRUE(fix.ed.record.pending);
  // Arrived, and nothing written for it.
  EXPECT_EQ(rows_in("koi/src/piece_tree.cpp"), before);

  // A boundary inside the window leaves it armed and still unwritten.
  NoteCommandBoundary(fix.ed);
  NoteInputBoundary(fix.ed);
  EXPECT_TRUE(fix.ed.record.pending);
  EXPECT_EQ(rows_in("koi/src/piece_tree.cpp"), before);

  // Walking away cancels it: nothing is recorded, then or later.
  const Index away = LineStart(fix.ed.doc.table, 250);
  fix.ed.doc.selections.Set(MinWidth1(fix.ed.doc.table, Selection{away, away, -1}));
  NoteCommandBoundary(fix.ed);
  EXPECT_FALSE(fix.ed.record.pending);
  EXPECT_EQ(rows_in("koi/src/piece_tree.cpp"), before);

  TEST_CASE("smart jump: two seconds standing in it is what buys the row");
  {
    const std::int64_t was = rows_in("koi/src/keymap.cpp");
    SmartJumpSubmit(fix.ed, "keym");
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keymap.cpp"});
    EXPECT_TRUE(fix.ed.record.pending);
    EXPECT_EQ(rows_in("koi/src/keymap.cpp"), was);

    // Reached by ageing the arrival rather than by waiting for it.
    fix.ed.record.pending_since -= kBounceSeconds + 1;
    NoteInputBoundary(fix.ed);
    EXPECT_FALSE(fix.ed.record.pending);
    EXPECT_EQ(rows_in("koi/src/keymap.cpp"), was + 1);

    // And once, not once per boundary after it.
    NoteInputBoundary(fix.ed);
    NoteCommandBoundary(fix.ed);
    EXPECT_EQ(rows_in("koi/src/keymap.cpp"), was + 1);
  }

  TEST_CASE("smart jump: an edit at the target does not wait out the clock");
  {
    const std::int64_t was = rows_in("koi/src/keylog.cpp");
    SmartJumpSubmit(fix.ed, "keyl");
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keylog.cpp"});
    EXPECT_TRUE(fix.ed.record.pending);
    EXPECT_EQ(rows_in("koi/src/keylog.cpp"), was);

    TypeInto(fix.ed, 'x');
    NoteCommandBoundary(fix.ed);
    EXPECT_FALSE(fix.ed.record.pending);
    EXPECT_EQ(rows_in("koi/src/keylog.cpp"), was + 1);
    // An edit row, not a visit: where you edited out-signals where you looked.
    EXPECT_EQ(Scalar(fix.db, "SELECT kind FROM locations WHERE path='koi/src/keylog.cpp';"),
              std::int64_t{1});
  }
}

// The four ways an unconfirmed arrival used to get itself recorded anyway: by
// being jumped away from, by landing past the end of the file, by one command
// that edits and moves at once, and by the prompt being reopened over it.
void SmartJumpArrivalRules() {
  TEST_CASE("smart jump: stepping past an arrival leaves it out of the corpus");
  {
    Fixture fix{"koi-smartjump-stepped-past"};
    if (fix.ed.project == nullptr) return;
    std::string error;
    fix.ed.jumps = JumpStore::Open(fix.ed.project, "pane", error);
    EXPECT_TRUE(fix.ed.jumps != nullptr);
    const auto described = [&fix](std::string_view key) {
      return Scalar(fix.db,
                    "SELECT COUNT(*) FROM locations WHERE path=? AND line=1"
                    " AND content IS NOT NULL;",
                    key);
    };

    SmartJumpSubmit(fix.ed, "key");
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keymap.cpp"});
    EXPECT_TRUE(fix.ed.record.pending);

    // The step away is a jump, and a jump records the place it leaves. That
    // place is the arrival the step is rejecting, so what goes in is the
    // position and nothing else -- no content, and so nothing the corpus reads.
    SmartJumpStep(fix.ed, true);
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keylog.cpp"});
    EXPECT_EQ(described("koi/src/keymap.cpp"), std::int64_t{0});
    EXPECT_EQ(Scalar(fix.db,
                     "SELECT COUNT(*) FROM locations WHERE path='koi/src/keymap.cpp' AND line=1;"),
              std::int64_t{1});
    {
      const SmartCorpus corpus = fix.Snapshot();
      std::int64_t seen = 0;
      for (const SmartRow& row : corpus.locations) {
        if ((row.file_key == "koi/src/keymap.cpp") && (row.line == 1)) ++seen;
      }
      EXPECT_EQ(seen, std::int64_t{0});
    }

    // The position is there for the list's sake, and stepping back proves it:
    // jump_backward returns to the place the step left.
    StepJump(fix.ed, false);
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keymap.cpp"});

    TEST_CASE("smart jump: standing in the arrival is what puts it in the corpus");
    // Same landing, stood in this time.
    SmartJumpSubmit(fix.ed, "keyl");
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keylog.cpp"});
    fix.ed.record.pending_since -= kBounceSeconds + 1;
    NoteInputBoundary(fix.ed);
    EXPECT_FALSE(fix.ed.record.pending);
    EXPECT_EQ(described("koi/src/keylog.cpp"), std::int64_t{1});
  }

  TEST_CASE("smart jump: a stored line past the end still records where it lands");
  {
    Fixture fix{"koi-smartjump-past-eof"};
    if (fix.ed.project == nullptr) return;
    // keylog.cpp is two lines long; the row remembers line 500, which is what a
    // file truncated between sessions leaves behind.
    fix.Write("koi/src/keylog.cpp", "// keylog\n// and a second line");
    SeedLocation(fix.db, "koi/src/keylog.cpp", 500, "farpastend_marker;", 6, 0, Now() - kHour);

    SmartJumpSubmit(fix.ed, "c farpastend");
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keylog.cpp"});
    EXPECT_TRUE(fix.ed.record.pending);
    // Armed where the cursor is, not where the row said: otherwise the clamp
    // alone is a 499-line disparity and the next boundary cancels the arrival.
    EXPECT_EQ(fix.ed.record.pending_line,
              LineAt(fix.ed.doc.table,
                     CursorOf(fix.ed.doc.table, fix.ed.doc.selections.Primary())) +
                  1);
    NoteCommandBoundary(fix.ed);
    EXPECT_TRUE(fix.ed.record.pending);

    fix.ed.record.pending_since -= kBounceSeconds + 1;
    NoteInputBoundary(fix.ed);
    EXPECT_FALSE(fix.ed.record.pending);
    EXPECT_EQ(Scalar(fix.db, "SELECT COUNT(*) FROM locations WHERE path='koi/src/keylog.cpp'"
                             " AND line<=2 AND content IS NOT NULL;"),
              std::int64_t{1});
    EXPECT_TRUE(Scalar(fix.db, "SELECT COUNT(*) FROM queries WHERE target LIKE"
                               " 'koi/src/keylog.cpp@%';") > 0);
  }

  TEST_CASE("smart jump: an edit that moves the cursor away is not an edit at the arrival");
  {
    Fixture fix{"koi-smartjump-edit-away"};
    if (fix.ed.project == nullptr) return;
    const std::int64_t before =
        Scalar(fix.db, "SELECT COUNT(*) FROM locations WHERE path='koi/src/piece_tree.cpp';");

    SmartJumpSubmit(fix.ed, "piece");
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"piece_tree.cpp"});
    EXPECT_TRUE(fix.ed.record.pending);

    // One turn that edits and then leaves -- undo at a distance, a multi-line
    // paste, a bound list of commands all look like this from here.
    TypeInto(fix.ed, 'x');
    const Index away = LineStart(fix.ed.doc.table, 250);
    fix.ed.doc.selections.Set(MinWidth1(fix.ed.doc.table, Selection{away, away, -1}));
    NoteCommandBoundary(fix.ed);

    EXPECT_FALSE(fix.ed.record.pending);
    EXPECT_EQ(Scalar(fix.db, "SELECT COUNT(*) FROM locations WHERE path='koi/src/piece_tree.cpp';"),
              before);
    EXPECT_EQ(Scalar(fix.db, "SELECT COUNT(*) FROM queries WHERE prefix='piece';"),
              std::int64_t{0});
  }

  TEST_CASE("smart jump: reopening the prompt abandons the arrival it is correcting");
  {
    Fixture fix{"koi-smartjump-retype"};
    if (fix.ed.project == nullptr) return;
    const auto rows_in = [&fix](std::string_view key) {
      return Scalar(fix.db, "SELECT COUNT(*) FROM locations WHERE path=?;", key);
    };
    const std::int64_t before = rows_in("koi/src/keymap.cpp");

    SmartJumpSubmit(fix.ed, "keym");
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keymap.cpp"});
    EXPECT_TRUE(fix.ed.record.pending);

    // The correction: the prompt comes back before the window is out. Prompt
    // keys move no cursor, so nothing else would ever cancel this arrival.
    SmartJumpPrompt(fix.ed);
    EXPECT_FALSE(fix.ed.record.pending);
    fix.ed.record.pending_since -= kBounceSeconds + 1;
    NoteInputBoundary(fix.ed);
    NoteCommandBoundary(fix.ed);
    EXPECT_EQ(rows_in("koi/src/keymap.cpp"), before);
    EXPECT_EQ(Scalar(fix.db, "SELECT COUNT(*) FROM queries WHERE prefix='keym';"),
              std::int64_t{0});
    PromptCancel(fix.ed);

    // An arrival that had already earned its row is not taken back: the
    // boundary on the keystroke that opens the prompt runs first, and it is
    // that boundary the confirmation belongs to.
    SmartJumpSubmit(fix.ed, "keyl");
    EXPECT_EQ(fix.ed.doc.file.filename().string(), std::string{"keylog.cpp"});
    fix.ed.record.pending_since -= kBounceSeconds + 1;
    NoteInputBoundary(fix.ed);
    SmartJumpPrompt(fix.ed);
    EXPECT_EQ(rows_in("koi/src/keylog.cpp"), std::int64_t{1});
    EXPECT_TRUE(Scalar(fix.db, "SELECT COUNT(*) FROM queries WHERE prefix='keyl';") > 0);
    PromptCancel(fix.ed);
  }
}

namespace {

// -- clause-free matching -------------------------------------------------------
//
// A corpus of its own, because these are about routing and not about the doc's
// worked example: two files with the same basename, a definition whose name is
// also in its own path, and stored lines that answer some terms by their text
// and others only by the file they sit in.
struct ModelessFixture {
  Scratch scratch;
  AsProjectRoot root;
  Editor ed;
  sqlite3* db{nullptr};

  // The keys, spelled once. Every expectation below names a row by one of
  // these, and a typo in one is a test that passes by matching nothing.
  static constexpr std::string_view kKeymap = "koi/src/keymap.cpp";
  static constexpr std::string_view kPiece = "koi/src/piece_tree.cpp";
  static constexpr std::string_view kWatch = "koi/src/watch.cpp";
  static constexpr std::string_view kWatchTest = "koi/src/tests/watch.cpp";
  static constexpr std::string_view kRender = "koi/src/tests/render.cpp";
  static constexpr std::string_view kRenderLine = "  const Surface render = Draw(ed);";

  explicit ModelessFixture(std::string_view name) : scratch{name}, root{scratch.dir} {
    std::error_code ec;
    fs::create_directories(scratch.dir / "koi" / "src" / "tests", ec);
    for (const std::string_view key : {kKeymap, kPiece, kWatch, kWatchTest, kRender}) {
      WriteFixtureFile(scratch.dir / key, NumberedLines(700));
    }

    ed.theme = BuiltinTheme();
    std::string error;
    ed.project = ProjectStore::Open(scratch.dir / "state.db", error);
    EXPECT_TRUE(ed.project != nullptr);
    if (ed.project == nullptr) return;
    db = ed.project->Connection();

    const double old = Now() - (2 * kHour);
    SeedFile(db, kKeymap, 343, 0, old);
    SeedFile(db, kPiece, 140, 0, old);
    SeedFile(db, kWatch, 80, 0, old);
    SeedFile(db, kWatchTest, 20, 0, old);
    // Weightless on purpose: with no frecency behind it a row's score IS its
    // banded match, which is what the max-not-sum case does arithmetic on.
    SeedFile(db, kRender, 0, 0, old);

    SeedSymbol(db, kWatch, "disk_deadline", 12, 4, old);
    SeedSymbol(db, kRender, "render", 88, 1, old);

    SeedLocation(db, kPiece, 212, "SplitNode(&node);", 4, 0, old);
    SeedLocation(db, kPiece, 250, "SplitNodeTwo(&node);", 1, 0, old);
    SeedLocation(db, kKeymap, 100, "void SplitBinding();", 1, 0, old);
    SeedLocation(db, kKeymap, 640, "if (key.mods == kModAlt)", 2, 0, old);
    SeedLocation(db, kWatch, 12, "if (now > disk_deadline) {", 3, 0, old);
    SeedLocation(db, kWatchTest, 44, "EXPECT_TRUE(disk_deadline > 0);", 1, 0, old);
    SeedLocation(db, kRender, 88, kRenderLine, 0, 0, old);
  }

  SmartCorpus Snapshot() const {
    SmartCorpus corpus;
    BuildSmartCorpus(*ed.project, corpus);
    return corpus;
  }

  // Ranked with no store, so nothing here depends on the adaptive prior.
  std::vector<SmartMatch> Rank(const SmartCorpus& corpus, std::string_view text) const {
    return RankSmartMatches(corpus, ParseSmartQuery(text), nullptr);
  }
};

// Where `key` sits in the ranking, or npos. "no worse bare than pinned" is two
// of these compared.
std::size_t RankOf(const std::vector<SmartMatch>& got, std::string_view key) {
  for (std::size_t at = 0; at < got.size(); ++at) {
    if (got[at].key == key) return at;
  }
  return std::string::npos;
}

std::size_t CountKind(const std::vector<SmartMatch>& got, SmartKind kind) {
  std::size_t count = 0;
  for (const SmartMatch& one : got) {
    if (one.kind == kind) ++count;
  }
  return count;
}

// The whole ranking as one line -- every key with its score to six places --
// which is what "identical to the clause pipeline" is compared as.
std::string Ranking(const std::vector<SmartMatch>& got) {
  std::string out;
  for (const SmartMatch& one : got) {
    char digits[32];
    std::snprintf(digits, sizeof(digits), "%.6f", one.score);
    if (!out.empty()) out += ' ';
    out += one.key;
    out += '@';
    out += digits;
  }
  return out;
}

}  // namespace

void SmartJumpModeless() {
  TEST_CASE("smart jump: a bare term routes itself by what it matched");

  ModelessFixture fix{"koi-smartjump-modeless"};
  if (fix.ed.project == nullptr) return;
  const SmartCorpus corpus = fix.Snapshot();
  EXPECT_EQ(corpus.files.size(), std::size_t{5});
  EXPECT_EQ(corpus.symbols.size(), std::size_t{2});
  EXPECT_EQ(corpus.locations.size(), std::size_t{7});

  {
    // 1. A name that is a file's and nothing else's: the file row, and none of
    // the lines that merely live in it. (†) is the whole of the reason -- a row
    // found only through its path is one its file row already stands for.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "keymap");
    EXPECT_EQ(got.size(), std::size_t{1});
    if (got.empty()) return;
    EXPECT_EQ(got[0].key, std::string{ModelessFixture::kKeymap});
    EXPECT_TRUE(got[0].kind == SmartKind::kFile);
    EXPECT_EQ(CountKind(got, SmartKind::kLocation), std::size_t{0});
  }

  {
    // 2. And the other way about: a name that only ever appears inside lines
    // gets the lines, and the file they are in is not a candidate at all.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "splitnode");
    EXPECT_EQ(got.size(), std::size_t{2});
    EXPECT_EQ(CountKind(got, SmartKind::kLocation), std::size_t{2});
    EXPECT_TRUE(RankOf(got, ModelessFixture::kPiece) == std::string::npos);
    if (got.empty()) return;
    EXPECT_TRUE(got[0].display.find("piece_tree.cpp:212") != std::string::npos);
  }

  {
    // 3. Two bare terms landing on different facets of the same row: the file by
    // its path, the line by its text. piece_tree's split lines fail `key` on
    // both facets and never reach the list -- and the keyworded spelling of the
    // query is the same query, to the score.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "key split");
    EXPECT_EQ(got.size(), std::size_t{1});
    if (got.empty()) return;
    EXPECT_TRUE(got[0].kind == SmartKind::kLocation);
    EXPECT_TRUE(got[0].display.find("keymap.cpp:100") != std::string::npos);
    EXPECT_EQ(Ranking(fix.Rank(corpus, "key c split")), Ranking(got));
  }

  {
    // 4. A definition and the lines that mention it, in one list: the name is
    // the whole of the definition's text, so it takes band 4, and no pile of
    // priors moves a band-2 line over it.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "disk_deadline");
    EXPECT_EQ(got.size(), std::size_t{3});
    EXPECT_EQ(CountKind(got, SmartKind::kSymbol), std::size_t{1});
    EXPECT_EQ(CountKind(got, SmartKind::kLocation), std::size_t{2});
    if (got.size() < 2) return;
    EXPECT_TRUE(got[0].kind == SmartKind::kSymbol);
    EXPECT_EQ(got[0].symbol, std::string{"disk_deadline"});
    EXPECT_TRUE((got[0].score - got[1].score) > 1.0);
  }

  {
    // 5. Two files with the same basename, and the lines inside them left where
    // they are: neither term is in either line's own text.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "watch cpp");
    EXPECT_EQ(got.size(), std::size_t{2});
    EXPECT_EQ(CountKind(got, SmartKind::kLocation), std::size_t{0});
    EXPECT_EQ(CountKind(got, SmartKind::kSymbol), std::size_t{0});
    EXPECT_TRUE(RankOf(got, ModelessFixture::kWatchTest) != std::string::npos);
    if (got.empty()) return;
    // The heavier of the twins leads: same band, same fuzzy, frecency decides.
    EXPECT_EQ(got[0].key, std::string{ModelessFixture::kWatch});
  }

  {
    // 6. The ambiguous one, which is the case the keywords used to exist for.
    // `key` is a file's name and a word inside one of its lines, and both rows
    // are candidates. Their banded match is the same number -- the line's better
    // facet is that same path -- so the priors decide, and the file has 686 of
    // frecency against the line's 4.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "key");
    EXPECT_EQ(got.size(), std::size_t{2});
    if (got.size() != 2) return;
    EXPECT_TRUE(got[0].kind == SmartKind::kFile);
    EXPECT_EQ(got[0].key, std::string{ModelessFixture::kKeymap});
    EXPECT_TRUE(got[1].kind == SmartKind::kLocation);
    EXPECT_TRUE(got[1].display.find("keymap.cpp:640") != std::string::npos);
    // 0.2 * 686/716 - 0.2 * 4/34 = 0.16809, and nothing else separates them.
    EXPECT_TRUE(std::abs((got[0].score - got[1].score) - 0.16809) < 1e-3);
  }

  {
    // 7. Digits are unchanged: they name a line, so the other two kinds are out
    // and the nearest visited line per file is what is left of the rest.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "key 640");
    EXPECT_EQ(got.size(), std::size_t{1});
    if (got.empty()) return;
    EXPECT_TRUE(got[0].kind == SmartKind::kLocation);
    EXPECT_EQ(got[0].line, Index{640});
    // The number carries the content evidence by itself, so a line in the file
    // that no term reached is still eligible to be the nearest one.
    const std::vector<SmartMatch> near_top = fix.Rank(corpus, "key 90");
    EXPECT_EQ(near_top.size(), std::size_t{1});
    if (near_top.empty()) return;
    EXPECT_EQ(near_top[0].line, Index{100});
  }

  {
    // 9. A dead end is a dead end, and the door it opens takes the whole query.
    EXPECT_TRUE(fix.Rank(corpus, "zqx").empty());
    const SmartHandoff handoff = SmartJumpHandoff(ParseSmartQuery("zqx"));
    EXPECT_TRUE(handoff.picker == SmartPicker::kContent);
    EXPECT_EQ(handoff.query, std::string{"zqx"});
    EXPECT_TRUE(SmartDroppedTerms(ParseSmartQuery("zqx"), handoff.picker).empty());
  }

  TEST_CASE("smart jump: a term matching both facets is worth the better one, not both");
  {
    // The row's file is weightless and so is the row, so what comes back IS the
    // banded match and the arithmetic can be done here. `render` is a prefix of
    // the basename (band 3) and a substring of the line (band 2): the row is
    // worth the first of those and not their sum.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "render");
    const std::size_t at = RankOf(got, std::string{ModelessFixture::kRender} + "@7");
    EXPECT_TRUE(at != std::string::npos);
    if (at == std::string::npos) return;
    const std::size_t name_at = ModelessFixture::kRender.find_last_of('/') + 1;
    const FuzzyMatch by_path =
        ScoreBanded("render", ModelessFixture::kRender, name_at, FuzzyConfig::Path());
    const FuzzyMatch by_text =
        ScoreBanded("render", ModelessFixture::kRenderLine, 0, FuzzyConfig::Default());
    EXPECT_TRUE(by_path.matched && by_text.matched);
    EXPECT_EQ(by_path.band, 3);
    EXPECT_EQ(by_text.band, 2);
    EXPECT_TRUE(std::abs(got[at].score - by_path.banded) < 1e-9);
    // ... and nowhere near their total, which is what would let a file whose
    // name is in every line of it outrank the line that actually says something.
    EXPECT_TRUE(((by_path.banded + by_text.banded) - got[at].score) > 2.0);
    // The file row of that same file scores the same, for the same reason: one
    // path, one facet, one number.
    const std::size_t file_at = RankOf(got, ModelessFixture::kRender);
    EXPECT_TRUE(file_at != std::string::npos);
    if (file_at == std::string::npos) return;
    EXPECT_TRUE(std::abs(got[file_at].score - got[at].score) < 1e-9);
  }

  TEST_CASE("smart jump: a row has to have been found by its own text to be offered");
  {
    // A definition whose name is also in its file's path is in by the name: the
    // path facet loses the max, and (†) is a statement about the text facet
    // having matched rather than about its having won.
    const std::vector<SmartMatch> got = fix.Rank(corpus, "render");
    EXPECT_TRUE(!got.empty());
    if (got.empty()) return;
    EXPECT_TRUE(got[0].kind == SmartKind::kSymbol);
    EXPECT_EQ(got[0].symbol, std::string{"render"});

    // A line whose text matches and whose path does not is in on the text
    // alone: there is no path evidence anywhere in this query.
    const std::vector<SmartMatch> lines = fix.Rank(corpus, "disk_deadline");
    EXPECT_EQ(CountKind(lines, SmartKind::kLocation), std::size_t{2});
    EXPECT_TRUE(RankOf(lines, ModelessFixture::kWatch) == std::string::npos);

    // And a query pinned to paths reaches no definition and no line whatever
    // their text says, because nothing in it may look at their text at all.
    for (const std::string_view typed : {"f watch", "f render", "f keymap", "f watch cpp"}) {
      const std::vector<SmartMatch> pinned = fix.Rank(corpus, typed);
      EXPECT_TRUE(!pinned.empty());
      EXPECT_EQ(CountKind(pinned, SmartKind::kSymbol), std::size_t{0});
      EXPECT_EQ(CountKind(pinned, SmartKind::kLocation), std::size_t{0});
    }
  }

  TEST_CASE("smart jump: taking the keywords off never costs the target its place");
  {
    // The de-keyworded spelling of a query has to rank the row the keyworded one
    // was for at least as high. Bare adds rows; it must not push the answer down.
    struct Pair {
      std::string_view pinned;
      std::string_view bare;
      std::string_view target;
    };
    const std::string piece_line = std::string{ModelessFixture::kPiece} + "@1";
    const std::string keymap_line = std::string{ModelessFixture::kKeymap} + "@3";
    const std::string render_def = std::string{ModelessFixture::kRender} + "#render";
    const std::string watch_def = std::string{ModelessFixture::kWatch} + "#disk_deadline";
    for (const Pair& one : {
             Pair{"c splitnode", "splitnode", piece_line},
             Pair{"f key c split", "key split", keymap_line},
             Pair{"d disk_deadline", "disk_deadline", watch_def},
             Pair{"d render", "render", render_def},
             Pair{"f watch cpp", "watch cpp", ModelessFixture::kWatch},
             Pair{"f keymap", "keymap", ModelessFixture::kKeymap},
         }) {
      const std::size_t pinned = RankOf(fix.Rank(corpus, one.pinned), one.target);
      const std::size_t bare = RankOf(fix.Rank(corpus, one.bare), one.target);
      EXPECT_TRUE(pinned != std::string::npos);
      EXPECT_TRUE(bare != std::string::npos);
      if ((pinned == std::string::npos) || (bare == std::string::npos)) continue;
      EXPECT_TRUE(bare <= pinned);
    }
  }

  TEST_CASE("smart jump: equal rows keep their order between one evaluation and the next");
  {
    // The first three keys are all query-dependent; a corpus position is not,
    // and it is what stops two rows tied on score, length and first match
    // trading places between keystrokes.
    EXPECT_FALSE(RanksBefore(RankKey{2.5, 10, 2, 7}, RankKey{2.5, 10, 2, 7}));
    EXPECT_TRUE(RanksBefore(RankKey{2.5, 10, 2, 3}, RankKey{2.5, 10, 2, 7}));
    EXPECT_FALSE(RanksBefore(RankKey{2.5, 10, 2, 7}, RankKey{2.5, 10, 2, 3}));

    const std::string once = Ranking(fix.Rank(corpus, "render"));
    for (int i = 0; i < 4; ++i) EXPECT_EQ(Ranking(fix.Rank(corpus, "render")), once);
  }

  TEST_CASE("smart jump: a fully keyworded query ranks exactly as the clause pipeline did");
  {
    // The literals are the clause pipeline's own output, taken from it before
    // this change went in: keys, order and scores to six places. The spellings
    // differ from the ones it was asked only by the `f` the old parser threw
    // away, so an old ranking of `key cpp` is the new ranking of `f key cpp`, or
    // the equivalence is broken.
    Fixture doc{"koi-smartjump-equivalence"};
    if (doc.ed.project == nullptr) return;
    const SmartCorpus was = doc.Snapshot();
    struct Pinned {
      std::string_view typed;
      std::string_view ranking;
    };
    for (const Pinned& one : {
             Pinned{"f key cpp", "koi/src/keymap.cpp@7.061185 koi/src/keylog.cpp@7.004348"
                                 " koi/src/tooey.cpp@3.776175"},
             Pinned{"c split", "koi/src/piece_tree.cpp@1@4.027820 koi/src/keymap.cpp@3@2.998214"},
             Pinned{"d pin", "koi/src/navigate.cpp#SetPinHere@2.800000"
                             " koi/src/navigate.cpp#JumpToPin@2.773529"},
             Pinned{"f key 640", "koi/src/keymap.cpp@2@4.023529"},
             Pinned{"f nav d pin", "koi/src/navigate.cpp#SetPinHere@6.800000"
                                   " koi/src/navigate.cpp#JumpToPin@6.773529"},
             Pinned{"f key cpp c split", "koi/src/keymap.cpp@3@9.867780"},
             Pinned{"c split f key cpp", "koi/src/keymap.cpp@3@9.867780"},
             Pinned{"f cpp", "koi/src/keymap.cpp@3.061185 koi/src/tooey.cpp@3.058784"
                             " koi/src/piece_tree.cpp@3.050210 koi/src/navigate.cpp@3.040994"
                             " koi/src/keylog.cpp@3.004348"},
             Pinned{"f key", "koi/src/keymap.cpp@4.191620 koi/src/keylog.cpp@4.134783"
                             " koi/src/tooey.cpp@0.906610"},
             Pinned{"c splitnode", "koi/src/piece_tree.cpp@1@4.025712"},
             Pinned{"d look", "koi/src/keymap.cpp#Lookup@3.994956"},
             Pinned{"f key 90", "koi/src/keymap.cpp@3@4.012500"},
             Pinned{"f piece c split", "koi/src/piece_tree.cpp@1@8.027820"},
             Pinned{"f key map cpp", "koi/src/keymap.cpp@9.669881"},
             Pinned{"f nav", "koi/src/navigate.cpp@4.171429"},
         }) {
      EXPECT_EQ(Ranking(RankSmartMatches(was, ParseSmartQuery(one.typed), nullptr)),
                std::string{one.ranking});
    }
  }

  TEST_CASE("smart jump: a mixed band keeps each row's own shape");
  {
    ModelessFixture band{"koi-smartjump-mixed-band"};
    if (band.ed.project == nullptr) return;
    PressKey press;
    press(band.ed, "h");
    for (const std::string_view key : {"r", "e", "n", "d", "e", "r"}) press(band.ed, key);
    EXPECT_TRUE(band.ed.smart_band != nullptr);
    if (band.ed.smart_band == nullptr) return;
    PickerState& rows = *band.ed.smart_band;
    EXPECT_EQ(rows.shown.size(), std::size_t{3});
    if (rows.shown.size() != 3) return;
    // Three kinds in one band, each keeping its own shape: a definition and a
    // line lead with their text and point at a `path:line` in the detail
    // column, and a file row is its path and carries no second copy of it.
    EXPECT_EQ(std::string{PickerRowLead(rows, 1)}, std::string{ModelessFixture::kRender});
    EXPECT_TRUE(PickerRowDetail(rows, 1).empty());
    EXPECT_EQ(std::string{PickerRowLead(rows, 2)}, std::string{ModelessFixture::kRenderLine});
    EXPECT_EQ(std::string{PickerRowDetail(rows, 2)},
              std::string{ModelessFixture::kRender} + ":88");
    EXPECT_TRUE(!PickerRowLead(rows, 0).empty());
    press(band.ed, "esc");
  }
}

void SmartJumpSnapshotCost() {
  TEST_CASE("smart jump: the snapshot and a keystroke both fit their budget");

  Fixture fix{"koi-smartjump-cost"};
  if (fix.ed.project == nullptr) return;

  // Four hundred locations over eighty files, which is the scale the design
  // measured: 78 live files, 130 symbols, ~180 in-project locations.
  std::error_code ec;
  fs::create_directories(fix.scratch.dir / "koi" / "bench", ec);
  const double now = Now();
  ExecSql(fix.db, "BEGIN IMMEDIATE;");
  for (int i = 0; i < 80; ++i) {
    const std::string key = "koi/bench/component_" + std::to_string(i) + "_impl.cpp";
    fix.Write(key, "// bench\n");
    SeedFile(fix.db, key, i + 1, i % 4, now - (i * kHour));
    for (int j = 0; j < 5; ++j) {
      SeedLocation(fix.db, key, (j * 37) + 1,
                   "  ResolveAnchorFor" + std::to_string(j) + "(row, in_place);", 1, j % 2,
                   now - (i * kHour));
    }
    SeedSymbol(fix.db, key, "ResolveComponent" + std::to_string(i), 12, 1, now - (i * kHour));
  }
  ExecSql(fix.db, "COMMIT;");

  SmartCorpus corpus;
  double best_ms = -1;
  for (int pass = 0; pass < 3; ++pass) {
    BuildSmartCorpus(*fix.ed.project, corpus);
    if ((best_ms < 0) || (corpus.build_ms < best_ms)) best_ms = corpus.build_ms;
  }
  EXPECT_EQ(corpus.files.size(), std::size_t{85});
  EXPECT_EQ(corpus.locations.size(), std::size_t{403});
  // One stat per distinct file key: every symbol and every location here lives
  // in a file the store already has a row for, so the sweep touches disk 85
  // times and not 571. This is what the 5ms budget is really a statement about,
  // and unlike a wall clock it says the same thing on every machine.
  EXPECT_EQ(corpus.stats, corpus.files.size());
  std::cout << "smart jump: snapshot of " << corpus.files.size() << " files, "
            << corpus.symbols.size() << " symbols, " << corpus.locations.size()
            << " locations in " << best_ms << "ms, " << corpus.stats << " stats\n";

  // Two shapes, because they cost very different things. `comp c resolve`
  // matches every location in the corpus, so every one of them runs the whole
  // DP; `component_73 c resolve` is what typing actually looks like after two
  // characters, and the gate kills all but a handful for free.
  const auto time_query = [&corpus](std::string_view text) {
    const SmartQuery query = ParseSmartQuery(text);
    long long best = -1;
    std::size_t count = 0;
    for (int pass = 0; pass < 3; ++pass) {
      const auto started = std::chrono::steady_clock::now();
      const std::vector<SmartMatch> got = RankSmartMatches(corpus, query, nullptr);
      const long long us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
      count = got.size();
      if ((best < 0) || (us < best)) best = us;
    }
    std::cout << "smart jump: `" << text << "` -> " << count << " matches in " << best << "us\n";
    return best;
  };

  const long long broad_us = time_query("comp c resolve");
  const long long narrow_us = time_query("component_73 c resolve");
  // The same two shapes with the keywords off, which is what typing looks like
  // now: every term is scored against every facet of every row, so this is the
  // pipeline's worst case for the same corpus and it is held to the same
  // budgets. The path facet is memoized per file key, so the extra work is one
  // DP per file per term and not one per row.
  const long long bare_broad_us = time_query("comp resolve");
  const long long bare_narrow_us = time_query("component_73 resolve");

  // What a keystroke actually runs: the same ranking, with display strings for
  // the band's five survivors and for none of the rest. The whole point of the
  // preview is that it costs the ranking and not the list, so it is held to the
  // same budgets the ranking is.
  const auto time_preview = [&corpus](std::string_view text) {
    const SmartQuery query = ParseSmartQuery(text);
    long long best = -1;
    std::size_t rows = 0;
    for (int pass = 0; pass < 3; ++pass) {
      const auto started = std::chrono::steady_clock::now();
      const SmartPreview got = PreviewSmartMatches(corpus, query, nullptr, kPickerRows);
      const long long us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
      rows = got.top.size();
      if ((best < 0) || (us < best)) best = us;
    }
    EXPECT_EQ(rows, kPickerRows);
    std::cout << "smart jump: `" << text << "` -> band of " << rows << " in " << best << "us\n";
    return best;
  };

  const long long broad_band_us = time_preview("comp c resolve");
  const long long narrow_band_us = time_preview("component_73 c resolve");

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
  // A sanitizer build is optimized and still several times slower; the number
  // measures the instrumentation, not the design. Same loose bound as the
  // unoptimized case.
  EXPECT_TRUE(best_ms < 250.0);
  EXPECT_TRUE(broad_us < 50000);
  EXPECT_TRUE(narrow_us < 10000);
  EXPECT_TRUE(bare_broad_us < 50000);
  EXPECT_TRUE(bare_narrow_us < 10000);
  EXPECT_TRUE(broad_band_us < 50000);
  EXPECT_TRUE(narrow_band_us < 10000);
#elif defined(__OPTIMIZE__)
  // The design budgets are 5ms for the open and 250us for the keystroke, and
  // the printed numbers are what to read them against: about 3ms and about
  // 150us on an idle machine here. The bounds below are those budgets times
  // four, because a builder running this alongside a compile measures the
  // machine and not the code -- 3ms became 5.1ms on one, which failed a bound
  // set at the budget itself. What a regression would do is change the shape --
  // a stat or a DP per row rather than per file -- and that is an order of
  // magnitude, which these still catch. The stat count above catches it exactly
  // and without a clock at all.
  EXPECT_TRUE(best_ms < 20.0);
  // The narrow query is the one the budget is about, and it clears it by an
  // order of magnitude. The broad one is the worst case the design costed out
  // -- every row through the whole DP -- and it does not: fuzzy.cpp's DP runs
  // at about 5ns a cell against the 0.3ns the doc's arithmetic assumed, so a
  // query that matches four hundred rows takes about a millisecond. It is
  // bounded and it is one frame; the place to fix it is the DP, not here.
  EXPECT_TRUE(narrow_us < 1000);
  EXPECT_TRUE(broad_us < 12000);
  // Bare costs more than pinned and is held to the same numbers: a keyword used
  // to let the file clause throw a row out before it was scored, and a bare term
  // has to look at the row's text before it knows.
  EXPECT_TRUE(bare_narrow_us < 1000);
  EXPECT_TRUE(bare_broad_us < 12000);
  // The five strings are a rounding error against the DP: same budgets.
  EXPECT_TRUE(narrow_band_us < 1000);
  EXPECT_TRUE(broad_band_us < 12000);
#else
  EXPECT_TRUE(best_ms < 250.0);
  EXPECT_TRUE(narrow_us < 5000);
  EXPECT_TRUE(broad_us < 40000);
  EXPECT_TRUE(bare_narrow_us < 5000);
  EXPECT_TRUE(bare_broad_us < 40000);
  EXPECT_TRUE(narrow_band_us < 5000);
  EXPECT_TRUE(broad_band_us < 40000);
#endif

  TEST_CASE("smart jump: a corpus at the cap is still one frame");
  {
    // Far past the scale the design measured, and the point is the shape of the
    // curve rather than the number: every row of every kind is now scored
    // against every term, so a per-row path resolve or a per-row file lookup
    // would show up here as quadratic and nowhere else.
    Fixture cap{"koi-smartjump-cap"};
    if (cap.ed.project == nullptr) return;
    std::error_code cap_ec;
    fs::create_directories(cap.scratch.dir / "koi" / "cap", cap_ec);
    const double cap_now = Now();
    constexpr int kCapFiles = 2000;
    ExecSql(cap.db, "BEGIN IMMEDIATE;");
    for (int i = 0; i < kCapFiles; ++i) {
      const std::string key = "koi/cap/module_" + std::to_string(i) + "_driver.cpp";
      cap.Write(key, "// cap\n");
      SeedFile(cap.db, key, (i % 30) + 1, i % 3, cap_now - ((i % 48) * kHour));
      SeedSymbol(cap.db, key, "DriveModule" + std::to_string(i), 40, 1,
                 cap_now - ((i % 48) * kHour));
      // One stored line per file for the first 2000 of them, which is the
      // location table's own design scale.
      SeedLocation(cap.db, key, (i % 400) + 1,
                   "  const Result driven = DriveModule" + std::to_string(i) + "(ctx, opts);", 1,
                   i % 2, cap_now - ((i % 48) * kHour));
    }
    ExecSql(cap.db, "COMMIT;");

    SmartCorpus big;
    BuildSmartCorpus(*cap.ed.project, big);
    EXPECT_EQ(big.files.size(), std::size_t{kCapFiles + 5});
    EXPECT_EQ(big.symbols.size(), std::size_t{kCapFiles + 3});
    EXPECT_EQ(big.locations.size(), std::size_t{kCapFiles + 3});

    const auto time_cap = [&big](std::string_view text) {
      const SmartQuery query = ParseSmartQuery(text);
      long long best = -1;
      std::size_t count = 0;
      for (int pass = 0; pass < 3; ++pass) {
        const auto started = std::chrono::steady_clock::now();
        const SmartPreview got = PreviewSmartMatches(big, query, nullptr, kPickerRows);
        const long long us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        count = got.count;
        if ((best < 0) || (us < best)) best = us;
      }
      std::cout << "smart jump: cap `" << text << "` -> " << count << " matches in " << best
                << "us\n";
      return best;
    };

    // The worst bare query this corpus has -- both terms reach every row of
    // every kind -- and a typed one, which is what a keystroke past the second
    // character actually looks like.
    const long long cap_broad_us = time_cap("module driver");
    const long long cap_typed_us = time_cap("module_1737 drive");

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    EXPECT_TRUE(cap_broad_us < 400000);
    EXPECT_TRUE(cap_typed_us < 200000);
#elif defined(__OPTIMIZE__)
    // Four times what an idle machine measures, for the reason the snapshot
    // bound gives: quadratic is what this test is looking for, and quadratic is
    // not something a busy builder can fake.
    EXPECT_TRUE(cap_broad_us < 60000);
    EXPECT_TRUE(cap_typed_us < 30000);
#else
    EXPECT_TRUE(cap_broad_us < 300000);
    EXPECT_TRUE(cap_typed_us < 150000);
#endif
  }
}

}  // namespace koi
