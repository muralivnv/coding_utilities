#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include <sys/wait.h>

#include <fstream>
#include <sstream>

#include "input.h"
#include "operation.h"
#include "process.h"
#include "regex.h"
#include "test_harness.h"

using namespace gai;

namespace {

std::string RunSub(const gai::Pcre2Substitution& sub, std::string_view input) {
  static std::string scratch(512, ' ');
  std::string_view result = gai::Substitute(sub, input, scratch);
  return std::string{result};
}

// The same, but keeping what Substitute reported: a match-time failure is a
// returned line plus a message, and only the message tells the two apart.
std::string SubError(const gai::Pcre2Substitution& sub, std::string_view input, std::string* line = nullptr) {
  static std::string scratch(512, ' ');
  std::string error;
  std::string_view result = gai::Substitute(sub, input, scratch, &error);
  if (line != nullptr)
    *line = std::string{result};
  return error;
}

// Collects every line the mmap reader yields. Views point into `content`, which the
// caller owns, so copying to std::string is only for convenient comparison.
std::vector<std::string> ReadAllMapped(std::string_view content, bool read0 = false) {
  gai::InputMemMappedFile reader(content.data(), content.data() + content.size(), read0);
  std::vector<std::string> out;
  while (std::optional<std::string_view> line = reader.GetLine()) out.emplace_back(*line);
  return out;
}

// Runs InputStream over `content` by pointing fd 0 at a temp file. There is no other
// way in: the reader reads fd 0 directly, which is the whole point of it. Copying
// each line out matters here -- the returned views live in a buffer the next
// GetLine() overwrites.
std::vector<std::string> ReadAllViaStdin(std::string_view content, bool read0 = false) {
  const char* tmpdir = std::getenv("TMPDIR");
  std::string tmpl = std::string(tmpdir != nullptr ? tmpdir : "/tmp") + "/gai_test_XXXXXX";
  std::vector<char> path(tmpl.begin(), tmpl.end());
  path.push_back('\0');

  const int fd = mkstemp(path.data());
  if (fd < 0)
    return {};
  if (!content.empty()) {
    const ssize_t written = ::write(fd, content.data(), content.size());
    (void)written;
  }
  ::lseek(fd, 0, SEEK_SET);

  const int saved = ::dup(STDIN_FILENO);
  ::dup2(fd, STDIN_FILENO);

  std::vector<std::string> out;
  {
    gai::InputStream reader(read0);
    while (std::optional<std::string_view> line = reader.GetLine()) out.emplace_back(*line);
  }

  ::dup2(saved, STDIN_FILENO);
  ::close(saved);
  ::close(fd);
  ::unlink(path.data());
  return out;
}

std::vector<MatchSpan> SpansOf(const char* pattern, std::string_view subject, bool jit = false, bool utf = false) {
  std::vector<MatchSpan> spans;
  FindSpans(Regex(Compile(pattern, jit, utf)), subject, spans);
  return spans;
}

// A subject that makes `kCatastrophic` exhaust the match limit. Twenty x's is
// the length that costs more than 100 thousand steps and far less than pcre2's
// own ten million, so it is a match-time error under gai's limits and a slow
// no-match without them -- the same shape TestBacktrackingIsBounded uses.
const char* const kCatastrophic = "(x+x+)+$";
const std::string kBlowsTheLimit = std::string(20, 'x') + "z";

// What one Process() run did: the lines it emitted, the diagnostics it
// produced, and the counters the exit status is built from.
struct ScanResult {
  std::vector<std::string> lines;
  std::vector<size_t> linenums;
  std::vector<std::string> diagnostics;
  bool any_error{false};
  size_t reported{0};     // distinct failures printed
  size_t occurrences{0};  // lines that ran into one
};

struct ScanRequest {
  std::string content;
  std::vector<std::string_view> filters{};
  std::vector<std::string_view> excludes{};
  std::vector<std::string_view> replacements{};
  std::string_view range{};
  bool color{false};
  bool jit{false};
  std::string_view filename{};
};

ScanResult RunScan(const ScanRequest& request) {
  ScanResult out;
  const std::vector<Pcre2Regex> filters = ParseFilters(request.filters, request.jit, false);
  const std::vector<Pcre2Regex> excludes = ParseFilters(request.excludes, request.jit, false);
  const std::vector<Pcre2Substitution> replacements = ParseSubstitutions(request.replacements, request.jit, false);
  std::optional<Range> range = ParseRange(request.range, request.jit, false);

  MatchDiagnostics diagnostics([&out](std::string_view text) { out.diagnostics.emplace_back(text); });
  InputMemMappedFile input(request.content.data(), request.content.data() + request.content.size(), false);
  const ScanConfig config{filters, excludes, replacements, request.color, request.filename};
  Process(
      config,
      [&out](std::string_view line, size_t linenum) {
        out.lines.emplace_back(line);
        out.linenums.push_back(linenum);
      },
      range, &input, diagnostics);

  out.any_error = diagnostics.Any();
  out.reported = diagnostics.reported();
  out.occurrences = diagnostics.occurrences();
  return out;
}

bool Mentions(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

// The gai binary sits next to this test binary in the build tree; CMake makes
// gai_tests depend on it so it is always there to run.
std::string GaiBinaryPath() {
  char buf[4096];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0)
    return {};
  std::string path(buf, static_cast<size_t>(n));
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos)
    return {};
  return path.substr(0, slash + 1) + "gai";
}

std::string TempPath(std::string_view tag) {
  const char* tmpdir = std::getenv("TMPDIR");
  std::string tmpl = std::string(tmpdir != nullptr ? tmpdir : "/tmp") + "/gai_cli_" + std::string(tag) + "_XXXXXX";
  std::vector<char> path(tmpl.begin(), tmpl.end());
  path.push_back('\0');
  const int fd = mkstemp(path.data());
  if (fd >= 0)
    ::close(fd);
  return std::string(path.data());
}

std::string ReadWholeFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

struct CliResult {
  int status{-1};
  std::string out;
  std::string err;
};

// Runs the real CLI end to end: only the process itself can be asked what its
// exit status is, and stderr is a channel no in-process call exercises.
CliResult RunCli(std::string_view args, std::string_view stdin_content) {
  CliResult result;
  const std::string in_path = TempPath("in");
  const std::string out_path = TempPath("out");
  const std::string err_path = TempPath("err");
  {
    std::ofstream in(in_path, std::ios::binary);
    in.write(stdin_content.data(), static_cast<std::streamsize>(stdin_content.size()));
  }

  const std::string command = GaiBinaryPath() + " " + std::string(args) + " < " + in_path + " > " + out_path + " 2> " +
                              err_path;
  const int raw = std::system(command.c_str());
  if (WIFEXITED(raw))
    result.status = WEXITSTATUS(raw);
  result.out = ReadWholeFile(out_path);
  result.err = ReadWholeFile(err_path);

  ::unlink(in_path.c_str());
  ::unlink(out_path.c_str());
  ::unlink(err_path.c_str());
  return result;
}

size_t CountLines(const std::string& text) {
  size_t n = 0;
  for (const char c : text) {
    if (c == '\n')
      ++n;
  }
  return n;
}

}  // namespace

// ============================================================================
// operation.h -- expression parsing
// ============================================================================
static void TestTrimAndSplit() {
  TEST_CASE("Trim");
  EXPECT_EQ(Trim("  hello  "), "hello");
  EXPECT_EQ(Trim(""), "");
  EXPECT_EQ(Trim("   "), "");
  EXPECT_EQ(Trim("hello"), "hello");
  EXPECT_EQ(Trim("\t\nhello\r\n"), "hello");
  EXPECT_EQ(Trim("a b"), "a b");  // interior space is content

  TEST_CASE("Split takes its delimiter from the first character");
  {
    auto parts = Split("@one@two@three@");
    EXPECT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "one");
    EXPECT_EQ(parts[1], "two");
    EXPECT_EQ(parts[2], "three");
  }
  {
    // A different delimiter works the same way -- this is why /a/b/ and @a@b@ are
    // both valid substitution syntax.
    auto parts = Split("/one/two/");
    EXPECT_EQ(parts.size(), 2u);
    EXPECT_EQ(parts[0], "one");
    EXPECT_EQ(parts[1], "two");
  }
  {
    // Leading whitespace is trimmed first, so '$' becomes the delimiter here and
    // there is no second '$' to close a field.
    auto parts = Split("   $one@two/three@  ");
    EXPECT_EQ(parts.size(), 0u);
  }
  {
    EXPECT_EQ(Split("").size(), 0u);
    EXPECT_EQ(Split("@").size(), 0u);
    // Only delimiter-terminated fields count; a trailing tail is dropped, which is
    // what leaves substitution flags for ParseSub to read off the raw expression.
    auto parts = Split("/a/b/g");
    EXPECT_EQ(parts.size(), 2u);
    // Empty fields are preserved.
    auto empties = Split("@@@");
    EXPECT_EQ(empties.size(), 2u);
    EXPECT_TRUE(empties[0].empty());
  }
}

static void TestCompileAndFind() {
  TEST_CASE("Compile rejects malformed patterns");
  EXPECT_THROWS(Compile("invalid[regex", false, false));
  EXPECT_THROWS(Compile("(unclosed", false, false));
  EXPECT_THROWS(Compile("a{99,1}", false, false));
  EXPECT_NO_THROW(Compile("", false, false));  // empty is a legal pattern

  TEST_CASE("Find without JIT");
  {
    auto regex = Regex(Compile("hello", false, false));
    EXPECT_FALSE(regex.re.jitted);
    EXPECT_TRUE(Find(regex, "hello world"));
    EXPECT_FALSE(Find(regex, "goodbye world"));
    EXPECT_FALSE(Find(regex, ""));
  }

  TEST_CASE("asking for JIT never fails the compile");
  // Whether PCRE2 has a JIT is a property of the build and the kernel, not of
  // the pattern: built without JIT support, or running where W^X mappings are
  // refused, pcre2_jit_compile answers BADOPTION for everything. That has to
  // fall back to the interpreter -- Find and FindSpans both branch on `jitted`
  // -- and not throw, which used to make every search return nothing at all.
  EXPECT_NO_THROW(Compile("world", true, false));
  const bool jit_available = Regex(Compile("world", true, false)).re.jitted;

  TEST_CASE("Find with JIT gives the same answers");
  {
    auto regex = Regex(Compile("world", true, false));
    EXPECT_EQ(regex.re.jitted, jit_available);
    EXPECT_TRUE(Find(regex, "hello world"));
    EXPECT_TRUE(Find(regex, "goodbyeworld"));
  }
  {
    auto regex = Regex(Compile("\\bworld\\b", true, false));
    EXPECT_EQ(regex.re.jitted, jit_available);
    EXPECT_TRUE(Find(regex, "hello world"));
    EXPECT_FALSE(Find(regex, "goodbyeworld"));
  }

  TEST_CASE("captures, named captures and repeats");
  {
    auto regex = Regex(Compile("(\\d+)-(\\w+)", false, false));
    EXPECT_TRUE(Find(regex, "123-abc"));
    EXPECT_FALSE(Find(regex, "!!!"));
  }
  {
    auto regex = Regex(Compile("(?<num>\\d+)-(?<word>\\w+)", true, false));
    EXPECT_TRUE(Find(regex, "456-def"));
  }
  {
    auto regex = Regex(Compile("(ha){2,4}", false, false));
    EXPECT_TRUE(Find(regex, "hahaha"));
    EXPECT_TRUE(Find(regex, "hahahaha"));
    EXPECT_FALSE(Find(regex, "ha"));
  }

  TEST_CASE("utf mode enables unicode properties");
  {
    auto regex = Regex(Compile("\\p{L}+", false, true));
    EXPECT_TRUE(Find(regex, "こんにちは"));
    EXPECT_TRUE(Find(regex, "hello"));
    EXPECT_FALSE(Find(regex, "12345"));
  }

  TEST_CASE("a long subject and a long pattern");
  {
    const std::string long_str(10000, 'a');
    auto regex = Regex(Compile("a{10000}", false, false));
    EXPECT_TRUE(Find(regex, long_str));
    auto jitted = Regex(Compile("a{10000}", true, false));
    EXPECT_TRUE(Find(jitted, long_str));
  }
}

static void TestSubstitute() {
  TEST_CASE("basic substitution");
  {
    auto sub = Pcre2Substitution(Compile("world", false, false), "Earth");
    EXPECT_EQ(RunSub(sub, "hello world"), "hello Earth");
    EXPECT_EQ(RunSub(sub, "no match"), "no match");
    EXPECT_EQ(RunSub(sub, ""), "");
  }

  TEST_CASE("capture references");
  {
    auto sub = Pcre2Substitution(Compile("(\\d+)-(\\w+)", false, false), "$2:$1");
    EXPECT_EQ(RunSub(sub, "123-abc"), "abc:123");
  }
  {
    auto sub = Pcre2Substitution(Compile("(?<num>\\d+)-(?<word>\\w+)", false, false), "${word}:${num}");
    EXPECT_EQ(RunSub(sub, "456-def"), "def:456");
  }
  {
    auto sub = Pcre2Substitution(Compile("aa(.*)", false, false), "X$1");
    EXPECT_EQ(RunSub(sub, "aaaa"), "Xaa");
  }
  {
    auto sub = Pcre2Substitution(Compile("([\\p{L}]+)", false, true), "[$1]");
    EXPECT_EQ(RunSub(sub, "hello"), "[hello]");
    EXPECT_EQ(RunSub(sub, "こんにちは"), "[こんにちは]");
  }

  TEST_CASE("an unknown capture is an error, not a silent empty");
  {
    auto sub = Pcre2Substitution(Compile("a", false, false), "$9");
    EXPECT_THROWS(RunSub(sub, "a"));
  }

  TEST_CASE("first match only unless the g flag is set");
  {
    auto first = Pcre2Substitution(Compile("a", false, false), "X");
    EXPECT_EQ(RunSub(first, "aaaa"), "Xaaa");
    auto global = Pcre2Substitution(Compile("a", false, false), "X", true);
    EXPECT_EQ(RunSub(global, "aaaa"), "XXXX");
  }

  TEST_CASE("an empty match substitutes without looping");
  {
    auto sub = Pcre2Substitution(Compile("a*", false, false), "X");
    EXPECT_EQ(RunSub(sub, ""), "X");
    auto global = Pcre2Substitution(Compile("b*", false, false), "-", true);
    EXPECT_EQ(RunSub(global, "aa"), "-a-a-");
  }

  TEST_CASE("the scratch buffer grows instead of aborting the run");
  {
    // Result is 24x the buffer, so Substitute must resize and retry.
    std::string scratch(8, ' ');
    auto sub = Pcre2Substitution(Compile("a", false, false), "bbb", true);
    EXPECT_EQ(Substitute(sub, std::string(64, 'a'), scratch), std::string(192, 'b'));
  }
  {
    // A single substitution on a subject far larger than the buffer.
    std::string scratch(4, ' ');
    auto sub = Pcre2Substitution(Compile("^", false, false), "X");
    EXPECT_EQ(Substitute(sub, std::string(5000, 'a'), scratch), "X" + std::string(5000, 'a'));
  }
  {
    // Shrinking results must not be confused by a buffer that is already big.
    std::string scratch(4096, ' ');
    auto sub = Pcre2Substitution(Compile("a{10000}", false, false), "b");
    EXPECT_EQ(Substitute(sub, std::string(10000, 'a'), scratch), "b");
  }

  TEST_CASE("a catastrophic replacement pattern is stopped by the match limit, on both engines");
  {
    // Substituting matches per line exactly as filtering does, so the same
    // twenty x's that tell the Find side's budgets apart tell these apart:
    // over a hundred thousand steps and far short of ten million. With the
    // thread's match context the call is a reported error; with pcre2's own
    // default it grinds through the ten million and reports a silent no-match.
    // Nothing is timed -- which budget applies is the observable.
    for (const bool jit : {false, true}) {
      auto sub = Pcre2Substitution(Compile(kCatastrophic, jit, false), "Y");
      std::string line;
      const std::string error = SubError(sub, kBlowsTheLimit, &line);
      EXPECT_TRUE(error.find("match limit") != std::string::npos);
      // Fail-open: pcre2 abandons the whole call, so the line is handed back
      // as it arrived rather than half-substituted.
      EXPECT_EQ(line, kBlowsTheLimit);
    }
  }

  TEST_CASE("a match-time failure is reported, not thrown");
  {
    // (*LIMIT_MATCH=1) is the deterministic route to the same error, and the
    // one that does not depend on the context's own cap: pcre2 honours a
    // pattern's limit whenever it is the lower of the two.
    const std::string subject = std::string(8, 'a') + "b";
    for (const bool jit : {false, true}) {
      auto sub = Pcre2Substitution(Compile("(*LIMIT_MATCH=1)a*ab", jit, false), "X");
      EXPECT_NO_THROW(RunSub(sub, subject));
      EXPECT_EQ(RunSub(sub, subject), subject);
      EXPECT_FALSE(SubError(sub, subject).empty());
    }
  }

  TEST_CASE("error is left untouched on a substitution and on a clean no-match");
  {
    auto sub = Pcre2Substitution(Compile("world", false, false), "Earth");
    std::string line;
    EXPECT_EQ(SubError(sub, "hello world", &line), std::string{});
    EXPECT_EQ(line, "hello Earth");
    EXPECT_EQ(SubError(sub, "no match", &line), std::string{});
    EXPECT_EQ(line, "no match");
  }

  TEST_CASE("a replacement pcre2 will not accept is still fatal, error sink or not");
  {
    // The line-by-line fail-open is for failures this subject caused. A
    // replacement naming a group that does not exist fails on every line there
    // will ever be, so it stays an exception rather than a per-line report.
    auto sub = Pcre2Substitution(Compile("a", false, false), "$9");
    EXPECT_THROWS(SubError(sub, "a"));
  }
}

static void TestFindSpans() {
  TEST_CASE("FindSpans reports every match");
  {
    auto spans = SpansOf("a", "banana");
    EXPECT_EQ(spans.size(), 3u);
    EXPECT_EQ(spans[0].start, 1u);
    EXPECT_EQ(spans[0].end, 2u);
    EXPECT_EQ(spans[2].start, 5u);
    EXPECT_EQ(spans[2].end, 6u);
  }
  EXPECT_EQ(SpansOf("a", "banana", true).size(), 3u);  // same under JIT
  EXPECT_EQ(SpansOf("zzz", "banana").size(), 0u);
  EXPECT_EQ(SpansOf("a", "").size(), 0u);

  TEST_CASE("zero-length matches terminate and land on every position");
  {
    auto spans = SpansOf("a*", "aaa");
    EXPECT_EQ(spans.size(), 2u);  // [0,3) then the empty match at 3
    EXPECT_EQ(spans[0].start, 0u);
    EXPECT_EQ(spans[0].end, 3u);
    EXPECT_EQ(spans[1].start, 3u);
    EXPECT_EQ(spans[1].end, 3u);
  }
  EXPECT_EQ(SpansOf("b*", "aaa").size(), 4u);  // empty at 0,1,2,3
  EXPECT_EQ(SpansOf("a*", "").size(), 1u);
  EXPECT_EQ(SpansOf("", "abc").size(), 4u);
  EXPECT_EQ(SpansOf("^", "abc").size(), 1u);
  EXPECT_EQ(SpansOf("$", "abc").size(), 1u);
  EXPECT_EQ(SpansOf("\\b", "ab cd").size(), 4u);
  EXPECT_EQ(SpansOf("b*", "aaa", true).size(), 4u);  // and under JIT

  TEST_CASE("utf offsets are byte offsets and steps clear whole code points");
  {
    // "aé" is 3 bytes; the empty-match step must skip the continuation byte, so the
    // matches land at 0, 1 and 3 -- never at 2.
    auto spans = SpansOf("b*", "aé", false, true);
    EXPECT_EQ(spans.size(), 3u);
    EXPECT_EQ(spans[0].start, 0u);
    EXPECT_EQ(spans[1].start, 1u);
    EXPECT_EQ(spans[2].start, 3u);
  }
  {
    auto spans = SpansOf("é", "aéb", false, true);
    EXPECT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].start, 1u);
    EXPECT_EQ(spans[0].end, 3u);
  }

  TEST_CASE("spans accumulate across patterns");
  {
    std::vector<MatchSpan> spans;
    EXPECT_EQ(FindSpans(Regex(Compile("ab", false, false)), "abcXab", spans), 2u);
    EXPECT_EQ(FindSpans(Regex(Compile("bc", false, false)), "abcXab", spans), 1u);
    EXPECT_EQ(spans.size(), 3u);
    MergeSpans(spans);
    EXPECT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0].start, 0u);
    EXPECT_EQ(spans[0].end, 3u);
    EXPECT_EQ(spans[1].start, 4u);
    EXPECT_EQ(spans[1].end, 6u);
  }
  {
    // Touching spans collapse all the way: every byte of "abcab" is covered.
    std::vector<MatchSpan> spans;
    FindSpans(Regex(Compile("ab", false, false)), "abcab", spans);
    FindSpans(Regex(Compile("bc", false, false)), "abcab", spans);
    MergeSpans(spans);
    EXPECT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].start, 0u);
    EXPECT_EQ(spans[0].end, 5u);
  }

  TEST_CASE("MergeSpans sorts, merges and keeps gaps");
  {
    std::vector<MatchSpan> spans{{5, 7}, {0, 2}, {1, 3}, {6, 9}, {20, 20}};
    MergeSpans(spans);
    EXPECT_EQ(spans.size(), 3u);
    EXPECT_EQ(spans[0].start, 0u);
    EXPECT_EQ(spans[0].end, 3u);
    EXPECT_EQ(spans[1].start, 5u);
    EXPECT_EQ(spans[1].end, 9u);
    EXPECT_EQ(spans[2].start, 20u);
  }
  {
    std::vector<MatchSpan> nested{{0, 10}, {2, 4}};
    MergeSpans(nested);
    EXPECT_EQ(nested.size(), 1u);
    EXPECT_EQ(nested[0].end, 10u);
  }
  {
    std::vector<MatchSpan> touching{{0, 2}, {2, 4}};
    MergeSpans(touching);
    EXPECT_EQ(touching.size(), 1u);
    EXPECT_EQ(touching[0].end, 4u);
  }
  {
    std::vector<MatchSpan> one{{3, 4}};
    MergeSpans(one);
    EXPECT_EQ(one.size(), 1u);
    std::vector<MatchSpan> none;
    MergeSpans(none);
    EXPECT_TRUE(none.empty());
  }
}

// ============================================================================
// round 7 -- one answer whichever engine runs, and match-time errors that
// do not pass for "nothing found"
// ============================================================================
static void TestBothEnginesAgreeOnIllFormedSubjects() {
  TEST_CASE("a byte that is not UTF-8 does not hide the ASCII match next to it");

  // Compiling with jit=false is the only way to reach the interpreter: once
  // pcre2_jit_compile has succeeded, pcre2_match dispatches to the JIT itself,
  // so flipping the `jitted` flag on a JIT-compiled pattern changes nothing.
  // These two Compile calls are the two engines.
  const std::string needle = "needle";
  struct Case {
    const char* name;
    std::string subject;
    bool expect_hit;
  };
  const std::vector<Case> cases{
      {"valid utf-8", std::string("caf\xC3\xA9 needle here"), true},
      {"latin-1 byte before the needle", std::string("caf\xE9 needle here"), true},
      {"stray continuation byte before the needle", std::string("\x80 needle here"), true},
      {"truncated sequence before the needle", std::string("\xE2\x82 needle here"), true},
      {"bad byte after the match", std::string("needle here caf\xE9"), true},
      {"bad byte glued to the needle", std::string("\xE9") + "needle here", true},
      {"bad byte on both sides", std::string("\xF0 needle \xC0 here"), true},
      {"genuinely absent", std::string("caf\xE9 nothing here"), false},
  };

  for (const Case& one : cases) {
    for (const bool jit : {false, true}) {
      Pcre2Regex regex = Regex(Compile(needle.c_str(), jit, true));
      std::vector<MatchSpan> spans;
      std::string error;
      const size_t found = FindSpans(regex, one.subject, spans, &error);

      // Never an error: a subject that is not UTF-8 is content to scan, not a
      // failure, and "genuinely absent" must come back as a clean no-match.
      EXPECT_EQ(error, std::string{});
      EXPECT_EQ(found, one.expect_hit ? 1u : 0u);
      if (one.expect_hit && (spans.size() == 1u)) {
        const size_t at = one.subject.find(needle);
        EXPECT_EQ(spans[0].start, static_cast<uint32_t>(at));
        EXPECT_EQ(spans[0].end, static_cast<uint32_t>(at + needle.size()));
      }
      // Find agrees with FindSpans, on both engines.
      std::string find_error;
      EXPECT_EQ(Find(regex, one.subject, &find_error), one.expect_hit);
      EXPECT_EQ(find_error, std::string{});
      (void)one.name;
    }
  }

  // A pattern with no ill-formed bytes anywhere still behaves; the option is
  // not a licence to match rubbish.
  {
    std::vector<MatchSpan> spans;
    std::string error;
    EXPECT_EQ(FindSpans(Regex(Compile("\\w+", false, true)), std::string("a\xE9"), spans, &error), 1u);
    EXPECT_EQ(error, std::string{});
    EXPECT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].start, 0u);
    EXPECT_EQ(spans[0].end, 1u);  // the lone 0xE9 is not a word character, it is not anything
  }
}

static void TestMatchTimeErrorsAreReported() {
  TEST_CASE("a match-time failure is reported, not returned as an empty result");

  // (*LIMIT_MATCH=1) is the one deterministic, fast route to a match-time
  // error: `a*ab` over a run of a's backtracks past the limit on the first
  // attempt, and both the interpreter and the JIT count it the same way.
  const std::string subject = std::string(8, 'a') + "b";
  for (const bool jit : {false, true}) {
    Pcre2Regex regex = Regex(Compile("(*LIMIT_MATCH=1)a*ab", jit, false));

    std::vector<MatchSpan> spans;
    std::string error;
    EXPECT_EQ(FindSpans(regex, subject, spans, &error), 0u);
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(error.find("match limit") != std::string::npos);

    // Find reports it too, and says false -- a failure is not a match.
    std::string find_error;
    EXPECT_FALSE(Find(regex, subject, &find_error));
    EXPECT_FALSE(find_error.empty());

    // A caller that passes nothing still compiles and still gets no match.
    std::vector<MatchSpan> ignored;
    EXPECT_EQ(FindSpans(regex, subject, ignored), 0u);
    EXPECT_FALSE(Find(regex, subject));
  }

  TEST_CASE("error is left untouched on a match and on a clean no-match");
  for (const bool jit : {false, true}) {
    Pcre2Regex regex = Regex(Compile("needle", jit, true));
    std::vector<MatchSpan> spans;
    std::string error = "sentinel";
    EXPECT_EQ(FindSpans(regex, "a needle here", spans, &error), 1u);
    EXPECT_EQ(error, std::string("sentinel"));
    spans.clear();
    EXPECT_EQ(FindSpans(regex, "nothing here", spans, &error), 0u);
    EXPECT_EQ(error, std::string("sentinel"));
  }

  TEST_CASE("what was found before the failure is still returned");
  for (const bool jit : {false, true}) {
    // The leading `x` matches inside the budget; the scan that resumes after it
    // spends the rest on `a*ab` backtracking and blows the limit. The span
    // already collected stays, and `error` is what says the list is a prefix.
    Pcre2Regex regex = Regex(Compile("(*LIMIT_MATCH=2)x|a*ab", jit, false));
    std::vector<MatchSpan> spans;
    std::string error;
    const size_t found = FindSpans(regex, "x" + std::string(20, 'a') + "b", spans, &error);
    EXPECT_EQ(found, 1u);
    EXPECT_EQ(found, spans.size());
    EXPECT_TRUE(!spans.empty() && (spans[0].start == 0u) && (spans[0].end == 1u));
    EXPECT_FALSE(error.empty());
  }
}

static void TestBacktrackingIsBounded() {
  TEST_CASE("a catastrophic pattern is stopped by the match limit, on both engines");

  // With no limit on the match context pcre2 allows ten million match steps,
  // which does terminate -- at ~35 ms per subject under the JIT and ~275 ms
  // under the interpreter, as a constant cost rather than as a failure. A
  // pattern is run once per line of every file gai scans, and once per line of
  // the document on every keystroke typed into koi's search prompt, so ten
  // million steps a subject is measured in minutes of unkillable wait.
  //
  // Twenty x's is the length that tells the two budgets apart: `(x+x+)+$`
  // needs somewhere over 100 thousand steps to give up on it and well under
  // ten million, so it is an error here and a slow, silent no-match if the
  // limits come off. Nothing is timed -- which budget applies is the
  // observable.
  const std::string subject = std::string(20, 'x') + "z";
  for (const bool jit : {false, true}) {
    Pcre2Regex regex = Regex(Compile("(x+x+)+$", jit, true));
    std::vector<MatchSpan> spans;
    std::string error;
    EXPECT_EQ(FindSpans(regex, subject, spans, &error), 0u);
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(error.find("match limit") != std::string::npos);

    std::string find_error;
    EXPECT_FALSE(Find(regex, subject, &find_error));
    EXPECT_TRUE(find_error.find("match limit") != std::string::npos);
  }

  TEST_CASE("the limits leave ordinary work alone, and leave the engines agreeing");
  {
    // The depth limit is the one that has to stay generous. The JIT ignores it
    // outright, so any depth low enough to bite is a pattern that matches under
    // the JIT and reports an error under the interpreter -- the same
    // engine-dependent answer the invalid-UTF fix was about. `(word ?)+` over a
    // three-thousand-word line nests a frame per word and is a search someone
    // would really type; at a depth limit of 2000 the two engines disagreed
    // about it, which is why the depth limit is set no lower than the match
    // limit that will always fire first.
    std::string line;
    for (int i = 0; i < 3000; ++i) line += "word ";
    for (const bool jit : {false, true}) {
      for (const char* pattern : {"\\w+", "(word ?)+", "(\\w+\\s*)+$", "[a-z]+"}) {
        Pcre2Regex regex = Regex(Compile(pattern, jit, true));
        std::vector<MatchSpan> spans;
        std::string error;
        EXPECT_TRUE(FindSpans(regex, line, spans, &error) > 0u);
        EXPECT_EQ(error, std::string{});
      }
    }
  }
}

static void TestParsers() {
  TEST_CASE("ParseSub");
  {
    auto sub = ParseSub("@(\\d+)-(\\w+)@$2:$1@", false, false);
    EXPECT_TRUE(sub.has_value());
    EXPECT_EQ(RunSub(*sub, "42-foo"), "foo:42");
    EXPECT_FALSE(sub->global);
  }
  {
    auto sub = ParseSub("/a/X/g", false, false);
    EXPECT_TRUE(sub.has_value() && sub->global);
    EXPECT_EQ(RunSub(*sub, "aaaa"), "XXXX");
  }
  {
    auto plain = ParseSub("/a/X/", false, false);
    EXPECT_TRUE(plain.has_value() && !plain->global);
    EXPECT_EQ(RunSub(*plain, "aaaa"), "Xaaa");
  }
  // Leading/trailing whitespace around the whole expression is tolerated.
  EXPECT_NO_THROW(ParseSub("  /a/b/  ", false, false));

  TEST_CASE("ParseSub rejects malformed expressions");
  EXPECT_THROWS(ParseSub("@\\d-@$1", false, false));  // replacement not terminated
  EXPECT_THROWS(ParseSub("nodels", false, false));    // no delimiter at all
  EXPECT_THROWS(ParseSub("", false, false));
  EXPECT_THROWS(ParseSub("/a/X/z", false, false));   // unknown flag
  EXPECT_THROWS(ParseSub("/a/X/gg2", false, false));  // '2' is not a flag
  EXPECT_THROWS(ParseSub("/bad[/X/", false, false));  // pattern does not compile

  TEST_CASE("ParseFilters and ParseSubstitutions");
  {
    auto filters = ParseFilters({"a", "b", "c"}, false, false);
    EXPECT_EQ(filters.size(), 3u);
    EXPECT_TRUE(Find(filters[0], "xax"));
    EXPECT_TRUE(Find(filters[2], "xcx"));
    EXPECT_TRUE(ParseFilters({}, false, false).empty());
    EXPECT_THROWS(ParseFilters({"good", "bad["}, false, false));
  }
  {
    auto subs = ParseSubstitutions({"/a/1/", "/b/2/g"}, false, false);
    EXPECT_EQ(subs.size(), 2u);
    EXPECT_FALSE(subs[0].global);
    EXPECT_TRUE(subs[1].global);
    EXPECT_TRUE(ParseSubstitutions({}, false, false).empty());
  }

  TEST_CASE("ParseRange");
  {
    auto range = ParseRange("@2@4@", false, false);
    EXPECT_TRUE(range.has_value());
    EXPECT_EQ(std::get<size_t>(range->start), 2u);
    EXPECT_EQ(std::get<size_t>(range->end), 4u);
  }
  {
    auto range = ParseRange("@hello@world@", false, false);
    EXPECT_TRUE(range.has_value());
    EXPECT_TRUE(range->IsStartReached("hello", 1));
    range->Reset();
    EXPECT_FALSE(range->IsStartReached("hellw", 1));
    range->Reset();
    EXPECT_TRUE(range->IsEndReached("worldwow", 2));
    range->Reset();
    EXPECT_FALSE(range->IsEndReached("weewe", 2));
  }
  EXPECT_TRUE(ParseRange("@1@end@", false, false).has_value());  // mixed forms
  EXPECT_TRUE(ParseRange("", false, false) == std::nullopt);     // no range requested
  EXPECT_THROWS(ParseRange("@start@", false, false));            // only one field

  TEST_CASE("Range latches once reached");
  {
    Range r;
    r.start = Regex(Compile("start", false, false));
    r.end = Regex(Compile("end", false, false));
    EXPECT_FALSE(r.IsStartReached("no match", 1));
    EXPECT_TRUE(r.IsStartReached("this is start line", 1));
    // Latched: a later non-matching line stays inside the range.
    EXPECT_TRUE(r.IsStartReached("nothing here", 2));
    EXPECT_FALSE(r.IsEndReached("no match", 2));
    EXPECT_TRUE(r.IsEndReached("end of line", 3));
    EXPECT_TRUE(r.IsEndReached("anything", 4));
    r.Reset();
    EXPECT_FALSE(r.IsStartReached("nothing", 5));
  }
  {
    // A monostate start means "from the beginning"; a monostate end means "forever".
    Range r;
    EXPECT_TRUE(r.IsStartReached("anything", 1));
    EXPECT_FALSE(r.IsEndReached("anything", 1));
  }
  {
    // Line-number bounds compare against the counter, not the text.
    Range r;
    r.start = size_t{2};
    r.end = size_t{3};
    EXPECT_FALSE(r.IsStartReached("a", 1));
    EXPECT_TRUE(r.IsStartReached("b", 2));
    EXPECT_FALSE(r.IsEndReached("b", 2));
    EXPECT_TRUE(r.IsEndReached("c", 3));
  }
}

// ============================================================================
// input.h -- the two readers must agree, byte for byte
// ============================================================================
static void TestInputReaders() {
  TEST_CASE("mmap reader splits on the delimiter");
  {
    auto lines = ReadAllMapped("a\nb\nc\n");
    EXPECT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "a");
    EXPECT_EQ(lines[2], "c");
  }
  {
    // A trailing line with no delimiter is still a line.
    auto lines = ReadAllMapped("a\nb\nc");
    EXPECT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[2], "c");
  }
  {
    EXPECT_EQ(ReadAllMapped("").size(), 0u);
    // Blank lines are preserved as empty records, unlike tooey's item splitter.
    auto blanks = ReadAllMapped("\n\n\n");
    EXPECT_EQ(blanks.size(), 3u);
    EXPECT_TRUE(blanks[0].empty());
    auto one = ReadAllMapped("solo");
    EXPECT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0], "solo");
  }
  {
    auto nul = ReadAllMapped(std::string_view("a\0b\0", 4), true);
    EXPECT_EQ(nul.size(), 2u);
    EXPECT_EQ(nul[0], "a");
    // With --read0 a newline is payload, not a separator.
    auto embedded = ReadAllMapped(std::string_view("a\nb\0c\0", 6), true);
    EXPECT_EQ(embedded.size(), 2u);
    EXPECT_EQ(embedded[0], "a\nb");
  }

  TEST_CASE("stdin reader agrees with the mmap reader");
  for (std::string_view content : {"a\nb\nc\n", "a\nb\nc", "", "\n\n\n", "solo", "x\n"}) {
    const auto mapped = ReadAllMapped(content);
    const auto piped = ReadAllViaStdin(content);
    EXPECT_EQ(piped.size(), mapped.size());
    for (size_t i = 0; i < mapped.size() && i < piped.size(); ++i) EXPECT_EQ(piped[i], mapped[i]);
  }
  {
    const std::string_view nul("a\0b\0", 4);
    const auto mapped = ReadAllMapped(nul, true);
    const auto piped = ReadAllViaStdin(nul, true);
    EXPECT_EQ(piped.size(), mapped.size());
    EXPECT_TRUE(!piped.empty() && piped[0] == "a");
  }

  TEST_CASE("stdin reader reassembles records that straddle a refill");
  {
    // The reader refills in 64 KiB windows, so these lines cross the boundary and
    // exercise the carry buffer. A line several windows long is the case that broke
    // when this was written.
    std::string content;
    content.append(70000, 'q');
    content.append("MARK\n");
    content.append("short\n");
    content.append(200000, 'x');
    content.append("END\n");
    content.append("tail\n");

    const auto piped = ReadAllViaStdin(content);
    const auto mapped = ReadAllMapped(content);
    EXPECT_EQ(piped.size(), 4u);
    EXPECT_EQ(piped.size(), mapped.size());
    EXPECT_EQ(piped[0].size(), 70004u);
    EXPECT_TRUE(piped[0].ends_with("MARK"));
    EXPECT_EQ(piped[1], "short");
    EXPECT_EQ(piped[2].size(), 200003u);
    EXPECT_TRUE(piped[2].ends_with("END"));
    EXPECT_EQ(piped[3], "tail");
  }
  {
    // Exactly on the boundary: a delimiter as the last byte of a window, and a line
    // whose length equals the window size.
    for (size_t len : {65535u, 65536u, 65537u}) {
      std::string content(len, 'a');
      content.push_back('\n');
      content.append("next\n");
      const auto piped = ReadAllViaStdin(content);
      EXPECT_EQ(piped.size(), 2u);
      EXPECT_TRUE(!piped.empty() && piped[0].size() == len);
      EXPECT_TRUE(piped.size() > 1 && piped[1] == "next");
    }
  }
  {
    // A straddling final record with no trailing delimiter.
    std::string content(100000, 'z');
    const auto piped = ReadAllViaStdin(content);
    EXPECT_EQ(piped.size(), 1u);
    EXPECT_TRUE(!piped.empty() && piped[0].size() == 100000u);
  }
}

// ============================================================================
// process.h -- a match-time failure inside the scan is reported, not swallowed
// ============================================================================
static void TestScanReportsMatchTimeErrors() {
  TEST_CASE("a filter that fails at match time says so, and the run keeps going");
  for (const bool jit : {false, true}) {
    ScanRequest request;
    request.content = "hello\n" + kBlowsTheLimit + "\nsay hello again\n" + kBlowsTheLimit + "\n";
    request.filters = {"hello", kCatastrophic};
    request.jit = jit;
    const ScanResult result = RunScan(request);

    // The lines the working pattern matched are still printed, with their real
    // line numbers: the scan continued past the failure.
    EXPECT_EQ(result.lines.size(), 2u);
    EXPECT_TRUE(result.lines.size() == 2u && result.lines[0] == "hello");
    EXPECT_TRUE(result.lines.size() == 2u && result.lines[1] == "say hello again");
    EXPECT_TRUE(result.linenums.size() == 2u && result.linenums[0] == 1u && result.linenums[1] == 3u);

    // Two lines failed; one diagnostic came out, naming the pattern, the stage,
    // the place, and what the failure cost.
    EXPECT_TRUE(result.any_error);
    EXPECT_EQ(result.occurrences, 2u);
    EXPECT_EQ(result.reported, 1u);
    EXPECT_EQ(result.diagnostics.size(), 1u);
    if (!result.diagnostics.empty()) {
      const std::string& said = result.diagnostics.front();
      EXPECT_TRUE(Mentions(said, kCatastrophic));
      EXPECT_TRUE(Mentions(said, "match limit"));
      EXPECT_TRUE(Mentions(said, "--filter"));
      EXPECT_TRUE(Mentions(said, "(standard input):2"));
      EXPECT_TRUE(Mentions(said, "treated as not matching"));
    }
  }

  TEST_CASE("a filename replaces (standard input) in the diagnostic");
  {
    ScanRequest request;
    request.content = kBlowsTheLimit + "\n";
    request.filters = {kCatastrophic};
    request.filename = "notes.txt";
    const ScanResult result = RunScan(request);
    EXPECT_EQ(result.diagnostics.size(), 1u);
    EXPECT_TRUE(!result.diagnostics.empty() && Mentions(result.diagnostics.front(), "notes.txt:1"));
  }

  TEST_CASE("an exclude that fails does not silently exclude, and later excludes still run");
  for (const bool jit : {false, true}) {
    ScanRequest request;
    request.content = "keep me\n" + kBlowsTheLimit + "\ndrop me\n";
    request.excludes = {kCatastrophic, "drop"};
    request.jit = jit;
    const ScanResult result = RunScan(request);

    // The failing exclude cannot claim a match it never made, so the line it
    // failed on stays in the output -- and is reported, which is the whole
    // difference from before: dropping or keeping a line on the strength of a
    // failed match is a decision no one was told about.
    EXPECT_EQ(result.lines.size(), 2u);
    EXPECT_TRUE(result.lines.size() == 2u && result.lines[0] == "keep me");
    EXPECT_TRUE(result.lines.size() == 2u && result.lines[1] == kBlowsTheLimit);
    EXPECT_TRUE(result.any_error);
    EXPECT_EQ(result.diagnostics.size(), 1u);
    if (!result.diagnostics.empty()) {
      EXPECT_TRUE(Mentions(result.diagnostics.front(), "--exclude"));
      EXPECT_TRUE(Mentions(result.diagnostics.front(), "not excluded"));
    }
  }

  TEST_CASE("a range start that fails is reported, not a quiet swallowing of every line");
  for (const bool jit : {false, true}) {
    ScanRequest request;
    request.content = kBlowsTheLimit + "\nplain\n";
    request.range = "@(x+x+)+$@@";
    request.jit = jit;
    const ScanResult result = RunScan(request);

    // The range never opens, so nothing prints -- which used to be an empty run
    // with no explanation anywhere.
    EXPECT_EQ(result.lines.size(), 0u);
    EXPECT_TRUE(result.any_error);
    EXPECT_EQ(result.diagnostics.size(), 1u);
    if (!result.diagnostics.empty()) {
      EXPECT_TRUE(Mentions(result.diagnostics.front(), "--range start"));
      EXPECT_TRUE(Mentions(result.diagnostics.front(), kCatastrophic));
      EXPECT_TRUE(Mentions(result.diagnostics.front(), "does not open"));
    }
  }

  TEST_CASE("a range end that fails is reported, and the range stays open");
  for (const bool jit : {false, true}) {
    ScanRequest request;
    request.content = "first\n" + kBlowsTheLimit + "\nafter\n";
    request.range = "@1@(x+x+)+$@";
    request.jit = jit;
    const ScanResult result = RunScan(request);

    EXPECT_EQ(result.lines.size(), 3u);
    EXPECT_TRUE(result.lines.size() == 3u && result.lines[2] == "after");
    EXPECT_TRUE(result.any_error);
    EXPECT_EQ(result.diagnostics.size(), 1u);
    if (!result.diagnostics.empty()) {
      EXPECT_TRUE(Mentions(result.diagnostics.front(), "--range end"));
      EXPECT_TRUE(Mentions(result.diagnostics.front(), "does not close"));
    }
  }

  TEST_CASE("a replacement that fails prints the line unsubstituted instead of aborting the scan");
  for (const bool jit : {false, true}) {
    ScanRequest request;
    request.content = "xx\n" + kBlowsTheLimit + "\nxx\n";
    request.replacements = {"@(x+x+)+$@Y@"};
    request.jit = jit;
    const ScanResult result = RunScan(request);

    // Every line is still printed and the lines the replacement could finish
    // are still replaced: the failure used to be an exception that ended the
    // process where it stood, with whatever had been written already flushed
    // and nothing on stderr to say the rest was never scanned.
    EXPECT_EQ(result.lines.size(), 3u);
    EXPECT_TRUE(result.lines.size() == 3u && result.lines[0] == "Y");
    EXPECT_TRUE(result.lines.size() == 3u && result.lines[1] == kBlowsTheLimit);
    EXPECT_TRUE(result.lines.size() == 3u && result.lines[2] == "Y");
    EXPECT_TRUE(result.any_error);
    EXPECT_EQ(result.occurrences, 1u);
    EXPECT_EQ(result.diagnostics.size(), 1u);
    if (!result.diagnostics.empty()) {
      const std::string& said = result.diagnostics.front();
      EXPECT_TRUE(Mentions(said, "--replace"));
      EXPECT_TRUE(Mentions(said, kCatastrophic));
      EXPECT_TRUE(Mentions(said, "match limit"));
      EXPECT_TRUE(Mentions(said, "(standard input):2"));
      EXPECT_TRUE(Mentions(said, "replacement not made"));
    }
  }

  TEST_CASE("a later replacement still runs on a line an earlier one could not finish");
  {
    ScanRequest request;
    request.content = kBlowsTheLimit + "\n";
    request.replacements = {"@(x+x+)+$@Y@", "@z@!@"};
    const ScanResult result = RunScan(request);
    EXPECT_EQ(result.lines.size(), 1u);
    EXPECT_TRUE(!result.lines.empty() && result.lines.front() == std::string(20, 'x') + "!");
    EXPECT_EQ(result.diagnostics.size(), 1u);
  }

  TEST_CASE("a highlight that fails still prints the line, and says the paint is partial");
  for (const bool jit : {false, true}) {
    ScanRequest request;
    request.content = "needle " + kBlowsTheLimit + "\n";
    request.filters = {"needle", kCatastrophic};
    request.color = true;
    request.jit = jit;
    const ScanResult result = RunScan(request);

    EXPECT_EQ(result.lines.size(), 1u);
    EXPECT_TRUE(!result.lines.empty() && Mentions(result.lines.front(), "\x1b[01;31mneedle"));
    EXPECT_TRUE(result.any_error);
    EXPECT_EQ(result.diagnostics.size(), 1u);
    if (!result.diagnostics.empty()) {
      EXPECT_TRUE(Mentions(result.diagnostics.front(), "highlighting"));
      EXPECT_TRUE(Mentions(result.diagnostics.front(), "unhighlighted"));
    }
  }

  TEST_CASE("one report per pattern, however many lines fail");
  {
    ScanRequest request;
    for (int i = 0; i < 50; ++i) request.content += kBlowsTheLimit + "\n";
    request.filters = {kCatastrophic};
    const ScanResult result = RunScan(request);
    EXPECT_EQ(result.occurrences, 50u);
    EXPECT_EQ(result.reported, 1u);
    EXPECT_EQ(result.diagnostics.size(), 1u);
  }

  TEST_CASE("two patterns that fail are two reports");
  {
    ScanRequest request;
    request.content = kBlowsTheLimit + "\n" + std::string(20, 'y') + "z\n";
    request.filters = {kCatastrophic, "(y+y+)+$"};
    const ScanResult result = RunScan(request);
    EXPECT_EQ(result.lines.size(), 0u);
    EXPECT_EQ(result.occurrences, 2u);
    EXPECT_EQ(result.reported, 2u);
    EXPECT_EQ(result.diagnostics.size(), 2u);
  }

  TEST_CASE("a scan with nothing wrong reports nothing");
  {
    ScanRequest request;
    request.content = "hello\nworld\n";
    request.filters = {"hello"};
    const ScanResult result = RunScan(request);
    EXPECT_EQ(result.lines.size(), 1u);
    EXPECT_FALSE(result.any_error);
    EXPECT_EQ(result.occurrences, 0u);
    EXPECT_EQ(result.diagnostics.size(), 0u);
  }
}

// ============================================================================
// the CLI's exit status: 0 clean, 1 nothing scanned, 2 partially scanned
// ============================================================================
static void TestCliExitStatus() {
  TEST_CASE("a clean run prints matches, says nothing on stderr, and exits 0");
  {
    const CliResult run = RunCli("-f hello", "hello\nworld\n");
    EXPECT_EQ(run.status, 0);
    EXPECT_EQ(run.out, std::string("hello\n"));
    EXPECT_EQ(run.err, std::string{});
  }

  TEST_CASE("no match is still exit 0 -- gai does not use grep's 1 for it");
  {
    const CliResult run = RunCli("-f zzz", "hello\nworld\n");
    EXPECT_EQ(run.status, 0);
    EXPECT_EQ(run.out, std::string{});
  }

  TEST_CASE("a match-time failure is exit 2, once on stderr, with the matches still on stdout");
  {
    const std::string input = "hello\n" + kBlowsTheLimit + "\nsay hello again\n" + kBlowsTheLimit + "\n";
    const CliResult run = RunCli("-f hello -f '(x+x+)+$'", input);

    // 2, not 1: the scan ran and printed real answers. Not 0 either: some lines
    // were never honestly tested, and a script piping this must be able to tell.
    EXPECT_EQ(run.status, 2);
    EXPECT_EQ(run.out, std::string("hello\nsay hello again\n"));
    EXPECT_EQ(CountLines(run.err), 1u);
    EXPECT_TRUE(Mentions(run.err, "match limit"));
    EXPECT_TRUE(Mentions(run.err, kCatastrophic));
    EXPECT_TRUE(Mentions(run.err, "gai: (standard input):2:"));
  }

  TEST_CASE("a pattern that will not compile is still exit 1, and nothing is scanned");
  {
    const CliResult run = RunCli("-f 'bad['", "hello\n");
    EXPECT_EQ(run.status, 1);
    EXPECT_EQ(run.out.find("hello\n"), std::string::npos);
  }

  TEST_CASE("a replacement that fails at match time is exit 2, not a fatal 1 mid-scan");
  {
    // The old cost of this run was pcre2's ten million steps on every line and
    // then, on a long enough one, an exception: exit 1, the lines up to it
    // already flushed, and no way to tell that from a run that finished. Now
    // the whole file is scanned, the line the replacement could not finish is
    // printed as it arrived, and the status plus the one stderr line say which
    // line that was and what it cost.
    const std::string input = "xx\n" + kBlowsTheLimit + "\nxx\n";
    const CliResult run = RunCli("-r '@(x+x+)+$@Y@'", input);
    EXPECT_EQ(run.status, 2);
    EXPECT_EQ(run.out, "Y\n" + kBlowsTheLimit + "\nY\n");
    EXPECT_EQ(CountLines(run.err), 1u);
    EXPECT_TRUE(Mentions(run.err, "--replace"));
    EXPECT_TRUE(Mentions(run.err, "match limit"));
    EXPECT_TRUE(Mentions(run.err, "gai: (standard input):2:"));
    EXPECT_TRUE(Mentions(run.err, "replacement not made"));
  }

  TEST_CASE("a replacement text pcre2 rejects is still exit 1");
  {
    // Not a limit and not this line's fault -- `$9` names a group the pattern
    // does not have, and every remaining line would fail the same way, so it
    // stays fatal rather than becoming a per-line report.
    const CliResult run = RunCli("-r '@(a)@$9@'", "a\n");
    EXPECT_EQ(run.status, 1);
  }

  TEST_CASE("the failing pattern is named per stage: an exclude reports as an exclude");
  {
    const std::string input = "keep me\n" + kBlowsTheLimit + "\n";
    const CliResult run = RunCli("-e '(x+x+)+$'", input);
    EXPECT_EQ(run.status, 2);
    EXPECT_EQ(CountLines(run.err), 1u);
    EXPECT_TRUE(Mentions(run.err, "--exclude"));
    // Fail-open: the line the exclude could not test is still printed.
    EXPECT_EQ(run.out, "keep me\n" + kBlowsTheLimit + "\n");
  }
}

int main() {
  TestTrimAndSplit();
  TestCompileAndFind();
  TestSubstitute();
  TestFindSpans();
  TestBothEnginesAgreeOnIllFormedSubjects();
  TestMatchTimeErrorsAreReported();
  TestBacktrackingIsBounded();
  TestParsers();
  TestInputReaders();
  TestScanReportsMatchTimeErrors();
  TestCliExitStatus();
  return common::TestSummary();
}
