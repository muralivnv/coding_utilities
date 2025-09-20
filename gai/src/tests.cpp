#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "operation.h"
#include "regex.h"

#define EXPECT_TRUE(expr)                                                                              \
  do {                                                                                                 \
    if (!(expr)) {                                                                                     \
      std::cerr << "EXPECT_TRUE failed: " #expr << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
    }                                                                                                  \
  } while (0)

#define EXPECT_THROWS(expr)                                                                                            \
  do {                                                                                                                 \
    bool caught = false;                                                                                               \
    try {                                                                                                              \
      expr;                                                                                                            \
    } catch (...) {                                                                                                    \
      caught = true;                                                                                                   \
    }                                                                                                                  \
    if (!caught) {                                                                                                     \
      std::cerr << "EXPECT_THROWS failed: " #expr << " did not throw at " << __FILE__ << ":" << __LINE__ << std::endl; \
    }                                                                                                                  \
  } while (0)

static std::string RunSub(const gai::Pcre2Substitution& sub, std::string_view input) {
  static std::string scratch(512, ' ');
  std::string_view result = gai::Substitute(sub, input, scratch);
  return std::string{result};
}

int main() {
  using namespace gai;

  // Trim Test
  EXPECT_TRUE(Trim("  hello  ") == "hello");
  EXPECT_TRUE(Trim("") == "");
  EXPECT_TRUE(Trim("   ") == "");

  // Split Test
  {
    {  // Set 1
      auto parts = Split("@one@two@three@");
      EXPECT_TRUE(parts.size() == 3u);
      EXPECT_TRUE(parts[0] == "one");
      EXPECT_TRUE(parts[1] == "two");
      EXPECT_TRUE(parts[2] == "three");
    }
    {  // Set 2
      auto parts = Split("   $one@two/three@  ");
      EXPECT_TRUE(parts.size() == 0);
    }
  }

  {  // Find - No JIT Test
    auto regex = Regex(Compile("hello", false, false));
    EXPECT_TRUE(Find(regex, "hello world"));
    EXPECT_TRUE(!Find(regex, "goodbye world"));
  }

  {  // Find - JIT Test
    auto regex = Regex(Compile("world", true, false));
    EXPECT_TRUE(regex.re.jitted);
    EXPECT_TRUE(Find(regex, "hello world"));
    EXPECT_TRUE(Find(regex, "goodbyeworld"));
  }

  {  // Find Word JIT
    auto regex = Regex(Compile("\\bworld\\b", true, false));
    EXPECT_TRUE(regex.re.jitted);
    EXPECT_TRUE(Find(regex, "hello world"));
    EXPECT_TRUE(!Find(regex, "goodbyeworld"));
  }

  {  // Substitution Test
    auto sub = Pcre2Substitution(Compile("world", false, false), "Earth");
    EXPECT_TRUE(RunSub(sub, "hello world") == "hello Earth");
    EXPECT_TRUE(RunSub(sub, "no match") == "no match");
  }

  {  // Regex with Captures
    {
      auto regex = Regex(Compile("(\\d+)-(\\w+)", false, false));
      EXPECT_TRUE(Find(regex, "123-abc"));
      EXPECT_TRUE(!Find(regex, "abc-123"));
    }
    {
      auto sub = Pcre2Substitution(Compile("(\\d+)-(\\w+)", false, false), "$2:$1");
      EXPECT_TRUE(RunSub(sub, "123-abc") == "abc:123");
    }
  }

  // Named Captures
  {
    auto regex = Regex(Compile("(?<num>\\d+)-(?<word>\\w+)", true, false));
    EXPECT_TRUE(Find(regex, "456-def"));

    auto sub = Pcre2Substitution(Compile("(?<num>\\d+)-(?<word>\\w+)", false, false), "${word}:${num}");
    EXPECT_TRUE(RunSub(sub, "456-def") == "def:456");
  }

  // Repeated Groups
  {
    auto regex = Regex(Compile("(ha){2,4}", false, false));
    EXPECT_TRUE(Find(regex, "hahaha"));
    EXPECT_TRUE(Find(regex, "hahahaha"));
    EXPECT_TRUE(!Find(regex, "ha"));
  }

  // Unicode
  {
    auto regex = Regex(Compile("\\p{L}+", false, true));
    EXPECT_TRUE(Find(regex, "こんにちは"));
    EXPECT_TRUE(Find(regex, "hello"));
    EXPECT_TRUE(!Find(regex, "12345"));
  }

  // Unicode Substitution
  {
    auto sub = Pcre2Substitution(Compile("([\\p{L}]+)", false, true), "[$1]");
    EXPECT_TRUE(RunSub(sub, "hello") == "[hello]");
    EXPECT_TRUE(RunSub(sub, "こんにちは") == "[こんにちは]");
  }

  // Edge cases
  {
    auto regex = Regex(Compile("a*", false, false));
    EXPECT_TRUE(Find(regex, ""));
    auto sub = Pcre2Substitution(Compile("a*", false, false), "X");
    EXPECT_TRUE(RunSub(sub, "") == "X");
  }

  {
    std::string long_str(10000, 'a');
    auto regex = Regex(Compile("a{10000}", false, false));
    EXPECT_TRUE(Find(regex, long_str));
    auto sub = Pcre2Substitution(Compile("a{10000}", false, false), "b");
    EXPECT_TRUE(RunSub(sub, long_str) == "b");
  }

  {
    auto sub = Pcre2Substitution(Compile("aa(.*)", false, false), "X$1");
    EXPECT_TRUE(RunSub(sub, "aaaa") == "Xaa");
  }

  // Range / Filters / ParseSub
  {
    Range r;
    r.start = Regex(Compile("start", false, false));
    r.end = Regex(Compile("end", false, false));
    EXPECT_TRUE(!r.IsStartReached("no match", 1));
    EXPECT_TRUE(r.IsStartReached("this is start line", 1));
    EXPECT_TRUE(!r.IsEndReached("no match", 2));
    EXPECT_TRUE(r.IsEndReached("end of line", 2));
  }

  {
    auto sub = ParseSub("@(\\d+)-(\\w+)@$2:$1@", false, false);
    EXPECT_TRUE(sub.has_value());
    EXPECT_TRUE(RunSub(*sub, "42-foo") == "foo:42");
  }

  // ParseRange
  {
    auto range = ParseRange("@2@4@", false, false);
    EXPECT_TRUE(range.has_value());
    EXPECT_TRUE(std::get<size_t>(range->start) == 2u);
    EXPECT_TRUE(std::get<size_t>(range->end) == 4u);
  }

  {
    auto range = ParseRange("@hello@world@", false, false);
    EXPECT_TRUE(range.has_value());
    EXPECT_TRUE(range->IsStartReached("hello", 1));
    range->Reset();
    EXPECT_TRUE(!range->IsStartReached("hellw", 1));
    range->Reset();
    EXPECT_TRUE(range->IsEndReached("worldwow", 2));
    range->Reset();
    EXPECT_TRUE(!range->IsEndReached("weewe", 2));
  }

  // Malformed input
  EXPECT_THROWS(Compile("invalid[regex", false, false));

  // Missing delimiter, incorrect format, etc.
  EXPECT_THROWS(ParseSub("@\\d-@$1", false, false));
  EXPECT_THROWS(ParseSub("nodels", false, false));

  EXPECT_TRUE(ParseRange("@1@end@", false, false).has_value());
  EXPECT_THROWS(ParseRange("@start@", false, false));

  return EXIT_SUCCESS;
}
