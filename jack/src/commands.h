#ifndef JACK_SRC_COMMANDS_H_
#define JACK_SRC_COMMANDS_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <optional>

namespace jack {
namespace fs = std::filesystem;

struct CommandOutput {
  std::vector<std::string_view> user_selections{};
  std::string_view user_query{};
  std::string storage;
};

std::optional<CommandOutput> OpenFilePicker(const fs::path& file_filter_file, const std::string& query);

std::optional<CommandOutput> OpenContentPicker(const fs::path& file_filter_file, const std::string& query);

std::optional<CommandOutput> OpenSymbolPicker(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                                              const std::string& query, const fs::path& file);

std::optional<CommandOutput> GoToDefinition(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                                            const std::string& symbol);

std::optional<CommandOutput> ShowReferences(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                                            const std::string& symbol);

} // namespace jack

#endif // JACK_SRC_COMMANDS_H_
