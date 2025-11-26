#ifndef JACK_SRC_COMMANDS_H_
#define JACK_SRC_COMMANDS_H_

#include <filesystem>
#include <string>

namespace jack {

namespace fs = std::filesystem;

std::string OpenLastPicker(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                           const fs::path& last_picker_state_file);

std::string OpenFilePicker(const fs::path& file_filter_file, const std::string& query);

std::string OpenContentPicker(const fs::path& file_filter_file, const std::string& query);

std::string OpenSymbolPicker(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                             const std::string& query, const fs::path& file);

std::string GoToDefinition(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                           const std::string& symbol);

std::string ShowReferences(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                           const std::string& symbol);

} // namespace jack

#endif // JACK_SRC_COMMANDS_H_
