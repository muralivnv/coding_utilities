#include "printx.hpp"

#include <cstdlib>
#include <cstring>

#include <unistd.h>

#include "mmap_file.h"
#include "args.h"
#include "input.h"
#include "operation.h"
#include "process.h"

constexpr const char* kVersion = PROJECT_VERSION;  // defined in root CMakeLists.txt

namespace gai {

constexpr int kExitOk = EXIT_SUCCESS;
constexpr int kExitFatal = EXIT_FAILURE;
constexpr int kExitMatchError = 2;

static bool ResolveColor(const common::Args& cli) {
  if (cli.Has("--no-color"))
    return false;
  if (cli.Has("--color"))
    return true;
  if (!::isatty(STDOUT_FILENO))
    return false;
  // https://no-color.org -- set to anything non-empty means opt out.
  const char* const no_color = std::getenv("NO_COLOR");
  if ((no_color != nullptr) && (no_color[0] != '\0'))
    return false;
  const char* const term = std::getenv("TERM");
  if ((term == nullptr) || (std::strcmp(term, "dumb") == 0))
    return false;
  return true;
}

}  // namespace gai

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOFBF, 1 << 20);
  common::Args cli(argc, argv);
  constexpr std::string_view kCliHelpMessage = R"CLI(
Usage: gai [options]

Options:
  -f, --filter              List of filters (default: [])
  -e, --exclude             List of exclusions (default: [])
  -r, --replace             List of replacements: /pattern/replacement/[g]
                            Replaces the first match only; append 'g' to replace
                            every match on the line (default: [])
      --range               Optional filter range (default: )
      --utf                 Enable UTF (default: false)
      --no-jit              Disable JIT compilation of expressions (default: false)
      --files               List of Input files. If not given STDIN will be used (default: [])
      --read0               Use null delimiter to split lines
  -v, --verbose             Verbose print output (default: false)
  -d, --delim               Delimiter to use for verbose printing (default - ':')
      --color               Highlight matches even when not writing to a terminal
      --no-color            Never highlight matches
                            Without either, matches are highlighted only when
                            stdout is a terminal, TERM is not 'dumb', and NO_COLOR
                            is unset
  -h, --help                Show this help message
      --version             Print version number

Exit status:
  0  the scan completed
  1  nothing was scanned: a pattern would not compile, an expression was
     malformed, or a substitution failed
  2  the scan completed but some lines could not be matched: a pattern hit
     pcre2's match or depth limit. The reason is on stderr, once per pattern,
     and the output is a subset of what a working pattern would have printed
  )CLI";

  if (cli.Has("-h") || cli.Has("--help")) {
    rostd::printf<"%s">(kCliHelpMessage);
    return gai::kExitOk;
  }
  if (cli.Has("--version")) {
    rostd::printf<"%s">(kVersion);
    return gai::kExitOk;
  }
  gai::MatchDiagnostics diagnostics;
  try {
    using VecStringView = std::vector<std::string_view>;
    const bool jit = !cli.Has("--no-jit");
    const bool utf = cli.Has("--utf");
    const bool verbose = cli.Has("--verbose") || cli.Has("-v");
    std::string_view delimiter = cli.Value({"-d", "--delim"}).value_or(":");

    const VecStringView filter_exprs = cli.MultiValue({"-f", "--filter"}, true).value_or(VecStringView{});
    const VecStringView exclude_exprs = cli.MultiValue({"-e", "--exclude"}, true).value_or(VecStringView{});
    const VecStringView replace_exprs = cli.MultiValue({"-r", "--replace"}, true).value_or(VecStringView{});
    const std::string_view range_expr = cli.Value({"--range"}).value_or("");

    const std::vector<gai::Pcre2Regex> filters = gai::ParseFilters(filter_exprs, jit, utf);
    const std::vector<gai::Pcre2Regex> excludes = gai::ParseFilters(exclude_exprs, jit, utf);
    const std::vector<gai::Pcre2Substitution> replacements = gai::ParseSubstitutions(replace_exprs, jit, utf);
    std::optional<gai::Range> range = gai::ParseRange(range_expr, jit, utf);
    const VecStringView files = cli.MultiValue({"--files"}, true).value_or(VecStringView{});

    const bool read0 = cli.Has("--read0");
    const bool color = gai::ResolveColor(cli);

    if (files.empty()) {
      const gai::OutputFunc fn = gai::MakeOutputFunc(verbose, delimiter, read0, {}, color);
      gai::InputStream stream(read0);
      const gai::ScanConfig config{filters, excludes, replacements, color, {}};
      gai::Process(config, fn, range, &stream, diagnostics);
    } else {
      for (const std::string_view& f : files) {
        std::optional<common::MmapFileReadOnly> contents = common::MmapFileReadOnly::Open(std::string{f});
        if (!contents)
          continue;
        gai::InputMemMappedFile mmap_stream(contents->begin(), contents->end(), read0);
        if (range)
          range->Reset();
        const gai::OutputFunc fn = gai::MakeOutputFunc(verbose, delimiter, read0, f, color);
        const gai::ScanConfig config{filters, excludes, replacements, color, f};
        gai::Process(config, fn, range, &mmap_stream, diagnostics);
      }
    }
  } catch (const std::exception& ex) {
    rostd::printf<"Exception raised!!\nException: %s\n">(ex.what());
    return gai::kExitFatal;
  }
  // Whatever matched has already been printed; the status is what says the
  // list is a subset. Diagnostics themselves went to stderr as they happened.
  return diagnostics.Any() ? gai::kExitMatchError : gai::kExitOk;
}
