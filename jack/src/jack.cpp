#include <iostream>
#include "args.h"
#include "printx.hpp"
#include "subprocess.h"

constexpr const char* kVersion = PROJECT_VERSION; // defined in root CMakeLists.txt

namespace jack {

} // namespace jack

int main(int argc, char** argv) {
  common::Args cli(argc, argv);
  constexpr std::string_view kCliHelpMessage = R"CLI(
Usage: jack [options]

Options:
      --open-file-picker          Open file picker (default: false)
      --open-content-picker       Open content picker (default: false)
      --open-last-picker          Open last launched picker (default: false)
      --open-symbol-picker        Open symbol picker (default: false)
      --file                      File whose symbols to be extracted (default: "")
      --goto-definition           Go to definition (default: false)
      --show-references           Show references (default: false)
      --symbol                    Symbol name to be used for --goto-definition and --show-references (default: "")
      --parent-id                 Sway container ID of the launched process (required)
  -h, --help                      Show this help message
      --version                   Print version number
  )CLI";

  if (cli.Has("-h") || cli.Has("--help")) {
    rostd::printf<"%s">(kCliHelpMessage);
    return EXIT_SUCCESS;
  }
  if (cli.Has("--version")) {
    rostd::printf<"%s">(kVersion);
    return EXIT_SUCCESS;
  }

  try {
    jack::SetEnv();
    const std::string output = jack::ShellExec("ls | fzf --multi --bind load:abort");
    std::cout << "SELECTED -------------- \n" << output;
  } catch (const std::exception& ex) {
    rostd::printf<"Exception raised!!\nException: %s\n">(ex.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
