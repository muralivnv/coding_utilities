#include <vector>
#include <filesystem>
#include <unordered_map>
#include <algorithm>
#include <string_view>

#include "args.h"
#include "config.h"
#include "extract.h"
#include "printx.hpp"

constexpr const char* kVersion = PROJECT_VERSION; // define in root CMakeLists.txt
namespace fs = std::filesystem;


int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOFBF, 1 << 20);
  common::Args cli(argc, argv);
  constexpr std::string_view kCliHelpMessage = R"CLI(
Usage: sakura [options]

Options:
      --config        Config file (required)
      --references    List references (default: false)
      --definitions   List definitions (default: true)
      --files         Input list of files (required)
  -h, --help          Show this help message
  -v, --version       Print version number (default: false)
    )CLI";

  if (cli.Has("-h") || cli.Has("--help")) {
    rostd::printf<"%s">(kCliHelpMessage);
    return EXIT_SUCCESS;
  }
  if (cli.Has("--version")) {
    rostd::printf<"%s">(kVersion);
    return EXIT_SUCCESS;
  }
  if (!cli.Has("--config")) {
    rostd::printf<"Error!! Option --config is not specified.">();
    return EXIT_FAILURE;
  }

  using VecStringView = std::vector<std::string_view>;
  const std::string_view config_file = cli.Value({"--config"}).value_or("");
  const VecStringView files = cli.MultiValue({"--files"}, true).value_or(VecStringView{});

  if (!fs::exists(config_file)) {
    rostd::printf<"Error!! Input --config file does not exist.\n\tFile: %s\n">(config_file);
    return EXIT_FAILURE;
  }
  if (files.empty()) {
    rostd::printf<"Error!! Input --files list is empty\n">();
    return EXIT_FAILURE;
  }

  try {
    const std::unordered_map<std::string, LanguageInfo> config = ParseConfig(config_file);
    const auto queries = sakura::InitializeQueries(cli.Has("--definitions"), cli.Has("--references"), config);
    for (const std::string_view& file : files) {
      if (!fs::exists(file)) continue;
      const fs::path path{file};
      for (const sakura::Symbol& symbol : sakura::ExtractSymbols(path, config, queries)) {
        rostd::printf<"%s@%u@%u@%s\n">(path, symbol.row, symbol.col, symbol.name);
      }
    }
  } catch (const std::exception& ex) {
    rostd::printf<"Exception raised!!\nException: %s\n">(ex.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
