#include <fstream>
#include <sstream>

#include "config.h"
#include "args.h"
#include "format.h"

namespace fs = std::filesystem;

template<typename T>
static T ThrowIfNotValid(const common::Args& cli, std::string_view key) {
  if (!cli.Has(key)) {
    std::string_view error_msg = common::FormatIntoStringView<"Input flag %s is not specified\n">(key);
    throw std::runtime_error(std::string(error_msg));
  }
  if constexpr (std::is_same_v<T, std::string_view>) {
    std::optional<std::string_view> value = cli.Value({key});
    if (!value.has_value()) {
      std::string_view error_msg = common::FormatIntoStringView<"Input value to flag %s is not specified\n">(key);
      throw std::runtime_error(std::string(error_msg));
    }
    return value.value();
  } else if constexpr (std::is_same_v<T, std::vector<std::string_view>>) {
    std::optional<std::vector<std::string_view>> value = cli.MultiValue({key}, true);
    if (!value.has_value()) {
      std::string_view error_msg = common::FormatIntoStringView<"Input value to flag %s is not specified\n">(key);
      throw std::runtime_error(std::string(error_msg));      
    }
    return value.value();
  } else {
    static_assert("Unsupported type");
    return std::string_view{};
  }
}

std::unordered_map<std::string, LanguageInfo> ParseConfig(const fs::path& config_file) {
  constexpr std::string_view kConfigFormat = R"CLI(
  --language              Language (cpp/python/...) (required)
  --file-exts             File extensions (required)
  --query-definitions     Query file to extract definitions (required)
  --query-references      Query file to extract references (required)
    )CLI";
  std::ignore = kConfigFormat;

  std::unordered_map<std::string, LanguageInfo> retval;
  std::ifstream file(config_file);  
  if (file.is_open()) {
    std::string line;
    while (std::getline(file, line)) {
      if (line.empty() || line[0] == '#') continue;

      std::vector<std::string> args{"config_parser"};
      std::istringstream iss(line);
      std::string token;
      while (iss >> token) {
        args.push_back(token);
      }
      std::vector<char*> argv;
      argv.reserve(args.size());
      for (const std::string& s : args) {
        // NOTE: Args::Cli expects argv in 'char* argv[]' format
        argv.push_back(const_cast<char*>(s.c_str()));
      }
      common::Args parser(static_cast<int>(args.size()), argv.data());
      const std::string_view language = ThrowIfNotValid<std::string_view>(parser, "--language");
      const std::vector<std::string_view> file_exts = ThrowIfNotValid<std::vector<std::string_view>>(parser, "--file-exts");
      const std::string_view query_definitions = ThrowIfNotValid<std::string_view>(parser, "--query-definitions");
      const std::string_view query_references = ThrowIfNotValid<std::string_view>(parser, "--query-references");

      auto it = retval.insert({std::string{language}, LanguageInfo{}}).first;
      for (const std::string_view& f : file_exts) it->second.file_extensions.insert(std::string{f});
      it->second.query_definitions = config_file.parent_path() / fs::path(query_definitions);
      it->second.query_references = config_file.parent_path() / fs::path(query_references);
    }
    file.close();
  }
  return retval;
}
