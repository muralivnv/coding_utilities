#include "extract.h"

#include <algorithm>
#include <cctype>
#include <memory>

#include "printx.hpp"
#include "mmap_file.h"

namespace fs = std::filesystem;

extern "C" {
TSLanguage* tree_sitter_cpp();
TSLanguage* tree_sitter_python();
}

namespace sakura {
namespace {

using ParserFunctionPtr = TSLanguage* (*)();

const std::unordered_map<std::string, ParserFunctionPtr>& ParserMap() {
  static const std::unordered_map<std::string, ParserFunctionPtr> kMap{{"cpp", tree_sitter_cpp},
                                                                      {"python", tree_sitter_python}};
  return kMap;
}

std::string OpenFile(const fs::path& filename) {
  auto contents = common::MmapFileReadOnly::Open(filename);
  if (!contents) {
    rostd::printf<"Error!! Unable to memory map input.\n\tFile: %s\n">(filename);
    return {};
  }
  return std::string{contents->data(), contents->size()};
}

// tree-sitter pulls the subject through this rather than taking a buffer, so the
// whole file never needs copying into the parser.
const char* MemMappedFileRead(void* payload, uint32_t byte_offset, TSPoint position, uint32_t* bytes_read) {
  std::ignore = position;
  common::MmapFileReadOnly* contents = static_cast<common::MmapFileReadOnly*>(payload);
  if (byte_offset >= contents->size()) {
    *bytes_read = 0;
    return nullptr;
  }
  const char* start_ptr = contents->data() + byte_offset;
  *bytes_read = contents->size() - byte_offset;
  return start_ptr;
}

}  // namespace

bool HasParserFor(std::string_view language) {
  return ParserMap().contains(std::string{language});
}

std::string_view LStrip(std::string_view v) {
  // remove leading whitespace
  size_t start = 0;
  while (start < v.size() && std::isspace(static_cast<unsigned char>(v[start]))) ++start;
  v.remove_prefix(start);
  return v;
}

std::string_view LanguageForExtension(const fs::path& path,
                                      const std::unordered_map<std::string, LanguageInfo>& config) {
  std::string file_extension = path.extension().string();
  std::transform(file_extension.begin(), file_extension.end(), file_extension.begin(), ::tolower);

  for (const auto& [lang, info] : config) {
    if (info.file_extensions.contains(file_extension))
      return lang;
  }
  return {};
}

std::unordered_map<std::string, TreesitterQuery> InitializeQueries(
    bool want_definitions, bool want_references, const std::unordered_map<std::string, LanguageInfo>& config) {
  std::unordered_map<std::string, TreesitterQuery> out;

  for (const auto& [lang, info] : config) {
    if (!HasParserFor(lang))
      continue;

    std::string full_query;
    if (want_definitions && info.query_definitions.has_value()) {
      full_query.append(OpenFile(info.query_definitions.value()));
    }
    if (want_references && info.query_references.has_value()) {
      full_query.append(OpenFile(info.query_references.value()));
    }

    TSLanguage* const language = ParserMap().at(lang)();
    uint32_t error_offset{0};
    TSQueryError error_type{TSQueryErrorNone};
    TSQuery* const query = ts_query_new(language, full_query.data(), full_query.size(), &error_offset, &error_type);
    if (query == nullptr) {
      rostd::printf<"Error!! Query failed.\n\tQuery: %s\n\tError Offset: %u\n\tError Type: %?\n">(
          full_query, error_offset, error_type);
      continue;
    }
    auto it = out.insert({lang, TreesitterQuery()}).first;
    it->second.language = language;
    it->second.query = query;
  }
  return out;
}

std::generator<Symbol> ExtractSymbols(const fs::path& path, const std::unordered_map<std::string, LanguageInfo>& config,
                                      const std::unordered_map<std::string, TreesitterQuery>& queries) {
  std::error_code size_ec;
  if (fs::file_size(path, size_ec) == 0 || size_ec)
    co_return;

  const std::string_view lang = LanguageForExtension(path, config);
  if (lang.empty())
    co_return;
  const auto query_it = queries.find(std::string{lang});
  if (query_it == queries.end())
    co_return;

  TSLanguage* const language = query_it->second.language;
  TSQuery* const query = query_it->second.query;
  if ((language == nullptr) || (query == nullptr))
    co_return;

  auto contents = common::MmapFileReadOnly::Open(path.string());
  if (!contents) {
    rostd::printf<"Error!! Unable to memory map input file.\n\tFile: %s\n">(path);
    co_return;
  }

  std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser(ts_parser_new(), ts_parser_delete);
  std::ignore = ts_parser_set_language(parser.get(), language);

  TSInput parser_input{};
  parser_input.payload = static_cast<void*>(&contents.value());
  parser_input.read = MemMappedFileRead;
  parser_input.encoding = TSInputEncoding::TSInputEncodingUTF8;

  std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree(
      ts_parser_parse(parser.get(), nullptr, parser_input), ts_tree_delete);

  std::unique_ptr<TSQueryCursor, decltype(&ts_query_cursor_delete)> cursor(
      ts_query_cursor_new(), ts_query_cursor_delete);

  if (!tree) {
    rostd::printf<"Error!! Parsing failed for file %s\n">(path);
    co_return;
  }

  TSNode root_node = ts_tree_root_node(tree.get());
  ts_query_cursor_exec(cursor.get(), query, root_node);
  TSQueryMatch match;

  while (ts_query_cursor_next_match(cursor.get(), &match)) {
    for (uint32_t i = 0; i < match.capture_count; i++) {
      TSQueryCapture capture = match.captures[i];
      TSNode node = capture.node;

      TSPoint start_point = ts_node_start_point(node);
      const auto start_byte = ts_node_start_byte(node);
      const auto end_byte = ts_node_end_byte(node);
      std::string_view symbol_name(contents->begin() + start_byte, end_byte - start_byte);
      symbol_name = LStrip(symbol_name);
      co_yield Symbol{start_point.row + 1, start_point.column + 1, std::string{symbol_name}};
    }
  }
}

}  // namespace sakura
