// Tests for query.cpp: reading a whole file, the byte ranges handed back for
// one, and the two things a pattern can carry besides its captures -- the
// predicates that decide whether it matched, and the `#set!` properties that
// describe it once it has.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {
namespace {

// One query file over one buffer, through Syntax rather than around it: the
// indent algorithm will reach these captures the same way text objects do, so
// the tree, the predicates and the capture names here are all the real ones.
std::vector<Capture> CapturedBy(std::string_view filename, std::string_view query_file,
                                std::string_view source, std::string& error) {
  std::vector<Capture> out;
  const std::shared_ptr<Syntax> syntax = OpenSyntax(std::filesystem::path{filename}, error);
  if (syntax == nullptr) return out;
  PieceTable table;
  ResetToOriginal(table, std::string{source});
  const std::array<std::string_view, 1> files{query_file};
  if (!syntax->Captures(table, files, Interval(0, DocLength(table)), out, error)) out.clear();
  return out;
}

std::ptrdiff_t CountNamed(const std::vector<Capture>& captures, std::string_view name) {
  return std::ranges::count(captures, name, &Capture::name);
}

// Whether `name` was captured on exactly the bytes at `at`, which is how these
// tests say "this pattern fired on that node" without naming a node.
bool CapturedAt(const std::vector<Capture>& captures, std::string_view name, std::size_t at) {
  return std::ranges::any_of(captures, [name, at](const Capture& capture) {
    return (capture.name == name) && (capture.from == static_cast<Index>(at));
  });
}

}  // namespace

void ReadWholeFileContract() {
  TEST_CASE("ReadWholeFile: regular files, precise errors, never a block");

  const Scratch scratch{"koi-readwholefile-test"};
  std::error_code error;

  const std::filesystem::path file = scratch.Write("plain.txt", "alpha\nbravo\n");
  EXPECT_EQ(ReadWholeFile(file, error), std::string("alpha\nbravo\n"));
  EXPECT_TRUE(!error);

  // The one case where an empty answer is the truth, and the only one. Every
  // assertion below is about telling the other cases apart from this one.
  error = std::make_error_code(std::errc::io_error);
  EXPECT_EQ(ReadWholeFile(scratch.Write("empty.txt", ""), error), std::string{});
  EXPECT_TRUE(!error);

  EXPECT_EQ(ReadWholeFile(scratch.dir / "missing.txt", error), std::string{});
  EXPECT_TRUE(error == std::errc::no_such_file_or_directory);

  EXPECT_EQ(ReadWholeFile(scratch.dir, error), std::string{});
  EXPECT_TRUE(error == std::errc::is_a_directory);

  const std::filesystem::path fifo = scratch.dir / "pipe";
  EXPECT_EQ(mkfifo(fifo.c_str(), 0600), 0);
  EXPECT_EQ(ReadWholeFile(fifo, error), std::string{});
  EXPECT_TRUE(error == std::errc::not_supported);

  if (::getuid() == 0) {
    // root reads through the mode bits, so there is no unreadable file to make.
    EXPECT_TRUE(true);
    EXPECT_TRUE(true);
  } else {
    const std::filesystem::path locked = scratch.Write("locked.txt", "secret\n");
    std::error_code chmod_ec;
    std::filesystem::permissions(locked, std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, chmod_ec);
    EXPECT_EQ(ReadWholeFile(locked, error), std::string{});
    EXPECT_TRUE(error == std::errc::permission_denied);
    std::filesystem::permissions(locked, std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace, chmod_ec);
  }

  // The failure class the shim covers from outside, reached here without one.
  // /proc/self/mem is a regular file that fstat sizes at zero and that read(2)
  // refuses with EIO at offset 0 -- so open succeeds, fstat succeeds, and the
  // read is what dies. That is exactly the path that used to return an empty
  // string with `error` left clear, which every caller then read as "this file
  // has nothing in it". No privileges, no injection: the kernel provides it.
  {
    error.clear();
    const std::string mem = ReadWholeFile("/proc/self/mem", error);
    EXPECT_EQ(mem, std::string{});
    EXPECT_TRUE(static_cast<bool>(error));
  }

  // st_size zero and yet not empty: the tail loop past st_size is what /proc
  // files are read by, and a failure-first rewrite of the read loop is the
  // obvious way to lose it.
  {
    error = std::make_error_code(std::errc::io_error);
    const std::string stat = ReadWholeFile("/proc/self/stat", error);
    EXPECT_TRUE(!error);
    EXPECT_TRUE(!stat.empty());
  }
}

void ABufferPastFourGigabytesGetsNoByteRangeAtAll() {
  TEST_CASE("query: a byte range past uint32 is refused, not wrapped");

  // Every offset tree-sitter deals in is a uint32, and the three call sites
  // that set a cursor's byte range each wrote `static_cast<uint32_t>(size)`.
  // Past 4 GiB that is not a cast, it is a wrap: 4 GiB + 100 bytes becomes a
  // range of 100, and the answer -- a scan, a set of text objects, a file's
  // overview -- is about an arbitrary prefix while claiming to be about the
  // whole. No fixture in a test suite can be that large, so the arithmetic is
  // pinned where it now lives, at the one place all three go through.
  constexpr std::size_t kCeiling = std::numeric_limits<std::uint32_t>::max();

  std::uint32_t end = 12345;
  EXPECT_TRUE(TreeSitterByteRange(0, end));
  EXPECT_EQ(end, std::uint32_t{0});
  EXPECT_TRUE(TreeSitterByteRange(1024, end));
  EXPECT_EQ(end, std::uint32_t{1024});

  // The last size that fits, and the first that does not.
  EXPECT_TRUE(TreeSitterByteRange(kCeiling, end));
  EXPECT_EQ(end, std::numeric_limits<std::uint32_t>::max());

  end = 4242;
  EXPECT_FALSE(TreeSitterByteRange(kCeiling + 1, end));
  // Left as it was rather than wrapped to zero: a caller that forgets to look
  // at the answer is handed back the range it already had, not a new and
  // plausible-looking one.
  EXPECT_EQ(end, std::uint32_t{4242});
  EXPECT_FALSE(TreeSitterByteRange(std::numeric_limits<std::size_t>::max(), end));
  EXPECT_EQ(end, std::uint32_t{4242});
}

void QueryPropertiesAreReadOffThePatternThatSetThem() {
  TEST_CASE("query: #set! properties are per-pattern, and are not predicates");

  const FakeQueryDir queries{"c"};
  EXPECT_TRUE(queries.Ready());
  if (!queries.Ready()) return;

  // Three patterns: one with a pattern-scoped property -- the only shape the
  // vendored indents.scm corpus uses -- one with none at all, and one with the
  // capture-scoped shape plus a key that was given no value.
  queries.Write("koi-properties.scm",
                "((if_statement) @indent (#set! \"scope\" \"header\"))\n"
                "(parameter_list) @indent\n"
                "((compound_statement) @outdent\n"
                "  (#set! @outdent \"scope\" \"header\")\n"
                "  (#set! \"bare-key\"))\n");

  std::string error;
  const std::array<std::string_view, 1> files{"koi-properties.scm"};
  const std::shared_ptr<CompiledQuery> compiled = CompileQuery("c", files, error);
  EXPECT_TRUE(compiled != nullptr);
  if (compiled == nullptr) return;
  EXPECT_EQ(error, std::string{});

  const std::span<const QueryProperty> first = PropertiesFor(*compiled, 0);
  EXPECT_EQ(first.size(), std::size_t{1});
  if (first.size() == 1) {
    EXPECT_EQ(first[0].key, std::string{"scope"});
    EXPECT_EQ(first[0].value, std::string{"header"});
  }

  // A pattern that set nothing gets an empty span, not the previous pattern's.
  EXPECT_TRUE(PropertiesFor(*compiled, 1).empty());

  const std::span<const QueryProperty> third = PropertiesFor(*compiled, 2);
  EXPECT_EQ(third.size(), std::size_t{2});
  if (third.size() == 2) {
    // The capture the shape names is skipped, not counted as the key: the
    // property is the same one the pattern-scoped spelling sets, and it applies
    // to the whole pattern. Nothing stores the capture id -- no consumer narrows
    // to it -- so what is pinned here is that the key and the value are still
    // read off the right steps.
    EXPECT_EQ(third[0].key, std::string{"scope"});
    EXPECT_EQ(third[0].value, std::string{"header"});
    // Key with no value: legal in the dialect, and not to be confused with the
    // capture-scoped shape by a parser that counts steps rather than reading
    // their types.
    EXPECT_EQ(third[1].key, std::string{"bare-key"});
    EXPECT_EQ(third[1].value, std::string{});
  }

  // A pattern index no pattern has -- the one a caller reaches by asking about
  // a match from a different query -- is empty rather than out of bounds.
  EXPECT_TRUE(PropertiesFor(*compiled, ts_query_pattern_count(QueryOf(*compiled))).empty());
  EXPECT_TRUE(PropertiesFor(*compiled, 4242).empty());

  // The one property with an answer cached beside the list, because it is asked
  // per capture on the indent path. Same answers as walking the list would
  // give, including for the capture-scoped shape -- which still counts
  // pattern-wide, exactly as the hand-rolled scan it replaces did -- and for an
  // index no pattern has.
  EXPECT_TRUE(ScopeIsHeader(*compiled, 0));
  EXPECT_FALSE(ScopeIsHeader(*compiled, 1));
  EXPECT_TRUE(ScopeIsHeader(*compiled, 2));
  EXPECT_FALSE(ScopeIsHeader(*compiled, ts_query_pattern_count(QueryOf(*compiled))));
  EXPECT_FALSE(ScopeIsHeader(*compiled, 4242));

  // The regression the property plumbing is most likely to cause: `#set!` is
  // not a predicate, so a pattern carrying one still matches everything it
  // names. Parsed as a predicate it would have been an unknown one, skipped --
  // which is the same answer for the wrong reason -- and parsed as a predicate
  // with an operand it does not have, none of these would match at all.
  queries.Write("koi-set-only.scm", "((compound_statement) @body (#set! \"scope\" \"header\"))\n");
  const std::vector<Capture> bodies =
      CapturedBy("props.c", "koi-set-only.scm", "void f(void) { }\n", error);
  EXPECT_EQ(error, std::string{});
  EXPECT_EQ(CountNamed(bodies, "body"), std::ptrdiff_t{1});

  // And the corpus this exists for: the four unbraced-body patterns in the
  // vendored C file each set the scope, and nothing else in it does.
  const std::array<std::string_view, 1> indents{"indents.scm"};
  const std::shared_ptr<CompiledQuery> vendored = CompileQuery("c", indents, error);
  EXPECT_TRUE(vendored != nullptr);
  if (vendored == nullptr) return;
  std::size_t with_scope = 0;
  std::size_t cached = 0;
  for (std::uint32_t pattern = 0; pattern < ts_query_pattern_count(QueryOf(*vendored)); ++pattern) {
    for (const QueryProperty& property : PropertiesFor(*vendored, pattern)) {
      if ((property.key == "scope") && (property.value == "header")) ++with_scope;
    }
    if (ScopeIsHeader(*vendored, pattern)) ++cached;
  }
  EXPECT_EQ(with_scope, std::size_t{4});
  EXPECT_EQ(cached, with_scope);
}

void KindAndLinePredicatesAreAnsweredFromTheTree() {
  TEST_CASE("query: #kind-eq?, #same-line? and #one-line? read the tree, not the buffer");

  const FakeQueryDir queries{"c"};
  EXPECT_TRUE(queries.Ready());
  if (!queries.Ready()) return;

  // The four unbraced-body patterns are the reason this predicate exists:
  // without it they fire on braced bodies too and indent every block twice.
  // The third pattern spells its kind the way helix's dart file does, bare and
  // unquoted, which tree-sitter hands over as a string step all the same.
  queries.Write("koi-kinds.scm",
                "(if_statement consequence: (_) @bare\n"
                "  (#not-kind-eq? @bare \"compound_statement\"))\n"
                "(if_statement consequence: (_) @braced\n"
                "  (#kind-eq? @braced \"compound_statement\"))\n"
                "(if_statement consequence: (_) @unquoted\n"
                "  (#not-kind-eq? @unquoted compound_statement))\n");

  std::string error;
  const std::vector<Capture> unbraced =
      CapturedBy("kinds.c", "koi-kinds.scm", "void f(int a) { if (a) g(); }\n", error);
  EXPECT_EQ(error, std::string{});
  EXPECT_EQ(CountNamed(unbraced, "bare"), std::ptrdiff_t{1});
  EXPECT_EQ(CountNamed(unbraced, "unquoted"), std::ptrdiff_t{1});
  EXPECT_EQ(CountNamed(unbraced, "braced"), std::ptrdiff_t{0});

  const std::vector<Capture> braced =
      CapturedBy("kinds.c", "koi-kinds.scm", "void f(int a) { if (a) { g(); } }\n", error);
  EXPECT_EQ(error, std::string{});
  EXPECT_EQ(CountNamed(braced, "bare"), std::ptrdiff_t{0});
  EXPECT_EQ(CountNamed(braced, "unquoted"), std::ptrdiff_t{0});
  EXPECT_EQ(CountNamed(braced, "braced"), std::ptrdiff_t{1});

  // Rows, from the nodes' own points. Both spellings of both predicates are
  // asked of the same buffer, so a predicate that answered the same either way
  // fails one of the two.
  queries.Write("koi-lines.scm",
                "((binary_expression left: (_) @l right: (_) @r) @joined (#same-line? @l @r))\n"
                "((binary_expression left: (_) @l right: (_) @r) @split (#not-same-line? @l @r))\n"
                "((parameter_list) @flat (#one-line? @flat))\n"
                "((parameter_list) @tall (#not-one-line? @tall))\n");

  const std::string source =
      "int f(int a, int b);\n"
      "int g(\n"
      "    int c);\n"
      "int x = 1 + 2;\n"
      "int y = 3 +\n"
      "        4;\n";
  const std::vector<Capture> lines = CapturedBy("lines.c", "koi-lines.scm", source, error);
  EXPECT_EQ(error, std::string{});

  EXPECT_EQ(CountNamed(lines, "joined"), std::ptrdiff_t{1});
  EXPECT_TRUE(CapturedAt(lines, "joined", source.find("1 + 2")));
  EXPECT_EQ(CountNamed(lines, "split"), std::ptrdiff_t{1});
  EXPECT_TRUE(CapturedAt(lines, "split", source.find("3 +")));

  EXPECT_EQ(CountNamed(lines, "flat"), std::ptrdiff_t{1});
  EXPECT_TRUE(CapturedAt(lines, "flat", source.find("(int a")));
  EXPECT_EQ(CountNamed(lines, "tall"), std::ptrdiff_t{1});
  EXPECT_TRUE(CapturedAt(lines, "tall", source.find("(\n")));
}

void CapturesRememberTheMatchTheyCameFrom() {
  TEST_CASE("query: a capture carries the match and the pattern that produced it");

  const FakeQueryDir queries{"c"};
  EXPECT_TRUE(queries.Ready());
  if (!queries.Ready()) return;

  // The `@align`/`@anchor` shape, which is the whole reason a capture needs to
  // know its match: an align is meaningless except paired with the anchor from
  // the same match, and an indent query has several of both live over one byte.
  queries.Write("koi-align.scm",
                "(parameter_list . (parameter_declaration) @anchor) @align\n"
                "(binary_expression) @op\n");

  const std::string source =
      "int f(int a, int b);\n"
      "int g(int c);\n"
      "int h = 1 + 2;\n";
  std::string error;
  const std::vector<Capture> captures = CapturedBy("align.c", "koi-align.scm", source, error);
  EXPECT_EQ(error, std::string{});

  for (const Capture& capture : captures) {
    const bool from_align_pattern = (capture.name == "align") || (capture.name == "anchor");
    EXPECT_EQ(capture.pattern_index, from_align_pattern ? std::uint32_t{0} : std::uint32_t{1});
  }
  EXPECT_EQ(CountNamed(captures, "align"), std::ptrdiff_t{2});
  EXPECT_EQ(CountNamed(captures, "anchor"), std::ptrdiff_t{2});
  EXPECT_EQ(CountNamed(captures, "op"), std::ptrdiff_t{1});

  // The pairing S4 has to be able to make: for each align, the anchor that
  // came out of the same match, found by match id and by nothing else.
  const auto anchor_of = [&captures](const Capture& align) -> const Capture* {
    for (const Capture& other : captures) {
      if ((other.name == "anchor") && (other.match_id == align.match_id)) return &other;
    }
    return nullptr;
  };

  std::vector<std::uint32_t> align_matches;
  for (const Capture& capture : captures) {
    if (capture.name != "align") continue;
    align_matches.push_back(capture.match_id);
    const Capture* anchor = anchor_of(capture);
    EXPECT_TRUE(anchor != nullptr);
    if (anchor == nullptr) continue;
    // The anchor is the first parameter inside the list the align covers, so
    // the pair reads as "line the contents up under this column".
    EXPECT_TRUE((anchor->from > capture.from) && (anchor->to <= capture.to));
  }

  // Two matches of one pattern are two ids: an id shared by every match of a
  // pattern would pair f's align with g's anchor.
  EXPECT_EQ(align_matches.size(), std::size_t{2});
  if (align_matches.size() == 2) EXPECT_TRUE(align_matches[0] != align_matches[1]);

  EXPECT_TRUE(CapturedAt(captures, "anchor", source.find("int a")));
  EXPECT_TRUE(CapturedAt(captures, "anchor", source.find("int c")));
}

void EveryVendoredIndentQueryCompilesAndItsPredicatesBite() {
  TEST_CASE("query: the vendored indents.scm files compile and their predicates decide matches");

  // Every language koi ships an indents.scm for, compiled against the grammar
  // it ships with it. A predicate spelling this engine does not know is not an
  // error and does not show up here; what does show up is an inherits chain
  // that stopped resolving, or a node type a grammar bump renamed.
  std::size_t compiled_files = 0;
  for (const std::string_view language : KnownLanguages()) {
    if (FindRuntimeFile(std::filesystem::path{"queries"} / language / "indents.scm").empty()) {
      continue;
    }
    std::string error;
    const std::array<std::string_view, 1> files{"indents.scm"};
    const std::shared_ptr<CompiledQuery> compiled = CompileQuery(language, files, error);
    if (compiled == nullptr) std::cout << "  " << language << ": " << error << "\n";
    EXPECT_TRUE(compiled != nullptr);
    ++compiled_files;
  }
  EXPECT_TRUE(compiled_files >= 10);

  // And one of them taken at its word. rust's `#not-same-line?` guards the
  // right-hand side of a `let`: the value is only an indent when it starts on
  // a line of its own, and an integer literal is captured by nothing else in
  // that file, so its presence is the predicate's answer and nothing else's.
  const std::string source =
      "fn main() {\n"
      "    let x = 1;\n"
      "    let y =\n"
      "        2;\n"
      "}\n";
  std::string error;
  const std::vector<Capture> captures = CapturedBy("indent.rs", "indents.scm", source, error);
  EXPECT_EQ(error, std::string{});
  EXPECT_TRUE(!captures.empty());
  EXPECT_FALSE(CapturedAt(captures, "indent", source.find("1;")));
  EXPECT_TRUE(CapturedAt(captures, "indent", source.find("2;")));
}

}  // namespace koi
