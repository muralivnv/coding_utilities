#include <functional>
#include <mio/mmap.hpp>

#include "args.h"
#include "operation.h"
#include "input.h"
#include "printx.hpp"

constexpr const char* kVersion = "25.10.1";

namespace gai {
using OutputFunc = std::function<void(std::string_view, size_t)>;

static void Process(const std::vector<gai::Pcre2Regex>& filters,
                    const std::vector<gai::Pcre2Regex>& excludes,
                    const std::vector<gai::Pcre2Substitution>& replacements,
                    const OutputFunc& out_fn,
                    std::optional<gai::Range>& range, InputBase* const input) {
  thread_local std::string replacement_buffer(1024, ' ');
  thread_local std::string replacement_line(1024, ' ');
  size_t linenum = 0;
  while (std::optional<std::string_view> line_opt = input->GetLine()) {
    ++linenum;
    std::string_view& line = line_opt.value();
    if (range) {
      if (!range->IsStartReached(line, linenum)) continue;
      if (range->IsEndReached(line, linenum)) continue;
    }

    bool match = std::any_of(filters.begin(), filters.end(),
                             [&line](const auto& r) { return Find(r, line); });
    if (!filters.empty() && !match) {
      continue;
    }

    match = std::any_of(excludes.begin(), excludes.end(),
                        [&line](const auto& r) { return Find(r, line); });
    if (!excludes.empty() && match) {
      continue;
    }

    if (!replacements.empty()) {
      replacement_line.assign(line);
      for (const Pcre2Substitution& r : replacements) {    
        std::string_view replace = Substitute(r, replacement_line, replacement_buffer);
        replacement_line.assign(replace);
      }
      out_fn(replacement_line, linenum);
    } else {
      out_fn(line, linenum);
    }
  }
}

static void NormalPrint(std::string_view content, size_t linenum) {
  std::ignore = linenum;
  rostd::printf<"%s\n">(content);
}

static gai::OutputFunc MakeOutputFunc(bool verbose, std::string_view delimiter,
                                      std::string_view filename = {}) {
  if (!verbose) return gai::NormalPrint;
  if (filename.empty()) {
    return [delimiter](std::string_view c, size_t k) {
      rostd::printf<"%zu%s%s\n">(k, delimiter, c);
    };
  }
  return [filename, delimiter](std::string_view c, size_t k) {
    rostd::printf<"%s%s%zu%s%s\n">(filename, delimiter, k, delimiter, c);
  };
}

} // namespace gai

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOFBF, 1 << 20);
  common::Args cli(argc, argv);
  constexpr std::string_view kCliHelpMessage = R"CLI(
Usage: gai [options]

Options:
  -f, --filter              List of filters (default: [])
  -e, --exclude             List of exclusions (default: [])
  -r, --replace             List of replacements (default: [])
      --range               Optional filter range (default: )
      --utf                 Enable UTF (default: false)
      --no-jit              Disable JIT compilation of expressions (default: false)
      --files               List of Input files. If not given STDIN will be used (default: [])
  -v, --verbose             Verbose print output (default: false)
  -d, --delim               Delimiter to use for verbose printing (default - ':')
  -h, --help                Show this help message
      --version             Print version number
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
    using VecStringView = std::vector<std::string_view>;
    const bool jit = !cli.Has("--no-jit");
    const bool utf = cli.Has("--utf");
    const bool verbose = cli.Has("--verbose") || cli.Has("-v");
    std::string_view delimiter = cli.Value({"-d", "--delim"}).value_or(":");

    const VecStringView filter_exprs  = cli.MultiValue({"-f", "--filter"}, true).value_or(VecStringView{});
    const VecStringView exclude_exprs = cli.MultiValue({"-e", "--exclude"}, true).value_or(VecStringView{});
    const VecStringView replace_exprs = cli.MultiValue({"-r", "--replace"}, true).value_or(VecStringView{});
    const std::string_view range_expr = cli.Value({"--range"}).value_or("");

    const std::vector<gai::Pcre2Regex> filters = gai::ParseFilters(filter_exprs, jit, utf);
    const std::vector<gai::Pcre2Regex> excludes = gai::ParseFilters(exclude_exprs, jit, utf);
    const std::vector<gai::Pcre2Substitution> replacements = gai::ParseSubstitutions(replace_exprs, jit, utf);
    std::optional<gai::Range> range = gai::ParseRange(range_expr, jit, utf);
    const VecStringView files = cli.MultiValue({"--files"}, true).value_or(VecStringView{});

    if (files.empty()) {
      const gai::OutputFunc fn = gai::MakeOutputFunc(verbose, delimiter);
      gai::InputStream stream;
      gai::Process(filters, excludes, replacements, fn, range, &stream);
    } else {
      for (const std::string_view& f : files) {
        mio::mmap_source contents;
        std::error_code ec;
        contents.map(f, ec);
        if (ec) continue;

        gai::InputMemMappedFile mmap_stream(contents.begin(), contents.end());
        if (range) range->Reset();
        const gai::OutputFunc fn = gai::MakeOutputFunc(verbose, delimiter, f);
        gai::Process(filters, excludes, replacements, fn, range, &mmap_stream);
      }
    }
  } catch (const std::exception& ex) {
    rostd::printf<"Exception raised!!\nException: %s\n">(ex.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
