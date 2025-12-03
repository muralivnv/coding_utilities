#ifndef JACK_SRC_COMMANDS_H_
#define JACK_SRC_COMMANDS_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <optional>

namespace jack {
namespace fs = std::filesystem;

struct CommandOutput {
  struct Span {
    size_t start_offset{};
    size_t length{};
  };
  std::vector<Span> user_selections{};
  std::optional<Span> user_query{std::nullopt};
  std::string storage;

  std::string_view GetUserSelection(size_t index) const;
  std::string_view GetUserQuery() const;
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
