#include <vector>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <algorithm>
#include <string_view>
#include <tree_sitter/api.h>
#include <mio/mmap.hpp>
#include <charconv>

#include "config.h"

namespace fs = std::filesystem;

using ParserFunctionPtr = TSLanguage*(*)();

struct InputArguments {
  fs::path config_file;
  std::vector<fs::path> input_files;
  bool query_definitions{false};
  bool query_references{false};
};

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
  std::ifstream infile(filename);
  std::string out;
  if (infile.is_open()) {
    const size_t streamsize = fs::file_size(filename);
    out.resize(streamsize);
    infile.read(reinterpret_cast<char*>(out.data()), streamsize);
    infile.close();
  }
  return out;
}

static std::string_view FlattenSignature(std::string &s) {
  size_t len = s.size();
  if (len == 0) return {};

  size_t read = 0;
  while (read < len && std::isspace(static_cast<unsigned char>(s[read])))
    ++read;

  size_t write = 0;
  bool in_space = false;
  size_t count = 0;

  static constexpr char ellipsis[] = "...";
  constexpr size_t max_len = 90;

  while (read < len) {
    // Check if 'std::' starts here
    if (read + 4 < len && s[read] == 's' && s[read + 1] == 't' &&
        s[read + 2] == 'd' && s[read + 3] == ':' && s[read + 4] == ':') {
      read += 5;  // skip "std::"
      continue;
    }

    char c = s[read++];

    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!in_space) {
        s[write++] = ' ';
        ++count;
        in_space = true;
      }
    } else {
      s[write++] = c;
      ++count;
      in_space = false;
    }

    if (count >= max_len) {
      size_t ellipsis_len = sizeof(ellipsis) - 1;
      if (write >= ellipsis_len) {
        std::copy(ellipsis, ellipsis + ellipsis_len,
                  s.begin() + write - ellipsis_len);
        s.erase(write);
        return std::string_view(s.data(), write);
      } else {
        size_t new_end = std::min(write + ellipsis_len, len);
        std::copy(ellipsis, ellipsis + (new_end - write), s.begin() + write);
        s.erase(new_end);
        return std::string_view(s.data(), new_end);
      }
    }
  }

  if (write > 0 && s[write - 1] == ' ') --write;
  s.erase(write);
  return std::string_view(s.data(), write);
}

static const char* MemMappedFileRead(void* payload, uint32_t byte_offset,
                                     TSPoint position, uint32_t* bytes_read) {
  mio::mmap_source* contents = static_cast<mio::mmap_source*>(payload);
  if (byte_offset >= contents->size()) {
    *bytes_read = 0;
    return nullptr;
  }
  const char* start_ptr = contents->data() + byte_offset;
  *bytes_read = contents->size() - byte_offset;
  return start_ptr;
}

template<size_t N>
static void WriteToBuffer(std::basic_streambuf<char, std::char_traits<char>>* const buffer,
                          char (&intermediary_buffer)[N], size_t value) {
  auto [ptr, errc] = std::to_chars(intermediary_buffer, intermediary_buffer + N, value);
  if (errc == std::errc()) {
    buffer->sputn(intermediary_buffer, std::distance(intermediary_buffer, ptr));
  }
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
  if (contents.empty()) {
    ts_parser_delete(parser);
    return;
  }

  TSInput parser_input;
  parser_input.payload = static_cast<void*>(&contents);
  parser_input.read = MemMappedFileRead;
  parser_input.encoding = TSInputEncoding::TSInputEncodingUTF8;
  TSTree* tree = ts_parser_parse(parser, NULL, parser_input);
  if (!tree) {
    std::cerr << "[ERROR] Parsing failed for file: " << path.string() << '\n';
    ts_parser_delete(parser);
    return;
  }

  TSNode root_node = ts_tree_root_node(tree);
  TSQueryCursor *cursor = ts_query_cursor_new();
  ts_query_cursor_exec(cursor, query, root_node);
  TSQueryMatch match;

  auto cout_buffer = std::cout.rdbuf();
  static char temp[50];
  const std::string path_str = path.string();
  std::string symbol_name;
  while (ts_query_cursor_next_match(cursor, &match)) {
    for (uint32_t i = 0; i < match.capture_count; i++) {
      TSQueryCapture capture = match.captures[i];
      TSNode node = capture.node;

      TSPoint start_point = ts_node_start_point(node);
      auto start_byte = ts_node_start_byte(node);
      auto end_byte = ts_node_end_byte(node);
      symbol_name.assign(contents.begin() + start_byte, contents.begin() + end_byte);
      std::string_view name_flattened = FlattenSignature(symbol_name);

      cout_buffer->sputn(path_str.data(), path_str.size());
      cout_buffer->sputc('@');
      WriteToBuffer(cout_buffer, temp, start_point.row + 1);
      cout_buffer->sputc('@');
      WriteToBuffer(cout_buffer, temp, start_point.column + 1);
      cout_buffer->sputc('@');
      cout_buffer->sputn(name_flattened.data(), name_flattened.size());
      cout_buffer->sputc('\n');
    }
  }
  ts_query_cursor_delete(cursor);
  ts_tree_delete(tree);
  ts_parser_delete(parser);
}

static InputArguments ParseCliArgs(int argc, char** argv) {
  InputArguments out;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      out.config_file = argv[++i];
    } else if (arg == "--definitions") {
      out.query_definitions = true;
    } else if (arg == "--references") {
      out.query_references = true;
    } else {
      out.input_files.emplace_back(argv[i]);
    }
  }
  return out;
}

static bool Validate(const InputArguments& input_args) {
  if (input_args.config_file.empty()) {
    std::cerr << "[ERROR] No --config file specified.\n";
    return false;
  }
  if (!fs::exists(input_args.config_file)) {
    std::cerr << "[ERROR] Config file does not exist: " << input_args.config_file << '\n';
    return false;
  }
  if (input_args.input_files.empty()) {
    std::cerr << "[ERROR] No input files specified.\n";
    return false;
  }
  if (!input_args.query_definitions && !input_args.query_references) {
    std::cerr << "[ERROR] No query type specified. Specify either or both of --definitions, --references\n";
    return false;
  }
  return true;
}

static std::unordered_map<std::string, TreesitterQuery>
InitializeQuery(const InputArguments& input_args,
                const std::unordered_map<std::string, LanguageInfo>& config) {
  std::unordered_map<std::string, TreesitterQuery> out;

  for (const auto& [lang, info] : config) {

    if (kFileExtensionToParserMap.contains(lang)) {
      std::string full_query;
      if (input_args.query_definitions && info.query_definitions.has_value()) {
        full_query.append(OpenFile(info.query_definitions.value()));
      }
      if (input_args.query_references && info.query_references.has_value()) {
        full_query.append(OpenFile(info.query_references.value()));
      }

      TSLanguage* const language = kFileExtensionToParserMap.at(lang)();
      uint32_t error_offset;
      TSQueryError error_type;
      TSQuery* const query = ts_query_new(language, full_query.data(), full_query.size(),
                                          &error_offset, &error_type);
      if (query == nullptr) {
        std::cerr << "[ERROR] Query failed to query -- " << full_query << '\n';
        std::cerr << "\tError at offset: " << error_offset << ", Error type: " << static_cast<int>(error_type) << '\n';
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
  const InputArguments cli_args = ParseCliArgs(argc, argv);
  if (!Validate(cli_args)) {
    std::cerr << "Usage: treesitter_tags --config <config_file> --definitions --references <source_file> <source_file> ...\n";
    return EXIT_FAILURE;
  }
  const std::unordered_map<std::string, LanguageInfo> config = ParseConfig(cli_args.config_file);
  const std::unordered_map<std::string, TreesitterQuery> queries = InitializeQuery(cli_args, config);
  for (const fs::path& file : cli_args.input_files) {
    TreesitterParse(file, config, queries);
  }
  return EXIT_SUCCESS;
}
