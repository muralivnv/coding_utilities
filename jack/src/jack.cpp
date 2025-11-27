#include <filesystem>
#include <fstream>
#include <regex>

#include "args.h"
#include "printx.hpp"
#include "format.h"
#include "commands.h"
#include "subprocess.h"

constexpr const char* kVersion = PROJECT_VERSION; // defined in root CMakeLists.txt

namespace fs = std::filesystem;
namespace jack {

struct FileLineCol {
  std::string_view file;
  std::string_view line;
  std::string_view col;
};

fs::path FindAlternatives(const fs::path& file) {
  const char* home_cstr = getenv("HOME");
  if (!home_cstr) {
    throw std::runtime_error("HOME environment variable not set.\n");
  }
  const fs::path home{home_cstr};
  const fs::path local = fs::path{".ronin"} / file;
  const fs::path global = home / ".config" / "ronin" / file;
  if (fs::is_regular_file(local)) {
    return local;
  }
  if (fs::is_regular_file(global)) {
    return global;
  }
  std::string_view error_msg = common::FormatIntoStringView<"Cannot find file '%s' at .ronin and $HOME/.config/ronin">(file);
  throw std::runtime_error(std::string{error_msg});
}

std::optional<FileLineCol> ToFileLineCol(std::string_view content) {
  static const std::regex pattern(R"(^([^@]+)@(\d+)(?:@(.*))?$)");

  std::string_view trimmed = content;
  const size_t first_char_pos = trimmed.find_first_not_of(" \t\n\r");
  if (first_char_pos == std::string_view::npos) {
    trimmed = ""; // Handle all-whitespace case
  } else {
    trimmed.remove_prefix(first_char_pos);
    const size_t last_char_pos = trimmed.find_last_not_of(" \t\n\r");
    trimmed.remove_suffix(trimmed.size() - (last_char_pos + 1));
  }

  std::cmatch match;
  if (!std::regex_match(trimmed.begin(), trimmed.end(), match, pattern)) {
    return std::nullopt;
  }
 
  std::string_view filename_view(match[1].first, match[1].length());
  std::string_view linenum_view(match[2].first, match[2].length());
  std::string_view rest_view;
  if (match[3].matched) {
    rest_view = std::string_view(match[3].first, match[3].length());
  }

  std::string_view colnum_view = "0"; // default value
    if (!rest_view.empty()) {
    auto pos = rest_view.find('@');
    std::string_view first;

    if (pos == std::string_view::npos) {
      first = rest_view;
    } else {
      first = rest_view.substr(0, pos);
    }
    // Check if the first part is numeric. This still requires iterating,
    // but avoids string allocation.
    if (!first.empty() && std::all_of(first.begin(), first.end(), ::isdigit)) {
      colnum_view = first;
    }
  }
  return FileLineCol{filename_view, linenum_view, colnum_view};
}

void OpenFilesInEditor(const CommandOutput& output, std::string_view parent_id) {
  std::string_view cmd = common::FormatIntoStringView<"swaymsg '[con_id=%s] focus'">(parent_id);
  std::ignore = ShellExec(std::string{cmd});
  for (std::string_view selection : output.user_selections) {
    const std::optional<FileLineCol> parsed = ToFileLineCol(selection);
    if (parsed.has_value()) {
      cmd = common::FormatIntoStringView<"wlrctl keyboard type ':open %s:%s:%s'">(parsed->file,
                                                                                  parsed->line,
                                                                                  parsed->col);
      std::ignore = ShellExec(std::string{cmd});
    }
  }
}

void WriteState(const fs::path& last_picker_state_file,
                std::string_view name, std::string_view query,
                const std::optional<fs::path>& file = std::nullopt) {
  std::ofstream state_file(last_picker_state_file);
  if (state_file.is_open()) {
    state_file << name << " " << "--query " << query;
    if (file.has_value()) {
      state_file << " --file ";
      if (!file.value().empty()) {
        state_file << file.value();
      }
    }
    state_file.close();
  }
}

void OpenPicker(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                const fs::path& last_picker_state_file, common::Args& cli) {
  const fs::path file{cli.Value({"--file"}).value_or(std::string_view{""})};
  const std::string query{cli.Value({"--query"}).value_or(std::string_view{" "})};

  jack::CommandOutput out;
  if (cli.Has("--open-file-picker")) {
     out = jack::OpenFilePicker(file_filter_file, query);
     WriteState(last_picker_state_file, "--open-file-picker", out.user_query);

  } else if (cli.Has("--open-content-picker")) {
    out = jack::OpenContentPicker(file_filter_file, query);
    WriteState(last_picker_state_file, "--open-content-picker", out.user_query);

  } else if (cli.Has("--open-symbol-picker")) {
    out = jack::OpenSymbolPicker(file_filter_file, treesitter_tags_file, query, file);
    WriteState(last_picker_state_file, "--open-symbol-picker", out.user_query, file);

  } else if (cli.Has("--goto-definition")) {
    if (query.empty()) {
      throw std::runtime_error("input --query is empty for --goto-definition");
    }
    out = jack::GoToDefinition(file_filter_file, treesitter_tags_file, query);
    WriteState(last_picker_state_file, "--goto-definition", out.user_query);

  } else if (cli.Has("--show-references")) {
    if (query.empty()) {
      throw std::runtime_error("input --query is empty for --show-references");
    }
    out = jack::ShowReferences(file_filter_file, treesitter_tags_file, query);
    WriteState(last_picker_state_file, "--show-references", out.user_query);
  }

  // NOTE: Whether parent-id is passed or not is checked in main.
  // so it is safe to be used with 'value_or' here
  OpenFilesInEditor(out, cli.Value({"--parent-id"}).value_or(std::string_view{"0"}));
}

void OpenLastPicker(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                    const fs::path& last_picker_state_file, common::Args& cli) {  
  struct Argv {
    int argc;
    std::vector<char*> argv_storage;
    std::vector<std::string> strings;
  };

  auto ParseToArgv = [](const std::string& line) {
    Argv result;
    std::string_view line_view(line);    
    result.strings.push_back("jack.out");
    size_t current_pos = 0; 
    while (current_pos < line_view.length()) {        
      current_pos = line_view.find_first_not_of(" \t\n\r", current_pos);
      if (current_pos == std::string_view::npos) break;
      const size_t end_pos = line_view.find_first_of(" \t\n\r", current_pos);
      const size_t length = (end_pos == std::string_view::npos) 
                            ? (line_view.length() - current_pos) // Token runs to the end
                            : (end_pos - current_pos);           // Token ends at whitespace
      result.strings.emplace_back(line_view.substr(current_pos, length));
      current_pos = end_pos;
    }
    for (std::string& s : result.strings) {
      result.argv_storage.push_back(s.data());
    }
    result.argc = static_cast<int>(result.argv_storage.size());
    return result;
  };

  if (fs::exists(last_picker_state_file)) {
    std::ifstream state_file(last_picker_state_file);
    if (state_file.is_open()) {
      std::string data;
      std::getline(state_file, data);
      Argv argv = ParseToArgv(data);
      argv.strings.push_back("--parent-id");
      argv.argv_storage.push_back(argv.strings.back().data());
      argv.strings.emplace_back(cli.Value({"--parent-id"}).value_or(std::string_view{"0"}));
      argv.argv_storage.push_back(argv.strings.back().data());
      argv.argc += 2;

      common::Args pseudo_cli(argv.argc, argv.argv_storage.data());
      OpenPicker(file_filter_file, treesitter_tags_file, last_picker_state_file, pseudo_cli);
      state_file.close();
    }
  }
}

} // namespace jack

int main(int argc, char** argv) {
  common::Args cli(argc, argv);
  constexpr std::string_view kCliHelpMessage = R"CLI(
Usage: jack [options]

Options:
      --open-file-picker          Open file picker (default: false)
      --open-content-picker       Open content picker (default: false)
      --open-last-picker          Open last launched picker (default: false)
      --open-symbol-picker        Open symbol picker (default: false)
      --file                      File whose symbols to be extracted (default: "")
      --goto-definition           Go to definition (default: false)
      --show-references           Show references (default: false)
      --query                     Initial query to be used (default: " ")
      --parent-id                 Sway container ID of the launched process (required)
  -h, --help                      Show this help message
      --version                   Print version number
  )CLI";

  if (cli.Has("-h") || cli.Has("--help")) {
    rostd::printf<"%s">(kCliHelpMessage);
    return EXIT_SUCCESS;
  }
  if (cli.Has("--version")) {
    rostd::printf<"%s">(kVersion);
    return EXIT_SUCCESS;
  }
  if (!cli.Has("--parent-id")) {
    rostd::printf<"Argument '--parent-id' is required">();
    return EXIT_FAILURE;
  }

  const std::optional<std::string_view> parent_id = cli.Value({"--parent-id"});
  if (!parent_id.has_value()) {
    rostd::printf<"Argument '--parent-id' is required">();
    return EXIT_FAILURE;
  }

  try {
    jack::SetEnv();
    const fs::path file_filter_file       = jack::FindAlternatives("file-filter.txt");
    const fs::path treesitter_tags_file   = jack::FindAlternatives("treesitter-tags.txt");
    const fs::path last_picker_state_file = fs::path(".ronin") / "last-picker-state.txt";

    if (cli.Has("--open-last-picker")) {
      jack::OpenLastPicker(file_filter_file, treesitter_tags_file, last_picker_state_file, cli);
    } else {
      jack::OpenPicker(file_filter_file, treesitter_tags_file, last_picker_state_file, cli);
    }
  } catch (const std::exception& ex) {
    rostd::printf<"Exception raised!!\nException: %s\n">(ex.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
