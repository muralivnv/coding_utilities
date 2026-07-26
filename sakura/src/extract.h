#ifndef SAKURA_EXTRACT_H_
#define SAKURA_EXTRACT_H_

#include <tree_sitter/api.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "config.h"

namespace sakura {

struct TreesitterQuery {
  TSLanguage* language{nullptr};
  TSQuery* query{nullptr};

  TreesitterQuery() {}
  ~TreesitterQuery() {
    if (query)
      ts_query_delete(query);
  }
  TreesitterQuery(const TreesitterQuery&) = delete;
  TreesitterQuery& operator=(const TreesitterQuery&) = delete;
  TreesitterQuery(TreesitterQuery&& other) noexcept : language(other.language), query(other.query) {
    other.language = nullptr;
    other.query = nullptr;
  }
  TreesitterQuery& operator=(TreesitterQuery&& other) noexcept {
    if (this != &other) {
      if (query)
        ts_query_delete(query);
      language = other.language;
      query = other.query;
      other.language = nullptr;
      other.query = nullptr;
    }
    return *this;
  }
};

// One captured symbol. Row and column are 1-based, matching what an editor shows.
struct Symbol {
  uint32_t row{0};
  uint32_t col{0};
  std::string name{};
};

bool HasParserFor(std::string_view language);

std::string_view LStrip(std::string_view v);

std::string_view LanguageForExtension(const std::filesystem::path& path,
                                      const std::unordered_map<std::string, LanguageInfo>& config);

std::unordered_map<std::string, TreesitterQuery> InitializeQueries(
    bool want_definitions, bool want_references, const std::unordered_map<std::string, LanguageInfo>& config);

std::vector<Symbol> ExtractSymbols(const std::filesystem::path& path,
                                   const std::unordered_map<std::string, LanguageInfo>& config,
                                   const std::unordered_map<std::string, TreesitterQuery>& queries);

}  // namespace sakura

#endif  // SAKURA_EXTRACT_H_
