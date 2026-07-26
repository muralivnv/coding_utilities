// Tests for sakura's config parsing and its symbol-extraction core. Fixtures --
// a config file, query files and sources to parse -- are written to a temp
// directory, because both halves are defined in terms of files on disk.

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "extract.h"
#include "test_harness.h"

namespace fs = std::filesystem;
using namespace sakura;

namespace {

// Cleans up after itself so a failing run does not leave fixtures behind.
class TempDir {
 public:
  TempDir() {
    const char* base = std::getenv("TMPDIR");
    path_ = fs::path(base != nullptr ? base : "/tmp") / ("sakura_tests_" + std::to_string(::getpid()));
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  ~TempDir() { fs::remove_all(path_); }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }

  fs::path Write(std::string_view name, std::string_view content) const {
    const fs::path p = path_ / name;
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << content;
    return p;
  }

 private:
  fs::path path_;
};

// Node types chosen for stability across tree-sitter-cpp versions.
constexpr std::string_view kCppDefs = "(function_definition) @def\n";
constexpr std::string_view kCppRefs = "(call_expression) @call\n";
constexpr std::string_view kPyDefs = "(function_definition name: (identifier) @def)\n";

std::vector<std::string> NamesOf(const std::vector<Symbol>& symbols) {
  std::vector<std::string> out;
  out.reserve(symbols.size());
  for (const Symbol& s : symbols) out.push_back(s.name);
  return out;
}

bool AnyStartsWith(const std::vector<Symbol>& symbols, std::string_view prefix) {
  for (const Symbol& s : symbols) {
    if (std::string_view(s.name).starts_with(prefix))
      return true;
  }
  return false;
}

}  // namespace

// ============================================================================
// config.h -- ParseConfig
// ============================================================================
static void TestParseConfig(const TempDir& tmp) {
  TEST_CASE("a well-formed config entry");
  {
    tmp.Write("q_defs.scm", kCppDefs);
    tmp.Write("q_refs.scm", kCppRefs);
    const fs::path cfg = tmp.Write("basic.conf",
                                   "--language cpp --file-exts .cpp .hpp "
                                   "--query-definitions q_defs.scm --query-references q_refs.scm\n");
    const auto config = ParseConfig(cfg);
    EXPECT_EQ(config.size(), 1u);
    EXPECT_TRUE(config.contains("cpp"));
    const LanguageInfo& info = config.at("cpp");
    EXPECT_EQ(info.file_extensions.size(), 2u);
    EXPECT_TRUE(info.file_extensions.contains(".cpp"));
    EXPECT_TRUE(info.file_extensions.contains(".hpp"));
    EXPECT_TRUE(info.query_definitions.has_value());
    EXPECT_TRUE(info.query_references.has_value());
    // Query paths resolve against the config file's directory, not the cwd, so
    // sakura can be invoked from anywhere.
    EXPECT_EQ(info.query_definitions.value(), tmp.path() / "q_defs.scm");
    EXPECT_EQ(info.query_references.value(), tmp.path() / "q_refs.scm");
  }

  TEST_CASE("comments and blank lines are skipped");
  {
    const fs::path cfg = tmp.Write("comments.conf",
                                   "# a comment\n"
                                   "\n"
                                   "--language cpp --file-exts .cpp "
                                   "--query-definitions q_defs.scm --query-references q_refs.scm\n"
                                   "# trailing comment\n");
    const auto config = ParseConfig(cfg);
    EXPECT_EQ(config.size(), 1u);
  }

  TEST_CASE("several languages");
  {
    tmp.Write("py_defs.scm", kPyDefs);
    const fs::path cfg = tmp.Write("multi.conf",
                                   "--language cpp --file-exts .cpp .cc "
                                   "--query-definitions q_defs.scm --query-references q_refs.scm\n"
                                   "--language python --file-exts .py "
                                   "--query-definitions py_defs.scm --query-references py_defs.scm\n");
    const auto config = ParseConfig(cfg);
    EXPECT_EQ(config.size(), 2u);
    EXPECT_TRUE(config.contains("cpp"));
    EXPECT_TRUE(config.contains("python"));
    EXPECT_TRUE(config.at("python").file_extensions.contains(".py"));
    EXPECT_TRUE(config.at("cpp").file_extensions.contains(".cc"));
  }

  TEST_CASE("every required key is enforced");
  {
    // Each of the four keys, omitted in turn.
    EXPECT_THROWS(ParseConfig(tmp.Write(
        "m1.conf", "--file-exts .cpp --query-definitions a.scm --query-references b.scm\n")));
    EXPECT_THROWS(ParseConfig(tmp.Write(
        "m2.conf", "--language cpp --query-definitions a.scm --query-references b.scm\n")));
    EXPECT_THROWS(ParseConfig(tmp.Write("m3.conf", "--language cpp --file-exts .cpp --query-references b.scm\n")));
    EXPECT_THROWS(ParseConfig(tmp.Write("m4.conf", "--language cpp --file-exts .cpp --query-definitions a.scm\n")));
    // A flag present but with no value is just as wrong as a missing flag.
    EXPECT_THROWS(ParseConfig(tmp.Write(
        "m5.conf", "--language --file-exts .cpp --query-definitions a.scm --query-references b.scm\n")));
    // One bad line sinks the parse, so a typo cannot silently drop a language.
    EXPECT_THROWS(ParseConfig(tmp.Write(
        "m6.conf",
        "--language cpp --file-exts .cpp --query-definitions a.scm --query-references b.scm\n"
        "--language python\n")));
  }

  TEST_CASE("a missing or empty config yields an empty map, not a throw");
  {
    // main checks fs::exists first, so this is the defensive path.
    EXPECT_NO_THROW(ParseConfig(tmp.path() / "does_not_exist.conf"));
    EXPECT_TRUE(ParseConfig(tmp.path() / "does_not_exist.conf").empty());
    EXPECT_TRUE(ParseConfig(tmp.Write("empty.conf", "")).empty());
    EXPECT_TRUE(ParseConfig(tmp.Write("only_comments.conf", "# nothing\n\n")).empty());
  }
}

// ============================================================================
// extract.h -- helpers
// ============================================================================
static void TestExtractHelpers() {
  TEST_CASE("LStrip");
  EXPECT_EQ(LStrip("  hello"), "hello");
  EXPECT_EQ(LStrip("hello  "), "hello  ");  // only leading, trailing is content
  EXPECT_EQ(LStrip(""), "");
  EXPECT_EQ(LStrip("   "), "");
  EXPECT_EQ(LStrip("\t\n x"), "x");
  EXPECT_EQ(LStrip("x"), "x");
  // High bytes must not be mistaken for whitespace, which is what an unsigned cast
  // to isspace protects against.
  EXPECT_EQ(LStrip("\xC3\xA9x"), "\xC3\xA9x");

  TEST_CASE("HasParserFor");
  EXPECT_TRUE(HasParserFor("cpp"));
  EXPECT_TRUE(HasParserFor("python"));
  EXPECT_FALSE(HasParserFor("rust"));
  EXPECT_FALSE(HasParserFor(""));
  EXPECT_FALSE(HasParserFor("CPP"));  // the config key is matched exactly
}

static void TestLanguageForExtension() {
  std::unordered_map<std::string, LanguageInfo> config;
  LanguageInfo cpp;
  cpp.file_extensions = {".cpp", ".hpp"};
  config.emplace("cpp", std::move(cpp));
  LanguageInfo py;
  py.file_extensions = {".py"};
  config.emplace("python", std::move(py));

  TEST_CASE("LanguageForExtension");
  EXPECT_EQ(LanguageForExtension("a/b/c.cpp", config), "cpp");
  EXPECT_EQ(LanguageForExtension("x.hpp", config), "cpp");
  EXPECT_EQ(LanguageForExtension("x.py", config), "python");
  // The extension is lowercased first, so a shouting filename still matches.
  EXPECT_EQ(LanguageForExtension("X.CPP", config), "cpp");
  EXPECT_EQ(LanguageForExtension("X.Py", config), "python");
  // Nothing configured for these.
  EXPECT_TRUE(LanguageForExtension("x.rs", config).empty());
  EXPECT_TRUE(LanguageForExtension("Makefile", config).empty());
  EXPECT_TRUE(LanguageForExtension("x.", config).empty());
  // A dotfile has no extension as far as std::filesystem is concerned.
  EXPECT_TRUE(LanguageForExtension(".cpp", config).empty());
  EXPECT_TRUE(LanguageForExtension("", config).empty());
}

// ============================================================================
// extract.h -- the tree-sitter core
// ============================================================================
static void TestExtractSymbols(const TempDir& tmp) {
  tmp.Write("q_defs.scm", kCppDefs);
  tmp.Write("q_refs.scm", kCppRefs);
  tmp.Write("py_defs.scm", kPyDefs);
  const fs::path cfg = tmp.Write("extract.conf",
                                 "--language cpp --file-exts .cpp "
                                 "--query-definitions q_defs.scm --query-references q_refs.scm\n"
                                 "--language python --file-exts .py "
                                 "--query-definitions py_defs.scm --query-references py_defs.scm\n");
  const auto config = ParseConfig(cfg);

  TEST_CASE("InitializeQueries compiles only what was asked for");
  {
    const auto defs = InitializeQueries(true, false, config);
    EXPECT_EQ(defs.size(), 2u);
    EXPECT_TRUE(defs.contains("cpp"));
    EXPECT_TRUE(defs.at("cpp").query != nullptr);
    EXPECT_TRUE(defs.at("cpp").language != nullptr);
    // Asking for neither still produces an entry: an empty query matches nothing,
    // which is the same outcome as no query but keeps the map shape predictable.
    const auto none = InitializeQueries(false, false, config);
    EXPECT_EQ(none.size(), 2u);
  }
  {
    // A language with no linked parser is skipped rather than fatal.
    tmp.Write("rust_defs.scm", "(x) @y\n");
    const fs::path rust_cfg = tmp.Write("rust.conf",
                                        "--language rust --file-exts .rs "
                                        "--query-definitions rust_defs.scm --query-references rust_defs.scm\n");
    const auto rust_config = ParseConfig(rust_cfg);
    EXPECT_EQ(rust_config.size(), 1u);
    EXPECT_TRUE(InitializeQueries(true, false, rust_config).empty());
  }
  {
    // A query that does not compile is skipped, and the others still work.
    tmp.Write("bad.scm", "(this_node_does_not_exist) @x\n");
    const fs::path bad_cfg = tmp.Write("bad.conf",
                                       "--language cpp --file-exts .cpp "
                                       "--query-definitions bad.scm --query-references bad.scm\n");
    const auto bad_config = ParseConfig(bad_cfg);
    EXPECT_TRUE(InitializeQueries(true, false, bad_config).empty());
  }

  const auto defs_queries = InitializeQueries(true, false, config);
  const auto refs_queries = InitializeQueries(false, true, config);

  TEST_CASE("ExtractSymbols finds C++ definitions with 1-based positions");
  {
    const fs::path src = tmp.Write("sample.cpp",
                                   "int add(int a, int b) {\n"
                                   "  return a + b;\n"
                                   "}\n"
                                   "\n"
                                   "void caller() {\n"
                                   "  add(1, 2);\n"
                                   "}\n");
    const auto symbols = ExtractSymbols(src, config, defs_queries);
    EXPECT_EQ(symbols.size(), 2u);  // two function_definition nodes
    if (symbols.size() >= 2) {
      // Rows are 1-based: the first definition starts on line 1, the second on 5.
      EXPECT_EQ(symbols[0].row, 1u);
      EXPECT_EQ(symbols[0].col, 1u);
      EXPECT_EQ(symbols[1].row, 5u);
      EXPECT_EQ(symbols[1].col, 1u);
      EXPECT_TRUE(std::string_view(symbols[0].name).starts_with("int add"));
      EXPECT_TRUE(std::string_view(symbols[1].name).starts_with("void caller"));
    }
  }

  TEST_CASE("the references query selects different nodes");
  {
    const fs::path src = tmp.path() / "sample.cpp";
    const auto symbols = ExtractSymbols(src, config, refs_queries);
    EXPECT_EQ(symbols.size(), 1u);  // one call_expression
    if (!symbols.empty()) {
      EXPECT_EQ(symbols[0].row, 6u);
      EXPECT_TRUE(std::string_view(symbols[0].name).starts_with("add("));
    }
  }

  TEST_CASE("captured text is left-stripped");
  {
    // The node starts after indentation, but a query capturing a block would include
    // it; LStrip is what keeps the emitted symbol from carrying leading whitespace.
    const fs::path src = tmp.Write("indented.cpp", "\n    int f() { return 0; }\n");
    const auto symbols = ExtractSymbols(src, config, defs_queries);
    EXPECT_EQ(symbols.size(), 1u);
    if (!symbols.empty()) {
      EXPECT_EQ(symbols[0].row, 2u);
      EXPECT_FALSE(symbols[0].name.starts_with(" "));
      EXPECT_TRUE(symbols[0].name.starts_with("int f"));
    }
  }

  TEST_CASE("python uses its own parser and query");
  {
    const fs::path src = tmp.Write("sample.py",
                                   "def alpha():\n"
                                   "    pass\n"
                                   "\n"
                                   "def beta(x):\n"
                                   "    return x\n");
    const auto symbols = ExtractSymbols(src, config, defs_queries);
    EXPECT_EQ(symbols.size(), 2u);
    const auto names = NamesOf(symbols);
    EXPECT_TRUE(AnyStartsWith(symbols, "alpha"));
    EXPECT_TRUE(AnyStartsWith(symbols, "beta"));
    if (symbols.size() >= 2) {
      EXPECT_EQ(symbols[0].row, 1u);
      EXPECT_EQ(symbols[1].row, 4u);
      // Column is 1-based and points at the name, past "def ".
      EXPECT_EQ(symbols[0].col, 5u);
    }
  }

  TEST_CASE("nothing to extract yields an empty vector, never a crash");
  {
    // Empty file.
    EXPECT_TRUE(ExtractSymbols(tmp.Write("empty.cpp", ""), config, defs_queries).empty());
    // Whitespace only: parses fine, matches nothing.
    EXPECT_TRUE(ExtractSymbols(tmp.Write("blank.cpp", "\n\n\n"), config, defs_queries).empty());
    // Extension not in the config.
    EXPECT_TRUE(ExtractSymbols(tmp.Write("thing.rs", "fn main() {}\n"), config, defs_queries).empty());
    // File does not exist.
    EXPECT_TRUE(ExtractSymbols(tmp.path() / "missing.cpp", config, defs_queries).empty());
    // Configured extension but no compiled query for it.
    EXPECT_TRUE(ExtractSymbols(tmp.path() / "sample.cpp", config, {}).empty());
    // Empty config.
    EXPECT_TRUE(ExtractSymbols(tmp.path() / "sample.cpp", {}, defs_queries).empty());
  }

  TEST_CASE("malformed source still parses -- tree-sitter recovers");
  {
    // A real code navigator runs over files mid-edit, so a syntax error must yield
    // whatever is still recognisable instead of failing the file.
    const fs::path src = tmp.Write("broken.cpp", "int good() { return 1; }\nint bad( {{{ \n");
    EXPECT_NO_THROW(ExtractSymbols(src, config, defs_queries));
    const auto symbols = ExtractSymbols(src, config, defs_queries);
    EXPECT_TRUE(AnyStartsWith(symbols, "int good"));
  }

  TEST_CASE("utf-8 in source keeps byte offsets and columns consistent");
  {
    const fs::path src = tmp.Write("unicode.cpp", "int f() { const char* s = \"caf\xC3\xA9\"; return 0; }\n");
    const auto symbols = ExtractSymbols(src, config, defs_queries);
    EXPECT_EQ(symbols.size(), 1u);
    if (!symbols.empty()) {
      EXPECT_EQ(symbols[0].row, 1u);
      // The captured text must include the multi-byte sequence intact.
      EXPECT_TRUE(symbols[0].name.find("caf\xC3\xA9") != std::string::npos);
    }
  }

  TEST_CASE("a large file does not truncate");
  {
    // MemMappedFileRead hands tree-sitter the whole remainder each call; a file well
    // past any internal buffer size proves nothing is being chunked away.
    std::string big;
    for (int i = 0; i < 2000; ++i) {
      big += "int f" + std::to_string(i) + "() { return " + std::to_string(i) + "; }\n";
    }
    const auto symbols = ExtractSymbols(tmp.Write("big.cpp", big), config, defs_queries);
    EXPECT_EQ(symbols.size(), 2000u);
    if (symbols.size() == 2000u) {
      EXPECT_EQ(symbols[0].row, 1u);
      EXPECT_EQ(symbols[1999].row, 2000u);
      EXPECT_TRUE(symbols[1999].name.starts_with("int f1999"));
    }
  }
}

int main() {
  const TempDir tmp;
  TestParseConfig(tmp);
  TestExtractHelpers();
  TestLanguageForExtension();
  TestExtractSymbols(tmp);
  return common::TestSummary();
}
