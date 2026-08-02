#include "printx.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include <unistd.h>

#include "mmap_file.h"
#include "args.h"
#include "input.h"
#include "operation.h"

constexpr const char* kVersion = PROJECT_VERSION;  // defined in root CMakeLists.txt

namespace gai {
using OutputFunc = std::function<void(std::string_view, size_t)>;

static constexpr std::string_view kSgrMatch     = "\x1b[01;31m";
static constexpr std::string_view kSgrMatchEnd  = "\x1b[m\x1b[K";
static constexpr std::string_view kSgrFilename  = "\x1b[35m";
static constexpr std::string_view kSgrLineNum   = "\x1b[32m";
static constexpr std::string_view kSgrSeparator = "\x1b[36m";
static constexpr std::string_view kSgrEnd       = "\x1b[m";

consteval bool IsSgrSequence(std::string_view sgr) {
  size_t i = 0;
  while (i < sgr.size()) {
    if (i + 2 >= sgr.size() || sgr[i] != '\x1b' || sgr[i + 1] != '[')
      return false;
    i += 2;
    while (i < sgr.size() && ((sgr[i] >= '0' && sgr[i] <= '9') || sgr[i] == ';')) ++i;
    if (i >= sgr.size() || (sgr[i] != 'm' && sgr[i] != 'K'))
      return false;
    ++i;
  }
  return i == sgr.size() && !sgr.empty();
}

static_assert(IsSgrSequence(kSgrMatch));
static_assert(IsSgrSequence(kSgrMatchEnd));
static_assert(IsSgrSequence(kSgrFilename));
static_assert(IsSgrSequence(kSgrLineNum));
static_assert(IsSgrSequence(kSgrSeparator));
static_assert(IsSgrSequence(kSgrEnd));
static_assert(!IsSgrSequence("\x1b[01;31"), "a sequence with no final byte must be rejected");
static_assert(!IsSgrSequence("[01;31m"), "a sequence with no CSI introducer must be rejected");

static constexpr size_t kSgrBytesPerSpan = kSgrMatch.size() + kSgrMatchEnd.size();

static std::string_view Colorize(std::string_view line, const std::vector<Pcre2Regex>& filters,
                                 std::vector<MatchSpan>& spans, std::string& buffer) {
  spans.clear();
  for (const Pcre2Regex& filter : filters) FindSpans(filter, line, spans);
  if (spans.empty())
    return line;
  MergeSpans(spans);

  buffer.clear();
  buffer.reserve(line.size() + spans.size() * kSgrBytesPerSpan);
  size_t at = 0;
  for (const MatchSpan& span : spans) {
    if (span.start > at)
      buffer.append(line.data() + at, span.start - at);
    buffer.append(kSgrMatch);
    buffer.append(line.data() + span.start, span.end - span.start);
    buffer.append(kSgrMatchEnd);
    at = span.end;
  }
  buffer.append(line.data() + at, line.size() - at);
  return buffer;
}

// Writes text, wrapped in an SGR sequence when colouring.
static inline void WriteField(std::string_view text, std::string_view sgr, bool color) {
  if (color)
    ::fwrite_unlocked(sgr.data(), 1, sgr.size(), stdout);
  ::fwrite_unlocked(text.data(), 1, text.size(), stdout);
  if (color)
    ::fwrite_unlocked(kSgrEnd.data(), 1, kSgrEnd.size(), stdout);
}

static void Process(const std::vector<gai::Pcre2Regex>& filters, const std::vector<gai::Pcre2Regex>& excludes,
                    const std::vector<gai::Pcre2Substitution>& replacements, const OutputFunc& out_fn,
                    std::optional<gai::Range>& range, InputBase* const input, bool color) {
  thread_local std::string replacement_buffer(1024, ' ');
  thread_local std::string replacement_line(1024, ' ');
  thread_local std::string color_buffer;
  thread_local std::vector<MatchSpan> spans;
  size_t linenum = 0;
  while (std::optional<std::string_view> line_opt = input->GetLine()) {
    ++linenum;
    std::string_view& line = line_opt.value();
    if (range) {
      if (!range->IsStartReached(line, linenum))
        continue;
      if (range->IsEndReached(line, linenum))
        continue;
    }

    bool match = std::any_of(filters.begin(), filters.end(), [&line](const auto& r) { return Find(r, line); });
    if (!filters.empty() && !match) {
      continue;
    }

    match = std::any_of(excludes.begin(), excludes.end(), [&line](const auto& r) { return Find(r, line); });
    if (!excludes.empty() && match) {
      continue;
    }

    std::string_view emit = line;
    if (!replacements.empty()) {
      replacement_line.assign(line);
      for (const Pcre2Substitution& r : replacements) {
        std::string_view replace = Substitute(r, replacement_line, replacement_buffer);
        replacement_line.assign(replace);
      }
      emit = replacement_line;
    }

    if (color)
      emit = Colorize(emit, filters, spans, color_buffer);

    out_fn(emit, linenum);
  }
}

static gai::OutputFunc MakeOutputFunc(bool verbose, std::string_view delimiter, bool print0 = false,
                                      std::string_view filename = {}, bool color = false) {
  char newline_char = '\n';
  if (print0)
    newline_char = '\0';
  if (!verbose) {
    return [newline_char](std::string_view c, size_t /*k*/) {
      ::fwrite_unlocked(c.data(), 1, c.size(), stdout);
      ::putc_unlocked(newline_char, stdout);
    };
  }

  if (filename.empty()) {
    return [delimiter, newline_char, color](std::string_view c, size_t k) {
      char num[24];
      const int n = rostd::snprintf<"%zu">(num, sizeof(num), k);
      WriteField({num, static_cast<size_t>(n)}, kSgrLineNum, color);
      WriteField(delimiter, kSgrSeparator, color);
      ::fwrite_unlocked(c.data(), 1, c.size(), stdout);
      ::putc_unlocked(newline_char, stdout);
    };
  }
  return [filename, delimiter, newline_char, color](std::string_view c, size_t k) {
    char num[24];
    const int n = rostd::snprintf<"%zu">(num, sizeof(num), k);
    WriteField(filename, kSgrFilename, color);
    WriteField(delimiter, kSgrSeparator, color);
    WriteField({num, static_cast<size_t>(n)}, kSgrLineNum, color);
    WriteField(delimiter, kSgrSeparator, color);
    ::fwrite_unlocked(c.data(), 1, c.size(), stdout);
    ::putc_unlocked(newline_char, stdout);
  };
}

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
      gai::Process(filters, excludes, replacements, fn, range, &stream, color);
    } else {
      for (const std::string_view& f : files) {
        std::optional<common::MmapFileReadOnly> contents = common::MmapFileReadOnly::Open(std::string{f});        
        if (!contents)
          continue;
        gai::InputMemMappedFile mmap_stream(contents->begin(), contents->end(), read0);
        if (range)
          range->Reset();
        const gai::OutputFunc fn = gai::MakeOutputFunc(verbose, delimiter, read0, f, color);
        gai::Process(filters, excludes, replacements, fn, range, &mmap_stream, color);
      }
    }
  } catch (const std::exception& ex) {
    rostd::printf<"Exception raised!!\nException: %s\n">(ex.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
