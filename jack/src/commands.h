#ifndef JACK_SRC_COMMANDS_H_
#define JACK_SRC_COMMANDS_H_

#include <filesystem>
#include <optional>
#include <string>

namespace jack {
namespace fs = std::filesystem;

struct CommandOutput {
  std::vector<std::string> user_selections{};
  std::string user_query{};
};

std::optional<CommandOutput> OpenLastPicker(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                                            const fs::path& last_picker_state_file);

CommandOutput OpenFilePicker(const fs::path& file_filter_file, const std::string& query);

CommandOutput OpenContentPicker(const fs::path& file_filter_file, const std::string& query);

CommandOutput OpenSymbolPicker(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                               const std::string& query, const fs::path& file);

CommandOutput GoToDefinition(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                             const std::string& symbol);

CommandOutput ShowReferences(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                             const std::string& symbol);

} // namespace jack

#endif // JACK_SRC_COMMANDS_H_
