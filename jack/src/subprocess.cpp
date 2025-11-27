#include <cstdio>
#include <cstdlib>
#include <array>
#include <vector>
#include <string>
#include <filesystem>

#include "format.h"

namespace fs = std::filesystem;
namespace jack {

void SetEnv() {
  const char* current_path_cstr = getenv("PATH");
  std::string current_path = current_path_cstr ? current_path_cstr : "";
  const char* home_dir_cstr = getenv("HOME");
  if (!home_dir_cstr) {
    throw std::runtime_error("HOME environment variable not set.\n");
    return;
  }
  const fs::path home_path(home_dir_cstr); 
  const fs::path custom_bin_path = home_path / ".local" / "bin";
  const std::string new_path = custom_bin_path.string() + ":" + current_path;
  if (setenv("PATH", new_path.c_str(), 1) != 0) {
    throw std::runtime_error("setenv command failed.\n");
  }
}

std::string ShellExec(const char* cmd) {
  static std::array<char, 256> buffer;
  FILE* pipe = popen(cmd, "r");
  if (!pipe) {
    std::string_view error_msg = common::FormatIntoStringView<"popen failed to start command.\nCommand: %?">(cmd);
    throw std::runtime_error(std::string(error_msg));    
  }

  std::string result;
  while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result += buffer.data();
  }
  int status = pclose(pipe);
  if (status != 0) {
    std::string_view error_msg = common::FormatIntoStringView<"Command failed with status %d.\nCommand: %?">(
                                                              WEXITSTATUS(status),cmd);
    throw std::runtime_error(std::string(error_msg));
  }
  return result;
}

} // namespace jack
