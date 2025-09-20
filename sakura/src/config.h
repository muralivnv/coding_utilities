#ifndef CONFIG_H_
#define CONFIG_H_

#include <filesystem>
#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <string>

struct LanguageInfo {
  std::unordered_set<std::string> file_extensions{};
  std::optional<std::filesystem::path> query_definitions{std::nullopt};
  std::optional<std::filesystem::path> query_references{std::nullopt};
};

std::unordered_map<std::string, LanguageInfo> ParseConfig(const std::filesystem::path& config_file);

#endif // CONFIG_H_
