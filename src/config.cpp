#include <fstream>
#include <sstream>
#include "config.h"
#include <argy.hpp>

namespace fs = std::filesystem;

std::unordered_map<std::string, LanguageInfo> ParseConfig(const fs::path& config_file) {
  std::unordered_map<std::string, LanguageInfo> retval;
  std::ifstream file(config_file);  
  if (file.is_open()) {
    std::string line;
    while (std::getline(file, line)) {
      if (line.empty() || line[0] == '#') continue;

      std::vector<std::string> args{"config_parser"};
      std::istringstream iss(line);
      std::string token;
      while (iss >> token) {
        args.push_back(token);
      }
      std::vector<char*> argv;
      for (const std::string& s : args) argv.push_back(const_cast<char*>(s.c_str()));

      Argy::ParsedArgs parsed_config;
      Argy::CliParser config_parser(static_cast<int>(args.size()), argv.data());
      try {
        config_parser.addString({"--language"}         , "Language (cpp/python/...)");
        config_parser.addStrings({"--file-exts"}       , "File extensions");
        config_parser.addString({"--query-definitions"}, "Query file to extract definitions");
        config_parser.addString({"--query-references"} , "Query file to extract references");
        parsed_config = config_parser.parse();
      } catch (const Argy::Exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        config_parser.printHelp(argv[0]);
        continue;
      }

      auto it = retval.insert({parsed_config.getString("language"), LanguageInfo{}}).first;
      auto file_exts = parsed_config.getStrings("file-exts");
      it->second.file_extensions.insert(file_exts.begin(), file_exts.end());
      it->second.query_definitions = config_file.parent_path() / fs::path(parsed_config.getString("query-definitions"));
      it->second.query_references = config_file.parent_path() / fs::path(parsed_config.getString("query-references"));      
    }
    file.close();
  }
  return retval;
}
