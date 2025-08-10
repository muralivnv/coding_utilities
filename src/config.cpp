#include <fstream>
#include <string_view>
#include "config.h"

namespace fs = std::filesystem;
using namespace std::string_view_literals;

static std::string_view Strip(std::string_view s) {
  auto is_not_space = [](unsigned char ch) { return !std::isspace(ch); };
  auto begin = std::find_if(s.begin(), s.end(), is_not_space);
  auto end = std::find_if(s.rbegin(), s.rend(), is_not_space).base();
  if (begin >= end) {
    return {};
  }
  return std::string_view(begin, end - begin);
}

static std::tuple<std::string_view, std::string_view>
SplitAtFirstDelimiter(std::string_view str, char delim) {
  const size_t eq_op = str.find_first_of(delim);
  if (eq_op == std::string::npos) {
    return {str, std::string_view{}};
  }
  std::string_view lhs = Strip(str.substr(0, eq_op));
  std::string_view rhs = Strip(str.substr(eq_op + 1));
  return {lhs, rhs};
}

static std::unordered_set<std::string> ParseFileExtensions(std::string_view str) {
  std::unordered_set<std::string> out;
  while (!str.empty()) {
    size_t comma = str.find(',');
    std::string_view token = Strip(str.substr(0, comma));
    if (!token.empty())
      out.insert(std::string(token));
    if (comma == std::string_view::npos) break;
    str.remove_prefix(comma + 1);
  }
  return out;
}

std::unordered_map<std::string, LanguageInfo> ParseConfig(const fs::path& config_file) {
  std::unordered_map<std::string, LanguageInfo> retval;
  std::ifstream file(config_file);
  if (file.is_open()) {
    std::string line;
    while (std::getline(file, line)) {
      std::string_view line_sv(line);
      auto [lhs, rhs] = SplitAtFirstDelimiter(line_sv, '=');
      if (lhs.empty() || rhs.empty()) continue;
      auto [lang, type] = SplitAtFirstDelimiter(lhs, '.');
      if (lang.empty() || type.empty()) continue;

      std::string lang_str(lang);
      auto it = retval.find(lang_str);
      if (it == retval.end()) {
        it = retval.insert({lang_str, LanguageInfo{}}).first;
      }
      if (type == "file_extensions"sv) {
        it->second.file_extensions = ParseFileExtensions(rhs);
      } else if (type == "query_definitions"sv) {
        it->second.query_definitions = config_file.parent_path() / fs::path(rhs);
      } else if (type == "query_references"sv) {
        it->second.query_references = config_file.parent_path() / fs::path(rhs);
      }
    }
    file.close();
  }
  return retval;
}
