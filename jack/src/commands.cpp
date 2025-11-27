#include <unordered_set>
#include <string_view>

#include "format.h"
#include "commands.h"
#include "subprocess.h"

namespace jack {

using namespace std::string_view_literals;

constexpr const char * FZF_COMMAND = "fzf --ansi --border -i --preview 'bat {1} --highlight-line {2}' --preview-window 'right,+{2}+3/3,~3' "
                                     "--delimiter '@' --scrollbar '▍' "
                                     "--nth=-1 --bind=tab:down,shift-tab:up --smart-case --cycle "
                                     "--style=full:line --layout=reverse --print-query";

static std::vector<std::string_view> Split(std::string_view data, char delim) {
  std::vector<std::string_view> out{};
  if (data.empty()) return out;

  size_t start = 0;
  while (start < data.size()) {
    const size_t end = data.find(delim, start);
    if (end == std::string_view::npos) break;
    out.push_back(data.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

static CommandOutput ParseQueryAndSelection(const std::string& output) {
  CommandOutput retval;   
  retval.storage = output;
  if (retval.storage.empty()) {
    return retval;
  }
  const std::vector<std::string_view> lines = Split(retval.storage, '\n');
  if (lines.empty()) {
    return retval;
  }

  if (lines.size() == 1) {
    retval.user_selections.push_back(lines[0]);
  } else {
    retval.user_query = lines[0];
    for (size_t i = 1; i < lines.size(); ++i) {
      retval.user_selections.push_back(lines[i]);
    }
  }

  return retval;
}

// Reference: https://github.com/python/cpython/blob/bc9e63dd9d2931771415cca1b0ed774471d523c0/Lib/shlex.py#L320
std::string Quote(const std::string &s) {
  // Empty string → "''"
  if (s.empty()) {
    return "''";
  }

  // Set of safe characters
  static const std::unordered_set<char> safe_chars = [] {
    std::unordered_set<char> st;
    std::string chars =
        "%+,-./0123456789:=@"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ_"
        "abcdefghijklmnopqrstuvwxyz";
    for (char c : chars) st.insert(c);
    return st;
  }();

  // Check ASCII-only and all characters in safe set
  bool ascii_only = true;
  bool all_safe = true;
  for (unsigned char c : s) {
    if (c > 0x7F) {
      ascii_only = false;
      all_safe = false;
      break;
    }
    if (!safe_chars.count(c)) {
      all_safe = false;
    }
  }

  // No quoting needed
  if (ascii_only && all_safe) {
    return s;
  }

  // Otherwise: wrap in single quotes and escape internal single quotes
  // Replace "'" → "'\"'\"'"
  std::string result;
  result.reserve(s.size() + 2);  // small optimization
  result.push_back('\'');
  for (char c : s) {
    if (c == '\'') {
      result += "'\"'\"'";
    } else {
      result.push_back(c);
    }
  }
  result.push_back('\'');
  return result;
}

CommandOutput OpenFilePicker(const fs::path& file_filter_file, const std::string& query) {
  std::string_view cmd = common::FormatIntoStringView<"bash %s | gai -r '/(\\S+)/$1@1/' | %s --nth=1 --tiebreak=pathname --query=%s">(
                                                           file_filter_file, FZF_COMMAND, Quote(query));
  std::string output = ShellExec(std::string{cmd});
  return ParseQueryAndSelection(output);
}

CommandOutput OpenContentPicker(const fs::path& file_filter_file, const std::string& query) {
  std::string_view cmd = common::FormatIntoStringView<"bash %s | xargs gai -f '\\w' -v -d @ --files | %s --tiebreak=begin --query=%s">(
                                                      file_filter_file, FZF_COMMAND, Quote(query));
  std::string output = ShellExec(std::string{cmd});
  return ParseQueryAndSelection(output);
}

CommandOutput OpenSymbolPicker(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                               const std::string& query, const fs::path& file) {
  std::string output;
  if (file.empty()) { // open project wide symbol picker
    std::string_view cmd = common::FormatIntoStringView<"bash %s | xargs sakura --config %s --definitions --files | %s --query=%s">(
                                                        file_filter_file, treesitter_tags_file, FZF_COMMAND, Quote(query));
    output = ShellExec(std::string{cmd});
  } else { // open file symbol picker
    std::string_view cmd = common::FormatIntoStringView<"sakura --config %s --definitions --files %s | %s --with-nth=-1 --query=%s">(
                                                        treesitter_tags_file, file, FZF_COMMAND, Quote(query));
    output = ShellExec(std::string{cmd});
  }
  return ParseQueryAndSelection(output);
}

CommandOutput GoToDefinition(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                             const std::string& symbol) {
  std::string_view cmd = common::FormatIntoStringView<"bash %s | xargs sakura --config %s --definitions --files | gai -f '\\b%s\\b' | ifne %s --query=%s --select-1 --exit-0">(
                                                      file_filter_file, treesitter_tags_file, symbol, FZF_COMMAND, Quote(symbol));
  std::string output = ShellExec(std::string{cmd});
  return ParseQueryAndSelection(output);
}

CommandOutput ShowReferences(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                             const std::string& symbol) {
  std::string_view cmd = common::FormatIntoStringView<"bash %s | xargs sakura --config %s --definitions --references --files | gai -f '\\b%s\\b' | ifne %s --query=%s">(
                                                      file_filter_file, treesitter_tags_file, symbol, FZF_COMMAND, Quote(symbol));
  std::string output = ShellExec(std::string{cmd});
  return ParseQueryAndSelection(output);
}

} // namespace jack
