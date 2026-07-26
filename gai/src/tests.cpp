#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "input.h"
#include "operation.h"
#include "regex.h"
#include "test_harness.h"

using namespace gai;

namespace {

std::string RunSub(const gai::Pcre2Substitution& sub, std::string_view input) {
  static std::string scratch(512, ' ');
  std::string_view result = gai::Substitute(sub, input, scratch);
  return std::string{result};
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

  TEST_CASE("Find with JIT gives the same answers");
  {
    auto regex = Regex(Compile("world", true, false));
    EXPECT_TRUE(regex.re.jitted);
    EXPECT_TRUE(Find(regex, "hello world"));
    EXPECT_TRUE(Find(regex, "goodbyeworld"));
  }
  {
    auto regex = Regex(Compile("\\bworld\\b", true, false));
    EXPECT_TRUE(regex.re.jitted);
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

int main() {
  TestTrimAndSplit();
  TestCompileAndFind();
  TestSubstitute();
  TestFindSpans();
  TestParsers();
  TestInputReaders();
  return common::TestSummary();
}
