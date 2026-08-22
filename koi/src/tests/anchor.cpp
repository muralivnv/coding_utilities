// Tests for anchor.cpp: the resolve ladder, the diff and distance it is built
// on, the heal job, and the live shifting that keeps an open buffer's rows true
// between heals.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

#include "subprocess.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

std::string Joined(const std::vector<std::string>& lines) {
  std::string out;
  for (const std::string& line : lines) {
    out += line;
    out += '\n';
  }
  return out;
}

// One line of a synthetic source file. Distinguishable in the way real code is
// and a numbered filler line is not: two neighbours differ in most of their
// characters, so the fuzzy rung cannot mistake one for the other and a test
// about rung 7 is about rung 7 rather than about the fixture.
std::string StepLine(int i) {
  std::uint64_t hash = AnchorLineHash("step-" + std::to_string(i));
  std::string token;
  for (int k = 0; k < 20; ++k) {
    token += static_cast<char>('a' + static_cast<int>(hash % 26));
    hash /= 26;
    if (hash == 0) hash = AnchorLineHash(token);
  }
  return "  call_" + token + "(" + std::to_string(i) + ");";
}

std::vector<std::string> StepLines(int count) {
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) out.push_back(StepLine(i));
  return out;
}

std::string StepContent(int i) { return NormalizeAnchorLine(StepLine(i)); }

AnchorFile FileOf(std::string_view text) {
  AnchorFile out;
  SplitAnchorLines(text, out);
  return out;
}

void PutCursorOnLine(Editor& ed, Index line) {
  const Index at = LineStart(ed.doc.table, line - 1);
  ed.doc.selections.Set(Selection{at, at, -1});
  ed.doc.selections.EnsureBlockCursors(ed.doc.table);
}

// One heal, run where the test can see every part of it: no thread, no pump,
// and the job left behind so the rung, the subprocess and the parse are all
// assertable.
std::shared_ptr<AnchorJob> HealFile(ProjectStore& store, const fs::path& path,
                                    const fs::path& git_root = {}, bool apply = true) {
  auto job = std::make_shared<AnchorJob>();
  job->key = LocationKey(path.string());
  job->path = path;
  job->git_root = git_root;
  job->rows = store.AnchorsFor(job->key);
  RunAnchorJob(*job);
  if (apply && !job->heals.empty()) store.ApplyHeals(job->heals);
  return job;
}

AnchorRow RowOf(ProjectStore& store, const fs::path& path) {
  const std::vector<AnchorRow> rows = store.AnchorsFor(LocationKey(path.string()));
  return rows.empty() ? AnchorRow{} : rows.front();
}

// `last_ts` and `seq` are the two columns a heal must never touch: one is the
// row's recency and the other is its place in the jump list.
bool RankingOf(sqlite3* db, std::int64_t id, double& last_ts, std::int64_t& seq) {
  Stmt stmt{db, "SELECT last_ts, seq FROM locations WHERE id=?1;"};
  if (!stmt) return false;
  stmt.Int(1, id);
  if (!stmt.Step()) return false;
  last_ts = stmt.Double(0);
  seq = stmt.Integer(1);
  return true;
}

// Records one place, the way the recorder would, out of a file on disk.
std::int64_t SeedRow(ProjectStore& store, const fs::path& path, std::string_view text, Index line,
                     bool with_blob = true) {
  const AnchorFile file = FileOf(text);
  LocationRecord row;
  row.path = path.string();
  row.line = line;
  row.col = 1;
  row.exact = true;
  row.has_text = true;
  row.content = file.lines[static_cast<std::size_t>(line - 1)];
  row.context = AnchorContextAt(file, line - 1);
  row.uniq = 0;
  for (const auto& [hash, at] : file.index) {
    if (hash == AnchorLineHash(row.content)) ++row.uniq;
  }
  if (with_blob) row.blob = GitBlobOid(text);
  return store.WriteLocation(row);
}

bool HaveGit() {
  const common::CmdResult result = common::RunCmdWithCapture(
      "git --version", common::CaptureMode::kDevNull, common::CaptureMode::kDevNull);
  return result.exit_status == 0;
}

bool Git(const fs::path& dir, const std::string& args) {
  const common::CmdResult result =
      common::RunCmdWithCapture("git -C '" + dir.string() + "' " + args,
                                common::CaptureMode::kDevNull, common::CaptureMode::kDevNull);
  return result.exit_status == 0;
}

// Fixture setup, past the HaveGit() gate. A command that does not take is a
// failure and not a reason to stop quietly: the harness has no skip accounting,
// so a group that returned early checks nothing and still prints `ok`. The
// failing command is named, since git's own diagnostics went to /dev/null.
bool GitSetup(const fs::path& dir, std::initializer_list<std::string> commands) {
  bool ok = true;
  for (const std::string& args : commands) {
    if (Git(dir, args)) continue;
    std::cerr << "      failing git command: " << args << "\n";
    ok = false;
    break;
  }
  EXPECT_TRUE(ok);
  return ok;
}

}

void AnchorLineRules() {
  TEST_CASE("anchor: a line normalises exactly once, and the recorder uses that one");

  EXPECT_EQ(NormalizeAnchorLine("  if (foo) {\t"), std::string{"if (foo) {"});
  EXPECT_EQ(NormalizeAnchorLine("\t \r\n"), std::string{});
  EXPECT_EQ(NormalizeAnchorLine("a"), std::string{"a"});

  {
    // Two hundred and one bytes of three-byte code points: the cut lands inside
    // one and has to walk back off it, so what comes out is still UTF-8.
    std::string wide;
    for (int i = 0; i < 67; ++i) wide += "\xE4\xB8\xAD";
    const std::string kept = NormalizeAnchorLine("  " + wide + "  ");
    EXPECT_EQ(kept.size(), std::size_t{198});
    EXPECT_TRUE(IsWellFormedUtf8(kept));
  }

  TEST_CASE("anchor: the file splits the way the document counts lines");
  {
    const AnchorFile file = FileOf("alpha\n  beta  \n");
    // A trailing newline leaves a final empty line, which is what LineCount()
    // reports and therefore what a stored line number is measured against.
    EXPECT_EQ(std::ssize(file.lines), Index{3});
    if (file.lines.size() < 3) return;
    EXPECT_EQ(file.lines[1], std::string{"beta"});
    EXPECT_EQ(file.lines[2], std::string{});
    // Blank lines are not in the index: nothing heals onto emptiness.
    EXPECT_EQ(std::ssize(file.index), Index{2});
    EXPECT_EQ(std::ssize(file.hashes), Index{3});
    if (file.hashes.empty()) return;
    EXPECT_EQ(file.hashes[0], AnchorLineHash("alpha"));

    const AnchorFile crlf = FileOf("alpha\r\nbeta\r\n");
    EXPECT_EQ(std::ssize(crlf.lines), Index{3});
    if (crlf.lines.size() < 2) return;
    EXPECT_EQ(crlf.lines[0], std::string{"alpha"});
    EXPECT_EQ(crlf.lines[1], std::string{"beta"});
  }

  TEST_CASE("anchor: what a record stores is what a heal reads back");
  {
    const Scratch scratch{"koi-anchor-lines"};
    const AsProjectRoot root{scratch.dir};
    const std::string text = "one\n  two  \nthree\nfour\nfive\nsix\n";
    const fs::path source = scratch.Write("same.txt", text);

    Editor ed;
    ed.theme = BuiltinTheme();
    std::string error;
    ed.project = ProjectStore::Open(scratch.dir / "state.db", error);
    EXPECT_TRUE(ed.project != nullptr);
    EXPECT_FALSE(static_cast<bool>(LoadDocument(source, ed.doc)));

    PutCursorOnLine(ed, 3);
    LocationRecord row;
    EXPECT_TRUE(LocationHere(ed, row));

    // The same line, reached the other way: through the file the heal reads.
    const AnchorFile file = FileOf(text);
    EXPECT_EQ(row.content, file.lines[2]);
    EXPECT_EQ(row.context, AnchorContextAt(file, 2));
    EXPECT_EQ(row.blob, GitBlobOid(text));

    // And at the top of the file, where the context is clipped rather than
    // padded -- the two have to clip the same way or a heal there compares an
    // entry against the wrong offset.
    PutCursorOnLine(ed, 1);
    EXPECT_TRUE(LocationHere(ed, row));
    EXPECT_EQ(row.context, AnchorContextAt(file, 0));
  }
}

void AnchorPatienceDiff() {
  TEST_CASE("anchor: patience diff maps unchanged lines exactly across an insertion");

  const std::vector<std::string> before = StepLines(20);
  {
    std::vector<std::string> after = before;
    after.insert(after.begin() + 4, {"  inserted_a();", "  inserted_b();", "  inserted_c();"});
    const AnchorFile a = FileOf(Joined(before));
    const AnchorFile b = FileOf(Joined(after));
    const std::vector<DiffHunk> hunks = DiffLineMap(a.hashes, b.hashes);

    // Above the insertion: unmoved.
    EXPECT_TRUE(MapLineThroughDiff(hunks, 2).exact);
    EXPECT_EQ(MapLineThroughDiff(hunks, 2).line, Index{2});
    // Below it: shifted by exactly what went in.
    EXPECT_TRUE(MapLineThroughDiff(hunks, 6).exact);
    EXPECT_EQ(MapLineThroughDiff(hunks, 6).line, Index{9});
    EXPECT_EQ(MapLineThroughDiff(hunks, 19).line, Index{22});
  }

  TEST_CASE("anchor: and across a deletion, with the deleted lines reported as changed");
  {
    std::vector<std::string> after = before;
    after.erase(after.begin() + 2, after.begin() + 5);
    const AnchorFile a = FileOf(Joined(before));
    const AnchorFile b = FileOf(Joined(after));
    const std::vector<DiffHunk> hunks = DiffLineMap(a.hashes, b.hashes);

    EXPECT_TRUE(MapLineThroughDiff(hunks, 1).exact);
    EXPECT_EQ(MapLineThroughDiff(hunks, 1).line, Index{1});
    EXPECT_TRUE(MapLineThroughDiff(hunks, 7).exact);
    EXPECT_EQ(MapLineThroughDiff(hunks, 7).line, Index{4});
    // A line that is gone is not mapped anywhere: it is seeded at the hunk.
    EXPECT_FALSE(MapLineThroughDiff(hunks, 3).exact);
    EXPECT_EQ(MapLineThroughDiff(hunks, 3).line, Index{2});
  }

  TEST_CASE("anchor: a moved block keeps its own lines and does not drag the rest");
  {
    const std::vector<std::string> a_lines{"AAA();", "BBB();", "CCC();",
                                           "DDD();", "EEE();", "FFF();"};
    const std::vector<std::string> b_lines{"DDD();", "EEE();", "FFF();",
                                           "AAA();", "BBB();", "CCC();"};
    const AnchorFile a = FileOf(Joined(a_lines));
    const AnchorFile b = FileOf(Joined(b_lines));
    const std::vector<DiffHunk> hunks = DiffLineMap(a.hashes, b.hashes);

    // One of the two blocks survives as the anchor run and maps exactly; the
    // other reads as deleted and re-inserted, which is what every line-diff
    // does with a move and is honest about.
    const MappedLine ddd = MapLineThroughDiff(hunks, 3);
    EXPECT_TRUE(ddd.exact);
    EXPECT_EQ(ddd.line, Index{0});
    EXPECT_EQ(MapLineThroughDiff(hunks, 5).line, Index{2});
    EXPECT_FALSE(MapLineThroughDiff(hunks, 0).exact);
  }

  TEST_CASE("anchor: repeated lines are never matched to each other");
  {
    // Every line the same but one. Patience has no unique-common line to anchor
    // on in the repeated run, so it reports the region changed rather than
    // pairing off braces that have nothing to do with each other.
    const std::vector<std::string> a_lines{"}", "}", "}", "marker();", "}", "}"};
    const std::vector<std::string> b_lines{"}", "}", "marker();", "}", "}", "}", "}"};
    const AnchorFile a = FileOf(Joined(a_lines));
    const AnchorFile b = FileOf(Joined(b_lines));
    const std::vector<DiffHunk> hunks = DiffLineMap(a.hashes, b.hashes);
    const MappedLine marker = MapLineThroughDiff(hunks, 3);
    EXPECT_TRUE(marker.exact);
    EXPECT_EQ(marker.line, Index{2});
  }

  TEST_CASE("anchor: identical files have no hunks at all");
  {
    const AnchorFile a = FileOf(Joined(before));
    EXPECT_TRUE(DiffLineMap(a.hashes, a.hashes).empty());
    for (Index i = 0; i < 20; ++i) {
      const MappedLine at = MapLineThroughDiff({}, i);
      EXPECT_TRUE(at.exact);
      EXPECT_EQ(at.line, i);
    }
  }
}

void AnchorEditDistance() {
  TEST_CASE("anchor: bit-parallel edit distance against known pairs");

  EXPECT_EQ(EditErrors("kitten", "sitting", 99), 3);
  EXPECT_EQ(EditErrors("sitting", "kitten", 99), 3);
  EXPECT_EQ(EditErrors("flaw", "lawn", 99), 2);
  EXPECT_EQ(EditErrors("saturday", "sunday", 99), 3);
  EXPECT_EQ(EditErrors("abc", "abc", 99), 0);
  EXPECT_EQ(EditErrors("", "abc", 99), 3);
  EXPECT_EQ(EditErrors("abc", "", 99), 3);
  EXPECT_EQ(EditErrors("", "", 99), 0);
  EXPECT_EQ(EditErrors("if (foo)", "if (bar)", 99), 3);

  TEST_CASE("anchor: the cap is a ceiling on what is reported, never a wrong answer");
  EXPECT_EQ(EditErrors("kitten", "sitting", 1), 1);
  EXPECT_EQ(EditErrors("kitten", "sitting", 3), 3);
  EXPECT_EQ(EditErrors("kitten", "sitting", 0), 0);

  TEST_CASE("anchor: a long side is exact while the pattern still fits the word");
  {
    // Sixty-four is the word, and only the *pattern* has to fit it -- so a
    // short stored line against a long file line is measured exactly.
    const std::string haystack(200, 'a');
    EXPECT_EQ(EditErrors("aaa", haystack, 999), 197);
    EXPECT_EQ(EditErrors(haystack, "aaa", 999), 197);
    const std::string sixty_four(64, 'x');
    EXPECT_EQ(EditErrors(sixty_four, sixty_four, 999), 0);
    EXPECT_EQ(EditErrors(sixty_four, sixty_four + "y", 999), 1);
  }

  TEST_CASE("anchor: past the word both sides take the tail-verify route");
  {
    // Both over sixty-four: the first word is exact, the tails are charged the
    // cheap upper bound.
    std::string a(70, 'a');
    std::string b = a;
    b[65] = 'z';
    EXPECT_EQ(EditErrors(a, b, 999), 1);

    const std::string base(100, 'q');
    EXPECT_EQ(EditErrors(base, base, 999), 0);
    EXPECT_EQ(EditErrors(base, base + "xyz", 999), 3);
    // A change inside the first word is still measured by Myers.
    std::string head_change = base;
    head_change[3] = 'w';
    head_change[7] = 'w';
    EXPECT_EQ(EditErrors(base, head_change, 999), 2);
    // And the over-report only ever refuses a match, so it is bounded above by
    // the length itself.
    EXPECT_TRUE(EditErrors(base, std::string(100, 'r'), 999) == 100);
  }
}

void AnchorResolveLadder() {
  const std::vector<std::string> lines = StepLines(400);
  const std::string text = Joined(lines);
  const AnchorFile file = FileOf(text);
  // Held, not spelled inline: HealInput views into what it is handed, and a
  // temporary string would be gone before ResolveAnchor read it.
  const std::string kAnchored = StepContent(100);

  const auto base = [&file](Index line, std::string_view content) {
    HealInput in;
    in.file = &file;
    in.line = line;
    in.content = content;
    return in;
  };

  TEST_CASE("anchor ladder: rung 3, exact where the cache said");
  {
    const HealResult found = ResolveAnchor(base(101, kAnchored));
    EXPECT_EQ(found.rung, 3);
    EXPECT_EQ(found.line, Index{101});
    EXPECT_TRUE(found.unique);
    EXPECT_FALSE(found.miss);
  }

  TEST_CASE("anchor ladder: rung 4, exact within the window, nearest wins");
  {
    const HealResult found = ResolveAnchor(base(91, kAnchored));
    EXPECT_EQ(found.rung, 4);
    EXPECT_EQ(found.line, Index{101});
  }

  TEST_CASE("anchor ladder: rung 5, exact inside the symbol the row was in");
  {
    HealInput in = base(5, kAnchored);
    // Out of the +/-50 window, inside the re-found function.
    in.symbol = SymbolSpan{95, 130};
    const HealResult found = ResolveAnchor(in);
    EXPECT_EQ(found.rung, 5);
    EXPECT_EQ(found.line, Index{101});
  }

  TEST_CASE("anchor ladder: rung 6, exact anywhere, and only because it is unique");
  {
    const HealResult found = ResolveAnchor(base(5, kAnchored));
    EXPECT_EQ(found.rung, 6);
    EXPECT_EQ(found.line, Index{101});
    EXPECT_TRUE(found.unique);
  }

  TEST_CASE("anchor ladder: rung 6 refuses a line that is not unique, with nothing to confirm it");
  {
    // A bare brace, three of them, and the row says nothing about which. The
    // cached line is far from all three, so the position rungs have nothing to
    // say either and rung 6 is the one being asked.
    std::vector<std::string> braced = StepLines(400);
    braced[9] = "}";
    braced[49] = "}";
    braced[299] = "}";
    const AnchorFile many = FileOf(Joined(braced));

    HealInput in;
    in.file = &many;
    in.line = 200;
    in.content = "}";
    const HealResult found = ResolveAnchor(in);
    EXPECT_TRUE(found.miss);
    EXPECT_TRUE(found.rung != 6);

    TEST_CASE("anchor ladder: rung 6 takes it once the context confirms it");
    const std::string context = AnchorContextAt(many, 299);
    in.context = context;
    const HealResult confirmed = ResolveAnchor(in);
    EXPECT_EQ(confirmed.rung, 6);
    EXPECT_EQ(confirmed.line, Index{300});
    // Confirmed is not unique, and the write-back rule cares about the
    // difference: this is not a match that may rewrite what the row says.
    EXPECT_FALSE(confirmed.unique);
  }

  TEST_CASE("anchor ladder: rung 7, the line itself edited");
  {
    std::string edited = kAnchored;
    edited[6] = 'Q';
    edited[8] = 'Q';
    const HealResult found = ResolveAnchor(base(101, edited));
    EXPECT_EQ(found.rung, 7);
    EXPECT_EQ(found.line, Index{101});
    const double want = 1.0 - (2.0 / static_cast<double>(edited.size()));
    EXPECT_TRUE(std::abs(found.similarity - want) < 1e-9);
    // Two errors in thirty-one characters is well over the bar, so the row is
    // allowed to say what the line now reads.
    EXPECT_TRUE(found.similarity >= kRefreshSimilarity);
    EXPECT_TRUE(found.unique);
  }

  TEST_CASE("anchor ladder: rung 7 refuses anything scoring over the threshold");
  {
    const HealResult found =
        ResolveAnchor(base(101, "nothing in this file looks remotely like this line"));
    EXPECT_EQ(found.rung, 8);
    EXPECT_TRUE(found.miss);
    EXPECT_EQ(found.line, Index{0});

    // Close enough in text, too far away in lines: the distance term is what
    // pushes it over, and it is refused rather than taken at a distance.
    std::string edited = kAnchored;
    edited[6] = 'Q';
    HealInput in = base(1, edited);
    in.symbol = SymbolSpan{95, 130};
    EXPECT_EQ(ResolveAnchor(in).rung, 8);
  }

  TEST_CASE("anchor ladder: rung 2 puts a branch switch back exactly");
  {
    std::vector<std::string> shifted = lines;
    shifted.insert(shifted.begin() + 10, {"  added_a();", "  added_b();", "  added_c();",
                                          "  added_d();", "  added_e();"});
    const AnchorFile after = FileOf(Joined(shifted));
    const std::vector<DiffHunk> hunks = DiffLineMap(file.hashes, after.hashes);

    HealInput in;
    in.file = &after;
    in.line = 101;
    in.content = kAnchored;
    in.hunks = &hunks;
    const HealResult found = ResolveAnchor(in);
    EXPECT_EQ(found.rung, 2);
    EXPECT_EQ(found.line, Index{106});

    TEST_CASE("anchor ladder: a line inside a changed hunk falls through, seeded there");
    std::vector<std::string> rewritten = lines;
    rewritten[100] = StepLine(100) + "  // renamed";
    const AnchorFile edited = FileOf(Joined(rewritten));
    const std::vector<DiffHunk> edits = DiffLineMap(file.hashes, edited.hashes);
    HealInput on_it;
    on_it.file = &edited;
    on_it.line = 101;
    on_it.content = kAnchored;
    on_it.hunks = &edits;
    const HealResult fuzzy = ResolveAnchor(on_it);
    EXPECT_EQ(fuzzy.rung, 7);
    EXPECT_EQ(fuzzy.line, Index{101});
    // Twelve errors in thirty-one characters: found, and not similar enough to
    // rewrite what the row says.
    EXPECT_TRUE(fuzzy.similarity < kRefreshSimilarity);
  }

  TEST_CASE("anchor ladder: rung 2's position is kept as the seed when the text disagrees");
  {
    // The row's blob names the file as it is, but its content is one edit
    // older: a sub-0.9 heal writes the blob and leaves the text alone, so the
    // two describe different moments. Rung 2 maps the line exactly, cannot
    // verify it, and what it found is still the only current answer to "where
    // did this line go" -- the cached number belongs to a file 120 lines shorter.
    std::vector<std::string> disk = StepLines(200);
    disk[99] = "  value_marker(1234) + offsets;";
    const AnchorFile was = FileOf(Joined(disk));
    std::vector<std::string> grown;
    for (int i = 0; i < 120; ++i) grown.push_back("  prepended_" + std::to_string(i) + "();");
    grown.insert(grown.end(), disk.begin(), disk.end());
    const AnchorFile now = FileOf(Joined(grown));
    const std::vector<DiffHunk> hunks = DiffLineMap(was.hashes, now.hashes);
    const std::string stale = "value_marker(1234) + offset;";

    HealInput in;
    in.file = &now;
    in.line = 100;
    in.content = stale;
    in.hunks = &hunks;
    const HealResult found = ResolveAnchor(in);
    EXPECT_EQ(found.rung, 7);
    EXPECT_EQ(found.line, Index{220});

    // Unless the line is the open buffer's, which has already been carried by
    // the buffer's own edits: mapping it a second time would shift it twice, so
    // that one searches where the buffer says it is and misses instead.
    HealInput live = in;
    live.live_line = true;
    EXPECT_TRUE(ResolveAnchor(live).miss);
  }

  TEST_CASE("anchor ladder: a row that never said what it was is not resolvable");
  {
    HealInput in;
    in.file = &file;
    in.line = 101;
    const HealResult found = ResolveAnchor(in);
    EXPECT_TRUE(found.miss);
  }
}

void AnchorHealJobShapes() {
  const Scratch scratch{"koi-anchor-job"};
  const AsProjectRoot root{scratch.dir};
  const fs::path db = scratch.dir / "state.db";
  std::string error;
  std::shared_ptr<ProjectStore> store = ProjectStore::Open(db, error);
  EXPECT_TRUE(store != nullptr);
  if (store == nullptr) return;

  const std::vector<std::string> lines = StepLines(200);
  const std::string text = Joined(lines);

  TEST_CASE("anchor heal: an untouched file stops at the blob gate and writes nothing");
  {
    const fs::path source = scratch.Write("gate.txt", text);
    const std::int64_t seq = SeedRow(*store, source, text, 101);
    EXPECT_TRUE(seq != 0);
    const AnchorRow before = RowOf(*store, source);
    double was_ts = 0;
    std::int64_t was_seq = 0;
    EXPECT_TRUE(RankingOf(store->Connection(), before.id, was_ts, was_seq));

    const std::shared_ptr<AnchorJob> job = HealFile(*store, source);
    EXPECT_TRUE(job->blob_gate);
    EXPECT_TRUE(job->heals.empty());
    EXPECT_FALSE(job->parsed);

    // Nothing moved, and in particular neither of the two columns that decide
    // where this row stands in the list and in the ranking.
    double now_ts = 0;
    std::int64_t now_seq = 0;
    EXPECT_TRUE(RankingOf(store->Connection(), before.id, now_ts, now_seq));
    EXPECT_EQ(now_seq, was_seq);
    EXPECT_TRUE(now_ts <= was_ts);
    EXPECT_TRUE(now_ts >= was_ts);
    EXPECT_EQ(RowOf(*store, source).line, Index{101});
  }

  TEST_CASE("anchor heal: a plain rewrite heals by content and forks nothing");
  {
    const fs::path source = scratch.Write("plain.txt", text);
    EXPECT_TRUE(SeedRow(*store, source, text, 101) != 0);

    // The same shape a branch switch would make, made by writing the file --
    // and in a fixture with no repository, which is a supported shape and not a
    // failure.
    std::vector<std::string> moved = lines;
    moved.insert(moved.begin() + 10, {"  added_a();", "  added_b();", "  added_c();",
                                      "  added_d();", "  added_e();", "  added_f();",
                                      "  added_g();"});
    WriteFixtureFile(source, Joined(moved));

    const std::shared_ptr<AnchorJob> job = HealFile(*store, source);
    // The helper hands the job no root, so this only says the rung-2 block
    // honours an empty one. What decides that a project with no repository gets
    // an empty root in the first place is StartAnchorHeal, and the assertion
    // about it is in AnchorHealsFromTheEditor.
    EXPECT_FALSE(job->ran_git);
    EXPECT_EQ(std::ssize(job->heals), Index{1});
    if (!job->heals.empty()) {
      // Out of the +/-50 window from 101 to 108, so it is rung 6 -- exact,
      // unique, anywhere -- and not rung 2.
      EXPECT_TRUE(job->heals.front().rung >= 3);
      EXPECT_TRUE(job->heals.front().rung <= 6);
      EXPECT_FALSE(job->heals.front().miss);
    }
    EXPECT_EQ(RowOf(*store, source).line, Index{108});
  }

  TEST_CASE("anchor heal: a miss counts, three times, and never deletes the row");
  {
    const fs::path source = scratch.Write("truncated.txt", text);
    EXPECT_TRUE(SeedRow(*store, source, text, 101) != 0);
    const AnchorRow before = RowOf(*store, source);

    WriteFixtureFile(source, "the generator produced nothing this time\n");
    for (int i = 1; i <= 3; ++i) {
      const std::shared_ptr<AnchorJob> job = HealFile(*store, source);
      EXPECT_EQ(std::ssize(job->heals), Index{1});
      if (!job->heals.empty()) EXPECT_TRUE(job->heals.front().miss);
      const AnchorRow now = RowOf(*store, source);
      EXPECT_EQ(now.misses, static_cast<std::int64_t>(i));
      // Intact: the row still says what it was and where it was.
      EXPECT_EQ(now.line, before.line);
      EXPECT_EQ(now.content, before.content);
      EXPECT_EQ(now.blob, before.blob);
    }

    // Put the file back, and the row is true again -- which is a hit, whichever
    // rung says so. Here it is the blob gate, since the bytes are the very ones
    // the row was recorded against.
    WriteFixtureFile(source, text);
    const std::shared_ptr<AnchorJob> job = HealFile(*store, source);
    EXPECT_TRUE(job->blob_gate);
    EXPECT_EQ(RowOf(*store, source).misses, std::int64_t{0});
    EXPECT_EQ(RowOf(*store, source).line, Index{101});
  }

  TEST_CASE("anchor heal: a row with no text of its own is not counted against");
  {
    const fs::path source = scratch.Write("textless.txt", text);
    LocationRecord row;
    row.path = source.string();
    row.line = 40;
    row.exact = true;
    EXPECT_TRUE(store->WriteLocation(row) != 0);
    WriteFixtureFile(source, "something else entirely\n");

    const std::shared_ptr<AnchorJob> job = HealFile(*store, source);
    EXPECT_TRUE(job->heals.empty());
    EXPECT_EQ(RowOf(*store, source).misses, std::int64_t{0});
  }

  TEST_CASE("anchor heal: a reformat is not a rewrite -- every line is where it reads");
  {
    // The commit that re-indents the tree. Every line moves in the file and
    // every line changes on disk, but nothing a row stores about itself
    // changed, because what a row stores is the normalised line. A
    // normalisation that leaked whitespace into the hash would send this to
    // rung 7 at best and a miss at worst, on every file in the repository at
    // once.
    const fs::path source = scratch.Write("reformat.txt", text);
    EXPECT_TRUE(SeedRow(*store, source, text, 101) != 0);
    const AnchorRow before = RowOf(*store, source);

    std::vector<std::string> formatted;
    formatted.reserve(lines.size() + 7);
    for (int i = 0; i < 7; ++i) formatted.push_back("#include <added_" + std::to_string(i) + ">");
    for (const std::string& line : lines) {
      // Four more spaces of indent, a tab in front of them, and the trailing
      // whitespace a formatter leaves behind -- with CRLF endings on top, since
      // the same commit is what a checkout on another platform looks like.
      formatted.push_back("\t    " + line + "  \r");
    }
    WriteFixtureFile(source, Joined(formatted));

    const std::shared_ptr<AnchorJob> job = HealFile(*store, source);
    EXPECT_FALSE(job->blob_gate);
    EXPECT_EQ(std::ssize(job->heals), Index{1});
    if (!job->heals.empty()) {
      EXPECT_FALSE(job->heals.front().miss);
      // An exact rung -- 4, within the window -- and not the fuzzy one: the
      // text matches character for character once it is normalised.
      EXPECT_TRUE(job->heals.front().rung >= 3);
      EXPECT_TRUE(job->heals.front().rung <= 6);
      EXPECT_EQ(job->heals.front().line, Index{108});
    }
    const AnchorRow after = RowOf(*store, source);
    EXPECT_EQ(after.line, Index{108});
    EXPECT_EQ(after.misses, std::int64_t{0});
    EXPECT_EQ(after.content, before.content);
  }

  TEST_CASE("anchor heal: a file that is not there is not a miss");
  {
    // Half a repository is transiently missing across a branch switch, and a
    // history that counted that against itself would be worth nothing on the
    // way back.
    const fs::path source = scratch.Write("vanishing.txt", text);
    EXPECT_TRUE(SeedRow(*store, source, text, 101) != 0);
    const AnchorRow before = RowOf(*store, source);

    std::error_code ec;
    fs::remove(source, ec);
    EXPECT_FALSE(static_cast<bool>(ec));
    EXPECT_FALSE(fs::exists(source));

    const std::shared_ptr<AnchorJob> job = HealFile(*store, source);
    EXPECT_TRUE(job->heals.empty());
    EXPECT_FALSE(job->blob_gate);
    const AnchorRow gone = RowOf(*store, source);
    EXPECT_EQ(gone.misses, std::int64_t{0});
    EXPECT_EQ(gone.line, before.line);
    EXPECT_EQ(gone.content, before.content);
    EXPECT_EQ(gone.blob, before.blob);

    // Back on the way back: the row is true as it was recorded, so the blob
    // gate answers and nothing is written.
    WriteFixtureFile(source, text);
    const std::shared_ptr<AnchorJob> restored = HealFile(*store, source);
    EXPECT_TRUE(restored->blob_gate);
    EXPECT_TRUE(restored->heals.empty());
    EXPECT_EQ(RowOf(*store, source).line, Index{101});
    EXPECT_EQ(RowOf(*store, source).misses, std::int64_t{0});
  }
}

void AnchorHealWriteBack() {
  TEST_CASE("anchor heal: the line always, the text only when it is close and unambiguous");

  const Scratch scratch{"koi-anchor-drift"};
  const AsProjectRoot root{scratch.dir};
  std::string error;
  std::shared_ptr<ProjectStore> store = ProjectStore::Open(scratch.dir / "state.db", error);
  EXPECT_TRUE(store != nullptr);
  if (store == nullptr) return;

  // The doc's own walk: a decoy that a run of plausible repairs could arrive
  // at, well outside every window the ladder is allowed to search.
  std::vector<std::string> lines = StepLines(400);
  const std::string kOriginal =
      "if (foo && count > 0) { total += weight(foo) * scale(index, depth) + carry; }";
  lines[19] = "  " + kOriginal;
  lines[299] = "  if (bar && count > 0) { total += weight(bar) * scale(index, depth) + carry; }";

  const fs::path source = scratch.Write("drift.cpp", Joined(lines));
  EXPECT_TRUE(SeedRow(*store, source, Joined(lines), 20) != 0);
  const AnchorRow seeded = RowOf(*store, source);
  EXPECT_EQ(seeded.content, kOriginal);

  std::string live = kOriginal;
  std::string stored = kOriginal;
  for (int step = 1; step <= 10; ++step) {
    // One-character repairs, and at step five a bigger one -- sixteen
    // characters of a seventy-six character line, which is under the fuzzy
    // rung's threshold and over the write-back bar.
    if (step == 5) {
      live.replace(4, 16, "everything else!");
    } else {
      live[static_cast<std::size_t>(30 + step)] = '_';
    }
    lines[19] = "  " + live;
    WriteFixtureFile(source, Joined(lines));

    const std::shared_ptr<AnchorJob> job = HealFile(*store, source);
    EXPECT_EQ(std::ssize(job->heals), Index{1});
    if (job->heals.empty()) break;
    const AnchorHeal& heal = job->heals.front();
    EXPECT_FALSE(heal.miss);
    // The line follows the text it names, and never leaves it.
    EXPECT_EQ(heal.rung, 7);
    EXPECT_EQ(heal.line, Index{20});
    // Up to the big repair each step is a 0.9 match and the row is allowed to
    // say what the line now reads. From there on it is not -- and it stays put
    // while the *line* goes on following the text, which is the whole rule.
    const bool close = step < 5;
    EXPECT_EQ(heal.refresh_text, close);
    if (close) stored = live;
    const AnchorRow after = RowOf(*store, source);
    EXPECT_EQ(after.line, Index{20});
    EXPECT_EQ(after.content, stored);
  }

  // Never the decoy, at any point, and the decoy is still sitting there.
  EXPECT_EQ(RowOf(*store, source).line, Index{20});
  EXPECT_TRUE(RowOf(*store, source).content.find("bar") == std::string::npos);
}

void AnchorLiveShifting() {
  const Scratch scratch{"koi-anchor-shadow"};
  const AsProjectRoot root{scratch.dir};
  const std::string text = Joined(StepLines(80));
  const fs::path source = scratch.Write("shadow.txt", text);

  Editor ed;
  ed.theme = BuiltinTheme();
  std::string error;
  ed.project = ProjectStore::Open(scratch.dir / "state.db", error);
  EXPECT_TRUE(ed.project != nullptr);
  if (!ed.project) return;
  EXPECT_FALSE(static_cast<bool>(LoadDocument(source, ed.doc)));
  EXPECT_TRUE(SeedRow(*ed.project, source, text, 40) != 0);
  const std::int64_t id = RowOf(*ed.project, source).id;

  AdoptAnchorRows(ed, ed.doc);
  EXPECT_EQ(std::ssize(ed.doc.anchors.rows), Index{1});

  TEST_CASE("anchor shadow: whole lines inserted above shift the anchor exactly");
  {
    Edit edit;
    ExpectOk(Insert("added\nadded\nadded\n", LineStart(ed.doc.table, 4), ed.doc.table, &edit),
             "insert above");
    Index line = 0;
    EXPECT_TRUE(AnchorShadowLine(ed, ed.doc, id, line));
    EXPECT_EQ(line, Index{43});
    // The store still says forty. That is the point: the row is not rewritten
    // per keystroke, it is caught up when it is read.
    EXPECT_EQ(RowOf(*ed.project, source).line, Index{40});
  }

  TEST_CASE("anchor shadow: lines removed above shift it back");
  {
    Edit edit;
    ExpectOk(Delete(LineStart(ed.doc.table, 4), LineStart(ed.doc.table, 7), ed.doc.table, &edit),
             "delete above");
    Index line = 0;
    EXPECT_TRUE(AnchorShadowLine(ed, ed.doc, id, line));
    EXPECT_EQ(line, Index{40});
  }

  TEST_CASE("anchor shadow: an edit below leaves it alone");
  {
    Edit edit;
    ExpectOk(Insert("tail\n", LineStart(ed.doc.table, 60), ed.doc.table, &edit), "insert below");
    Index line = 0;
    EXPECT_TRUE(AnchorShadowLine(ed, ed.doc, id, line));
    EXPECT_EQ(line, Index{40});
  }

  TEST_CASE("anchor shadow: an edit on the anchor's own line marks it dirty, not moved");
  {
    Edit edit;
    const Index at = LineStart(ed.doc.table, 39) + 4;
    ExpectOk(Insert("XYZ", at, ed.doc.table, &edit), "edit the anchor line");
    Index line = 0;
    EXPECT_FALSE(AnchorShadowLine(ed, ed.doc, id, line));

    TEST_CASE("anchor shadow: and a save-heal re-resolves it by content");
    // The buffer as it stands is what the save would have written.
    const std::string live = ReadDocRange(ed.doc.table, Interval(0, DocLength(ed.doc.table)));
    WriteFixtureFile(source, live);
    auto job = std::make_shared<AnchorJob>();
    job->key = LocationKey(source.string());
    job->path = source;
    job->rows = ed.project->AnchorsFor(job->key);
    job->text = live;
    job->have_text = true;
    RunAnchorJob(*job);
    EXPECT_EQ(std::ssize(job->heals), Index{1});
    if (!job->heals.empty()) {
      // One line was added below the anchor and none above, so it is still on
      // forty -- found by the fuzzy rung, since its own text has changed.
      EXPECT_EQ(job->heals.front().rung, 7);
      EXPECT_EQ(job->heals.front().line, Index{40});
    }
  }

  TEST_CASE("anchor shadow: falling behind the journal marks every row dirty rather than guessing");
  {
    // From a clean shadow: the row above is dirty, and a dirty row would pass
    // this test for the wrong reason.
    ed.doc.anchors = AnchorShadow{};
    AdoptAnchorRows(ed, ed.doc);
    Index line = 0;
    EXPECT_TRUE(AnchorShadowLine(ed, ed.doc, id, line));

    // What TrimJournal does at the cap, without sixty-five thousand edits: two
    // edits happen and then the base moves past where the shadow was standing.
    // The invariant the journal keeps -- base + entries == revision -- holds
    // either side of it.
    Edit edit;
    ExpectOk(Insert("more\n", 0, ed.doc.table, &edit), "one");
    ExpectOk(Insert("more\n", 0, ed.doc.table, &edit), "two");
    ed.doc.table.journal.clear();
    ed.doc.table.journal_base = ed.doc.table.revision;
    // The shifts cannot be reconstructed from a journal that no longer reaches
    // back to them, so every row goes to the content ladder instead of being
    // guessed at.
    EXPECT_FALSE(AnchorShadowLine(ed, ed.doc, id, line));
  }
}

void AnchorJumpBackwardFollowsEdits() {
  TEST_CASE("anchor: jump_backward lands on the text it left, not on the number");

  const Scratch scratch{"koi-anchor-jumpback"};
  const AsProjectRoot root{scratch.dir};
  const std::string text = Joined(StepLines(80));
  const fs::path source = scratch.Write("back.txt", text);

  Editor ed;
  ed.theme = BuiltinTheme();
  std::string error;
  ed.project = ProjectStore::Open(scratch.dir / "state.db", error);
  EXPECT_TRUE(ed.project != nullptr);
  if (!ed.project) return;
  std::string jump_error;
  ed.jumps = JumpStore::Open(ed.project, "pane-anchor", jump_error);
  EXPECT_TRUE(ed.jumps != nullptr);
  EXPECT_FALSE(static_cast<bool>(LoadDocument(source, ed.doc)));
  AdoptAnchorRows(ed, ed.doc);

  // The place to come back to, recorded the way a jump motion records it.
  PutCursorOnLine(ed, 20);
  const std::string want = ReadDocRange(ed.doc.table, LineContentRange(ed.doc.table, 19));
  RecordJump(ed);

  // Somewhere else in the same file, so the step back has a row to step from.
  PutCursorOnLine(ed, 60);
  RecordJump(ed);

  // Twelve lines above the target. The stored row still says twenty.
  Edit edit;
  std::vector<Edit> edits;
  std::string added;
  for (int i = 0; i < 12; ++i) added += "  inserted_" + std::to_string(i) + "();\n";
  ExpectOk(Insert(added, LineStart(ed.doc.table, 4), ed.doc.table, &edit), "insert above");
  edits.push_back(edit);
  ed.doc.selections.MapThroughEdits(ed.doc.table, edits);

  StepJump(ed, false);
  StepJump(ed, false);

  const Index landed = LineAt(ed.doc.table, Cur(ed));
  EXPECT_EQ(landed, Index{31});  // 0-based: line 32
  EXPECT_EQ(ReadDocRange(ed.doc.table, LineContentRange(ed.doc.table, landed)), want);
  // And it got there without a heal: the store was never rewritten.
  bool stored_twenty = false;
  for (const AnchorRow& row : ed.project->AnchorsFor(LocationKey(source.string()))) {
    if (row.line == 20) stored_twenty = true;
  }
  EXPECT_TRUE(stored_twenty);
}

void AnchorHealsThroughGit() {
  TEST_CASE("anchor heal: a branch switch heals through the diff, exactly");

  if (!HaveGit()) return;

  const Scratch scratch{"koi-anchor-git"};
  const fs::path repo = scratch.dir / "repo";
  fs::create_directories(repo);
  const AsProjectRoot root{repo};

  if (!GitSetup(repo, {"init -q", "config user.email t@example.com",
                       "config user.name test"})) {
    return;
  }

  const std::vector<std::string> lines = StepLines(200);
  const std::string text = Joined(lines);
  const fs::path source = repo / "src.txt";
  WriteFixtureFile(source, text);
  if (!GitSetup(repo, {"add src.txt", "-c commit.gpgsign=false commit -q -m first"})) return;
  const std::string first_branch = GitBranch(repo);
  EXPECT_TRUE(!first_branch.empty());
  if (first_branch.empty()) return;

  std::string error;
  std::shared_ptr<ProjectStore> store = ProjectStore::Open(scratch.dir / "state.db", error);
  EXPECT_TRUE(store != nullptr);
  if (store == nullptr) return;
  EXPECT_TRUE(SeedRow(*store, source, text, 101) != 0);
  EXPECT_EQ(RowOf(*store, source).blob, GitBlobOid(text));

  // A branch where the file grew seven lines above the anchor, committed so the
  // old blob is still in the odb to diff against.
  if (!GitSetup(repo, {"checkout -q -b other"})) return;
  std::vector<std::string> moved = lines;
  moved.insert(moved.begin() + 10,
               {"  added_a();", "  added_b();", "  added_c();", "  added_d();", "  added_e();",
                "  added_f();", "  added_g();"});
  WriteFixtureFile(source, Joined(moved));
  if (!GitSetup(repo, {"-c commit.gpgsign=false commit -q -am second"})) return;

  const std::shared_ptr<AnchorJob> job = HealFile(*store, source, repo);
  EXPECT_TRUE(job->ran_git);
  EXPECT_EQ(std::ssize(job->heals), Index{1});
  if (!job->heals.empty()) {
    EXPECT_EQ(job->heals.front().rung, 2);
    EXPECT_EQ(job->heals.front().line, Index{108});
  }
  EXPECT_EQ(RowOf(*store, source).line, Index{108});
  // The heal took the new blob with it, which is what the way back is measured
  // against.
  EXPECT_EQ(RowOf(*store, source).blob, GitBlobOid(Joined(moved)));

  TEST_CASE("anchor heal: and switching back puts it where it came from");
  {
    // The round trip. The row now describes the branch it was healed on, so
    // going back is the same problem again in the other direction: one diff
    // from the row's blob to the file on disk, and the row lands on 101.
    if (!GitSetup(repo, {"checkout -q " + first_branch})) return;
    const std::shared_ptr<AnchorJob> back = HealFile(*store, source, repo);
    EXPECT_TRUE(back->ran_git);
    EXPECT_EQ(std::ssize(back->heals), Index{1});
    if (!back->heals.empty()) {
      EXPECT_EQ(back->heals.front().rung, 2);
      EXPECT_EQ(back->heals.front().line, Index{101});
    }
    EXPECT_EQ(RowOf(*store, source).line, Index{101});
    EXPECT_EQ(RowOf(*store, source).blob, GitBlobOid(text));

    // And now that it is true again, the next focus-in costs the blob gate and
    // nothing else -- no diff, no fork.
    const std::shared_ptr<AnchorJob> settled = HealFile(*store, source, repo);
    EXPECT_TRUE(settled->blob_gate);
    EXPECT_FALSE(settled->ran_git);
    EXPECT_TRUE(settled->heals.empty());

    // Put it back on `other` for the block below, which is about the odb and
    // not about the branch.
    if (!GitSetup(repo, {"checkout -q other"})) return;
    const std::shared_ptr<AnchorJob> again = HealFile(*store, source, repo);
    EXPECT_EQ(RowOf(*store, source).line, Index{108});
    EXPECT_TRUE(!again->heals.empty());
  }

  TEST_CASE("anchor heal: an oid the odb does not hold is skipped, not fatal");
  {
    // The exact shape of a project with no git at all, reached from one that
    // has it: the object simply is not there.
    Stmt stmt{store->Connection(),
              "UPDATE locations SET blob='0123456789abcdef0123456789abcdef01234567';"};
    EXPECT_TRUE(stmt.Run());
    std::vector<std::string> again = moved;
    again.insert(again.begin() + 10, {"  more_a();", "  more_b();"});
    WriteFixtureFile(source, Joined(again));

    const std::shared_ptr<AnchorJob> second = HealFile(*store, source, repo);
    EXPECT_TRUE(second->ran_git);
    EXPECT_EQ(std::ssize(second->heals), Index{1});
    if (!second->heals.empty()) {
      // Fell straight to the content rungs and found it anyway.
      EXPECT_TRUE(second->heals.front().rung >= 3);
      EXPECT_FALSE(second->heals.front().miss);
    }
    EXPECT_EQ(RowOf(*store, source).line, Index{110});
  }
}

namespace {

// Two identical function bodies, far apart, with two hundred lines of filler
// between them: the content is not unique, and the +/-2 context around the
// anchor is the same in both copies -- so it confirms nothing, and rung 6 has
// to refuse. What is left to tell them apart is the name of the function the
// row was recorded in, which is rung 5.
std::string TwinFunctions(int lead, int gap, const std::string& first, const std::string& second) {
  const auto body = [](const std::string& name) {
    return "int " + name + "(int n) {\n"
           "  int value = 0;\n"
           "  value += n;\n"
           "  value *= 2;\n"
           "  value -= 1;\n"
           "  return value;\n"
           "}\n";
  };
  std::string out;
  for (int i = 0; i < lead; ++i) out += "static const int filler_" + std::to_string(i) + " = " +
                                        std::to_string(i * 7) + ";\n";
  out += body(first);
  for (int i = 0; i < gap; ++i) out += "static const int between_" + std::to_string(i) + " = " +
                                       std::to_string(i * 11) + ";\n";
  out += body(second);
  return out;
}

}

void AnchorHealNamesTheSymbol() {
  TEST_CASE("anchor heal: rung 5 re-finds the function by name, off the main thread");

  const Scratch scratch{"koi-anchor-symbol"};
  const AsProjectRoot root{scratch.dir};
  const std::string text = TwinFunctions(10, 200, "Alpha", "Beta");
  const fs::path source = scratch.Write("twins.cpp", text);

  Editor ed;
  ed.theme = BuiltinTheme();
  std::string error;
  ed.project = ProjectStore::Open(scratch.dir / "state.db", error);
  EXPECT_TRUE(ed.project != nullptr);
  if (!ed.project) return;

  // `value *= 2;` inside Beta: line 10 lead + 7 for Alpha + 200 between + 4.
  const Index anchored = 10 + 7 + 200 + 4;
  const AnchorFile before = FileOf(text);
  EXPECT_EQ(before.lines[static_cast<std::size_t>(anchored - 1)], std::string{"value *= 2;"});
  EXPECT_TRUE(SeedRow(*ed.project, source, text, anchored) != 0);
  {
    // The recorder resolved the name; it left `symbol` null in the copy below,
    // which is the cold-buffer case the parse is also there to fix.
    Stmt stmt{ed.project->Connection(), "UPDATE locations SET symbol='Beta';"};
    EXPECT_TRUE(stmt.Run());
  }

  // Sixty lines at the top. That is more than the +/-50 window, so neither the
  // row's own copy nor the decoy is reachable by position.
  std::string added;
  for (int i = 0; i < 60; ++i) added += "static const int added_" + std::to_string(i) + " = 1;\n";
  WriteFixtureFile(source, added + text);

  StartAnchorHeal(ed, source, {}, false);
  EXPECT_EQ(std::ssize(ed.anchor_jobs), Index{1});
  if (ed.anchor_jobs.empty()) return;
  const std::shared_ptr<AnchorJob> job = ed.anchor_jobs.front();
  PumpUntilIdle(ed);

  // Parsed on the worker, with the grammar and query caches that scan uses.
  EXPECT_TRUE(job->parsed);
  EXPECT_EQ(std::ssize(job->heals), Index{1});
  if (!job->heals.empty()) EXPECT_EQ(job->heals.front().rung, 5);
  EXPECT_EQ(RowOf(*ed.project, source).line, anchored + 60);

  TEST_CASE("anchor heal: a null symbol is filled in by the parse the ladder already paid for");
  {
    // The cold-buffer case: the recorder had no live tree, so the row named no
    // function. It is filled in by the parse a hard heal already had to do --
    // and only then, which is what keeps an easy heal free of one.
    std::string one_function = "static const int lead = 1;\n";
    one_function +=
        "int Gamma(int n) {\n"
        "  int unique_marker_here = 12345;\n"
        "  return unique_marker_here + n;\n"
        "}\n";
    const fs::path lone = scratch.Write("lone.cpp", one_function);
    EXPECT_TRUE(SeedRow(*ed.project, lone, one_function, 3) != 0);
    EXPECT_TRUE(RowOf(*ed.project, lone).symbol_null);

    // Eighty lines above it, so the position rungs cannot reach it and the
    // ladder goes as far as rung 6.
    std::string above;
    for (int i = 0; i < 80; ++i) above += "static const int over_" + std::to_string(i) + " = 2;\n";
    WriteFixtureFile(lone, above + one_function);

    const std::shared_ptr<AnchorJob> second = HealFile(*ed.project, lone);
    EXPECT_TRUE(second->parsed);
    EXPECT_EQ(std::ssize(second->heals), Index{1});
    if (!second->heals.empty()) EXPECT_EQ(second->heals.front().rung, 6);
    const AnchorRow row = RowOf(*ed.project, lone);
    EXPECT_EQ(row.line, Index{83});
    EXPECT_FALSE(row.symbol_null);
    EXPECT_EQ(row.symbol, std::string{"Gamma"});
  }
}

void AnchorHealCost() {
  TEST_CASE("anchor heal: a source file's worth of rows costs a read and a hash pass");

  // koi/src/navigate.cpp's shape: about four thousand lines and a hundred and
  // fifty kilobytes.
  std::vector<std::string> lines;
  lines.reserve(4000);
  for (int i = 0; i < 4000; ++i) {
    lines.push_back("  const Index value_" + std::to_string(i) +
                    " = Compute(table, selection, " + std::to_string(i % 37) +
                    ", flags | kSomething);");
  }
  const std::string text = Joined(lines);
  EXPECT_TRUE(text.size() > 100000);

  std::vector<std::string> after = lines;
  after.insert(after.begin() + 900, {"  Added(1);", "  Added(2);", "  Added(3);"});
  const std::string moved = Joined(after);

  const long long ms = MillisecondsOf([&] {
    AnchorFile was;
    AnchorFile now;
    SplitAnchorLines(text, was);
    SplitAnchorLines(moved, now);
    const std::vector<DiffHunk> hunks = DiffLineMap(was.hashes, now.hashes);
    // Twenty rows, which is more than any one file in the live store has.
    for (int i = 0; i < 20; ++i) {
      HealInput in;
      in.file = &now;
      in.line = 100 + (i * 150);
      in.content = was.lines[static_cast<std::size_t>(in.line - 1)];
      in.hunks = &hunks;
      const HealResult found = ResolveAnchor(in);
      EXPECT_FALSE(found.miss);
    }
  });
  // The budget is 10ms; the bound is loose because what it separates is a heal
  // that is linear in the file from one that is quadratic in the rows.
  EXPECT_TRUE(ms < 100);
}

void AnchorHealsFromTheEditor() {
  TEST_CASE("anchor heal: the editor's own path -- queued, worked, applied on the main thread");

  const Scratch scratch{"koi-anchor-pump"};
  const AsProjectRoot root{scratch.dir};
  const std::vector<std::string> lines = StepLines(200);
  const std::string text = Joined(lines);
  const fs::path source = scratch.Write("pumped.txt", text);

  Editor ed;
  ed.theme = BuiltinTheme();
  std::string error;
  ed.project = ProjectStore::Open(scratch.dir / "state.db", error);
  EXPECT_TRUE(ed.project != nullptr);
  if (!ed.project) return;
  EXPECT_TRUE(SeedRow(*ed.project, source, text, 101) != 0);

  std::vector<std::string> moved = lines;
  moved.insert(moved.begin() + 10, {"  added_a();", "  added_b();", "  added_c();",
                                    "  added_d();", "  added_e();", "  added_f();",
                                    "  added_g();"});
  WriteFixtureFile(source, Joined(moved));

  EXPECT_FALSE(static_cast<bool>(LoadDocument(source, ed.doc)));
  AdoptAnchorRows(ed, ed.doc);
  StartAnchorHeal(ed, source, {}, false);
  EXPECT_EQ(std::ssize(ed.anchor_jobs), Index{1});
  if (ed.anchor_jobs.empty()) return;
  const std::shared_ptr<AnchorJob> job = ed.anchor_jobs.front();
  // The fixture is under the system temp directory and has no .git anywhere
  // above it, so StartAnchorHeal left the root empty -- and that is the single
  // decision behind "a project with no repository forks nothing at all". The
  // job outlives the pump, so it is still readable below.
  EXPECT_TRUE(job->git_root.empty());

  // A second trigger for the same file while one is in flight is not a second
  // job: two of them would be racing to describe one file two ways.
  StartAnchorHeal(ed, source, {}, false);
  EXPECT_EQ(std::ssize(ed.anchor_jobs), Index{1});

  PumpUntilIdle(ed);
  EXPECT_TRUE(ed.anchor_jobs.empty());
  EXPECT_FALSE(job->ran_git);
  EXPECT_EQ(RowOf(*ed.project, source).line, Index{108});
  // And the open buffer's shadow was moved with it, so a jump into this file
  // does not have to wait for another heal to be right.
  const std::int64_t id = RowOf(*ed.project, source).id;
  Index line = 0;
  EXPECT_TRUE(AnchorShadowLine(ed, ed.doc, id, line));
  EXPECT_EQ(line, Index{108});

  TEST_CASE("anchor heal: quit applies the job it stopped to wait for");
  // The last save before `:wq` queues a heal and the loop never comes round to
  // pump it, but shutdown joins the pool and the pool finishes what it has
  // accepted -- so quit pays for the work either way. What it must not do is
  // throw the answer away and leave the same job for the next session.
  std::vector<std::string> again = moved;
  again.insert(again.begin() + 10, {"  more_a();", "  more_b();", "  more_c();",
                                    "  more_d();", "  more_e();", "  more_f();",
                                    "  more_g();"});
  WriteFixtureFile(source, Joined(again));
  StartAnchorHeal(ed, source, {}, false);
  EXPECT_EQ(std::ssize(ed.anchor_jobs), Index{1});

  ShutdownEditor(ed);
  EXPECT_TRUE(ed.anchor_jobs.empty());
  const std::vector<AnchorRow> rows = ed.project->AnchorsFor(LocationKey(source.string()));
  const auto healed = std::ranges::find(rows, id, &AnchorRow::id);
  EXPECT_TRUE(healed != rows.end());
  if (healed != rows.end()) EXPECT_EQ(healed->line, Index{115});

  TEST_CASE("anchor heal: the save trigger is handed the bytes the save wrote");
  // The write already builds the document's text; the trigger takes that string
  // rather than reading the document a second time for it. The worker moves the
  // text out of the job the moment it runs, so the job's buffer cannot be read
  // back here without racing it. The proof is downstream instead: the save just
  // recorded a row carrying the written bytes' blob, so the blob the worker
  // hashes from the handed string must gate against it -- anything but the
  // file's exact bytes, or an empty hand-over, and the gate cannot answer.
  RunTypableCommand(ed, "w!");
  EXPECT_TRUE(ed.status.find("wrote") != std::string::npos);
  EXPECT_EQ(std::ssize(ed.anchor_jobs), Index{1});
  const std::shared_ptr<AnchorJob> saved =
      ed.anchor_jobs.empty() ? nullptr : ed.anchor_jobs.front();
  // Written on the main thread before the job is queued, never by the worker.
  if (saved != nullptr) EXPECT_TRUE(saved->have_text);

  // And a save while that job is still in flight declines -- one at a time per
  // file -- without the caller having built anything for it to decline. The
  // jobs list only shrinks on the main thread's pump, so this cannot race the
  // worker either.
  RunTypableCommand(ed, "w!");
  EXPECT_EQ(std::ssize(ed.anchor_jobs), Index{1});
  PumpUntilIdle(ed);
  // The save's own edit record carries the written bytes' blob, so the rung-1
  // bit proves the worker hashed exactly those bytes -- an empty or wrong
  // hand-over gates nothing. Not `blob_gate`: that flag means every row gated,
  // and the older seeded row here rightly goes down the ladder instead.
  if (saved != nullptr) {
    EXPECT_TRUE((saved->rungs & (1u << static_cast<int>(HealRung::kBlob))) != 0);
  }
}


void AnchorHealAtFileEdges() {
  TEST_CASE("anchor heal: a context at the top of the file lines up with the stored one");

  const Scratch scratch{"koi-anchor-edge"};
  const AsProjectRoot root{scratch.dir};
  std::string error;
  std::shared_ptr<ProjectStore> store = ProjectStore::Open(scratch.dir / "state.db", error);
  EXPECT_TRUE(store != nullptr);
  if (store == nullptr) return;

  // The anchored text twice over, once at line 150 with named neighbours and
  // once at line 401. Cutting the first 148 lines puts the true home on line 2,
  // where two of its four context slots fall off the top of the file -- and a
  // context that dropped them would compare "one above" against "two above" and
  // score the true home at zero.
  const auto build = [](bool decoy_context) {
    std::vector<std::string> out = StepLines(500);
    out[147] = "  prev_a();";
    out[148] = "  prev_b();";
    out[149] = "  shared_body();";
    out[150] = "  next_a();";
    out[151] = "  next_b();";
    out[400] = "  shared_body();";
    if (decoy_context) {
      // Three of the stored context's four entries, which is what rung 6
      // confirms at. Aligned, the true home scores the same and the margin test
      // refuses; misaligned, this one wins on its own.
      out[398] = "  prev_a();";
      out[399] = "  prev_b();";
      out[401] = "  next_a();";
    }
    return out;
  };
  // Everything above the anchor, gone. The row's cached line is then 150 in a
  // file whose line 150 is filler, and both copies are outside every window the
  // position rungs may search from there.
  const auto cut = [](const std::vector<std::string>& all) {
    return Joined(std::vector<std::string>{all.begin() + 148, all.end()});
  };

  {
    const std::vector<std::string> lines = build(false);
    const std::string text = Joined(lines);
    const fs::path source = scratch.Write("edge_alone.txt", text);
    EXPECT_TRUE(SeedRow(*store, source, text, 150) != 0);
    EXPECT_EQ(RowOf(*store, source).content, std::string{"shared_body();"});
    WriteFixtureFile(source, cut(lines));

    const std::shared_ptr<AnchorJob> job = HealFile(*store, source);
    EXPECT_EQ(std::ssize(job->heals), Index{1});
    if (!job->heals.empty()) {
      EXPECT_FALSE(job->heals.front().miss);
      EXPECT_EQ(job->heals.front().rung, 6);
      EXPECT_EQ(job->heals.front().line, Index{2});
    }
    EXPECT_EQ(RowOf(*store, source).line, Index{2});
  }

  TEST_CASE("anchor heal: a decoy that reads as well as the true home is refused");
  {
    const std::vector<std::string> lines = build(true);
    const std::string text = Joined(lines);
    const fs::path source = scratch.Write("edge_decoy.txt", text);
    EXPECT_TRUE(SeedRow(*store, source, text, 150) != 0);
    WriteFixtureFile(source, cut(lines));

    const std::shared_ptr<AnchorJob> job = HealFile(*store, source);
    EXPECT_EQ(std::ssize(job->heals), Index{1});
    if (!job->heals.empty()) {
      // Both candidates read the same, so neither is evidence: a miss is the
      // right answer and line 253 -- the decoy -- is the wrong one.
      EXPECT_TRUE(job->heals.front().miss);
      EXPECT_EQ(job->heals.front().rung, 8);
    }
    const AnchorRow after = RowOf(*store, source);
    EXPECT_EQ(after.line, Index{150});
    EXPECT_EQ(after.misses, std::int64_t{1});
  }
}

void AnchorHealStaleness() {
  const Scratch scratch{"koi-anchor-stale"};
  const AsProjectRoot root{scratch.dir};
  std::string error;
  std::shared_ptr<ProjectStore> store = ProjectStore::Open(scratch.dir / "state.db", error);
  EXPECT_TRUE(store != nullptr);
  if (store == nullptr) return;

  const std::vector<std::string> lines = StepLines(200);
  const std::string text = Joined(lines);
  std::vector<std::string> shifted = lines;
  shifted.insert(shifted.begin() + 10, {"  added_a();", "  added_b();", "  added_c();",
                                        "  added_d();", "  added_e();", "  added_f();",
                                        "  added_g();"});
  const std::string moved = Joined(shifted);

  TEST_CASE("anchor heal: a heal from a stale snapshot does not undo the record that beat it");
  {
    const fs::path source = scratch.Write("stale.txt", text);
    EXPECT_TRUE(SeedRow(*store, source, text, 101) != 0);
    // A second place, recorded after it: the row under test is then not at the
    // front of the list, which is what lets a re-record move its seq at all.
    const fs::path elsewhere = scratch.Write("elsewhere.txt", text);
    EXPECT_TRUE(SeedRow(*store, elsewhere, text, 5) != 0);
    WriteFixtureFile(source, moved);

    // Worked but not applied: this is the window the editor holds open across a
    // read, a `git cat-file` and a parse queued behind other pool work.
    const std::shared_ptr<AnchorJob> job = HealFile(*store, source, {}, false);
    EXPECT_EQ(std::ssize(job->heals), Index{1});
    if (job->heals.empty()) return;
    EXPECT_EQ(job->heals.front().line, Index{108});

    // The user stands in the same place and the recorder merges onto the row:
    // new line, new text, new seq -- and this is the truer of the two answers.
    const AnchorRow before = RowOf(*store, source);
    LocationRecord fresh;
    fresh.path = source.string();
    fresh.line = 105;
    fresh.col = 1;
    fresh.has_text = true;
    fresh.content = "the_user_is_here();";
    fresh.context = "above();\nnearby();\nbelow();\nfurther();";
    EXPECT_TRUE(store->WriteLocation(fresh) != 0);
    const AnchorRow recorded = RowOf(*store, source);
    EXPECT_EQ(recorded.id, before.id);
    EXPECT_TRUE(recorded.seq != before.seq);
    EXPECT_EQ(recorded.line, Index{105});

    // The stale batch lands. It commits -- a skipped row is not a failure --
    // and the row it would have dragged back to 108 is untouched.
    EXPECT_TRUE(store->ApplyHeals(job->heals));
    const AnchorRow after = RowOf(*store, source);
    EXPECT_EQ(after.line, Index{105});
    EXPECT_EQ(after.content, recorded.content);
    EXPECT_EQ(after.seq, recorded.seq);
    EXPECT_EQ(after.misses, std::int64_t{0});

    // And the next job, which snapshots what the recorder wrote, heals it.
    const std::shared_ptr<AnchorJob> again = HealFile(*store, source);
    EXPECT_EQ(std::ssize(again->heals), Index{1});
    EXPECT_TRUE(RowOf(*store, source).line != Index{101});
  }

  TEST_CASE("anchor heal: a write-back that rolled back leaves the shadow where it was");
  {
    const fs::path source = scratch.Write("rollback.txt", text);
    EXPECT_TRUE(SeedRow(*store, source, text, 101) != 0);
    const std::int64_t id = RowOf(*store, source).id;
    WriteFixtureFile(source, moved);

    Editor ed;
    ed.theme = BuiltinTheme();
    ed.project = store;
    EXPECT_FALSE(static_cast<bool>(LoadDocument(source, ed.doc)));
    AdoptAnchorRows(ed, ed.doc);
    StartAnchorHeal(ed, source, {}, false);
    // The store stops taking writes while the job is in flight. A locked
    // database and a full disk both arrive here: the batch rolls back whole.
    EXPECT_TRUE(ExecSql(store->Connection(), "PRAGMA query_only = ON;"));
    PumpUntilIdle(ed);
    EXPECT_TRUE(ExecSql(store->Connection(), "PRAGMA query_only = OFF;"));

    // Nothing was written, so nothing moved -- including the shadow, which is
    // the only other copy of the answer and the one AdoptAnchorRows would keep
    // in preference to the store.
    EXPECT_EQ(RowOf(*store, source).line, Index{101});
    Index line = 0;
    EXPECT_TRUE(AnchorShadowLine(ed, ed.doc, id, line));
    EXPECT_EQ(line, Index{101});

    // With the store taking writes again the same trigger lands both.
    StartAnchorHeal(ed, source, {}, false);
    PumpUntilIdle(ed);
    EXPECT_EQ(RowOf(*store, source).line, Index{108});
    EXPECT_TRUE(AnchorShadowLine(ed, ed.doc, id, line));
    EXPECT_EQ(line, Index{108});
  }

  TEST_CASE("anchor heal: a file past the size cap is gated, never split");
  {
    std::string big;
    big.reserve(static_cast<std::size_t>(kMaxUniqBytes) + 4096);
    for (int i = 0; big.size() <= static_cast<std::size_t>(kMaxUniqBytes); ++i) {
      big += "  const int generated_value_" + std::to_string(i) + " = " + std::to_string(i) +
             ";\n";
    }
    const fs::path huge = scratch.Write("generated.txt", big);
    EXPECT_TRUE(SeedRow(*store, huge, big, 100) != 0);

    // Untouched: rung 1 is a read and a hash, and it still answers.
    const std::shared_ptr<AnchorJob> gated = HealFile(*store, huge);
    EXPECT_TRUE(gated->blob_gate);
    EXPECT_FALSE(gated->too_big);
    EXPECT_TRUE(gated->heals.empty());

    // Changed: nothing left that is cheap, so the job declines rather than
    // splitting and indexing megabytes on a pool thread for every save.
    WriteFixtureFile(huge, "  const int generated_value_first = 1;\n" + big);
    const std::shared_ptr<AnchorJob> declined = HealFile(*store, huge);
    EXPECT_TRUE(declined->too_big);
    EXPECT_FALSE(declined->parsed);
    EXPECT_TRUE(declined->heals.empty());
    EXPECT_EQ(declined->rungs, std::uint32_t{0});
    // Not a miss: the row was not looked for, and counting it would hide it for
    // a fact about the file's size.
    const AnchorRow after = RowOf(*store, huge);
    EXPECT_EQ(after.line, Index{100});
    EXPECT_EQ(after.misses, std::int64_t{0});
  }
}

}
