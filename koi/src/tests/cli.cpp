// Tests for cli.cpp: the non-interactive modes koi can be run in.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void CliModes() {
  TEST_CASE("--render-mode, --symbol-mode, --overview");

  {
    RenderOptions options;
    options.color = false;
    options.language = "cpp";
    std::string error;
    const std::string out = RenderToAnsi("int main() { return 0; }\n", options, error);
    EXPECT_EQ(out, std::string("int main() { return 0; }\n"));
    EXPECT_TRUE(out.find('\x1b') == std::string::npos);
  }

  {
    RenderOptions options;
    options.color = false;
    options.first_line = 2;
    options.last_line = 3;
    std::string error;
    EXPECT_EQ(RenderToAnsi("a\nb\nc\nd\n", options, error), std::string("b\nc\n"));
    options.last_line = 99;
    EXPECT_EQ(RenderToAnsi("a\nb\nc\nd\n", options, error), std::string("b\nc\nd\n"));
  }

  {
    RenderOptions options;
    options.color = true;
    options.highlight_line = 2;
    std::string error;
    const std::string out = RenderToAnsi("aaa\nbbb\nccc\n", options, error);
    std::vector<std::string> lines;
    for (size_t at = 0; at < out.size();) {
      const size_t eol = std::min(out.find('\n', at), out.size());
      lines.emplace_back(out.substr(at, eol - at));
      at = eol + 1;
    }
    EXPECT_EQ(std::ssize(lines), Index{3});
    EXPECT_TRUE(lines[0].find("48;2;") == std::string::npos);
    EXPECT_TRUE(lines[1].find("48;2;") != std::string::npos);
    EXPECT_TRUE(lines[1].find("\x1b[K") != std::string::npos);
    EXPECT_TRUE(lines[2].find("48;2;") == std::string::npos);
  }

  {
    RenderOptions options;
    options.language = "klingon";
    std::string error;
    EXPECT_EQ(RenderToAnsi("hello\n", options, error), std::string("hello\n"));
  }

  {
    // --no-syntax: the grammar is skipped, the theme is not. The guard is the
    // first case -- the same text with syntax on has to come back painted, or
    // this test would pass just as well against a flag that does nothing.
    const std::string_view code = "int main() { return 0; }\n";
    RenderOptions painted;
    painted.language = "cpp";
    std::string error;
    EXPECT_TRUE(RenderToAnsi(code, painted, error).find('\x1b') != std::string::npos);

    RenderOptions plain = painted;
    plain.syntax = false;
    EXPECT_EQ(RenderToAnsi(code, plain, error), std::string{code});
  }

  {
    // The marked line under --no-syntax wears ui.excerpt.match -- a foreground,
    // the colour an excerpt view gives what was searched for -- where with
    // syntax on it wears the ui.cursorline.primary background.
    RenderOptions options;
    options.language = "cpp";
    options.syntax = false;
    options.highlight_line = 2;
    std::string error;
    const std::string out = RenderToAnsi("aaa\nbbb\nccc\n", options, error);
    std::vector<std::string> lines;
    for (size_t at = 0; at < out.size();) {
      const size_t eol = std::min(out.find('\n', at), out.size());
      lines.emplace_back(out.substr(at, eol - at));
      at = eol + 1;
    }
    EXPECT_EQ(std::ssize(lines), Index{3});
    EXPECT_TRUE(lines[0].find('\x1b') == std::string::npos);
    EXPECT_TRUE(lines[1].find("38;2;") != std::string::npos);
    EXPECT_TRUE(lines[1].find("48;2;") == std::string::npos);
    EXPECT_TRUE(lines[2].find('\x1b') == std::string::npos);
    EXPECT_TRUE(lines[1].find("bbb") != std::string::npos);
  }

  Scratch scratch{"koi-cli-test"};

  {
    const std::filesystem::path file =
        scratch.Write("s.cpp", "int Alpha() { return 0; }\nint Beta() { return Alpha(); }\n");
    const std::vector<std::string> paths{file.string()};

    const auto written = [&paths](const SymbolModeOptions& options, std::string& error) {
      std::FILE* sink = std::tmpfile();
      EXPECT_TRUE(sink != nullptr);
      const bool ok = WriteSymbols(paths, options, sink, error);
      EXPECT_TRUE(ok);
      std::fflush(sink);
      std::rewind(sink);
      std::string text;
      char buffer[4096];
      while (const size_t n = std::fread(buffer, 1, sizeof(buffer), sink)) text.append(buffer, n);
      std::fclose(sink);
      return text;
    };

    std::string error;
    SymbolModeOptions options;
    options.kind = SymbolKind::kDefinitions;
    const std::string out = written(options, error);
    EXPECT_TRUE(out.find(":1:5:Alpha") != std::string::npos);
    EXPECT_TRUE(out.find(":2:5:Beta") != std::string::npos);
    for (const char c : out) EXPECT_TRUE((c != '\r') && (c != '\t'));

    options.kind = SymbolKind::kBoth;
    options.containing = "Alpha";
    const std::string filtered = written(options, error);
    EXPECT_TRUE(filtered.find("Alpha") != std::string::npos);
    EXPECT_TRUE(filtered.find("Beta") == std::string::npos);

    options.containing.clear();
    options.kind = SymbolKind::kDefinitions;
    options.picker_rows = true;
    const std::string rows = written(options, error);
    Symbol first;
    EXPECT_TRUE(ParseSymbolRow(RowPayload(std::string_view{rows}.substr(0, rows.find('\n'))),
                               first));
    EXPECT_EQ(first.name, std::string{"Alpha"});
    EXPECT_EQ(first.line, Index{1});
  }

  {
    const std::filesystem::path file = scratch.Write("o.cpp", R"CODE(#include <vector>
#include "local.h"

struct Widget {
  int count;
  int Size() const;
};

void Draw() {
  Paint();
  Paint();
  size();
}
)CODE");
    const std::vector<std::string> paths{file.string()};
    std::string out;
    std::string error;
    EXPECT_TRUE(OverviewOf(paths, {}, out, error));
    EXPECT_TRUE(out.find("includes: <vector> local.h") != std::string::npos);
    EXPECT_TRUE(out.find("Widget@4:") != std::string::npos);
    EXPECT_TRUE(out.find("members: count@5") != std::string::npos);
    EXPECT_TRUE(out.find("methods: Size@6") != std::string::npos);
    EXPECT_TRUE(out.find("Draw@9: Paint@10 size@12") != std::string::npos);

    const std::vector<std::string> filter{"size"};
    std::string filtered;
    EXPECT_TRUE(OverviewOf(paths, filter, filtered, error));
    EXPECT_TRUE(filtered.find("Draw@9: Paint@10\n") != std::string::npos);
  }
}

}  // namespace koi
