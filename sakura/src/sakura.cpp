#include <vector>
#include <filesystem>
#include <unordered_map>
#include <algorithm>
#include <string_view>

#include <tree_sitter/api.h>
#include <mio/mmap.hpp>

#include "args.h"
#include "config.h"
#include "printx.hpp"

constexpr const char* kVersion = "25.10.0";
namespace fs = std::filesystem;

using ParserFunctionPtr = TSLanguage*(*)();

struct TreesitterQuery {
  TSLanguage* language{nullptr};
  TSQuery* query{nullptr};

  TreesitterQuery() {}
  ~TreesitterQuery() {
    if (query) ts_query_delete(query);
  }
};

extern "C" {
TSLanguage *tree_sitter_cpp();
TSLanguage *tree_sitter_python();
}

static std::unordered_map<std::string, ParserFunctionPtr> kFileExtensionToParserMap {
  {"cpp", tree_sitter_cpp},
  {"python" , tree_sitter_python}
};

static std::string OpenFile(const fs::path& filename) {
  mio::mmap_source contents;
  std::error_code ec;
  contents.map(filename.c_str(), ec);

  std::string out;
  if (ec) {
    rostd::printf<"Error!! Unable to memory map input.\n\tFile: %s\n\tError Code: %d\n\tError Msg: %s\n">(
      filename, ec.value(), ec.message());
    return out;
  }
  out.resize(contents.size());
  std::copy(contents.begin(), contents.end(), out.begin());
  return out;
}

static const char* MemMappedFileRead(void* payload, uint32_t byte_offset,
                                     TSPoint position, uint32_t* bytes_read) {
  std::ignore = position;
  mio::mmap_source* contents = static_cast<mio::mmap_source*>(payload);
  if (byte_offset >= contents->size()) {
    *bytes_read = 0;
    return nullptr;
  }
  const char* start_ptr = contents->data() + byte_offset;
  *bytes_read = contents->size() - byte_offset;
  return start_ptr;
}

static std::string_view LStrip(std::string_view v) {
  // remove leading whitespace
  size_t start = 0;
  while (start < v.size() && std::isspace(v[start])) ++start;
  v.remove_prefix(start);
  return v;
}

static void TreesitterParse(const fs::path& path,
                            const std::unordered_map<std::string, LanguageInfo>& config,
                            const std::unordered_map<std::string, TreesitterQuery>& queries) {
  std::string file_extension = path.extension().string();
  std::transform(file_extension.begin(), file_extension.end(), file_extension.begin(),
                 ::tolower);

  TSLanguage* language{nullptr};
  TSQuery* query{nullptr};
  for (const auto& [lang, info] : config) {
    if (info.file_extensions.contains(file_extension) && queries.contains(lang)) {
      const TreesitterQuery& q = queries.at(lang);
      language = q.language;
      query = q.query;
      break;
    }
  }
  if ((language == nullptr) || (query == nullptr)) {
    return;
  }

  TSParser *parser = ts_parser_new();
  std::ignore = ts_parser_set_language(parser, language);

  mio::mmap_source contents;
  std::error_code ec;
  contents.map(path.c_str(), ec);
  if (ec) {
    rostd::printf<"Error!! Unable to memory map input file.\n\tFile: %s\n\tError Code: %d\n\tError Msg: %s\n">(
      path, ec.value(), ec.message());
    return;
  }  
  if (contents.empty()) {
    ts_parser_delete(parser);
    return;
  }

  TSInput parser_input{};
  parser_input.payload = static_cast<void*>(&contents);
  parser_input.read = MemMappedFileRead;
  parser_input.encoding = TSInputEncoding::TSInputEncodingUTF8;
  TSTree* tree = ts_parser_parse(parser, NULL, parser_input);
  if (!tree) {
    rostd::printf<"Error!! Parsing failed for file %s\n">(path);
    ts_parser_delete(parser);
    return;
  }

  TSNode root_node = ts_tree_root_node(tree);
  TSQueryCursor *cursor = ts_query_cursor_new();
  ts_query_cursor_exec(cursor, query, root_node);
  TSQueryMatch match;

  auto path_native = path.c_str();
  while (ts_query_cursor_next_match(cursor, &match)) {
    for (uint32_t i = 0; i < match.capture_count; i++) {
      TSQueryCapture capture = match.captures[i];
      TSNode node = capture.node;

      TSPoint start_point = ts_node_start_point(node);
      auto start_byte = ts_node_start_byte(node);
      auto end_byte = ts_node_end_byte(node);
      std::string_view symbol_name(contents.begin() + start_byte, end_byte - start_byte);
      symbol_name = LStrip(symbol_name);
      rostd::printf<"%s@%u@%u@%s\n">(path, start_point.row + 1, start_point.column + 1, symbol_name);
    }
  }
  ts_query_cursor_delete(cursor);
  ts_tree_delete(tree);
  ts_parser_delete(parser);
}

static std::unordered_map<std::string, TreesitterQuery>
InitializeQuery(const common::Args& cli,
                const std::unordered_map<std::string, LanguageInfo>& config) {
  std::unordered_map<std::string, TreesitterQuery> out;
  const bool query_definitions = cli.Has("--definitions");
  const bool query_references = cli.Has("--references");

  for (const auto& [lang, info] : config) {

    if (kFileExtensionToParserMap.contains(lang)) {
      std::string full_query;
      if (query_definitions && info.query_definitions.has_value()) {
        full_query.append(OpenFile(info.query_definitions.value()));
      }
      if (query_references && info.query_references.has_value()) {
        full_query.append(OpenFile(info.query_references.value()));
      }

      TSLanguage* const language = kFileExtensionToParserMap.at(lang)();
      uint32_t error_offset{0};
      TSQueryError error_type{TSQueryErrorNone};
      TSQuery* const query = ts_query_new(language, full_query.data(), full_query.size(),
                                          &error_offset, &error_type);
      if (query == nullptr) {
        rostd::printf<"Error!! Query failed.\n\tQuery: %s\n\tError Offset: %u\n\tError Type: %?\n">(
                      full_query, error_offset, error_type);
        continue;
      }
      auto it = out.insert({lang, TreesitterQuery()}).first;
      it->second.language = language;
      it->second.query = query;
    }
  }
  return out;
}

int main(int argc, char** argv) {
  common::Args cli(argc, argv);
  constexpr std::string_view kCliHelpMessage = R"CLI(
Usage: sakura [options]

Options:
      --config        Config file (required)
      --references    List references (default: false)
      --definitions   List definitions (default: true)
      --files         Input list of files (required)
  -h, --help          Show this help message
  -v, --version       Print version number (default: false)
    )CLI";

  if (cli.Has("-h") || cli.Has("--help")) {
    rostd::printf<"%s">(kCliHelpMessage);
    return EXIT_SUCCESS;
  }
  if (cli.Has("--version")) {
    rostd::printf<"%s">(kVersion);
    return EXIT_SUCCESS;
  }
  if (!cli.Has("--config")) {
    rostd::printf<"Error!! Option --config is not specified.">();
    return EXIT_FAILURE;
  }

  using VecStringView = std::vector<std::string_view>;
  const std::string_view config_file = cli.Value({"--config"}).value_or("");
  const VecStringView files = cli.MultiValue({"--files"}, true).value_or(VecStringView{});

  if (!fs::exists(config_file)) {
    rostd::printf<"Error!! Input --config file does not exist.\n\tFile: %s\n">(config_file);
    return EXIT_FAILURE;
  }
  if (files.empty()) {
    rostd::printf<"Error!! Input --files list is empty\n">();
    return EXIT_FAILURE;
  }

  try {
    const std::unordered_map<std::string, LanguageInfo> config = ParseConfig(config_file);
    const std::unordered_map<std::string, TreesitterQuery> queries = InitializeQuery(cli, config);
    for (const std::string_view& file : files) {
      if (!fs::exists(file)) continue;
      TreesitterParse(fs::path{file}, config, queries);
    }
  } catch (const std::exception& ex) {
    rostd::printf<"Exception thrown!!\nException: %s\n">(ex.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
