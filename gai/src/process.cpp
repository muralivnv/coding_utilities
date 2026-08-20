#include "process.h"

#include "printx.hpp"

#include <cstdio>
#include <variant>

namespace gai {

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

namespace {

std::string_view StageName(MatchStage stage) {
  switch (stage) {
    case MatchStage::kFilter: return "--filter";
    case MatchStage::kExclude: return "--exclude";
    case MatchStage::kRangeStart: return "--range start";
    case MatchStage::kRangeEnd: return "--range end";
    case MatchStage::kReplace: return "--replace";
    case MatchStage::kHighlight: return "--filter (highlighting)";
  }
  return "regex";
}

// What the failure cost, in the words of the stage it happened in. A user who
// sees "match limit exceeded" still has to be told whether the line went
// missing or came through unpainted.
std::string_view StageConsequence(MatchStage stage) {
  switch (stage) {
    case MatchStage::kFilter: return "the line is treated as not matching";
    case MatchStage::kExclude: return "the line is not excluded";
    case MatchStage::kRangeStart: return "the range does not open here";
    case MatchStage::kRangeEnd: return "the range does not close here";
    case MatchStage::kReplace: return "the line is printed with the replacement not made";
    case MatchStage::kHighlight: return "the line is printed with matches unhighlighted";
  }
  return "the line is skipped";
}

// The pattern text behind a range bound, or nothing when the bound is a line
// number or absent -- only a regex bound can fail at match time.
std::string_view RangePattern(const RangeValue& bound) {
  const Pcre2Regex* regex = std::get_if<Pcre2Regex>(&bound);
  return (regex != nullptr) ? std::string_view{regex->re.pattern} : std::string_view{};
}

// Stops at the first match, exactly as the std::any_of it replaces did. A
// match-time failure is reported and then counts as a non-match: pcre2 never
// found a match, and reporting one it did not find would invent output.
// `scratch` is the caller's reused buffer, so the no-failure path -- every line
// of a healthy run -- allocates nothing.
bool AnyMatch(const std::vector<Pcre2Regex>& regexes, std::string_view line, MatchStage stage,
              std::string_view filename, size_t linenum, MatchDiagnostics& diagnostics, std::string& scratch) {
  for (const Pcre2Regex& regex : regexes) {
    scratch.clear();
    const bool hit = Find(regex, line, &scratch);
    if (!scratch.empty())
      diagnostics.Report(stage, regex.re.pattern, filename, linenum, scratch);
    if (hit)
      return true;
  }
  return false;
}

std::string_view Colorize(std::string_view line, const std::vector<Pcre2Regex>& filters, std::vector<MatchSpan>& spans,
                          std::string& buffer, std::string_view filename, size_t linenum,
                          MatchDiagnostics& diagnostics, std::string& scratch) {
  spans.clear();
  for (const Pcre2Regex& filter : filters) {
    scratch.clear();
    FindSpans(filter, line, spans, &scratch);
    // The spans found before the failure are still real and still worth
    // painting; what the caller must not do is believe they are all of them.
    if (!scratch.empty())
      diagnostics.Report(MatchStage::kHighlight, filter.re.pattern, filename, linenum, scratch);
  }
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
inline void WriteField(std::string_view text, std::string_view sgr, bool color) {
  if (color)
    ::fwrite_unlocked(sgr.data(), 1, sgr.size(), stdout);
  ::fwrite_unlocked(text.data(), 1, text.size(), stdout);
  if (color)
    ::fwrite_unlocked(kSgrEnd.data(), 1, kSgrEnd.size(), stdout);
}

}  // namespace

void MatchDiagnostics::Report(MatchStage stage, std::string_view pattern, std::string_view file, size_t linenum,
                              std::string_view message) {
  ++occurrences_;

  // The identity of a failure is the pattern, the stage it ran in and what
  // pcre2 said -- not where it happened, or the second line would report the
  // same thing again.
  std::string key;
  key.reserve(StageName(stage).size() + pattern.size() + message.size() + 2);
  key.append(StageName(stage)).push_back('\0');
  key.append(pattern).push_back('\0');
  key.append(message);
  for (const std::string& seen : reported_) {
    if (seen == key)
      return;
  }
  reported_.push_back(std::move(key));

  std::string line;
  line.append("gai: ");
  line.append(file.empty() ? std::string_view{"(standard input)"} : file);
  line.push_back(':');
  line.append(std::to_string(linenum));
  line.append(": ");
  line.append(StageName(stage));
  line.append(" '");
  line.append(pattern);
  line.append("': ");
  line.append(message);
  line.append(" -- ");
  line.append(StageConsequence(stage));
  line.append(" (further occurrences suppressed)");

  if (sink_) {
    sink_(line);
    return;
  }
  line.push_back('\n');
  ::fwrite(line.data(), 1, line.size(), stderr);
}

void Process(const ScanConfig& config, const OutputFunc& out_fn, std::optional<Range>& range, InputBase* const input,
             MatchDiagnostics& diagnostics) {
  thread_local std::string replacement_buffer(1024, ' ');
  thread_local std::string replacement_line(1024, ' ');
  thread_local std::string color_buffer;
  thread_local std::vector<MatchSpan> spans;
  // One buffer for every match-time error message this scan might produce. It
  // is cleared before each match and only written to when the match fails, so a
  // run with no failures never touches it and never allocates for it.
  thread_local std::string match_error;
  size_t linenum = 0;
  while (std::optional<std::string_view> line_opt = input->GetLine()) {
    ++linenum;
    std::string_view& line = line_opt.value();
    if (range) {
      match_error.clear();
      const bool started = range->IsStartReached(line, linenum, &match_error);
      if (!match_error.empty())
        diagnostics.Report(MatchStage::kRangeStart, RangePattern(range->start), config.filename, linenum, match_error);
      if (!started)
        continue;

      match_error.clear();
      const bool ended = range->IsEndReached(line, linenum, &match_error);
      if (!match_error.empty())
        diagnostics.Report(MatchStage::kRangeEnd, RangePattern(range->end), config.filename, linenum, match_error);
      if (ended)
        continue;
    }

    if (!config.filters.empty() &&
        !AnyMatch(config.filters, line, MatchStage::kFilter, config.filename, linenum, diagnostics, match_error)) {
      continue;
    }

    if (!config.excludes.empty() &&
        AnyMatch(config.excludes, line, MatchStage::kExclude, config.filename, linenum, diagnostics, match_error)) {
      continue;
    }

    std::string_view emit = line;
    if (!config.replacements.empty()) {
      replacement_line.assign(line);
      for (const Pcre2Substitution& r : config.replacements) {
        match_error.clear();
        std::string_view replace = Substitute(r, replacement_line, replacement_buffer, &match_error);
        if (!match_error.empty())
          diagnostics.Report(MatchStage::kReplace, r.re.pattern, config.filename, linenum, match_error);
        replacement_line.assign(replace);
      }
      emit = replacement_line;
    }

    if (config.color)
      emit = Colorize(emit, config.filters, spans, color_buffer, config.filename, linenum, diagnostics, match_error);

    out_fn(emit, linenum);
  }
}

OutputFunc MakeOutputFunc(bool verbose, std::string_view delimiter, bool print0, std::string_view filename,
                          bool color) {
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

}  // namespace gai
