#include "commands.h"
#include "subprocess.h"

namespace jack {

constexpr const char * FZF_COMMAND = "fzf --ansi --border -i --preview 'bat {1} --highlight-line {2}' --preview-window 'right,+{2}+3/3,~3' "
                                     "--delimiter '@' --scrollbar '▍' "
                                     "--nth=-1 --bind=tab:down,shift-tab:up --smart-case --cycle "
                                     "--style=full:line --layout=reverse --print-query";

static std::string Execute(const std::string& name, const std::string& cmd, const std::string& query) {
  // TODO: REQUIRES IMPLEMENTATION
  return "";
}

std::string OpenLastPicker(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                           const fs::path& last_picker_state_file) {
  // TODO: REQUIRES IMPLEMENTATION
  return "";
}

std::string OpenFilePicker(const fs::path& file_filter_file, const std::string& query) {
  // TODO: REQUIRES IMPLEMENTATION
  return "";
}

std::string OpenContentPicker(const fs::path& file_filter_file, const std::string& query) {
  // TODO: REQUIRES IMPLEMENTATION
  return "";
}

std::string OpenSymbolPicker(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                             const std::string& query, const fs::path& file) {
  // TODO: REQUIRES IMPLEMENTATION
  return "";
}

std::string GoToDefinition(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                           const std::string& symbol) {
  // TODO: REQUIRES IMPLEMENTATION
  return "";
}

std::string ShowReferences(const fs::path& file_filter_file, const fs::path& treesitter_tags_file,
                           const std::string& symbol) {
  // TODO: REQUIRES IMPLEMENTATION
  return "";
}

} // namespace jack
