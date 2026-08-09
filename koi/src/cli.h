#ifndef KOI_CLI_H_
#define KOI_CLI_H_

#include <cstdio>
#include <filesystem>
#include <generator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "piece_doc.h"
#include "project.h"
#include "symbols.h"

namespace koi {

struct RenderOptions {
  std::string language;

  Index first_line{1};
  Index last_line{0};

  Index highlight_line{0};

  bool line_numbers{false};

  bool color{true};

  // Whether to load a grammar and paint captures. Off, the theme is still read
  // and `highlight_line` is still drawn -- what goes is the tree-sitter work,
  // which is nearly all of what a render costs: compiling one language's
  // highlight query runs from under a millisecond (json) to ~230 ms (C++, whose
  // parse table `ts_query_new` walks is 5.6 MB), and it is paid per process.
  // A preview window that redraws as a picker's selection moves cannot afford
  // that, and does not need it: what it has to show is *where* the match is,
  // which is `highlight_line`, not what language the file is in.
  bool syntax{true};
};

std::string RenderToAnsi(std::string_view text, const RenderOptions& options, std::string& error);

enum class SymbolOrder : std::uint8_t {
  kFileOrder,

  kHotFirst,
};

struct SymbolModeOptions {
  SymbolKind kind{SymbolKind::kDefinitions};

  std::string containing;

  bool picker_rows{false};

  SymbolOrder order{SymbolOrder::kFileOrder};
  std::string from;

  int hot_limit{kDefaultHotFileLimit};
};

bool WriteSymbols(std::span<const std::string> paths, const SymbolModeOptions& options,
                  std::FILE* out, std::string& error);

std::generator<std::string_view> OverviewSections(std::span<const std::string> paths,
                                                  std::span<const std::string> filter,
                                                  std::string& error);

bool OverviewOf(std::span<const std::string> paths, std::span<const std::string> filter,
                std::string& out, std::string& error);

std::vector<std::string> ReadFilterFile(const std::filesystem::path& path);

int RunRenderMode(int argc, char** argv);
int RunSymbolMode(int argc, char** argv);
int RunOverviewMode(int argc, char** argv);

}

#endif
